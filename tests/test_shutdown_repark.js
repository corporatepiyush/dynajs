// flags: --std
/* test_shutdown_repark.js -- the shutdown re-park stragglers, the net-style
 * park template, and the parked-file liveness policy, as permanent rows.
 *
 * Every row is a CHILD process (the assertion is its exit code and output,
 * which is exactly where the teardown abort used to appear). The shapes
 * come from the adversarial review's probes: a park made DURING the
 * shutdown drain must be released WITHOUT settling (the straggler's pins
 * drop BEFORE the runtime's final GC, so what they pin is collectable --
 * releasing them later used to abort the teardown with exit 134), a
 * dead-crawl next() must REJECT instead of parking a waiter nothing
 * releases, and a stale settle on a graveyarded flight must walk away in
 * live memory. Two more rows pin the drain's failure paths: a job that
 * THROWS in the drain (the engine releases the unconsumed exception exactly
 * once -- e.g. the parked failure is still reported and the teardown
 * completes) with its no-throw control, and a sweep that re-registers the
 * identical pair (the pass terminates at its documented cap).
 *
 * The quiet-file row pins the documented liveness policy (both
 * declarations, dyna-file.c and dyna-stream.c): a parked native op never
 * holds the event loop by itself -- a live event source does. A file job's
 * kernel completion is such a source (the worker can still land the read),
 * so a writer-less FIFO read KEEPS the process alive (Node's fs does the
 * same; test_shutdown_parked.js's file-normal-complete row proves the
 * settlement half: when the writer comes the read settles from the WORK).
 * Parks whose only settler is future JS hold nothing and exit quietly.
 *
 * Needs /bin/sh + timeout. Run: dynajs (CONFIG_NATIVE_MODULES=y)
 * tests/test_shutdown_repark.js */
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, readFile, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function skipped(w) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; }
    skip++; print("  SKIP  " + w);
}
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

