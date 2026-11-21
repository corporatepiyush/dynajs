// covers: c:src/builtins/builtin-regexp.c
// RegExp.prototype[Symbol.split] with the intrinsic %RegExp% species and an intact exec searches
// forward from q instead of trying a sticky match at every position, and reuses rx's program instead
// of compiling flags+"y". Two references: the engine's own generic path (forced by wrapping
// RegExp.prototype.exec, which the spec observes) and a transcription of the spec's loop over a
// sticky clone. Then every step the spec makes observable, which the fast path must keep.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_regexp_split_fast");
let n = 0,
    fails = 0;
const ok = (got, want, what) => {
    n++;
    const g = JSON.stringify(got),
        w = JSON.stringify(want);
    if (g !== w) {
        fails++;
        if (fails <= 25) print("FAIL: " + what + "\n  got  " + g + "\n  want " + w);
    }
};

const origExec = RegExp.prototype.exec;
const legacy = () => [RegExp.lastMatch, RegExp.$1, RegExp.leftContext.length];
const slowSplit = (s, re, lim) => {
    RegExp.prototype.exec = function (x) {
        return origExec.call(this, x);
    };
    try {
        const r = s.split(re, lim);
        return [r, legacy()];
    } finally {
        RegExp.prototype.exec = origExec;
    }
};
const fastSplit = (s, re, lim) => {
    const r = s.split(re, lim);
    return [r, legacy()];
};
const specSplit = (S, rx, lim) => {
    const flags = rx.flags;
    const unicode = flags.includes("u") || flags.includes("v");
    const sp = new RegExp(rx, flags.includes("y") ? flags : flags + "y");
    const A = [];
    const L = lim === undefined ? 2 ** 32 - 1 : lim >>> 0;
    if (L === 0) return A;
    const size = S.length;
    if (size === 0) {
        sp.lastIndex = 0;
        if (origExec.call(sp, S) !== null) return A;
        return [S];
    }
    const adv = (q) => {
        if (!unicode || q + 1 >= size) return q + 1;
        const c = S.charCodeAt(q);
        if (c < 0xd800 || c > 0xdbff) return q + 1;
        const d = S.charCodeAt(q + 1);
        return d >= 0xdc00 && d <= 0xdfff ? q + 2 : q + 1;
    };
    let p = 0,
        q = 0;
    while (q < size) {
        sp.lastIndex = q;
        const z = origExec.call(sp, S);
        if (z === null) {
            q = adv(q);
            continue;
        }
        const e = Math.min(sp.lastIndex, size);
        if (e === p) {
            q = adv(q);
            continue;
        }
        A.push(S.substring(p, q));
        if (A.length === L) return A;
        p = e;
        for (let i = 1; i < z.length; i++) {
            A.push(z[i]);
            if (A.length === L) return A;
        }
        q = p;
    }
    A.push(S.substring(p, size));
    return A;
};

const PATTERNS = [
    /\s+/, /,/, /(?:)/, /(\d)/, /(a)|(b)/, /x*/, /$/m, /^/m, /\b/, /\B/, /(?<=a)/, /(?<!a)b/, /(?=b)/,
    /a|/, /[À-ÿξ]/, /\r?\n/, /(?:)/u, /./u, /(?:)/v, /[^\x00-\x7f]/, /\uD83D/, /\uD83D/u, /(?<=\uD83D)/,
    /b/i, /B/gi, /(?<x>\w)(\w)?/, /a+?/, /$/, /^/, /(?<=^|,)/, /\s*/, /[,;]\s*/, /(,)(;)?/, /€/,
    /ab/y, /,/y, /(?:)/y, /\d/dg, /a/s,
];
const SUBJECTS = [
    "", "a", "ab", "a b  c ", " lead", "trail ", "a,b,,c,", "1a2b3", "abba", "aaa", "xyz", "a\nb\r\nc\n",
    "ÀbÿcξdÉ", "😀a😀", "\uD83D\uD83Dx\uDE00", "\uDE00a\uD83D", "a,b;c ;d", "€x€", "AbaB",
    "word, word; word ".repeat(6), "x".repeat(40) + "ab" + "x".repeat(40), "ξ".repeat(33) + ",a,ξ", "\uD83D".repeat(40) + "😀",
    "line one\nline two\r\nline three\n".repeat(4), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab",
];
const LIMITS = [undefined, 0, 1, 2, 3, -1, 2 ** 32 + 2];
let cmp = 0;
for (const re of PATTERNS)
    for (const s of SUBJECTS)
        for (const lim of LIMITS) {
            const what = re + " on " + JSON.stringify(s.length > 30 ? s.slice(0, 30) + "..." : s) + " limit " + lim;
            const f = fastSplit(s, re, lim);
            const sl = slowSplit(s, re, lim);
            if (JSON.stringify(f) !== JSON.stringify(sl)) ok(f, sl, "fast vs generic path: " + what);
            const sp = specSplit(s, re, lim);
            if (JSON.stringify(f[0]) !== JSON.stringify(sp)) ok(f[0], sp, "fast vs spec transcription: " + what);
            cmp++;
        }
ok(cmp, PATTERNS.length * SUBJECTS.length * LIMITS.length, "differential cases run");
ok(fails, 0, "differential: no disagreement");

ok("a b  c".split(/\s+/), ["a", "b", "c"], "basic");
ok("ab".split(/(?<=a)/), ["a", "b"], "(c) lookbehind sees the character before q");
ok(("x".repeat(40) + "ab").split(/(?<=a)/), ["x".repeat(40) + "a", "b"], "(c) lookbehind, long subject");
ok("a b".split(/\b/), ["a", " ", "b"], "(c) \\b at q sees the previous character");
ok("ab cd".split(/\b/, 3), ["ab", " ", "cd"], "(c) \\b with a limit");
ok("x".split(/$/), ["x"], "a match only at the end is never tried");
ok("abc".split(/(?:)/), ["a", "b", "c"], "empty matches advance");
ok("😀😀".split(/(?:)/u), ["😀", "😀"], "empty matches advance by code point under /u");
ok("😀😀".split(/(?:)/).length, 4, "and by unit without /u");

