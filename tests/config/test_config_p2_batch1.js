// flags: --std
// Split from tests/test_p2_batch1.js: the config sections.
import { TOML } from "dyna:config";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + " want " + JSON.stringify(b) + ")"); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); }
    }
}
{
    const o = { a: 1, b: "x" };
    const s = TOML.stringify(o);
    assert(s.includes("a = 1"), "TOML stringify normal");
    const parsed = TOML.parse("[a]\nx=1");
    assert(!Object.keys(parsed).includes("__toml_header") && !Object.keys(parsed.a).includes("__toml_header"), "TOML __toml_header not enumerable");
    assert(!TOML.stringify(parsed).includes("__toml_header"), "TOML stringify does not leak header");
}
if (fails) { print("test_config_p2_batch1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_config_p2_batch1 failed"); }
print("test_config_p2_batch1: " + n + " assertions, 0 failures");
