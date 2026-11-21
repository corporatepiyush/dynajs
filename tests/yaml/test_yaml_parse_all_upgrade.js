// covers: dyna:yaml.ParseAll
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse_all_upgrade");
import { Parse, ParseAll } from "dyna:yaml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, ctor, msg, needle) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof ctor,
        msg + " (" + ctor.name + ", got " + caught + ")");
    if (needle)
        assert(caught.message.indexOf(needle) >= 0,
            msg + " (message mentions " + needle + "): " + caught.message);
}

__kitS.section("--- 1 --- 2", () => {
assert(ParseAll("--- 1\n--- 2\n").length === 2, "ParseAll unchanged");
});
__kitS.end();

print("test_yaml_upgrade: all tests passed (" + n + " assertions)");
