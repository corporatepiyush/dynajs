// covers: c:src/modules/stream/stream-lines.c c:src/modules/stream/stream-cps.c c:src/modules/stream/stream-pipe.c c:src/modules/stream/stream-push.c c:src/modules/stream/stream-module.c c:src/modules/libc/libc-wst-adapt.c c:src/modules/libc/libc-wst-bytes.c c:src/modules/libc/libc-blob.c c:src/modules/libc/libc-wst-misc.c c:src/modules/libc/libc-wst-pipe.c c:src/modules/libc/libc-wst-rs.c c:src/modules/libc/libc-wst-ts.c c:src/modules/libc/libc-wst-ws.c -- one-shot interrupt at every poll of dyna:stream and web-stream paths (D50 sites)
// Each case is swept with tests/lib/dyn-isweep.h: the interrupt fires once, at poll N, for N = 1 until the case
// finishes; a native that takes, rejects with or drops it lets JS run on and the case fails at that N.
// Size-strategy cases run JS inside TransformStream enqueue and WritableStream write, so the interrupt reaches the libc-wst error paths.
// The Blob case runs JS from an ArrayIterator next getter (read once, as GetIterator does); the Response case from a pulled body.
// The socketStreams case drives the TCP adapter through __streams (js_streams_call) with a fake conn whose write runs JS.
// The engine backstop only turns a normal return with a pending uncatchable into JS_EXCEPTION; the oracle self-test proves a native
// that takes the interrupt and calls JS again still fails (isw_expect_bad), so per-site fixes stay required.
// Usage: test_stream_isweep (ISWEEP_ONLY=<substring>, ISWEEP_VERBOSE=1); run from the repo root.
#define ISWEEP_CTX_INIT stream_isweep_init
#include "../lib/dyn-isweep.h"

#include <unistd.h>

#define S "const st = await import('dyna:stream');\n"
#define CATCH ".then(ok, leak)"

#define BUSY "const busy = () => { let s = 0; for (let i = 0; i < 40; i++) s += i; return 1; };\n"

static JSValue stream_isweep_streams(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* name;
    JSValue r;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "__streams(name, ...args)");
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    r = js_streams_call(ctx, name, argc - 1, argv + 1);
    JS_FreeCString(ctx, name);
    return r;
}

static JSValue stream_isweep_swallow_call(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue r;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "__swallow_call(fn)");
    r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    }
    return r;
}

static JSValue leave_call(JSContext* ctx, JSValueConst fn)
{
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r))
        return JS_UNDEFINED;
    JS_FreeValue(ctx, r);
    return JS_UNDEFINED;
}

