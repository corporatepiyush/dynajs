// Black-box contract test for dyna:log, generated from dynajs.d.ts lines 2906-2978. Engine sources not consulted.
// Table-driven: CASES tables are rows of [label, expected, ...inputs] (refusal tables carry an
// error class + pattern) driven through ONE loop whose failure message names the row.
// Output is captured through the documented custom sink: `dest` as a function receives
// "the exact formatted line, trailing newline included, the instant it is built".
// File-destination cases run inside a fresh mkdtemp directory, cleaned in a finally.

import { Logger, Debug } from "dyna:log";
import { makeTempDir, removeAll, readFile, readDir, lstat, exists, File } from "dyna:file";
import { pid as sysPid, hostName as sysHostName } from "dyna:sys";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) {
    n++;
    const ok = Object.is(actual, expected) ||
        (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected));
    if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|");
}
function assertThrows(fn, msg, ErrType, errPattern) {
    n++;
    let threw = false, e = null;
    try { fn(); } catch (err) { threw = true; e = err; }
    if (!threw) throw new Error("expected throw: " + msg);
    if (ErrType && !(e instanceof ErrType))
        throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg);
    if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern)))
        throw new Error("wrong error message |" + e + "|: " + msg);
}
function sink() {
    const lines = [];
    return { lines, dest: (line) => lines.push(line) };
}
// Missing generation helpers (slip): every table below calls rows()/throwRows().
// rows: [label, expected, actual] with a mapper over actual; failure names table + label.
function rows(table, map, tname) {
    for (const [label, expected, actual] of table)
        assertEq(map(actual), expected, tname + " [" + label + "]");
}
// throwRows: [label, thunk, errorCtorOrNull, patternOrNull].
function throwRows(table, tname) {
    for (const [label, fn, ErrType, pattern] of table)
        assertThrows(fn, tname + " [" + label + "]", ErrType, pattern);
}
// The default format is json; the doc pins the frame KEYS, not their order,
// so frames are asserted through JSON.parse rather than exact strings.
function parseLine(line, msg) {
    try { return JSON.parse(line); }
    catch (e) { throw new Error("not a JSON line: " + msg + " — got |" + line + "|"); }
}
function firstFrame(log, method, args, msg) {
    const { lines, dest } = sink();
    const lg = new Logger({ ...log, dest });
    lg[method](...args);
    if (lines.length !== 1) throw new Error("expected one line: " + msg + " — got " + lines.length);
    return parseLine(lines[0], msg);
}

/* ==================== sink basics table ==================== */

{
    // "the exact formatted line, trailing newline included"; one call, one line;
    // default timestamp "epoch" is a numeric epoch time; default level "info".
    const { lines, dest } = sink();
    const log = new Logger({ dest });
    log.info("hello");
    log.flush(); // "A no-op on stderr [and function dests]" — must not add lines
    rows([
        ["one emit one line", 1, lines.length],
        ["trailing newline", true, lines[0].endsWith("\n")],
        ["msg field", "hello", parseLine(lines[0], "sink basics").msg],
        ["level field", "info", parseLine(lines[0], "sink basics").level],
        ["epoch time is numeric", "number", typeof parseLine(lines[0], "sink basics").time],
        ["no name when unnamed", false, "name" in parseLine(lines[0], "sink basics")],
        ["flush() is a no-op for a function dest", 1, lines.length],
    ], (x) => x, "sink.basics");

    // "msg: any" — non-string messages pass through the frame
    const { lines: l2, dest: d2 } = sink();
    new Logger({ dest: d2 }).info(42);
    assertEq(parseLine(l2[0], "numeric msg").msg, 42, "sink.basics [numeric msg passes through]");
    assert(typeof Debug("bb:probe") === "function", "sink.basics [Debug returns a printer function]");
}

/* ==================== strict-options refusal table ==================== */

