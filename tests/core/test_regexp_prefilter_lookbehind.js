// covers: c:src/libregexp.c
// Prefilter past leading zero-width assertions (libregexp re_pf_build / re_pf_first_bytes):
// a pattern that opens with a lookbehind or lookahead (positive or negative, any body) or
// \b / \B consumes nothing there, so the first consuming op after it still decides where a
// match can start. Before this, /(?<![\w$])zzz(?![\w$])/g ran the backtracker at every
// position of the subject (11 MB x20: 4.8 s against 11 ms for /\bzzz\b/g).
// Oracle for correctness: a sticky clone tried at every start position, which is the
// definition of the leftmost match and never enters the prefilter loop (sticky bytecode has
// no unanchored prologue). The lookbehind reads text BEFORE the candidate start, including
// text before lastIndex, so random lastIndex starts are part of the differential.
import { suite } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
import { bestMs } from "../lib/cpuclock.js";

const t = suite("test_regexp_prefilter_lookbehind");

let seed = 0x2545f491;
function rnd() { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed; }
function rndInt(n) { return rnd() % n; }
function pick(a) { return a[rndInt(a.length)]; }

function isLoAt(s, j) {
    if (j <= 0 || j >= s.length) return false;
    const c = s.charCodeAt(j), p = s.charCodeAt(j - 1);
    return c >= 0xdc00 && c <= 0xdfff && p >= 0xd800 && p <= 0xdbff;
}
function advance(s, i, uni) {
    if (!uni || i + 1 >= s.length) return i + 1;
    const c = s.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) {
        const d = s.charCodeAt(i + 1);
        if (d >= 0xdc00 && d <= 0xdfff) return i + 2;
    }
    return i + 1;
}
function shape(m, re) {
    if (m === null) return null;
    return { index: m.index, all: Array.from(m), groups: m.groups ? Object.assign({}, m.groups) : undefined, li: re ? re.lastIndex : -1 };
}
function stickyOf(src, flags) {
    return new RegExp(src, flags.replace("g", "").replace("y", "") + "y");
}
function oracleFrom(st, s, from, uni) {
    for (let j = from; j <= s.length; j++) {
        if (uni && j > from && isLoAt(s, j)) continue;
        st.lastIndex = j;
        const m = st.exec(s);
        if (m) return { m, end: st.lastIndex };
    }
    return null;
}
function oracleAll(src, flags, s) {
    const uni = /[uv]/.test(flags), st = stickyOf(src, flags), out = [];
    let i = 0;
    while (i <= s.length && out.length < 400) {
        const r = oracleFrom(st, s, i, uni);
        if (!r) break;
        out.push(shape(r.m, null));
        i = r.end === r.m.index ? advance(s, r.end, uni) : r.end;
    }
    return out;
}

const LB = [
    "(?<=a)", "(?<!a)", "(?<=[\\w$])", "(?<![\\w$])", "(?<=(a|bc))", "(?<!(?:x|yz))",
    "(?<=(?<g>x)y)", "(?<=^|\\s)", "(?<!\\d+)", "(?<=\\b)", "(?<=é)", "(?<!Ā)",
    "(?<=(a)\\1?)", "(?<=$)", "(?<=a*)", "(?<!)", "(?<=(?=b)a|c)", "(?<=\\n)",
];
const LEAD = ["", "", "", "\\b", "\\B", "(?=a)", "(?!b)", "(?<!z)", "(?=(a))"];
const BODY = [
    "ab", "aba", "a", "zz", "b", "(ab|ac)", "[ab]c", "a+", "Āb", "é", "x*", "", "(?:ab)+",
    "(?<n>a)b", "abcabc", "[^a]b", "\\d", "\\s", "^a", "a$", ".b",
];
const TAIL = ["", "", "(?![\\w$])", "(c)?", "(?=b)", "\\b"];
const FLAGS = ["g", "gi", "gu", "gm", "giu", "gim", "", "i", "u", "gv", "gs", "gd"];
const ALPHA = ["a", "b", "c", "x", "y", "z", " ", "$", "_", "\n", "9", "é", "Ā", "A", "B", "Z", "\u{1F600}", "ab", "ab", "zz"];

