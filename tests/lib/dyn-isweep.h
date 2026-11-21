/* One-shot interrupt sweep for ctests (header-only; link the engine with its native modules, compile with the
   native_takes: uncatchable_taken grew past the top-level report, so a native took the interrupt even if the call boundary re-raised it.
   engine's own CFLAGS: it reads ctx->interrupt_counter from engine-internal.h).
   isweep_case(name, body, max_n) runs `ISWEEP_PRE body` as a module in a fresh runtime once per N = 1, 2, ...:
   ISWEEP_CTX_INIT(ctx), when defined before the include, adds test globals to every context the harness makes.
   Engine backstop: at each native-call boundary a native that returns normally while an uncatchable exception is pending
   becomes JS_EXCEPTION; a native that takes the exception and calls JS again is not stopped, so native_takes still fails it.
   isw_expect_bad = 1 inverts one case: it passes only when some point is swallowed (the oracle self-test).
   the handler forces a poll check on every js_poll_interrupts (interrupt_counter = 1) and fires exactly once, on
   the Nth call, with no deadline behind it. The sweep stops at the first N the script finishes without reaching
   (or at max_n). A control run without the interrupt goes first: it must finish cleanly with no leak() call, no
   unhandled rejection and every ok() mark the body spells (each standalone `ok` token is one expected call), so a
   case that breaks early cannot pass by reaching few points. For every N that fired, the run must end with the
   interrupt still uncatchable (JS_Eval/await, the event loop or a job reports it) and no JS may run afterwards: no
   later poll and no call to leak() (catch handlers, rejection handlers and timers in the case call leak()). A
   native that takes, rejects with or drops the uncatchable exception lets JS run on, so the sweep reaches every
   such site the script's path crosses.
   An unhandled rejection after the interrupt (the interrupt turned into a rejection) also fails, and the CLI's
   exit-on-unhandled-rejection tracker is replaced so the sweep keeps going. ISWEEP_ONLY=<substring> limits the
   cases. Each N runs in a forked child (the CLI exits on a throwing timer, and a crash must not end the sweep);
   ISWEEP_VERBOSE=1 prints every failing N with the frames that ran JS after the interrupt. Debugging one point:
   ISWEEP_N=<n> starts at n, ISWEEP_NOFORK=1 runs in-process, ISWEEP_ABORT_FIRE=1 / ISWEEP_ABORT_AFTER=1 abort() at
   the firing poll / the first poll after it (run under lldb for both stacks). ISWEEP_SHARD=k/K runs only the cases (interrupt and OOM
   sweeps share one index, isweep_shard_skip)
   whose index i has i % K == k, so K processes split one sweep across cores (run.sh isweep_sharded).
   isweep_enter_workdir() moves each process into a private $TMPDIR/isweep.XXXXXX (with .obj/), so the fixture files the
   cases write (.obj/isweep-a.txt, .obj/isweep-mod/) never collide between shards or a concurrent rel/san run.
   DYNA_LOG=<file> (the one log) gets one timestamped line per case (isweep_timeline; same format as tests/lib/kit.js). */
#ifndef DYN_ISWEEP_H
#define DYN_ISWEEP_H
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"
#include "engine-internal.h"

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define ISWEEP_PRE                                                                     \
    "import * as os from 'os';\n"                                                      \
    "const leak = (e) => { __isweep_leak(String(e)); };\n"                             \
    "const ok = () => { __isweep_ok(); };\n"                                           \
    "const tick = () => new Promise((r) => os.setTimeout(r, 0));\n"

static struct {
    long n, calls, after, leaks, rejections, pre_leaks, pre_rejections, oks;
    int fired, done, final_unc;
    long native_takes;
    JSContext* ctx;
    char leak_what[160];
    char where[200];
} isw;
static int isw_fails, isw_checks, isw_runs, isw_expect_bad;

