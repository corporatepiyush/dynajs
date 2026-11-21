// covers: dyna:log.Logger.info
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_info");
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

__kitS.section("a suppressed line does not serialize its fields", () => {
    const log = new Logger({ level: "error", timestamp: false });
    let touched = false;
    const trap = { get boom() { touched = true; return 1; } };
    log.info(trap, "suppressed");
    eq(touched, false, "a suppressed line does not serialize its fields");
    log.error(trap, "emitted");
    eq(touched, true, "an emitted line DOES serialize its fields");
});

__kitS.section("a cyclic object does not hang", () => {
    const log = new Logger({ level: "trace", timestamp: false });
    const cyclic = { a: 1 };
    cyclic.self = cyclic;
    eq(log.info(cyclic, "cyclic"), undefined, "a cyclic object does not hang");
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
});

const TMP = makeTempDir("dyna-log-test-");

function lines(name) {
    const body = readFile(new Path(TMP + "/" + name));
    return body.length ? body.split("\n").filter((l) => l.length) : [];
}

__kitS.section("real message", () => {
    const log = new Logger({ dest: TMP + "/frame.log", name: "f" });
    log.info({ level: "debug", msg: "forged" }, "real message");
    log.flush();
    const [line] = lines("frame.log");
    assert(line.startsWith('{"time":'), "the frame's time opens the line");
    assert(line.includes('"level":"info","name":"f"'), "level and name intact");
    assert(line.includes('"msg":"real message"'), "the real message wins");
});

__kitS.section("ts off", () => {
    const log = new Logger({ dest: TMP + "/frame2.log", timestamp: false });
    log.info({ time: 12345 }, "ts off");
    log.flush();
    const [line] = lines("frame2.log");
    assert(line.includes('"time":12345'), "with timestamp:false, time is the caller's");
});

__kitS.section("pid and hostname follow the level:", () => {
    const log = new Logger({ dest: TMP + "/rich.log", timestamp: false,
                             pid: true, hostname: true });
    log.info("x");
    log.flush();
    const [line] = lines("rich.log");
    assert(/^\{"level":"info","pid":\d+,"hostname":"[^"]+","msg":"x"\}$/.test(line),
           "pid and hostname follow the level: " + line);
});

__kitS.section("a buffered line is not on disk yet", () => {
    const log = new Logger({ dest: TMP + "/buf.log", timestamp: false, buffer: true });
    log.info("held");
    eq(lines("buf.log").length, 0, "a buffered line is not on disk yet");
    log.flush();
    eq(lines("buf.log").length, 1, "flush() lands the buffered line");
    log.info("second");
    log.info("third");
    log.flush();
    eq(lines("buf.log").length, 3, "flush() lands every buffered line");
});

__kitS.section("padding padding", () => {
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
    const last = readFile(new Path(TMP + "/rot/" + numbered[2]));
    assert(last.includes("line-11"), "the highest number holds the newest lines");
});

__kitS.section("first period", () => {
    const log = new Logger({ dest: TMP + "/freq/app.log", mkdir: true,
                             timestamp: false, rollover: { frequency: 60 } });
    log.info("first period");
    log.flush();
    const t0 = Date.now();
    while (Date.now() - t0 < 120)
        ;
    log.info("second period");
    log.flush();
    const files = readDir(new Path(TMP + "/freq")).map((e) => e.name).sort();
    assert(files.length >= 2, "the period boundary rotated the file");
    assert(readFile(new Path(TMP + "/freq/" + files[0])).includes("first period"),
           "the earlier period's file keeps its lines");
});

__kitS.section("logged in", () => {
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
});

__kitS.section("DEL and U+2028/U+2029 round-trip through JSON.parse", () => {
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
});
__kitS.end();

removeAll(TMP);

if (fails) {
    print("test_log: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log failed");
}
print("test_log: " + n + " assertions, 0 failures");
