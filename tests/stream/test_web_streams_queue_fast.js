// covers: integration c:src/modules/libc/libc-wst-core.c c:src/modules/libc/libc-wst-ws.c c:src/modules/libc/libc-wst-rs.c c:src/modules/libc/libc-wst-pipe.c c:src/mm/js_malloc.inc.c -- queue ring, size fast path, job slots
// v10b-23. The value queues and write-request queues are power-of-two rings indexed by mask with fixed-size typed push/shift;
// an int32 chunk size >= 0 is queued without conversion, every other size value takes the full check (RangeError for
// negative, NaN, Infinity, non-number results). Job entries with up to 5 arguments are recycled through a bounded per-runtime
// free list; the realm reference is dropped inline. The await and loop drivers run jobs in one loop that stops before the next
// job once the awaited promise settles or a job error is pending, exactly where the old one-job-per-iteration loop stopped.
import { suite } from "../lib/kit.js";

const t = suite("web_streams_queue_fast");

t.test("ring wrap and growth keep FIFO order and exact queue totals", async (a) => {
    const N = 1000, got = [];
    let c0;
    const rs = new ReadableStream({ start(c) { c0 = c; } }, { highWaterMark: 0 });
    const r = rs.getReader();
    for (let round = 0; round < 7; round++) {
        const k = 1 + ((round * 37) % 13);
        for (let i = 0; i < k; i++) c0.enqueue(round * 100 + i);
        a.eq(c0.desiredSize, -k);
        for (let i = 0; i < k - 1; i++) got.push((await r.read()).value);
        a.eq(c0.desiredSize, -1);
        got.push((await r.read()).value);
    }
    const want = [];
    for (let round = 0; round < 7; round++) { const k = 1 + ((round * 37) % 13); for (let i = 0; i < k; i++) want.push(round * 100 + i); }
    a.eq(got.join(), want.join());
    const big = new ReadableStream({ start(c) { for (let i = 0; i < N; i++) c.enqueue(i); c.close(); } });
    let n = 0, ok = true;
    for await (const v of big) { if (v !== n) ok = false; n++; }
    a.ok(ok && n === N, "1000 queued chunks come out in order");
});

t.test("growth while the ring is wrapped keeps FIFO order (push 2, read 1 per round: four growths happen with head != 0)", async (a) => {
    let c0;
    const r = new ReadableStream({ start(c) { c0 = c; } }, { highWaterMark: 0 }).getReader();
    let next = 0, want = 0, ok = true;
    for (let round = 0; round < 40; round++) {
        for (let i = 0; i < 2; i++) c0.enqueue(next++);
        for (let i = 0; i < 1; i++) { const v = (await r.read()).value; if (v !== want++) ok = false; }
    }
    a.eq(c0.desiredSize, -40);
    while (want < next) { const v = (await r.read()).value; if (v !== want++) ok = false; }
    a.ok(ok, "every value in order across wrapped growth");
    let release;
    const gate = new Promise((res) => { release = res; });
    const seen = [];
    const w = new WritableStream({ write(c) { seen.push(c); return c === 0 ? gate : undefined; } }, { highWaterMark: 1000 }).getWriter();
    const ps = [];
    for (let i = 0; i < 37; i++) ps.push(w.write(i));
    release();
    await Promise.all(ps);
    a.eq(seen.join(), Array.from({ length: 37 }, (_, i) => i).join());
});

t.test("writable queue totals follow int, fractional and zero sizes", async (a) => {
    const sizes = [3, 0, 2.5, 1, 7];
    let release;
    const gate = new Promise((r) => { release = r; });
    const ws = new WritableStream({ write() { return gate; } }, { highWaterMark: 100, size: (c) => c });
    const w = ws.getWriter();
    const ps = sizes.map((s) => w.write(s));
    a.eq(w.desiredSize, 100 - 13.5);
    release();
    await Promise.all(ps);
    a.eq(w.desiredSize, 100);
});

t.test("bad size results reject and error the stream (negative, NaN, Infinity, non-number)", async (a) => {
    for (const bad of [-1, -0.5, NaN, Infinity, "1", undefined]) {
        const ws = new WritableStream({}, { size: () => bad });
        const w = ws.getWriter();
        let e = null;
        await w.write(1).catch((x) => { e = x; });
        a.ok(e instanceof RangeError, `writable size ${String(bad)} -> RangeError`);
        let threw = null;
        new ReadableStream({ start(c) { try { c.enqueue(1); } catch (x) { threw = x; } } }, { size: () => bad });
        a.ok(threw instanceof RangeError, `readable size ${String(bad)} -> RangeError`);
    }
    const w0 = new WritableStream({}, { size: () => -0 }).getWriter();
    a.eq(await w0.write(1), undefined);
});

t.test("pipe with options and many chunks: every chunk once, in order", async (a) => {
    let i = 0;
    const out = [];
    const rs = new ReadableStream({ pull(c) { if (i < 300) c.enqueue(i++); else c.close(); } }, { highWaterMark: 3 });
    await rs.pipeTo(new WritableStream({ write(c) { out.push(c); } }, { highWaterMark: 5 }), { preventClose: false, preventAbort: true, preventCancel: true });
    a.eq(out.length, 300);
    a.ok(out.every((v, k) => v === k));
    let j = 0;
    const kept = new WritableStream({ write(c) { out.push(c); } });
    await new ReadableStream({ pull(c) { if (j < 5) c.enqueue(j++); else c.close(); } }).pipeTo(kept, { preventClose: true });
    const w = kept.getWriter();
    await w.write("after");
    a.eq(out.slice(300).join(), "0,1,2,3,4,after");
});

t.test("job slots: interleaved promise, async and stream jobs keep FIFO order across many rounds", async (a) => {
    const log = [];
    const w = new WritableStream({ write(c) { log.push("w" + c); } }).getWriter();
    for (let k = 0; k < 50; k++) {
        const p = w.write(k).then(() => log.push("p" + k));
        Promise.resolve().then(() => log.push("u" + k));
        (async () => { await null; log.push("a" + k); })();
        await p;
    }
    const want = [];
    for (let k = 0; k < 50; k++) want.push("w" + k, "u" + k, "a" + k, "p" + k);
    a.eq(log.join(), want.join());
});

await t.run();
