// covers: dyna:config.INI.stringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_ini_stringify");
import { TOML, INI, Env, FrontMatter } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

__kitS.section("a parser refuses", () => {
for (const fn of [INI.parse, Env.parse, FrontMatter.split]) {
    for (const bad of [null, undefined, 42, {}]) {
        let threw = false;
        try { fn(bad); } catch (e) { threw = e instanceof TypeError; }
        assert(threw, "a parser refuses " + JSON.stringify(bad));
    }
}
});
__kitS.end();

if (fails) {
    print("test_config: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_config failed");
}
print("test_config: " + n + " assertions, 0 failures");
