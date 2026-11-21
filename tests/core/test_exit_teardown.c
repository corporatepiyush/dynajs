// covers: c:src/mm/js_malloc.inc.c
// JS_SetExitTeardown on a release (non-sanitized) engine. Cases:
//   order   the same script on two runtimes, one torn down precisely and one
//           with the exit flag: the native-class finalizers run in the same
//           order and all of them run; the precise runtime returns every
//           allocation, the exit runtime returns fewer (it frees nothing).
//   guard   the exit flag is honoured only on the process's first runtime
//           (the CLI's main runtime) while it is the only one alive: on the
//           first runtime with a second one alive, and on a later-created
//           runtime (a worker's) even when it is the last one alive, the flag
//           is ignored and the runtime returns every allocation. (In DUMP_LEAKS
//           builds a flag on a non-first runtime aborts, which is what makes
//           the leaks stage fail if a worker ever sets it.)
//   leak    an object still referenced from C at JS_FreeRuntime under the
//           exit flag: the decrement pass sees the external reference, the
//           precise path runs, and the release leak assert aborts (in a
//           forked child).
// Mutants (all red): skip the decrement pass in the exit pass (leak); ignore
// the live-runtime count (guard); ignore the first-runtime check (guard);
// skip native finalizers (order); walk the candidates in reverse (order).
#include "dynajs.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static JSClassID fin_class;
static int fin_log[256];
static int fin_n;

static void check(int ok, const char* what)
{
    if (!ok) {
        failures++;
        fprintf(stderr, "  FAIL %s\n", what);
    }
}

typedef struct {
    long live;
} Counter;

static void* t_malloc(JSMallocState* s, size_t size)
{
    void* p = malloc(size);
    if (p)
        ((Counter*)s->opaque)->live++;
    return p;
}

static void t_free(JSMallocState* s, void* ptr)
{
    if (ptr)
        ((Counter*)s->opaque)->live--;
    free(ptr);
}

static void* t_realloc(JSMallocState* s, void* ptr, size_t size)
{
    void* p;
    if (!ptr)
        return t_malloc(s, size);
    if (!size) {
        t_free(s, ptr);
        return NULL;
    }
    p = realloc(ptr, size);
    return p;
}

static const JSMallocFunctions MF = { t_malloc, t_free, t_realloc, NULL };

static void fin_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    if (fin_n < 256)
        fin_log[fin_n++] = (int)(intptr_t)JS_GetOpaque(val, fin_class);
}

static JSValue js_mk(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int32_t i = 0;
    JSValue o;
    (void)this_val;
    (void)argc;
    if (JS_ToInt32(ctx, &i, argv[0]))
        return JS_EXCEPTION;
    o = JS_NewObjectClass(ctx, (int)fin_class);
    if (!JS_IsException(o))
        JS_SetOpaque(o, (void*)(intptr_t)(i + 1));
    return o;
}

static const char* const SCRIPT = "globalThis.keep = [];"
                                  "for (let i = 0; i < 40; i++) {"
                                  "  const n = { f: mk(i), i }; n.self = n; keep.push(n);"
                                  "  if (i % 3 === 0) keep.push(() => n.f);"
                                  "}"
                                  "globalThis.big = Array.from({ length: 5000 }, (_, i) => ({ i, s: 'k' + i }));";

static JSRuntime* make_rt(Counter* c, JSContext** pctx)
{
    JSRuntime* rt = JS_NewRuntime2(&MF, c);
    JSClassDef def = { "Fin", fin_finalizer, NULL, NULL, NULL };
    JSContext* ctx;
    JSValue g, v;
    JS_NewClass(rt, fin_class, &def);
    ctx = JS_NewContext(rt);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "mk", JS_NewCFunction(ctx, js_mk, "mk", 1));
    JS_FreeValue(ctx, g);
    v = JS_Eval(ctx, SCRIPT, strlen(SCRIPT), "<exit-teardown>", JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(v), "script runs");
    JS_FreeValue(ctx, v);
    *pctx = ctx;
    return rt;
}

int main(void)
{
    Counter ca = { 0 }, cb = { 0 }, cc = { 0 }, cd = { 0 };
    JSContext *ctx, *ctx2;
    JSRuntime *rt, *rt2;
    int precise_log[256], precise_n, i, same, status;
    pid_t pid;

    JS_NewClassID(&fin_class);

    rt = make_rt(&ca, &ctx);
    JS_FreeContext(ctx);
    fin_n = 0;
    JS_FreeRuntime(rt);
    precise_n = fin_n;
    memcpy(precise_log, fin_log, sizeof(fin_log));
    check(precise_n == 40, "order: the precise teardown finalizes all 40 native objects");
    check(ca.live == 0, "order: the precise teardown returns every allocation");

    rt = make_rt(&cb, &ctx);
    JS_FreeContext(ctx);
    fin_n = 0;
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
    same = fin_n == precise_n;
    for (i = 0; same && i < fin_n; i++)
        same = fin_log[i] == precise_log[i];
    check(same, "order: the exit teardown runs the same finalizers in the same order");
    check(cb.live > 0, "order: the exit teardown frees nothing (fast path taken)");

    rt = make_rt(&cc, &ctx);
    rt2 = make_rt(&cd, &ctx2);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
    check(cc.live == 0, "guard: on the first runtime with another alive the exit flag is ignored");
    JS_FreeContext(ctx2);
    JS_SetExitTeardown(rt2, 1);
    JS_FreeRuntime(rt2);
    check(cd.live == 0, "guard: on a later-created runtime the exit flag is ignored even when it is the last");

    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        Counter ce = { 0 };
        JSValue held;
        rt = make_rt(&ce, &ctx);
        held = JS_Eval(ctx, "keep[0]", 7, "<leak>", JS_EVAL_TYPE_GLOBAL);
        (void)held;
        JS_FreeContext(ctx);
        JS_SetExitTeardown(rt, 1);
        close(2);
        JS_FreeRuntime(rt);
        _exit(0);
    }
    waitpid(pid, &status, 0);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "leak: an object held from C still trips the release leak assert");

    if (failures) {
        fprintf(stderr, "test_exit_teardown: %d failures\n", failures);
        return 1;
    }
    printf("ok test_exit_teardown (40 finalizers, exit teardown left %ld allocations)\n", cb.live);
    return 0;
}
