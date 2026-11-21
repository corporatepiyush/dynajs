// covers: c:src/libregexp.c
// The alternation first-byte prefilter (re_pf_first_bytes) now also serves case-insensitive patterns
// and 16-bit subjects. /i: each alternative's first char_i / range_i adds every Latin-1 byte whose
// canonical form it accepts (from a per-thread canonicalize table), only on subjects of 256+ units as
// the single-char /i prefilter does. 16-bit subjects: a BITMAP16 scan over the Latin-1 bitmap, and any
// alternative that can start with a unit above U+00FF (a wide char, an astral char, a class reaching
// past U+00FF, or ANY /i char, since U+212A, U+017F, U+0178 and others fold onto Latin-1 letters) makes
// every unit above U+00FF a candidate. The 13-marker scan over 0.9 MB: UTF-16 g 43 -> 0.5 ms, latin-1
// gi 64 -> 29 ms, UTF-16 gi 68 -> 32 ms.
// Oracle: the pattern run sticky (no unanchored prologue, so never prefiltered) at every position.
// Subjects are random mixes of ASCII, Latin-1, the fold-onto-Latin-1 code points, surrogate pairs and
// lone surrogates, both short (no prefilter, < 32 units) and padded past 256 units; also Latin-1-only subjects
// (the 8-bit BITMAP fold path), mid lengths 32..255 (prefilter on, no fold: an /i alternative bails),
// and one regexp object run over 8-bit, 16-bit, short and long subjects in both orders (the prefilter
// cache key carries the subject width and the fold threshold).
import { suite } from "../lib/kit.js";

const t = suite("regexp alternation prefilter: case folding and UTF-16");
let seed = 424242;
const rnd = (n) => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) % n);
const POOL = [
    "a", "b", "k", "K", "s", "S", "t", "T", "y", "x", " ", "-", "0", "\u00b5", "\u03bc", "\u039c", "\u00ff", "\u0178",
    "\u017f", "\u212a", "\u00e5", "\u00c5", "\u212b", "\u00df", "\u1e9e", "\u0131", "\u0130", "\u2014", "\u{1f600}",
    "\u{10400}", "\u{10428}", "\ud800", "\udc00", "\u00e9", "\u00c9",
];
const pick = () => POOL[rnd(POOL.length)];
function subject(len) {
    let s = "";
    while (s.length < len) s += rnd(3) ? "plain words here ".charAt(rnd(17)) : pick();
    return s;
}
function viaExec(re, s) {
    const out = [];
    re.lastIndex = 0;
    let m;
    while ((m = re.exec(s)) !== null) {
        out.push(m.index + ":" + JSON.stringify(Array.from(m)));
        if (m[0] === "") re.lastIndex = m.index + 1;
    }
    return out;
}
function viaSticky(re, s) {
    const y = new RegExp(re.source, re.flags.replace("g", "") + "y"), u = re.unicode, out = [];
    let i = 0;
    while (i <= s.length) {
        y.lastIndex = i;
        const m = y.exec(s);
        if (m !== null && m.index === i) {
            out.push(i + ":" + JSON.stringify(Array.from(m)));
            if (m[0] !== "") { i += m[0].length; continue; }
        }
        i += u && s.codePointAt(i) > 0xffff ? 2 : 1;
    }
    return out;
}
const SOURCES = [
    "kelvin|sun|yes", "K|S|Y", "\u00b5s|ms|ns", "\u00ff+|\u0178x|zz", "\u00e5|\u00c5ngstr|b", "(?:tt|ss)+|\u00df",
    "[k-m]x|[s-t]y|\u2014", "[\u00e0-\u00ff]|qq|\u212a", "\u{1f600}|ab|\u{10400}", "\u0131|\u0130|i", "\\bsun|kk\\b|yy",
    "(a)|(b)|(\u00e9)", "[^a-z]k|s[^a-z]", "\ud800|\udc00|x", "(?=k)k|s", "[\u0100-\u017f]|e|\u00e9",
];
const FLAGS = ["g", "gi", "gu", "giu", "gim"];
t.test("global exec equals the sticky oracle", (a) => {
    let bad = 0, cases = 0;
    for (const src of SOURCES) {
        for (const fl of FLAGS) {
            let re;
            try { re = new RegExp(src, fl); } catch (e) { continue; }
            for (let k = 0; k < 40; k++) {
                const s = subject(k < 12 ? 1 + rnd(30) : 260 + rnd(500));
                cases++;
                const got = viaExec(re, s), want = viaSticky(re, s);
                if (JSON.stringify(got) !== JSON.stringify(want) && ++bad <= 5) a.deep(got, want, "/" + src + "/" + fl + " on " + JSON.stringify(s));
            }
        }
    }
    a.eq(bad, 0, "mismatches over " + cases + " subjects");
});
const POOL8 = POOL.filter((c) => c.length === 1 && c.charCodeAt(0) <= 0xff);
function subject8(len) {
    let s = "";
    while (s.length < len) s += rnd(3) ? "plain words here ".charAt(rnd(17)) : POOL8[rnd(POOL8.length)];
    return s;
}
t.test("Latin-1 subjects, mid lengths, and one regexp reused across subject kinds", (a) => {
    let bad = 0, cases = 0;
    for (const src of SOURCES) {
        for (const fl of FLAGS) {
            let re;
            try { re = new RegExp(src, fl); } catch (e) { continue; }
            const subs = [];
            for (let k = 0; k < 12; k++) {
                const len = [5, 40, 120, 255, 256, 300, 900][rnd(7)];
                subs.push(rnd(2) ? subject8(len) : subject(len));
            }
            for (const order of [subs, subs.slice().reverse()]) {
                for (const s of order) {
                    cases++;
                    const got = viaExec(re, s), want = viaSticky(re, s);
                    if (JSON.stringify(got) !== JSON.stringify(want) && ++bad <= 5) a.deep(got, want, "/" + src + "/" + fl + " on " + JSON.stringify(s));
                }
            }
        }
    }
    a.eq(bad, 0, "mismatches over " + cases + " subjects");
});
t.test("fold-onto-Latin-1 code points are found past 256 units", (a) => {
    const pad = "plain text ".repeat(40);
    a.eq((pad + "\u212a").search(/q|k/i), -1, "non-unicode /i: Kelvin does not fold to k");
    a.eq((pad + "\u212a").search(/q|k/iu), pad.length, "unicode /iu: Kelvin folds to k");
    a.eq((pad + "\u017f").search(/q|s/iu), pad.length, "unicode /iu: long s folds to s");
    a.eq((pad + "\u0178").search(/q|\u00ff/i), pad.length, "Y-diaeresis upper matches \u00ff");
    a.eq((pad + "\u039c").search(/q|\u00b5/i), pad.length, "Greek capital mu matches micro sign");
    a.eq((pad + "\u2014" + "XYZ").search(/q|xyz/i), pad.length + 1, "latin /i after a wide unit");
    a.eq((pad + "\u{1f600}").search(/q|\u{1f600}/u), pad.length, "astral alternative");
    a.eq(("\u2014" + pad + "TODO").match(/FIXME|todo/gi)[0], "TODO", "markers in a UTF-16 subject");
});
await t.run();
