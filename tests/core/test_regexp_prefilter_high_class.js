// covers: c:src/libregexp.c
// The regexp first-character prefilter on 16-bit (UTF-16) subjects. A class whose lowest member is
// above U+00FF (\p{M}, \p{Script=Greek}, [\u0300-\u036f], ...) skips every unit below that member
// with a vector scan instead of entering the backtracker at each position: \p{M} over 9.6k units of
// mostly-ASCII text went 96 -> 0.8 us. In unicode mode a class with astral members can start at a
// high surrogate, and a scan that skipped a high surrogate could stop on the low half of a pair and
// match it as a lone surrogate, so in unicode mode the threshold is never above U+D800 (found by this
// test: /[\udc00-\udfff]/gu matched the low halves of pairs).
// Oracle: the same pattern run sticky (no unanchored prologue, so no prefilter) at every code point
// position, which must give the same match list as the global exec loop the prefilter drives.
// Subjects mix ASCII, Latin-1, marks, Greek, astral marks, astral Greek, lone surrogates and the
// code points around each threshold; lengths straddle the 32-unit prefilter minimum.
import { suite } from "../lib/kit.js";

const t = suite("regexp prefilter high-only class on wide subjects");
let seed = 12345;
const rnd = (n) => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) % n);
const POOL = [
    "a", "b", " ", "x", "Z", "0", "\u00e9", "\u00ff", "\u0100", "\u02ff", "\u0300", "\u0301", "\u036f", "\u0370",
    "\u03b1", "\u03c9", "\u0391", "\u2014", "\ud7ff", "\ue000", "\uf8ff", "\uffff", "\u{1d165}", "\u{1d167}",
    "\u{10140}", "\u{1018e}", "\u{100000}", "\ud800", "\udc00", "\udbff", "\udfff", "\u212a", "\u017f",
];
function subject(len) {
    let s = "";
    while (s.length < len) s += rnd(4) ? "abc def ghi".charAt(rnd(11)) : POOL[rnd(POOL.length)];
    return s;
}
function viaExec(re, s) {
    const g = new RegExp(re.source, re.flags.includes("g") ? re.flags : re.flags + "g"), out = [];
    let m;
    while ((m = g.exec(s)) !== null) {
        out.push(m.index + ":" + m[0]);
        if (m[0] === "") g.lastIndex = m.index + 1;
    }
    return out;
}
function viaSticky(re, s) {
    const y = new RegExp(re.source, re.flags.replace("g", "") + "y"), u = re.unicode || re.unicodeSets, out = [];
    let i = 0;
    while (i <= s.length) {
        y.lastIndex = i;
        const m = y.exec(s);
        if (m !== null && m.index === i) {
            out.push(i + ":" + m[0]);
            if (m[0] !== "") { i += m[0].length; continue; }
        }
        const c = s.codePointAt(i);
        i += u && c > 0xffff ? 2 : 1;
    }
    return out;
}
const WIDE_PATTERNS = [
    /\p{M}/gu, /\p{M}+/gu, /\p{Script=Greek}+/gu, /[\u0300-\u036f]/g, /[\u0300-\u036f]+x?/g, /[\u0391-\u03c9]\s/g,
    /[\ud800-\udfff]/g, /[\udc00-\udfff]/gu, /[\u{10000}-\u{10ffff}]/gu, /[\u{1d165}-\u{1d169}]/gu, /\p{Mn}\p{L}/gu,
    /[\u0100-\uffff]/g, /[\ue000-\uf8ff]/gu, /[\u{e000}-\u{f8ff}\u{f0000}-\u{ffffd}]/gu, /[\u{10140}-\u{1018e}]|\u2014/gu,
    /\P{ASCII}/gu, /[^\x00-\u02ff]/g, /[^\x00-\u02ff]/gu,
];
t.test("global exec equals the sticky oracle on wide subjects", (a) => {
    let bad = 0;
    for (const re of WIDE_PATTERNS) {
        for (let k = 0; k < 400; k++) {
            const s = subject(1 + rnd(k < 150 ? 40 : 300));
            const got = viaExec(re, s), want = viaSticky(re, s);
            if (JSON.stringify(got) !== JSON.stringify(want)) {
                if (++bad <= 5) a.deep(got, want, re + " on " + JSON.stringify(s));
            }
        }
    }
    a.eq(bad, 0, "mismatching subjects");
});
t.test("a short match after a long low run is found", (a) => {
    const pad = "plain ascii text ".repeat(20);
    a.deep(viaExec(/\p{M}/gu, pad + "e\u0301" + pad + "\u0300"), [pad.length + 1 + ":\u0301", 2 * pad.length + 2 + ":\u0300"]);
    a.deep(viaExec(/[\u{1d165}-\u{1d169}]/gu, "\u2014" + pad + "\u{1d165}"), [pad.length + 1 + ":\u{1d165}"]);
    a.deep(viaExec(/[\ue000-\uf8ff]/gu, "\u2014" + pad + "\u{10000}\ue001"), [pad.length + 3 + ":\ue001"]);
    a.eq(("\u2014" + pad).replace(/\p{M}/gu, ""), "\u2014" + pad);
    a.eq(("\u2014" + pad + "\u0301").replace(/\p{M}/gu, ""), "\u2014" + pad);
});
await t.run();
