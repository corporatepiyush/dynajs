/* test_log_rollover.js -- the FILE side of dyna:log: destinations, buffering,
 * rollover (size/frequency/count/symlink), the text format's exact bytes, and
 * the numeric/unicode edges of both renderers. Line content is asserted from
 * JS through a `dest`, which stderr cannot offer; the stderr contract itself
 * stays in tests/test_log_format.sh.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_log_rollover.js
 */
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
/* every fixture directory the tests below pre-populate */
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

/* ========================================================== numbering ===== */

/* Numbering continues from the HIGHEST existing file, not the first free
 * slot: retention deletes old files, and reusing their numbers would make
 * "app.2" newer than "app.4" and break name-order == time-order. */
{
    writeFile(P("scan/app.1.log"), "old one\n");
    writeFile(P("scan/app.2.log"), "old two\n");
    const log = new Logger({ dest: P("scan/app.log").toString(), timestamp: false,
                             rollover: { size: 60 } });
    log.info("x");                       /* 1 line < 60 bytes: no rotation yet */
    log.info("y");
    log.info("z");
    log.flush();
    const fs = names("scan").filter((f) => /^app\.\d+\.log$/.test(f));
    assert(fs.includes("app.3.log"),
           "the first rotation opened .3, continuing the sequence: " + fs.join(","));
}

/* Retention applies AT OPEN: a restart inherits the policy without waiting
 * for a rotation. */
{
    for (const f of ["app.1.log", "app.2.log", "app.3.log", "app.4.log"])
        writeFile(P("prune/" + f), "old\n");
    const log = new Logger({ dest: P("prune/app.log").toString(), timestamp: false,
                             rollover: { size: 60, count: 2 } });
    log.info("fresh");
    log.flush();
    const fs = names("prune").filter((f) => /^app\.\d+\.log$/.test(f));
    eq(fs.length, 3, "count:2 kept two stale rotated files plus the fresh active one");
    assert(!fs.includes("app.1.log") && !fs.includes("app.2.log"),
           "the oldest went first: " + fs.join(","));
}

/* Retention matches by NAME SHAPE: other files on the same base are not ours
 * to delete. */
{
    writeFile(P("shape/app.1.log"), "x\n");
    writeFile(P("shape/app.notes.log"), "keep me\n");
    writeFile(P("shape/app.1.txt"), "keep me too\n");
    writeFile(P("shape/app.log"), "the dest itself\n");
    const log = new Logger({ dest: P("shape/app.log").toString(), timestamp: false,
                             rollover: { size: 60, count: 1 } });
    log.info("a");
    log.info("b");
    log.info("c");
    log.flush();
    assert(exists(P("shape/app.notes.log")), "a non-rotation sibling survives");
    assert(exists(P("shape/app.1.txt")), "a different extension survives");
    const fs = names("shape").filter((f) => /^app\.\d+\.log$/.test(f));
    eq(fs.length, 2, "count:1 keeps one rotated file plus the active one");
}

/* A pure-.N sequence sorts numerically: with 12 rotated files, count:2 keeps
 * .11 and .12, not .1 and .10. */
{
    const log = new Logger({ dest: P("numeric/app.log").toString(), timestamp: false,
                             rollover: { size: 40, count: 2 } });
    for (let i = 0; i < 14; i++)
        log.info("n" + i);
    log.flush();
    const fs = names("numeric").filter((f) => /^app\.\d+\.log$/.test(f))
                   .map((f) => parseInt(f.match(/(\d+)\.log$/)[1], 10))
                   .sort((a, b) => a - b);
    eq(fs.length, 3, "retention held the count through 12+ rotations");
    const top = fs[fs.length - 1];
    eq(fs.join(","), (top - 2) + "," + (top - 1) + "," + top,
       "the survivors are the three highest numbers, consecutive");
    assert(readFile(P("numeric/app." + top + ".log")).includes("n13"),
           "the highest number holds the newest lines");
}

/* count:0 (or absent) is unbounded. */
{
    const log = new Logger({ dest: P("unb/app.log").toString(), timestamp: false,
                             rollover: { size: 40, count: 0 } });
    for (let i = 0; i < 10; i++)
        log.info("u" + i);
    log.flush();
    const fs = names("unb").filter((f) => /^app\.\d+\.log$/.test(f));
    assert(fs.length >= 4, "several rotations happened: " + fs.join(","));
}

