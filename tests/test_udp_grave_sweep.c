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

static const char scenario[] =
    "import { UDPSocket } from \"dyna:net\";\n"
    "globalThis.__parked = 0;\n"
    "const R = new UDPSocket({ port: 0, host: \"127.0.0.1\" });\n"
    "R.start({ message: () => { R.close(); globalThis.__parked = 1; } });\n"
    "R.send(new Uint8Array([7]), \"127.0.0.1\", R.port);\n";

static const char prelude[] = "import \"dyna:net\";\n";

static const char recycle_stage[] =
    "import { UDPSocket } from \"dyna:net\";\n"
    "globalThis.__parkedA = 0;\n"
    "const A = new UDPSocket({ port: 0, host: \"127.0.0.1\" });\n"
    "A.start({ message: () => { A.close(); globalThis.__parkedA = 1; } });\n"
    "A.send(new Uint8Array([7]), \"127.0.0.1\", A.port);\n";

static const char probe_new[] =
    "import { UDPSocket } from \"dyna:net\";\n"
    "globalThis.__t = new UDPSocket({ port: 0, host: \"127.0.0.1\" });\n";
static const char probe_close[] =
    "import { UDPSocket } from \"dyna:net\";\n"
    "globalThis.__t.close();\n";
static const char probe_drop[] =
    "globalThis.__t = undefined;\n";

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
    js_std_loop(ctx);
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

    if (!eval(recycle_stage, "<grave-recycle>")) {
        row_fail(row, "recycle stage eval threw");
        return (uint64_t)-1;
    }
    for (i = 0; i < 1000 && !global_flag_p("__parkedA"); i++)
        js_std_loop(ctx);
    JS_RunGC(rt);
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
            dyn_aio_drain(a);
        else
            js_std_loop(ctx);
    }
    return parked_p();
}

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
    JS_RunGC(rt);
    l1 = dyn_nat_bytes();
    if (l1 != l0)
        row_fail("udp-grave-teardown", l1 > l0
                 ? "the ledger GREW while parked -- something allocated on a "
                   "path this row does not control"
                 : "the ledger SHRANK while parked -- the shell was freed "
                   "before teardown (the flush ran on a boundary-free path?)");
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
