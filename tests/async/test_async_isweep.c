// covers: c:src/dyna-async.c c:src/vm/interpreter.inc.c c:src/builtins/disposable.inc.c c:src/runtime/using.inc.c c:src/serialize/bc_read.inc.c c:src/parser/parser.inc.c c:src/dyna-nat.h c:src/modules/libc/libc-wst-rs.c c:src/modules/libc/libc-wst-core.c c:src/builtins/builtin-promise.c -- JS_PromiseSettle results under a one-shot interrupt and failing allocations
// Each case settles a dyna:async or web-stream promise with a value whose `then` is a JS getter, so JS_PromiseSettle
// runs JS and can fail. Two sweeps per case:
//  interrupt: tests/lib/dyn-isweep.h fires the interrupt once at poll N for every N; a caller that drops the -1 of
//             JS_PromiseSettle lets JS run on (Pool pumps its next job, a channel loop continues) and the case fails;
//  oom:       tests/lib/dyn-oomrt.h refuses allocation N once, N strided over the control run's allocation count
//             (about 200 points per case); each run must end without a crash and, when it ends normally, with no
//             exception left pending (a dropped -1 leaves its out-of-memory error behind).
// Usage: test_async_isweep (ISWEEP_ONLY=<substring>, ISWEEP_VERBOSE=1, OOM_POINTS=<n>, OOM_VERBOSE=1 lists bad N, OOM_STOP=1 stops at the first, OOM_N=<n> runs one, OOM_BT=1 prints the refused allocation stack on san); run from the repo root.
#include "../lib/dyn-isweep.h"
#include "../lib/dyn-oomrt.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>

#define A "const as = await import('dyna:async');\nconst th = (v) => ({ v, get then() { return undefined; } });\n"
#define PROTO_THEN "Object.defineProperty(Object.prototype, 'then', { get() { return undefined; }, configurable: true });\n"

static const char* const cases[][2] = {
    { "Pool settles with a then getter", A "const p = new as.Pool(1); const a = p.submit(() => th(1)).then(ok, leak); const b = p.submit(() => 2).then(ok, leak); await a; await b;\n" },
    { "Semaphore.run settles with a then getter", A "const s = new as.Semaphore(1); await s.run(() => th(1)).then(ok, leak); await s.run(() => 2).then(ok, leak);\n" },
    { "withTimeout settles with a then getter", A "await as.withTimeout(Promise.resolve().then(() => th(2)), 1000).then(ok, leak);\n" },
    { "retry settles with a then getter", A "await as.retry(() => th(3), { backoffMs: 0 }).then(ok, leak);\n" },
    { "pmap settles each element with a then getter", A "await as.pmap([1, 2, 3], (x) => th(x)).then(ok, leak);\n" },
    { "Channel send and close with Object.prototype.then", A PROTO_THEN "const ch = new as.Channel(); const r = ch.recv(); await ch.send(1); await r; const r2 = ch.recv(); ch.close(); await r2; ok();\n" },
    { "ReadableStream reads with Object.prototype.then", PROTO_THEN "const rs = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.close(); } }); const rd = rs.getReader(); await rd.read(); await rd.read(); await rd.read(); ok();\n" },
    { "async generator awaits a then getter", A "async function* g() { try { await th(1); } catch (e) { leak(e); } yield 2; }\nfor await (const x of g()) {} ok();\n" },
    { "async generator throws after awaiting a then getter", A "async function* g() { await th(1); throw 2; }\nawait g().next().then(leak, ok);\n" },
    { "async function rejects after awaiting a then getter", A "async function f() { await th(1); throw 1; }\nawait f().then(leak, ok);\n" },
    { "async function resolves to a then getter", A "async function f() { await null; return th(1); }\nawait f().then(ok, leak);\n" },
    { "await using disposes with a then getter", A "{ await using x = { async [Symbol.asyncDispose]() { return th(1); } }; }\nok();\n" },
    { "AsyncDisposableStack disposeAsync with then getters", A "const st = new AsyncDisposableStack(); st.defer(() => th(1)); st.defer(() => th(2));\nawait st.disposeAsync().then(ok, leak);\n" },
    { "AsyncDisposableStack rejects after a then getter", A "const st = new AsyncDisposableStack(); st.defer(() => th(1)); st.defer(() => { throw 1; });\nawait st.disposeAsync().then(leak, ok);\n" },
    { "AsyncDisposableStack.prototype.disposeAsync on a non-stack", "await AsyncDisposableStack.prototype.disposeAsync.call({}).then(leak, ok);\n" },
    { "using accumulates suppressed errors", "try { using a = { [Symbol.dispose]() { throw 1; } }; using b = { [Symbol.dispose]() { throw 2; } }; throw 3; } catch (e) { if (e instanceof SuppressedError) ok(); }\n" },
    { "Array.fromAsync with then getters", A "await Array.fromAsync([th(1), th(2)]).then(ok, leak);\n" },
    { "Array.fromAsync mapper throws and closes", A "let i = 0; const src = { [Symbol.asyncIterator]() { return { next: () => Promise.resolve({ value: th(i), done: i++ > 2 }), return: () => th(0) }; } };\nawait Array.fromAsync(src, () => { throw 1; }).then(leak, ok);\n" },
    { "dynamic import of a missing module", PROTO_THEN "await import('./no_such_module_isweep.js').then(leak, ok);\n" },
    { "AsyncGenerator next on a non-generator", "const AG = Object.getPrototypeOf(async function* () {}).prototype;\nawait AG.next.call({}).then(leak, ok);\n" },
    { "async iterator over a stream with Object.prototype.then", PROTO_THEN "const rs = new ReadableStream({ start(c) { c.enqueue(1); c.close(); } }); for await (const x of rs) {} ok();\n" },
};

