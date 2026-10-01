import { formEncode, formDecode, encodeURIComponentStrict,
         URLSearchParams } from "dyna:url";

let n = 0, fails = 0;
const ok = (got, want, what) => {
    n++;
    if (got !== want) { fails++; print("FAIL: " + what + "\n  got  " + JSON.stringify(got) + "\n  want " + JSON.stringify(want)); }
};

{
    const enc = [];
    for (let b = 0; b < 256; b++) {
        const c = String.fromCharCode(b);
        let want = "";
        for (const ub of new TextEncoder().encode(c)) {
            const ch = String.fromCharCode(ub);
            // WHATWG urlencoded serializer (URL standard,
            // "application/x-www-form-urlencoded serializing"): A-Za-z0-9 and
            // * - . _ ~ stay literal, space becomes +, everything else escapes.
            const alnum = (ub >= 0x41 && ub <= 0x5A) || (ub >= 0x61 && ub <= 0x7A) ||
                          (ub >= 0x30 && ub <= 0x39);
            if (alnum || "*-._~".includes(ch)) want += ch;
            else if (ch === " ") want += "+";
            else want += "%" + ub.toString(16).toUpperCase().padStart(2, "0");
        }
        enc.push([c, want]);
    }
    for (const [c, want] of enc)
        ok(formEncode({ k: c }).slice(2), want,
           "formEncode code point U+" + c.charCodeAt(0).toString(16));

    ok(encodeURIComponentStrict("a b"), "a%20b", "strict: space is %20 (API.md: like encodeURIComponent)");
    ok(encodeURIComponentStrict("!*-._'()"), "%21*-._%27%28%29", "strict: !'() escape, *-._ do not");
}

{
    const d = formDecode("k=%41%42+%C3%A9");
    ok(d.k, "AB é", "decode %XX pairs, + as space, UTF-8 bytes reassembled");
    const m = formDecode("k=100%+no%zz%X");
    ok(m.k, "100%+no%zz%X" .replace("+", " "), "malformed escapes stay literal");
}

{
    // WHATWG urlencoded serializer: ~ stays verbatim (Node URLSearchParams
    // parity: new URLSearchParams([["k","1~2"]]).toString() === "k=1~2").
    ok(new URLSearchParams([["k", "1~2"]]).toString(), "k=1~2",
       "WHATWG urlencoded: ~ verbatim");
    // A name that itself contains '=' must escape it %3D, ~ stays verbatim.
    ok(new URLSearchParams([["k=1~2", "x"]]).toString(), "k%3D1~2=x",
       "WHATWG urlencoded: = is %3D, ~ verbatim");
    ok(new URLSearchParams([["k", "a\"b#c<d>e"]]).toString(),
       "k=a%22b%23c%3Cd%3Ee", "WHATWG: \" # < > escape");
    ok(new URLSearchParams([["k", " "]]).toString(), "k=+", "WHATWG: space is +");
}

{
    const s = "héllo wörld *-._!~'()#?&=%+/ \t\x7f";
    const enc = formEncode({ k: s }).slice(2);
    ok(formDecode("k=" + enc).k, s, "formEncode/formDecode round-trip");
}

print("test_pct_core: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_pct_core: " + fails + " failures");
