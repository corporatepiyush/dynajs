// flags: --std
// covers: integration c:src/modules/libc/libc-os.c c:src/modules/libc/libc-event-loop.c c:src/dyna-evloop.c -- microsecond deadlines span the timer list, the reactor kqueue and the poll path
// Timer deadlines are kept in microseconds and both waits are microsecond-precise, so a timer fires as
// soon as its full delay has passed. They used to be whole milliseconds with a +1 ms guard (armed at
// X.9 ms, setTimeout(1) was due at X+2) and the loop slept in whole milliseconds: a setTimeout(1) chain
// took ~2.5 ms per hop. A plain microsecond kevent timeout is not enough on macOS: the kernel coalesces
// it (node, bun and a bare C kevent loop all measure 1.26 ms for 1, 6.27 for 5, 12.0 for 10), so the
// wait arms an EVFILT_TIMER with NOTE_USECONDS|NOTE_CRITICAL for timeouts under 1 s: on the reactor's own
// kqueue (dyna-evloop.c, net in use) and on a per-thread deadline kqueue polled next to the loop's fds
// (libc-event-loop.c, plain poll path); Linux uses epoll_pwait2 / ppoll. Both paths are pinned: the
// second half repeats the chains with a TCPServer started, which moves the wait into the reactor.
// The guard's job, never firing early, is pinned here too (and by test_timer_min_delay.js and
// test_timer_clock.js): every hop is >= its delay on the monotonic clock.
// Medians, not single samples, carry the speed claims, and each chain gets up to three tries (the best
// median counts): load on a busy host only ever adds delay, while the coalescing the fix removes is
// deterministic, so a regression fails every try. Precise medians are ~1.02 ms for 1 and d + 0.05 for
// larger d; the bound is d * 1.04 + 0.2 (1.2 for 1 ms), under the coalesced 1.26 / 2.51 / 3.76 / 6.27 / 12.0.
// Under DYNA_COMPAT_ONLY=1 (the docker legs: CPU-shared VMs) only the never-early checks run; the speed bounds print SKIP.
// Mutants (red): the +1 ms guard put back in setTimeout; NOTE_CRITICAL dropped in the reactor wait;
// the deadline kqueue left out of the poll set.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_timer_us_deadline", { chain: true });
import { monotonicNano } from "dyna:time";
import { TCPServer } from "dyna:net";
import * as std from "std";

let checks = 0, fails = 0;
const COMPAT = std.getenv("DYNA_COMPAT_ONLY") === "1";
const fast = (c, m) => { if (COMPAT) { print("SKIP (DYNA_COMPAT_ONLY: timing is not asserted in a VM): " + m); return; } ok(c, m); };
function ok(c, m) { checks++; if (!c) { fails++; if (fails < 10) print("FAIL: " + m); } }
const median = (a) => { const s = a.slice().sort((x, y) => x - y); return s[s.length >> 1]; };

async function chain(arm, delay, n) {
    const hops = [];
    let prev = monotonicNano();
    for (let i = 0; i < n; i++) {
        await arm(delay);
        const now = monotonicNano();
        hops.push(Number(now - prev) / 1e6);
        prev = now;
    }
    return hops;
}
async function best(arm, d, n, bound) {
    let all = [], bestMed = Infinity;
    for (let t = 0; t < 3 && bestMed >= bound; t++) {
        const hops = await chain(arm, d, n);
        all = all.concat(hops);
        bestMed = Math.min(bestMed, median(hops));
    }
    return { all, med: bestMed };
}
const bound = (d) => d === 1 ? 1.2 : d * 1.04 + 0.2;
const viaTimeout = (d) => new Promise((res) => setTimeout(res, d));
const viaSleep = (d) => sleep(d);

