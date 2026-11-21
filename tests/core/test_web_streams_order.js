// covers: c:src/modules/libc/libc-wst-pipe.c
// Promise-ordering goldens for the WHATWG Streams core: each scenario records, in one trace, the property reads the core makes on
// underlying sources, sinks, strategies and options (Proxy get traps), the calls into them, the settlement of every promise it hands out,
// and a ticker of user microtasks ("t1".."tN") interleaved with them, so one extra or missing tick moves an entry. The goldens were
// produced by the v7 JS core (the spec-exact reference); run with --print to dump fresh traces.
// pipe_basic, pipe_dest_error and pipe_through were re-recorded in v10a: pipeTo's read chunk steps now queue a microtask before the
// write (WPT streams/piping/general-addition.any.js "enqueue() must not synchronously call write algorithm"), so each write lands one
// tick later and the source's next pull now precedes it; nothing else in any trace moved.
// v10c re-recorded readable_error_start, writable_basic, writable_abort_mid_write, transform_basic, pipe_through, text_streams: WebIDL "a promise resolved with x" (start results) and
// the Promise<T> conversion of callback results (pull/write/close/abort/cancel/transform/flush) always create a NEW promise that adopts
// x, so a callback that returns a promise settles two microtasks later than PromiseResolve would (WPT transform-streams/errors.any.js
// and general.any.js "... immediately after readable.cancel()", cancel.any.js "... parallel writable.close() should reject ...").
// v10b-7 added start_* and writable_signal (recorded on the pre-v10b-7 core): start results now reach the controller through a
// native hop chain with no adopting promise when Promise.prototype.then/constructor/@@species are untouched, and the
// controller's AbortController is created on first signal read or abort; these traces pin the patched and lazy edges.
import { suite } from "../lib/kit.js";

const t = suite("web_streams_order");
const PRINT = scriptArgs.includes("--print");
function rec() {
    const log = [];
    const L = (s) => log.push(s);
    const ticker = (n, tag = "t") => { let p = Promise.resolve(); for (let i = 1; i <= n; i++) { const k = i; p = p.then(() => L(tag + k)); } return p; };
    const spy = (name, obj) => new Proxy(obj, { get(o, k, r) { if (typeof k === "string") L("get " + name + "." + k); return Reflect.get(o, k, r); } });
    const watch = (name, p) => p.then((v) => L(name + " ok " + show(v)), (e) => L(name + " err " + (e && e.message !== undefined ? e.message : String(e))));
    return { log, L, ticker, spy, watch };
}
function show(v) {
    if (v === undefined) return "u";
    if (v && typeof v === "object" && "done" in v) return "{" + (v.value instanceof Uint8Array ? "u8:" + Array.from(v.value).join(",") : show(v.value)) + "," + v.done + "}";
    if (v instanceof Uint8Array) return "u8:" + Array.from(v).join(",");
    return typeof v === "string" ? JSON.stringify(v) : String(v);
}
const S = {};

