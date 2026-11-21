// covers: c:src/modules/libc/libc-wst-ws.c
// WHATWG Streams, WritableStream, TransformStream, queuing strategies and pipeTo/pipeThrough: backpressure,
// abort/close/error state machine, controller.signal, the prevent* options and AbortSignal. Derived from the
// spec's WritableStream*, TransformStream* and ReadableStreamPipeTo algorithms; the WPT copy is not vendored.
import { suite } from "../lib/kit.js";

const t = suite("web_streams_writable");
const flush = async () => { for (let i = 0; i < 10; i++) await Promise.resolve(); };
const tick = () => new Promise((r) => setTimeout(r, 0));
function recorder(extra) {
    const log = [];
    const sink = Object.assign({ write(c) { log.push(["write", c]); }, close() { log.push(["close"]); }, abort(r) { log.push(["abort", r]); } }, extra || {});
    return { log, sink };
}

t.test("constructor validation and dictionary order", (a) => {
    a.throws(() => new WritableStream(null), TypeError);
    a.throws(() => new WritableStream({ write: 1 }), TypeError);
    a.throws(() => new WritableStream({ type: "bytes" }), RangeError);
    a.throws(() => new WritableStream({}, { highWaterMark: -1 }), RangeError);
    const seen = [];
    new WritableStream(new Proxy({}, { get(o, k) { seen.push(String(k)); } }));
    a.deep(seen, ["abort", "close", "start", "type", "write"]);
});

t.test("writes reach the sink in order with the controller; close resolves after the sink closes", async (a) => {
    const { log, sink } = recorder();
    let ctlArg;
    sink.write = (c, ctl) => { ctlArg = ctl; log.push(["write", c]); };
    const ws = new WritableStream(sink);
    const w = ws.getWriter();
    w.write(1); w.write(2);
    await w.close();
    a.deep(log, [["write", 1], ["write", 2], ["close"]]);
    a.ok(ctlArg instanceof WritableStreamDefaultController);
    await w.closed;
});

t.test("writes are serialized: the next write waits for the previous sink promise", async (a) => {
    let active = 0, max = 0;
    const ws = new WritableStream({ async write() { active++; max = Math.max(max, active); await tick(); active--; } });
    const w = ws.getWriter();
    await Promise.all([w.write(1), w.write(2), w.write(3)]);
    a.eq(max, 1);
});

t.test("backpressure: ready and desiredSize follow the queue", async (a) => {
    let release;
    const ws = new WritableStream({ write() { return new Promise((r) => { release = r; }); } }, new CountQueuingStrategy({ highWaterMark: 2 }));
    const w = ws.getWriter();
    a.eq(w.desiredSize, 2);
    w.write("a"); w.write("b");
    a.eq(w.desiredSize, 0);
    let ready = false; w.ready.then(() => { ready = true; });
    await flush();
    a.ok(!ready, "ready is pending under backpressure");
    release(); await flush();
    a.ok(ready, "ready resolves once the queue drains below hwm");
});

t.test("ByteLengthQueuingStrategy sizes by byteLength", async (a) => {
    const s = new ByteLengthQueuingStrategy({ highWaterMark: 16 });
    a.eq(s.highWaterMark, 16);
    a.eq(s.size(new Uint8Array(5)), 5);
    a.eq(s.size, new ByteLengthQueuingStrategy({ highWaterMark: 1 }).size, "one size function per realm");
    a.eq(s.size.name, "size");
    a.eq(s.size.length, 1);
    a.eq(new CountQueuingStrategy({ highWaterMark: 1 }).size.length, 0);
    a.throws(() => new ByteLengthQueuingStrategy(), TypeError);
    a.throws(() => new CountQueuingStrategy({}), TypeError);
    const ws = new WritableStream({ write() { return new Promise(() => {}); } }, s);
    const w = ws.getWriter();
    w.write(new Uint8Array(10));
    a.eq(w.desiredSize, 6);
});

