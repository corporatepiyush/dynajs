// covers: dyna:log.Logger.enabled
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_enabled");
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

__kitS.section("level reads back", () => {
    const log = new Logger({ level: "warn", timestamp: false });
    eq(log.level, "warn", "level reads back");
    eq(log.enabled("error"), true, "error is enabled at level warn");
    eq(log.enabled("warn"), true, "warn is enabled at level warn");
    eq(log.enabled("info"), false, "info is NOT enabled at level warn");
    eq(log.enabled("trace"), false, "trace is NOT enabled at level warn");
});

__kitS.section("is enabled at level trace", () => {
    const log = new Logger({ level: "trace", timestamp: false });
    for (const lv of ["trace", "debug", "info", "warn", "error", "fatal"])
        eq(log.enabled(lv), true, lv + " is enabled at level trace");
});

__kitS.section("is disabled at level silent", () => {
    const log = new Logger({ level: "silent", timestamp: false });
    for (const lv of ["trace", "info", "error", "fatal"])
        eq(log.enabled(lv), false, lv + " is disabled at level silent");
});

__kitS.section("debug off before the change", () => {
    const log = new Logger({ level: "info", timestamp: false });
    eq(log.enabled("debug"), false, "debug off before the change");
    log.level = "debug";
    eq(log.level, "debug", "the level changed");
    eq(log.enabled("debug"), true, "debug on after the change");
});

__kitS.section("setting an unknown level is refused", () => {
    const log = new Logger({ timestamp: false });
    throws(() => { log.level = "loud"; }, "setting an unknown level is refused");
    throws(() => log.enabled("loud"), "enabled() refuses an unknown level");
    throws(() => log.enabled(42), "enabled() refuses a non-string");
});
__kitS.end();

const TMP = makeTempDir("dyna-log-test-");

removeAll(TMP);

if (fails) {
    print("test_log: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log failed");
}
print("test_log: " + n + " assertions, 0 failures");
