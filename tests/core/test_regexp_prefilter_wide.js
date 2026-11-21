// covers: c:src/libregexp.c
// The regexp prefilter skips ahead to the first code unit that can start a match. On a 16-bit
// subject a character class's candidates above U+00FF must still be candidates: the old bitmap kept
// only the Latin-1 part, so /[^\x00-\x7f]/ missed "€" once the subject reached 32 units.
// Oracle: class membership computed per character from the class's own range list (and the spec's
// non-unicode Canonicalize for /i), never by another regexp.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_regexp_prefilter_wide");
let n = 0,
    fails = 0;
const ok = (got, want, what) => {
    n++;
    if (got !== want) {
        fails++;
        if (fails <= 20) print("FAIL: " + what + " got " + JSON.stringify(got) + " want " + JSON.stringify(want));
    }
};

__kitS.section("negated ASCII class finds a wide char past 32 units", () => {
ok(/[^\x00-\x7f]/.test("a".repeat(40) + "€"), true, "negated ASCII class finds a wide char past 32 units");
ok(/[^\x00-\x7f]/.test("a".repeat(10) + "€"), true, "negated ASCII class, short subject");
ok(" JS_FreeValue(ctξ, el);\n        ".search(/[À-ÿξ]/), 16, "mixed Latin-1 + wide class on a wide subject");
ok(/[ξé]/.test("x".repeat(64) + "ξ"), true, "two-element class, the wide member");
ok(/[ξé]/.test("x".repeat(64) + "ξé"), true, "two-element class, both");
ok(/[α-γ]/.test("x".repeat(64) + "β"), true, "wide-only range");
ok(/[α-γ]/.test("x".repeat(64) + "δ"), false, "wide-only range, non-member");
ok(/[a-cĀ-ſ]+/.exec("€".repeat(40) + "xxĀaŁ")[0], "ĀaŁ", "mixed class match text");
});
__kitS.end();

const canon = (c) => {
    const u = String.fromCharCode(c).toUpperCase();
    if (u.length !== 1) return c;
    const cu = u.charCodeAt(0);
    if (c >= 128 && cu < 128) return c;
    return cu;
};
const inRanges = (ranges, c) => {
    for (const [lo, hi] of ranges) if (c >= lo && c <= hi) return true;
    return false;
};
const complement = (ranges) => {
    const out = [];
    let next = 0;
    for (const [lo, hi] of [...ranges].sort((a, b) => a[0] - b[0])) {
        if (lo > next) out.push([next, lo - 1]);
        next = Math.max(next, hi + 1);
    }
    if (next <= 0xffff) out.push([next, 0xffff]);
    return out;
};
const foldMember = (ranges) => {
    const set = new Uint8Array(0x10000);
    for (const [lo, hi] of ranges) for (let c = lo; c <= hi; c++) set[canon(c)] = 1;
    return (c) => set[canon(c)] === 1;
};

const CLASSES = [
    { name: "latin1-only", re: /[a-f\xe0-\xef]/, member: (c) => inRanges([[0x61, 0x66], [0xe0, 0xef]], c) },
    { name: "wide-only", re: /[ξα-γ一-丏]/, member: (c) => inRanges([[0x3be, 0x3be], [0x3b1, 0x3b3], [0x4e00, 0x4e0f]], c) },
    { name: "mixed", re: /[À-ÿξ]/, member: (c) => inRanges([[0xc0, 0xff], [0x3be, 0x3be]], c) },
    { name: "mixed2", re: /[a-zĀ-ſ\ud800-\udbff]/, member: (c) => inRanges([[0x61, 0x7a], [0x100, 0x17f], [0xd800, 0xdbff]], c) },
    { name: "negated-ascii", re: /[^\x00-\x7f]/, member: (c) => inRanges(complement([[0, 0x7f]]), c) },
    { name: "negated-mixed", re: /[^a-zЀ-ӿ]/, member: (c) => !inRanges([[0x61, 0x7a], [0x400, 0x4ff]], c) },
    { name: "icase-mixed", re: /[a-càа-г]/i, member: foldMember([[0x61, 0x63], [0xe0, 0xe0], [0x430, 0x433]]) },
    { name: "icase-latin1", re: /[x-zé]/i, member: foldMember([[0x78, 0x7a], [0xe9, 0xe9]]) },
];
for (const k of CLASSES) {
    const rangesOk = (c) => k.member(c) === k.re.test(String.fromCharCode(c));
    let firstBad = -1;
    for (let c = 0; c <= 0xffff && firstBad < 0; c++) if (!rangesOk(c)) firstBad = c;
    ok(firstBad, -1, k.name + ": the oracle agrees with a one-character subject");
}

const OFFSETS = [0, 31, 32, 33, 100];
const TAIL = 40;
const pickFiller = (member, from, to) => {
    for (let c = from; c <= to; c++) if (!member(c)) return String.fromCharCode(c);
    return null;
};
for (const k of CLASSES) {
    const fill = pickFiller(k.member, 0x21, 0xff);
    const wfill = pickFiller(k.member, 0x2600, 0x26ff);
    const W = "☃";
    const wMember = k.member(0x2603);
    const pads = OFFSETS.map((o) => fill.repeat(o));
    const tail = fill.repeat(TAIL);
    let bad = 0;
    for (let c = 0; c <= 0xffff; c++) {
        const ch = String.fromCharCode(c);
        const m = k.member(c);
        for (let j = 0; j < OFFSETS.length; j++) {
            const off = OFFSETS[j];
            if (c <= 0xff) {
                const got = (pads[j] + ch + tail).search(k.re);
                const want = m ? off : -1;
                if (got !== want) {
                    bad++;
                    if (bad <= 5) ok(got, want, k.name + " latin1 U+" + c.toString(16) + " at " + off);
                }
            }
            const wide = wfill === null ? pads[j] + ch + tail + W : pads[j] + ch + tail + wfill;
            const got = wide.search(k.re);
            const want = m ? off : wfill === null && wMember ? off + 1 + TAIL : -1;
            if (got !== want) {
                bad++;
                if (bad <= 5) ok(got, want, k.name + " wide U+" + c.toString(16) + " at " + off);
            }
        }
    }
    ok(bad, 0, k.name + ": every code point at offsets " + OFFSETS.join("/") + ", Latin-1 and wide subjects");
}

print("test_regexp_prefilter_wide: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_regexp_prefilter_wide: " + fails + " failures");
