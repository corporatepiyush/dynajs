/* test_udp_grave_sweep.c -- ledger-exact rows for the UDP graveyard's two
 * free points (the re-home onto the engine shutdown machinery).
 *
 * THE ENTRIES UNDER TEST. A UDPSocket that closes itself from inside a
 * delivery parks its shell (the adapter's burst loop still holds the
 * pointer; freeing at close was a heap-use-after-free). The parked shell has
 * exactly two free points, and both are pinned here on the module-native
 * ledger (memoryUsage().nativeSize's C side, dyn_nat_bytes):
 *
 *   udp-grave-flush     the pass-boundary flush (udp_grave_flush, armed
 *                       through dyn_net_on_drain_notick -- boundary-driven,
 *                       never arming the reactor's clock). The moment the
 *                       drain returns, exactly the shell's bytes are back.
 *
 *   udp-grave-teardown  the shutdown-sweep backstop. This row constructs
 *                       the state the backstop exists for -- a shell parked
 *                       with NO pass boundary ever reaching the flush: the
 *                       delivery is dispatched by calling dyn_aio_drain
 *                       DIRECTLY, bypassing dyn_net_drain's hook loop (that
 *                       bypass IS "the boundary never arrived", the shape an
 *                       interrupted pass hands teardown). The parked shell
 *                       must leave the ledger untouched; then the full
 *                       CLI-shaped teardown -- JS_FreeContext runs the
 *                       shell's sweep (udp_grave_sweep -> the
 *                       JS_ShutdownDeferFree registry), JS_FreeRuntime frees
 *                       every deferred block LAST, when no delivery can run
 *                       again -- and the ledger must be back to EXACTLY what
 *                       it was before the runtime existed.
 *
 * The ordering invariant the re-home must keep sits under both rows: a
 * stale delivery reads the dead flag in LIVE memory until no delivery can
 * run again -- which is why the teardown free point is the engine's
 * deferred-free list, not the sweep body itself.
 *
 * Rows (named; each mutation in the lane report fails at least one):
 *   udp-grave-flush     revert the flush re-arm -> the shell's bytes stay on
 *                       the ledger, the equality fails high.
 *   udp-grave-teardown  latch the sweep registration (or drop the
 *                       DeferFree) -> the shell's bytes survive JS_FreeRuntime,
 *                       the equality fails high; cancel the flush's
 *                       sweep/defer cancellation instead and this row is the
 *                       double-free detector.
 *
 * Build/run: make test-net-udp-grave (needs a CONFIG_NATIVE_MODULES=y tree;
 * under CONFIG_ASAN=y the same rows are the sanitizer gate -- run them with
 * ASAN_OPTIONS=detect_leaks=1 on Darwin, where LSan is off by default; the
 * parked shell is REACHABLE through the module's grave array until freed,
 * so the ledger equality, not LSan, is the detector here).
 */
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"
#include "dyna-aio.h"

#include <stdio.h>
#include <string.h>

static int rows, fails;

static void row_fail(const char *row, const char *why)
{
    fprintf(stderr, "FAIL row %s: %s\n", row, why);
    fails++;
}

static void row_ok(const char *row)
{
    printf("row %s: ok\n", row);
}

static void print_exc(JSContext *ctx)
{
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    fprintf(stderr, "  exception: %s\n", s ? s : "(?)");
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
}

static JSRuntime *rt;
static JSContext *ctx;

static int global_flag_p(const char *name);

static int setup(void)
{
    rt = JS_NewRuntime();
    ctx = rt ? JS_NewContext(rt) : NULL;
    if (!rt || !ctx)
        return -1;
    js_std_init_handlers(rt);
    /* The CLI calls this before running a script: it arms the global
     * timers and the event-loop poll hook (os_poll_func). Without it
     * js_std_loop never polls and no completion is ever dispatched. */
    js_std_add_helpers(ctx, 0, NULL);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
                            js_module_check_attributes, NULL);
#ifdef CONFIG_NATIVE_MODULES
    if (js_nat_init_net(ctx) < 0) {
        print_exc(ctx);
        return -1;
    }
#else
    fprintf(stderr, "built without CONFIG_NATIVE_MODULES\n");
    return -1;
#endif
    return 0;
}

static int eval(const char *src, const char *name)
{
    JSValue ret = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_MODULE);
    int ok = !JS_IsException(ret);
    if (!ok)
        print_exc(ctx);
    JS_FreeValue(ctx, ret);
    return ok;
}

static int parked_p(void)
{
    return global_flag_p("__parked");
}

/* Module bodies run as jobs after their imports resolve: an eval is only
 * DONE once the job queue is empty. */
static void drain_jobs(void)
{
    int i;
    for (i = 0; i < 100; i++) {
        if (JS_ExecutePendingJob(JS_GetRuntime(ctx), NULL) <= 0)
            break;
    }
}

static void teardown_runtime(void)
{
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    rt = NULL;
    ctx = NULL;
}

