// covers: dyna:log.Logger.flush
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_flush_properties");
import { Logger, Debug } from "dyna:log";
import { Path, makeTempDir, makeDir, readFile, readDir, removeAll } from "dyna:file";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const TMP = makeTempDir("dyna-log-prop-");
const P = (s) => new Path(TMP + "/" + s);
for (const sub of ["json", "text", "rot", "rot2", "share"])
    makeDir(P(sub), { recursive: true });
function lines(name) {
    const body = readFile(P(name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}

__kitS.section("must not appear", () => {
    const log = new Logger({ dest: P("json/silent.log").toString(),
                             timestamp: false, level: "silent" });
    for (const lv of ["trace", "debug", "info", "warn", "error", "fatal"])
        log[lv]("must not appear");
    log.flush();
    eq(lines("json/silent.log").length, 0, "silent writes nothing at all");
});
__kitS.end();

removeAll(P("."));
if (fails) {
    print("test_log_properties: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_properties failed");
}
print("test_log_properties: " + n + " assertions, 0 failures");