t.test("a sink write rejection errors the stream; later writes reject with it", async (a) => {
    const e = new Error("w");
    const ws = new WritableStream({ write() { throw e; } });
    const w = ws.getWriter();
    a.eq(await w.write(1).then(() => null, (x) => x), e);
    a.eq(await w.write(2).then(() => null, (x) => x), e);
    a.eq(await w.closed.then(() => null, (x) => x), e);
});

t.test("abort: sink.abort gets the reason, queued writes reject, signal fires", async (a) => {
    let signalReason, aborted = false;
    let release;
    const { log, sink } = recorder({
        start(c) { c.signal.addEventListener("abort", () => { aborted = true; signalReason = c.signal.reason; }); },
        write(c) { log.push(["write", c]); return new Promise((r) => { release = r; }); },
    });
    const ws = new WritableStream(sink);
    const w = ws.getWriter();
    await flush();
    const p1 = w.write(1), p2 = w.write(2);
    await flush();
    const ab = w.abort("stop");
    a.ok(aborted, "signal aborted synchronously");
    a.eq(signalReason, "stop");
    release();
    await ab;
    a.deep(log, [["write", 1], ["abort", "stop"]], "the in-flight write finishes, then abort");
    await p1;
    a.eq(await p2.then(() => null, (x) => x), "stop");
    a.eq(await w.closed.then(() => null, (x) => x), "stop");
});

t.test("abort on a locked stream rejects; close on a locked stream rejects", async (a) => {
    const ws = new WritableStream();
    ws.getWriter();
    await a.rejects(ws.abort(), TypeError);
    await a.rejects(ws.close(), TypeError);
});

t.test("controller.error errors the stream; close after close rejects", async (a) => {
    let ctl;
    const ws = new WritableStream({ start(c) { ctl = c; } });
    const w = ws.getWriter();
    const e = new Error("ce");
    ctl.error(e);
    a.eq(await w.closed.then(() => null, (x) => x), e);
    a.eq(w.desiredSize, null);
    const ws2 = new WritableStream();
    const w2 = ws2.getWriter();
    w2.close();
    await a.rejects(w2.close(), TypeError);
    await a.rejects(w2.write(1), TypeError, "write after close");
});

t.test("writer.releaseLock rejects closed and ready with TypeError", async (a) => {
    const ws = new WritableStream();
    const w = ws.getWriter();
    w.releaseLock();
    await a.rejects(w.closed, TypeError);
    await a.rejects(w.ready, TypeError);
    a.throws(() => w.desiredSize, TypeError);
    a.ok(!ws.locked);
});

t.test("TransformStream: identity, transform, flush, terminate", async (a) => {
    const id = new TransformStream();
    const w = id.writable.getWriter();
    w.write("x"); w.close();
    const r = id.readable.getReader();
    a.deep(await r.read(), { value: "x", done: false });
    a.deep(await r.read(), { value: undefined, done: true });

    const up = new TransformStream({ transform(c, ctl) { ctl.enqueue(c.toUpperCase()); }, flush(ctl) { ctl.enqueue("!"); } });
    const out = [];
    await ReadableStream.from(["a", "b"]).pipeThrough(up).pipeTo(new WritableStream({ write(c) { out.push(c); } }));
    a.deep(out, ["A", "B", "!"]);

    const term = new TransformStream({ transform(c, ctl) { ctl.enqueue(c); ctl.terminate(); } });
    const tw = term.writable.getWriter();
    tw.write(1).catch(() => {});
    const tr = term.readable.getReader();
    a.deep(await tr.read(), { value: 1, done: false });
    a.deep(await tr.read(), { value: undefined, done: true });
    await a.rejects(tw.write(2), TypeError, "writes after terminate");
});