/* The scenario: one self-addressed datagram whose handler closes the socket
 * from inside the delivery. The delivery is dispatched by whichever drain
 * the row drives (the JS loop's, or the raw dyn_aio_drain bypass). */
static const char scenario[] =
    "import { UDPSocket } from \"dyna:net\";\n"
    "globalThis.__parked = 0;\n"
    "const R = new UDPSocket({ port: 0, host: \"127.0.0.1\" });\n"
    "R.start({ message: () => { R.close(); globalThis.__parked = 1; } });\n"
    "R.send(new Uint8Array([7]), \"127.0.0.1\", R.port);\n";

/* The prelude settles everything the module machinery allocates on the
 * native ledger, so the row baselines start from the socket alone. */
static const char prelude[] = "import \"dyna:net\";\n";

/* The recycle stage: one parked shell that goes through the FULL mid-run
 * path -- park at a delivery, pass boundary, flush -- on a reactor that is
 * then destroyed (this socket is the last net resource, so the hook table
 * dies with it). The stage under test is armed AFTER it, against a fresh
 * reactor: a registration latched to "the first park" (the old one-shot
 * bug, either for the flush or for the sweep) covers the stage-A shell and
 * silently loses the stage-B one -- which is exactly what the rows below
 * then fail on. */
static const char recycle_stage[] =
    "import { UDPSocket } from \"dyna:net\";\n"
    "globalThis.__parkedA = 0;\n"
    "const A = new UDPSocket({ port: 0, host: \"127.0.0.1\" });\n"
    "A.start({ message: () => { A.close(); globalThis.__parkedA = 1; } });\n"
    "A.send(new Uint8Array([7]), \"127.0.0.1\", A.port);\n";

/* The shell's ledger size, measured, not assumed: open a second socket
 * (its ledger rise) and close it OUTSIDE any delivery (the immediate
 * dispose path gives the shell straight back). */
static const char probe_new[] =
    "import { UDPSocket } from \"dyna:net\";\n"
    "globalThis.__t = new UDPSocket({ port: 0, host: \"127.0.0.1\" });\n";
static const char probe_close[] =
    "import { UDPSocket } from \"dyna:net\";\n"
    "globalThis.__t.close();\n";
static const char probe_drop[] =
    "globalThis.__t = undefined;\n";

/* One row's runtime, armed: prelude settled, shell size measured, scenario
 * evaluated and its evaluation jobs drained. Returns the post-arm ledger
 * baseline, or (uint64_t)-1 after naming the failure. */
/* Read one of the scenario's global markers as a boolean. */
static int global_flag_p(const char *name)
{
    JSValue g, v;
    int r;
    g = JS_GetGlobalObject(ctx);
    v = JS_GetPropertyStr(ctx, g, name);
    r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, g);
    return r == 1;
}

static uint64_t arm_row(const char *row, uint64_t *shell_out)
{
    uint64_t l_up, l_down;
    int i;

    if (!eval(prelude, "<grave-prelude>")) {
        row_fail(row, "prelude eval threw");
        return (uint64_t)-1;
    }
    js_std_loop(ctx);            /* settle module-init scratch */
    JS_RunGC(rt);

    if (!eval(probe_new, "<grave-probe-new>")) {
        row_fail(row, "probe eval threw");
        return (uint64_t)-1;
    }
    drain_jobs();
    l_up = dyn_nat_bytes();
    if (!eval(probe_close, "<grave-probe-close>")) {
        row_fail(row, "probe close threw");
        return (uint64_t)-1;
    }
    drain_jobs();
    l_down = dyn_nat_bytes();
    if (l_up <= l_down || l_up - l_down > 4096) {
        row_fail(row, "cannot measure the shell's ledger size -- the probe "
                      "socket did not give its bytes straight back");
        return (uint64_t)-1;
    }
    *shell_out = l_up - l_down;
    (void)eval(probe_drop, "<grave-probe-drop>");
    drain_jobs();
    JS_RunGC(rt);

    /* The recycle stage FIRST: it parks and flushes a shell of its own on a
       reactor the stage then destroys, so the scenario below parks against a
       fresh reactor -- a latched registration loses only the scenario shell. */
    if (!eval(recycle_stage, "<grave-recycle>")) {
        row_fail(row, "recycle stage eval threw");
        return (uint64_t)-1;
    }
    for (i = 0; i < 1000 && !global_flag_p("__parkedA"); i++)
        js_std_loop(ctx);
    JS_RunGC(rt);
    /* Stage A's shell is flushed and its wrapper box is the only residue
       (the module namespace holds it until the context dies); both cancel
       out of the row equalities below, which diff the ledger across the
       scenario's own park and free. What the stage PRIMES is the
       registrations: a latch ("first park only") covers stage A and loses
       the scenario shell -- the exact old one-shot bug -- and the row
       equalities are what then fail, named. */

    if (!eval(scenario, "<udp-grave>")) {
        row_fail(row, "scenario eval threw");
        return (uint64_t)-1;
    }
    for (i = 0; i < 20; i++) {
        if (JS_ExecutePendingJob(JS_GetRuntime(ctx), NULL) <= 0)
            break;
    }
    JS_RunGC(rt);
    return dyn_nat_bytes();
}

