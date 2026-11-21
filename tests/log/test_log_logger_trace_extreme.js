// covers: dyna:log.Logger.trace
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_trace_extreme");
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

__kitS.section("enabled lines", () => {
    for (const lv of ["trace", "debug", "info", "warn", "error", "fatal", "silent"]) {
        const log = new Logger({ dest: P("torture/gate" + lv + ".log").toString(),
                                 mkdir: true, timestamp: false, level: lv });
        let touched = 0;
        const trap = { get x() { touched++; return 1; } };
        log.trace(trap, "t"); log.debug(trap, "d"); log.info(trap, "i");
        log.warn(trap, "w"); log.error(trap, "e"); log.fatal(trap, "f");
        log.flush();
        const ls = lines("torture/gate" + lv + ".log");
        const levels = ["trace", "debug", "info", "warn", "error", "fatal"];
        const cutoff = levels.indexOf(lv);
        const expected = cutoff < 0 ? 0 : 6 - cutoff;
        eq(ls.length, expected, lv + ": " + expected + " enabled lines");
        if (lv === "silent") eq(touched, 0, "silent touches nothing");
    }
});
__kitS.end();

removeAll(P("."));
if (fails) {
    print("test_log_extreme: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_extreme failed");
}
print("test_log_extreme: " + n + " assertions, 0 failures");
