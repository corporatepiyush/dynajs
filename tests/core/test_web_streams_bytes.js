// covers: c:src/modules/libc/libc-wst-bytes.c
// WHATWG Streams, readable byte streams: ReadableByteStreamController, BYOB readers and requests, the
// read(view, {min}) option, autoAllocateChunkSize, buffer transfer (detach) semantics and the byte-stream tee.
// Derived from the spec's ReadableByteStreamController* algorithms; the WPT copy is not vendored.
import { suite } from "../lib/kit.js";

const t = suite("web_streams_bytes");
const flush = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); };

t.test("enqueue transfers (detaches) the chunk's buffer; a default reader gets a Uint8Array", async (a) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    a.ok(ctl instanceof ReadableByteStreamController);
    const chunk = new Uint8Array([1, 2, 3]);
    ctl.enqueue(chunk);
    a.eq(chunk.buffer.detached, true, "enqueued buffer is detached");
    const { value } = await rs.getReader().read();
    a.ok(value instanceof Uint8Array);
    a.deep(Array.from(value), [1, 2, 3]);
});

t.test("enqueue validation", (a) => {
    let ctl;
    new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    a.throws(() => ctl.enqueue(new Uint8Array(0)), TypeError, "zero-length view");
    a.throws(() => ctl.enqueue("abc"), TypeError, "not a view");
    a.throws(() => ctl.enqueue(new Uint8Array(new ArrayBuffer(0))), TypeError, "zero-length buffer");
    const d = new Uint8Array(4); d.buffer.transfer();
    a.throws(() => ctl.enqueue(d), TypeError, "detached");
    ctl.close();
    a.throws(() => ctl.enqueue(new Uint8Array(1)), TypeError, "after close");
    a.throws(() => ctl.close(), TypeError, "double close");
});

t.test("BYOB read fills the caller's buffer (transferred) and returns a view of the same type", async (a) => {
    const rs = new ReadableStream({ type: "bytes", pull(c) { const v = c.byobRequest.view; v.set([9, 8, 7, 6]); c.byobRequest.respond(4); } });
    const r = rs.getReader({ mode: "byob" });
    a.ok(r instanceof ReadableStreamBYOBReader);
    const buf = new Uint16Array(4);
    const { value, done } = await r.read(buf);
    a.eq(done, false);
    a.ok(value instanceof Uint16Array, "view constructor preserved");
    a.eq(value.length, 2, "4 bytes = 2 elements");
    a.eq(buf.buffer.detached, true, "the caller's buffer was transferred");
    a.eq(value.buffer.byteLength, 8, "same capacity");
});

t.test("read(view, {min}) waits until min elements are filled", async (a) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    const r = rs.getReader({ mode: "byob" });
    const p = r.read(new Uint8Array(8), { min: 5 });
    let settled = false; p.then(() => { settled = true; });
    ctl.enqueue(new Uint8Array([1, 2, 3]));
    await flush();
    a.ok(!settled, "3 < min");
    ctl.enqueue(new Uint8Array([4, 5, 6]));
    const { value } = await p;
    a.deep(Array.from(value), [1, 2, 3, 4, 5, 6]);
});

t.test("read(view, {min}) through byobRequest.respond keeps pulling until min bytes are written", async (a) => {
    let pulls = 0;
    const rs = new ReadableStream({ type: "bytes", pull(c) { pulls++; const v = c.byobRequest.view; v.set([pulls, pulls, pulls].slice(0, v.byteLength)); c.byobRequest.respond(Math.min(3, v.byteLength)); } });
    const { value } = await rs.getReader({ mode: "byob" }).read(new Uint8Array(8), { min: 5 });
    a.eq(pulls, 2, "one respond of 3 bytes is below min");
    a.deep(Array.from(value), [1, 1, 1, 2, 2, 2]);
});

