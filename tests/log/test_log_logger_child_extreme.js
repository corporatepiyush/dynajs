// covers: dyna:log.Logger.child
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_child_extreme");
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

__kitS.section("iso time", () => {
    const log = new Logger({ dest: P("best/b.log").toString(), timestamp: "iso",
                             name: "api", pid: true, base: { env: "prod" } });
    log.child({ route: "/v1" }).info({ req: "r1" }, "served");
    log.flush();
    const o = JSON.parse(lines("best/b.log")[0]);
    assert(/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/.test(o.time), "iso time");
    eq(o.level, "info", "level");
    eq(o.name, "api", "name");
    eq(o.env, "prod", "base");
    eq(o.route, "/v1", "child field");
    eq(o.req, "r1", "call field");
    eq(o.msg, "served", "msg");
    assert(typeof o.pid === "number", "pid present");
});
__kitS.end();

removeAll(P("."));
if (fails) {
    print("test_log_extreme: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_extreme failed");
}
print("test_log_extreme: " + n + " assertions, 0 failures");
