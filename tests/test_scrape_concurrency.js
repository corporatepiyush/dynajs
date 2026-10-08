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

    let raw = "";
    for (let i = 0; i < 900 && !raw; i++) {
        sh("sleep 0.1");
        try { raw = readFile(new Path(T + "/result")); } catch (e) {}
    }
    let log = "";
    try { log = readFile(new Path(T + "/child-stdio.log")); } catch (e) {}
    for (const line of log.split("\n")) {
        if (line.includes("  ok    ")) { pass++; print(line); }
        else if (line.includes("  FAIL  ")) { fail++; print(line); }
    }
    if (!raw) {
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
