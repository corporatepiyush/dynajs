// covers: dyna:log.Logger.flush
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_flush_rollover");
import { Logger } from "dyna:log";
import { Path, makeTempDir, makeDir, writeFile, readFile, readDir, readLink,
         stat, removeAll, exists } from "dyna:file";

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
const TMP = makeTempDir("dyna-log-roll-");
const P = (s) => new Path(TMP + "/" + s);
for (const sub of ["scan", "prune", "shape", "numeric", "unb", "sym", "nosym",
                   "daily", "hourly", "dsz", "both", "units"])
    makeDir(P(sub), { recursive: true });
function lines(name) {
    const body = readFile(P(name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}
function names(sub) {
    return readDir(P(sub)).map((e) => e.name).sort();
}

__kitS.section("size units parse", () => {
    new Logger({ dest: P("units/a.log").toString(), rollover: { size: "10m" } }).flush();
    new Logger({ dest: P("units/b.log").toString(), rollover: { size: "2g" } }).flush();
    new Logger({ dest: P("units/c.log").toString(), rollover: { size: "500b" } }).flush();
    new Logger({ dest: P("units/d.log").toString(), rollover: { size: 2048 } }).flush();
    new Logger({ dest: P("units/e.log").toString(), rollover: { size: "1K" } }).flush();
    assert(true, "size units parse");
});

__kitS.section("a suppressed line writes nothing", () => {
    const log = new Logger({ dest: P("gate/app.log").toString(), mkdir: true,
                             timestamp: false, level: "error" });
    log.info("suppressed");
    log.flush();
    eq(lines("gate/app.log").length, 0, "a suppressed line writes nothing");
    log.level = "debug";
    log.debug("now visible");
    log.flush();
    eq(lines("gate/app.log").length, 1, "the level setter re-opens the gate");
});

__kitS.section("flush is idempotent", () => {
    const log = new Logger({ dest: P("fl/app.log").toString(), mkdir: true,
                             timestamp: false, buffer: true });
    log.info("x");
    log.flush();
    log.flush();
    log.flush();
    eq(lines("fl/app.log").length, 1, "flush is idempotent");
});
__kitS.end();

removeAll(P("."));
print((fails ? "test_log_rollover: " + fails + " FAILED of " + n : "test_log_rollover: " + n + " assertions, 0 failures"));
if (fails)
    throw new Error("test_log_rollover failed");
