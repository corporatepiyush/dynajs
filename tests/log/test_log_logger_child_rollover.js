// covers: dyna:log.Logger.child
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_child_rollover");
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

__kitS.section("deep child", () => {
    const log = new Logger({ dest: P("fam/app.log").toString(), mkdir: true,
                             timestamp: false, name: "p" });
    log.child({ a: 1 }).child({ b: 2 }).info("deep child");
    log.flush();
    const [line] = lines("fam/app.log");
    assert(line.includes('"a":1') && line.includes('"b":2') && line.includes('"name":"p"'),
           "grandchild carries the whole chain: " + line);
});

__kitS.section("info api: hi pid", () => {
    const log = new Logger({ dest: P("text2/app.log").toString(), mkdir: true,
                             format: "text", timestamp: false, name: "api",
                             pid: true, base: { env: "dev" } });
    log.child({ route: "/x" }).info("hi");
    log.flush();
    const [line] = lines("text2/app.log");
    assert(line.startsWith("info  api: hi pid="), "name and pid land: " + line);
    assert(line.includes("env=dev route=/x"), "base then child fields: " + line);
});
__kitS.end();

removeAll(P("."));
print((fails ? "test_log_rollover: " + fails + " FAILED of " + n : "test_log_rollover: " + n + " assertions, 0 failures"));
if (fails)
    throw new Error("test_log_rollover failed");
