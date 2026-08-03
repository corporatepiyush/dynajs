/* bench_time_layout.js -- the W10.3 gate (STDLIB_OOP_PLAN section 11).
 *
 * The plan proposes `new Format(layout)` as a compiled capability: parse the
 * layout once, format many times. Its own rule is to VERIFY THE PER-CALL COST
 * FIRST -- "if the layout walk is already trivial this is a verdict-ledger
 * null, not a win" -- so this measures the walk before anything is built.
 *
 * The measurement is a decomposition, not a timing of the whole call:
 *
 *   formatRFC3339  a FIXED layout: no walk, no layout string to coerce. This
 *                  is the floor -- everything a Format instance could not
 *                  avoid either.
 *   formatUnix     the same output through the general layout walk.
 *   short vs long  the same output built from a 20-char and a 200-char layout.
 *                  If the walk dominated, cost would scale with layout length.
 *
 * `Format` can only ever remove (formatUnix - formatRFC3339), and only the part
 * of that which is the walk rather than the argument coercion. If that
 * difference is a small fraction of the call, the capability cannot pay and the
 * honest outcome is a null.
 *
 * Emits `#DATA<TAB>case<TAB>ns_per_call`.
 */
import { formatUnix, formatRFC3339 } from "dyna:time";

const TRIALS = 7;
const N = 200000;

function best(fn) {
    let b = Infinity;
    for (let t = 0; t < TRIALS; t++) {
        const t0 = performance.now();
        fn();
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    return b * 1e6 / N;
}

/* The driving loop itself costs 4-8 ns/iteration; calibrated and subtracted. */
const FLOOR = (() => {
    let b = Infinity;
    for (let t = 0; t < TRIALS; t++) {
        const t0 = performance.now();
        let s = 0;
        for (let i = 0; i < N; i++) s += i;
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
        if (s === -1) print("no");
    }
    return b * 1e6 / N;
})();

function bench(name, fn) {
    const per = best(fn) - FLOOR;
    print(`${name.padEnd(34)} ${per.toFixed(2).padStart(8)} ns/call`);
    print(`#DATA\t${name}\t${per.toFixed(3)}`);
    return per;
}

const SEC = 1735689600;                      /* 2025-01-01T00:00:00Z */
const SHORT = "2006-01-02T15:04:05Z";        /* 20 chars, 5 tokens */
/* Ten repetitions of the same layout: ten times the walk, ten times the
 * output, the SAME single date computation and the same one call. */
const LONG = new Array(10).fill(SHORT).join("|");

print(`loop floor ${FLOOR.toFixed(2)} ns/iteration`);
print("");

const fixed = bench("formatRFC3339 (fixed layout)",
                    () => { for (let i = 0; i < N; i++) formatRFC3339(SEC, 0, true); });
const short = bench("formatUnix (20-char layout)",
                    () => { for (let i = 0; i < N; i++) formatUnix(SEC, SHORT); });
const long = bench("formatUnix (209-char layout)",
                   () => { for (let i = 0; i < N; i++) formatUnix(SEC, LONG); });

/* A hoisted layout string is what a Format instance would hold; measuring with
 * a fresh string per call would charge the capability for an allocation the
 * free function does not make either. */
print("");
print(`layout-dependent cost, 20 chars : ${(short - fixed).toFixed(2)} ns` +
      `  (${((short - fixed) / short * 100).toFixed(1)}% of the call)`);
print(`layout-dependent cost, 209 chars: ${(long - fixed).toFixed(2)} ns` +
      `  (${((long - fixed) / long * 100).toFixed(1)}% of the call)`);
print(`per extra layout character      : ` +
      `${((long - short) / (LONG.length - SHORT.length)).toFixed(3)} ns`);
print("");
print(`#DATA\twalk_fraction_short\t${((short - fixed) / short).toFixed(4)}`);
print(`#DATA\twalk_fraction_long\t${((long - fixed) / long).toFixed(4)}`);
print("CALIBRATION -- read this before trusting the figure above.");
print("");
print("The 20-char number was used to predict what `class Format` would save,");
print("and it OVERSTATED it by 4.6x. Measured after building the class:");
print("Format.format is 1.11x formatUnix at scale, crossover ~12 -- not the");
print("~1.8x the 46.6% implied.");
print("");
print("Why: formatRFC3339 does not merely skip the SCAN. It writes a fixed");
print("20-byte pattern with no per-token dispatch at all, so the difference");
print("also contains the token EMISSION, which a compiled layout still has to");
print("do. Only the layout-argument coercion and the memcmp probing are");
print("removable.");
print("");
print("Rule: a ceiling computed as (general - specialised) credits the");
print("removable part with everything the specialised version does");
print("differently. It bounds the win from above and can be far from it.");
