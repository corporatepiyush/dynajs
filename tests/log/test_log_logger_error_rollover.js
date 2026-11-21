// covers: dyna:log.Logger.error
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_error_rollover");
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

__kitS.section("cyclic error", () => {
    const log = new Logger({ dest: P("errc/app.log").toString(), mkdir: true,
                             timestamp: false });
    const e = new Error("cyc");
    e.self = e;
    e.detail = { deep: e };
    log.error(e, "cyclic error");
    const e2 = new Error("nostack");
    e2.stack = undefined;
    log.error(e2, "no stack");
    log.flush();
    const ls = lines("errc/app.log");
    eq(ls.length, 2, "both error lines landed");
    assert(ls[0].includes('"type":"Error"') && !ls[0].includes('"stack"') === false,
           "the cyclic error still carries its own stack");
    assert(!ls[1].includes('"stack":'), "stack:undefined is omitted");
});
__kitS.end();

removeAll(P("."));
print((fails ? "test_log_rollover: " + fails + " FAILED of " + n : "test_log_rollover: " + n + " assertions, 0 failures"));
if (fails)
    throw new Error("test_log_rollover failed");