function genPattern() {
    let lb = pick(LB);
    if (rndInt(4) === 0) lb += pick(LB);
    return pick(LEAD) + lb + pick(LEAD) + pick(BODY) + pick(TAIL);
}
function genText(len, wide) {
    let s = "";
    while (s.length < len) {
        const piece = pick(ALPHA);
        if (!wide && (piece === "Ā" || piece === "\u{1F600}")) continue;
        s += piece;
    }
    return s;
}
function adversarial() {
    return [
        "a".repeat(700), "ab".repeat(350), ("x" + "a".repeat(40) + "b").repeat(12),
        ("$ab " + "zz ").repeat(80), "\n".repeat(50) + "ab\nab" + " ".repeat(300) + "aba",
        ("éab" + "A".repeat(30)).repeat(15), ("Āab" + "c".repeat(30)).repeat(15),
        ("yzab" + "xab").repeat(90), "9ab" + "b".repeat(500) + "aab",
    ];
}

function compile(src, flags) {
    try { return new RegExp(src, flags); } catch (e) { return null; }
}

t.test("generated leading-lookbehind patterns match the per-position sticky oracle", ({ deep, ok }) => {
    const texts = [];
    for (let i = 0; i < 26; i++) texts.push(genText(40 + rndInt(700), i % 2 === 1));
    texts.push(...adversarial());
    let compared = 0, hits = 0, bad = 0;
    for (let k = 0; k < 520 && bad < 8; k++) {
        const src = genPattern(), flags = pick(FLAGS);
        if (!compile(src, flags)) continue;
        for (let n = 0; n < 4 && bad < 8; n++) {
            const s = pick(texts);
            const want = oracleAll(src, flags, s);
            let got;
            if (flags.includes("g")) {
                got = [];
                for (const m of s.matchAll(new RegExp(src, flags))) { got.push(shape(m, null)); if (got.length >= 400) break; }
            } else {
                const m = new RegExp(src, flags).exec(s);
                got = m ? [shape(m, null)] : [];
                want.length = Math.min(want.length, 1);
            }
            compared++;
            hits += want.length;
            if (!deep(got, want, "/" + src + "/" + flags + " on " + JSON.stringify(s.slice(0, 60)) + " (len " + s.length + ")")) bad++;
        }
    }
    ok(compared > 1500, "differential is not vacuous: " + compared + " comparisons");
    ok(hits > 3000, "differential sees real matches: " + hits);
});

t.test("lastIndex starts and sticky flag agree with the oracle, lookbehind reads before lastIndex", ({ deep, ok }) => {
    let compared = 0, bad = 0;
    for (let k = 0; k < 260 && bad < 8; k++) {
        const src = genPattern(), flags = pick(["g", "gi", "gu", "gm", "giu"]);
        if (!compile(src, flags)) continue;
        const s = genText(64 + rndInt(500), k % 3 === 0);
        const uni = /[uv]/.test(flags), st = stickyOf(src, flags);
        for (let n = 0; n < 6 && bad < 8; n++) {
            const L = rndInt(s.length + 1);
            const re = new RegExp(src, flags);
            re.lastIndex = L;
            const got = shape(re.exec(s), re);
            const r = oracleFrom(st, s, L, uni);
            const want = r ? { index: r.m.index, all: Array.from(r.m), groups: r.m.groups ? Object.assign({}, r.m.groups) : undefined, li: r.end } : { index: -1 };
            compared++;
            if (!deep(got === null ? { index: -1 } : got, want, "/" + src + "/" + flags + " lastIndex " + L)) bad++;
            const y = new RegExp(src, flags + "y");
            y.lastIndex = L;
            const gy = shape(y.exec(s), y);
            const wy = r && r.m.index === L ? want : { index: -1 };
            if (!deep(gy === null ? { index: -1 } : gy, wy, "/" + src + "/" + flags + "y lastIndex " + L)) bad++;
        }
    }
    ok(compared > 1000, "lastIndex differential is not vacuous: " + compared);
});