S.readable_default = async (r) => {
    const src = r.spy("src", { start(c) { r.L("start"); c.enqueue("a"); }, pull(c) { r.L("pull " + c.desiredSize); c.enqueue("p"); }, cancel(why) { r.L("cancel " + why); } });
    const rs = new ReadableStream(src, r.spy("qs", { highWaterMark: 2, size(ch) { r.L("size " + ch); return 1; } }));
    const rd = rs.getReader();
    r.watch("read1", rd.read()); r.watch("read2", rd.read()); r.watch("read3", rd.read());
    r.watch("closed", rd.closed);
    await r.ticker(8);
    r.watch("cancel", rd.cancel("bye"));
    await r.ticker(4, "u");
};
S.readable_pending_reads_close = async (r) => {
    let ctl;
    const rs = new ReadableStream({ start(c) { ctl = c; } });
    const rd = rs.getReader();
    r.watch("r1", rd.read()); r.watch("r2", rd.read()); r.watch("closed", rd.closed);
    ctl.enqueue(1); r.L("enq"); ctl.close(); r.L("close");
    await r.ticker(5);
};
S.readable_thenable_pull = async (r) => {
    let n = 0;
    const rs = new ReadableStream({ pull(c) { r.L("pull"); c.enqueue(n++); const th = { get then() { r.L("get then"); return (res) => { r.L("then called"); res(); }; } }; return th; } }, { highWaterMark: 0 });
    const rd = rs.getReader();
    r.watch("r1", rd.read()); await r.ticker(6); r.watch("r2", rd.read()); await r.ticker(6, "u");
};
S.readable_error_start = async (r) => {
    const rs = new ReadableStream({ start() { r.L("start"); return Promise.reject(new Error("boom")); } });
    const rd = rs.getReader();
    r.watch("read", rd.read()); r.watch("closed", rd.closed);
    await r.ticker(5);
};
S.writable_basic = async (r) => {
    const sink = r.spy("sink", { start() { r.L("start"); }, write(ch) { r.L("write " + ch); return r.ticker(2, "w" + ch + "."); }, close() { r.L("sink close"); }, abort(e) { r.L("sink abort " + e); } });
    const ws = new WritableStream(sink, r.spy("qs", { highWaterMark: 2, size(ch) { r.L("size " + ch); return 1; } }));
    const w = ws.getWriter();
    r.L("desired " + w.desiredSize);
    r.watch("ready0", w.ready);
    r.watch("w1", w.write("x")); r.watch("w2", w.write("y")); r.watch("w3", w.write("z"));
    r.L("desired " + w.desiredSize);
    r.watch("ready1", w.ready);
    r.watch("close", w.close()); r.watch("closed", w.closed);
    await r.ticker(14);
};
S.writable_abort_mid_write = async (r) => {
    let release;
    const ws = new WritableStream({ write(ch) { r.L("write " + ch); return new Promise((res) => { release = res; }); }, abort(e) { r.L("abort " + e); } });
    const w = ws.getWriter();
    r.watch("w1", w.write(1)); r.watch("w2", w.write(2));
    await r.ticker(3);
    r.watch("abort", w.abort("stop")); r.watch("closed", w.closed); r.watch("ready", w.ready);
    await r.ticker(3, "u");
    release(); r.L("released");
    await r.ticker(6, "v");
};
S.writable_close_error = async (r) => {
    const ws = new WritableStream({ close() { r.L("close"); throw new Error("nope"); } });
    const w = ws.getWriter();
    r.watch("close", w.close()); r.watch("closed", w.closed);
    await r.ticker(5);
    w.releaseLock(); r.L("released");
    r.watch("closed2", w.closed);
    await r.ticker(3, "u");
};
S.transform_basic = async (r) => {
    const tr = r.spy("tr", { start(c) { r.L("ts start"); }, transform(ch, c) { r.L("transform " + ch); c.enqueue(ch + "!"); c.enqueue(ch + "?"); }, flush(c) { r.L("flush"); } });
    const ts = new TransformStream(tr, { highWaterMark: 1 }, { highWaterMark: 0 });
    const w = ts.writable.getWriter(), rd = ts.readable.getReader();
    r.watch("w1", w.write("a")); r.watch("w2", w.write("b"));
    r.watch("r1", rd.read()); r.watch("r2", rd.read()); r.watch("r3", rd.read());
    r.watch("close", w.close()); r.watch("r4", rd.read()); r.watch("r5", rd.read());
    await r.ticker(14);
};
S.pipe_basic = async (r) => {
    let i = 0;
    const rs = new ReadableStream({ pull(c) { r.L("pull " + i); if (i < 3) c.enqueue(i++); else c.close(); } }, { highWaterMark: 1 });
    const ws = new WritableStream({ write(ch) { r.L("write " + ch); }, close() { r.L("wclose"); } }, { highWaterMark: 1 });
    r.watch("pipe", rs.pipeTo(ws, r.spy("opts", { preventClose: false })));
    await r.ticker(24);
};
S.pipe_dest_error = async (r) => {
    const rs = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); }, cancel(e) { r.L("cancel " + e.message); } });
    const ws = new WritableStream({ write(ch) { r.L("write " + ch); if (ch === 2) throw new Error("w2"); } });
    r.watch("pipe", rs.pipeTo(ws));
    await r.ticker(16);
};
S.pipe_through = async (r) => {
    const rs = new ReadableStream({ start(c) { c.enqueue("x"); c.close(); } });
    const out = rs.pipeThrough(new TransformStream({ transform(ch, c) { r.L("tf " + ch); c.enqueue(ch.toUpperCase()); } }));
    const rd = out.getReader();
    r.watch("r1", rd.read()); r.watch("r2", rd.read());
    await r.ticker(16);
};
S.tee = async (r) => {
    let n = 0;
    const rs = new ReadableStream({ pull(c) { r.L("pull"); if (n < 2) c.enqueue(n++); else c.close(); } });
    const [a, b] = rs.tee();
    const ra = a.getReader(), rb = b.getReader();
    r.watch("a1", ra.read()); r.watch("b1", rb.read()); r.watch("a2", ra.read()); r.watch("a3", ra.read()); r.watch("b2", rb.read());
    await r.ticker(14);
};
S.bytes_byob = async (r) => {
    const src = r.spy("bsrc", { type: "bytes", autoAllocateChunkSize: 4, pull(c) { const q = c.byobRequest; r.L("pull " + (q ? q.view.byteLength : "none")); if (q) { q.view[0] = 7; q.respond(1); } else c.enqueue(new Uint8Array([9])); } });
    const rs = new ReadableStream(src);
    const br = rs.getReader({ mode: "byob" });
    r.watch("b1", br.read(new Uint8Array(3)));
    await r.ticker(5);
    br.releaseLock();
    const dr = rs.getReader();
    r.watch("d1", dr.read());
    await r.ticker(5, "u");
};
S.bytes_enqueue_pending = async (r) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    const br = rs.getReader({ mode: "byob" });
    r.watch("b1", br.read(new Uint8Array(4), { min: 3 }));
    ctl.enqueue(new Uint8Array([1, 2])); r.L("enq2");
    await r.ticker(3);
    ctl.enqueue(new Uint8Array([3, 4, 5])); r.L("enq3");
    r.watch("b2", br.read(new Uint8Array(4)));
    ctl.close(); r.L("close");
    r.watch("b3", br.read(new Uint8Array(4)));
    await r.ticker(6, "u");
};
S.async_iter = async (r) => {
    const rs = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); }, cancel() { r.L("cancel"); } });
    const it = rs.values({ preventCancel: false });
    r.watch("n1", it.next()); r.watch("n2", it.next()); r.watch("ret", it.return("x"));
    await r.ticker(8);
};
S.text_streams = async (r) => {
    const ts = new TextEncoderStream();
    const w = ts.writable.getWriter(), rd = ts.readable.getReader();
    r.watch("w1", w.write("hé")); r.watch("r1", rd.read()); r.watch("close", w.close()); r.watch("r2", rd.read());
    const dec = new TextDecoderStream();
    const dw = dec.writable.getWriter(), dr = dec.readable.getReader();
    r.watch("dw", dw.write(new Uint8Array([0x68, 0xc3]))); r.watch("dr1", dr.read()); r.watch("dw2", dw.write(new Uint8Array([0xa9]))); r.watch("dr2", dr.read());
    await r.ticker(12);
};
S.strategies = async (r) => {
    const b = new ByteLengthQueuingStrategy(r.spy("init", { highWaterMark: 16 }));
    const c = new CountQueuingStrategy({ highWaterMark: 2 });
    r.L("b " + b.highWaterMark + " " + b.size(new Uint8Array(5)) + " c " + c.highWaterMark + " " + c.size());
    const rs = new ReadableStream({ start(ctl) { ctl.enqueue(new Uint8Array(10)); r.L("ds " + ctl.desiredSize); ctl.enqueue(new Uint8Array(10)); r.L("ds " + ctl.desiredSize); } }, b);
    r.watch("lockedRead", rs.getReader().read());
    await r.ticker(3);
};