if (!Which("timeout")) {
    skipped("timeout missing -- child exit shapes cannot be bounded");
    print("test_shutdown_repark: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_shutdown_repark: " + fail + " failures");
} else {
    const T = makeTempDir("shutdown_repark");
    const CHILD = "tests/test_shutdown_repark_child.js";
    sh(`mkfifo ${T}/fifo`);
    sh(`mkfifo ${T}/fifo2`);

    /* The park-template fixture (tests/park_sweep.c) is a shared library
       the template rows dlopen; built here so the suite owns its fixture
       (the loader wants a CWD-relative name, hence tests/park_sweep.so). */
    const CC = getEnv("CC") ? getEnv("CC") : "cc";
    let haveSo = 0;
    if (sh(`command -v ${CC} >/dev/null 2>&1`) === 0) {
        const link = sh(`(uname | grep -q Darwin && ` +
            `${CC} -shared -undefined dynamic_lookup -I src ` +
            `-o tests/park_sweep.so tests/park_sweep.c || ` +
            `${CC} -shared -fPIC -I src -o tests/park_sweep.so tests/park_sweep.c) ` +
            `>/dev/null 2>&1`);
        haveSo = link === 0 ? 1 : 0;
    }
    if (!haveSo)
        skipped("tests/park_sweep.so could not be built (no working " + CC + ")");

    /* run one shape; return {rc, out, err} */
    function runShape(shape, budget) {
        sh(`timeout -k 5 ${budget ? budget : 25} ./dynajs ${CHILD} ${shape} ${T} ` +
           `> ${T}/${shape}.out 2> ${T}/${shape}.err; ` +
           `echo $? > ${T}/${shape}.rc`);
        let rc = parseInt(readFile(new Path(T + "/" + shape + ".rc")), 10);
        return {
            rc,
            out: readFile(new Path(T + "/" + shape + ".out")),
            err: readFile(new Path(T + "/" + shape + ".err")),
        };
    }
    const noAbort = (r, w) =>
        ok(r.err.indexOf("Assertion failed") < 0 &&
           r.err.indexOf("Segmentation") < 0, w + ": no teardown abort");
    const noRow = (r, w, tag) =>
        ok(r.out.indexOf(tag) < 0, w + ": " + tag + " never printed");

    /* ---- the net-style park template (a bare prototype park) ---------- */
    if (haveSo) {
        const r = runShape("template-t1");
        ok(r.rc === 0, "template t1 (normal settle) exits 0 (got " + r.rc + ")");
        ok(r.out.indexOf("RESOLVED 42") >= 0 && r.out.indexOf("settled") >= 0,
           "template t1 settled from the work (42)");
        noAbort(r, "template t1");

        const r2 = runShape("template-t2");
        ok(r2.rc === 0, "template t2 (parked) exits 0 (got " + r2.rc + ")");
        ok(r2.out.indexOf("REJECTED probe.park: aborted at engine shutdown") >= 0,
           "template t2 failed observably at teardown");
        noAbort(r2, "template t2");

        const r3 = runShape("template-t3");
        ok(r3.rc === 0, "template t3 (re-park in the drain) exits 0 (got " +
           r3.rc + ")");
        ok(r3.out.indexOf("REJECTED 1") >= 0,
           "template t3: the first park failed observably");
        noRow(r3, "template t3", "REJECTED 2");
        noRow(r3, "template t3", "RESOLVED 2");
        noAbort(r3, "template t3");
    }

    /* ---- a PIPE park made during the drain ---------------------------- */
    {
        const r = runShape("repark-pipe");
        ok(r.rc === 0, "repark-pipe exits 0 (got " + r.rc + ")");
        ok(r.out.indexOf("REJECTED 1: stream.pipe: aborted at engine shutdown") >= 0,
           "repark-pipe: the first pipe failed observably");
        noRow(r, "repark-pipe", "REJECTED 2");
        noRow(r, "repark-pipe", "RESOLVED 2");
        noAbort(r, "repark-pipe");
    }

    /* ---- a FILE park made during the drain (exit-driver variant: the
           parked FIFO read holds the loop, so the quiet shape never
           reaches teardown -- see the quiet-file row) ------------------- */
    {
        const r = runShape("repark-file-driver");
        ok(r.rc === 1, "repark-file-driver exits 1 (got " + r.rc + ")");
        ok(r.out.indexOf("REJECTED 1: readFileAsync: aborted at engine shutdown") >= 0,
           "repark-file: the first read failed observably");
        noRow(r, "repark-file-driver", "REJECTED 2");
        noRow(r, "repark-file-driver", "RESOLVED 2");
        ok(r.err.indexOf("exit-driver") >= 0, "the throw still surfaces");
        noAbort(r, "repark-file-driver");
    }

    /* ---- a throw from the DRAIN's own user code -----------------------
           The drain stops on the exception; the engine must release the
           unconsumed value exactly once (a value left in
           rt->current_exception pins its prototype chain -- and through the
           realm ref C functions hold, the whole context graph -- so the
           runtime's GCs collect nothing and the gc_obj_list assertion
           aborts the teardown). The parked failure is still reported by the
           drain, and the throwing microtask really ran. */
    {
        const r = runShape("drain-throw");
        ok(r.rc === 0, "drain-throw exits 0 (got " + r.rc + ")");
        ok(r.out.indexOf("REJECTED: stream.pipe: aborted at engine shutdown") >= 0,
           "drain-throw: the parked failure is still reported");
        ok(r.out.indexOf("microtask-ran") >= 0,
           "drain-throw: the throwing microtask ran in the drain");
        noAbort(r, "drain-throw");
    }
    /* ---- the control: same shape WITHOUT the throwing microtask ------- */
    {
        const r = runShape("drain-nothrow");
        ok(r.rc === 0, "drain-nothrow exits 0 (got " + r.rc + ")");
        ok(r.out.indexOf("REJECTED: stream.pipe: aborted at engine shutdown") >= 0,
           "drain-nothrow: the parked failure is reported");
        noAbort(r, "drain-nothrow");
    }

    /* ---- a sweep that re-registers the identical pair -----------------
           The pass is capped (JS_SHUTDOWN_SWEEP_PASS_MAX): the walk must
           TERMINATE at the cap and drop the entry without calling it, not
           spin. The row judges termination by rc (a spin is killed by the
           timeout: 124/137) plus the engine's one-line cap diagnostic. */
    if (haveSo) {
        const r = runShape("selfreg-sweep", 20);
        ok(r.rc === 0, "selfreg-sweep exits 0 (got " + r.rc +
           ") -- the sweep pass is bounded, no spin");
        ok(r.out.indexOf("REJECTED: probe.rereg: aborted at engine shutdown") >= 0,
           "selfreg-sweep: the park failed observably before the cap");
        ok(r.err.indexOf("sweep pass capped") >= 0,
           "selfreg-sweep: the pass reported hitting the cap");
        noAbort(r, "selfreg-sweep");
    }

    /* ---- next() re-parked on a DEAD crawl ----------------------------- */
    {
        const r = runShape("repark-waiter");
        ok(r.rc === 0, "repark-waiter exits 0 (got " + r.rc + ")");
        ok(r.out.indexOf("REJECTED outer: Crawl: next() aborted at engine shutdown") >= 0,
           "repark-waiter: the parked next() failed observably");
        ok(r.out.indexOf("REJECTED inner: Crawl: next() on a closed crawl") >= 0,
           "repark-waiter: the drain-time next() REJECTED (never parked)");
        ok(r.out.indexOf("page3") < 0,
           "repark-waiter: the dead crawl produced no page");
        noAbort(r, "repark-waiter");
    }

    /* ---- a stale settle landing on a graveyarded flight --------------- */
    {
        const r = runShape("graveyard-settle");
        ok(r.rc === 0, "graveyard-settle exits 0 (got " + r.rc + ")");
        ok(r.out.indexOf("stale-settle-landed") >= 0,
           "graveyard-settle: the stale settle landed on the dead-flag path");
        noAbort(r, "graveyard-settle");
    }

    /* ---- the liveness policy: a parked FIFO read holds the loop ------- */
    {
        const r = runShape("quiet-file", 3);
        ok(r.rc === 124 || r.rc === 137,
           "quiet-file is still parked when the timeout fires (got " + r.rc +
           ") -- the kernel completion is a live event source");
        ok(readFile(new Path(T + "/quiet.marker")) === "parked-quiet",
           "quiet-file: the read was parked");
        noRow(r, "quiet-file", "RESOLVED 1");
        noRow(r, "quiet-file", "REJECTED 1");
        noAbort(r, "quiet-file");
    }

    sh(`rm -rf ${T}`);
}

print("test_shutdown_repark: " + pass + " passed, " + fail + " failed, " +
      skip + " skipped");
if (fail) throw new Error("test_shutdown_repark: " + fail + " failures");
