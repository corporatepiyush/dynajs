// covers: dyna:url.formDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_form_decode_pct_core");
import { formEncode, formDecode, encodeURIComponentStrict,
         URLSearchParams } from "dyna:url";

let n = 0, fails = 0;
const ok = (got, want, what) => {
    n++;
    if (got !== want) { fails++; print("FAIL: " + what + "\n  got  " + JSON.stringify(got) + "\n  want " + JSON.stringify(want)); }
};

__kitS.section("AB é", () => {
    const d = formDecode("k=%41%42+%C3%A9");
    ok(d.k, "AB é", "decode %XX pairs, + as space, UTF-8 bytes reassembled");
    const m = formDecode("k=100%+no%zz%X");
    ok(m.k, "100%+no%zz%X" .replace("+", " "), "malformed escapes stay literal");
});
__kitS.end();

print("test_pct_core: " + n + " checks, " + fails + " failures");
if (fails) throw new Error("test_pct_core: " + fails + " failures");
