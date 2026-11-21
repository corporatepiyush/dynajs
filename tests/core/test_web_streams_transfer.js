// flags: --std
// covers: c:src/modules/libc/libc-worker.c
// Transferable streams (Streams spec "transfer steps"): a ReadableStream, WritableStream or TransformStream in a Worker postMessage
// transfer list moves to the other thread. The sender's stream is locked and piped into a cross-realm stream over a private native port pair
// (two message pipes, one per direction, never exposed as MessagePort); the receiver gets a fresh stream on its side. Flow is pull driven as in the
// spec's cross-realm transform (one "pull" per chunk), so a fast producer never floods the receiver. Errors cross as {name, message} (the port
// protocol predates Error support in structured clone). Refusals throw DataCloneError DOMExceptions (HTML StructuredSerializeWithTransfer). A worker that exits without reading sends "went away" from its port's finalizer, so the sender's
// pipe ends instead of waiting forever. Child: web_streams_transfer_child.js.
// fix9b: the worker's reply and the stream's "close" travel on different pipes, which nothing orders (the spec's cross-realm ports are separate
// MessagePorts too), and the sender's pipeTo releases its writer once the close completes (Streams "pipe finalize"). The old case awaited the
// reply and then asserted ws.locked, which held only when the reply overtook the close (release) and failed when it did not (ASan, 10/10).
// The case now asserts the lock right after the transfer and the spec end state (unlocked, closed) after both arrive and the microtask queue
// drains: the setTimeout(0) is the macrotask boundary that guarantees the drain, not a delay.
import { suite } from "../lib/kit.js";
import * as os from "os";

const t = suite("web_streams_transfer");
const T = { timeoutMs: 10000 };
function worker() {
    const w = new os.Worker("./web_streams_transfer_child.js");
    const waiters = [];
    w.onmessage = (e) => { const f = waiters.shift(); if (f) f(e.data); };
    return {
        send(msg, transfer) { w.postMessage(msg, transfer); },
        ask(msg, transfer) { const p = new Promise((r) => waiters.push(r)); w.postMessage(msg, transfer); return p; },
        done() { w.postMessage({ op: "bye" }); w.onmessage = null; },
        raw: w,
    };
}
const W = worker();
const tick = (ms = 0) => new Promise((r) => setTimeout(r, ms));

t.test("ReadableStream to a worker: bytes arrive in order, the original is locked", async (a) => {
    let i = 0;
    const rs = new ReadableStream({ pull(c) { if (i < 100) c.enqueue(new Uint8Array(1000).fill(i++ & 255)); else c.close(); } });
    const p = W.ask({ op: "sum", rs }, [rs]);
    a.ok(rs.locked, "locked by the transfer");
    a.deep(await p, { op: "sum", n: 100000, k: 100 });
}, T);

t.test("backpressure: the worker reads 3 chunks, the source is not drained", async (a) => {
    let pulls = 0;
    const rs = new ReadableStream({ pull(c) { pulls++; c.enqueue(pulls); } }, { highWaterMark: 1 });
    const r = await W.ask({ op: "take", rs, count: 3 }, [rs]);
    a.deep(r.got, [1, 2, 3]);
    await tick(30);
    a.ok(pulls <= 6, "source pulled " + pulls + " times for 3 reads");
    await rs.cancel().catch(() => {});
}, T);

t.test("cancel in the worker reaches the source with the reason (an Error crosses as its class and message)", async (a) => {
    let reason;
    const canceled = new Promise((res) => { reason = res; });
    const rs = new ReadableStream({ pull(c) { c.enqueue(1); }, cancel(r) { reason(r); } });
    await W.ask({ op: "cancel", rs }, [rs]);
    const r = await canceled;
    a.ok(r instanceof TypeError, "a TypeError");
    a.eq(r.message, "stop");
}, T);

t.test("a source error reaches the worker's reader", async (a) => {
    let n = 0;
    const rs = new ReadableStream({ pull(c) { if (n++ < 2) c.enqueue(n); else c.error(new RangeError("broken source")); } });
    a.deep(await W.ask({ op: "readErr", rs }, [rs]), { op: "readErr", name: "RangeError", message: "broken source" });
}, T);

t.test("WritableStream to a worker: its writes and close land in the sink, then the transfer pipe releases it", async (a) => {
    const got = [];
    let closed;
    const closedP = new Promise((r) => { closed = r; });
    const ws = new WritableStream({ write(c) { got.push(c); }, close() { closed(); } });
    const reply = W.ask({ op: "produce", ws, count: 5 }, [ws]);
    a.ok(ws.locked, "transfer locks the original: a cross-realm readable is piped into it");
    await Promise.all([reply, closedP]);
    await new Promise((r) => setTimeout(r, 0));
    a.deep(got, ["line 0", "line 1", "line 2", "line 3", "line 4"]);
    a.eq(ws.locked, false, "the pipe's finalize released its writer");
    let end = "pending";
    await ws.getWriter().closed.then(() => { end = "closed"; }, () => { end = "errored"; });
    a.eq(end, "closed");
}, T);

