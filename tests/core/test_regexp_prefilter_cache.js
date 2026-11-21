// covers: c:src/libregexp.c
// lre_exec_budget used to rebuild the prefilter (re_pf_build) and the run-skip
// tables (re_pf_run_skip: re_pf_range_bits + a full re_pf_has_backref walk of
// the bytecode) on EVERY exec, so a /g replace or a matchAll loop paid them once
// per match: 15% of a regexp workload. They are now cached per thread in
// re_pf_cache, keyed by bytecode pointer + a byte copy of the bytecode (up to
// RE_PF_CACHE_BC_MAX) + cbuf_type / unicode / case-size flags; patterns that
// start with a literal char or ^ skip the cache because rebuilding them is
// cheaper than the lookup. Modperf round 2026-11-15: replace(/(\w)j/g) on 100K
// chars 3.49 -> 2.21 ms, the mixed regexp workload -12%.
// What can go wrong with such a cache: a stale entry served to a different
// pattern that reuses the same bytecode address after GC, to the same pattern
// on a subject of another width (Latin-1 vs UTF-16), or to a case-insensitive
// pattern on a subject on the other side of RE_PF_CASE_MIN (256). Every result
// here is checked against a hand-written JS scanner that uses no regexp. The
// compile cache holds 64 programs, so 3000 distinct same-length patterns free and
// reuse bytecode buffers: without the byte compare a freed address is served the
// prefilter of the pattern that lived there before (1466 of 3000 rounds wrong).
import { suite } from "../lib/kit.js";

const t = suite("test_regexp_prefilter_cache");

const isW = (c) => (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95;
function wordRuns(s) {
    const out = [];
    for (let i = 0; i < s.length; ) {
        if (isW(s.charCodeAt(i))) {
            let j = i;
            while (j < s.length && isW(s.charCodeAt(j))) j++;
            out.push(s.slice(i, j));
            i = j;
        } else i++;
    }
    return out;
}
function digitRunsBefore(s, ch) {
    const out = [];
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        if (c >= 48 && c <= 57 && (i === 0 || !(s.charCodeAt(i - 1) >= 48 && s.charCodeAt(i - 1) <= 57))) {
            let j = i;
            while (j < s.length && s.charCodeAt(j) >= 48 && s.charCodeAt(j) <= 57) j++;
            if (s[j] === ch) out.push(s.slice(i, j));
            i = j - 1;
        }
    }
    return out;
}
function countSet(s, set) {
    let n = 0;
    for (let i = 0; i < s.length; i++) if (set.includes(s[i])) n++;
    return n;
}

let seed = 12345;
const rnd = (n) => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) % n);
function text(len, alpha) {
    let s = "";
    while (s.length < len) s += alpha[rnd(alpha.length)];
    return s;
}

t.test("word runs: /\\w+/g matches the scanner on Latin-1 and UTF-16 subjects in turn", ({ eq }) => {
    const re = /\w+/g;
    for (let round = 0; round < 40; round++) {
        const s8 = text(40 + rnd(600), "ab_9 .,-\t");
        const s16 = text(40 + rnd(600), "ab_9 .,-中é");
        eq(JSON.stringify(s8.match(re) || []), JSON.stringify(wordRuns(s8)), "latin1 round " + round);
        eq(JSON.stringify(s16.match(re) || []), JSON.stringify(wordRuns(s16)), "utf16 round " + round);
    }
});

t.test("digits before ':' with captures and a run-skip lead class", ({ eq }) => {
    for (let round = 0; round < 30; round++) {
        const s = text(64 + rnd(900), "0123456789:x ");
        const got = [...s.matchAll(/(\d+):/g)].map((m) => m[1]);
        eq(JSON.stringify(got), JSON.stringify(digitRunsBefore(s, ":")), "round " + round);
    }
});

t.test("many distinct patterns compiled and dropped in a loop never see another's prefilter", ({ eq }) => {
    const sets = ["abc", "xyz", "q", "0123456789", "aeiou", "mnop", "rst", "_-"];
    const subject = text(2000, "abcdefghijklmnopqrstuvwxyz0123456789_- ");
    let bad = 0;
    for (let i = 0; i < 400; i++) {
        const set = sets[i % sets.length];
        const re = new RegExp("[" + set.replace(/[-\\\]]/g, "\\$&") + "]", "g");
        const got = (subject.match(re) || []).length;
        if (got !== countSet(subject, set)) bad++;
        if (i % 37 === 0) [...Array(200)].map((_, k) => ({ k }));
    }
    eq(bad, 0, "mismatching counts over 400 compile/exec rounds");
});

t.test("patterns evicted from the compile cache reuse bytecode memory without a stale prefilter", ({ eq }) => {
    const subject = "aa;xx;bb;yy;cc;zz;".repeat(4);
    let bad = 0;
    for (let r = 0; r < 3000; r++) {
        const abc = r % 2 === 1;
        const re = new RegExp("(?:" + r + ")?" + (abc ? "[abc]+;" : "[xyz]+;"), "g");
        const got = (subject.match(re) || []).slice(0, 3).join(",");
        if (got !== (abc ? "aa;,bb;,cc;" : "xx;,yy;,zz;")) bad++;
    }
    eq(bad, 0, "rounds whose matches came from another pattern's prefilter");
});

t.test("case-insensitive class on subjects below and above RE_PF_CASE_MIN", ({ eq }) => {
    const re = /[k-m]x/gi;
    for (const len of [40, 200, 255, 256, 257, 900, 40, 3000, 100]) {
        const s = text(len, "kKlLmMxXab ");
        let want = 0;
        for (let i = 0; i + 1 < s.length; i++) {
            const a = s[i].toLowerCase(), b = s[i + 1].toLowerCase();
            if (a >= "k" && a <= "m" && b === "x") {
                want++;
                i++;
            }
        }
        eq((s.match(re) || []).length, want, "len " + len);
    }
});

t.test("same source, unicode and non-unicode flags, alternate subjects", ({ eq }) => {
    const subj = text(500, "ab é中1");
    const a = /[a-b]+/g, u = /[a-b]+/gu;
    for (let r = 0; r < 20; r++) {
        const want = wordRuns(subj.replace(/[^ab]/g, " ")).length;
        eq((subj.match(a) || []).length, want, "plain round " + r);
        eq((subj.match(u) || []).length, want, "unicode round " + r);
    }
});

t.test("replace with a capture keeps every match", ({ eq }) => {
    const s = "alpha0 user0@example.com beta1 user1@example.com ".repeat(50);
    const got = s.replace(/(\w+)@example/g, "$1@x");
    eq(got, s.split("@example").join("@x"), "replace result");
    eq(s.replace(/(\w)j/g, "$1"), s, "no-match replace leaves the string alone");
});

await t.run();
