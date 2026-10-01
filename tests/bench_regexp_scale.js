import * as std from "std";

const MAX_MB = parseFloat(scriptArgs[1] || "64");
const WIDTH = scriptArgs[2] || "both";
if (["narrow", "wide", "both"].indexOf(WIDTH) < 0) {
    print("bad width '" + WIDTH + "': want narrow, wide or both");
    std.exit(1);
}
const MB = 1024 * 1024;

function mkSubject(bytes, wide) {
    const unit = "the quick brown fox jumps over the lazy dog 0123456789 ";
    const TAIL = " NEEDLE_AT_THE_VERY_END";
    const body = Math.max(0, bytes - TAIL.length - (wide ? 1 : 0));
    let s = body > 0 ? unit.repeat(Math.ceil(body / unit.length)).slice(0, body) : "";
    if (wide) s = "中" + s;
    return s + TAIL;
}

const CASES = [
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
            if (name === "global_count" && bytes > 16 * MB) continue;
            if (name === "dot_star" && bytes > 16 * MB) continue;

            const re = mk();
            const reps = bytes < 4096 ? 200000 : bytes < MB ? 200 : 3;
            const f = () => {
                for (let i = 0; i < reps; i++) {
                    if (re.global) { re.lastIndex = 0; let c = 0;
                        while (re.exec(subj) && ++c < 1000000) {} }
                    else re.test(subj);
                }
            };
            f();
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
