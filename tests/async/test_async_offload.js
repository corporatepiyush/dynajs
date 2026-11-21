// covers: dyna:async.offload
// Owner ask E: on-demand concurrency with no ...Async twins. offload(fn, ...args) runs a native dyna:* function on a fiber;
// the stretch the native marks as safe off the loop (dyna:compress gzip: deflate over its private input copy) runs on a
// worker thread and the promise settles back on the loop thread. Called directly, gzip stays synchronous.
// Oracle: the synchronous call on the same input (byte-identical output) plus gunzip round trip.
// Off-loop case (mechanism, not timing; a 0 ms timer race flaked under load): run inline, the fiber settles the promise
// before offload() returns, so Promise.race([p, resolved]) picks p; sent to the pool, p cannot settle until the loop
// runs the job's completion, so the race picks the marker. With the gzip offload mark removed the race picks p.
// Frame-chain case: the native's call frame lives on the fiber. Unless the offload entry starts the fiber's frame chain
// empty (JS_SwapStackFrame(rt, NULL)), that frame points at the caller's frames, gone once offload() returned, so the next native error
// after an awaited offload walked freed frames building its backtrace (ASan heap-buffer-overflow in build_backtrace_held; SIGSEGV on release).
// The marks skip their out-of-line call while no fiber is live (an atomic count, so a direct call pays about 2 ns);
// the first case runs before any other fiber exists: with the count never raised, that first offload ran inline.
import { suite } from "../lib/kit.js";
import { offload } from "dyna:async";
import { gzip, gunzip } from "dyna:compress";
import { stats } from "dyna:mathx";
import { settledInline } from "../lib/offload_check.js";

const t = suite("async.offload");

function text(n) {
    const u = new Uint8Array(n);
    const enc = new TextEncoder();
    for (let i = 0; i < n; i += 4096) u.set(enc.encode(("row " + i + " lorem ipsum dolor sit amet ").repeat(8)).subarray(0, Math.min(200, n - i)), i);
    return u;
}
const same = (x, y) => {
    if (x.length !== y.length) return false;
    for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false;
    return true;
};
const BIG = text(8 << 20);

t.test("the first offload of the process already leaves the loop thread (the live-fiber count gates the marks)", async (a) => {
    a.eq(await settledInline(gzip, [BIG]), false, "the first offload is still pending when offload() returns");
});

t.test("the offloaded result is byte-identical to the direct call and round-trips", async (a) => {
    for (const n of [0, 1, 64, 4096, 1 << 20]) {
        const u = text(n);
        const p = offload(gzip, u);
        a.ok(p instanceof Promise, `n=${n}: offload returns a Promise`);
        const r = await p;
        a.ok(r instanceof Uint8Array, `n=${n}: resolves to a Uint8Array`);
        a.ok(same(r, gzip(u)), `n=${n}: same bytes as gzip()`);
        a.ok(same(gunzip(r), u), `n=${n}: gunzip restores the input`);
    }
    const r9 = await offload(gzip, BIG, { level: 9 });
    a.ok(same(r9, gzip(BIG, { level: 9 })), "options pass through (level 9)");
});

t.test("the direct call is unchanged: synchronous, not a promise", (a) => {
    const r = gzip(text(4096));
    a.ok(r instanceof Uint8Array, "gzip() returns bytes, not a Promise");
});

t.test("the work leaves the loop thread: the promise is still pending when offload() returns", async (a) => {
    a.eq(await settledInline(gzip, [text(64)]), false, "even a 64-byte gzip goes to the pool");
    const p = offload(gzip, BIG);
    a.eq(await Promise.race([p, Promise.resolve("pending")]), "pending", "an 8 MiB gzip is pending at return");
    const r = await p;
    a.ok(same(gunzip(r), BIG), "and the result is right");
});

t.test("parallel offloads all complete with the right bytes", async (a) => {
    const ins = [0, 1, 2, 3, 4, 5].map((k) => text((1 << 20) + k * 977));
    const outs = await Promise.all(ins.map((u) => offload(gzip, u)));
    outs.forEach((r, k) => a.ok(same(gunzip(r), ins[k]), `job ${k}`));
});

t.test("the input is copied at the call: changing it afterwards does not change the result", async (a) => {
    const u = text(1 << 20);
    const want = gzip(u);
    const p = offload(gzip, u);
    u.fill(0);
    a.ok(same(await p, want), "result is of the bytes at call time");
});

t.test("errors: a non-native fn throws; the native's own argument errors reject", async (a) => {
    a.throws(() => offload((x) => x, 1), TypeError, "a JS function is refused");
    a.throws(() => offload(gzip.bind(null), BIG), TypeError, "a bound function is refused");
    a.throws(() => offload(42), TypeError, "a non-function is refused");
    a.throws(() => offload(), TypeError, "no arguments is refused");
    await a.rejects(offload(gzip, BIG, { level: "x" }), TypeError, "gzip's TypeError rejects the promise");
    await a.rejects(offload(gzip, BIG, { nope: 1 }), TypeError, "an unknown option rejects the promise");
});

t.test("a native error after an awaited offload builds a sound backtrace", async (a) => {
    for (let i = 0; i < 3; i++) {
        await offload(gzip, text(64));
        const e = a.throws(() => offload((x) => x, 1), TypeError, `round ${i}: refused`);
        a.ok(typeof (e && e.stack) === "string", `round ${i}: the error has a stack`);
        const e2 = a.throws(() => new Uint8Array(-1), RangeError, `round ${i}: an engine error after it`);
        a.ok(typeof (e2 && e2.stack) === "string", `round ${i}: with a stack`);
    }
});

t.test("an existing fiber API keeps its caller in the backtrace of an error raised before its first offload", async (a) => {
    const e = await a.rejects(stats.bootstrap([1, 2, 3], { bad: 1 }), TypeError, "unknown option rejects");
    a.match(e && e.stack, /test_async_offload\.js/, "the stack names this file");
});

await t.run();
