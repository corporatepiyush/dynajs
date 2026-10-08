// flags: --std
// timeout: 120
// R5-6: the metrics registry's global lock must not introduce unbounded
// latency. met_scrape_buf holds met_lock across snprintf/realloc of at most
// MET_MAX series (no JS call, no nesting), so a full-registry scrape is
// bounded; concurrent native recorders only serialize behind that bounded
// render. This probe measures a full-registry scrape, then keeps 4 workers
// hammering counter/histogram recorders while the main thread scrapes in a
// loop, and pins the counter's exact final value (no lost updates).
import * as os from "os";
import * as std from "std";
import { Metrics } from "dyna:net";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }

const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_metlock.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
const WORKERS = 4, ITERS = 200000;

function write(path, s) {
    const f = std.open(path, "w");
    f.puts(s);
    f.close();
}

write(`${T}_wkr.js`, [
    'import * as os from "os";',
    'import { Metrics } from "dyna:net";',
    "const parent = os.Worker.parent;",
    "for (let i = 0; i < " + ITERS + "; i++) {",
    '    Metrics.counter("lk_hot", 1);',
    '    if ((i & 1023) === 0) Metrics.histogram("lk_lat", (i % 100) / 100);',
    "}",
    "parent.postMessage({ done: 1 });",
].join("\n"));

(async () => {
    Metrics.reset();
    for (let i = 0; i < 250; i++) Metrics.counter("full_" + i, 1);
    ok(Metrics.scrape().length > 0, "a 250-series registry scrapes");

    {
        let max = 0, total = 0, inner = 0;
        for (let i = 0; i < 2000; i++) {
            const t0 = Date.now();
            try { Metrics.scrape(); } catch (e) { inner++; }
            const dt = Date.now() - t0;
            if (dt > max) max = dt;
            total += dt;
        }
        ok(inner === 0, "2000 full-registry scrapes all succeed");
        print("  full-registry scrape: avg " + (total / 2000).toFixed(3)
              + "ms max " + max + "ms");
        ok(max < 200, "a full-registry scrape stays under 200 ms (got " + max + "ms)");
    }

    let done = 0, workerError = null;
    const ws = [];
    const t0 = Date.now();
    for (let i = 0; i < WORKERS; i++) {
        const w = new os.Worker(`${T}_wkr.js`);
        w.onmessage = () => { done++; w.onmessage = null; };
        w.onerror = (e) => { workerError = e; };
        ws.push(w);
    }
    let maxScrape = 0, scrapes = 0;
    while (done < WORKERS && Date.now() - t0 < 60000) {
        const s = Date.now();
        Metrics.scrape();
        const dt = Date.now() - s;
        if (dt > maxScrape) maxScrape = dt;
        scrapes++;
        if ((scrapes & 255) === 0) await sleep(1);
    }
    ok(workerError === null, "no worker error while recording under scrape churn");
    ok(done === WORKERS, "all " + WORKERS + " recording workers finished (" + done + ")");
    print("  contended phase: " + scrapes + " scrapes, max " + maxScrape
          + "ms, " + (Date.now() - t0) + "ms wall");
    ok(scrapes > 0, "the main thread kept scraping during the worker phase");
    ok(maxScrape < 2000, "a contended scrape stays under 2 s (got " + maxScrape + "ms)");
    {
        const line = Metrics.scrape().split("\n").find((l) => l.startsWith("lk_hot "));
        const want = WORKERS * ITERS;
        ok(line === "lk_hot " + want,
           "concurrent increments are exact (want " + want + ", got " + line + ")");
    }

    try { std.remove(`${T}_wkr.js`); } catch (e) { }

    if (fails === 0) print("test_metrics_lock_latency: all " + n + " checks passed");
    else {
        print("test_metrics_lock_latency: " + fails + " FAILED of " + n);
        throw new Error("test_metrics_lock_latency: " + fails + " failures");
    }
})();
