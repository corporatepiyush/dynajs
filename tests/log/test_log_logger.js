// covers: dyna:log.Logger
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger");
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

eq(new Logger({ timestamp: false }).level, "info", "the default level is info");

eq(new Logger().level, "info", "a Logger with no options at all still works");

{
    const log = new Logger({ level: "trace", name: "test", timestamp: false });
    for (const lv of ["trace", "debug", "info", "warn", "error", "fatal"]) {
        eq(typeof log[lv], "function", lv + " is a method");
        eq(log[lv]("plain message"), undefined, lv + "(msg) returns undefined");
        eq(log[lv]({ a: 1 }, "with fields"), undefined, lv + "(fields, msg)");
        eq(log[lv](), undefined, lv + "() with no arguments at all");
        eq(log[lv]({}), undefined, lv + "(fields) with no message");
        eq(log[lv](null), undefined, lv + "(null)");
        eq(log[lv](undefined, "msg"), undefined, lv + "(undefined, msg)");
    }
}

throws(() => new Logger({ level: "verbose" }), "an unknown level is refused");

eq(new Logger({ timestamp: "iso" }).level, "info", "timestamp: iso is accepted");

eq(new Logger({ timestamp: "epochMs" }).level, "info", "timestamp: epochMs is accepted");

eq(new Logger({ timestamp: "epoch" }).level, "info",
   "timestamp: epoch (the declared spelling) is accepted");

eq(new Logger({ timestamp: false }).level, "info", "timestamp: false is accepted");

const TMP = makeTempDir("dyna-log-test-");

__kitS.section("new Logger({ level }): unknown level", () => {
    const levelThrower = { toString() { throw new Error("boom-level"); } };
    let msg = "";

    msg = "";
    try { new Logger({ level: 42 }); } catch (e) { msg = e.message; }
    eq(msg, "new Logger({ level }): unknown level", "ctor refuses level: 42");
    msg = "";
    try { new Logger({ level: null }); } catch (e) { msg = e.message; }
    eq(msg, "new Logger({ level }): unknown level", "ctor refuses level: null");
    try { new Logger({ level: "verbose" }); }
    catch (e) { eq(e.message, "new Logger({ level }): unknown level", "ctor refuses unknown string levels"); }

    msg = "";
    try { new Logger({ level: levelThrower }); } catch (e) { msg = e.message; }
    eq(msg, "boom-level", "ctor level getter throw propagates the original error");
    msg = "";
    try { new Logger({ name: "x2" }).level = levelThrower; }
    catch (e) { msg = e.message; }
    eq(msg, "boom-level", "setter level throw propagates the original error");

    const coerced = new Logger({ level: { toString() { return "warn"; } } });
    eq(coerced.level, "warn", "ctor accepts an object that coerces to a level, like the setter");
    const viaSetter = new Logger({ name: "x2" });
    viaSetter.level = { toString() { return "error"; } };
    eq(viaSetter.level, "error", "setter accepts the same coercing object");

    eq(new Logger({ name: "a" }).level, "info", "absent level is the default");
    eq(new Logger({ level: undefined }).level, "info",
       "explicit undefined level is the default, like every optional option");
    eq(new Logger({ level: "warn" }).level, "warn", "good levels still work");
});

__kitS.section("buffer getter throw propagates the original error", () => {
    let msg = "";
    try { new Logger({ buffer: { valueOf() { throw new Error("boom-buffer"); } } }); }
    catch (e) { msg = e.message; }
    eq(msg, "boom-buffer", "buffer getter throw propagates the original error");
    msg = "";
    try { new Logger({ buffer: "64k" }); } catch (e) { msg = e.message; }
    eq(msg, "Logger buffer: use true, 0 or 1..1048576 bytes",
       "buffer that converts to NaN/out-of-range is refused, not ignored");
    eq(new Logger({ buffer: { valueOf() { return 4096; } }, name: "bc" }) instanceof Logger,
       true, "buffer accepts an object coercing to a byte count, like the setter's level");
    eq(new Logger({ buffer: true, name: "b1" }) instanceof Logger, true, "buffer: true builds");
    eq(new Logger({ buffer: 4096, name: "b2" }) instanceof Logger, true, "buffer: bytes builds");
    eq(new Logger({ buffer: 0, name: "b3" }) instanceof Logger, true, "buffer: 0 builds");
    eq(new Logger({ buffer: undefined, name: "b4" }) instanceof Logger, true, "buffer: undefined builds");
});
__kitS.end();

removeAll(TMP);

if (fails) {
    print("test_log: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log failed");
}
print("test_log: " + n + " assertions, 0 failures");