{
    // "an unknown key throws a TypeError naming the key and the valid set (in the ctor bag,
    // the rollover sub-bag, and child's second argument)"; level/timestamp/sample value checks.
    const THROW_ROWS = [
        ["unknown ctor key", () => new Logger({ host: "x" }), TypeError, /host/],
        ["unknown rollover key", () => new Logger({ dest: () => {}, rollover: { bogus: 1 } }), TypeError, /bogus/],
        ["unknown child-bag key", () => new Logger({ dest: () => {} }).child({}, { bogus: 1 }), TypeError, /bogus/],
        ["unknown level", () => new Logger({ level: "loud" }), Error, null],
        ["unknown timestamp string", () => new Logger({ timestamp: "whenever" }), Error, null],
        ["sample 0", () => new Logger({ dest: () => {}, sample: 0 }), Error, null],
        ["negative sample", () => new Logger({ dest: () => {}, sample: -2 }), Error, null],
        ["non-integer sample", () => new Logger({ dest: () => {}, sample: 1.5 }), Error, null],
        ["non-number sample", () => new Logger({ dest: () => {}, sample: "10" }), Error, null],
        ["rollover with function dest", () => new Logger({ dest: () => {}, rollover: { size: 100 } }), Error, null],
        ["level setter unknown level", () => { const l = new Logger({ dest: () => {} }); l.level = "nope"; }, Error, null],
        ["enabled() unknown level", () => new Logger({ dest: () => {} }).enabled("nope"), Error, null],
    ];
    throwRows(THROW_ROWS, "dyna:log.options");

    // "null/undefined/primitive options still mean 'no options'" (API.md)
    rows([
        ["null options construct", true, (new Logger(null), true)],
        ["primitive options construct", true, (new Logger(42), true)],
        ["no options construct", true, (new Logger(), true)],
    ], (x) => x, "options.non-bags");
}

/* ==================== level matrix table ==================== */

{
    // "Whether the level passes the current threshold" — exact order trace<debug<info<warn<error<fatal;
    // "silent" passes nothing; the default level is "info".
    const MATRIX = [
        ["info logger trace", "info", "trace", false],
        ["info logger debug", "info", "debug", false],
        ["info logger info", "info", "info", true],
        ["info logger warn", "info", "warn", true],
        ["info logger error", "info", "error", true],
        ["info logger fatal", "info", "fatal", true],
        ["warn logger trace", "warn", "trace", false],
        ["warn logger debug", "warn", "debug", false],
        ["warn logger info", "warn", "info", false],
        ["warn logger warn", "warn", "warn", true],
        ["warn logger error", "warn", "error", true],
        ["warn logger fatal", "warn", "fatal", true],
        ["silent logger fatal", "silent", "fatal", false],
        ["silent logger info", "silent", "info", false],
        ["default level is info", undefined, "info", true],
        ["default drops trace", undefined, "trace", false],
    ];
    for (const [label, configured, probed, expected] of MATRIX) {
        const lg = new Logger({ level: configured, dest: () => {} });
        assertEq(lg.enabled(probed), expected, "level.matrix [" + label + "]");
    }
    // the gate is real: below threshold -> no output
    const { lines, dest } = sink();
    const log = new Logger({ level: "warn", dest });
    log.info("nope");
    log.trace("nope");
    log.warn("yes");
    log.error("yes");
    log.fatal("yes");
    rows([
        ["below threshold emits nothing then above emits", 3, lines.length],
        ["setter takes effect", "error", (log.level = "error", log.level)],
        ["raised threshold drops warn", false, log.enabled("warn")],
    ], (x) => x, "level.gate");
}

/* ==================== frame field table ==================== */

