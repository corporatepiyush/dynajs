// covers: c:src/modules/stream/stream-pipe.c c:src/modules/stream/stream-lines.c c:src/modules/stream/stream-module.c c:src/modules/libc/libc-wst-misc.c c:src/modules/libc/libc-wst-adapt.c -- the uncatchable interrupt through dyna:stream and the web-stream error paths under a one-shot embedder handler
// The CLI --timeout-ms handler keeps firing after its deadline, so a site that consumes the uncatchable
// "interrupted" error is usually rescued by the next poll and the CLI suite (test_stream_interrupt.js) cannot see it.
// An embedder whose handler fires once is not rescued: the swallowed interrupt lets the script run on. Here each
// case runs as a module in a fresh runtime with the full engine and native modules; the handler fires once, on the
// first poll after the script calls arm() (every case calls arm() right before an endless loop). A case passes
// when the event loop ends with an error (the interrupt surfaced, directly or through the job runner's pending
// exception report) and the script never set globalThis.leak (a catch handler, or a 50 ms timer that only runs when
// the loop keeps going).
// transfer cases: a hostile error name/message getter or Error[Symbol.hasInstance] spins while the error is packed for the
// other side; the adapter stops before calling JS again and the cancel/write algorithm returns the interrupt.
// Usage: test_stream_interrupt_api [CASE-SUBSTRING]; run from the repo root (a case loads a worker by path).
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int armed, fired;
static int fails, checks;

static int handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    if (armed && !fired) {
        fired = 1;
        return 1;
    }
    return 0;
}

static JSValue h_arm(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    armed = 1;
    return JS_UNDEFINED;
}

static JSContext* new_ctx(JSRuntime* rt)
{
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    js_nat_init_all(ctx);
    return ctx;
}

static void check(int ok, const char* name, const char* what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL: [%s] %s\n", name, what);
    }
}

#define PRE                                                                                     \
    "import * as os from 'os';\n"                                                               \
    "const spin = () => { arm(); for (;;) {} };\n"                                              \
    "const leak = (e) => { globalThis.leak = String(e); };\n"                                   \
    "const tick = () => new Promise((r) => os.setTimeout(r, 0));\n"                             \
    "os.setTimeout(() => { if (globalThis.leak === undefined) globalThis.leak = 'timer'; }, 50);\n"                              \
    "const { toReadableStream, toWritableStream, lines, ndjson, fromBytes } = await import('dyna:stream');\n"
#define SRC1 "toReadableStream({ read(b) { b[0] = 65; return Promise.resolve(1); }, close() {} })"
#define SPIN_THEN "Object.defineProperty(Object.prototype, 'then', { configurable: true, get() { spin(); } });\n"

