// CPU-time budgets for tests that assert "this is fast" or "refused before any
// work". Wall clock measures the machine's load as much as the code: under a
// parallel gate a 5 ms refusal can take 600 ms of wall time and flake. CPU time
// of this process (getrusage via dyna:sys cpuUsage, helper threads included)
// does not grow while the process waits for a core, so a budget on it only
// trips when the code itself does the work.
//
// Slim and one-module binaries do not link dyna:sys; there the clock falls back
// to the monotonic wall clock and bestMs's best-of-N is what absorbs the load.
// Budgets for timers, deadlines and I/O stay on the wall clock (that is what
// they measure) and use retry-until-quiet (wallWithin) instead.
//
// mark()/spent() serve probes that may block rather than spin (fuzzers,
// pentest probes over file and net calls): spent() is the larger of the CPU
// time and the wall time divided by WALL_SLACK, so a call that sleeps or waits
// on I/O for ten budgets still trips, while load on the machine alone does not.
let usage = null;
try {
    const m = await import("dyna:sys");
    if (typeof m.cpuUsage === "function") usage = m.cpuUsage;
} catch (e) {
    usage = null;
}
export const clockKind = usage ? "cpu" : "wall";
export function cpuNow() {
    if (usage) {
        const u = usage();
        return (u.user + u.system) * 1000;
    }
    return performance.now();
}
export function cpuMs(fn) {
    const t = cpuNow();
    const value = fn();
    return { ms: cpuNow() - t, value };
}
export const WALL_SLACK = 10;
export function mark() {
    return { c: cpuNow(), w: performance.now() };
}
export function spent(m) {
    return Math.max(cpuNow() - m.c, (performance.now() - m.w) / WALL_SLACK);
}
export function bestMs(fn, n = 3) {
    let best = Infinity, value;
    for (let i = 0; i < n; i++) {
        const r = cpuMs(fn);
        if (r.ms < best) best = r.ms;
        value = r.value;
    }
    return { ms: best, value };
}
export async function wallWithin(attempt, limitMs, tries = 3) {
    let last = null;
    for (let i = 0; i < tries; i++) {
        last = await attempt();
        if (last.ms < limitMs) return last;
    }
    return last;
}