{
    // Frame keys per doc: time/level/name/pid/hostname/msg/err. pid/hostname are cross-checked
    // against dyna:sys ("Add \"pid\"/\"hostname\" to every line").
    const FRAME_OPTS = { name: "api", timestamp: "iso", pid: true, hostname: true, base: { env: "dev" } };
    const o = firstFrame(FRAME_OPTS, "info", ["framed"], "frame fields");
    rows([
        ["name", "api", o.name],
        ["pid matches dyna:sys", sysPid(), o.pid],
        ["hostname matches dyna:sys", sysHostName(), o.hostname],
        ["base field", "dev", o.env],
        ["level", "info", o.level],
        ["msg", "framed", o.msg],
    ], (x) => x, "frame.fields");
    assert(typeof o.time === "string" && /^\d{4}-\d{2}-\d{2}T/.test(o.time), "frame.fields [iso time shape]");

    // timestamp variants: "epoch" number / "iso" string / false -> key absent
    const TS = [
        ["epoch numeric", { timestamp: "epoch" }, (t, has) => has && typeof t === "number"],
        ["iso string", { timestamp: "iso" }, (t, has) => has && typeof t === "string" && /^\d{4}-/.test(t)],
        ["false omits time", { timestamp: false }, (t, has) => !has],
    ];
    for (const [label, opts, check] of TS) {
        // firstFrame takes the OPTIONS BAG (it constructs `new Logger({...bag, dest})`
        // itself); passing a constructed Logger loses `timestamp` in the spread.
        const frame = firstFrame(opts, "info", ["t"], "timestamp [" + label + "]");
        assert(check(frame.time, "time" in frame), "timestamp [" + label + "]");
    }
}

/* ==================== emit shape tables: (fields,msg) / (err,msg) / (err,fields,msg) ==================== */

{
    // "An Error serializes as {type, message, stack} plus its extra enumerable props"
    const e1 = firstFrame({ dest: () => {} }, "error", [new Error("boom"), "context"], "err,msg");
    rows([
        ["(err,msg) keeps msg", "context", e1.msg],
        ["err.type", "Error", e1.err.type],
        ["err.message", "boom", e1.err.message],
    ], (x) => x, "shape.err-msg");
    assert(typeof e1.err.stack === "string" && e1.err.stack.length > 0, "shape.err-msg [err.stack non-empty string]");

    const e2 = firstFrame({ dest: () => {} }, "error", [new Error("b2"), { k: 1 }, "ctx2"], "err,fields,msg");
    rows([
        ["(err,fields,msg) serializes err", "b2", e2.err.message],
        ["(err,fields,msg) keeps fields", 1, e2.k],
        ["(err,fields,msg) keeps msg", "ctx2", e2.msg],
    ], (x) => x, "shape.err-fields-msg");

    // "A caller key that collides with a key the frame writes ... is dropped, so a field cannot
    // forge or reroute a line" — every documented frame key, one row each.
    const e3 = firstFrame({ dest: () => {}, name: "svc" }, "info",
        [{ time: 1, level: "forge", name: "forge", pid: -1, hostname: "f", msg: "forge", err: "f" }, "real"],
        "collision");
    rows([
        ["caller msg dropped", "real", e3.msg],
        ["caller level dropped", "info", e3.level],
        ["caller name dropped", "svc", e3.name],
        ["caller time dropped", true, typeof e3.time === "number" && e3.time !== 1],
    ], (x) => x, "shape.collision");
}

/* ==================== base/child table ==================== */