static JSValue leave_generic(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    return leave_call(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue leave_method0(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue fn = JS_GetPropertyStr(ctx, this_val, "fn"), r;
    (void)argc;
    (void)argv;
    if (JS_IsException(fn))
        return fn;
    r = leave_call(ctx, fn);
    JS_FreeValue(ctx, fn);
    return r;
}

static JSValue leave_data(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    (void)this_val;
    (void)magic;
    (void)data;
    return leave_call(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue leave_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int* pdone, int magic)
{
    JSValue fn = JS_GetPropertyStr(ctx, this_val, "fn"), r;
    (void)argc;
    (void)argv;
    (void)magic;
    *pdone = 0;
    if (JS_IsException(fn))
        return fn;
    r = leave_call(ctx, fn);
    JS_FreeValue(ctx, fn);
    return r;
}

static JSValue leave_set(JSContext* ctx, JSValueConst this_val, JSValueConst val)
{
    (void)this_val;
    return leave_call(ctx, val);
}

static JSValue leave_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    (void)magic;
    (void)is_reject;
    (void)arg;
    return leave_call(ctx, data);
}

static JSValue leave_rx_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue rx = JS_NewPromiseReaction(ctx, leave_rx, argc > 0 ? argv[0] : JS_UNDEFINED, 0);
    int r;
    (void)this_val;
    if (JS_IsException(rx))
        return rx;
    r = JS_EnqueueReaction(ctx, rx, JS_UNDEFINED, 0);
    JS_FreeValue(ctx, rx);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static void stream_isweep_init(JSContext* ctx)
{
    JSValue g = JS_GetGlobalObject(ctx), obj;
    JSCFunctionType ft;
    JSAtom ax = JS_NewAtom(ctx, "x");
    JS_SetPropertyStr(ctx, g, "__leave", JS_NewCFunction(ctx, leave_generic, "__leave", 1));
    JS_SetPropertyStr(ctx, g, "__leave0", JS_NewCFunction(ctx, leave_method0, "__leave0", 0));
    JS_SetPropertyStr(ctx, g, "__leave_data", JS_NewCFunctionData(ctx, leave_data, 1, 0, 0, NULL));
    ft.iterator_next = leave_next;
    JS_SetPropertyStr(ctx, g, "__leave_next", JS_NewCFunction2(ctx, ft.generic, "next", 0, JS_CFUNC_iterator_next, 0));
    ft.setter = leave_set;
    obj = JS_NewObject(ctx);
    JS_DefinePropertyGetSet(ctx, obj, ax, JS_UNDEFINED, JS_NewCFunction2(ctx, ft.generic, "x", 1, JS_CFUNC_setter, 0), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, ax);
    JS_SetPropertyStr(ctx, g, "__leave_obj", obj);
    JS_SetPropertyStr(ctx, g, "__leave_rx", JS_NewCFunction(ctx, leave_rx_start, "__leave_rx", 1));
    JS_SetPropertyStr(ctx, g, "__streams", JS_NewCFunction(ctx, stream_isweep_streams, "__streams", 1));
    JS_SetPropertyStr(ctx, g, "__swallow_call", JS_NewCFunction(ctx, stream_isweep_swallow_call, "__swallow_call", 1));
    JS_FreeValue(ctx, g);
}

static const char* const cases[][2] = {
    { "boundary: a generic native returns normally with the interrupt pending", BUSY "(async () => { for (let i = 0; i < 3; i++) __leave(busy); })()" CATCH ";\n" },
    { "boundary: a zero-argument native method call returns normally with the interrupt pending", BUSY "(async () => { const o = { m: __leave0, fn: busy }; for (let i = 0; i < 3; i++) o.m(); })()" CATCH ";\n" },
    { "boundary: a cfunction-data native returns normally with the interrupt pending", BUSY "(async () => { for (let i = 0; i < 3; i++) __leave_data(busy); })()" CATCH ";\n" },
    { "boundary: a native iterator next returns normally with the interrupt pending", BUSY "(async () => { let k = 0; for (const v of { next: __leave_next, fn: busy, [Symbol.iterator]() { return this; } }) if (++k > 3) break; })()" CATCH ";\n" },
    { "boundary: a native setter returns normally with the interrupt pending", BUSY "(async () => { for (let i = 0; i < 3; i++) __leave_obj.x = busy; })()" CATCH ";\n" },
    { "boundary: a promise reaction native returns normally with the interrupt pending", BUSY "(async () => { __leave_rx(busy); __leave_rx(busy); await 0; await 0; await 0; })()" CATCH ";\n" },
    { "socketStreams default connection highWaterMark is 16 KiB under a write flood to a conn that never drains", BUSY "(async () => { let buf = 0, maxBuf = 0, maxPiece = 0; const conn = { get bufferedAmount() { return buf; }, write(b) { busy(); buf += b.length; maxBuf = Math.max(maxBuf, buf); maxPiece = Math.max(maxPiece, b.length); return false; }, end() {}, close() {}, pause() {}, resume() {}, on() {}, once() {} };\nconst w = __streams('socketStreams', conn, { writableHighWaterMark: 1 << 20 }).writable.getWriter();\nfor (let i = 0; i < 64; i++) w.write(new Uint8Array(4096)).catch(() => {});\nfor (let i = 0; i < 8; i++) await 0;\nif (maxPiece > 16384 || maxBuf > 2 * 16384) throw new Error('piece ' + maxPiece + ' buffered ' + maxBuf);\nfor (const v of [Infinity, 2 ** 53, { valueOf() { return 4096; } }]) __streams('socketStreams', conn, {}, v);\nlet e; try { __streams('socketStreams', conn, {}, { valueOf() { return 0; } }); } catch (x) { e = x; } if (!(e instanceof RangeError)) throw new Error('valueOf 0 accepted');\ne = undefined; try { __streams('socketStreams', conn, {}, { valueOf() { throw new TypeError('hostile'); } }); } catch (x) { e = x; } if (!(e instanceof TypeError)) throw new Error('hostile valueOf swallowed'); })()" CATCH ";\n" },
    { "socketStreams connection highWaterMark: absent ends, below 1 throws RangeError", BUSY "(async () => { const conn = { bufferedAmount: 0, write(b) { busy(); return true; }, end() {}, close() {}, pause() {}, resume() {} };\nfor (const bad of [0, -1, NaN, 0.5]) { let e; try { __streams('socketStreams', conn, {}, bad); } catch (x) { e = x; } if (!(e instanceof RangeError)) throw new Error('hwm ' + bad + ' accepted'); }\nconst w = __streams('socketStreams', conn, { writableHighWaterMark: 16 }).writable.getWriter(); await w.write(new Uint8Array(40)); w.releaseLock(); })()" CATCH ";\n" },
    { "socketStreams writable over a fake conn whose write runs JS", BUSY "(async () => { const conn = { bufferedAmount: 0, write(b) { busy(); return true; }, end() {}, close() {}, pause() {}, resume() {} };\nconst { writable } = __streams('socketStreams', conn, { writableHighWaterMark: 16 }, 16);\nconst w = writable.getWriter(); await w.write(new Uint8Array(40)); w.releaseLock(); })()" CATCH ";\n" },
    { "Blob parts through a hooked ArrayIterator next", BUSY "const ap = Object.getPrototypeOf([][Symbol.iterator]()), nx = ap.next;\nlet reads = 0;\nObject.defineProperty(ap, 'next', { get() { reads++; busy(); return nx; }, configurable: true });\n(async () => { const b = new Blob(['ab', 'cd']); if (reads !== 1) throw new Error('next read ' + reads + ' times'); if (await b.text() !== 'abcd') throw new Error('bad text'); })()" CATCH ";\n" },
    { "Response body over a pulled stream", BUSY "let k = 0;\n(async () => { await new Response(new ReadableStream({ pull(c) { busy(); if (k++ < 3) c.enqueue(new Uint8Array([1])); else c.close(); } })).arrayBuffer(); })()" CATCH ";\n" },
    { "lines over a web stream", S "(async () => { for await (const l of st.lines(new ReadableStream({ start(c) { c.enqueue('a\\nbb\\n'); c.enqueue(new Uint8Array([99, 10])); c.close(); } }))) {} })()" CATCH ";\n" },
    { "lines batch over a pulled stream", S "let k = 0; (async () => { for await (const b of st.lines(new ReadableStream({ async pull(c) { await tick(); if (k++ < 2) c.enqueue('x\\ny\\n'); else c.close(); } }), { batch: true })) {} })()" CATCH ";\n" },
    { "lines rejects a bad chunk", S "(async () => { for await (const l of st.lines(new ReadableStream({ start(c) { c.enqueue(42); } }))) {} })().then(() => leak('no error'), ok);\n" },
    { "lines over a duck reader", S "let r = 0; (async () => { for await (const l of st.lines({ read(b) { if (r++) return Promise.resolve(0); b[0] = 113; b[1] = 10; b[2] = 119; return Promise.resolve(3); }, close() {} })) {} })()" CATCH ";\n" },
    { "ndjson parse and error", S "(async () => { for await (const v of st.ndjson(new ReadableStream({ start(c) { c.enqueue('{\"a\":1}\\n[2]\\n'); c.close(); } }))) {} })()" CATCH ";\n"
                                 "(async () => { for await (const v of st.ndjson(new ReadableStream({ start(c) { c.enqueue('{bad\\n'); c.close(); } }))) {} })().then(() => leak('no error'), ok);\n" },
    { "pipe toReadableStream to toWritableStream", S "let n = 0; const rs = st.toReadableStream({ read(b) { if (n++ > 2) return Promise.resolve(0); b[0] = 65; return Promise.resolve(1); }, close() {} });\n"
                                                    "rs.pipeTo(st.toWritableStream({ write(v) { return Promise.resolve(v.length); }, close() {} }))" CATCH ";\n" },
    { "fromBytes through web pipe", S "await st.fromBytes(new Uint8Array(100)).pipeTo(new WritableStream({ write() {} })); ok();\n" },
    { "TransformStream enqueue runs a size strategy", "const busy = () => { let s = 0; for (let i = 0; i < 40; i++) s += i; return 1; };\n(async () => { const ts = new TransformStream({ transform(c, ctl) { ctl.enqueue(c); ctl.enqueue(c); } }, undefined, { highWaterMark: 8, size() { return busy(); } });\nconst w = ts.writable.getWriter(), r = ts.readable.getReader();\nawait w.write(1); await r.read(); await r.read(); })()" CATCH ";\n" },
    { "WritableStream write runs a size strategy", "const busy = () => { let s = 0; for (let i = 0; i < 40; i++) s += i; return 1; };\n(async () => { const ws = new WritableStream({ write() {} }, { highWaterMark: 4, size() { return busy(); } });\nconst w = ws.getWriter(); await w.write(1); await w.write(2); await w.close(); })()" CATCH ";\n" },
    { "byte stream autoAllocate through a JS ArrayBuffer", "const AB = ArrayBuffer; globalThis.ArrayBuffer = class extends AB { constructor(n) { let s = 0; for (let i = 0; i < 40; i++) s += i; super(n); } };\n(async () => { const rs = new ReadableStream({ type: 'bytes', autoAllocateChunkSize: 16, pull(c) { c.byobRequest.view[0] = 1; c.byobRequest.respond(1); } });\nconst r = rs.getReader(); await r.read(); await r.read(); })()" CATCH ";\n" },
    { "TextDecoderStream and TextEncoderStream", "const rs = new ReadableStream({ start(c) { c.enqueue(new Uint8Array([104, 105])); c.close(); } });\n"
                                                "const out = []; await rs.pipeThrough(new TextDecoderStream()).pipeThrough(new TextEncoderStream()).pipeTo(new WritableStream({ write(c) { out.push(c); } })); ok();\n" },
    { "TransformStream with flush", "const ts = new TransformStream({ transform(c, k) { k.enqueue(c * 2); }, flush(k) { k.enqueue(0); } });\n"
                                    "const w = ts.writable.getWriter(); w.write(1); w.write(2); w.close();\n"
                                    "const r = ts.readable.getReader(); for (;;) { const x = await r.read(); if (x.done) break; } ok();\n" },
    { "byte stream BYOB read", "const rs = new ReadableStream({ type: 'bytes', pull(c) { const v = c.byobRequest.view; v[0] = 7; c.byobRequest.respond(1); } });\n"
                               "const r = rs.getReader({ mode: 'byob' }); await r.read(new Uint8Array(4)); await r.read(new Uint8Array(4)); r.releaseLock(); ok();\n" },
    { "tee and cancel", "const rs = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); c.close(); } });\n"
                        "const [a, b] = rs.tee(); const ra = a.getReader(); await ra.read(); const pc = b.cancel('x'); await ra.read(); await ra.cancel('y'); await pc; ok();\n" },
    { "WritableStream errors and abort", "const ws = new WritableStream({ write() { throw new Error('w'); } });\n"
                                         "const w = ws.getWriter(); await w.write(1).then(() => leak('no error'), () => {}); await w.closed.then(() => leak('closed'), () => {});\n"
                                         "const ws2 = new WritableStream(); await ws2.abort('stop'); ok();\n" },
    { "pipeTo with abort signal", "const ac = new AbortController(); const rs = new ReadableStream({ pull(c) { c.enqueue(1); } });\n"
                                  "const p = rs.pipeTo(new WritableStream({ write() { return tick(); } }), { signal: ac.signal }); await tick(); ac.abort('s'); await p.then(() => leak('no abort'), () => {}); ok();\n" },
    { "CompressionStream round trip", "const src = new ReadableStream({ start(c) { c.enqueue(new Uint8Array(300).fill(65)); c.close(); } });\n"
                                      "const out = []; await src.pipeThrough(new CompressionStream('gzip')).pipeThrough(new DecompressionStream('gzip')).pipeTo(new WritableStream({ write(c) { out.push(c.length); } })); ok();\n" },
    { "fromFile to toFile native pipe", S "await st.fromBytes('hello\\n').pipeTo(st.toFile('.obj/isweep-a.txt'));\n"
                                        "await st.fromFile('.obj/isweep-a.txt').pipeTo(st.toFile('.obj/isweep-b.txt'));\n"
                                        "for await (const l of st.lines(st.fromFile('.obj/isweep-b.txt'))) {} ok();\n" },
    { "stream helpers over web streams", S "const rs = new ReadableStream({ start(c) { c.enqueue('a'); c.enqueue('b'); c.close(); } });\n"
                                          "if (st.text) await st.text(rs); else for await (const x of rs) {} ok();\n" },
    { "lines over a rejected thenable read", S "(async () => { for await (const l of st.lines({ read(b) { const p = Promise.reject(new Error('r')); p.then = function (a, c) { [1, 2].map((x) => x); return Promise.prototype.then.call(this, a, c); }; return p; }, close() {} })) {} })().then(() => leak('no error'), ok);\n" },
    { "identity TransformStream with a sizing strategy", "const ts = new TransformStream(undefined, undefined, { highWaterMark: 8, size(c) { [c, c].map((x) => x); return 1; } });\n"
                                                        "const w = ts.writable.getWriter(); const r = ts.readable.getReader(); w.write(1); w.write(2); w.close(); for (;;) { const x = await r.read(); if (x.done) break; } ok();\n" },
    { "web-stream work queued at teardown", "const jobs = [() => new TextDecoderStream(), () => new TextEncoderStream(), () => new CompressionStream('gzip'), () => new DecompressionStream('gzip'),\n"
                                           "  () => new ByteLengthQueuingStrategy({ highWaterMark: 1 }).size, () => new CountQueuingStrategy({ highWaterMark: 1 }).size, () => new ReadableStream()[Symbol.asyncIterator](),\n"
                                           "  () => new ReadableStream().pipeTo(new WritableStream(), { signal: new AbortController().signal }).catch(() => {}), () => new WritableStream().getWriter().write(1),\n"
                                           "  () => new ReadableStream({ type: 'bytes' }).getReader({ mode: 'byob' }), () => new TransformStream().writable.getWriter().write(1)];\n"
                                           "for (let i = 0; i < 3; i++) for (const j of jobs) Promise.resolve().then(() => { try { j(); } catch {} });\n"
                                           "await tick(); ok();\n" },
    { "abort listeners after an interrupt", "const ac = new AbortController(); let x = 0;\n"
                                            "ac.signal.addEventListener('abort', () => { x++; }); ac.signal.addEventListener('abort', () => { try { null.x; } catch {} x++; });\n"
                                            "ac.signal.onabort = () => { try { null.y; } catch {} }; ac.abort('s'); if (x !== 2) leak('listeners'); ok();\n" },
    { "duck reader failures with a working close", S "const mk = (read) => ({ read, get close() { [1].map((x) => x); return () => { [2].map((x) => x); }; } });\n"
                                                     "for (const rd of [() => Promise.resolve(-1), () => Promise.reject(new Error('r')), () => { throw new Error('t'); }, () => Promise.resolve({ valueOf() { throw new Error('v'); } })])\n"
                                                     "  await (async () => { for await (const l of st.lines(mk(rd))) {} })().then(() => leak('no error'), () => {});\n"
                                                     "let k = 0; for await (const l of st.lines(mk(() => Promise.resolve(k++ ? 0 : 1)))) {}\n"
                                                     "const rs = st.toReadableStream(mk(() => new Promise(() => {}))); const rr = rs.getReader(); rr.read(); await rr.cancel('c'); ok();\n" },
};

int main(void)
{
    size_t i;
    long max_n = getenv("ISWEEP_MAX") ? atol(getenv("ISWEEP_MAX")) : 20000;
    alarm(600);
    if (isweep_enter_workdir() != 0)
        return 1;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        isweep_case(cases[i][0], cases[i][1], max_n);
    isw_expect_bad = 1;
    isweep_case("oracle self-test: a native that takes the interrupt and calls JS again", BUSY "(async () => { __swallow_call(busy); })()" CATCH ";\n", max_n);
    isw_expect_bad = 0;
    unlink(".obj/isweep-a.txt");
    unlink(".obj/isweep-b.txt");
    isweep_leave_workdir();
    return isweep_report("stream_isweep");
}