S.start_patched_then = async (r) => {
    const orig = Promise.prototype.then;
    Promise.prototype.then = function (a, b) { r.L("pthen"); return orig.call(this, a, b); };
    try {
        const ts = new TransformStream({ transform(ch, c) { r.L("tf " + ch); c.enqueue(ch); } });
        const rs = new ReadableStream({ start() { r.L("rs start"); return Promise.resolve(); }, pull(c) { r.L("pull"); c.enqueue(1); } });
        const ws = new WritableStream({ start() { r.L("ws start"); return Promise.resolve(); }, write(ch) { r.L("write " + ch); } });
        r.watch("tw", ts.writable.getWriter().write("a")); r.watch("rr", rs.getReader().read()); r.watch("ww", ws.getWriter().write("b"));
        await r.ticker(8);
    } finally { Promise.prototype.then = orig; }
};
S.start_patched_species = async (r) => {
    const d0 = Object.getOwnPropertyDescriptor(Promise.prototype, "constructor"), d1 = Object.getOwnPropertyDescriptor(Promise, Symbol.species);
    Object.defineProperty(Promise.prototype, "constructor", { configurable: true, get() { r.L("get ctor"); return Promise; } });
    Object.defineProperty(Promise, Symbol.species, { configurable: true, get() { r.L("get species"); return Promise; } });
    try {
        const ts = new TransformStream();
        const rs = new ReadableStream({ start() { r.L("rs start"); return new Promise((res) => res()); }, pull(c) { r.L("pull"); c.enqueue(1); } });
        r.watch("tw", ts.writable.getWriter().write("a")); r.watch("tr", ts.readable.getReader().read()); r.watch("rr", rs.getReader().read());
        await r.ticker(8);
    } finally { Object.defineProperty(Promise.prototype, "constructor", d0); Object.defineProperty(Promise, Symbol.species, d1); }
};
S.start_patched_later = async (r) => {
    const orig = Promise.prototype.then;
    const rs = new ReadableStream({ start() { r.L("rs start"); return Promise.resolve(7); }, pull(c) { r.L("pull"); c.enqueue(1); } });
    const d0 = Object.getOwnPropertyDescriptor(Promise.prototype, "constructor");
    Object.defineProperty(Promise.prototype, "constructor", { configurable: true, get() { r.L("get ctor"); return Promise; } });
    Promise.prototype.then = function (a, b) { r.L("pthen"); return orig.call(this, a, b); };
    try { r.watch("rr", rs.getReader().read()); await r.ticker(6); } finally { Promise.prototype.then = orig; Object.defineProperty(Promise.prototype, "constructor", d0); }
};
S.start_value_then_getter = async (r) => {
    const v = { get then() { r.L("get then"); return 5; } };
    const rs = new ReadableStream({ start() { r.L("rs start"); return Promise.resolve(1).then(() => v); }, pull(c) { r.L("pull"); c.enqueue(1); } });
    const ws = new WritableStream({ async start() { r.L("ws start"); return v; }, write(ch) { r.L("write " + ch); } });
    r.watch("rr", rs.getReader().read()); r.watch("ww", ws.getWriter().write("b"));
    await r.ticker(10);
};
S.start_rejects = async (r) => {
    const rs = new ReadableStream({ start() { r.L("rs start"); return Promise.reject(new Error("rsboom")); } });
    const ws = new WritableStream({ start() { r.L("ws start"); return Promise.reject(new Error("wsboom")); } });
    const ts = new TransformStream({ start() { r.L("ts start"); return Promise.reject(new Error("tsboom")); } });
    r.watch("rr", rs.getReader().read()); r.watch("ww", ws.getWriter().write("b")); r.watch("tr", ts.readable.getReader().read());
    await r.ticker(8);
};
S.writable_signal = async (r) => {
    let c1, c2, c3, c4;
    const ws1 = new WritableStream({ start(c) { c1 = c; const s = c.signal; r.L("same " + (s === c.signal) + " aborted " + s.aborted); s.addEventListener("abort", () => r.L("abort event " + s.reason)); } });
    const ws2 = new WritableStream({ start(c) { c2 = c; } });
    const ws3 = new WritableStream({ start(c) { c3 = c; } });
    const ws4 = new WritableStream({ start(c) { c4 = c; }, write() { return new Promise(() => {}); } });
    await r.ticker(2);
    r.watch("a1", ws1.abort("r1")); r.watch("a2", ws2.abort("r2"));
    r.L("s2 " + c2.signal.aborted + " " + c2.signal.reason + " same " + (c2.signal === c2.signal));
    c3.error(new Error("e3")); r.L("s3 " + c3.signal.aborted);
    const w4 = ws4.getWriter(); r.watch("w4", w4.write(1)); await r.ticker(2, "v");
    c4.error(new Error("e4")); r.watch("a4", w4.abort("r4")); r.watch("a4b", ws4.abort === undefined ? null : w4.abort("r4b"));
    r.L("s4 " + c4.signal.aborted + " " + c4.signal.reason);
    await r.ticker(6, "u");
};

