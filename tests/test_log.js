/* test_log.js -- Logger and Debug in dyna:log (design 19).
 *
 * Lines go to stderr, which this process cannot read back, so the assertions
 * here are about the CONTRACT the caller can observe: the level gate, child
 * binding, and that nothing throws or hangs on the shapes that break loggers
 * (a cycle, a huge payload, an Error, a message with quotes and newlines).
 * The line FORMAT is checked by tests/test_log_format.sh, which captures fd 2.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_log.js
 */
import { Logger, Debug } from "dyna:log";

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

/* ------------------------------------------------------------- the gate */

{
    const log = new Logger({ level: "warn", timestamp: false });
    eq(log.level, "warn", "level reads back");
    eq(log.enabled("error"), true, "error is enabled at level warn");
    eq(log.enabled("warn"), true, "warn is enabled at level warn");
    eq(log.enabled("info"), false, "info is NOT enabled at level warn");
    eq(log.enabled("trace"), false, "trace is NOT enabled at level warn");
}
{
    const log = new Logger({ level: "trace", timestamp: false });
    for (const lv of ["trace", "debug", "info", "warn", "error", "fatal"])
        eq(log.enabled(lv), true, lv + " is enabled at level trace");
}
{
    const log = new Logger({ level: "silent", timestamp: false });
    for (const lv of ["trace", "info", "error", "fatal"])
        eq(log.enabled(lv), false, lv + " is disabled at level silent");
}
/* The default level is info, which is what a service wants without saying so. */
eq(new Logger({ timestamp: false }).level, "info", "the default level is info");
eq(new Logger().level, "info", "a Logger with no options at all still works");

/* The level is settable at runtime -- a service turns debug on without a
 * restart, and that is the whole reason it is a setter and not a constructor
 * argument only. */
{
    const log = new Logger({ level: "info", timestamp: false });
    eq(log.enabled("debug"), false, "debug off before the change");
    log.level = "debug";
    eq(log.level, "debug", "the level changed");
    eq(log.enabled("debug"), true, "debug on after the change");
}

/* ------------------------------------------------------------- emitting */

/* Every level method exists, accepts every documented shape, and returns
 * undefined. A logger that throws takes down the thing it was meant to
 * observe, so the surface here is deliberately total. */
{
    const log = new Logger({ level: "trace", name: "test", timestamp: false });
    for (const lv of ["trace", "debug", "info", "warn", "error", "fatal"]) {
        eq(typeof log[lv], "function", lv + " is a method");
        eq(log[lv]("plain message"), undefined, lv + "(msg) returns undefined");
        eq(log[lv]({ a: 1 }, "with fields"), undefined, lv + "(fields, msg)");
        eq(log[lv](), undefined, lv + "() with no arguments at all");
        eq(log[lv]({}), undefined, lv + "(fields) with no message");
        eq(log[lv](null), undefined, lv + "(null)");
        eq(log[lv](undefined, "msg"), undefined, lv + "(undefined, msg)");
    }
}

/* A message below the level must do NO work -- including not serializing the
 * field object. This is observable: a getter that throws must never run. */
{
    const log = new Logger({ level: "error", timestamp: false });
    let touched = false;
    const trap = { get boom() { touched = true; return 1; } };
    log.info(trap, "suppressed");
    eq(touched, false, "a suppressed line does not serialize its fields");
    log.error(trap, "emitted");
    eq(touched, true, "an emitted line DOES serialize its fields");
}

/* An Error is serialized structurally, not as JSON.stringify's `{}`. */
{
    const log = new Logger({ level: "trace", timestamp: false });
    eq(log.error(new Error("boom"), "failed"), undefined, "logging an Error");
    eq(log.error(new TypeError("bad type")), undefined, "an Error with no message arg");
    const e = new Error("outer");
    e.code = "ENOENT";
    eq(log.error(e, "with a code"), undefined, "an Error carrying extra fields");
}

