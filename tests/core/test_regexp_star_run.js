// covers: c:src/libregexp.c
// A greedy X* or X+ whose X is one character matcher (a char, ., [\s\S], \s, \S, a class, /i or
// /u forms) used to push a 24-byte backtrack frame per character, so /^[\s\S]*NEEDLE/ hit the 256 MB
// backtrack cap ("out of memory in regexp execution") at ~11.2M characters (bench_regexp_scale.js at
// 16 MB). The executor now runs such a loop to its end and pushes ONE frame that steps back a
// character at a time on backtrack (by code point under /u, never splitting a pair across the loop
// start), with the loop start kept in a scratch register whose save sits under the frame. The compiler
// sets bit 31 of the bytecode length when such a loop exists; only then does exec run the variant that
// checks splits for the shape (and reserve the scratch register), so other patterns keep the old code.
// /^[\s\S]*NEEDLE/ runs at ~720 MB/s instead of ~20; kv-line, quoted-string and email scans -20..-30%.
// Oracle: the same pattern with every quantified atom X rewritten to (?:X|(?!)), which matches the
// same strings but is not a single matcher, so it takes the old per-iteration frames.
import { suite } from "../lib/kit.js";

const t = suite("regexp greedy single-character loops");
let seed = 99;
const rnd = (n) => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) % n);
const ATOMS = ["a", "b", ".", "[\\s\\S]", "\\s", "\\S", "[a-c]", "[^b]", "\\d", "x", "A"];
const ATOMS_U = ["\\u{1f600}", "[\\u{1f600}-\\u{1f601}]", "[\\ud800-\\udbff]", "[^a]", "\\p{L}"];
const QUANTS = ["*", "+", "*", "+", "*?", "{2,}"];
const CHARS = ["a", "a", "b", "c", "x", "A", "B", " ", "\n", "1", "\u{1f600}", "\u{1f601}", "\ud83d", "\ude00", "\u212a", "k", "-"];

function piece(u, depth, caps) {
    const k = rnd(depth > 1 ? 6 : 10);
    if (k <= 4) {
        const pool = u && rnd(3) === 0 ? ATOMS_U : ATOMS;
        const at = pool[rnd(pool.length)], q = QUANTS[rnd(QUANTS.length)];
        return [at + q, "(?:" + at + "|(?!))" + q];
    }
    if (k === 5) {
        if (rnd(2)) { const c = CHARS[rnd(9)]; return [c, c]; }
        const g = ATOMS[rnd(ATOMS.length)] + ATOMS[rnd(ATOMS.length)], q = QUANTS[rnd(QUANTS.length)];
        return ["(?:" + g + ")" + q, "(?:" + g + "|(?!))" + q];
    }
    if (k === 6) { caps.n++; const [p, o] = seq(u, depth + 1, caps); return ["(" + p + ")", "(" + o + ")"]; }
    if (k === 7) { const [p, o] = seq(u, depth + 1, caps); return ["(?=" + p + ")", "(?=" + o + ")"]; }
    if (k === 8) { const [p, o] = seq(u, depth + 1, caps); const [p2, o2] = seq(u, depth + 1, caps); return ["(?:" + p + "|" + p2 + ")", "(?:" + o + "|" + o2 + ")"]; }
    if (caps.n > 0) return ["\\1", "\\1"];
    return ["(?<=a)", "(?<=a)"];
}
function seq(u, depth, caps) {
    let p = "", o = "";
    for (let i = 1 + rnd(3); i > 0; i--) { const [a, b] = piece(u, depth, caps); p += a; o += b; }
    return [p, o];
}
function subject() {
    let s = "";
    for (let i = rnd(14); i > 0; i--) s += CHARS[rnd(CHARS.length)];
    return s;
}
const mt = (s, re) => { const m = s.match(re); return m ? [...m] : m; };
const all = (re, s) => JSON.stringify([...s.matchAll(re)].map((m) => [m.index, ...m]));

t.test("random patterns equal the (?:X|(?!)) rewrite", (a) => {
    let bad = 0, cases = 0;
    for (const flags of ["g", "gi", "gu", "giu", "gm", "gs", "gsu"]) {
        const u = flags.includes("u");
        for (let k = 0; k < 400; k++) {
            const caps = { n: 0 };
            const [p, o] = seq(u, 0, caps);
            const anchor = ["", "^", "$"][rnd(3)];
            let re, oracle;
            try {
                re = new RegExp(anchor === "^" ? "^" + p : p + anchor, flags);
                oracle = new RegExp(anchor === "^" ? "^" + o : o + anchor, flags);
            } catch (e) { continue; }
            for (let j = 0; j < 6; j++) {
                const s = subject();
                cases++;
                const got = all(re, s), want = all(oracle, s);
                if (got !== want && ++bad <= 5) a.eq(got, want, re + " on " + JSON.stringify(s));
            }
        }
    }
    a.eq(bad, 0, "mismatches over " + cases + " cases");
});
t.test("backtracking into the loop, by code unit and by code point", (a) => {
    a.deep(mt("aaab", /^(a*)(a)b/), ["aaab", "aa", "a"]);
    a.deep(mt("xaaa", /x(a+)(a{2})$/), ["xaaa", "a", "aa"]);
    a.deep(mt("\u{1f600}\u{1f600}z", /^(.*)(.)z$/u), ["\u{1f600}\u{1f600}z", "\u{1f600}", "\u{1f600}"]);
    a.deep(mt("\u{1f600}\u{1f600}z", /^(.*)(.)z$/), ["\u{1f600}\u{1f600}z", "\u{1f600}\ud83d", "\ude00"]);
    a.deep(mt("\ude00\u{1f600}", /^(?:[\s\S]*)(\u{1f600})$/u), ["\ude00\u{1f600}", "\u{1f600}"]);
    a.deep(mt("ab ab", /(\S+) \1/), ["ab ab", "ab"]);
    a.deep(mt("aXbXc", /^(.*)X(.*)$/), ["aXbXc", "aXb", "c"]);
    a.deep(mt("KKk\u212a", /^(k*)(\u212a)$/iu), ["KKk\u212a", "KKk", "\u212a"]);
    a.eq("line1\nline2".match(/^.*$/gm).join("|"), "line1|line2");
    a.deep(mt("ababx", /^(?:ab)+x/), ["ababx"], "two-op group is not a one-character loop");
    a.deep(mt("aaa", /(?=(a*))a/), ["a", "aaa"]);
    a.deep(mt("aaab", /(?!a*b)a|b/), ["b"]);
});
t.test("a greedy one-character loop over 12M characters stays within the backtrack cap", (a) => {
    const big = "x".repeat(12e6) + "NEEDLE" + "y".repeat(10);
    a.ok(/^[\s\S]*NEEDLE/.test(big), "[\\s\\S]* then a literal");
    a.eq(/^.*E/.exec(big)[0].length, 12e6 + 6, ".* backs off to the last E");
    a.eq(/x+N/.exec(big).index, 0, "x+ then N");
});
await t.run();