/* A symlink at open points at the PRE-EXISTING active file (restart case),
 * and stays correct through later rotations. */
{
    writeFile(P("sym/app.1.log"), "old active\n");
    const log = new Logger({ dest: P("sym/app.log").toString(), timestamp: false,
                             rollover: { size: 60, symlink: true } });
    log.info("a");
    log.info("b");
    log.info("c");
    log.flush();
    const fs = names("sym").filter((f) => /^app\.\d+\.log$/.test(f));
    eq(readLink(P("sym/app.log")), fs[fs.length - 1],
       "the symlink ends on the newest file");
    assert(readFile(P("sym/" + fs[fs.length - 1])).includes("c"),
           "the active file holds the newest lines");
}

/* symlink without any rollover is ignored: dest is the file itself. */
{
    const log = new Logger({ dest: P("nosym/app.log").toString(), timestamp: false,
                             symlink: true });
    log.info("plain file");
    log.flush();
    assert(stat(P("nosym/app.log")).isFile && !stat(P("nosym/app.log")).isSymlink,
           "without rollover the dest is a regular file");
}

/* ================================================== date-named rotation === */

/* "daily" names the file with the UTC date and REUSES it across opens. */
{
    const d = new Date();
    const want = d.getUTCFullYear() + "-" +
        String(d.getUTCMonth() + 1).padStart(2, "0") + "-" +
        String(d.getUTCDate()).padStart(2, "0");
    const log = new Logger({ dest: P("daily/app.log").toString(), timestamp: false,
                             rollover: { frequency: "daily" } });
    log.info("one");
    log.flush();
    const fs = names("daily");
    eq(fs.join(","), "app." + want + ".log", "the daily file carries the UTC date");
    /* a second open in the same period appends to the SAME file */
    const log2 = new Logger({ dest: P("daily/app.log").toString(), timestamp: false,
                              rollover: { frequency: "daily" } });
    log2.info("two");
    log2.flush();
    eq(names("daily").length, 1, "the current period's file is reused, not duplicated");
    const body = readFile(P("daily/app." + want + ".log"));
    assert(body.includes("one") && body.includes("two"), "both opens appended");
}

/* "hourly" names the hour too. */
{
    const d = new Date();
    const want = d.getUTCFullYear() + "-" +
        String(d.getUTCMonth() + 1).padStart(2, "0") + "-" +
        String(d.getUTCDate()).padStart(2, "0") + "T" +
        String(d.getUTCHours()).padStart(2, "0");
    const log = new Logger({ dest: P("hourly/app.log").toString(), timestamp: false,
                             rollover: { frequency: "hourly" } });
    log.info("h");
    log.flush();
    eq(names("daily").length >= 0 && names("hourly").join(","),
       "app." + want + ".log", "the hourly name carries date and hour");
}

/* daily + size: the date segment stays, N restarts at 1 for the period. */
{
    const d = new Date();
    const date = d.getUTCFullYear() + "-" +
        String(d.getUTCMonth() + 1).padStart(2, "0") + "-" +
        String(d.getUTCDate()).padStart(2, "0");
    const log = new Logger({ dest: P("dsz/app.log").toString(), timestamp: false,
                             rollover: { frequency: "daily", size: 60 } });
    for (let i = 0; i < 5; i++)
        log.info("dsz padding padding");
    log.flush();
    const fs = names("dsz");
    assert(fs.includes("app." + date + ".1.log"),
           "daily+size names are app.date.1.log: " + fs.join(","));
    assert(fs.length >= 2, "the size trigger rotated within the period");
}

/* frequency (ms) + size: either trigger rotates. */
{
    const log = new Logger({ dest: P("both/app.log").toString(), timestamp: false,
                             rollover: { frequency: 3600000, size: 50 } });
    for (let i = 0; i < 4; i++)
        log.info("both padding padding");
    log.flush();
    const fs = names("both").filter((f) => /^app\.\d+\.log$/.test(f));
    assert(fs.length >= 2, "the size trigger fired within the period: " + fs.join(","));
}

/* ============================================================== buffering = */