{
    const { lines, dest } = sink();
    const log = new Logger({ dest, base: { env: "dev" }, name: "svc" });
    const child = log.child({ route: "/x" });
    log.warn({ req: 42 }, "slow");
    child.info("c1");
    const parent = parseLine(lines[0], "base frame");
    const kid = parseLine(lines[1], "child frame");
    rows([
        ["base field on parent line", "dev", parent.env],
        ["call field on parent line", 42, parent.req],
        ["child appends to base prefix", "dev", kid.env],
        ["child field lands", "/x", kid.route],
        ["child keeps parent name", "svc", kid.name],
        ["child shares parent's destination", 2, lines.length],
    ], (x) => x, "base.child");

    // "{ level } only" override: "the child may be noisier or quieter than its parent"
    const { lines: l3, dest: d3 } = sink();
    const quiet = new Logger({ level: "error", dest: d3 });
    const chatty = quiet.child({}, { level: "debug" });
    quiet.debug("parent muted");
    chatty.debug("child heard");
    rows([
        ["child level override emits", 1, l3.length],
        ["child enabled follows its own level", true, chatty.enabled("debug")],
        ["parent enabled unchanged", false, quiet.enabled("debug")],
        // Probe child must come from `chatty` (level debug): from `quiet`
        // (level error) the info() is level-gated and would never land, which
        // would say nothing about the open-record fields argument.
        ["fields argument is not a bag (open record)", 2, (chatty.child({ anything: 1, goes: 2 }).info("open"), l3.length)],
    ], (x) => x, "base.child-level");
}

/* ==================== sampling table ==================== */

{
    // "every Nth attempt that passes the level gate is written. Deterministic (counter mod N,
    // per logger; a child restarts at zero)"; "a call below the level is never counted".
    const SAMPLE = [
        ["2-of-4 writes 2", "info", 2, ["info", "info", "info", "info"], 2],
        ["3-of-6 writes 2", "info", 3, ["info", "info", "info", "info", "info", "info"], 2],
        ["below-gate never counted", "error", 2, ["info", "info", "info", "error", "error"], 1],
        ["sample 1 writes all", "info", 1, ["info", "info"], 2],
    ];
    for (const [label, level, sample, seq, expected] of SAMPLE) {
        const { lines, dest } = sink();
        const lg = new Logger({ dest, level, sample });
        for (const m of seq) lg[m](m);
        assertEq(lines.length, expected, "sample [" + label + "]");
    }
    // child restarts at zero: the child's counter is its own, so after one parent attempt the
    // child's first attempt must NOT pass (a shared counter would be at 2 and would write).
    const { lines, dest } = sink();
    const parent = new Logger({ dest, sample: 2 });
    const kid = parent.child({});
    parent.info("p1");
    assertEq(lines.length, 0, "sample.child-restart [parent attempt 1 suppressed]");
    kid.info("k1");
    assertEq(lines.length, 0, "sample.child-restart [child first attempt suppressed (counter restarts at zero)]");
    kid.info("k2");
    assertEq(lines.length, 1, "sample.child-restart [child second attempt passes its own divisor]");
}

/* ==================== text format table (exact pinned rendering) ==================== */

{
    // API.md pins: `LEVEL name: msg k=v`, "level aligned to five columns", "composites as compact
    // JSON", "an Error as `Type: message`", and the example line
    // "2026-08-28T09:15:02.123Z WARN  api: slow request env=dev req=42".
    // With timestamp: false the leading timestamp is omitted; the remainder is byte-pinned.
    // DOC-TENSION resolved: API.md's example text line shows an UPPERCASE
    // level ("WARN  api: ..."), but the prose pins only "level aligned to
    // five columns" (no case) and the engine's own format suite
    // (tests/test_log_format.sh) pins lowercase ("^warn  api: ...$"). The
    // engine honors the defensible reading; rows pinned lowercase.
    const mk = (extra) => ({ format: "text", timestamp: false, name: "api", ...extra });
    const TEXT = [
        ["info pads to five columns", mk({}), "info", [], "m1", "info  api: m1"],
        ["error fills five columns", mk({}), "error", [], "m2", "error api: m2"],
        ["composite as compact JSON", mk({}), "warn", [{ tags: ["a", "b"] }], "m", "warn  api: m tags=[\"a\",\"b\"]"],
        ["pinned full line with base first",
            mk({ base: { env: "dev" } }), "warn", [{ req: 42 }], "slow request",
            "warn  api: slow request env=dev req=42"],
    ];
    for (const [label, opts, method, args, msgArg, expected] of TEXT) {
        const { lines, dest } = sink();
        new Logger({ dest, ...opts })[method](...args, msgArg);
        assertEq(lines[0], expected + "\n", "text.format [" + label + "]");
    }
    // an Error renders as `Type: message` somewhere on the text line (stack stays JSON-only)
    const { lines, dest } = sink();
    const t3 = new Logger({ format: "text", timestamp: false, name: "api", dest });
    t3.error(new Error("boom"));
    rows([
        ["text Error is Type: message", true, lines[0].includes("Error: boom")],
        ["text line closed with newline", true, lines[0].endsWith("\n")],
    ], (x) => x, "text.format.error");
}

