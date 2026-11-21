// covers: dyna:log.Logger.child
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_child_properties");
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

__kitS.section("the grandchild carries the parent's base field", () => {
    const parent = new Logger({ dest: P("json/child.log").toString(),
                                timestamp: false, name: "p",
                                base: { env: "prod" } });
    const child = parent.child({ route: "/api" });
    const grand = child.child({ span: "s1" });
    grand.info("deep");
    parent.flush();
    const o = JSON.parse(lines("json/child.log")[0]);
    eq(o.env, "prod", "the grandchild carries the parent's base field");
    eq(o.route, "/api", "and the child's");
    eq(o.span, "s1", "and its own");
    eq(o.name, "p", "and the logger name");

    child.level = "trace";
    eq(child.level, "trace", "the child's level changed");
    eq(parent.level, "info", "the parent's did not");
    eq(grand.level, "info", "a level change never propagates sideways");
});
__kitS.end();

removeAll(P("."));
if (fails) {
    print("test_log_properties: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_properties failed");
}
print("test_log_properties: " + n + " assertions, 0 failures");