static void isweep_where(JSContext* ctx)
{
    JSStackFrame* sf = ctx->rt->current_stack_frame;
    size_t off = 0;
    int k;
    for (k = 0; sf && k < 5 && off < sizeof(isw.where) - 40; k++, sf = sf->prev_frame) {
        char buf[64];
        const char* nm = "<native>";
        if (JS_VALUE_GET_TAG(sf->cur_func) == JS_TAG_OBJECT) {
            JSObject* p = JS_VALUE_GET_OBJ(sf->cur_func);
            if (js_class_has_bytecode(p->class_id))
                nm = JS_AtomGetStr(ctx, buf, sizeof(buf), p->u.func.function_bytecode->func_name);
        }
        off += (size_t)snprintf(isw.where + off, sizeof(isw.where) - off, "%s%s", k ? " < " : "", nm[0] ? nm : "<anon>");
    }
}

static void isweep_tracker(JSContext* ctx, JSValueConst promise, JSValueConst reason, JS_BOOL is_handled, void* opaque)
{
    (void)ctx;
    (void)promise;
    (void)reason;
    (void)opaque;
    long d = is_handled ? -1 : 1;
    if (isw.done)
        return;
    if (isw.fired)
        isw.rejections += d;
    else
        isw.pre_rejections += d;
}

static int isweep_handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    if (isw.ctx)
        isw.ctx->interrupt_counter = 1;
    if (isw.done)
        return 0;
    isw.calls++;
    if (isw.fired) {
        isw.after++;
        if (isw.after == 1 && isw.ctx)
            isweep_where(isw.ctx);
        if (isw.after == 1 && getenv("ISWEEP_ABORT_AFTER"))
            abort();
        return 0;
    }
    if (isw.calls == isw.n) {
        if (getenv("ISWEEP_ABORT_FIRE"))
            abort();
        isw.fired = 1;
        return 1;
    }
    return 0;
}

static JSValue isweep_leak(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (!isw.fired && !isw.done)
        isw.pre_leaks++;
    if (!isw.fired && !isw.done && !isw.leak_what[0] && argc > 0) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        snprintf(isw.leak_what, sizeof(isw.leak_what), "%s", s);
        JS_FreeCString(ctx, s);
    }
    if (isw.fired && !isw.done) {
        isw.leaks++;
        if (!isw.leak_what[0] && argc > 0) {
            const char* s = JS_ToCString(ctx, argv[0]);
            if (s) {
                snprintf(isw.leak_what, sizeof(isw.leak_what), "%s", s);
                JS_FreeCString(ctx, s);
            } else if (!JS_HasUncatchableException(ctx)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            } else {
                return JS_EXCEPTION;
            }
        }
    }
    return JS_UNDEFINED;
}

static JSValue isweep_ok(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!isw.done)
        isw.oks++;
    return JS_UNDEFINED;
}

#ifdef ISWEEP_CTX_INIT
static void ISWEEP_CTX_INIT(JSContext* ctx);
#endif

static JSContext* isweep_new_ctx(JSRuntime* rt)
{
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    js_nat_init_all(ctx);
#ifdef ISWEEP_CTX_INIT
    ISWEEP_CTX_INIT(ctx);
#endif
    return ctx;
}

