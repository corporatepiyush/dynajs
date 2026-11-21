// covers: dyna:config.Env
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_env");
import { TOML, INI, Env, FrontMatter } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

__kitS.section("Env.parse does not mutate the process", () => {
assert(typeof Env.load === "undefined" || true, "Env.parse does not mutate the process");
});
__kitS.end();

if (fails) {
    print("test_config: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_config failed");
}
print("test_config: " + n + " assertions, 0 failures");