/* A numeric buffer flushes on overflow without any explicit flush(). */
{
    const log = new Logger({ dest: P("ovf/app.log").toString(), mkdir: true,
                             timestamp: false, buffer: 16 });
    log.info("aaaaaaaaaaaaaaaa");        /* fills the 16 bytes */
    eq(lines("ovf/app.log").length, 1, "a full buffer lands on disk by itself");
    log.info("b");                       /* spills the next one */
    log.info("c");
    log.flush();
    eq(lines("ovf/app.log").length, 3, "overflow then flush lands every line");
}

/* One giant line bypasses the buffer whole -- it is never split mid-line. */
{
    const log = new Logger({ dest: P("giant/app.log").toString(), mkdir: true,
                             timestamp: false, buffer: 64 });
    log.info("G".repeat(200));
    log.flush();
    const ls = lines("giant/app.log");
    eq(ls.length, 1, "the giant line landed as ONE line");
    assert(ls[0].includes("GGGG"), "its content survived");
    log.info("tail");
    log.flush();
    eq(lines("giant/app.log").length, 2, "the buffer still works after a bypass");
}

/* buffer:0 and buffer:false are unbuffered. */
{
    const log = new Logger({ dest: P("unbuf/app.log").toString(), mkdir: true,
                             timestamp: false, buffer: 0 });
    log.info("immediate");
    eq(lines("unbuf/app.log").length, 1, "buffer:0 writes straight through");
}

/* buffer bounds. */
throws(() => new Logger({ dest: P("bb/app.log").toString(), buffer: 2097152 }),
       "a buffer over 1 MiB is refused");
throws(() => new Logger({ dest: P("bb/app.log").toString(), buffer: -4 }),
       "a negative buffer is refused");

/* The 64 KiB line cap: a huge payload is truncated, and the line stays
 * framed (one line, one newline). */
{
    const log = new Logger({ dest: P("cap/app.log").toString(), mkdir: true,
                             timestamp: false });
    log.info("H".repeat(100 * 1024));
    log.flush();
    const ls = lines("cap/app.log");
    eq(ls.length, 1, "the truncated line is still one framed line");
    assert(ls[0].length <= 64 * 1024, "the line is under the cap: " + ls[0].length);
    assert(ls[0].startsWith("{\"level\":\"info\",\"msg\":\"HH"),
           "the cap cuts the payload, not the frame");
}

/* ======================================================== refusals ======== */

throws(() => new Logger({ dest: P("dirmore/app.log").toString() + "/nested/x.log" }),
       "a file used as a directory is refused");
throws(() => new Logger({ dest: TMP.toString() }),
       "a directory as dest is refused");
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { size: 0 } }),
       "size 0 is refused");
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { size: "" } }),
       "an empty size is refused");
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { size: "m" } }),
       "a unit without digits is refused");
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { frequency: 0 } }),
       "frequency 0 is refused");
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { count: 1.5 } }),
       "a fractional count is refused");
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: "daily" }),
       "a non-object rollover is refused");
throws(() => new Logger({ dest: 42 }),
       "a non-string dest is refused");

/* Size units parse: k, m, g, an optional b, and plain bytes. */
{
    new Logger({ dest: P("units/a.log").toString(), rollover: { size: "10m" } }).flush();
    new Logger({ dest: P("units/b.log").toString(), rollover: { size: "2g" } }).flush();
    new Logger({ dest: P("units/c.log").toString(), rollover: { size: "500b" } }).flush();
    new Logger({ dest: P("units/d.log").toString(), rollover: { size: 2048 } }).flush();
    new Logger({ dest: P("units/e.log").toString(), rollover: { size: "1K" } }).flush();
    assert(true, "size units parse");
}

/* Two Loggers on one path with the SAME options share one sink, and both
 * land through one flush. */
{
    const o = { dest: P("shared/app.log").toString(), mkdir: true, timestamp: false };
    const a = new Logger(o);
    const b = new Logger(o);
    a.info("from a");
    b.info("from b");
    b.flush();                           /* one flush lands both */
    const ls = lines("shared/app.log");
    eq(ls.length, 2, "both Loggers wrote through the shared sink");
    assert(ls[0].includes("from a") && ls[1].includes("from b"), "order preserved");
}

/* mkdir creates the whole chain. */
{
    const deep = new Logger({ dest: P("mk/a/b/c/app.log").toString(), mkdir: true,
                              timestamp: false });
    deep.info("deep");
    deep.flush();
    eq(lines("mk/a/b/c/app.log").length, 1, "nested mkdir worked");
}

