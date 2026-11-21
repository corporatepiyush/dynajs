// covers: dyna:log.Logger.flush
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_flush");
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

const TMP = makeTempDir("dyna-log-test-");

eq(new Logger().flush(), undefined, "flush() works on a stderr logger");

__kitS.section("an unknown format is refused", () => {
    const log = new Logger({ timestamp: false });
    throws(() => new Logger({ format: "yaml" }), "an unknown format is refused");
    throws(() => new Logger({ timestamp: "unix" }),
           "an unknown timestamp is refused");
    throws(() => new Logger({ rollover: { size: 100 } }),
           "rollover without a dest is refused");
    throws(() => new Logger({ dest: TMP + "/sz.log", rollover: { size: "10x" } }),
           "an unknown size unit is refused");
    throws(() => new Logger({ dest: TMP + "/fq.log", rollover: { frequency: "weekly" } }),
           "an unknown frequency is refused");
    throws(() => new Logger({ dest: TMP + "/cnt.log", rollover: { count: -1 } }),
           "a negative count is refused");
    throws(() => new Logger({ dest: TMP + "/nope/deeper/x.log" }),
           "a missing directory is refused without mkdir");
    const hold = new Logger({ dest: TMP + "/shared.log", rollover: { size: 1000 } });
    throws(() => new Logger({ dest: TMP + "/shared.log", rollover: { size: 2000 } }),
           "a second Logger with different rollover options is refused");
    new Logger({ dest: TMP + "/shared.log", rollover: { size: 1000 } }).flush();
    hold.flush();
});
__kitS.end();

removeAll(TMP);

if (fails) {
    print("test_log: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log failed");
}
print("test_log: " + n + " assertions, 0 failures");
