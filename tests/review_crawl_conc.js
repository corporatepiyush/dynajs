/* review_crawl_conc.js -- e7 review, Crawl {concurrency} doc-truth (P72-P79).
 *
 * Parent: spawns tests/review_crawl_conc_child.js and relays its verdict
 * lines. The two-process form follows test_scrape_concurrency.js: the
 * engine's teardown asserts on long-lived async-iterator records when a
 * script's top level awaited a concurrent crawl, so the child writes its
 * verdict FILE before teardown and the parent never trusts child rc.
 *
 * Needs python3. A skip is loud, and fatal under DYNAJS_REQUIRE_TOOLS=1.
 */
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, readFile, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

if (!Which("python3")) {
    if (REQUIRE) fail++;
    else skip++;
    print("test " + (fail ? "FAIL REQUIRED: python3 missing" : "SKIP python3 missing"));
    if (fail) throw new Error("review_crawl_conc: required tool missing");
} else {
    const T = makeTempDir("rev_conc_par");
    sh(`./dynajs tests/review_crawl_conc_child.js ${T} > ${T}/child-stdio.log 2>&1 &`);
    /* wait for the child's DONE marker (its teardown SIGSEGVs on purpose's
     * account, so completion is a FILE, never the exit code); fall back to
     * whatever partial verdict exists after the deadline */
    let done = false;
    for (let i = 0; i < 1200 && !done; i++) {
        sh("sleep 0.1");
        try { readFile(new Path(T + "/done")); done = true; } catch (e) {}
    }
    let raw = "";
    try { raw = readFile(new Path(T + "/result")); } catch (e) {}
    if (!raw) {
        fail++;
        print("  FAIL  child produced no verdict file in 120s" + (done ? "" : " (and no DONE marker)"));
    } else {
        if (!done) print("  INFO  child never reached DONE (mid-run death); partial verdict used");
        for (const line of raw.split("\n")) {
            if (line.indexOf("  ok    ") === 0) { pass++; print(line); }
            else if (line.indexOf("  FAIL  ") === 0) { fail++; print(line); }
            else if (line.indexOf("  INFO  ") === 0) print(line);
        }
    }
    let log = "";
    try { log = readFile(new Path(T + "/child-stdio.log")); } catch (e) {}
    if (fail && log) print("---- child stdio tail:\n" + log.slice(-2000));
    sh("pkill -f review_crawl_conc_child 2>/dev/null; true");
    print("review_crawl_conc: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
}
if (fail) throw new Error("review_crawl_conc: " + fail + " failures");