t.test("read(view, {min}) validation", async (a) => {
    const r = new ReadableStream({ type: "bytes" }).getReader({ mode: "byob" });
    await a.rejects(r.read(new Uint8Array(4), { min: 0 }), TypeError, "min 0");
    await a.rejects(r.read(new Uint8Array(4), { min: 5 }), RangeError, "min > length");
    await a.rejects(r.read(new Uint16Array(4), { min: 5 }), RangeError, "min counts elements");
    await a.rejects(r.read(new DataView(new ArrayBuffer(4)), { min: 5 }), RangeError, "DataView min counts bytes");
    await a.rejects(r.read(new Uint8Array(0)), TypeError, "zero-length view");
    await a.rejects(r.read({}), TypeError, "not a view");
    await a.rejects(r.read(new Uint8Array(4), { min: -1 }), TypeError, "EnforceRange");
    const d = new Uint8Array(4); d.buffer.transfer();
    await a.rejects(r.read(d), TypeError, "detached view");
});

t.test("a partial element stays queued: odd byte count into a Uint16Array", async (a) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    const r = rs.getReader({ mode: "byob" });
    ctl.enqueue(new Uint8Array([1, 2, 3]));
    const { value } = await r.read(new Uint16Array(2));
    a.eq(value.length, 1, "one whole element");
    const second = await r.read(new Uint8Array(4));
    a.deep(Array.from(second.value), [3], "the remainder byte is delivered next");
});

t.test("close with a partially filled element is a TypeError", async (a) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    const r = rs.getReader({ mode: "byob" });
    const p = r.read(new Uint16Array(2));
    await flush();
    ctl.byobRequest.view[0] = 1;
    ctl.byobRequest.respond(1);
    a.throws(() => ctl.close(), TypeError);
    await a.rejects(p, TypeError);
});

t.test("close: a pending BYOB read resolves done with an empty view once respond(0) is called", async (a) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    const r = rs.getReader({ mode: "byob" });
    const p = r.read(new Uint8Array(4));
    await flush();
    ctl.close();
    a.throws(() => ctl.byobRequest.respond(1), TypeError, "bytesWritten must be 0 when closed");
    ctl.byobRequest.respond(0);
    const { value, done } = await p;
    a.eq(done, true);
    a.eq(value.byteLength, 0);
    a.eq(value.buffer.byteLength, 4, "the buffer comes back");
    const after = await r.read(new Uint8Array(2));
    a.eq(after.done, true, "reads after close resolve done immediately");
});

t.test("respond validation: 0 on readable, overflow, invalidated request", async (a) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    const r = rs.getReader({ mode: "byob" });
    r.read(new Uint8Array(4));
    await flush();
    const req = ctl.byobRequest;
    a.eq(req, ctl.byobRequest, "the same request object until it is answered");
    a.throws(() => req.respond(0), TypeError);
    a.throws(() => req.respond(5), RangeError);
    req.respond(2);
    a.eq(req.view, null, "an answered request is invalidated");
    a.throws(() => req.respond(1), TypeError);
});

t.test("respondWithNewView: the same offset and buffer capacity are required", async (a) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    const r = rs.getReader({ mode: "byob" });
    const p = r.read(new Uint8Array(8));
    await flush();
    const v = ctl.byobRequest.view;
    a.throws(() => ctl.byobRequest.respondWithNewView(new Uint8Array(16)), RangeError, "different buffer capacity");
    a.throws(() => ctl.byobRequest.respondWithNewView(new Uint8Array(v.buffer, 1, 2)), RangeError, "wrong offset");
    const nv = new Uint8Array(v.buffer, 0, 3);
    nv.set([5, 6, 7]);
    ctl.byobRequest.respondWithNewView(nv);
    a.deep(Array.from((await p).value), [5, 6, 7]);
});

t.test("autoAllocateChunkSize gives default readers a byobRequest", async (a) => {
    let seen;
    const rs = new ReadableStream({ type: "bytes", autoAllocateChunkSize: 16, pull(c) { seen = c.byobRequest.view.byteLength; c.byobRequest.view[0] = 42; c.byobRequest.respond(1); } });
    const { value } = await rs.getReader().read();
    a.eq(seen, 16);
    a.deep(Array.from(value), [42]);
});

t.test("without autoAllocateChunkSize a default reader's pull sees byobRequest === null", async (a) => {
    let req = "unset";
    const rs = new ReadableStream({ type: "bytes", pull(c) { req = c.byobRequest; c.enqueue(new Uint8Array([1])); } });
    await rs.getReader().read();
    a.eq(req, null);
});

