// covers: dyna:config.TOML.stringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_toml_stringify_leak");
import { TOML } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function eqj(a, b, msg) {
    assert(JSON.stringify(a) === JSON.stringify(b), msg +
        " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}

__kitS.section("key with spaces", () => {
    for (let round = 0; round < 50; round++) {
        const doc = {};
        for (let i = 0; i < 40; i++)
            doc["key with spaces " + i] = { "sub.key": [i, "v".repeat(i * 16)], "ü": round };
        const text = TOML.stringify(doc);
        assert(text.indexOf('"key with spaces 39"') >= 0, "round " + round + ": quoted keys emitted");
        const back = TOML.parse(text);
        eqj(back, doc, "round " + round + ": stringify/parse round trip");
    }
    const mixed = TOML.stringify({ bare: 1, "needs quoting": 2 });
    assert(/^bare = 1/m.test(mixed), "bare keys stay bare");
    assert(/"needs quoting" = 2/.test(mixed), "others are quoted");
});
__kitS.end();

print("test_toml_leak: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed (LSan must be flat at exit)"));
if (fails) throw new Error(fails + " failures");
