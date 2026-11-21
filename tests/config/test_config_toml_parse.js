// covers: dyna:config.TOML.parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_toml_parse");
import { TOML, INI, Env, FrontMatter } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

__kitS.section("uFEFFa = 1 [t] b = \"x\"", () => {
    const d = TOML.parse("\uFEFFa = 1\n[t]\nb = \"x\"\n");
    eq(d.a, 1, "TOML BOM-led document parses");
    eq(d.t.b, "x", "TOML BOM table content");
});
__kitS.end();

if (fails) {
    print("test_config: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_config failed");
}
print("test_config: " + n + " assertions, 0 failures");