t.test("desiredSize counts bytes against highWaterMark", (a) => {
    let ctl;
    new ReadableStream({ type: "bytes", start(c) { ctl = c; } }, { highWaterMark: 10 });
    a.eq(ctl.desiredSize, 10);
    ctl.enqueue(new Uint8Array(4));
    a.eq(ctl.desiredSize, 6);
});

t.test("queued bytes are copied across BYOB reads of any size", async (a) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    ctl.enqueue(new Uint8Array([1, 2, 3, 4, 5]));
    ctl.enqueue(new Uint8Array([6, 7]));
    ctl.close();
    const r = rs.getReader({ mode: "byob" });
    const out = [];
    for (;;) {
        const { value, done } = await r.read(new Uint8Array(3));
        if (done) break;
        out.push(...value);
    }
    a.deep(out, [1, 2, 3, 4, 5, 6, 7]);
});

t.test("releasing a BYOB reader mid-read keeps the pull-into; a later default read gets the bytes", async (a) => {
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    const r = rs.getReader({ mode: "byob" });
    const p = r.read(new Uint8Array(4));
    await flush();
    r.releaseLock();
    await a.rejects(p, TypeError);
    ctl.byobRequest.view.set([1, 2]);
    ctl.byobRequest.respond(2);
    const { value } = await rs.getReader().read();
    a.deep(Array.from(value), [1, 2]);
});

t.test("cancel discards queued bytes and pending pull-intos", async (a) => {
    let ctl, why;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; }, cancel(r) { why = r; } });
    ctl.enqueue(new Uint8Array(3));
    const r = rs.getReader({ mode: "byob" });
    await r.cancel("x");
    a.eq(why, "x");
    a.eq((await r.read(new Uint8Array(1))).done, true);
});

t.test("byte tee: independent copies for both branches; BYOB read on one branch", async (a) => {
    let n = 0;
    const rs = new ReadableStream({ type: "bytes", pull(c) { if (n < 3) c.enqueue(new Uint8Array([++n, n * 10])); else { c.close(); if (c.byobRequest) c.byobRequest.respond(0); } } });
    const [b1, b2] = rs.tee();
    const r1 = b1.getReader({ mode: "byob" });
    const r2 = b2.getReader();
    const x1 = await r1.read(new Uint8Array(8));
    const x2 = await r2.read();
    a.deep(Array.from(x1.value), [1, 10]);
    a.deep(Array.from(x2.value), [1, 10]);
    a.ne(x1.value.buffer, x2.value.buffer, "branches do not share a buffer");
    const rest1 = [], rest2 = [];
    for (;;) { const r = await r1.read(new Uint8Array(8)); if (r.done) break; rest1.push(...r.value); }
    for (;;) { const r = await r2.read(); if (r.done) break; rest2.push(...r.value); }
    a.deep(rest1, [2, 20, 3, 30]);
    a.deep(rest2, [2, 20, 3, 30]);
});

t.test("byte tee: an error reaches both branches", async (a) => {
    let ctl;
    const [b1, b2] = new ReadableStream({ type: "bytes", start(c) { ctl = c; } }).tee();
    const e = new Error("bt");
    ctl.error(e);
    a.eq(await b1.getReader().closed.then(() => null, (x) => x), e);
    a.eq(await b2.getReader().closed.then(() => null, (x) => x), e);
});

t.test("async iteration over a byte stream yields Uint8Arrays", async (a) => {
    const rs = new ReadableStream({ type: "bytes", start(c) { c.enqueue(new Uint8Array([1])); c.enqueue(new Uint8Array([2, 3])); c.close(); } });
    const out = [];
    for await (const v of rs) out.push(Array.from(v));
    a.deep(out, [[1], [2, 3]]);
});

t.test("SharedArrayBuffer-backed views are refused", async (a) => {
    if (typeof SharedArrayBuffer !== "function") a.skip("no SharedArrayBuffer");
    let ctl;
    const rs = new ReadableStream({ type: "bytes", start(c) { ctl = c; } });
    a.throws(() => ctl.enqueue(new Uint8Array(new SharedArrayBuffer(4))), TypeError);
    await a.rejects(rs.getReader({ mode: "byob" }).read(new Uint8Array(new SharedArrayBuffer(4))), TypeError);
});

await t.run();