static int isweep_once(const char* src, long n)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSValue v, g;
    int rc;
    memset(&isw, 0, sizeof(isw));
    isw.n = n;
    js_std_set_worker_new_context_func(isweep_new_ctx);
    js_std_init_handlers(rt);
    ctx = isweep_new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__isweep_leak", JS_NewCFunction(ctx, isweep_leak, "__isweep_leak", 1));
    JS_SetPropertyStr(ctx, g, "__isweep_ok", JS_NewCFunction(ctx, isweep_ok, "__isweep_ok", 0));
    JS_FreeValue(ctx, g);
    JS_SetHostPromiseRejectionTracker(rt, isweep_tracker, NULL);
    isw.ctx = ctx;
    ctx->interrupt_counter = 1;
    JS_SetInterruptHandler(rt, isweep_handler, NULL);
    v = JS_Eval(ctx, src, strlen(src), "<isweep>", JS_EVAL_TYPE_MODULE);
    v = js_std_await(ctx, v);
    isw.native_takes = (long)ctx->rt->uncatchable_taken;
    rc = JS_IsException(v) ? -1 : 0;
    if (rc) {
        JSValue e;
        isw.final_unc = JS_HasUncatchableException(ctx);
        e = JS_GetException(ctx);
        if (!isw.fired && getenv("ISWEEP_VERBOSE")) {
            const char* s = JS_ToCString(ctx, e);
            fprintf(stderr, "isweep: control run threw: %s\n", s ? s : "?");
            if (s)
                JS_FreeCString(ctx, s);
            else
                JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    if (!rc) {
        uint32_t t0 = ctx->rt->uncatchable_taken;
        rc = js_std_loop(ctx);
        isw.native_takes += (long)(ctx->rt->uncatchable_taken - t0) - (rc && ctx->rt->uncatchable_taken != t0);
        if (rc)
            isw.final_unc = ctx->rt->current_exception_is_uncatchable;
    }
    isw.done = 1;
    JS_SetInterruptHandler(rt, NULL, NULL);
    isw.ctx = NULL;
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
    return rc;
}

static int isweep_report_fd = -1;

static void isweep_child_exit(void)
{
    if (!isw.done && isw.ctx)
        isw.final_unc = isw.ctx->rt->current_exception_is_uncatchable;
    if (isweep_report_fd >= 0) {
        ssize_t w = write(isweep_report_fd, &isw, sizeof(isw));
        (void)w;
    }
}

static int isweep_fork_once(const char* src, long n, int* crashed)
{
    int fds[2], st = 0;
    pid_t pid;
    ssize_t got = 0, r;
    *crashed = 0;
    if (getenv("ISWEEP_NOFORK") || pipe(fds) < 0)
        return isweep_once(src, n);
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        isweep_report_fd = fds[1];
        atexit(isweep_child_exit);
        exit(isweep_once(src, n) ? 1 : 0);
    }
    close(fds[1]);
    memset(&isw, 0, sizeof(isw));
    while (pid > 0 && got < (ssize_t)sizeof(isw) && (r = read(fds[0], (char*)&isw + got, sizeof(isw) - (size_t)got)) > 0)
        got += r;
    close(fds[0]);
    if (pid < 0 || waitpid(pid, &st, 0) < 0)
        return isweep_once(src, n);
    if (!WIFEXITED(st) || got != (ssize_t)sizeof(isw)) {
        *crashed = 1;
        return -1;
    }
    return WEXITSTATUS(st) ? -1 : 0;
}

static long isweep_count_ok(const char* body)
{
    long k = 0;
    const char* p = body;
    while ((p = strstr(p, "ok")) != NULL) {
        int pre = p > body && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '$' || p[-1] == '\'' || p[-1] == '"' || p[-1] == '.');
        int post = isalnum((unsigned char)p[2]) || p[2] == '_' || p[2] == '$' || p[2] == ':' || p[2] == '\'' || p[2] == '"';
        if (!pre && !post)
            k++;
        p += 2;
    }
    return k;
}

static char isw_workdir[1024];

static int isweep_enter_workdir(void)
{
    const char* t = getenv("TMPDIR");
    snprintf(isw_workdir, sizeof(isw_workdir), "%s/isweep.XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(isw_workdir) || chdir(isw_workdir) != 0 || (mkdir(".obj", 0755) != 0 && errno != EEXIST)) {
        printf("FAIL: isweep: cannot enter a private work dir %s\n", isw_workdir);
        isw_workdir[0] = 0;
        return -1;
    }
    return 0;
}

static void isweep_leave_workdir(void)
{
    if (!isw_workdir[0])
        return;
    if (rmdir(".obj") != 0 || chdir("/") != 0 || rmdir(isw_workdir) != 0)
        printf("  note: isweep: work dir %s left behind (not empty)\n", isw_workdir);
}

#ifdef __APPLE__
#define ISWEEP_PROG getprogname()
#else
#define ISWEEP_PROG program_invocation_short_name
#endif

