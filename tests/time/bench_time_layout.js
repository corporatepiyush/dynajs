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

const SEC = 1735689600;
const SHORT = "2006-01-02T15:04:05Z";
const LONG = new Array(10).fill(SHORT).join("|");

print(`loop floor ${FLOOR.toFixed(2)} ns/iteration`);
print("");

const fixed = bench("formatRFC3339 (fixed layout)",
                    () => { for (let i = 0; i < N; i++) formatRFC3339(SEC, 0, true); });
const short = bench("formatUnix (20-char layout)",
                    () => { for (let i = 0; i < N; i++) formatUnix(SEC, SHORT); });
const long = bench("formatUnix (209-char layout)",
                   () => { for (let i = 0; i < N; i++) formatUnix(SEC, LONG); });

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
