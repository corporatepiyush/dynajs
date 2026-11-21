// covers: dyna:log.Logger.trace
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_trace_properties");
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

__kitS.section("suppressed emits never touch the fields object", () => {
    const log = new Logger({ dest: P("json/gate.log").toString(),
                             timestamp: false, level: "error" });
    let touched = 0;
    const trap = { get boom() { touched++; return 1; } };
    log.trace(trap, "t");
    log.debug(trap, "d");
    log.info(trap, "i");
    eq(touched, 0, "suppressed emits never touch the fields object");
    eq(lines("json/gate.log").length, 0, "suppressed emits write nothing");
    log.error(trap, "e");
    eq(touched, 1, "an enabled emit walks the fields object once");
});
__kitS.end();

removeAll(P("."));
if (fails) {
    print("test_log_properties: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_properties failed");
}
print("test_log_properties: " + n + " assertions, 0 failures");