static const char* const cases[][2] = {
    { "pipe: write count valueOf spins", "const rs = " SRC1 "; await tick();\n"
                                         "rs.pipeTo(toWritableStream({ write() { return Promise.resolve({ valueOf() { spin(); } }); }, close() {} })).catch(leak);\n" },
    { "pipe: read count valueOf spins", "const rs = toReadableStream({ read() { return Promise.resolve({ valueOf() { spin(); } }); }, close() {} }); await tick();\n"
                                        "rs.pipeTo(toWritableStream({ write(v) { return Promise.resolve(v.length); }, close() {} })).catch(leak);\n" },
    { "lines: encoding toString spins", "try { lines(fromBytes('a\\n'), { encoding: { toString() { spin(); } } }); } catch (e) { leak(e); }\nleak('after lines');\n" },
    { "lines: line result then getter spins", "const it = lines(new ReadableStream({ start(c) { c.enqueue('a\\nb'); } }));\n" SPIN_THEN "it.next().then(leak, leak);\n" },
    { "lines: done result then getter spins", "const it = lines(new ReadableStream({ start(c) { c.close(); } }));\n" SPIN_THEN "it.next().then(leak, leak);\n" },
    { "lines: batch result then getter spins", "const it = lines(new ReadableStream({ start(c) { c.enqueue('a\\nb\\n'); } }), { batch: true });\n" SPIN_THEN "it.next().then(leak, leak);\n" },
    { "lines: async line result then getter spins", "Object.defineProperty(Object.prototype, 'then', { configurable: true, get() { if (this.value === 'a') spin(); } });\n"
                                                   "lines(new ReadableStream({ async pull(c) { await tick(); c.enqueue('a\\nb'); } })).next().then(leak, leak);\n" },
    { "lines: async done result then getter spins", "let dk = 0; Object.defineProperty(Object.prototype, 'then', { configurable: true, get() { if (this.done === true && ++dk === 2) spin(); } });\n"
                                                   "lines(new ReadableStream({ async pull(c) { await tick(); c.close(); } })).next().then(leak, leak);\n" },
    { "lines: async batch result then getter spins", "Object.defineProperty(Object.prototype, 'then', { configurable: true, get() { if (Array.isArray(this.value)) spin(); } });\n"
                                                    "lines(new ReadableStream({ async pull(c) { await tick(); c.enqueue('a\\nb\\n'); } }), { batch: true }).next().then(leak, leak);\n" },
    { "lines: pull spins", "(async () => { try { for await (const l of lines(new ReadableStream({ pull() { spin(); } }))); } catch (e) { leak(e); } })();\n" },
    { "lines batch: pull spins", "(async () => { try { for await (const l of lines(new ReadableStream({ pull() { spin(); } }), { batch: true })); } catch (e) { leak(e); } })();\n" },
    { "ndjson: pull spins", "(async () => { try { for await (const l of ndjson(new ReadableStream({ pull() { spin(); } }))); } catch (e) { leak(e); } })();\n" },
    { "lines: duck read spins", "(async () => { try { for await (const l of lines({ read() { spin(); }, close() {} })); } catch (e) { leak(e); } })();\n" },
    { "TextDecoderStream: enqueue then getter spins", "const ts = new TextDecoderStream(); ts.readable.getReader().read().catch(leak);\n" SPIN_THEN
                                                     "ts.writable.getWriter().write(new Uint8Array([65])).catch(leak);\n" },
    { "transfer: error name getter spins", "const w = new os.Worker('./tests/core/web_streams_transfer_child.js'); w.onmessage = (e) => leak(JSON.stringify(e.data));\n"
                                           "const err = new RangeError('x'); Object.defineProperty(err, 'name', { get() { spin(); } });\n"
                                           "const rs = new ReadableStream({ pull(c) { c.error(err); } }); w.postMessage({ op: 'readErr', rs }, [rs]);\n" },
    { "transfer: error message getter spins", "const w = new os.Worker('./tests/core/web_streams_transfer_child.js'); w.onmessage = (e) => leak(JSON.stringify(e.data));\n"
                                              "const err = new RangeError('x'); Object.defineProperty(err, 'message', { get() { spin(); } });\n"
                                              "const rs = new ReadableStream({ pull(c) { c.error(err); } }); w.postMessage({ op: 'readErr', rs }, [rs]);\n" },
    { "transfer: Error hasInstance spins", "const w = new os.Worker('./tests/core/web_streams_transfer_child.js'); w.onmessage = (e) => leak(JSON.stringify(e.data));\n"
                                           "const err = new RangeError('x'); Object.defineProperty(Error, Symbol.hasInstance, { value(v) { if (v === err) spin(); return false; } });\n"
                                           "const rs = new ReadableStream({ pull(c) { c.error(err); } }); w.postMessage({ op: 'readErr', rs }, [rs]);\n" },
    { "transfer: write clone error name getter spins", "const w = new os.Worker('./tests/core/web_streams_transfer_child.js');\n"
                                                       "w.onmessage = (e) => { if (!e.data.ts) return leak(JSON.stringify(e.data)); for (const P of [DOMException.prototype, TypeError.prototype, Error.prototype]) Object.defineProperty(P, 'name', { configurable: true, get() { spin(); } });\n"
                                                       "  e.data.ts.writable.getWriter().write(() => 1).then(leak, leak); };\n"
                                                       "w.postMessage({ op: 'upper' });\n" },
};

static void run_case(const char* name, const char* body)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSValue v, g, l;
    char src[4096];
    int rc;
    armed = 0;
    fired = 0;
    js_std_set_worker_new_context_func(new_ctx);
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    JS_SetInterruptHandler(rt, handler, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "arm", JS_NewCFunction(ctx, h_arm, "arm", 0));
    snprintf(src, sizeof(src), "%s%s", PRE, body);
    v = JS_Eval(ctx, src, strlen(src), "<case>", JS_EVAL_TYPE_MODULE);
    v = js_std_await(ctx, v);
    rc = JS_IsException(v) ? -1 : 0;
    if (rc)
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, v);
    if (!rc)
        rc = js_std_loop(ctx);
    l = JS_GetPropertyStr(ctx, g, "leak");
    check(fired, name, "the handler fired");
    check(rc != 0, name, "the run ended with the interrupt");
    if (!JS_IsUndefined(l)) {
        const char* s = JS_ToCString(ctx, l);
        char what[256];
        if (!s) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            check(0, name, "user code observed the interrupt");
        } else {
            snprintf(what, sizeof(what), "user code observed the interrupt (leak = %s)", s);
            check(0, name, what);
            JS_FreeCString(ctx, s);
        }
    } else {
        check(1, name, "no leak");
    }
    JS_FreeValue(ctx, l);
    JS_FreeValue(ctx, g);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
}

int main(int argc, char** argv)
{
    size_t i;
    alarm(60);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && !strstr(cases[i][0], argv[1]))
            continue;
        printf("case: %s\n", cases[i][0]);
        fflush(stdout);
        run_case(cases[i][0], cases[i][1]);
    }
    printf("stream_interrupt_api: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
