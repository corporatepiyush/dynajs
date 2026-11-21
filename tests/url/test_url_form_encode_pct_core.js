// covers: dyna:url.formEncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_form_encode_pct_core");
import { formEncode, formDecode, encodeURIComponentStrict,
         URLSearchParams } from "dyna:url";

let n = 0, fails = 0;
const ok = (got, want, what) => {
    n++;
    if (got !== want) { fails++; print("FAIL: " + what + "\n  got  " + JSON.stringify(got) + "\n  want " + JSON.stringify(want)); }
};

__kitS.section("formEncode code point U", () => {
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

    ok(encodeURIComponentStrict("a b"), "a%20b", "strict: space is %20 (dynajs.d.ts: like encodeURIComponent)");
    ok(encodeURIComponentStrict("!*-._'()"), "%21*-._%27%28%29", "strict: !'() escape, *-._ do not");
});

__kitS.section("héllo wörld *-._!~'()#?&=%+/ tx7f", () => {
    const s = "héllo wörld *-._!~'()#?&=%+/ \t\x7f";
    const enc = formEncode({ k: s }).slice(2);
    ok(formDecode("k=" + enc).k, s, "formEncode/formDecode round-trip");
});
__kitS.end();

print("test_pct_core: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_pct_core: " + fails + " failures");