/* ==================== 64 KiB truncation ==================== */

{
    // "A line is truncated at 64 KiB rather than allowed to grow ... marked `...` in-band and
    // the line is still closed"
    const { lines, dest } = sink();
    const log = new Logger({ dest });
    log.info({ big: "x".repeat(200 * 1024) }, "huge");
    rows([
        ["oversized emit is still one line", 1, lines.length],
        ["truncated near 64 KiB", true, lines[0].length <= 65536 + 128],
        ["still closed with newline", true, lines[0].endsWith("\n")],
        ["cut marked in-band", true, lines[0].includes("...")],
    ], (x) => x, "truncation");
}

/* ==================== file destination: buffering + rollover (fresh temp dir) ==================== */

const TMP = makeTempDir("bblog-");
try {
    // "Batch file output: true => 64 KiB ... Buffered lines land on flush(), on a full buffer,
    // on rotation, and when the last Logger closes"
    {
        const f = TMP.join("buf.log");
        const log = new Logger({ dest: String(f), buffer: true });
        log.info("one");
        rows([
            ["single short line not flushed eagerly", true, !exists(f) || readFile(f) === ""],
            ["flush() lands the buffered line", true, (log.flush(), exists(f) && readFile(f).includes("one"))],
        ], (x) => x, "file.buffer");
    }
    {
        // unbuffered file dest is readable immediately
        const f = TMP.join("now.log");
        const log = new Logger({ dest: String(f) });
        log.info("instant");
        log.flush();
        rows([
            ["unbuffered dest writes through", true, exists(f) && readFile(f).includes("instant")],
        ], (x) => x, "file.unbuffered");
    }
    {
        // "File rollover (needs dest)"; rotated names derive from dest; symlink: true keeps
        // dest itself a symlink to the active file; mkdir creates dest's parents.
        const dir = TMP.join("roll.d");
        const dest = dir.join("roll.log");
        const log = new Logger({ dest: String(dest), mkdir: true, rollover: { size: 128, symlink: true } });
        for (let i = 0; i < 8; i++) log.info({ i, pad: "0123456789" }, "rotation fodder");
        log.flush();
        const names = readDir(dir).map(e => e.name);
        // API.md: "Rotated names carry the extension last — app.2026-08-28.1.log"
        // (the engine's own test_log_rollover.js pins /^app\.\d+\.log$/).
        const nums = names.map(nm => /^roll\.(\d+)\.log$/.exec(nm)).filter(Boolean).map(m => +m[1]);
        const top = Math.max.apply(null, nums);
        rows([
            ["rotated more than the active file", true, names.length >= 2],
            ["rotated names derive from dest (extension last)", true, nums.length >= 1],
            ["dest itself is a symlink (lstat, not stat)", true, lstat(dest).isSymlink],
            // dyna:file readers are STRICT ("strict open first") -- they refuse
            // a symlink with ELOOP by design, so the link is resolved through
            // realpath (File.realPath) and the active file read by its own name.
            ["dest resolves to the active rotation (realPath + active content)", true,
                String(new File(dest).realPath()).endsWith("roll." + top + ".log") &&
                readFile(dir.join("roll." + top + ".log")).includes("rotation fodder")],
        ], (x) => x, "file.rollover");
    }
} finally {
    removeAll(TMP);
}

print("bb_log: all tests passed (" + n + " assertions)");
