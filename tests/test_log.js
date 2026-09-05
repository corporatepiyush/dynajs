/* test_log.js -- Logger and Debug in dyna:log.
 *
 * stderr is unreadable from within the process, so the format-sensitive
 * assertions live in two places: this file's FILE sections (a `dest` makes
 * every line readable back from JS) and tests/test_log_format.sh, which
 * captures fd 2 for the stderr contract. Between them the full line format,
 * the rollover machinery and the Debug matcher are covered.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_log.js
 */
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

/* ------------------------------------------------------------- destinations */

/* A `dest` turns stderr-lines into file-lines, which JS can read back: the
 * sections below assert the LINE CONTENT itself, not just the absence of
 * throw. Everything lives in a temp dir this test removes at the end. */
const TMP = makeTempDir("dyna-log-test-");
function lines(name) {
    const body = readFile(new Path(TMP + "/" + name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}

/* The frame is the logger's, not the caller's: a field named level/msg/time
 * must not be ABLE to clobber what a consumer parses to route a line. A key
 * the frame does not write stays free -- `time` is only reserved when a
 * timestamp is emitted. */
{
    const log = new Logger({ dest: TMP + "/frame.log", name: "f" });
    log.info({ level: "debug", msg: "forged" }, "real message");
    log.flush();
    const [line] = lines("frame.log");
    assert(line.startsWith('{"time":'), "the frame's time opens the line");
    assert(line.includes('"level":"info","name":"f"'), "level and name intact");
    assert(line.includes('"msg":"real message"'), "the real message wins");
}
{
    const log = new Logger({ dest: TMP + "/frame2.log", timestamp: false });
    log.info({ time: 12345 }, "ts off");
    log.flush();
    const [line] = lines("frame2.log");
    assert(line.includes('"time":12345'), "with timestamp:false, time is the caller's");
}

/* (err, fields, msg) -- an error's context rides in the same call. */
{
    const log = new Logger({ dest: TMP + "/errf.log", timestamp: false });
    const e = new Error("boom");
    e.code = "EPIPE";
    log.error(e, { reqId: "r1" }, "failed");
    log.flush();
    const [line] = lines("errf.log");
    assert(line.includes('"err":{"type":"Error","message":"boom"'), "err object leads");
    assert(line.includes('"code":"EPIPE"'), "error extra props ride along");
    assert(line.includes('"reqId":"r1"'), "the second object is fields");
    assert(line.includes('"msg":"failed"'), "the message is last");
}

/* pid/hostname enrichment is folded into the prefix at construction. */
{
    const log = new Logger({ dest: TMP + "/rich.log", timestamp: false,
                             pid: true, hostname: true });
    log.info("x");
    log.flush();
    const [line] = lines("rich.log");
    assert(/^\{"level":"info","pid":\d+,"hostname":"[^"]+","msg":"x"\}$/.test(line),
           "pid and hostname follow the level: " + line);
}

/* Buffering: `buffer: true` holds lines until flush (or a full buffer). */
{
    const log = new Logger({ dest: TMP + "/buf.log", timestamp: false, buffer: true });
    log.info("held");
    eq(lines("buf.log").length, 0, "a buffered line is not on disk yet");
    log.flush();
    eq(lines("buf.log").length, 1, "flush() lands the buffered line");
    log.info("second");
    log.info("third");
    log.flush();
    eq(lines("buf.log").length, 3, "flush() lands every buffered line");
}

/* Rollover by size, with retention and a symlink tracking the active file. */
{
    const log = new Logger({ dest: TMP + "/rot/app.log", mkdir: true,
                             timestamp: false,
                             rollover: { size: 100, count: 2, symlink: true } });
    for (let i = 0; i < 12; i++)
        log.info("line-" + i + " padding padding");
    log.flush();
    const entries = readDir(new Path(TMP + "/rot")).map((e) => e.name).sort();
    const numbered = entries.filter((f) => /^app\.\d+\.log$/.test(f));
    eq(numbered.length, 3, "count:2 keeps two rotated files plus the active one");
    assert(entries.includes("app.log"), "the requested path exists (as the symlink)");
    eq(readLink(new Path(TMP + "/rot/app.log")),
       numbered[numbered.length - 1], "the symlink tracks the active file");
    /* numbering never reuses a pruned number: the newest lines live in the
       highest number */
    const last = readFile(new Path(TMP + "/rot/" + numbered[2]));
    assert(last.includes("line-11"), "the highest number holds the newest lines");
}

/* Rollover by frequency (a number of milliseconds), date-less numbering. */
{
    const log = new Logger({ dest: TMP + "/freq/app.log", mkdir: true,
                             timestamp: false, rollover: { frequency: 60 } });
    log.info("first period");
    log.flush();
    const t0 = Date.now();
    while (Date.now() - t0 < 120)
        ;                                    /* cross the period boundary */
    log.info("second period");
    log.flush();
    const files = readDir(new Path(TMP + "/freq")).map((e) => e.name).sort();
    assert(files.length >= 2, "the period boundary rotated the file");
    assert(readFile(new Path(TMP + "/freq/" + files[0])).includes("first period"),
           "the earlier period's file keeps its lines");
}

/* A child shares the parent's destination. */
{
    const log = new Logger({ dest: TMP + "/child.log", timestamp: false, name: "p" });
    log.child({ route: "/x" }).info("from child");
    log.flush();
    const [line] = lines("child.log");
    assert(line.includes('"route":"/x"') && line.includes('"name":"p"'),
           "a child writes through the parent's dest: " + line);
}

/* child(fields, { level }): a child BORN at its own level. `level` as a
 * FIELD stays a reserved (dropped) frame key -- the override is an option,
 * so a field named level can never silently become one. */
{
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
    /* inheritance and refusal behave exactly like the constructor's */
    eq(log.child({}).level, "warn", "child without opts inherits");
    eq(log.child({}, {}).level, "warn", "child with empty opts inherits");
    eq(child.child({}).level, "debug", "a grandchild inherits the override");
    throws(() => log.child({}, { level: "loud" }), "an unknown child level is refused");
    throws(() => log.child({}, { level: 7 }), "a non-string child level is refused");
    throws(() => log.child({}, "debug"), "non-object child options are refused");
}

/* The text format: one line, aligned level, name, message, k=v fields.
   Base fields precede the call's fields -- the call comes last in time. */
{
    const log = new Logger({ dest: TMP + "/text.log", format: "text",
                             timestamp: false, name: "api", base: { env: "dev" } });
    log.info({ userId: 42 }, "logged in");
    log.warn({ nested: { a: [1] } }, "careful");
    log.info("plain");
    log.flush();
    const ls = lines("text.log");
    eq(ls[0], "info  api: logged in env=dev userId=42",
       "text line: level, name, message, then fields");
    assert(ls[1].startsWith("warn  api: careful env=dev nested={\"a\":[1]}"),
           "composites render as compact JSON: " + ls[1]);
    eq(ls[2], "info  api: plain env=dev",
       "no trailing whitespace when fields end the line");
}

/* An Error in the text format prints `Type: message`; the stack stays in JSON. */
{
    const log = new Logger({ dest: TMP + "/texterr.log", format: "text",
                             timestamp: false });
    log.error(new TypeError("bad type"), "failed");
    log.flush();
    const [line] = lines("texterr.log");
    eq(line, "error failed TypeError: bad type", "error as `Type: message`");
}

/* DEL and U+2028/U+2029 are valid JSON but hostile to consumers that embed a
 * log line in JS source: U+2028/U+2029 terminate a line literal (pre-ES2019
 * parsers) and DEL slips past byte-level filters. The JSON writer escapes
 * all three, so the line round-trips AND carries no raw such byte. */
{
    const log = new Logger({ dest: TMP + "/esc.log", timestamp: false });
    const msg = "del\u007fls\u2028ps\u2029end";
    log.info(msg);
    log.flush();
    const [line] = lines("esc.log");
    eq(JSON.parse(line).msg, msg,
       "DEL and U+2028/U+2029 round-trip through JSON.parse");
    assert(!line.includes("\u007f"), "no raw DEL byte reaches the file");
    assert(!line.includes("\u2028") && !line.includes("\u2029"),
           "no raw line separator reaches the file");
    assert(line.includes("\\u007f") && line.includes("\\u2028") &&
           line.includes("\\u2029"),
           "the three are escaped as \\uXXXX: " + line);
}

/* flush() on stderr is a no-op (stderr is unbuffered by contract). */
eq(new Logger().flush(), undefined, "flush() works on a stderr logger");

/* Refusals around destinations. */
{
    const log = new Logger({ timestamp: false });
    throws(() => new Logger({ format: "yaml" }), "an unknown format is refused");
    throws(() => new Logger({ timestamp: "unix" }),
           "an unknown timestamp is refused");
    throws(() => new Logger({ rollover: { size: 100 } }),
           "rollover without a dest is refused");
    throws(() => new Logger({ dest: TMP + "/sz.log", rollover: { size: "10x" } }),
           "an unknown size unit is refused");
    throws(() => new Logger({ dest: TMP + "/fq.log", rollover: { frequency: "weekly" } }),
           "an unknown frequency is refused");
    throws(() => new Logger({ dest: TMP + "/cnt.log", rollover: { count: -1 } }),
           "a negative count is refused");
    throws(() => new Logger({ dest: TMP + "/nope/deeper/x.log" }),
           "a missing directory is refused without mkdir");
    /* same path twice must agree on rollover behavior: silent divergence
       would corrupt both Loggers' rotation state */
    const hold = new Logger({ dest: TMP + "/shared.log", rollover: { size: 1000 } });
    throws(() => new Logger({ dest: TMP + "/shared.log", rollover: { size: 2000 } }),
           "a second Logger with different rollover options is refused");
    new Logger({ dest: TMP + "/shared.log", rollover: { size: 1000 } }).flush();
    hold.flush();
}

/* Debug: multiple arguments join with a space. */
{
    const d = Debug("app:multi");
    eq(d("one", "two", 3), undefined, "Debug joins its arguments");
}

removeAll(TMP);

if (fails) {
    print("test_log: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log failed");
}
print("test_log: " + n + " assertions, 0 failures");