static int parked_wait(int bypass, struct dyn_aio *a)
{
    int i;
    for (i = 0; i < 1000 && !parked_p(); i++) {
        if (bypass)
            dyn_aio_drain(a);    /* completions ONLY: the hooks never run */
        else
            js_std_loop(ctx);    /* the real loop: hooks run at the boundary */
    }
    return parked_p();
}

/* ROW 1: the pass boundary flush. Drive the real JS loop; the boundary of
 * the delivery's own pass runs the flush hooks, and exactly the shell's
 * bytes must come back. */
static void row_flush(void)
{
    uint64_t l0, l1, shell = 0, start;

    rows++;
    start = dyn_nat_bytes();
    if (setup() < 0) {
        row_fail("udp-grave-flush", "cannot set up the runtime");
        if (rt)
            teardown_runtime();
        return;
    }
    l0 = arm_row("udp-grave-flush", &shell);
    if (l0 == (uint64_t)-1) {
        teardown_runtime();
        return;
    }
    if (!parked_wait(0, NULL)) {
        row_fail("udp-grave-flush",
                 "the delivery never ran -- the row would prove nothing");
        teardown_runtime();
        return;
    }
    JS_RunGC(rt);
    l1 = dyn_nat_bytes();
    if (l1 != l0 - shell)
        row_fail("udp-grave-flush", l1 > l0 - shell
                 ? "the shell's bytes are still on the ledger after the pass "
                   "boundary -- the flush never freed it (missed registration?)"
                 : "the ledger dropped MORE than the shell -- an over-free "
                   "somewhere in the pass");
    else
        row_ok("udp-grave-flush");
    teardown_runtime();
    if (dyn_nat_bytes() != start)
        row_fail("udp-grave-flush", "the runtime teardown left ledger bytes "
                 "behind -- the row's own footprint did not come back");
}

/* ROW 2: the teardown backstop. Dispatch the delivery by calling
 * dyn_aio_drain DIRECTLY on the shared reactor -- dyn_net_drain's hook loop
 * never runs, so no flush frees the shell: parked with no boundary in
 * sight, exactly the state the sweep exists for. Then the full CLI-shaped
 * teardown, and the ledger must be back to the pre-runtime start. */
static void row_teardown(void)
{
    uint64_t l0, l1, shell = 0, start;
    struct dyn_aio *a;

    rows++;
    start = dyn_nat_bytes();
    if (setup() < 0) {
        row_fail("udp-grave-teardown", "cannot set up the runtime");
        if (rt)
            teardown_runtime();
        return;
    }
    l0 = arm_row("udp-grave-teardown", &shell);
    if (l0 == (uint64_t)-1) {
        teardown_runtime();
        return;
    }
    a = dyn_net_reactor_acquire(ctx);
    if (!a) {
        row_fail("udp-grave-teardown", "no shared reactor");
        teardown_runtime();
        return;
    }
    if (!parked_wait(1, a)) {
        row_fail("udp-grave-teardown",
                 "the delivery never ran -- the row would prove nothing");
        dyn_net_reactor_release_rt(rt);
        teardown_runtime();
        return;
    }
    dyn_net_reactor_release_rt(rt);
    /* Parked: the shell's bytes are still on the ledger -- nothing on the
       bypass path freed them, which is the precondition that makes the
       teardown half of this row mean anything. */
    JS_RunGC(rt);
    l1 = dyn_nat_bytes();
    if (l1 != l0)
        row_fail("udp-grave-teardown", l1 > l0
                 ? "the ledger GREW while parked -- something allocated on a "
                   "path this row does not control"
                 : "the ledger SHRANK while parked -- the shell was freed "
                   "before teardown (the flush ran on a boundary-free path?)");
    /* THE BACKSTOP: the last JS_FreeContext runs every registered sweep
       (the shell's body untracks the ledger and registers the
       JS_ShutdownDeferFree), the job queue drains, and JS_FreeRuntime frees
       every deferred block LAST. */
    teardown_runtime();
    if (dyn_nat_bytes() != start)
        row_fail("udp-grave-teardown", dyn_nat_bytes() > start
                 ? "the shell's bytes survived JS_FreeRuntime -- the sweep or "
                   "the defer never ran (a latched registration is this "
                   "exact failure)"
                 : "the teardown freed MORE than the runtime owned -- an "
                   "over-free on the teardown path");
    else
        row_ok("udp-grave-teardown");
}

int main(void)
{
    row_flush();
    row_teardown();
    printf("test_udp_grave_sweep: rows=%d fails=%d\n", rows, fails);
    return fails ? 1 : 0;
}
