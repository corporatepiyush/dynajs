/* bench_log_ab.js -- the dyna:log emit path, A/B'd old build vs new build.
 *
 * CLAUDE.md sec.2: an optimisation is guilty until a benchmark proves it
 * innocent, and THE SUITE IS THE VERDICT -- a micro-benchmark is only a
 * hypothesis. This is the hypothesis side, shaped so two builds can be
 * diffed line for line:
 *
 *   /tmp/dynajs.old tests/bench_log_ab.js > /tmp/ab.old
 *   /tmp/dynajs.new tests/bench_log_ab.js > /tmp/ab.new
 *   paste /tmp/ab.old /tmp/ab.new | column -t
 *
 * It is pinned to the PRE-redesign API surface (no dest/format/rollover/
 * pid/hostname) so the SAME file runs on both binaries -- a benchmark the
 * old build cannot execute cannot be an A/B. The clock is Date.now on both
 * sides: mixing clocks would make the delta a measurement of the clock.
 */
import { Logger } from "dyna:log";

const TARGET_MS = 140;
let sink = 0;

function measure(fn) {
    let r = 1, ms = 0;
    for (;;) {
        const t0 = Date.now();
        for (let i = 0; i < r; i++) sink += (fn(), 1);
        ms = Date.now() - t0;
        if (ms >= TARGET_MS || r >= (1 << 24)) break;
        r = Math.max(r * 2, Math.ceil(r * (TARGET_MS / Math.max(ms, 0.01))));
    }
    let best = ms;
    for (let k = 0; k < 3; k++) {
        const t0 = Date.now();
        for (let i = 0; i < r; i++) sink += (fn(), 1);
        const p = Date.now() - t0;
        if (p < best) best = p;
    }
    return (best / r) * 1e6;
}

/* fixtures built ONCE: nothing but the emit is in the timed region */
const SHORT = "request completed";
const LONG = "request completed for user=" + "9".repeat(400);
const SMALL = { userId: 42, ok: true, ms: 3.5 };
const FLOATS = { price: 12.34, ratio: 0.001, big: 1e21 };
const BIG = (() => {
    const o = {};
    for (let i = 0; i < 40; i++) o["field_number_" + i] = "value-" + i;
    return o;
})();
const E = new Error("boom");
E.code = "ENOENT";

const log = new Logger({ level: "trace", timestamp: false, name: "bench" });
const gated = new Logger({ level: "silent", timestamp: false, name: "bench" });

const rows = [
    ["control", "empty-loop", () => {}],
    ["control", "silent-gate", () => gated.info(SHORT)],
    ["emit", "json-short-msg", () => log.info(SHORT)],
    ["emit", "json-long-msg", () => log.info(LONG)],
    ["emit", "json-small-fields", () => log.info(SMALL, SHORT)],
    ["emit", "json-float-fields", () => log.info(FLOATS, SHORT)],
    ["emit", "json-40-fields", () => log.info(BIG, SHORT)],
    ["emit", "json-err-fields-msg", () => log.error(E, SMALL, SHORT)],
    ["alloc", "inline-100b", () => log.info("x".repeat(100))],
    ["alloc", "heap-4k", () => log.info("x".repeat(4000))],
];

console.log("case\tdetail\tns");
for (const row of rows) {
    const ns = measure(row[2]);
    console.log(row[0] + "\t" + row[1] + "\t" + ns.toFixed(1));
}
if (sink === 0)
    throw new Error("bench_log_ab: sink never written -- the loop was elided");
