// tests/lib/offload_check.js -- shared checks for natives marked offloadable (dyna:async offload, owner ask E).
// For one marked function and its arguments:
//   same(a, label, fn, args, eq)  the direct call and the offloaded call give equal results (eq defaults to byte/strict equality)
//   loopTicks(a, label, fn, args) the work left the loop thread: the promise is still pending when offload() returns.
//                                 Mechanism, not timing: run inline (the native's DYN_OFFLOAD_* mark removed, or
//                                 --io-threads 0) the fiber settles the promise before offload() returns, so
//                                 Promise.race([p, resolved]) picks p; sent to the pool, p can only settle after the
//                                 loop runs the job's completion, which cannot happen inside the current microtask
//                                 checkpoint, so the race picks the marker. Holds at any input size and any load (a
//                                 0 ms timer raced against the result failed at load 34: lz4Decompress won).
//   settledInline(fn, args)       true when the promise was already settled as offload() returned.
import { offload } from "dyna:async";
import { TimingSafeEqual } from "dyna:crypto";

const PENDING = Symbol("pending");

export function equalValue(x, y) {
    if (x instanceof Uint8Array || y instanceof Uint8Array) {
        if (!(x instanceof Uint8Array && y instanceof Uint8Array) || x.length !== y.length) return false;
        return x.length === 0 || TimingSafeEqual(x, y);
    }
    return x === y;
}

export function text(n, seed = 0) {
    const u = new Uint8Array(n);
    const enc = new TextEncoder();
    for (let i = 0; i < n; i += 4096) {
        const row = enc.encode(("row " + (i + seed) + " lorem ipsum dolor sit amet " + (i % 977) + " ").repeat(8));
        u.set(row.subarray(0, Math.min(200, n - i)), i);
    }
    return u;
}

export async function same(a, label, fn, args, eq = equalValue) {
    const direct = await fn(...args);
    const off = await offload(fn, ...args);
    a.ok(eq(direct, off), `${label}: offload result equals the direct call`);
    return off;
}

export async function settledInline(fn, args) {
    const p = offload(fn, ...args);
    const first = await Promise.race([p, Promise.resolve(PENDING)]);
    await p;
    return first !== PENDING;
}

export async function loopTicks(a, label, fn, args) {
    a.eq(await settledInline(fn, args), false, `${label}: still pending when offload() returned (the work left the loop thread)`);
}
