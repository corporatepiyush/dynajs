// covers: c:src/libregexp.c
// ENGINE-PERF section 9: an unanchored regex whose lead is an alternation gets a first-byte bitmap prefilter
// (re_pf_first_bytes): the scan jumps to bytes that can start some alternative and runs the backtracker only
// there. The analysis recursed into the left (nested) branch of each split with a depth cap of 8, and an
// alternation of N words compiles to N-1 nested splits, so 9+ words got no prefilter and the backtracker ran at
// every byte (13 marker words over 4.6 MB: 246 ms, now 2 ms). It is now a bounded worklist (64 pending
// branches, 1024 steps); it still bails (no prefilter, same results) on an empty-matchable alternative or an
// unknown op. Case-insensitive alternatives and 16-bit subjects are covered by test_regexp_alt_prefilter_fold.js.
// Oracle: the same pattern with the sticky flag walked position by position (a sticky regex has no unanchored
// prologue, so it never takes the prefilter): leftmost match, same index, same text, same captures.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_regexp_alt_prefilter");
import { bestMs } from "../lib/cpuclock.js";
import { perfCheck } from "../lib/perf.js";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }

let seed = 987654321;
const rnd = (m) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % m; };
const ALPHA = "abcdefgxyzABCXYZ_019";
const word = (len) => { let w = ""; for (let i = 0; i < len; i++) w += ALPHA[rnd(ALPHA.length)]; return w; };
const esc = (w) => w.replace(/[\\^$.*+?()[\]{}|]/g, "\\$&");

function allMatches(re, s) {
    const out = [];
    re.lastIndex = 0;
    let m;
    while ((m = re.exec(s))) { out.push(m.index + ":" + JSON.stringify(Array.from(m))); if (m[0] === "") re.lastIndex++; }
    return out;
}
function oracle(source, flags, s) {
    const re = new RegExp(source, flags.replace("g", "") + "y");
    const out = [];
    let i = 0;
    while (i <= s.length) {
        let found = null;
        for (let j = i; j <= s.length; j++) { re.lastIndex = j; const m = re.exec(s); if (m) { found = m; break; } }
        if (!found) break;
        out.push(found.index + ":" + JSON.stringify(Array.from(found)));
        i = found.index + (found[0].length || 1);
    }
    return out;
}
function same(source, flags, s, what) {
    const got = allMatches(new RegExp(source, flags), s), want = oracle(source, flags, s);
    const g = got.join("|"), w = want.join("|");
    ok(g === w, what + " /" + source.slice(0, 60) + "/" + flags + " on " + s.length + " chars: got " + got.length + " matches, want " + want.length + (g === w ? "" : " first diff at " + got.findIndex((x, k) => x !== want[k])));
}

__kitS.section("the ENGINE-PERF repro: 13 marker words, global exec over source text", () => {
    const words = ["TODO", "FIXME", "XXX", "HACK", "BUG", "NOTE", "WARNING", "OPTIMIZE", "REVIEW", "DEPRECATED", "SAFETY", "PANIC", "UNSAFE"];
    let s = "";
    for (let i = 0; i < 300; i++) s += "static int f" + i + "(int x) { return x; }" + (i % 7 === 0 ? " // " + words[i % words.length] + " " : "") + "\n";
    same(words.join("|"), "g", s, "bare alternation");
    same("\\b(?:" + words.join("|") + ")\\b", "g", s, "word-bounded group");
    same("(" + words.join("|") + ")", "g", s, "capturing group");
    let c = 0; const re = new RegExp(words.join("|"), "g"); while (re.exec(s)) c++;
    ok(c === 43, "marker count " + c);
});

__kitS.section("random literal alternations, 2..60 words, several shapes and subjects", () => {
    for (let t = 0; t < 120; t++) {
        const nw = 2 + rnd(59);
        const words = Array.from({ length: nw }, () => word(1 + rnd(5)));
        let s = "";
        while (s.length < 600 + rnd(1500)) s += rnd(4) ? word(1 + rnd(6)) + " " : words[rnd(nw)] + (rnd(2) ? " " : "");
        const alt = words.map(esc).join("|");
        const shape = rnd(5);
        const src = shape === 0 ? alt : shape === 1 ? "\\b(?:" + alt + ")\\b" : shape === 2 ? "(" + alt + ")" : shape === 3 ? "(?:" + alt + ")[a-z]?" : "(?=[a-zA-Z_0-9])(?:" + alt + ")";
        same(src, rnd(3) ? "g" : "gm", s, "trial " + t);
    }
});

__kitS.section("shapes that must keep bailing out still match exactly", () => {
    const s = ("one two three four five six seven eight nine ten eleven twelve " + "\n").repeat(20);
    const words = "one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve";
    same(words + "|", "g", s, "empty last alternative");
    same("(?:" + words + ")?x?", "g", s, "optional group");
    same(words.toUpperCase(), "gi", s, "case-insensitive");
    same("(?:" + words + ")|\\d", "g", s, "with a class");
    same("(?:" + words + ")|[0-9]+|[\u00c0-\u00ff]", "g", s.replace(/ten/g, "10 \u00e0"), "a class alternative that matches");
    same("(?:" + words + ")+", "g", s, "repeated group");
    same("(?:x|" + words + ")*e", "g", s, "star then char");
    same(words, "g", s.replace(/seven/g, "séven☃"), "16-bit subject");
    same(words, "gu", s, "unicode flag");
    same("(?:" + words + "|été)", "g", s + "été ", "latin-1 alternative");
});

__kitS.section("String methods over a long alternation agree with the oracle", () => {
    const words = Array.from({ length: 30 }, (_, i) => "w" + i + "q");
    let s = ""; for (let i = 0; i < 400; i++) s += (i % 3 ? "filler" + i : words[i % 30]) + " ";
    const re = new RegExp(words.join("|"), "g");
    const viaReplace = s.replace(re, (m, off) => "<" + off + ">");
    let manual = "", last = 0;
    for (const m of oracle(words.join("|"), "g", s)) { const at = +m.split(":")[0]; const txt = JSON.parse(m.slice(m.indexOf(":") + 1))[0]; manual += s.slice(last, at) + "<" + at + ">"; last = at + txt.length; }
    manual += s.slice(last);
    ok(viaReplace === manual, "replace agrees");
    ok(s.split(re).length === oracle(words.join("|"), "g", s).length + 1, "split agrees");
    ok(s.search(new RegExp(words.join("|"))) === +oracle(words.join("|"), "g", s)[0].split(":")[0], "search finds the first marker");
});

__kitS.section("the 13-word scan takes the prefilter: within 8x of a one-literal scan (CPU time, best of 3)", () => {
    const words = ["TODO", "FIXME", "XXX", "HACK", "BUG", "NOTE", "WARNING", "OPTIMIZE", "REVIEW", "DEPRECATED", "SAFETY", "PANIC", "UNSAFE"];
    let s = ""; for (let i = 0; i < 40000; i++) s += "static int f" + i + "(int x) { return x; }\n";
    const scan = (re) => () => { let c = 0; re.lastIndex = 0; while (re.exec(s)) c++; return c; };
    const alt = bestMs(scan(new RegExp(words.join("|"), "g"))), one = bestMs(scan(/QQQQ/g));
    ok(alt.value === 0 && one.value === 0, "no matches in the filler");
    perfCheck(ok, alt.ms <= Math.max(8 * one.ms, 5), "alternation " + alt.ms.toFixed(2) + " ms vs literal " + one.ms.toFixed(2) + " ms over " + s.length + " chars (no prefilter: about 100x)");
});

print("test_regexp_alt_prefilter: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
__kitS.end();
