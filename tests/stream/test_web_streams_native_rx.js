// covers: integration c:src/builtins/builtin-promise.c c:src/modules/libc/libc-wst-rs.c c:src/modules/libc/libc-wst-ws.c c:src/modules/libc/libc-wst-ts.c -- native reaction jobs
// v10b-22. JS_EnqueueReaction queues a native reaction (reaction object or class reaction) as a 2-argument job whose job function
// carries fulfil/reject, in the same queue slot the generic 5-argument promise reaction job used; a non-native reaction keeps the
// generic job. Rejections must reach the reaction as rejections (sink write/close/abort throwing, pull throwing, transform
// throwing) and fulfilments as fulfilments; ordering against user promise jobs is pinned by test_web_streams_order*.js.
// The pull path reads the memoised controller directly; the first pull of a pull-only stream still creates it.
// then() keeps only callables (bytecode functions, other callables); a stream object given to then() stays a pass-through value.
import { suite } from "../lib/kit.js";

const t = suite("web_streams_native_rx");

t.test("sink write throwing rejects that write and errors the stream; the next write rejects with the same error", async (a) => {
    const boom = new Error("w");
    const w = new WritableStream({ write(c) { if (c === 2) throw boom; } }).getWriter();
    await w.write(1);
    let e1 = null, e2 = null;
    await w.write(2).catch((e) => { e1 = e; });
    await w.write(3).catch((e) => { e2 = e; });
    a.ok(e1 === boom, "write 2 rejects with the thrown error");
    a.ok(e2 === boom, "write 3 rejects with the stored error");
    let ce = null;
    await w.closed.catch((e) => { ce = e; });
    a.ok(ce === boom, "closed rejects");
});

t.test("sink write returning a rejected promise rejects the write; a fulfilled one resolves it", async (a) => {
    const boom = new Error("p");
    const seen = [];
    const w = new WritableStream({ write(c) { seen.push(c); return c === "bad" ? Promise.reject(boom) : Promise.resolve(); } }).getWriter();
    a.eq(await w.write("ok"), undefined);
    let e = null;
    await w.write("bad").catch((x) => { e = x; });
    a.ok(e === boom);
    a.eq(seen.join(), "ok,bad");
});

t.test("pull throwing errors the stream and rejects the pending read; pull fulfilling with undefined keeps pulling", async (a) => {
    const boom = new Error("pull");
    let n = 0;
    const r = new ReadableStream({ pull(c) { if (++n === 3) throw boom; c.enqueue(n); } }, { highWaterMark: 0 }).getReader();
    a.eq((await r.read()).value, 1);
    a.eq((await r.read()).value, 2);
    let e = null;
    await r.read().catch((x) => { e = x; });
    a.ok(e === boom);
});

t.test("first pull of a pull-only stream gets a real controller, later pulls the same one", async (a) => {
    const seen = [];
    const r = new ReadableStream({ pull(c) { seen.push(c); c.enqueue(seen.length); } }, { highWaterMark: 0 }).getReader();
    a.eq((await r.read()).value, 1);
    a.eq((await r.read()).value, 2);
    a.ok(seen[0] instanceof ReadableStreamDefaultController);
    a.ok(seen[0] === seen[1]);
});

t.test("transform throwing errors both sides; flush rejection errors the readable", async (a) => {
    const boom = new Error("tf");
    const ts = new TransformStream({ transform(c, ctl) { if (c === 2) throw boom; ctl.enqueue(c * 10); } });
    const w = ts.writable.getWriter(), r = ts.readable.getReader();
    const p1 = w.write(1);
    a.eq((await r.read()).value, 10);
    await p1;
    let we = null, re = null;
    const rp = r.read().catch((x) => { re = x; });
    await w.write(2).catch((x) => { we = x; });
    await rp;
    a.ok(we === boom && re === boom);
    const f = new Error("flush");
    const ts2 = new TransformStream({ flush() { return Promise.reject(f); } });
    const r2 = ts2.readable.getReader();
    ts2.writable.getWriter().close().catch(() => {});
    let fe = null;
    await r2.read().catch((x) => { fe = x; });
    a.ok(fe === f);
});

t.test("pipeTo: a destination write rejection aborts the pipe with that error and cancels the source", async (a) => {
    const boom = new Error("dst");
    let cancelled = null, i = 0;
    const rs = new ReadableStream({ pull(c) { c.enqueue(i++); }, cancel(r) { cancelled = r; } });
    let e = null;
    await rs.pipeTo(new WritableStream({ write(c) { if (c === 5) throw boom; } })).catch((x) => { e = x; });
    a.ok(e === boom);
    a.ok(cancelled === boom);
});

t.test("then() keeps callables of every kind and drops non-callables", async (a) => {
    a.eq(await Promise.resolve(2).then(Math.abs), 2);
    a.eq(await Promise.resolve(-3).then(Math.abs.bind(null)), 3);
    a.eq(await Promise.resolve(4).then(async (v) => v + 1), 5);
    a.eq(await Promise.resolve(5).then(new Proxy((v) => v * 2, {})), 10);
    a.eq(await Promise.resolve(6).then({}), 6);
    a.eq(await Promise.resolve(7).then(new ReadableStream(), new WritableStream()), 7);
    let e = null;
    await Promise.reject(8).then(null, new TransformStream()).catch((x) => { e = x; });
    a.eq(e, 8);
    a.eq(await Promise.reject(9).then(null, (x) => x + 1), 10);
});

t.test("order: native reactions interleave with user jobs exactly as before (sink write vs then chain; order = node 26)", async (a) => {
    const log = [];
    const w = new WritableStream({ write(c) { log.push("w" + c); } }).getWriter();
    const p = w.write(1).then(() => log.push("p1"));
    Promise.resolve().then(() => log.push("u1")).then(() => log.push("u2")).then(() => log.push("u3"));
    await p;
    await new Promise((r) => setTimeout(r, 0));
    a.eq(log.join(), "w1,u1,u2,p1,u3");
});

await t.run();
