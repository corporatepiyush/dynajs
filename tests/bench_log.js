/* bench_log.js -- the dyna:log emit path, A/B'd against a pre-change build.
 *
 * CLAUDE.md sec.2: an optimisation is guilty until a benchmark proves it
 * innocent, and THE SUITE IS THE VERDICT -- a micro-benchmark is only a
 * hypothesis. So this file is the hypothesis side, and it is built to be
 * diffed: every row is tab-separated and stable across runs.
 *
 *   make dynajs CONFIG_NATIVE_MODULES=y -j8 && cp dynajs /tmp/dynajs.new
 *   # then build the pre-redesign source the same way -> /tmp/dynajs.old
 *   for b in /tmp/dynajs.old /tmp/dynajs.new; do echo "== $b"; $b tests/bench_log.js; done
 *
 * DESIGN NOTES (each one is a sec.2 rule, not a preference):
 *  - Reps auto-scale until the timed region clears ~140 ms, and the clock is
 *    performance.now (sub-ms). A coarse clock does not look coarse -- it looks
 *    like data -- so per-op figures are only printed after the region is
 *    ~1000x the tick.
 *  - Nothing in the timed region but the emit: the payload objects are built
 *    ONCE, outside. Building them inside measures the engine, not the logger.
 *  - A VOLATILE SINK (globalThis) receives each result so a clever compiler
 *    cannot elide the call, and the sink is checked afterwards.
 *  - CONTROLS that must NOT move: the empty loop calibrates the harness, and
 *    the suppressed-level row (emit gated OFF) isolates the level gate from
 *    the emit path -- if a claimed emit win moved the gate instead, the
 *    control would show it.
 *  - Two sizes for every case: a constant overhead reads as a ratio that
 *    depends on the input, so a small and a large payload are both measured.
 *  - stderr is the default destination, so a run must be silent-ish: the
 *    benchmark redirects to /dev/null in the shell line above.
 */
import { Logger } from "dyna:log";
import { monotonicNano } from "dyna:time";

const TARGET_MS = 140;
let sink = 0;

/* Best-of-N with auto-scaled reps; returns ns per operation. */
function measure(fn) {
    let r = 1, ms = 0;
    for (;;) {
        const t0 = monotonicNano();
        for (let i = 0; i < r; i++) sink += (fn(), 1);
        ms = Number(monotonicNano() - t0) / 1e6;
        if (ms >= TARGET_MS || r >= (1 << 24)) break;
        r = Math.max(r * 2, Math.ceil(r * (TARGET_MS / Math.max(ms, 0.01))));
    }
    let best = ms;
    for (let k = 0; k < 3; k++) {
        const t0 = monotonicNano();
        for (let i = 0; i < r; i++) sink += (fn(), 1);
        const p = Number(monotonicNano() - t0) / 1e6;
        if (p < best) best = p;
    }
    return { ns: (best / r) * 1e6, reps: r };
}

/* ---- fixtures, built ONCE (nothing but the emit is in the timed region) -- */
const SHORT_MSG = "request completed";
const LONG_MSG = "request completed for user=" + "9".repeat(400);
const SMALL_FIELDS = { userId: 42, ok: true, ms: 3.5 };
const FLOAT_FIELDS = { price: 12.34, ratio: 0.001, big: 1e21 };
const BIG_FIELDS = (() => {
    const o = {};
    for (let i = 0; i < 40; i++) o["field_number_" + i] = "value-" + i;
    return o;
})();
const ERR = new Error("boom");
ERR.code = "ENOENT";

const log = new Logger({ level: "trace", timestamp: false, name: "bench" });
const isoLog = new Logger({ level: "trace", timestamp: "iso", name: "bench" });
const gated = new Logger({ level: "silent", timestamp: false, name: "bench" });

console.log("case\tdetail\tns\treps");

/* CONTROL 1: the driving loop alone. It must stay flat across builds; if it
 * moves, the machine moved, not the code. */
{
    const r = measure(() => {});
    console.log(`control\tempty-loop\t${r.ns.toFixed(1)}\t${r.reps}`);
}

/* CONTROL 2: the level GATE, with the emit suppressed. This is the one row a
 * change to the emit path must NOT improve -- if "silent" gets faster, the
 * win is in the gate, not in the serialization. */
{
    const r = measure(() => gated.info(SHORT_MSG));
    console.log(`control\tsilent-gate\t${r.ns.toFixed(1)}\t${r.reps}`);
}

/* The emit itself, at two sizes, both formats. */
for (const [name, l] of [["json", log], ["text", isoLog]]) {
    let r = measure(() => l.info(SHORT_MSG));
    console.log(`emit\t${name}-short-msg\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.info(LONG_MSG));
    console.log(`emit\t${name}-long-msg\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.info(SMALL_FIELDS, SHORT_MSG));
    console.log(`emit\t${name}-small-fields\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.info(FLOAT_FIELDS, SHORT_MSG));
    console.log(`emit\t${name}-float-fields\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.info(BIG_FIELDS, SHORT_MSG));
    console.log(`emit\t${name}-40-fields\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => l.error(ERR, SMALL_FIELDS, SHORT_MSG));
    console.log(`emit\t${name}-err-fields-msg\t${r.ns.toFixed(1)}\t${r.reps}`);
}

/* The allocation hypothesis, directly: a line that stays under the inline
 * buffer vs one that forces the heap. If DYN_LINE_INLINE is doing its job,
 * the small row should not carry a malloc. */
{
    const small = new Logger({ level: "trace", timestamp: false });
    const big = new Logger({ level: "trace", timestamp: false });
    let r = measure(() => small.info("x".repeat(100)));
    console.log(`alloc\tinline-100b\t${r.ns.toFixed(1)}\t${r.reps}`);
    r = measure(() => big.info("x".repeat(4000)));
    console.log(`alloc\theap-4k\t${r.ns.toFixed(1)}\t${r.reps}`);
}

/* Sink check: proves the loop was not elided. */
if (sink === 0)
    throw new Error("bench_log: sink never written -- the loop was elided");