__kitS.section("(a) result elements are defined, never set through Array.prototype", () => {
    const d0 = Object.getOwnPropertyDescriptor(Array.prototype, "0");
    let hits = 0;
    Object.defineProperty(Array.prototype, "0", { get: () => "G", set: () => hits++, configurable: true });
    Object.defineProperty(Array.prototype, "1", { get: () => "H", set: () => hits++, configurable: true });
    const r = "a,b".split(/(,)/);
    delete Array.prototype[0];
    delete Array.prototype[1];
    if (d0) Object.defineProperty(Array.prototype, "0", d0);
    ok(hits, 0, "(a) result elements are defined, never set through Array.prototype");
    ok([Object.getOwnPropertyDescriptor(r, 0).value, Object.getOwnPropertyDescriptor(r, 1).value, r.length], ["a", ",", 3], "(a) own data properties");
});
__kitS.section("(b) rx.lastIndex untouched", () => {
    const rx = /,/g;
    rx.lastIndex = 5;
    "a,b,c".split(rx);
    ok(rx.lastIndex, 5, "(b) rx.lastIndex untouched");
    let called = 0;
    const li = { valueOf: () => (called++, 0) };
    rx.lastIndex = li;
    ok("a,b,c".split(rx), ["a", "b", "c"], "(b) split with an object lastIndex");
    ok([called, rx.lastIndex === li], [0, true], "(b) rx.lastIndex is never read or written");
});
__kitS.section("@@match getter on rx", () => {
    const rx = /,/;
    let gets = 0;
    Object.defineProperty(rx, Symbol.match, { get: () => (gets++, true) });
    ok("a,b".split(rx), ["a", "b"], "@@match getter on rx");
    ok(gets, 1, "IsRegExp reads @@match exactly once");
});
__kitS.section("rx recompiled during IsRegExp: new source, the flags read before", () => {
    const rx = /a/;
    Object.defineProperty(rx, Symbol.match, { get: () => (rx.compile("b", "i"), true) });
    ok("xAbBx".split(rx), ["xA", "Bx"], "rx recompiled during IsRegExp: new source, the flags read before");
});
__kitS.section("rx recompiled during IsRegExp, same flags", () => {
    const rx = /a/;
    Object.defineProperty(rx, Symbol.match, { get: () => (rx.compile("b"), true) });
    ok("xAbBx".split(rx), ["xA", "Bx"], "rx recompiled during IsRegExp, same flags");
});
__kitS.section("flags reported by the getter decide the clone's flags", () => {
    const rx = /a/;
    Object.defineProperty(rx, "flags", { value: "i" });
    ok("xAyaz".split(rx), ["x", "y", "z"], "flags reported by the getter decide the clone's flags");
});
__kitS.section("a flags getter that drops i", () => {
    const rx = /a/i;
    Object.defineProperty(rx, "flags", { value: "" });
    ok("xAyaz".split(rx), ["xAy", "z"], "a flags getter that drops i");
});
__kitS.section("exec replaced inside ToUint32(limit)", () => {
    const rx = /,/;
    let calls = 0;
    const lim = {
        valueOf() {
            RegExp.prototype.exec = function (s) {
                calls++;
                return origExec.call(this, s);
            };
            return 10;
        },
    };
    let r;
    try {
        r = "a,b,c".split(rx, lim);
    } finally {
        RegExp.prototype.exec = origExec;
    }
    ok(r, ["a", "b", "c"], "exec replaced inside ToUint32(limit)");
    ok(calls > 0, true, "the replaced exec is called");
});
__kitS.section("rx.compile inside ToUint32(limit) does not reach the splitter", () => {
    const rx = /,/;
    const lim = { valueOf: () => (rx.compile(";"), 10) };
    ok("a,b;c".split(rx, lim), ["a", "b;c"], "rx.compile inside ToUint32(limit) does not reach the splitter");
});
__kitS.section("the same with the generic path", () => {
    const rx = /,/;
    const lim = { valueOf: () => (rx.compile(";"), 10) };
    RegExp.prototype.exec = function (s) {
        return origExec.call(this, s);
    };
    let r;
    try {
        r = "a,b;c".split(rx, lim);
    } finally {
        RegExp.prototype.exec = origExec;
    }
    ok(r, ["a", "b;c"], "the same with the generic path");
});
__kitS.section("species constructor", () => {
    let made = 0;
    class R extends RegExp {
        static get [Symbol.species]() {
            return function (re, f) {
                made++;
                return new RegExp(re, f);
            };
        }
    }
    ok("a,b".split(new R(",")), ["a", "b"], "species constructor");
    ok(made, 1, "species constructor called once");
});
__kitS.section("legacy statics after a fast split", () => {
    "q1w2e".split(/(\d)/);
    ok(legacy(), ["2", "2", 3], "legacy statics after a fast split");
    "zzz".split(/(\d)/);
    ok(legacy(), ["2", "2", 3], "a split without a match leaves them");
});
__kitS.section("invalid flags from the getter throw like the constructor", () => {
    let threw = "";
    try {
        const rx = /a/;
        Object.defineProperty(rx, "flags", { value: "uv" });
        "a".split(rx);
    } catch (e) {
        threw = e.name;
    }
    ok(threw, "SyntaxError", "invalid flags from the getter throw like the constructor");
});
__kitS.end();

print("test_regexp_split_fast: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_regexp_split_fast: " + fails + " failures");
