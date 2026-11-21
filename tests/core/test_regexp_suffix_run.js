// covers: c:src/libregexp.c
// Suffix-run fast path (libregexp re_suffix_run_exec). An unanchored regexp whose whole body is
// C+$, C*$ (greedy or lazy) or ^C+|C+$, with C one of \s, a single code unit, or a code-unit range
// class, no m flag, no i-folded class and no extra captures, is answered without the backtracker:
// the match is the maximal trailing C-run that starts at or after lastIndex (or, for ^C+|C+$ at
// position 0, the leading run). It is O(run) on any subject size and storage, including UTF-16
// subjects and u/v mode. In u/v mode a range class whose high bound reaches 0xd800 falls back to the
// backtracker: it may hold surrogates, and a high bound of 0xffff also matches every astral code point.
// Why: trims such as s.replace(/ +$/, "") were 1-30 ms per MB and dominated short-string replace
// (ENGINE-PERF.md item 4). Oracles: a sticky regexp tried at every start position (sticky bytecode
// has no unanchored prologue, so it never takes the fast path), and the same pattern wrapped in a
// capture group (two captures, so it also never takes the fast path).
import { suite } from "../lib/kit.js";
import { cpuNow } from "../lib/cpuclock.js";
import { perfCheck } from "../lib/perf.js";
const t = suite("test_regexp_suffix_run");

function viaSticky(src, flags, s, from) {
    const st = new RegExp(src, flags.replace("g", "") + "y");
    for (let j = from; j <= s.length; j++) {
        st.lastIndex = j;
        const m = st.exec(s);
        if (m) return m.index + ":" + JSON.stringify(m[0]);
    }
    return "none";
}

function viaExec(src, flags, s, from) {
    const re = new RegExp(src, flags.includes("g") ? flags : flags + "g");
    re.lastIndex = from;
    const m = re.exec(s);
    return m ? m.index + ":" + JSON.stringify(m[0]) : "none";
}

const PATS = ["\\s+$", "\\s*$", " +$", "-+$", "[ \\t]+$", "\\s+?$", "\\s*?$", " *$", "^\\s+|\\s+$", "^ +| +$",
    "^[ab]+|[cd]+$", "[\\u2014\\s]+$", "[\\ud800-\\udfff]+$", "\\ud83d+$", "[^x]+$", "^\\s+?|\\s+$", "\\s{2,}$",
    "[^\\u0000-\\uefff]+$", "[\\uf000-\\uffff]+$", "[\\ue000-\\uffff]*$", "[^a]+$", "\\S+$"];
const FLAGS = ["", "g", "u", "gu", "v", "i", "m", "gm"];
const ALPHA = ["a", "b", "c", "d", "x", " ", " ", "\t", "-", "\n", " ", "—", " ", "😀", "\ud83d", "\ude00", "　"];

let seed = 777;
function rnd(n) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed % n;
}
function gen(len) {
    let s = "";
    for (let i = 0; i < len; i++) s += ALPHA[rnd(ALPHA.length)];
    return s;
}

const inputs = ["", " ", "  ", "x", "x ", " x", "  x  ", "—", "a— ", "unsigned long ", "cd", "abcd", "ab", "aa cd",
    "x" + " ".repeat(40), " ".repeat(40) + "x", " ".repeat(70), "—" + " ".repeat(40), "x😀 ", "x😀",
    "\ud83d\ud83d", "a\ud83d", "-".repeat(33) + "x" + "-".repeat(33)];
for (let k = 0; k < 150; k++) inputs.push(gen(rnd(12)));
for (let k = 0; k < 60; k++) inputs.push(gen(30 + rnd(50)));

t.test("exec at every lastIndex agrees with the sticky oracle", (a) => {
    let n = 0;
    for (const src of PATS) for (const flags of FLAGS) {
        let bad = 0;
        try { new RegExp(src, flags); } catch { continue; }
        for (const s of inputs) {
            for (const from of [0, 1, s.length >> 1, s.length - 1, s.length]) {
                if (from < 0) continue;
                n++;
                const got = viaExec(src, flags, s, from), want = viaSticky(src, flags, s, from);
                if (got !== want && bad++ < 2)
                    a.eq(got, want, "/" + src + "/" + flags + " from " + from + " on " + JSON.stringify(s));
            }
        }
        a.ok(bad === 0, "/" + src + "/" + flags + ": " + bad + " mismatches");
    }
    a.ok(n > 10000, "ran " + n + " exec comparisons");
});

