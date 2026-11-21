// covers: dyna:log.Logger.enabled
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_enabled_properties");
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

__kitS.section("logger", () => {
    const levels = ["trace", "debug", "info", "warn", "error", "fatal", "silent"];
    const logs = levels.map((lv) => new Logger({ level: lv, timestamp: false }));
    for (let i = 0; i < levels.length; i++)
        eq(logs[i].level, levels[i], "logger " + i + " kept its own level");
    for (let i = 0; i < levels.length; i++) {
        let count = 0;
        for (const lv of ["trace", "debug", "info", "warn", "error", "fatal"])
            if (logs[i].enabled(lv)) count++;
        if (levels[i] === "silent")
            eq(count, 0, "silent enables nothing");
        else if (i <= 5)
            eq(count, 6 - i, "level " + levels[i] + " enables " + (6 - i) + " of six");
    }
});
__kitS.end();

removeAll(P("."));
if (fails) {
    print("test_log_properties: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_properties failed");
}
print("test_log_properties: " + n + " assertions, 0 failures");