t.test("TransformStream: validation, start receives the controller, readableType/writableType are RangeErrors", (a) => {
    a.throws(() => new TransformStream({ readableType: "bytes" }), RangeError);
    a.throws(() => new TransformStream({ writableType: "bytes" }), RangeError);
    a.throws(() => new TransformStream({ transform: 1 }), TypeError);
    let c;
    new TransformStream({ start(x) { c = x; } });
    a.ok(c instanceof TransformStreamDefaultController);
    a.eq(c.desiredSize, 0, "readable side hwm defaults to 0");
    const seen = [];
    new TransformStream(new Proxy({}, { get(o, k) { seen.push(String(k)); } }));
    a.deep(seen, ["cancel", "flush", "readableType", "start", "transform", "writableType"]);
});

t.test("TransformStream: a throwing transform errors both sides", async (a) => {
    const e = new Error("tx");
    const ts = new TransformStream({ transform() { throw e; } });
    const w = ts.writable.getWriter();
    const rd = ts.readable.getReader();
    rd.read().catch(() => {});
    a.eq(await w.write(1).then(() => null, (x) => x), e);
    a.eq(await rd.closed.then(() => null, (x) => x), e);
});

t.test("TransformStream: cancel on the readable side calls transformer.cancel and errors the writable", async (a) => {
    let why;
    const ts = new TransformStream({ cancel(r) { why = r; } });
    await ts.readable.cancel("rc");
    a.eq(why, "rc");
    a.eq(await ts.writable.getWriter().closed.then(() => null, (x) => x), "rc");
});

t.test("TransformStream: backpressure holds transform until the readable side pulls", async (a) => {
    let calls = 0;
    const ts = new TransformStream({ transform(c, ctl) { calls++; ctl.enqueue(c); } });
    const w = ts.writable.getWriter();
    w.write(1); w.write(2);
    await flush();
    a.eq(calls, 0, "nothing is transformed before the readable side pulls (readable hwm 0)");
    const r = ts.readable.getReader();
    a.eq((await r.read()).value, 1);
    await flush();
    a.eq(calls, 1, "one read lets exactly one chunk through");
    a.eq((await r.read()).value, 2);
});

t.test("pipeTo: copies everything and closes the destination", async (a) => {
    const { log, sink } = recorder();
    await ReadableStream.from([1, 2, 3]).pipeTo(new WritableStream(sink));
    a.deep(log, [["write", 1], ["write", 2], ["write", 3], ["close"]]);
});

t.test("pipeTo: a source error aborts the destination (or not, with preventAbort)", async (a) => {
    const e = new Error("src");
    const { log, sink } = recorder();
    const rs = new ReadableStream({ start(c) { c.enqueue(1); c.error(e); } });
    a.eq(await rs.pipeTo(new WritableStream(sink)).then(() => null, (x) => x), e);
    a.deep(log.at(-1), ["abort", e]);
    const r2 = recorder();
    const ws = new WritableStream(r2.sink);
    a.eq(await new ReadableStream({ start(c) { c.error(e); } }).pipeTo(ws, { preventAbort: true }).then(() => null, (x) => x), e);
    a.ok(!r2.log.some((l) => l[0] === "abort"));
    a.ok(!ws.locked, "locks released");
});

t.test("pipeTo: a destination error cancels the source (or not, with preventCancel)", async (a) => {
    const e = new Error("dst");
    let why = "none";
    const rs = new ReadableStream({ pull(c) { c.enqueue(1); }, cancel(r) { why = r; } });
    a.eq(await rs.pipeTo(new WritableStream({ write() { throw e; } })).then(() => null, (x) => x), e);
    a.eq(why, e);
    why = "none";
    const rs2 = new ReadableStream({ pull(c) { c.enqueue(1); }, cancel(r) { why = r; } });
    a.eq(await rs2.pipeTo(new WritableStream({ write() { throw e; } }), { preventCancel: true }).then(() => null, (x) => x), e);
    a.eq(why, "none");
    a.ok(!rs2.locked);
});