/* THE SHAPES THAT BREAK LOGGERS. None of these may throw or hang. */
{
    const log = new Logger({ level: "trace", timestamp: false });
    const cyclic = { a: 1 };
    cyclic.self = cyclic;
    eq(log.info(cyclic, "cyclic"), undefined, "a cyclic object does not hang");
    /* A DAG that repeats a node is legal and must serialise, not be called a
     * cycle -- the check is against the ancestor chain, not everything seen. */
    const shared = { v: 1 };
    eq(log.info({ l: shared, r: shared }, "dag"), undefined,
       "a repeated non-cyclic node is not mistaken for a cycle");

    const deep = { v: 0 };
    let cur = deep;
    for (let i = 0; i < 2000; i++) { cur.next = { v: i }; cur = cur.next; }
    eq(log.info(deep, "deep"), undefined, "a 2000-deep object does not crash");

    const huge = { blob: "x".repeat(500000) };
    eq(log.info(huge, "huge"), undefined, "a 500 KB payload is truncated, not fatal");

    eq(log.info({ q: 'has "quotes" and \n newlines \t tabs' }, "escaping"),
       undefined, "quotes, newlines and tabs in a field");
    eq(log.info('msg with "quotes" and \n a newline'), undefined,
       "quotes and newlines in the message itself");
    eq(log.info({ "\u0000ctl": "\u0001\u001F" }, "control chars"), undefined,
       "control characters in a key and a value");
    eq(log.info({ u: "\u4F60\u597D \u{1f600}" }, "unicode"), undefined,
       "non-ASCII and astral characters");
    eq(log.info({ nested: { a: [1, 2, { b: null }] } }, "nested"), undefined,
       "nested structures");
    eq(log.info({ n: NaN, i: Infinity, neg: -0 }, "odd numbers"), undefined,
       "NaN, Infinity and -0 in fields");
}

/* --------------------------------------------------------------- child */

{
    const log = new Logger({ level: "warn", name: "api", timestamp: false,
                             base: { pid: 1234 } });
    const child = log.child({ requestId: "abc" });
    eq(child.level, "warn", "a child inherits the level");
    eq(child.enabled("info"), false, "a child inherits the gate");
    eq(child.info({ x: 1 }, "suppressed"), undefined, "a child can emit");
    eq(child.warn({ x: 1 }, "emitted"), undefined, "a child emits at its level");
    /* A child's level is independent once changed -- it is a separate object. */
    child.level = "trace";
    eq(child.level, "trace", "a child's level can change");
    eq(log.level, "warn", "changing the child did NOT change the parent");
    /* Children nest. */
    const grand = child.child({ span: "s1" });
    eq(grand.level, "trace", "a grandchild inherits from its parent");
    eq(grand.info("nested child"), undefined, "a grandchild emits");
    eq(log.child().level, "warn", "child() with no fields works");
}

/* ------------------------------------------------------------- refusals */

throws(() => new Logger({ level: "verbose" }), "an unknown level is refused");
{
    const log = new Logger({ timestamp: false });
    throws(() => { log.level = "loud"; }, "setting an unknown level is refused");
    throws(() => log.enabled("loud"), "enabled() refuses an unknown level");
    throws(() => log.enabled(42), "enabled() refuses a non-string");
}
/* Both timestamp shapes are accepted. */
eq(new Logger({ timestamp: "iso" }).level, "info", "timestamp: iso is accepted");
eq(new Logger({ timestamp: "epochMs" }).level, "info", "timestamp: epochMs is accepted");
eq(new Logger({ timestamp: false }).level, "info", "timestamp: false is accepted");

/* ---------------------------------------------------------------- Debug */

{
    const d = Debug("app:db");
    eq(typeof d, "function", "Debug returns a function");
    eq(d("a message"), undefined, "the debug function is callable");
    eq(d(), undefined, "the debug function takes no arguments");
    /* Whether it prints depends on DEBUG in the environment; either way it
     * must not throw, because that is the one thing a disabled logger can do
     * to a caller who never checks. */
    const d2 = Debug("other:ns");
    eq(d2("also fine"), undefined, "a second namespace is independent");
}
throws(() => Debug(), "Debug() with no namespace is refused");
throws(() => Debug(42), "Debug refuses a non-string namespace");

if (fails) {
    print("test_log: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log failed");
}
print("test_log: " + n + " assertions, 0 failures");