t.test("replace, split, test and match agree with the capture-wrapped pattern", (a) => {
    for (const src of PATS) for (const flags of FLAGS) {
        let fast, slow;
        try { fast = new RegExp(src, flags); slow = new RegExp("(" + src + ")", flags); } catch { continue; }
        for (const s of inputs) {
            const tag = "/" + src + "/" + flags + " on " + JSON.stringify(s);
            a.eq(s.replace(fast, "<$&>"), s.replace(slow, "<$&>"), "replace " + tag);
            a.eq(s.replace(fast, () => "#"), s.replace(slow, () => "#"), "replace fn " + tag);
            a.eq(JSON.stringify(s.match(fast)?.slice(0, 1)), JSON.stringify(s.match(slow)?.slice(0, 1)), "match " + tag);
            fast.lastIndex = slow.lastIndex = 0;
            a.eq(fast.test(s), slow.test(s), "test " + tag);
            a.eq(fast.lastIndex, slow.lastIndex, "lastIndex after test " + tag);
            a.eq(JSON.stringify(s.split(fast)), JSON.stringify(s.split(slow).filter((_, i) => i % 2 === 0)), "split " + tag);
        }
    }
});

t.test("sticky, multiline and lastIndex state are unchanged", (a) => {
    const g = /\s+$/g;
    g.lastIndex = 3;
    const m = g.exec("ab    ");
    a.eq(m && m.index, 3, "a g match starts at lastIndex inside the run");
    a.eq(g.lastIndex, 6, "lastIndex moves to the end");
    a.eq(g.exec("ab    "), null, "the next exec at the end finds nothing");
    a.eq(g.lastIndex, 0, "and resets lastIndex");
    const z = /\s*$/g;
    a.eq(JSON.stringify("ab  ".match(z)), '["  ",""]', "\\s*$ with g also matches the empty string at the end");
    a.eq("a b \nc  ".replace(/ +$/gm, "#"), "a b#\nc#", "m flag keeps line-end semantics");
    a.eq("  x  ".replace(/^\s+|\s+$/g, ""), "x", "the trim idiom");
    a.eq("    ".replace(/^\s+|\s+$/g, ""), "", "an all-blank subject is one leading match");
    a.eq(RegExp.lastMatch, "    ", "legacy lastMatch is recorded");
    const big = ("x" + " ".repeat(64)).repeat(15000) + "x" + " ".repeat(5);
    a.eq(big.replace(/ +$/, "").length, big.length - 5, "1 MB Latin-1 subject");
    a.eq(("—" + big).replace(/\s+$/, "").length, big.length - 4, "1 MB UTF-16 subject");
    a.eq(("—" + big + "　").replace(/\s*$/u, "").length, big.length - 4, "u flag on a UTF-16 subject");
});

t.test("cost does not grow with the subject size", (a) => {
    const unit = "x" + " ".repeat(64);
    const small = unit.repeat(250) + "x  ", large = unit.repeat(8000) + "x  ";
    const wideS = "\u2014" + small, wideL = "\u2014" + large;
    const best = (s, re) => { let b = 1e9; for (let r = 0; r < 3; r++) { const t0 = cpuNow(); for (let i = 0; i < 5; i++) { re.lastIndex = 0; re.exec(s); } b = Math.min(b, cpuNow() - t0); } return b; };
    for (const [name, re] of [["/ +$/", / +$/], ["/\\s*$/", /\s*$/], ["/^\\s+|\\s+$/g", /^\s+|\s+$/g]]) {
        for (const [kind, s, l] of [["Latin-1", small, large], ["UTF-16", wideS, wideL]]) {
            const ms = best(s, re), ml = best(l, re);
            perfCheck(a.ok.bind(a), ml < ms * 8 + 3, name + " " + kind + ": 512 KB costs " + ml.toFixed(2) + " ms vs 16 KB " + ms.toFixed(2) + " ms (x5 exec)", a.skip.bind(a));
        }
    }
});

await t.run();
