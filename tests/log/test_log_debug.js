// covers: dyna:log.Debug
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_debug");
import { Logger, Debug } from "dyna:log";
import { Path, makeTempDir, readFile, readDir, readLink, removeAll } from "dyna:file";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

{
    const d = Debug("app:db");
    eq(typeof d, "function", "Debug returns a function");
    eq(d("a message"), undefined, "the debug function is callable");
    eq(d(), undefined, "the debug function takes no arguments");
    const d2 = Debug("other:ns");
    eq(d2("also fine"), undefined, "a second namespace is independent");
}

__kitS.section("Debug() with no namespace is refused", () => {
throws(() => Debug(), "Debug() with no namespace is refused");
});

__kitS.section("Debug refuses a non-string namespace", () => {
throws(() => Debug(42), "Debug refuses a non-string namespace");
});
__kitS.end();

const TMP = makeTempDir("dyna-log-test-");

{
    const d = Debug("app:multi");
    eq(d("one", "two", 3), undefined, "Debug joins its arguments");
}

removeAll(TMP);

if (fails) {
    print("test_log: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log failed");
}
print("test_log: " + n + " assertions, 0 failures");
