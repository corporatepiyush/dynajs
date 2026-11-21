// covers: c:src/modules/libc/libc-wst-pipe.c
// Randomized promise-ordering goldens for the Streams core (v10b): 2 seeds x 400 generated scenarios (default, byte and BYOB readers,
// writers, tee, async iteration, sources of up to 39 chunks, pipeTo with
// options and AbortSignal, pipeThrough with identity and user TransformStreams; every callback returns undefined, a plain object,
// a settled, pending or rejected promise, or a side-effecting thenable; user microtask tickers interleave). Each scenario logs one
// trace; the FNV-1a hash of all traces was recorded from the pre-v10b core (main-sync-1121), so a change that adds, drops or moves
// one microtask anywhere fails here. v10b replaced the pipe loop's internal promises (read promise, resolve-thenable hop, step
// promise) and the WebIDL promise for non-object pull/write/close results with native jobs that run in the same queue slots.
// Run with --dump SEED to print one seed's traces.
import { suite } from "../lib/kit.js";

let seed = 1;
const rnd = (n) => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return (seed >>> 8) % n; };
const pick = (a) => a[rnd(a.length)];

function rec() {
    const log = [];
    const L = (s) => log.push(s);
    const ticker = (n, tag) => { let p = Promise.resolve(); for (let i = 1; i <= n; i++) { const k = i; p = p.then(() => L(tag + k)); } return p; };
    const show = (v) => v === undefined ? "u" : v && typeof v === "object" && "done" in v ? "{" + show(v.value) + "," + v.done + "}" : v instanceof Error ? "E:" + v.message : typeof v === "object" && v !== null ? "obj" : String(v);
    const watch = (name, p) => p.then((v) => L(name + " ok " + show(v)), (e) => L(name + " err " + (e && e.message !== undefined ? e.message : String(e))));
    return { log, L, ticker, watch };
}

function cbResult(r, tag) {
    switch (rnd(9)) {
    case 0: case 1: case 2: return undefined;
    case 3: return { plain: 1 };
    case 4: return Promise.resolve();
    case 5: return r.ticker(rnd(3) + 1, tag + ".");
    case 6: return { get then() { r.L(tag + " get then"); return (res) => { r.L(tag + " then called"); res(); }; } };
    case 7: return rnd(4) === 0 ? Promise.reject(new Error(tag + "-rej")) : undefined;
    default: return r.ticker(6, tag + "~");
    }
}

function makeSource(r, n) {
    const kind = rnd(3);
    let i = 0, ctl;
    const errAt = rnd(5) === 0 ? rnd(n + 1) : -1;
    const src = {
        start(c) { ctl = c; r.L("start"); if (kind === 1) { for (let k = 0; k < Math.min(n, 2); k++) c.enqueue("s" + k); } return rnd(4) === 0 ? cbResult(r, "st") : undefined; },
        pull(c) {
            r.L("pull " + c.desiredSize);
            if (i === errAt) { c.error(new Error("srcerr")); return; }
            if (i < n) c.enqueue("c" + i++); else if (rnd(2)) { c.close(); r.L("closed-src"); }
            else { i = 0; n = 0; c.close(); }
            return cbResult(r, "pl" + i);
        },
        cancel(why) { r.L("cancel " + (why && why.message || why)); return cbResult(r, "cn"); },
    };
    const hwm = pick([0, 1, 2, 16]);
    return { rs: new ReadableStream(src, { highWaterMark: hwm }), ctl: () => ctl };
}

function makeSink(r) {
    const errAt = rnd(5) === 0 ? rnd(4) : -1;
    let k = 0;
    const sink = {
        start() { r.L("wstart"); return rnd(4) === 0 ? cbResult(r, "ws") : undefined; },
        write(ch) { r.L("write " + ch); if (k++ === errAt) throw new Error("sinkerr"); return cbResult(r, "w" + ch); },
        close() { r.L("sink close"); return cbResult(r, "cl"); },
        abort(e) { r.L("sink abort " + (e && e.message || e)); return cbResult(r, "ab"); },
    };
    return new WritableStream(sink, { highWaterMark: pick([0, 1, 2, 16]) });
}

function makeTransform(r) {
    if (rnd(3) === 0) return new TransformStream();
    return new TransformStream({
        transform(ch, c) { r.L("tf " + ch); c.enqueue(ch + "'"); if (rnd(6) === 0) c.enqueue(ch + "''"); return cbResult(r, "tf"); },
        flush() { r.L("flush"); return cbResult(r, "fl"); },
    }, { highWaterMark: pick([0, 1, 4]) }, { highWaterMark: pick([0, 1, 4]) });
}

