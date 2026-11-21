// flags: --std
// Re-pin the five oracle_* parser differentials (tests/core/oracle_*.js).
//
// Each of those files hashes every case's result and compares the case count, the rejected (or
// positions) count and the hash with three pinned constants (PIN_CASES, PIN_COUNT, PIN_HASH); a
// mismatch prints FAIL lines and throws. After a DELIBERATE parser behaviour change, run
//
//   ./dynajs --std tools/pin-oracle-verdicts.js [--dry]
//
// from the repo root: it runs each file with ./dynajs, reads the observed values from its verdict
// lines and rewrites the three constants. REVIEW the printed old -> new line before keeping it: a
// pin that moves without a reviewed behaviour change is a deleted test.
import * as std from "std";
import { Exec } from "dyna:sys";

const FILES = ["tests/core/oracle_asi.js", "tests/core/oracle_expr_precedence.js", "tests/core/oracle_line_col.js",
    "tests/core/oracle_parse_ident.js", "tests/core/oracle_parse_string.js"];
const dry = scriptArgs.includes("--dry");

export function observed(name, out) {
    const ok = new RegExp("^" + name + ": OK \\((\\d+) cases, (\\d+) \\w+, hash ([0-9a-f]+) matches").exec(out.split("\n").find((l) => l.startsWith(name + ": OK")) || "");
    if (ok) return { cases: +ok[1], count: +ok[2], hash: ok[3] };
    const fed = /FAIL: \S+ fed (\d+) cases/.exec(out), cnt = new RegExp("FAIL: " + name + " (\\d+) \\w+, pinned").exec(out), h = /FAIL: \S+ hash ([0-9a-f]+), pinned/.exec(out);
    return { fed: fed && +fed[1], cnt: cnt && +cnt[1], h: h && h[1] };
}
const pinOf = (src, k) => { const m = new RegExp("const " + k + " = (\"?)([0-9a-f]+)\\1;").exec(src); return m ? m[2] : null; };

let bad = 0;
for (const path of FILES) {
    const name = path.slice(path.lastIndexOf("/") + 1, -3);
    const src = std.loadFile(path);
    if (src === null || pinOf(src, "PIN_HASH") === null) { print("no pinned verdict in " + path); bad++; continue; }
    const r = Exec("./dynajs", [path], { maxBuffer: 64 << 20 });
    const o = observed(name, String(r.stdout));
    const now = o.hash !== undefined ? o : {
        cases: o.fed ?? +pinOf(src, "PIN_CASES"), count: o.cnt ?? +pinOf(src, "PIN_COUNT"), hash: o.h ?? pinOf(src, "PIN_HASH"),
    };
    if (o.hash === undefined && o.fed === null && o.cnt === null && o.h === null) { print("cannot read the verdict of " + path + ":\n" + String(r.stdout).slice(-400)); bad++; continue; }
    const was = `${pinOf(src, "PIN_CASES")} cases, ${pinOf(src, "PIN_COUNT")}, ${pinOf(src, "PIN_HASH")}`;
    const is = `${now.cases} cases, ${now.count}, ${now.hash}`;
    if (was === is) { print(`unchanged ${path.padEnd(40)} ${is}`); continue; }
    const out = src.replace(/const PIN_CASES = \d+;/, `const PIN_CASES = ${now.cases};`)
        .replace(/const PIN_COUNT = \d+;/, `const PIN_COUNT = ${now.count};`)
        .replace(/const PIN_HASH = "[0-9a-f]+";/, `const PIN_HASH = "${now.hash}";`);
    print(`${dry ? "would re-pin" : "re-pinned"} ${path.padEnd(40)} ${was} -> ${is}`);
    if (!dry) { const fd = std.open(path, "w"); fd.puts(out); fd.close(); }
}
std.exit(bad ? 1 : 0);
