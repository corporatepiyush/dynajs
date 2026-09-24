// flags: --std
/* test_shutdown_parked.js -- C-pinned parked work fails at shutdown, and
 * every exit shape leaves clean.
 *
 * The defect class: any module that parks a promise across JS -- readFileAsync
 * on a FIFO with no writer, a pipe over a source that never resolves, a
 * crawl flight on a requestAsync that never settles -- kept its resolve/reject
 * (and through them the promise and its forest) alive past the last
 * JS_FreeContext, so JS_FreeRuntime aborted on assert(list_empty(&rt->gc_obj_list))
 * (exit 134) and the parked promise silently vanished. The engine now runs
 * shutdown sweeps at teardown: every parked operation FAILS with a rejection
 * naming engine shutdown while JS is still legal (a .catch handler observes
 * it), and the process exits with its own code -- 0 for a normal end, 1 for
 * a thrown one.
 *
 * Each ROW is a child process (the assertion is the child's exit code, which
 * is exactly where the abort used to appear); the in-process CONTROL rows at
 * the bottom prove the other half of the contract: in-flight work that CAN
 * complete still completes normally -- the sweeps fire only at teardown.
 *
 * Needs /bin/sh + timeout. Run: dynajs (CONFIG_NATIVE_MODULES=y)
 * tests/test_shutdown_parked.js */
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, readFile, removeAll, Path } from "dyna:file";
import { pipe, fromBytes, toFile, lines } from "dyna:stream";

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
    print("test_shutdown_parked: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_shutdown_parked: " + fail + " failures");
} else {
    const T = makeTempDir("shutdown_parked");
    const CHILD = "tests/test_shutdown_parked_child.js";
    sh(`mkfifo ${T}/fifo`);
    sh(`mkfifo ${T}/fifo2`);

    /* run one shape; return {rc, out, err} */
    function runShape(shape, budget, flags) {
        sh(`timeout -k 5 ${budget ? budget : 25} ./dynajs ${flags ? flags + " " : ""}${CHILD} ${shape} ${T} ` +
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

    /* ---- the named repro: parked FIFO read + uncaught throw ---------- */
    {
        const r = runShape("file-throw");
        ok(r.rc === 1, "parked-read + uncaught throw exits 1 (got " + r.rc + ")");
        noAbort(r, "parked-read + uncaught throw");
        ok(r.out.indexOf("REJECTED file: readFileAsync: aborted at engine shutdown") >= 0,
           "the parked read's .catch observed the shutdown failure");
        ok(r.err.indexOf("exit-driver") >= 0, "the throw still surfaces");
    }

    /* ---- parked read whose promise was dropped + uncaught throw ------ */
    {
        const r = runShape("file-gc-drop-throw");
        ok(r.rc === 1, "GC-dropped parked read exits 1 (got " + r.rc + ")");
        noAbort(r, "GC-dropped parked read");
    }

    /* ---- parked read + an uncaught (unhandled) rejection ------------- */
    {
        const r = runShape("file-async-uncaught");
        ok(r.rc === 1, "parked read + unhandled rejection exits 1 (got " +
           r.rc + ")");
        noAbort(r, "parked read + unhandled rejection");
        ok(r.err.indexOf("orphan-rejection") >= 0,
           "the unhandled rejection is the exit reason");
    }

    /* ---- CONTROL: the writer arrives and the parked read SETTLES from
           the WORK itself -- whatever its outcome (the family slurps by fd,
           so a FIFO lands on its own EINVAL), it is never the shutdown's
           "aborted at engine shutdown" and the process exits clean. In-flight
           semantics untouched: the sweep speaks only at teardown. -------- */
    {
        sh(`( sleep 0.4; printf hello > ${T}/fifo2 ) >/dev/null 2>&1 &`);
        const r = runShape("file-normal-complete");
        ok(r.rc === 0, "parked read + late writer exits 0 (got " + r.rc + ")");
        ok(r.out.indexOf("RESOLVED file:") >= 0 ||
           r.out.indexOf("REJECTED file:") >= 0,
           "the parked read settled from the work when the writer came");
        ok(r.out.indexOf("engine shutdown") < 0,
           "the settled read did NOT get the shutdown failure");
        noAbort(r, "parked read + late writer");
    }

    /* ---- parked pipeline over a never-resolving source, normal end --- */
    {
        const r = runShape("stream-normal");
        ok(r.rc === 0, "parked pipe exits 0 (got " + r.rc + ")");
        noAbort(r, "parked pipe");
        ok(r.out.indexOf("REJECTED pipe: stream.pipe: aborted at engine shutdown") >= 0,
           "the parked pipe rejected with the shutdown reason");
    }

    /* ---- parked line-iterator next(), normal end --------------------- */
    {
        const r = runShape("stream-lines-normal");
        ok(r.rc === 0, "parked lines next() exits 0 (got " + r.rc + ")");
        noAbort(r, "parked lines next()");
        ok(r.out.indexOf("REJECTED lines: stream.lines: next() aborted at engine shutdown") >= 0,
           "the parked next() rejected with the shutdown reason");
    }

    /* ---- parked crawl flight + waiter, normal end -------------------- */
    {
        const r = runShape("scrape-normal");
        ok(r.rc === 0, "parked crawl exits 0 (got " + r.rc + ")");
        noAbort(r, "parked crawl");
        ok(r.out.indexOf("REJECTED crawl: Crawl: next() aborted at engine shutdown") >= 0,
           "the parked crawl next() rejected with the shutdown reason");
    }

    /* ---- multi-module parks + uncaught throw ------------------------- */
    {
        const r = runShape("multi-throw");
        ok(r.rc === 1, "multi-module parks + throw exits 1 (got " + r.rc + ")");
        noAbort(r, "multi-module parks + throw");
        const n = r.out.split("REJECTED ").length - 1;
        ok(n === 3, "all THREE parked operations failed (got " + n + ")");
        ok(r.out.indexOf("readFileAsync: aborted at engine shutdown") >= 0 &&
           r.out.indexOf("stream.pipe: aborted at engine shutdown") >= 0 &&
           r.out.indexOf("Crawl: next() aborted at engine shutdown") >= 0,
           "each park failed with its own named reason");
    }

    /* ---- multi-module parks, normal end ------------------------------ */
    {
        const r = runShape("multi-normal");
        ok(r.rc === 0, "multi-module parks exit 0 (got " + r.rc + ")");
        noAbort(r, "multi-module parks");
        const n = r.out.split("REJECTED ").length - 1;
        ok(n === 3, "all THREE parked operations failed (got " + n + ")");
    }

    /* ---- GC-dropped parks (no handlers at all), normal end ----------- */
    {
        const r = runShape("multi-gc-drop");
        ok(r.rc === 0, "GC-dropped parks exit 0 (got " + r.rc + ")");
        noAbort(r, "GC-dropped parks");
    }

    /* ---- parked crawl + uncaught throw ------------------------------- */
    {
        const r = runShape("scrape-park-uncaught");
        ok(r.rc === 1, "parked crawl + throw exits 1 (got " + r.rc + ")");
        noAbort(r, "parked crawl + throw");
        ok(r.out.indexOf("REJECTED crawl:") >= 0,
           "the parked crawl still failed observably");
    }

    /* ---- the dyna:http family's parked exchanges (t3-x1 ITEM 3) --------
       An HTTPClient exchange in flight at exit is parked work whose
       self-pin is a C-held JSValue the collector deliberately cannot see
       (a marked self-reference reads as a one-object cycle and was collected
       mid-flight once) -- so without the per-job shutdown sweep the wrapper
       is never collectable and JS_FreeRuntime aborts on gc_obj_list. These
       rows are that sweep's detector: on the pre-sweep code the first one
       exits 134 on ~4 runs out of 5 (the fifth only when the hop happened
       to complete before the exit). The second row is the App.rpc spot
       check made permanent: the app's pending-response machinery needs NO
       sweep -- its pend struct is owned by a gc-visible object whose
       finalizer releases the conn without JS -- but the WAITING CLIENT is a
       parked exchange and its rejection must surface. */
    {
        const r = runShape("http-async-inflight");
        ok(r.rc === 1, "parked http exchange + throw exits 1 (got " + r.rc + ")");
        noAbort(r, "parked http exchange");
        ok(r.out.indexOf("REJECTED http:") >= 0,
           "the parked exchange still failed observably");
    }
    {
        const r = runShape("http-app-parked");
        ok(r.rc === 1, "parked app rpc + waiting client + throw exits 1 (got " + r.rc + ")");
        noAbort(r, "parked app rpc");
        ok(r.out.indexOf("REJECTED app:") >= 0,
           "the waiting client still sees the failure");
    }

    /* ---- a WORKER runtime dies, then THIS runtime rejects unhandled -----
           The rejection-quieting latch must be per thread: a process-wide
           one is set by the worker's own js_std_free_handlers and silences
           this runtime's tracker for the rest of the process. The control
           is the same rejection without the worker (file-async-uncaught's
           sibling shape: it is always reported). ------------------------- */
    {
        const r = runShape("worker-then-rejection", 25, "--std");
        if (r.out.indexOf("SKIP worker-unavailable") >= 0) {
            skipped("worker-then-rejection: os.Worker unavailable");
        } else {
            ok(r.rc === 1, "rejection after a worker runtime died exits 1 (got " +
               r.rc + ")");
            ok(r.err.indexOf("post-worker-rejection") >= 0,
               "the parent runtime's rejection is still reported");
            noRow(r, "worker-then-rejection", "survived");
            noAbort(r, "worker-then-rejection");
        }
    }

    /* ---- in-process CONTROLS: work that can complete still completes,
           and the parent's OWN teardown (with settled parks) is clean --- */
    {
        const enc = (s) => new TextEncoder().encode(s);
        const dec = (u) => new TextDecoder().decode(u);
        const total = await pipe(fromBytes(enc("in-flight bytes")),
                                 toFile(new Path(T + "/ctl.bin")));
        ok(total === 15, "an in-flight pipeline runs to completion (" + total + ")");
        const got = readFile(new Path(T + "/ctl.bin"));
        ok(got === "in-flight bytes", "the pipeline's bytes landed");
        const it = lines(fromBytes(enc("a\nb\n")));
        const r1 = await it.next();
        const r2 = await it.next();
        const r3 = await it.next();
        ok(r1.value === "a" && r2.value === "b" && r3.done === true,
           "an in-flight line iterator drains normally");
    }

    try { removeAll(new Path(T)); } catch (e) {}

    print("test_shutdown_parked: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_shutdown_parked: " + fail + " failures");
}