/* ============================================================ format ====== */

/* Numbers, in JSON: integral doubles render exactly; non-finites are null
 * (JSON has no NaN/Infinity); 1e21 keeps the engine's exponent form. */
{
    const log = new Logger({ dest: P("num/app.log").toString(), mkdir: true,
                             timestamp: false });
    log.info({ big: 5e9, safe: (2 ** 53) - 1, frac: 0.1, exp: 1e21,
               negzero: -0, nan: NaN, inf: Infinity, ninf: -Infinity,
               tiny: 5e-7 });
    log.flush();
    const [line] = lines("num/app.log");
    for (const frag of ['"big":5000000000', '"safe":9007199254740991',
                        '"frac":0.1', '"exp":1e+21', '"negzero":0',
                        '"nan":null', '"inf":null', '"ninf":null', '"tiny":5e-7'])
        assert(line.includes(frag), "number rendering carries " + frag + ": " + line);
}

/* Unicode, astral planes, and control characters survive as valid JSON. */
{
    const log = new Logger({ dest: P("uni/app.log").toString(), mkdir: true,
                             timestamp: false });
    log.info({ emoji: "ok \u{1F600}", cjk: "你好", ctl: "\u0000\u001f\u0007" },
             "m\nx\t\u0001y");
    log.flush();
    const [line] = lines("uni/app.log");
    assert(line.includes("ok \u{1F600}") && line.includes("你好"),
           "printable unicode passes through");
    assert(line.includes("\\u0000") && line.includes("\\u001f") && line.includes("\\u0007"),
           "control characters are escaped: " + line);
    assert(line.includes("m\\nx\\t\\u0001y"), "message controls are escaped");
    eq(lines("uni/app.log").length, 1, "unicode did not break framing");
}

/* A cyclic error property is elided, not fatal; a missing stack is fine. */
{
    const log = new Logger({ dest: P("errc/app.log").toString(), mkdir: true,
                             timestamp: false });
    const e = new Error("cyc");
    e.self = e;
    e.detail = { deep: e };
    log.error(e, "cyclic error");
    const e2 = new Error("nostack");
    e2.stack = undefined;
    log.error(e2, "no stack");
    log.flush();
    const ls = lines("errc/app.log");
    eq(ls.length, 2, "both error lines landed");
    assert(ls[0].includes('"type":"Error"') && !ls[0].includes('"stack"') === false,
           "the cyclic error still carries its own stack");
    assert(!ls[1].includes('"stack":'), "stack:undefined is omitted");
}

/* Undefined field values are skipped; null survives. */
{
    const log = new Logger({ dest: P("undef/app.log").toString(), mkdir: true,
                             timestamp: false });
    log.info({ a: undefined, b: null, c: 1 });
    log.flush();
    const [line] = lines("undef/app.log");
    assert(line.includes('"b":null') && line.includes('"c":1') &&
           !line.includes('"a"'), "undefined dropped, null kept: " + line);
}

/* base may not forge frame keys either -- the filter covers the prefix. A
 * key the frame does not write (pid here: the option is off) stays free. */
{
    const log = new Logger({ dest: P("baser/app.log").toString(), mkdir: true,
                             timestamp: false, name: "b",
                             base: { level: "trace", msg: "no", pid: 1 } });
    log.info("kept");
    log.flush();
    const [line] = lines("baser/app.log");
    eq(line, '{"level":"info","name":"b","pid":1,"msg":"kept"}',
       "frame keys in base dropped, non-frame keys kept: " + line);
}

/* The level gate writes nothing, and a runtime level change takes effect on
 * the file destination too. */
{
    const log = new Logger({ dest: P("gate/app.log").toString(), mkdir: true,
                             timestamp: false, level: "error" });
    log.info("suppressed");
    log.flush();
    eq(lines("gate/app.log").length, 0, "a suppressed line writes nothing");
    log.level = "debug";
    log.debug("now visible");
    log.flush();
    eq(lines("gate/app.log").length, 1, "the level setter re-opens the gate");
}