t.test("pipeTo: preventClose leaves the destination open", async (a) => {
    const { log, sink } = recorder();
    const ws = new WritableStream(sink);
    await ReadableStream.from([1]).pipeTo(ws, { preventClose: true });
    a.deep(log, [["write", 1]]);
    const w = ws.getWriter();
    await w.write(2);
    a.deep(log.at(-1), ["write", 2]);
});

t.test("pipeTo: a closed destination cancels the source with a TypeError", async (a) => {
    let why;
    const ws = new WritableStream();
    await ws.close();
    const rs = new ReadableStream({ cancel(r) { why = r; } });
    await a.rejects(rs.pipeTo(ws), TypeError);
    a.ok(why instanceof TypeError);
});

t.test("pipeTo: AbortSignal aborts both sides with signal.reason; an already-aborted signal too", async (a) => {
    const ctl = new AbortController();
    let canceled, aborted;
    const rs = new ReadableStream({ pull(c) { return tick().then(() => c.enqueue(1)); }, cancel(r) { canceled = r; } });
    const ws = new WritableStream({ abort(r) { aborted = r; } });
    const p = rs.pipeTo(ws, { signal: ctl.signal });
    await tick();
    ctl.abort("halt");
    a.eq(await p.then(() => null, (x) => x), "halt");
    a.eq(canceled, "halt");
    a.eq(aborted, "halt");
    const c2 = new AbortController();
    c2.abort("pre");
    a.eq(await ReadableStream.from([1]).pipeTo(new WritableStream(), { signal: c2.signal }).then(() => null, (x) => x), "pre");
    await a.rejects(ReadableStream.from([1]).pipeTo(new WritableStream(), { signal: {} }), TypeError, "signal must be an AbortSignal");
});

t.test("pipeTo: locked source or destination, and non-WritableStream destinations, reject", async (a) => {
    const rs = new ReadableStream(); rs.getReader();
    await a.rejects(rs.pipeTo(new WritableStream()), TypeError);
    const ws = new WritableStream(); ws.getWriter();
    await a.rejects(new ReadableStream().pipeTo(ws), TypeError);
    await a.rejects(new ReadableStream().pipeTo({ write() {} }), TypeError);
    a.throws(() => new ReadableStream().pipeThrough({ readable: 1, writable: new WritableStream() }), TypeError);
});

t.test("pipeThrough returns the transform's readable and locks the source", async (a) => {
    const ts = new TransformStream();
    const rs = ReadableStream.from([1]);
    a.eq(rs.pipeThrough(ts), ts.readable);
    a.ok(rs.locked);
});

t.test("pipeTo respects backpressure: the source is not read far ahead of a slow sink", async (a) => {
    let pulled = 0;
    const rs = new ReadableStream({ pull(c) { pulled++; c.enqueue(pulled); } }, { highWaterMark: 0 });
    let writes = 0;
    const p = rs.pipeTo(new WritableStream({ write() { writes++; return new Promise(() => {}); } }, { highWaterMark: 1 }));
    await tick(); await tick();
    a.ok(pulled <= 3, "pulled " + pulled + " with one write stuck");
    a.eq(writes, 1);
    void p;
});

t.test("transform result object: its then getter is read once by WebIDL conversion and once by the react-to resolution (no backpressure)", async (a) => {
    const log = [];
    const obj = { get then() { log.push("get then"); return undefined; } };
    const ts = new TransformStream({ transform(ch, c) { c.enqueue(ch); log.push("tf " + ch); return obj; } });
    const w = ts.writable.getWriter(), r = ts.readable.getReader();
    let p = Promise.resolve(); for (let i = 1; i <= 8; i++) { const k = i; p = p.then(() => log.push("t" + k)); }
    r.read().then((x) => log.push("read " + x.value));
    await Promise.resolve(); await Promise.resolve(); await Promise.resolve();
    w.write("a").then(() => log.push("write ok"));
    await p; await new Promise((res) => setTimeout(res, 0));
    a.eq(log.join(" | "), "t1 | t2 | t3 | tf a | get then | t4 | read a | get then | t5 | t6 | write ok | t7 | t8");
});

await t.run();
