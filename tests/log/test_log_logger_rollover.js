// covers: dyna:log.Logger
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_logger_rollover");
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

__kitS.section("a buffer over 1 MiB is refused", () => {
throws(() => new Logger({ dest: P("bb/app.log").toString(), buffer: 2097152 }),
       "a buffer over 1 MiB is refused");
});

__kitS.section("a negative buffer is refused", () => {
throws(() => new Logger({ dest: P("bb/app.log").toString(), buffer: -4 }),
       "a negative buffer is refused");
});

__kitS.section("a file used as a directory is refused", () => {
throws(() => new Logger({ dest: P("dirmore/app.log").toString() + "/nested/x.log" }),
       "a file used as a directory is refused");
});

__kitS.section("a directory as dest is refused", () => {
throws(() => new Logger({ dest: TMP.toString() }),
       "a directory as dest is refused");
});

__kitS.section("size 0 is refused", () => {
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { size: 0 } }),
       "size 0 is refused");
});

__kitS.section("an empty size is refused", () => {
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { size: "" } }),
       "an empty size is refused");
});

__kitS.section("a unit without digits is refused", () => {
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { size: "m" } }),
       "a unit without digits is refused");
});

__kitS.section("frequency 0 is refused", () => {
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { frequency: 0 } }),
       "frequency 0 is refused");
});

__kitS.section("a fractional count is refused", () => {
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: { count: 1.5 } }),
       "a fractional count is refused");
});

__kitS.section("a non-object rollover is refused", () => {
throws(() => new Logger({ dest: P("sz/app.log").toString(), rollover: "daily" }),
       "a non-object rollover is refused");
});

__kitS.section("a non-string dest is refused", () => {
throws(() => new Logger({ dest: 42 }),
       "a non-string dest is refused");
});
__kitS.end();

removeAll(P("."));
print((fails ? "test_log_rollover: " + fails + " FAILED of " + n : "test_log_rollover: " + n + " assertions, 0 failures"));
if (fails)
    throw new Error("test_log_rollover failed");
