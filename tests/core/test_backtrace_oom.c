/* The interpreter decorates a native exception with its stack via build_backtrace(rt->current_exception),
   a BORROWED reference. An allocation failure inside it throws "out of memory", JS_Throw releases the old
   current exception -- the object being decorated -- and the next property store wrote into freed memory
   (found by the leaks stage: a SIGSEGV in find_own_property on an error object with a NULL shape).
   Which allocation fails depends on the headroom, so the headroom is swept byte range by byte range: some
   value lands the first failure inside build_backtrace, after the TypeError was created and thrown.
   The probe rethrows anything that is not the TypeError, so a refused allocation reaches the sweep as an
   exception instead of being caught as one more error object. Built with ASan+UBSan by the ctest stage
   (small allocations then bypass the arenas, so every allocation meets the limit); on the old code ASan
   reports heap-use-after-free. */
#include "dynajs.h"

#include <stdio.h>
#include <string.h>

static const char SRC[] = "function f(d) { if (d) return f(d - 1); return null.x; }\n"
                          "function run() { for (let i = 0; i < 4; i++) try { f(24); } catch (e) { if (!(e instanceof TypeError)) throw e; } }\n"
                          "run();\n";

int main(void)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSMemoryUsage mu;
    JSValue global, run;
    size_t base, head;
    long ooms = 0, oks = 0;

    if (!rt || !(ctx = JS_NewContext(rt))) {
        fprintf(stderr, "test_backtrace_oom: no runtime\n");
        return 1;
    }
    {
        JSValue v = JS_Eval(ctx, SRC, strlen(SRC), "<backtrace-oom>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            fprintf(stderr, "test_backtrace_oom: the probe fails without a limit\n");
            return 1;
        }
        JS_FreeValue(ctx, v);
    }
    global = JS_GetGlobalObject(ctx);
    run = JS_GetPropertyStr(ctx, global, "run");
    JS_FreeValue(ctx, global);
    for (head = 0; head < 48 * 1024; head += 8) {
        JSValue v;
        JS_RunGC(rt);
        JS_ComputeMemoryUsage(rt, &mu);
        base = (size_t)mu.malloc_size;
        JS_SetMemoryLimit(rt, base + head);
        v = JS_Call(ctx, run, JS_UNDEFINED, 0, NULL);
        JS_SetMemoryLimit(rt, (size_t)-1);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            ooms++;
        } else
            oks++;
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, run);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (!ooms || !oks) {
        fprintf(stderr, "test_backtrace_oom: the sweep never crossed the failure boundary (%ld refused, %ld ran)\n", ooms, oks);
        return 1;
    }
    printf("test_backtrace_oom: %ld headrooms refused, %ld ran, no use of a released exception\n", ooms, oks);
    return 0;
}
