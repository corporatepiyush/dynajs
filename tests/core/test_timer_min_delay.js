// flags: --std
// covers: c:src/modules/libc/libc-os.c
// A positive setTimeout delay is a minimum (dynajs.d.ts setTimeout): the timer clock is in whole milliseconds, so arming at X.9 ms used to fire at X+delay, up to 1 ms early; bb_time's "80ms doze advanced monotonic >= 80ms" failed that way under gate load (g57, 11-19). The arm now rounds up.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_timer_min_delay", { chain: true });
import { monotonicNano } from "dyna:time";
import * as os from "os";

let checks = 0, fails = 0;
function ok(c, m) { checks++; if (!c) { fails++; if (fails < 10) print("FAIL: " + m); } }

async function measure(arm, delay, n) {
    let early = 0, worst = 0n;
    for (let i = 0; i < n; i++) {
        const t0 = monotonicNano();
        await arm(delay);
        const dt = monotonicNano() - t0;
        if (dt < BigInt(delay) * 1000000n) { early++; if (BigInt(delay) * 1000000n - dt > worst) worst = BigInt(delay) * 1000000n - dt; }
    }
    return { early, worst };
}

await __kitS.section("setTimeout never fires before its delay", async () => {
    for (const [delay, n] of [[1, 300], [2, 60], [3, 40], [5, 20], [17, 6]]) {
        const r = await measure((d) => new Promise((res) => setTimeout(res, d)), delay, n);
        ok(r.early === 0, `setTimeout(${delay}): ${r.early} of ${n} fired early (worst by ${r.worst} ns)`);
    }
});

await __kitS.section("armed in the last tenth of a millisecond and the loop woken just past the boundary, setTimeout(1) still waits a full millisecond", async () => {
    let early = 0, worst = 0n;
    const n = 50;
    for (let i = 0; i < n; i++) {
        let t0 = os.now();
        while (t0 % 1 < 0.9) t0 = os.now();
        const fired = new Promise((res) => setTimeout(res, 1));
        const edge = Math.floor(t0) + 1.05;
        while (os.now() < edge) {}
        await fired;
        const dt = os.now() - t0;
        if (dt < 1) { early++; const by = BigInt(Math.round((1 - dt) * 1e6)); if (by > worst) worst = by; }
    }
    ok(early === 0, `late-phase setTimeout(1): ${early} of ${n} fired early (worst by ${worst} ns)`);
});

await __kitS.section("setTimeout extra arguments and zero delay still work", async () => {
    const got = await new Promise((res) => setTimeout((a, b) => res(a + b), 1, 2, 3));
    ok(got === 5, "extra args passed through: " + got);
    const t0 = monotonicNano();
    await new Promise((res) => setTimeout(res, 0));
    ok(monotonicNano() - t0 < 1000000000n, "setTimeout(0) runs promptly");
});

__kitS.end();
print(`test_timer_min_delay: ${checks - fails} of ${checks} checks passed`);
if (fails) throw new Error(fails + " failures");
