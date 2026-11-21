// covers: c:src/libregexp.c
// tests/core/test_regexp_prefilter_nolatin1.js -- B9 (O2): a pattern whose
// first atom is a class with no member at or below U+00FF (\p{M}, [\u0300-\u036f],
// any range starting above Latin-1) cannot match anywhere in a Latin-1 string,
// so the prefilter answers "no match" without running the matcher at every
// position. Before, replace(/\p{M}/gu, "") on 160 ASCII characters cost about
// 2 us, 17x what normalize("NFD") costs on the same text.
import { sections as __kitSections } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
const __kitS = __kitSections("test_regexp_prefilter_nolatin1");
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, failures = 0;
function eq(got, want, msg) { n++; if (got !== want) { failures++; print("  FAIL:", msg, JSON.stringify(got), "!==", JSON.stringify(want)); } }
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

const ascii = "The quick brown fox jumps over the lazy dog, 0123456789. ".repeat(4);
const latin1 = "Caf\u00e9 cr\u00e8me br\u00fbl\u00e9e, \u00fcber na\u00efve fa\u00e7ade. ".repeat(4);
const wide = "Cafe\u0301 cre\u0300me, \u4e2d\u6587 \u{1F600} ".repeat(4);

const PATTERNS = [/\p{M}/gu, /[\u0300-\u036f]/g, /[\u0300-\u036f]+/g, /\p{Mn}\p{Mn}?/gu, /[\u0100-\uffff]/g, /\p{Script=Han}/gu, /[\u{1F600}-\u{1F64F}]/gu];
for (const re of PATTERNS) {
    eq(ascii.replace(re, ""), ascii, re + " leaves an ASCII string unchanged");
    eq(latin1.replace(re, ""), latin1, re + " leaves a Latin-1 string unchanged");
    eq(new RegExp(re.source, re.flags.replace("g", "")).test(ascii), false, re + " does not test true on ASCII");
    eq(ascii.search(re), -1, re + " search on ASCII is -1");
    const wideHits = wide.match(re);
    check(wideHits !== null && wideHits.length > 0, re + " still matches in a wide string");
}
eq(wide.replace(/\p{M}/gu, ""), wide.replace(/[\u0300\u0301]/g, ""), "marks are removed from a wide string");
eq(("x".repeat(100) + "e\u0301").replace(/\p{M}/gu, ""), "x".repeat(100) + "e", "a mark after a long ASCII run in a wide string is found");
eq(ascii.replace(/[a-z\u0300]/g, "").length, ascii.replace(/[a-z]/g, "").length, "a class with Latin-1 members still matches them");
eq(latin1.replace(/[\u00e0-\u00ff\u0300]/g, "_").includes("_"), true, "a class starting inside Latin-1 still matches Latin-1 letters");

__kitS.section("ASCII chars x40: /\\u0301/g", () => {
    const big = ascii.repeat(80);
    const timeIt = (re) => { let best = 1e9; for (let r = 0; r < 5; r++) { const t = cpuNow(); for (let i = 0; i < 40; i++) big.replace(re, ""); best = Math.min(best, cpuNow() - t); } return best; };
    const lit = /\u0301/g;
    timeIt(lit);
    const litMs = timeIt(lit);
    for (const cls of [/\p{M}/gu, /[\u0300-\u036f]/g]) {
        const clsMs = timeIt(cls);
        print("  " + big.length + " ASCII chars x40: /\\u0301/g " + litMs.toFixed(2) + " ms, " + cls + " " + clsMs.toFixed(2) + " ms");
        perfCheck(check, clsMs < litMs * 4 + 2, cls + " skips an ASCII subject as fast as a no-Latin-1 literal (" + clsMs.toFixed(2) + " vs " + litMs.toFixed(2) + " ms)");
    }
});
__kitS.end();

print("test_regexp_prefilter_nolatin1: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_regexp_prefilter_nolatin1: " + failures + " failures");
