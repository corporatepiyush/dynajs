/* test_log_upgrade.js -- phase-1 additions to dyna:log.
 *
 * Logger({ dest: fn }) -- a custom sink. The function receives the
 *   exact formatted line (trailing newline included) the instant it is
 *   built: nothing is batched, so flush() is a no-op and exit-time
 *   flushing has nothing to do. A throwing dest propagates the throw.
 *   child() shares the fn dest; a child's opts-level string dest still
 *   overrides (unchanged behavior for paths).
 * Logger({ sample: N }) -- deterministic per-logger sampling: every
 *   Nth emit attempt that passes the level gate is written. N defaults to
 *   1 (everything). The counter is per logger; a child restarts at zero.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_log_upgrade.js
 */

import { Logger } from "dyna:log";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throwsTypeError(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof TypeError, msg + " (TypeError, got " + caught + ")");
}

/* ----------------: function dest ---------------- */
{
    const lines = [];
    const lg = new Logger({ dest: (line) => lines.push(line) });
    assert(typeof lg.info === "function", "fn-dest logger constructs");
    lg.info("hello fn dest");
    assert(lines.length === 1, "one line delivered");
    assert(lines[0].endsWith("\n"), "line keeps its trailing newline");
    assert(lines[0].indexOf("hello fn dest") >= 0, "line carries the message");
    lg.warn({ ctx: 1 }, "with fields");
    assert(lines.length === 2 && lines[1].indexOf("ctx") >= 0,
        "fields ride along to the fn dest");
    lg.error(new Error("boom"), "with error");
    assert(lines.length === 3 && lines[2].indexOf("boom") >= 0,
        "error rides along to the fn dest");
    lg.flush();   /* no-op for fn dests -- must not throw */
    assert(lines.length === 3, "flush() is a no-op for function dests");

    /* JSON format reaches the fn dest intact (one JSON value per line) */
    const jlines = [];
    const jlg = new Logger({ dest: (l) => jlines.push(l), format: "json" });
    jlg.info("json via fn");
    assert(jlines.length === 1 && jlines[0].indexOf("\"level\":\"info\"") >= 0,
        "json format delivered to fn dest");

    /* a throwing dest propagates: a sink that raises is a caller bug */
    const thr = new Logger({ dest: () => { throw new RangeError("sink boom"); } });
    let caught = null;
    try { thr.info("x"); } catch (e) { caught = e; }
    assert(caught instanceof RangeError && caught.message === "sink boom",
        "throwing dest propagates");

    /* child shares the fn dest */
    const shared = [];
    const parent = new Logger({ dest: (l) => shared.push("P" + l) });
    const kid = parent.child({ sub: "k" });
    kid.info("from child");
    assert(shared.length === 1 && shared[0].startsWith("P"),
        "child shares the parent's fn dest");
}

/* ----------------: sample ---------------- */
{
    throwsTypeError(() => new Logger({ sample: 0 }), "sample 0 refused");
    throwsTypeError(() => new Logger({ sample: -3 }), "sample negative refused");
    throwsTypeError(() => new Logger({ sample: 1.5 }), "sample non-integer refused");
    throwsTypeError(() => new Logger({ sample: "3" }), "sample string refused");
    throwsTypeError(() => new Logger({ sample: NaN }), "sample NaN refused");

    /* every Nth attempt: 7 attempts at N=3 -> attempts 3 and 6 */
    const lines = [];
    const lg = new Logger({ dest: (l) => lines.push(l), sample: 3 });
    for (let i = 0; i < 7; i++) lg.info("n" + i);
    assert(lines.length === 2, "7 attempts at N=3 deliver 2, got " + lines.length);
    assert(lines[0].indexOf("n2") >= 0, "first delivery is attempt 3 (n2)");
    assert(lines[1].indexOf("n5") >= 0, "second delivery is attempt 6 (n5)");

    /* N=1 is the default and means everything */
    const all = [];
    const every = new Logger({ dest: (l) => all.push(l) });
    for (let i = 0; i < 5; i++) every.info("e" + i);
    assert(all.length === 5, "default sample delivers every line");

    /* explicit N=1 same */
    const ones = [];
    const one = new Logger({ dest: (l) => ones.push(l), sample: 1 });
    for (let i = 0; i < 5; i++) one.info("o" + i);
    assert(ones.length === 5, "sample 1 delivers every line");

    /* counters are per logger: two loggers do not share progress */
    const bucket = [];
    const a = new Logger({ dest: (l) => bucket.push(l), sample: 2 });
    a.info("a1");                     /* attempt 1 -> dropped */
    const b = new Logger({ dest: (l) => bucket.push(l), sample: 2 });
    b.info("b1");                     /* b's attempt 1 -> dropped */
    b.info("b2");                     /* b's attempt 2 -> delivered */
    assert(bucket.length === 1 && bucket[0].indexOf("b2") >= 0,
        "sample counters are per logger");

    /* the counter counts level-PASSING attempts only */
    const gated = [];
    const gl = new Logger({ dest: (l) => gated.push(l), sample: 2, level: "warn" });
    gl.info("i1");                    /* below level: not counted */
    gl.info("i2");                    /* below level: not counted */
    gl.warn("w1");                    /* warn attempt 1 -> dropped */
    gl.warn("w2");                    /* warn attempt 2 -> delivered */
    assert(gated.length === 1 && gated[0].indexOf("w2") >= 0,
        "level gate precedes the sample counter");
}

print("test_log_upgrade: all tests passed (" + n + " assertions)");