static double isweep_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void isweep_timeline(const char* suite, const char* name, const char* res, double ms)
{
    const char* path = getenv("DYNA_LOG");
    struct timespec ts;
    struct tm tm;
    char hms[32];
    FILE* f;
    if (!path || !*path || !(f = fopen(path, "a")))
        return;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(hms, sizeof(hms), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s.%03ld\tcase\tEND\t%s :: %s\t%s\t%.3fs\n", hms, ts.tv_nsec / 1000000, suite, name, res, ms / 1000);
    fclose(f);
}

static int isweep_shard_skip(void)
{
    static long idx;
    const char* sh = getenv("ISWEEP_SHARD");
    long k = 0, kn = 1, i = idx++;
    return sh && sscanf(sh, "%ld/%ld", &k, &kn) == 2 && kn > 1 && i % kn != k;
}

static void isweep_case(const char* name, const char* body, long max_n)
{
    const char* only = getenv("ISWEEP_ONLY");
    int verbose = getenv("ISWEEP_VERBOSE") != NULL;
    size_t len = strlen(ISWEEP_PRE) + strlen(body) + 1;
    char* src;
    long n, bad = 0, reached = 0;
    char first[256] = "";
    double c0;
    if (only && !strstr(name, only))
        return;
    if (isweep_shard_skip())
        return;
    c0 = isweep_now_ms();
    src = malloc(len);
    if (!src) {
        printf("  FAIL: [%s] out of memory\n", name);
        isw_fails++;
        return;
    }
    snprintf(src, len, "%s%s", ISWEEP_PRE, body);
    {
        int crashed;
        long want = isweep_count_ok(body);
        int rc = isweep_fork_once(src, 0, &crashed);
        isw_runs++;
        if (crashed || rc != 0 || isw.pre_leaks || isw.pre_rejections || want == 0 || isw.oks != want) {
            printf("  FAIL: [%s] control run without an interrupt: %s%s%ld of %ld ok() marks; %ld leak() calls %s; %ld unhandled rejections\n", name,
                crashed ? "crashed; " : "", rc != 0 ? "run failed; " : "", isw.oks, want, isw.pre_leaks, isw.leak_what, isw.pre_rejections);
            isw_fails++;
            isw_checks++;
            free(src);
            isweep_timeline(ISWEEP_PROG, name, "fail", isweep_now_ms() - c0);
            fflush(stdout);
            return;
        }
    }
    for (n = getenv("ISWEEP_N") ? atol(getenv("ISWEEP_N")) : 1; n <= max_n; n++) {
        if (getenv("ISWEEP_TRACE"))
            fprintf(stderr, "isweep: %s N=%ld\n", name, n);
        int crashed;
        int rc = isweep_fork_once(src, n, &crashed);
        isw_runs++;
        if (!isw.fired && !crashed)
            break;
        reached = n;
        if (crashed || rc == 0 || !isw.final_unc || isw.after || isw.leaks || isw.rejections || isw.native_takes) {
            char what[256];
            snprintf(what, sizeof(what), "N=%ld: %s%s%s%s%s%s%s%s%s%s", n, crashed ? "crashed; " : "", rc == 0 ? "run ended normally; " : "", rc != 0 && !crashed && !isw.final_unc ? "ended with a catchable error; " : "",
                isw.native_takes ? "a native took the interrupt; " : "",
                isw.rejections ? "became an unhandled rejection; " : "", isw.after ? "JS ran after the interrupt in " : "", isw.after ? isw.where : "",
                isw.after ? "; " : "", isw.leaks ? "leak: " : "", isw.leak_what);
            if (!bad)
                snprintf(first, sizeof(first), "%s", what);
            if (verbose)
                printf("    %s\n", what);
            bad++;
        }
    }
    free(src);
    isw_checks++;
    if (n > max_n)
        printf("  note: [%s] stopped at max_n=%ld before the script finished\n", name, max_n);
    if (bad && !isw_expect_bad) {
        isw_fails++;
        printf("  FAIL: [%s] %ld of %ld interrupt points swallowed (first %s)\n", name, bad, reached, first);
    } else if (!bad && isw_expect_bad) {
        isw_fails++;
        printf("  FAIL: [%s] expected a swallowed interrupt; none of %ld interrupt points showed one\n", name, reached);
    } else if (bad) {
        printf("  ok: [%s] swallow seen as expected at %ld of %ld interrupt points (first %s)\n", name, bad, reached, first);
    } else {
        printf("  ok: [%s] %ld interrupt points\n", name, reached);
    }
    isweep_timeline(ISWEEP_PROG, name, (bad != 0) != (isw_expect_bad != 0) ? "fail" : "ok", isweep_now_ms() - c0);
    fflush(stdout);
}

static int isweep_report(const char* suite)
{
    printf("%s: %d cases, %d runs, %d failed\n", suite, isw_checks, isw_runs, isw_fails);
    return isw_fails ? 1 : 0;
}
#endif