const GOLDEN = {
    readable_default: ["get qs.highWaterMark","get qs.size","get src.autoAllocateChunkSize","get src.cancel","get src.pull","get src.start","get src.type","start","size a","pull 2","read1 ok {\"a\",false}","t1","read2 ok {\"p\",false}","pull 2","t2","read3 ok {\"p\",false}","pull 2","size p","t3","pull 1","size p","t4","t5","t6","t7","t8","cancel bye","closed ok u","u1","cancel ok u","u2","u3","u4","end1","end2","end3","end4"],
    readable_pending_reads_close: ["enq","close","r1 ok {1,false}","closed ok u","r2 ok {u,true}","t1","t2","t3","t4","t5","end1","end2","end3","end4"],
    readable_thenable_pull: ["pull","get then","t1","r1 ok {0,false}","then called","t2","t3","t4","t5","t6","pull","get then","then called","r2 ok {1,false}","u1","u2","u3","u4","u5","u6","end1","end2","end3","end4"],
    readable_error_start: ["start","t1","t2","t3","closed err boom","read err boom","t4","t5","end1","end2","end3","end4"],
    writable_basic: ["get qs.highWaterMark","get qs.size","get sink.abort","get sink.close","get sink.start","get sink.type","get sink.write","start","desired 2","size x","size y","size z","desired -1","write x","ready0 ok u","ready1 ok u","t1","wx.1","t2","wx.2","t3","t4","write y","t5","w1 ok u","wy.1","t6","wy.2","t7","t8","write z","t9","w2 ok u","wz.1","t10","wz.2","t11","t12","sink close","t13","w3 ok u","t14","close ok u","closed ok u","end1","end2","end3","end4"],
    writable_abort_mid_write: ["write 1","t1","t2","t3","ready err stop","u1","u2","u3","released","v1","abort stop","v2","w1 ok u","w2 err stop","v3","abort ok u","closed err stop","v4","v5","v6","end1","end2","end3","end4"],
    writable_close_error: ["close","t1","t2","close err nope","closed err nope","t3","t4","t5","released","closed2 err the writer's lock was released","u1","u2","u3","end1","end2","end3","end4"],
    transform_basic: ["get tr.cancel","get tr.flush","get tr.readableType","get tr.start","get tr.transform","get tr.writableType","ts start","t1","t2","t3","transform a","t4","r1 ok {\"a!\",false}","r2 ok {\"a?\",false}","t5","t6","transform b","t7","w1 ok u","r3 ok {\"b!\",false}","r4 ok {\"b?\",false}","t8","flush","t9","w2 ok u","t10","r5 ok {u,true}","t11","close ok u","t12","t13","t14","end1","end2","end3","end4"],
    pipe_basic: ["get opts.preventAbort","get opts.preventCancel","get opts.preventClose","get opts.signal","pull 0","t1","pull 1","write 0","t2","t3","t4","pull 2","t5","write 1","t6","t7","t8","pull 3","t9","write 2","t10","t11","t12","t13","t14","wclose","t15","t16","t17","pipe ok u","t18","t19","t20","t21","t22","t23","t24","end1","end2","end3","end4"],
    pipe_dest_error: ["t1","write 1","t2","t3","t4","t5","write 2","t6","t7","cancel w2","t8","t9","t10","pipe err w2","t11","t12","t13","t14","t15","t16","end1","end2","end3","end4"],
    pipe_through: ["t1","t2","t3","tf x","t4","r1 ok {\"X\",false}","t5","t6","t7","t8","t9","t10","t11","t12","r2 ok {u,true}","t13","t14","t15","t16","end1","end2","end3","end4"],
    tee: ["pull","t1","pull","t2","pull","a1 ok {0,false}","b1 ok {0,false}","t3","a2 ok {1,false}","b2 ok {1,false}","a3 ok {u,true}","t4","t5","t6","t7","t8","t9","t10","t11","t12","t13","t14","end1","end2","end3","end4"],
    bytes_byob: ["get bsrc.autoAllocateChunkSize","get bsrc.cancel","get bsrc.pull","get bsrc.start","get bsrc.type","pull 3","t1","b1 ok {u8:7,false}","t2","t3","t4","t5","pull 4","d1 ok {u8:7,false}","u1","u2","u3","u4","u5","end1","end2","end3","end4"],
    bytes_enqueue_pending: ["enq2","t1","t2","t3","enq3","close","b1 ok {u8:1,2,3,4,false}","b2 ok {u8:5,false}","b3 ok {u8:,true}","u1","u2","u3","u4","u5","u6","end1","end2","end3","end4"],
    async_iter: ["t1","n1 ok {1,false}","t2","t3","t4","n2 ok {2,false}","cancel","t5","t6","t7","t8","ret ok {\"x\",true}","end1","end2","end3","end4"],
    text_streams: ["t1","t2","t3","t4","r1 ok {u8:104,195,169,false}","dr1 ok {\"h\",false}","t5","t6","t7","w1 ok u","dw ok u","dr2 ok {\"é\",false}","t8","r2 ok {u,true}","t9","close ok u","dw2 ok u","t10","t11","t12","end1","end2","end3","end4"],
    start_patched_then: ["rs start","ws start","pthen","pthen","pthen","pthen","pthen","pthen","pthen","pthen","pthen","pthen","pthen","pthen","pthen","pthen","pthen","t1","t2","pull","write b","t3","rr ok {1,false}","pull","t4","ww ok u","t5","t6","t7","t8","end1","end2","end3","end4"],
    start_patched_species: ["rs start","get ctor","get species","get ctor","get species","get ctor","get species","get ctor","get species","get ctor","get species","get ctor","get species","get ctor","get species","get ctor","get species","get ctor","get species","get ctor","get species","get ctor","get species","get ctor","get ctor","get ctor","get ctor","get species","get ctor","get species","get ctor","get species","t1","t2","pull","t3","rr ok {1,false}","pull","t4","tr ok {\"a\",false}","get ctor","get species","t5","t6","t7","tw ok u","t8","end1","end2","end3","end4"],
    start_patched_later: ["rs start","pthen","get ctor","pthen","get ctor","pthen","get ctor","pthen","get ctor","pthen","get ctor","pthen","get ctor","pthen","get ctor","get ctor","get ctor","get ctor","get ctor","t1","t2","pull","t3","rr ok {1,false}","pull","t4","t5","t6","end1","end2","end3","end4"],
    start_value_then_getter: ["rs start","ws start","get then","get then","t1","get then","get then","t2","pull","write b","t3","rr ok {1,false}","pull","t4","ww ok u","t5","t6","t7","t8","t9","t10","end1","end2","end3","end4"],
    start_rejects: ["rs start","ws start","ts start","t1","t2","t3","rr err rsboom","ww err wsboom","t4","tr err tsboom","t5","t6","t7","t8","end1","end2","end3","end4"],
    writable_signal: ["same true aborted false","t1","t2","abort event r1","s2 true r2 same true","s3 false","v1","a1 ok u","a2 ok u","v2","s4 true r4","u1","u2","u3","u4","u5","u6","end1","end2","end3","end4"],
    strategies: ["get init.highWaterMark","b 16 5 c 2 1","ds 6","ds -4","lockedRead ok {u8:0,0,0,0,0,0,0,0,0,0,false}","t1","t2","t3","end1","end2","end3","end4"],
};

const ONLY = (scriptArgs.find((a) => a.startsWith("--only=")) || "").slice(7).split(",").filter(Boolean);
for (const name of Object.keys(S).filter((n) => ONLY.length === 0 || ONLY.some((p) => n.startsWith(p)))) {
    t.test(name, async (a) => {
        const r = rec();
        await S[name](r);
        await r.ticker(4, "end");
        if (PRINT) { console.log("@@" + name + "@@" + JSON.stringify(r.log)); a.ok(true); return; }
        a.deep(r.log, GOLDEN[name], name);
    });
}
await t.run();
