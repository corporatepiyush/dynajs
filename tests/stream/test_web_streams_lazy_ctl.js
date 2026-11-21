// flags: --std
// covers: integration c:src/modules/libc/libc-wst-core.c c:src/modules/libc/libc-wst-rs.c c:src/modules/libc/libc-wst-bytes.c c:src/modules/libc/libc-wst-ws.c c:src/builtins/builtin-promise.c -- lazy controller objects, class reactions, start job
// v10b-20 layout. (1) A stream object is its own native promise reaction (JS_SetClassReaction): ReadableStream = pull settled,
// WritableStream = write settled, TransformStream = transform settled, so no reaction object is cached per controller. Because
// such objects are reachable from JS, Promise.prototype.then must treat them as the non-callable values they are (pass-through);
// the user-facing then normalises non-callable handlers to undefined before the reaction record is built. (2) The controller C
// state belongs to the stream; the controller JS object is created on first need (start/pull/write callbacks, byobRequest) and
// memoised, so start, pull and write see one identity. The controller object keeps the stream alive; the C state is freed by
// whichever of stream and controller is finalised last (cycle collection finalises them in any order). (3) A start result that
// is not an object settles through one direct job in the slot the reaction job used, so ordering is unchanged.
import { suite } from "../lib/kit.js";

const t = suite("web_streams_lazy_ctl");
const tick = () => new Promise((r) => setTimeout(r, 0));

t.test("stream objects passed to then() are non-callable handlers: values and rejections pass through", async (a) => {
    const rs = new ReadableStream(), ws = new WritableStream(), ts = new TransformStream();
    a.eq(await Promise.resolve(5).then(rs), 5);
    a.eq(await Promise.resolve(6).then(ws, ws), 6);
    a.eq(await Promise.resolve(7).then(ts), 7);
    let err = null;
    await Promise.reject(new Error("r")).then(rs, ws).catch((e) => { err = e.message; });
    a.eq(err, "r");
    a.eq(await Promise.resolve(8).finally(rs), 8);
    const got = [];
    await new Promise((res) => { Promise.resolve(1).then(rs).then((v) => { got.push(v); res(); }); });
    a.eq(got.join(), "1");
    a.eq(rs.locked, false);
    await rs.getReader().cancel();
});

t.test("one controller identity across start, pull and desiredSize; pull-only streams get a real controller", async (a) => {
    let s0 = null;
    const seen = [];
    const rs = new ReadableStream({ start(c) { s0 = c; }, pull(c) { seen.push(c); c.enqueue(seen.length); } });
    const r = rs.getReader();
    await r.read();
    await r.read();
    a.ok(seen.length >= 2 && seen.every((c) => c === s0), "start and every pull see the same controller");
    a.ok(s0 instanceof ReadableStreamDefaultController);
    const p = [];
    const rs2 = new ReadableStream({ pull(c) { p.push(c); if (p.length === 2) c.close(); else c.enqueue("x"); } });
    const r2 = rs2.getReader();
    a.eq((await r2.read()).value, "x");
    await r2.read();
    a.ok(p.length === 2 && p[0] === p[1] && p[0] instanceof ReadableStreamDefaultController, "pull-only controller memoised");
    const b = [];
    const bs = new ReadableStream({ type: "bytes", start(c) { b.push(c); }, pull(c) { b.push(c); c.enqueue(new Uint8Array([1])); } });
    const br = bs.getReader();
    a.eq((await br.read()).value[0], 1);
    a.ok(b.length >= 2 && b.every((c) => c === b[0]) && b[0] instanceof ReadableByteStreamController);
    const bp = [];
    const bs2 = new ReadableStream({ type: "bytes", pull(c) { bp.push(c); const q = c.byobRequest; if (q) { q.view[0] = 9; q.respond(1); } else c.enqueue(new Uint8Array([9])); } });
    const bbr = bs2.getReader({ mode: "byob" });
    a.eq((await bbr.read(new Uint8Array(4))).value[0], 9);
    a.ok(bp[0] instanceof ReadableByteStreamController);
});