static int oom_fails, oom_runs;

static int oom_once(const char* src, long n, long* used)
{
    JSRuntime* rt = oomrt_new();
    JSContext* ctx;
    JSValue v, g;
    int rc;
    memset(&isw, 0, sizeof(isw));
    js_std_set_worker_new_context_func(isweep_new_ctx);
    js_std_init_handlers(rt);
    ctx = isweep_new_ctx(rt);
    if (!ctx)
        return 3;
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__isweep_leak", JS_NewCFunction(ctx, isweep_leak, "__isweep_leak", 1));
    JS_SetPropertyStr(ctx, g, "__isweep_ok", JS_NewCFunction(ctx, isweep_ok, "__isweep_ok", 0));
    JS_FreeValue(ctx, g);
    JS_SetHostPromiseRejectionTracker(rt, isweep_tracker, NULL);
    oomrt_fail_at = n ? n : LONG_MAX;
    v = JS_Eval(ctx, src, strlen(src), "<oomsweep>", JS_EVAL_TYPE_MODULE);
    v = js_std_await(ctx, v);
    rc = JS_IsException(v) ? 1 : 0;
    JS_FreeValue(ctx, v);
    if (rc)
        JS_FreeValue(ctx, JS_GetException(ctx));
    else if (js_std_loop(ctx))
        rc = 1;
    else if (JS_HasException(ctx))
        rc = 2;
    if (rc && getenv("OOM_VERBOSE")) {
        JSValue e = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, e);
        fprintf(stderr, "oom N=%ld rc=%d pending: %s\n", n, rc, s ? s : "?");
        if (s)
            JS_FreeCString(ctx, s);
        else
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_Throw(ctx, e);
    }
    if (used)
        *used = LONG_MAX - oomrt_fail_at;
    oomrt_fail_at = 0;
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
    return rc;
}

static int oom_fork(const char* src, long n)
{
    int st = 0;
    pid_t pid;
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        alarm(30);
        _exit(oom_once(src, n, NULL));
    }
    if (pid < 0 || waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static void oom_case(const char* name, const char* body)
{
    const char* only = getenv("ISWEEP_ONLY");
    long points = getenv("OOM_POINTS") ? atol(getenv("OOM_POINTS")) : 200, total = 0, step, n, stale = 0, crash = 0, ran = 0, failed = 0;
    size_t len = strlen(ISWEEP_PRE) + strlen(body) + 1;
    char* src;
    long firstbad = 0;
    int rc;
    if (only && !strstr(name, only))
        return;
    double c0;
    if (isweep_shard_skip())
        return;
    c0 = isweep_now_ms();
    src = malloc(len);
    if (!src)
        return;
    snprintf(src, len, "%s%s", ISWEEP_PRE, body);
    if (getenv("OOM_N")) {
        rc = oom_once(src, atol(getenv("OOM_N")), &total);
        printf("  oom [%s] N=%s rc=%d\n", name, getenv("OOM_N"), rc);
        free(src);
        return;
    }
    rc = oom_once(src, 0, &total);
    if (rc != 0 || total <= 0) {
        printf("  FAIL: [oom %s] control run rc=%d allocations=%ld\n", name, rc, total);
        oom_fails++;
        free(src);
        isweep_timeline(ISWEEP_PROG, name, "oom fail", isweep_now_ms() - c0);
        return;
    }
    step = points > 0 && total > points ? total / points : 1;
    for (n = 1; n <= total; n += step) {
        rc = oom_fork(src, n);
        oom_runs++;
        ran++;
        if (rc == 1)
            failed++;
        else if (rc == 2 || rc >= 3) {
            if (!firstbad)
                firstbad = n;
            if (rc == 2)
                stale++;
            else
                crash++;
            if (getenv("OOM_VERBOSE"))
                printf("  bad N=%ld rc=%d\n", n, rc);
            if (getenv("OOM_STOP"))
                break;
        }
    }
    free(src);
    if (stale || crash) {
        oom_fails++;
        printf("  FAIL: [oom %s] %ld runs over %ld allocations: %ld crashed, %ld left an exception pending (first N=%ld)\n", name, ran, total, crash, stale, firstbad);
    } else {
        printf("  ok: [oom %s] %ld runs over %ld allocations (step %ld), %ld failed cleanly\n", name, ran, total, step, failed);
    }
    isweep_timeline(ISWEEP_PROG, name, stale || crash ? "oom fail" : "oom ok", isweep_now_ms() - c0);
    fflush(stdout);
}

int main(void)
{
    size_t i;
    long max_n = getenv("ISWEEP_MAX") ? atol(getenv("ISWEEP_MAX")) : 20000;
    int rc;
    alarm(1200);
    if (isweep_enter_workdir() != 0)
        return 1;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        isweep_case(cases[i][0], cases[i][1], max_n);
    if (!getenv("ISWEEP_NO_OOM"))
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            oom_case(cases[i][0], cases[i][1]);
    unlink(".obj/isweep-a.txt");
    unlink(".obj/isweep-b.txt");
    isweep_leave_workdir();
    rc = isweep_report("async_isweep");
    printf("async_isweep oom: %d runs, %d failed\n", oom_runs, oom_fails);
    return rc || oom_fails ? 1 : 0;
}