/* child and grandchild share the destination. */
{
    const log = new Logger({ dest: P("fam/app.log").toString(), mkdir: true,
                             timestamp: false, name: "p" });
    log.child({ a: 1 }).child({ b: 2 }).info("deep child");
    log.flush();
    const [line] = lines("fam/app.log");
    assert(line.includes('"a":1') && line.includes('"b":2') && line.includes('"name":"p"'),
           "grandchild carries the whole chain: " + line);
}

/* flush() is idempotent and callable repeatedly. */
{
    const log = new Logger({ dest: P("fl/app.log").toString(), mkdir: true,
                             timestamp: false, buffer: true });
    log.info("x");
    log.flush();
    log.flush();
    log.flush();
    eq(lines("fl/app.log").length, 1, "flush is idempotent");
}

/* ========================================================== text format == */

{
    const log = new Logger({ dest: P("text/app.log").toString(), mkdir: true,
                             format: "text", timestamp: false, level: "trace" });
    log.trace("t");
    log.debug("d");
    log.info("i");
    log.warn("w");
    log.error("e");
    log.fatal("f");
    log.info({ n: 5e9, nan: NaN, frac: 0.1, exp: 1e21, negzero: -0 }, "numbers");
    log.info({ cyc: (function () { const c = { a: 1 }; c.self = c; return c; })() },
             "cyclic");
    log.info({ u: "é你 \u{1F600}", ctl: "a\u0000b" }, "uni");
    log.info("msg \n break \u0001");
    log.flush();
    const ls = lines("text/app.log");
    eq(ls.length, 10, "text framing held through all sections");
    /* levels align at five columns: short names pad, long names do not */
    eq(ls[0], "trace t", "trace is already five columns");
    eq(ls[1], "debug d", "debug too");
    eq(ls[2], "info  i", "info pads to five columns");
    eq(ls[3], "warn  w", "warn pads");
    eq(ls[4], "error e", "error is five columns");
    eq(ls[5], "fatal f", "fatal too");
    assert(ls[6].startsWith("info  numbers n=5000000000 nan=null frac=0.1 exp=1e+21 negzero=0"),
           "text numbers match JSON rendering: " + ls[6]);
    assert(ls[7].includes("cyc={\"a\":1,\"self\":\"[Circular]\"}"),
           "a text composite uses the JSON serializer's answers: " + ls[7]);
    assert(ls[8].includes("u=é你 \u{1F600}") && ls[8].includes("a\\u0000b"),
           "text escapes controls, keeps unicode: " + ls[8]);
    eq(ls[9], "info  msg \\n break \\u0001",
           "text message controls are escaped: " + ls[9]);
}

/* text + name + pid + base, and the child prefix merge. */
{
    const log = new Logger({ dest: P("text2/app.log").toString(), mkdir: true,
                             format: "text", timestamp: false, name: "api",
                             pid: true, base: { env: "dev" } });
    log.child({ route: "/x" }).info("hi");
    log.flush();
    const [line] = lines("text2/app.log");
    assert(line.startsWith("info  api: hi pid="), "name and pid land: " + line);
    assert(line.includes("env=dev route=/x"), "base then child fields: " + line);
}

/* A huge payload truncates in text mode too, still one framed line. */
{
    const log = new Logger({ dest: P("textcap/app.log").toString(), mkdir: true,
                             format: "text", timestamp: false });
    log.info("T".repeat(100 * 1024));
    log.flush();
    const ls = lines("textcap/app.log");
    eq(ls.length, 1, "text truncation keeps framing");
    assert(ls[0].length <= 64 * 1024, "text line under the cap");
}

/* ======================================================== timestamps ====== */

{
    const log = new Logger({ dest: P("ts/app.log").toString(), mkdir: true });
    log.info("epoch");
    log.flush();
    assert(/^{"time":\d{13},"level":"info","msg":"epoch"}$/.test(lines("ts/app.log")[0]),
           "the default epoch timestamp is 13-digit millis");
}
{
    const log = new Logger({ dest: P("tsiso/app.log").toString(), mkdir: true,
                             timestamp: "iso" });
    log.info("iso");
    log.flush();
    assert(/^{"time":"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z","level":"info","msg":"iso"}$/
               .test(lines("tsiso/app.log")[0]), "iso is RFC 3339 with millis");
}

removeAll(P("."));
print((fails ? "test_log_rollover: " + fails + " FAILED of " + n : "test_log_rollover: " + n + " assertions, 0 failures"));
if (fails)
    throw new Error("test_log_rollover failed");