t.test("writable controller identity across start and write; write-only sinks get a controller with a signal", async (a) => {
    const seen = [];
    const ws = new WritableStream({ start(c) { seen.push(c); }, write(ch, c) { seen.push(c); } });
    const w = ws.getWriter();
    await w.write(1);
    await w.write(2);
    a.ok(seen.length === 3 && seen.every((c) => c === seen[0]) && seen[0] instanceof WritableStreamDefaultController);
    const wc = [];
    const ws2 = new WritableStream({ write(ch, c) { wc.push(c); } });
    const w2 = ws2.getWriter();
    await w2.write("a");
    await w2.write("b");
    a.ok(wc[0] === wc[1] && wc[0].signal instanceof AbortSignal);
    const ws3 = new WritableStream({ write() {} });
    const w3 = ws3.getWriter();
    await w3.write(0);
    await w3.close();
    a.ok(true);
});

t.test("rejections from pull and write still error the stream through the class reaction", async (a) => {
    const rs = new ReadableStream({ pull() { return Promise.reject(new Error("pullfail")); } });
    let e1 = null;
    await rs.getReader().read().catch((e) => { e1 = e.message; });
    a.eq(e1, "pullfail");
    const ws = new WritableStream({ write() { throw new Error("wfail"); } });
    const w = ws.getWriter();
    let e2 = null;
    await w.write(1).catch((e) => { e2 = e.message; });
    a.eq(e2, "wfail");
    const ts = new TransformStream({ transform() { throw new Error("tfail"); } });
    ts.readable.getReader().read().catch(() => {});
    const tw = ts.writable.getWriter();
    let e3 = null;
    await tw.write(1).catch((e) => { e3 = e.message; });
    a.eq(e3, "tfail");
    const ts2 = new TransformStream({ transform() { return Promise.reject(new Error("tfail2")); } }, undefined, { highWaterMark: 1 });
    let e4 = null;
    await tick();
    await ts2.writable.getWriter().write(1).catch((e) => { e4 = e.message; });
    a.eq(e4, "tfail2", "no backpressure: the transform result settles through the TransformStream class reaction");
    const ts3 = new TransformStream({ transform() { throw new Error("tfail3"); } }, undefined, { highWaterMark: 1 });
    let e5 = null;
    await tick();
    await ts3.writable.getWriter().write(1).catch((e) => { e5 = e.message; });
    a.eq(e5, "tfail3");
});

t.test("controller outliving its stream, stream outliving its controller, and both dropped together", async (a) => {
    const ctls = [];
    for (let i = 0; i < 300; i++) {
        new ReadableStream({ start(c) { if (i % 2 === 0) ctls.push(c); } });
        new WritableStream({ start(c) { if (i % 3 === 0) ctls.push(c); } });
        new ReadableStream({ type: "bytes", start(c) { if (i % 5 === 0) ctls.push(c); } });
    }
    const streams = [];
    for (let i = 0; i < 300; i++) {
        let tmp = null;
        streams.push(new ReadableStream({ start(c) { tmp = c; } }), new WritableStream({ start(c) { tmp = c; } }));
        tmp = null;
    }
    await tick();
    std.gc();
    await tick();
    std.gc();
    let ok = 0;
    for (const c of ctls) {
        if (c instanceof ReadableStreamDefaultController) { c.enqueue(1); if (c.desiredSize === 0) ok++; }
        else if (c instanceof ReadableByteStreamController) { c.enqueue(new Uint8Array(2)); if (c.desiredSize === -2) ok++; }
        else { c.error(new Error("late")); ok++; }
    }
    a.eq(ok, ctls.length, "controllers keep their stream state alive");
    a.eq(streams.filter((s) => s.locked === false).length, streams.length);
    ctls.length = 0;
    streams.length = 0;
    std.gc();
    a.ok(true);
});

t.test("start results: undefined, a value, a fulfilled and a rejected promise all reach the start hop", async (a) => {
    const log = [];
    new ReadableStream({ start() {}, pull() { log.push("u"); } });
    new ReadableStream({ start() { return 3; }, pull() { log.push("v"); } });
    new ReadableStream({ start() { return Promise.resolve(); }, pull() { log.push("p"); } });
    const bad = new ReadableStream({ start() { return Promise.reject(new Error("sfail")); } });
    new WritableStream({ start() { return 1; }, write() {} }).getWriter().write(1).then(() => log.push("w"));
    await tick();
    a.eq(log.sort().join(), "p,u,v,w");
    let e = null;
    await bad.getReader().read().catch((x) => { e = x.message; });
    a.eq(e, "sfail");
});

await t.run();
