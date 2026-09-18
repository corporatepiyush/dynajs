/* bench_regexp_scale.js -- how the regex engine behaves as the SUBJECT grows,
 * from a few bytes to hundreds of megabytes.
 *
 * tests/bench_regexp.js fixes the subject at ~13 KB and varies the pattern.
 * That answers "which opcode is slow" and cannot answer "does this scale".
 * They are different questions and a change can improve one while ruining the
 * other -- an engine that gets faster per byte but allocates per byte will look
 * good at 13 KB and die at 100 MB.
 *
 * What this measures, per size:
 *   - throughput in MB/s, so a flat line means linear and a falling line means
 *     superlinear;
 *   - narrow (Latin-1) and wide (UTF-16) separately, since a wide subject is
 *     2 bytes per code unit and the prefilter gives up on it.
 *
 * Memory is the other half of "does it scale" and it is NOT measured here; see
 * the note on ru_maxrss below and tests/bench_regexp_memory.js.
 *
 * Sizes step by 4x so superlinearity is unmistakable: a 4x input that takes 16x
 * longer is O(n^2) and no amount of constant-factor work will save it.
 *
 * Output lines beginning with `#X` are machine-readable:
 *   #X <case> <width> <bytes> <ms> <MB/s>
 *
 * Usage: dynajs --std tests/bench_regexp_scale.js [maxMB] [narrow|wide|both]
 *   maxMB defaults to 64, which is the whole SIZES ladder bar the last rung and
 *   takes ~75 s. 256 adds the last rung and needs several GB.
 *   The width defaults to `both`; pass one to sweep a single width.
 */
import * as std from "std";

const MAX_MB = parseFloat(scriptArgs[1] || "64");
const WIDTH = scriptArgs[2] || "both";
if (["narrow", "wide", "both"].indexOf(WIDTH) < 0) {
    print("bad width '" + WIDTH + "': want narrow, wide or both");
    std.exit(1);
}
const MB = 1024 * 1024;

/* THIS FILE DELIBERATELY HAS NO MEMORY COLUMN. It had one that printed -1 on
 * every row, and restoring the probe behind it produced a number that was
 * wrong by the rep count -- 195 bytes per input character against a true 24.
 *
 * `ru_maxrss` is a high-water mark over a PROCESS. In a sweep it is a running
 * max, so a row inherits any peak an earlier row set; and it grows linearly
 * with the number of executions even though the allocator's peak LIVE bytes is
 * constant and everything is freed. A plain malloc ladder with no regex in it
 * -- grow to 24 MB by realloc, free, repeat -- reproduces the figures to the
 * kilobyte, so what the column measured was the platform allocator, not this
 * engine.
 *
 * A per-operation memory number therefore requires a process that performs the
 * operation ONCE. That is tests/bench_regexp_memory.js. Adding a column back
 * here would only re-tell the same lie in a different font. */

/* Build a subject of approximately `bytes`. The needle appears ONCE, at the
   very end, so every case is a full scan of the whole subject -- otherwise a
   short-circuiting match measures the position of the needle, not the engine. */
function mkSubject(bytes, wide) {
    const unit = "the quick brown fox jumps over the lazy dog 0123456789 ";
    const TAIL = " NEEDLE_AT_THE_VERY_END";
    /* Build to the ACTUAL target. A first version always materialised a 1 MB
       chunk first, so the "64 byte" row was really 1 MB run 200,000 times and
       the whole sweep hung. Size the body to `bytes` and nothing more. */
    const body = Math.max(0, bytes - TAIL.length - (wide ? 1 : 0));
    let s = body > 0 ? unit.repeat(Math.ceil(body / unit.length)).slice(0, body) : "";
    /* One non-Latin-1 char forces the whole JSString to uint16_t; at the FRONT
       so the subject is wide for its entire length. */
    if (wide) s = "中" + s;
    return s + TAIL;
}

const CASES = [
    /* name,           regexp,                       what it stresses */
    ["literal_scan", () => /NEEDLE_AT_THE_VERY_END/, "prefilter + memcmp"],
    ["class_scan",   () => /N[A-Z]{5}_AT_THE/,       "REOP_range, no literal prefilter"],
    ["dot_star",     () => /^[\s\S]*NEEDLE/,          "greedy backtrack over the whole subject"],
    ["alternation",  () => /(NEEDLE|HAYSTACK|MISSING)_AT_THE/, "split frames"],
    ["global_count", () => /o/g,                      "many matches: per-match capture cost"],
];

print("bench_regexp_scale: up to " + MAX_MB + " MB per subject, width=" + WIDTH);
print("  (memory is not measured here: tests/bench_regexp_memory.js)");
print("");
print("  case            width    MB      ms      MB/s");

const WIDTHS = WIDTH === "both" ? [false, true] : [WIDTH === "wide"];
for (const wide of WIDTHS) {
    const w = wide ? "wide" : "narrow";
    const SIZES = [64, 1024, 16384, 262144, 1048576, 4194304, 16777216,
                   67108864, 268435456];
    for (const bytes of SIZES) {
        if (bytes > MAX_MB * MB) break;
        const mb = bytes / MB;
        let subj;
        try { subj = mkSubject(bytes, wide); }
        catch (e) { print("  (OOM building " + mb + " MB " + w + ")"); break; }
        const realBytes = subj.length * (wide ? 2 : 1);

        for (const [name, mk] of CASES) {
            /* global_count on hundreds of MB is minutes of work; cap it. */
            if (name === "global_count" && bytes > 16 * MB) continue;
            if (name === "dot_star" && bytes > 16 * MB) continue;

            const re = mk();
            /* Reps chosen so tiny subjects still clear the timing floor. */
            const reps = bytes < 4096 ? 200000 : bytes < MB ? 200 : 3;
            const f = () => {
                for (let i = 0; i < reps; i++) {
                    if (re.global) { re.lastIndex = 0; let c = 0;
                        while (re.exec(subj) && ++c < 1000000) {} }
                    else re.test(subj);
                }
            };
            f();                                  /* warm */
            const t0 = performance.now();
            f();
            const t = performance.now() - t0;
            const total = realBytes * reps;
            print("  " + name.padEnd(15) + w.padEnd(7) +
                  String(mb < 1 ? mb.toFixed(6) : mb).padStart(6) +
                  String(t.toFixed(1)).padStart(9) +
                  String(((total / MB) / (t / 1000)).toFixed(1)).padStart(9));
            print("#X " + name + " " + w + " " + realBytes + " " + t.toFixed(3) + " " +
                  ((total / MB) / (t / 1000)).toFixed(2));
        }
        subj = null;
    }
}
print("#X DONE");
