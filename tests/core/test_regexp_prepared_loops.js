// covers: c:src/libregexp.c
// split, replace and match with /g run one exec per match. Each exec used to redo its setup: look the
// prefilter up in a 4-entry thread cache (memcmp of the bytecode, a struct copy), simd_init, allocate
// and free the backtrack stack. The loops now keep one prepared state for the call (lre_exec_prep):
// the prefilter is built once per subject width and fold bucket and read in place, the run-skip table
// is filled once, and a grown backtrack stack is reused. split also records the legacy RegExp.$_ /
// lastMatch state once, from the last match, instead of after every match (no JS runs in between).
// split(/\s+/) on 110k tokens 7.81 -> 6.77 ms, split(/\n/) 8.05 -> 6.62 ms.
// re_suffix_run_exec (run on every exec) is force_inline: once the exec body was inlined into both
// lre_exec_budget and lre_exec_prep, clang kept it out of line, and the extra call (stack protector and
// register zeroing on return) cost ~2 ns per exec, +3% on short no-match calls.
// Oracle: single exec calls (no loop state) on a fresh regexp per step, assembled per the spec
// algorithms, over random patterns and subjects long enough for the prefilter (>= 32 units), with /i
// patterns crossing the 256-unit fold threshold as the remaining subject shrinks.
import { suite } from "../lib/kit.js";

const t = suite("regexp loops with prepared exec state");
let seed = 77;
const rnd = (n) => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) % n);
const PIECES = ["a", "b", "k", "\\s+", "\\s", ",", "x*", "(a)", "(b|k)", "[a-c]", "\\d+", "K", "\\u212a", "\\u00e9", "(?:ab)+", "$", "\\b"];
const CH = ["a", "b", "k", "K", " ", "  ", "\n", ",", "1", "x", "\u212a", "\u00e9", "\u00c9", "ab", "-", "\u2014"];
function pattern() {
    let p = "";
    for (let i = 1 + rnd(3); i > 0; i--) p += PIECES[rnd(PIECES.length)];
    return rnd(4) ? p : p + "|" + PIECES[rnd(PIECES.length)];
}
function subject() {
    let s = "";
    const n = [40, 120, 300, 700][rnd(4)];
    while (s.length < n) s += CH[rnd(CH.length)];
    return s;
}
function execAt(src, flags, s, i) {
    const r = new RegExp(src, flags.replace("g", "") + "g");
    r.lastIndex = i;
    return r.exec(s);
}
function adv(s, i, u) { return u && i < s.length && s.codePointAt(i) > 0xffff ? i + 2 : i + 1; }
function matchOracle(src, flags, s) {
    const out = [], u = flags.includes("u");
    let i = 0;
    while (i <= s.length) {
        const m = execAt(src, flags, s, i);
        if (!m) break;
        out.push(m[0]);
        i = m[0] === "" ? adv(s, m.index + m[0].length, u) : m.index + m[0].length;
    }
    return out.length ? out : null;
}
function replaceOracle(src, flags, s) {
    const u = flags.includes("u");
    let out = "", pos = 0, i = 0;
    while (i <= s.length) {
        const m = execAt(src, flags, s, i);
        if (!m) break;
        out += s.slice(pos, m.index) + "<" + m[0] + "|" + (m[1] ?? "") + ">";
        pos = m.index + m[0].length;
        i = m[0] === "" ? adv(s, pos, u) : pos;
    }
    return out + s.slice(pos);
}
function splitOracle(src, flags, s) {
    const y = new RegExp(src, flags.replace("g", "") + "y"), u = flags.includes("u"), A = [];
    if (s.length === 0) return y.exec(s) ? [] : [s];
    let p = 0, q = 0;
    while (q < s.length) {
        y.lastIndex = q;
        const z = y.exec(s);
        const e = z ? Math.min(y.lastIndex, s.length) : -1;
        if (!z || e === p) { q = adv(s, q, u); continue; }
        A.push(s.slice(p, q));
        for (let k = 1; k < z.length; k++) A.push(z[k]);
        p = e;
        q = p;
    }
    A.push(s.slice(p));
    return A;
}
t.test("split, replace and match /g equal single-exec oracles", (a) => {
    let bad = 0, cases = 0;
    for (const flags of ["g", "gi", "gu", "giu", "gm"]) {
        for (let k = 0; k < 160; k++) {
            const src = pattern();
            let re;
            try { re = new RegExp(src, flags); } catch (e) { continue; }
            for (let j = 0; j < 3; j++) {
                const s = subject();
                cases++;
                const got = JSON.stringify([s.split(re), s.replace(re, (m, g1) => "<" + m + "|" + (typeof g1 === "string" ? g1 : "") + ">"), s.match(re)]);
                const want = JSON.stringify([splitOracle(src, flags, s), replaceOracle(src, flags, s), matchOracle(src, flags, s)]);
                if (got !== want && ++bad <= 4) a.eq(got, want, "/" + src + "/" + flags + " on " + JSON.stringify(s));
            }
        }
    }
    a.eq(bad, 0, "mismatches over " + cases + " cases");
});
t.test("legacy RegExp statics after split are the last match", (a) => {
    "aa  bb   cc".split(/\s+/);
    a.eq(RegExp.lastMatch, "   ");
    a.eq(RegExp.leftContext, "aa  bb");
    a.eq(RegExp.input, "aa  bb   cc");
    "x1y22z".split(/(\d+)/);
    a.eq(RegExp.lastMatch, "22");
    a.eq(RegExp.$1, "22");
    "p,q".split(/,/, 1);
    a.eq(RegExp.lastMatch, ",");
});
t.test("a backtrack stack grown in one exec is reused by the next", (a) => {
    const s = ("(" + "ab".repeat(3000) + ")").repeat(30);
    const parts = s.split(/\((?:a|b)*\)/);
    a.eq(parts.length, 31);
    a.ok(parts.every((p) => p === ""), "every group consumed");
    a.eq(s.replace(/\((?:a|b)*\)/g, "#"), "#".repeat(30));
    const grow = Array.from({ length: 6 }, (_, k) => "(" + "ab".repeat(500 << k) + ")").join("-");
    a.deep(grow.split(/\((?:a|b)*\)/), ["", "-", "-", "-", "-", "-", ""], "each exec needs more stack than the last");
});
await t.run();
