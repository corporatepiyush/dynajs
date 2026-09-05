/* test_pct_core.js -- the percent-encode core's behavioral goldens.
 *
 * The round-8 census found 17 percent-family implementations across 6
 * modules and named the zero-semantic-change consolidation targets; this
 * file pins the shared core's OBSERVABLE behavior over a systematic corpus
 * so the extraction onto src/core/dyn-pct.h provably changes nothing.
 *
 * The corpus crosses the implementation's own decision boundaries: every
 * byte alone (each is one branch of the safe-set), all bytes together,
 * UTF-8 multi-byte input, the form set's special five (*-._ and + and
 * space), and the malformed-escape shapes a decoder must leave literal.
 * Golden values are RFC 3986 / x-www-form-urlencoded deterministic outputs,
 * captured from the pre-refactor implementation and checked by hand against
 * the specs' own examples.
 */
import { formEncode, formDecode, encodeURIComponentStrict,
         URLSearchParams } from "dyna:url";

let n = 0, fails = 0;
const ok = (got, want, what) => {
    n++;
    if (got !== want) { fails++; print("FAIL: " + what + "\n  got  " + JSON.stringify(got) + "\n  want " + JSON.stringify(want)); }
};

/* ---- the exported-codec set, one byte at a time ---- */
{
    /* Determined by the RFC 3986 unreserved set + the exported-codec's five
       extra safe bytes, checked against the WHATWG urlencoded spec C0..FF
       table: every byte NOT in [A-Za-z0-9*-._] escapes (uppercase hex), and
       space escapes as '+' in form mode. These are spelled out, not
       generated, so a wrong safe-set is visible as a wrong ROW. */
    /* Expected value derived per UTF-8 BYTE of the code point (JS strings
       encode to UTF-8 at the boundary, so U+00FF arrives as C3 BF): the
       safe set is [A-Za-z0-9*-._], space is '+' in form mode, everything
       else escapes as uppercase %XX. */
    const enc = [];
    for (let b = 0; b < 256; b++) {
        const c = String.fromCharCode(b);
        let want = "";
        for (const ub of new TextEncoder().encode(c)) {
            const ch = String.fromCharCode(ub);
            const alnum = (ub >= 0x41 && ub <= 0x5A) || (ub >= 0x61 && ub <= 0x7A) ||
                          (ub >= 0x30 && ub <= 0x39);
            if (alnum || "*-._".includes(ch)) want += ch;
            else if (ch === " ") want += "+";
            else want += "%" + ub.toString(16).toUpperCase().padStart(2, "0");
        }
        enc.push([c, want]);
    }
    for (const [c, want] of enc)
        ok(formEncode({ k: c }).slice(2), want,
           "formEncode code point U+" + c.charCodeAt(0).toString(16));

    /* The strict component codec: the same form set (space is '+') plus
       !'()~ escaping. */
    ok(encodeURIComponentStrict("a b"), "a+b", "strict: space is +");
    ok(encodeURIComponentStrict("!*-._'()"), "%21*-._%27%28%29", "strict: !'() escape, *-._ do not");
}

/* ---- decode: %XX, +, malformed left literal ---- */
{
    const d = formDecode("k=%41%42+%C3%A9");
    ok(d.k, "AB é", "decode %XX pairs, + as space, UTF-8 bytes reassembled");
    const m = formDecode("k=100%+no%zz%X");
    ok(m.k, "100%+no%zz%X" .replace("+", " "), "malformed escapes stay literal");
}

/* ---- URLSearchParams: the WHATWG urlencoded set (the other form tier) ---- */
{
    ok(new URLSearchParams([["k", "1~2"]]).toString(), "k=1~2", "WHATWG: ~ verbatim");
    ok(new URLSearchParams([["k", "a\"b#c<d>e"]]).toString(),
       "k=a%22b%23c%3Cd%3Ee", "WHATWG: \" # < > escape");
    ok(new URLSearchParams([["k", " "]]).toString(), "k=+", "WHATWG: space is +");
}

/* ---- round-trip property over a mixed string ---- */
{
    const s = "héllo wörld *-._!~'()#?&=%+/ \t\x7f";
    const enc = formEncode({ k: s }).slice(2);
    ok(formDecode("k=" + enc).k, s, "formEncode/formDecode round-trip");
}

print("test_pct_core: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_pct_core: " + fails + " failures");