t.test("abort in the worker reaches the sink", async (a) => {
    let res;
    const aborted = new Promise((r) => { res = r; });
    const ws = new WritableStream({ abort(r) { res(r); } });
    await W.ask({ op: "abort", ws }, [ws]);
    const r = await aborted;
    a.ok(r instanceof RangeError); a.eq(r.message, "nope");
}, T);

t.test("TransformStream from a worker: both sides cross", async (a) => {
    const { ts } = await W.ask({ op: "upper" });
    a.ok(ts instanceof TransformStream);
    const out = [];
    for await (const x of ReadableStream.from(["a", "b", "c"]).pipeThrough(ts)) out.push(x);
    a.deep(out, ["A", "B", "C"]);
}, T);

t.test("a stream nested in the message, referenced twice, arrives as one stream; other values are kept", async (a) => {
    const rs = ReadableStream.from([new Uint8Array(3), new Uint8Array(4)]);
    const r = await W.ask({ op: "nested", a: { s: rs }, b: { s: rs }, list: [1, 7] }, [rs]);
    a.deep(r, { op: "nested", same: true, kind: "ReadableStream", n: 7, arr: true });
}, T);

t.test("refusals leave the streams usable: locked, listed twice, missing from the list, unclonable message", (a) => {
    const locked = new ReadableStream(); locked.getReader();
    a.throws(() => W.send({ op: "ignore", s: locked }, [locked]), DOMException, "locked (HTML StructuredSerializeWithTransfer: DataCloneError)");
    const twice = new ReadableStream();
    a.throws(() => W.send({ op: "ignore" }, [twice, twice]), DOMException, "twice (DataCloneError)");
    a.ok(!twice.locked);
    const inMsg = new ReadableStream(), other = new ReadableStream();
    a.throws(() => W.send({ op: "ignore", s: inMsg }, [other]), DOMException, "in the message but not in the list (DataCloneError)");
    a.ok(!inMsg.locked && !other.locked);
    const s = new ReadableStream();
    a.throws(() => W.send({ op: "ignore", s, f() {} }, [s]), Error, "unclonable message");
    a.ok(!s.locked, "a failed post does not lock the stream");
    a.throws(() => W.send({ op: "ignore", s }), TypeError, "a stream in a message with no transfer list");
    class Box { constructor(v) { this.v = v; } }
    a.throws(() => W.send([1, new Box(new WritableStream())]), TypeError, "a stream inside a class instance");
}, T);

t.test("Map and Set do not clone, so a stream inside one is refused and stays usable", (a) => {
    a.throws(() => W.send({ op: "ignore", m: new Map([["k", 1]]) }), /unsupported object class/, "a plain Map");
    a.throws(() => W.send({ op: "ignore", m: new Set([1]) }), /unsupported object class/, "a plain Set");
    const inMap = new ReadableStream({ start(c) { c.enqueue("m"); c.close(); } });
    a.throws(() => W.send({ op: "ignore", m: new Map([["k", inMap]]) }, [inMap]), /unsupported object class/, "a stream as a Map value");
    const key = new WritableStream();
    a.throws(() => W.send({ op: "ignore", m: new Map([[key, 1]]) }, [key]), /unsupported object class/, "a stream as a Map key");
    const inSet = new TransformStream();
    a.throws(() => W.send({ op: "ignore", s: new Set([inSet]) }, [inSet]), /unsupported object class/, "a stream in a Set");
    a.ok(!inMap.locked && !key.locked && !inSet.readable.locked && !inSet.writable.locked, "nothing locked after the refused posts");
    return inMap.getReader().read().then((r) => a.deep(r, { value: "m", done: false }, "the Map's stream still reads locally"));
}, T);

t.test("two workers load the Streams core at once (class registration races) and both compress", async (a) => {
    const w1 = worker(), w2 = worker();
    const [r1, r2] = await Promise.all([w1.ask({ op: "gz", text: "one" }), w2.ask({ op: "gz", text: "two" })]);
    a.eq(r1.n > 10 && r2.n > 10, true, "both produced gzip output");
    w1.done(); w2.done();
}, T);

t.test("a worker that exits without reading ends the sender's pipe (the source is canceled)", async (a) => {
    const w2 = worker();
    let res;
    const canceled = new Promise((r) => { res = r; });
    const rs = new ReadableStream({ pull(c) { c.enqueue(1); }, cancel(r) { res(r); } });
    await w2.ask({ op: "ignore", rs }, [rs]);
    w2.raw.onmessage = null;
    const r = await canceled;
    a.ok(r instanceof TypeError && /went away/.test(r.message), String(r && r.message));
}, { timeoutMs: 10000 });

await t.run();
W.done();
