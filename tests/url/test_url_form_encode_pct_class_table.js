// covers: dyna:url.formEncode
// covers: dyna:url.encodeURIComponentStrict
// covers: c:src/core/dyn-pct.h
// dyn_pct_encode_core picks the safe set from one 256-entry class table (bit 1:
// strict set, bit 2: form set). The oracle is the engine's own
// encodeURIComponent (a separate code path) adjusted by the documented rules:
// strict also escapes !'()~; form serialization escapes !'() and writes a space
// as '+'. Every code point up to U+02FF plus astral and BMP samples is checked.
import { formEncode, encodeURIComponentStrict } from "dyna:url";
import { suite } from "../lib/kit.js";

const t = suite("test_url_pct_class_table");
const esc = (c) => "%" + c.charCodeAt(0).toString(16).toUpperCase().padStart(2, "0");
const strictOracle = (s) => encodeURIComponent(s).replace(/[!'()~]/g, esc);
const formOracle = (s) => encodeURIComponent(s).replace(/[!'()]/g, esc).replace(/%20/g, "+");
const SAMPLES = [];
for (let c = 1; c < 0x300; c++) SAMPLES.push(String.fromCharCode(c));
SAMPLES.push("€", "￿", "\u{1f600}", "\u{10ffff}", "a b~c*d", "x!y'z(1)2", "");

t.test("encodeURIComponentStrict matches the oracle on every sample", ({ eq }) => {
    let bad = 0;
    for (const s of SAMPLES) {
        const got = encodeURIComponentStrict("k" + s + "v");
        const exp = strictOracle("k" + s + "v");
        if (got !== exp && bad++ < 5) eq(got, exp, `U+${(s.codePointAt(0) || 0).toString(16)}`);
    }
    eq(bad, 0, "mismatches");
});

t.test("formEncode keys and values match the form oracle", ({ eq }) => {
    let bad = 0;
    for (const s of SAMPLES) {
        const got = formEncode({ ["k" + s]: s + "v" });
        const exp = formOracle("k" + s) + "=" + formOracle(s + "v");
        if (got !== exp && bad++ < 5) eq(got, exp, `U+${(s.codePointAt(0) || 0).toString(16)}`);
    }
    eq(bad, 0, "mismatches");
});

t.test("URLSearchParams serialization uses the same form set", ({ eq }) => {
    const all = SAMPLES.join("");
    const sp = new URLSearchParams();
    sp.append(all, all);
    eq(sp.toString(), formOracle(all) + "=" + formOracle(all));
});

await t.run();