async function chains(tag) {
    await __kitS.section(tag + ": setTimeout(1) chain: never early, median hop under 1.2 ms", async () => {
        const { all, med } = await best(viaTimeout, 1, 120, bound(1));
        const early = all.filter((h) => h < 1).length;
        ok(early === 0, `${tag} setTimeout(1): ${early} of ${all.length} hops shorter than 1 ms (min ${Math.min(...all).toFixed(4)} ms)`);
        fast(med < bound(1), `${tag} setTimeout(1) best median hop ${med.toFixed(3)} ms, want < ${bound(1)}`);
    });
    await __kitS.section(tag + ": setTimeout(d) for d in 2, 3, 5, 10: never early, median under d * 1.04 + 0.2", async () => {
        for (const [d, n] of [[2, 40], [3, 30], [5, 20], [10, 12]]) {
            const { all, med } = await best(viaTimeout, d, n, bound(d));
            const early = all.filter((h) => h < d).length;
            ok(early === 0, `${tag} setTimeout(${d}): ${early} of ${all.length} hops early (min ${Math.min(...all).toFixed(4)})`);
            fast(med < bound(d), `${tag} setTimeout(${d}) best median ${med.toFixed(3)} ms, want < ${bound(d).toFixed(2)}`);
        }
    });
    await __kitS.section(tag + ": global sleep(1) chain: never early, median under 1.2 ms", async () => {
        const { all, med } = await best(viaSleep, 1, 80, bound(1));
        ok(all.every((h) => h >= 1), `${tag} sleep(1): min hop ${Math.min(...all).toFixed(4)} ms`);
        fast(med < bound(1), `${tag} sleep(1) best median ${med.toFixed(3)} ms, want < ${bound(1)}`);
    });
}

await chains("poll path");
{
    const srv = new TCPServer({ port: 0 });
    srv.start();
    await chains("reactor path");
    srv.close();
}

await __kitS.section("setInterval(2): consecutive runs at least 2 ms apart, median under 2.8 ms", async () => {
    const gaps = await new Promise((res) => {
        const out = [];
        let prev = monotonicNano();
        const id = setInterval(() => {
            const now = monotonicNano();
            out.push(Number(now - prev) / 1e6);
            prev = now;
            if (out.length === 40) { clearInterval(id); res(out); }
        }, 2);
    });
    ok(gaps.every((g) => g >= 2), `setInterval(2): min gap ${Math.min(...gaps).toFixed(4)} ms`);
    ok(median(gaps) < 2.8, `setInterval(2) median ${median(gaps).toFixed(3)} ms, want < 2.8`);
});

await __kitS.section("ordering: shorter delays first, equal delays in arming order, 0 before 1", async () => {
    for (let round = 0; round < 20; round++) {
        const seen = [];
        await new Promise((res) => {
            setTimeout(() => seen.push("a2"), 2);
            setTimeout(() => seen.push("b1"), 1);
            setTimeout(() => seen.push("c1"), 1);
            setTimeout(() => seen.push("d0"), 0);
            setTimeout(() => seen.push("e3"), 3);
            setTimeout(() => { seen.push("f1"); }, 1);
            setTimeout(() => res(), 6);
        });
        const want = "d0,b1,c1,f1,a2,e3";
        ok(seen.join() === want, `round ${round}: ${seen.join()} want ${want}`);
    }
});

await __kitS.section("a timer armed later with a shorter delay overtakes; clearTimeout still cancels", async () => {
    const seen = [];
    await new Promise((res) => {
        setTimeout(() => seen.push("late4"), 4);
        const t0 = monotonicNano();
        while (monotonicNano() - t0 < 1500000n) {}
        setTimeout(() => seen.push("early1"), 1);
        const x = setTimeout(() => seen.push("cancelled"), 1);
        clearTimeout(x);
        setTimeout(res, 8);
    });
    ok(seen.join() === "early1,late4", "overtake/cancel: " + seen.join());
});

__kitS.end();
print(`test_timer_us_deadline: ${checks - fails} of ${checks} checks passed`);
if (fails) throw new Error(fails + " failures");
