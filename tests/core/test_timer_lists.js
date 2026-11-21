// flags: --std
// covers: integration c:src/modules/libc/libc-event-loop.c c:src/modules/libc/libc-os.c -- timer lists, no live-timer cap
// net-2 (2026-11-21): deadlines are microseconds with no +1 ms guard, so a setTimeout(d) shares the offset-d list with a
// setInterval(d) and the two fire in creation order, as HTML requires for equal timeouts and as node and bun do; this file
// used to pin the guard's artifact (the interval made after the timeout fired first, "IT"). Due times are now read in microseconds
// (os.now() around each setTimeout): two timers due in the same millisecond fire by their microsecond due time, so the old
// same-millisecond tie rule (lower sequence first) no longer holds and was dropped; it failed under ASan, where creating the
// timers crosses millisecond boundaries.
// v10b-25. Timers live in one FIFO list per offset (the delay of a setTimeout or global sleep; an interval's period), and a binary heap orders the lists by their head's (due time, sequence). Every timer in a list was scheduled at a
// non-decreasing monotonic time with the same offset, so each list is already sorted and insert, cancel and fire are O(1)
// apart from the heap of lists; the firing order is exactly the old single heap's (due time, then scheduling sequence).
// An interval is re-appended to its own list with a fresh sequence. There is no cap on live timers (HTML timers have none).
// v10b-30: the burst and distinct-delay cases check the order against each timer's real due time (its creation ms read around the
// setTimeout call, plus delay), not "by delay": under a slow (sanitizer) build creating the timers spans milliseconds, so
// a shorter delay created later is legitimately due after a longer one created earlier (seen as 1 run in 3 under ASan).
import { suite } from "../lib/kit.js";
import * as os from "os";

const t = suite("timer_lists");
let seed = 12345;
const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
const arm = (d, fn) => { const lo = os.now(); const id = setTimeout(fn, d); const hi = os.now(); return { id, lo: lo + d, hi: hi + d }; };
const misordered = (fired, due) => { for (let k = 1; k < fired.length; k++) { const x = due.get(fired[k - 1]), y = due.get(fired[k]); if (y.hi < x.lo) return fired[k - 1] + " fired before " + fired[k]; } return ""; };

t.test("burst of timers fires by delay then creation order; cleared ones never fire", async (a) => {
    const fired = [], want = [], ids = [], due = new Map();
    const delays = [0, 15, 30, 45, 60];
    for (let i = 0; i < 4000; i++) {
        const d = delays[rnd(delays.length)];
        const r = arm(d, () => fired.push(i));
        due.set(i, { lo: r.lo, hi: r.hi, seq: i });
        ids.push({ i, d, id: r.id });
    }
    const cleared = new Set();
    for (let k = 0; k < 900; k++) { const e = ids[rnd(ids.length)]; clearTimeout(e.id); cleared.add(e.i); }
    for (const d of delays) for (const e of ids) if (e.d === d && !cleared.has(e.i)) want.push(e.i);
    await new Promise((r) => setTimeout(r, 120));
    a.eq(fired.length, want.length);
    a.eq([...fired].sort((x, y) => x - y).join(), [...want].sort((x, y) => x - y).join(), "exactly the uncleared timers fired");
    for (const d of delays) a.eq(fired.filter((i) => ids[i].d === d).join(), want.filter((i) => ids[i].d === d).join(), "delay " + d + " fired in creation order");
    a.eq(misordered(fired, due), "", "every timer fired by (due time, creation order)");
});

t.test("same-period intervals keep round-robin order; clearInterval from a callback", async (a) => {
    const log = [];
    const n = { A: 0, B: 0, C: 0 };
    const h = {};
    for (const k of ["A", "B", "C"]) h[k] = setInterval(() => { log.push(k); if (++n[k] === 4) clearInterval(h[k]); if (k === "B" && n.B === 2) clearInterval(h.C); }, 5);
    await new Promise((r) => setTimeout(r, 80));
    a.eq(log.join(""), "ABCABABAB");
});

