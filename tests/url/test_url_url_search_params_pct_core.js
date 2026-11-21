// covers: dyna:url.URLSearchParams
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_url_search_params_pct_core");
import { formEncode, formDecode, encodeURIComponentStrict,
         URLSearchParams } from "dyna:url";

let n = 0, fails = 0;
const ok = (got, want, what) => {
    n++;
    if (got !== want) { fails++; print("FAIL: " + what + "\n  got  " + JSON.stringify(got) + "\n  want " + JSON.stringify(want)); }
};

__kitS.section("WHATWG urlencoded: ~ verbatim", () => {
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
});
__kitS.end();

print("test_pct_core: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_pct_core: " + fails + " failures");
