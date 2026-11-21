// flags: --std
// covers: c:src/modules/libc/libc-event-loop.c
// v10b-28: timers and performance.now() read one clock (macOS: mach_continuous_time, which keeps counting across sleep like the
// CLOCK_MONOTONIC it replaces, at a quarter of the cost); a timer due by an earlier reading of that clock fires without reading it again,
// while every arming site (setTimeout, setInterval re-arm, sleep()) reads it fresh. So no timer fires before its delay as measured
// by performance.now(), even when it is armed after busy JS in the same turn (a stale per-turn clock at arming would fire it early).
import { suite } from "../lib/kit.js";
import * as os from "os";

const t = suite("timer_clock");
const spin = (ms) => { const e = performance.now() + ms; while (performance.now() < e) {} };

t.test("120 timers of 1..13 ms never fire before their delay (performance.now)", async (a) => {
    const ds = [1, 2, 3, 5, 8, 13];
    let early = [], left = 0;
    await new Promise((res) => {
        for (let r = 0; r < 20; r++) for (const d of ds) {
            left++;
            const t0 = performance.now();
            setTimeout(() => { const el = performance.now() - t0; if (el < d) early.push(`${d}:${el.toFixed(3)}`); if (--left === 0) res(); }, d);
        }
    });
    a.eq(early.join(" "), "");
});

t.test("timers armed after busy JS in the same turn still wait their full delay", async (a) => {
    let early = [];
    for (let r = 0; r < 8; r++) {
        await new Promise((res) => {
            spin(4);
            const t0 = performance.now();
            setTimeout(() => { const el = performance.now() - t0; if (el < 3) early.push(el.toFixed(3)); res(); }, 3);
            spin(2);
        });
    }
    a.eq(early.join(" "), "");
});

t.test("global sleep() armed after busy JS waits its full delay", async (a) => {
    let early = [];
    for (let r = 0; r < 6; r++) {
        spin(3);
        const t0 = performance.now();
        await sleep(4);
        const el = performance.now() - t0;
        if (el < 4) early.push(el.toFixed(3));
    }
    a.eq(early.join(" "), "");
});

t.test("setInterval re-arms from a fresh reading: consecutive runs are >= period - 1 ms apart", async (a) => {
    const at = [];
    await new Promise((res) => {
        const iv = setInterval(() => { at.push(performance.now()); spin(1); if (at.length === 12) { clearInterval(iv); res(); } }, 4);
    });
    let bad = [];
    for (let i = 1; i < at.length; i++) if (at[i] - at[i - 1] < 3) bad.push((at[i] - at[i - 1]).toFixed(3));
    a.eq(bad.join(" "), "");
});

t.test("setInterval overdue behind a timer armed earlier in the turn still re-arms from a fresh reading", async (a) => {
    const at = [];
    await new Promise((res) => {
        const iv = setInterval(() => {
            at.push(performance.now());
            if (at.length === 12) { clearInterval(iv); res(); return; }
            if (at.length % 2 === 0) return;
            spin(5);
            setTimeout(() => {}, 0);
            spin(3);
        }, 4);
    });
    let bad = [];
    for (let i = 2; i < at.length; i += 2) if (at[i] - at[i - 1] < 3) bad.push((at[i] - at[i - 1]).toFixed(3));
    a.eq(bad.join(" "), "", "gap after each quiet run (its re-arm read the clock when it fired)");
});

t.test("timer firing order: due timers by (deadline, arming order); setTimeout(0) chains interleave with due timers", async (a) => {
    const order = [];
    await new Promise((res) => {
        setTimeout(() => order.push("a5"), 5);
        setTimeout(() => order.push("b0"), 0);
        setTimeout(() => order.push("c5"), 5);
        setTimeout(() => { order.push("d0"); setTimeout(() => order.push("e0"), 0); }, 0);
        setTimeout(() => { order.push("f9"); res(); }, 9);
    });
    a.eq(order.join(","), "b0,d0,e0,a5,c5,f9");
});

t.test("performance.now and os.now read the same clock", (a) => {
    const p = performance.now(), o = os.now(), p2 = performance.now();
    a.ok(o >= p && o <= p2, `os.now ${o} within [${p}, ${p2}]`);
});
t.test("performance.now advances at the wall-clock rate (timebase scaling)", async (a) => {
    const p0 = performance.now(), d0 = Date.now();
    await sleep(120);
    // wall-clock: a ratio of two clocks over the same interval, not a duration budget; load stretches both equally
    const r = (performance.now() - p0) / (Date.now() - d0);
    a.ok(r > 0.8 && r < 1.25, `performance.now / Date.now rate ${r.toFixed(3)}`);
});
await t.run();