t.test("clearing a list head and re-adding keeps order; the global sleep shares the setTimeout list", async (a) => {
    const log = [];
    const x = setTimeout(() => log.push("x"), 10);
    setTimeout(() => log.push("y"), 10);
    const s = sleep(10).then(() => log.push("s"));
    setTimeout(() => log.push("z"), 10);
    clearTimeout(x);
    setTimeout(() => log.push("w"), 10);
    setTimeout(() => log.push("zero"), 0);
    await s;
    await new Promise((r) => setTimeout(r, 30));
    a.eq(log.join(), "zero,y,s,z,w");
});

t.test("nested scheduling from callbacks and mixed delays", async (a) => {
    const log = [];
    setTimeout(() => { log.push("a"); setTimeout(() => log.push("a2"), 0); setTimeout(() => log.push("a3"), 0); }, 0);
    setTimeout(() => log.push("b"), 0);
    setTimeout(() => { log.push("c"); setTimeout(() => log.push("c2"), 0); }, 0);
    await new Promise((r) => setTimeout(r, 20));
    a.eq(log.join(), "a,b,c,a2,a3,c2");
});

t.test("more than 100000 live timers are accepted and all fire", async (a) => {
    const N = 150000;
    let left = N;
    const done = new Promise((r) => { for (let i = 0; i < N; i++) setTimeout(() => { if (--left === 0) r(); }, 1 + (i % 7)); });
    await done;
    a.eq(left, 0);
    const ids = [];
    for (let i = 0; i < 50000; i++) ids.push(setTimeout(() => { throw new Error("cleared timer fired"); }, 5 + (i % 3)));
    for (const id of ids) clearTimeout(id);
    await new Promise((r) => setTimeout(r, 20));
    a.eq(os.loopStats().timers > 0, true);
});

t.test("many distinct delays: lists are freed past the idle cache and found again", async (a) => {
    for (let round = 0; round < 3; round++) {
        const fired = [], due = new Map();
        for (let d = 0; d < 100; d++) { const r = arm(99 - d, () => fired.push(d)); due.set(d, { lo: r.lo, hi: r.hi, seq: d }); }
        await new Promise((r) => setTimeout(r, 140));
        a.eq(fired.length, 100);
        a.eq(misordered(fired, due), "", "every timer fired by (due time, creation order)");
    }
});

t.test("a re-armed interval yields to an earlier timer of another list", async (a) => {
    const log = [];
    setTimeout(() => log.push("T"), 13);
    const h = setInterval(() => { log.push("I"); if (log.filter((x) => x === "I").length === 3) clearInterval(h); }, 10);
    await new Promise((r) => setTimeout(r, 60));
    a.eq(log.join(""), "ITII");
});

t.test("setTimeout(d) and setInterval(d) made in the same millisecond fire in creation order (HTML: equal timeouts run in the order they started)", async (a) => {
    let checked = 0;
    for (let k = 0; k < 20 && checked < 5; k++) {
        const log = [];
        const t0 = os.now();
        setTimeout(() => log.push("T"), 10);
        const h = setInterval(() => { log.push("I"); clearInterval(h); }, 10);
        const t1 = os.now();
        await new Promise((r) => setTimeout(r, 25));
        if (Math.floor(t0) !== Math.floor(t1)) continue;
        a.eq(log.join(""), "TI");
        checked++;
    }
    a.eq(checked, 5);
});

t.test("clearing a list head re-keys the list behind another list", async (a) => {
    const log = [];
    const a1 = setTimeout(() => log.push("A1"), 30);
    setTimeout(() => {
        setTimeout(() => log.push("A2"), 30);
        setTimeout(() => log.push("B"), 27);
        clearTimeout(a1);
    }, 4);
    await new Promise((r) => setTimeout(r, 70));
    a.eq(log.join(), "B,A2");
});

await t.run();