t.test("hand-picked lookbehind cases on prefiltered subjects", ({ deep, eq }) => {
    const pad = "-".repeat(300);
    eq((pad + "xzzz zzz $zzz azzz zzz_ zzz").match(/(?<![\w$])zzz(?![\w$])/g).length, 2, "identifier-boundary zzz");
    deep((pad + "ab Ab aB").match(/(?<=\s)ab/gi), ["Ab", "aB"], "case-insensitive literal after lookbehind");
    deep((pad + "\nab\nxab").match(/(?<=^)ab/gm), ["ab"], "multiline ^ in lookbehind");
    deep(Object.assign({}, /(?<=(?<p>[xy]))ab/.exec(pad + "zab yab").groups), { p: "y" }, "named capture inside lookbehind");
    deep(Array.from(/(?<=(x)|(y))ab/.exec(pad + "yab xab")), ["ab", undefined, "y"], "alternated captures inside lookbehind");
    eq((pad + "Āab Āab").replace(/(?<=Ā)ab/g, "!"), pad + "Ā! Ā!", "two-byte subject");
    eq((pad + "\u{1F600}ab").search(/(?<=\u{1F600})ab/u), pad.length + 2, "astral lookbehind in u mode");
    const re = /(?<=a)b/g;
    re.lastIndex = pad.length + 1;
    eq(re.exec(pad + "ab").index, pad.length + 1, "lookbehind reads the char before lastIndex");
    const y = /(?<=a)b/y;
    y.lastIndex = pad.length + 1;
    eq(y.test(pad + "ab"), true, "sticky lookbehind before lastIndex");
    y.lastIndex = pad.length;
    eq(y.test(pad + "ab"), false, "sticky does not search forward");
    deep((pad + "xab ab").split(/(?<!x)ab/), [pad + "xab ", ""], "split");
});

function bigText(wide) {
    const base = "the quick brown fox jumps over the lazy dog $foo bar_baz 12345 zz z\n";
    return base.repeat(Math.ceil(3e6 / base.length)) + (wide ? "Ā" : "") + " zzz end";
}

t.test("leading lookbehind costs about what the \\b form costs on a 3 MB subject", ({ ok, eq }) => {
    const pairs = [
        [/(?<![\w$])zzz(?![\w$])/g, /\bzzz\b/g, false],
        [/(?<![\w$])zzz(?![\w$])/gu, /\bzzz\b/gu, false],
        [/(?<=\s)zzz/g, /\bzzz\b/g, false],
        [/(?<!x)(?=z)zzz/g, /\bzzz\b/g, false],
        [/(?<![\w$])(?:zzz|zzy)/g, /\b(?:zzz|zzy)/g, false],
        [/(?<![\w$])(?:\bzzz|(?=q)zzy|(?<!a)zzq)/g, /\b(?:zzz|zzy|zzq)/g, false],
        [/(?<![\w$])zzz(?![\w$])/g, /\bzzz\b/g, true],
    ];
    const plain = bigText(false), wide = bigText(true);
    for (const [lb, wb, isWide] of pairs) {
        const s = isWide ? wide : plain;
        const a = bestMs(() => { lb.lastIndex = 0; return s.match(lb).length; });
        const b = bestMs(() => { wb.lastIndex = 0; return s.match(wb).length; });
        eq(a.value, 1, String(lb) + " finds the one match");
        eq(b.value, 1, String(wb) + " finds the one match");
        perfCheck(ok, a.ms <= 4 * b.ms + 4, String(lb) + (isWide ? " (two-byte)" : "") + " took " + a.ms.toFixed(2) + " ms vs " + String(wb) + " " + b.ms.toFixed(2) + " ms");
    }
});

await t.run();
