// covers: dyna:log.Logger.enabled
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_enabled_extreme");
import { Logger, Debug } from "dyna:log";
import { Path, makeTempDir, makeDir, writeFile, readFile, readDir,
         readLink, removeAll, exists } from "dyna:file";

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
function parseAll(lines) {
    const out = [];
    for (const l of lines) {
        let o = null;
        try { o = JSON.parse(l); } catch (e) { o = null; }
        assert(o !== null, "line parses: " + l.slice(0, 60));
        if (o) out.push(o);
    }
    return out;
}

const TMP = makeTempDir("dyna-log-extreme-");
const P = (s) => new Path(TMP + "/" + s);
for (const sub of ["best", "worst", "ugly", "torture"])
    makeDir(P(sub), { recursive: true });

function lines(name) {
    const body = readFile(P(name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}

__kitS.section("enabled(42) throws", () => {
    const log = new Logger({ timestamp: false });
    throws(() => log.enabled(42), "enabled(42) throws");
    throws(() => log.enabled(), "enabled() throws");
    throws(() => { log.level = 42; }, "level = 42 throws");
    throws(() => { log.level = "loud"; }, "level = loud throws");
    log.level = "silent";
    eq(log.level, "silent", "silent works");
    log.level = "trace";
    eq(log.level, "trace", "trace works");
});

__kitS.section("empty opts", () => {
    eq(new Logger({}).level, "info", "empty opts");
    eq(new Logger().enabled("warn"), true, "no opts");
});
__kitS.end();

removeAll(P("."));
if (fails) {
    print("test_log_extreme: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_extreme failed");
}
print("test_log_extreme: " + n + " assertions, 0 failures");
