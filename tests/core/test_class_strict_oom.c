/* A class in sloppy code runs its heritage and computed keys in strict mode (OP2_enter_strict /
   OP2_leave_strict); a catch in the same frame restores the function's own mode. This sweeps the memory
   limit byte range by byte range so "out of memory" lands inside a computed key (and, for the global
   variant, inside the compile of the class), is caught in the same frame, and the next statement is a
   sloppy assignment to an undeclared name: it must create the global, never throw ReferenceError.
   probe() runs on the tailcall interpreter, the global script on the switch interpreter. The sweep also
   covers every push_scope result in the parser (an unchecked failure popped the outer scope and read
   scopes[-1]) and delete of a global var-ref property whose remove_global_object_property fails (the
   property was left unlinked but counted deleted, and the next compact_properties overflowed the new
   shape). Built with ASan+UBSan by the ctest stage. */
#include "dynajs.h"
#include "../lib/dyn-oomrt.h"

#include <stdio.h>
#include <string.h>

static const char SRC[] = "var caught = 0, strictSeen = 0;\n"
                          "function probe() {\n"
                          "  delete globalThis.cseSloppy;\n"
                          "  try { (class { [Array(64).fill('ab').join('') + 'k']() {} }); } catch (e) { caught++; }\n"
                          "  try { cseSloppy = 1; } catch (e) { if (e instanceof ReferenceError) strictSeen++; else throw e; }\n"
                          "}\n";

static const char GSRC[] = "delete globalThis.cseSloppy2;\n"
                           "try { (class extends Object { [Array(64).fill('cd').join('') + 'k']() {} }); } catch (e) { caught++; }\n"
                           "try { cseSloppy2 = 1; } catch (e) { if (e instanceof ReferenceError) strictSeen++; else throw e; }\n";

static int get_int(JSContext* ctx, const char* name)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, g, name);
    int32_t n = -1;
    if (JS_ToInt32(ctx, &n, v) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        n = -1;
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, g);
    return n;
}

int main(void)
{
    JSRuntime* rt = oomrt_new();
    JSContext* ctx;
    JSMemoryUsage mu;
    JSValue global, probe;
    size_t base, head;
    long ooms = 0, oks = 0;
    int pass, caught, strict_seen;

    if (!rt || !(ctx = JS_NewContext(rt))) {
        fprintf(stderr, "test_class_strict_oom: no runtime\n");
        return 1;
    }
    {
        JSValue v = JS_Eval(ctx, SRC, strlen(SRC), "<class-strict-oom>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            fprintf(stderr, "test_class_strict_oom: the probe fails without a limit\n");
            return 1;
        }
        JS_FreeValue(ctx, v);
    }
    global = JS_GetGlobalObject(ctx);
    probe = JS_GetPropertyStr(ctx, global, "probe");
    JS_FreeValue(ctx, global);
    for (pass = 0; pass < 2; pass++) {
        for (head = 0; head < 24 * 1024; head += 8) {
            JSValue v;
            JS_RunGC(rt);
            JS_ComputeMemoryUsage(rt, &mu);
            base = (size_t)mu.malloc_size;
            oomrt_cap = base + head;
            if (pass == 0)
                v = JS_Call(ctx, probe, JS_UNDEFINED, 0, NULL);
            else
                v = JS_Eval(ctx, GSRC, strlen(GSRC), "<class-strict-oom-global>", JS_EVAL_TYPE_GLOBAL);
            oomrt_cap = (size_t)-1;
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                ooms++;
            } else
                oks++;
            JS_FreeValue(ctx, v);
        }
    }
    caught = get_int(ctx, "caught");
    strict_seen = get_int(ctx, "strictSeen");
    JS_FreeValue(ctx, probe);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("test_class_strict_oom: %ld refused, %ld ran, %d caught in a key, %d strict leaks\n", ooms, oks, caught, strict_seen);
    if (!ooms || !oks || caught <= 0) {
        fprintf(stderr, "test_class_strict_oom: the sweep never caught an out-of-memory inside a key\n");
        return 1;
    }
    return strict_seen == 0 ? 0 : 1;
}
