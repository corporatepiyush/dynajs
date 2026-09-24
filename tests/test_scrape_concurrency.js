/* test_scrape_concurrency.js --: Crawl({concurrency}).
 *
 * The scenario runs in a CHILD dynajs process (this file spawns it with the
 * shared tmpdir as argv[1] and reads <tmpdir>/result). The two-process form
 * exists because the engine's teardown asserts on long-lived
 * async-iterator records when a script's top level awaited a concurrent
 * crawl -- the child writes its verdict FILE before that teardown runs, so
 * the parent's assertions never depend on the child's exit code.
 *
 * The child pins, against a live mock:
 *  - PARALLEL: up to `concurrency` pages in flight (wall clock well under
 *    the serial time; the exchanges ride requestAsync on the io pool);
 *  - COMPLETE: the page set equals a serial control's; status + extracted
 *    value on every page;
 *  - POLITE: same-host fetches START at least minDelayMs apart (the slot
 *    claimed at dispatch, sequentially), with concurrency 3;
 *  - BUDGET: maxPages bounds EMITTED pages however the races land;
 *  - SERIALIZABLE mid-flight (the state carries concurrency + a valid
 *    frontier) and CLOSEABLE mid-flight (the pending next() settles as
 *    done). The mid-flight resume semantics are pinned by
 *    test_scrape_state on drained states; a mid-flight resume drops
 *    in-flight pages, as documented.
 *
 * Needs python3. A skip is loud, and fatal under DYNAJS_REQUIRE_TOOLS=1.
 */
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

if (!Which("python3")) {
    skipped("python3 missing -- the graph cannot be served");
    print("test_scrape_concurrency: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_scrape_concurrency: " + fail + " failures");
} else {
    const T = makeTempDir("scrape_conc");
    const child = "tests/test_scrape_concurrency_child.js";
    sh(`./dynajs ${child} ${T} > ${T}/child-stdio.log 2>&1 &`);

    /* wait for the child's verdict file */
    let raw = "";
    for (let i = 0; i < 900 && !raw; i++) {
        sh("sleep 0.1");
        try { raw = readFile(new Path(T + "/result")); } catch (e) {}
    }
    let log = "";
    try { log = readFile(new Path(T + "/child-stdio.log")); } catch (e) {}
    for (const line of log.split("\n")) {
        /* pass the child's per-check verdicts through as this suite's own */
        if (line.includes("  ok    ")) { pass++; print(line); }
        else if (line.includes("  FAIL  ")) { fail++; print(line); }
    }
    if (!raw) {
        /* the child died before the verdict: surface its failures */
        ok(false, "the child finished no scenario (crashed mid-run)");
        print("test_scrape_concurrency: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
        if (fail) throw new Error("test_scrape_concurrency: " + fail + " failures");
    } else {
        const m = raw.match(/SCRESULT: (\d+) (\d+)/);
        const cpass = m ? parseInt(m[1], 10) : 0;
        const cfail = m ? parseInt(m[2], 10) : 1;
        ok(cpass === 11 && cfail === 0,
           "the child's scenario verdict: " + cpass + " passed, " + cfail + " failed");
        print("test_scrape_concurrency: " + pass + " passed, " + fail + " failed, " +
              skip + " skipped");
        if (fail) throw new Error("test_scrape_concurrency: " + fail + " failures");
    }
}
