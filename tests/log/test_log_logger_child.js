// covers: dyna:log.Logger.child
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_child");
import { Logger, Debug } from "dyna:log";
import { Path, makeTempDir, readFile, readDir, readLink, removeAll } from "dyna:file";

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

__kitS.section("a child inherits the level", () => {
    const log = new Logger({ level: "warn", name: "api", timestamp: false,
                             base: { pid: 1234 } });
    const child = log.child({ requestId: "abc" });
    eq(child.level, "warn", "a child inherits the level");
    eq(child.enabled("info"), false, "a child inherits the gate");
    eq(child.info({ x: 1 }, "suppressed"), undefined, "a child can emit");
    eq(child.warn({ x: 1 }, "emitted"), undefined, "a child emits at its level");
    child.level = "trace";
    eq(child.level, "trace", "a child's level can change");
    eq(log.level, "warn", "changing the child did NOT change the parent");
    const grand = child.child({ span: "s1" });
    eq(grand.level, "trace", "a grandchild inherits from its parent");
    eq(grand.info("nested child"), undefined, "a grandchild emits");
    eq(log.child().level, "warn", "child() with no fields works");
});

const TMP = makeTempDir("dyna-log-test-");

function lines(name) {
    const body = readFile(new Path(TMP + "/" + name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}

__kitS.section("from child", () => {
    const log = new Logger({ dest: TMP + "/child.log", timestamp: false, name: "p" });
    log.child({ route: "/x" }).info("from child");
    log.flush();
    const [line] = lines("child.log");
    assert(line.includes('"route":"/x"') && line.includes('"name":"p"'),
           "a child writes through the parent's dest: " + line);
});

__kitS.section("child opts override the level", () => {
    const log = new Logger({ dest: TMP + "/childlv.log", level: "warn",
                             timestamp: false });
    const child = log.child({ requestId: "r1" }, { level: "debug" });
    eq(child.level, "debug", "child opts override the level");
    eq(log.level, "warn", "the parent's level is untouched");
    child.debug("from the child");
    log.debug("from the parent");
    log.flush();
    const ls = lines("childlv.log");
    eq(ls.length, 1, "the child's debug passes, the parent's is filtered");
    assert(ls[0].includes('"level":"debug"'), "the emitted record is a debug record");
    assert(ls[0].includes('"requestId":"r1"'), "child fields still merge");
    assert(ls[0].includes('"msg":"from the child"'), "the child's line is the one emitted");
    eq(log.child({}).level, "warn", "child without opts inherits");
    eq(log.child({}, {}).level, "warn", "child with empty opts inherits");
    eq(child.child({}).level, "debug", "a grandchild inherits the override");
    throws(() => log.child({}, { level: "loud" }), "an unknown child level is refused");
    throws(() => log.child({}, { level: 7 }), "a non-string child level is refused");
    throws(() => log.child({}, "debug"), "non-object child options are refused");
});

function throwsMsg(fn, re, msg) {
    let m = "";
    try { fn(); } catch (e) { m = String(e.message); }
    assert(re.test(m), msg + " (got: " + (m || "no throw") + ")");
}

__kitS.section("ctor bag names the key AND the valid set", () => {
    throwsMsg(() => new Logger({ host: "p2" }),
              /^unknown option "host" \(valid: level, name, timestamp, format, pid, hostname, base, mkdir, buffer, sample, rollover, dest\)$/,
              "ctor bag names the key AND the valid set");
    throwsMsg(() => new Logger({ levle: "debug" }), /^unknown option "levle"/,
              "ctor bag rejects a second unknown key");
    assert(new Logger({ level: "debug", name: "s" }) instanceof Logger,
           "ctor bag with only valid keys is unchanged");
    assert(new Logger(null) instanceof Logger && new Logger() instanceof Logger,
           "null / absent bags still mean all defaults");
    throwsMsg(() => new Logger({ dest: TMP + "/r.log", rollover: { sise: 1e6 } }),
              /^unknown option "sise" \(valid: size, frequency, count, symlink\)$/,
              "rollover sub-bag is strict");
    assert(new Logger({ dest: TMP + "/r2.log",
                        rollover: { size: "1m", count: 3 } }) instanceof Logger,
           "valid rollover sub-bag is unchanged");
    const parent = new Logger({ name: "cc6", timestamp: false, dest: TMP + "/c.log" });
    throwsMsg(() => parent.child({ route: "/x" }, { levle: "debug" }),
              /^unknown option "levle" \(valid: level\)$/,
              "child opts bag is strict");
    assert(parent.child({ route: "/x" }, { level: "error" }) instanceof Logger,
           "valid child opts bag is unchanged");
    assert(new Logger(42) instanceof Logger, "a primitive bag reads as absent");
    { const sym = Symbol("s"); assert(new Logger({ name: "sym", [sym]: 1 }) instanceof Logger,
        "symbol keys are invisible to the check"); }
});
__kitS.end();

removeAll(TMP);

if (fails) {
    print("test_log: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log failed");
}
print("test_log: " + n + " assertions, 0 failures");
