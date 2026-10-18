// timeout: 180
// Split from tests/test_data_w3_fixes.js: the yaml sections.
import { TOML } from "dyna:config";
import { Parse as YAMLParse } from "dyna:yaml";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
async function throws(fn, m) { n++; try { await fn(); fails++; print("FAIL: " + m + " (did not throw)"); } catch (e) { } }
function peq(text, expected, m) {
    n++;
    let got;
    try { got = TOML.parse(text).s; }
    catch (e) { fails++; print("FAIL: " + m + " (threw " + e + ")"); return; }
    check(got === expected, m + " -- got " + JSON.stringify(got) + ", want " + JSON.stringify(expected));
}
await throws(() => YAMLParse("{:a}"), "final-verify: {:a} is refused like PyYAML");
await throws(() => YAMLParse("{: a}"), "final-verify: {: a} is refused like PyYAML");
await throws(() => YAMLParse("{ : a }"), "final-verify: { : a } is refused like PyYAML");
eq(JSON.stringify(YAMLParse("{a:b}")), JSON.stringify({ "a:b": null }),
   "final-verify: the PyYAML-exact {a:b} rule still holds");
if (fails === 0) print("test_yaml_data_w3_fixes: all " + n + " checks passed");
else print("test_yaml_data_w3_fixes: " + fails + " FAILED of " + n);