async function scenario(r) {
    const n = rnd(3) === 0 ? 20 + rnd(20) : rnd(6);
    const shape = rnd(8);
    if (shape === 4) {
        let i = 0;
        const bs = new ReadableStream({ type: "bytes", pull(c) { r.L("bpull " + c.desiredSize); if (i >= n) { c.close(); if (c.byobRequest) c.byobRequest.respond(0); return cbResult(r, "bp"); } const v = c.byobRequest && rnd(2) ? c.byobRequest.view : null; if (v) { v[0] = i++; c.byobRequest.respond(1); } else c.enqueue(new Uint8Array([i++])); return cbResult(r, "bp" + i); } }, { highWaterMark: pick([0, 1, 4]) });
        const byob = rnd(2) === 0;
        const rd = bs.getReader(byob ? { mode: "byob" } : undefined);
        for (let k = 0; k < n + 2; k++) { r.watch("b" + k, byob ? rd.read(new Uint8Array(2)) : rd.read()); if (rnd(2)) await r.ticker(rnd(3), "y" + k + "."); }
        await r.ticker(16, "t");
        return;
    }
    if (shape === 5) {
        const ws = makeSink(r);
        const w = ws.getWriter();
        r.watch("ready0", w.ready); r.watch("closed", w.closed);
        for (let k = 0; k < n + 1; k++) { r.watch("w" + k, w.write("x" + k)); r.L("ds " + w.desiredSize); if (rnd(2)) await r.ticker(rnd(3), "z" + k + "."); }
        if (rnd(3) === 0) r.watch("abort", w.abort(new Error("wab"))); else r.watch("close", w.close());
        await r.ticker(20, "t");
        return;
    }
    const { rs } = makeSource(r, n);
    if (shape === 6) {
        const [a, b] = rs.tee();
        const ra = a.getReader(), rb = b.getReader();
        for (let k = 0; k < n + 2; k++) { r.watch("ta" + k, ra.read()); if (rnd(2)) r.watch("tb" + k, rb.read()); if (rnd(2)) await r.ticker(rnd(3), "q" + k + "."); }
        if (rnd(3) === 0) r.watch("cb", rb.cancel("cx"));
        await r.ticker(20, "t");
        return;
    }
    if (shape === 7) {
        let k = 0;
        try { for await (const c of rs) { r.L("it " + c); if (++k > n + 2 || rnd(9) === 0) break; if (rnd(2)) await r.ticker(rnd(3), "f" + k + "."); } } catch (e) { r.L("iter err " + e.message); }
        await r.ticker(8, "t");
        return;
    }
    if (shape === 0) {
        const rd = rs.getReader();
        r.watch("closed", rd.closed);
        for (let k = 0; k < n + 2; k++) { r.watch("read" + k, rd.read()); if (rnd(2)) await r.ticker(rnd(4), "u" + k + "."); }
        if (rnd(3) === 0) r.watch("cancel", rd.cancel("bye"));
        await r.ticker(12, "t");
        return;
    }
    let src = rs;
    if (shape >= 2) src = rs.pipeThrough(makeTransform(r), { preventClose: rnd(4) === 0 });
    if (shape === 3) {
        const rd = src.getReader();
        for (let k = 0; k < n + 3; k++) { r.watch("tr" + k, rd.read()); if (rnd(2)) await r.ticker(rnd(3), "v" + k + "."); }
        await r.ticker(20, "t");
        return;
    }
    const ac = rnd(5) === 0 ? new AbortController() : null;
    const opts = { preventClose: rnd(4) === 0, preventAbort: rnd(4) === 0, preventCancel: rnd(4) === 0 };
    if (ac) opts.signal = ac.signal;
    r.watch("pipe", src.pipeTo(makeSink(r), opts));
    if (ac) { await r.ticker(rnd(6), "a"); ac.abort(new Error("aborted")); }
    await r.ticker(30, "t");
}

async function run(sd, count, dump) {
    seed = sd >>> 0;
    let all = "";
    for (let s = 0; s < count; s++) {
        const r = rec();
        const at = seed;
        try { await scenario(r); } catch (e) { r.L("THROW " + e.message); }
        await new Promise((res) => setTimeout(res, 1));
        const line = "#" + s + " seed " + at + ": " + r.log.join(" | ");
        if (dump) console.log(line);
        all += line + "\n";
    }
    let h = 2166136261 >>> 0;
    for (let i = 0; i < all.length; i++) h = Math.imul(h ^ all.charCodeAt(i), 16777619) >>> 0;
    return h.toString(16) + "/" + all.length;
}
const di = scriptArgs.indexOf("--dump");
if (di >= 0) { await run(+scriptArgs[di + 1], 400, true); } else {
    const t = suite("web_streams_order_fuzz");
    t.test("seed 11: 400 scenarios match the pre-v10b trace hash", async (a) => { a.eq(await run(11, 400, false), "73a1aeb3/218305"); });
    t.test("seed 12: 400 scenarios match the pre-v10b trace hash", async (a) => { a.eq(await run(12, 400, false), "10a463/211692"); });
    t.test("pipeTo does not call a patched Promise.prototype.then or constructor getter per chunk (spec: internal promise reactions)", async (a) => {
        const orig = Promise.prototype.then, desc = Object.getOwnPropertyDescriptor(Promise.prototype, "constructor");
        let calls = 0, gets = 0, i = 0;
        try {
            Promise.prototype.then = function (x, y) { calls++; return orig.call(this, x, y); };
            Object.defineProperty(Promise.prototype, "constructor", { get() { gets++; return Promise; }, configurable: true });
            const rs = new ReadableStream({ pull(c) { if (i < 50) c.enqueue(i++); else c.close(); } });
            const p = rs.pipeTo(new WritableStream({ write() {} }));
            await new Promise((res) => orig.call(p, res, res));
        } finally { Promise.prototype.then = orig; Object.defineProperty(Promise.prototype, "constructor", desc); }
        a.ok(calls <= 2 && gets <= 5, "then calls " + calls + ", constructor gets " + gets + " for 50 chunks (pre-v10b: 51 and 54)");
    });
    await t.run();
}
