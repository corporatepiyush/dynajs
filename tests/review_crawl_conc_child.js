/* review_crawl_conc_child.js -- e7 review, Crawl {concurrency} doc-truth
 * runner (P72-P79). Spawned by review_crawl_conc.js with the shared tmpdir
 * as scriptArgs[1]. Persists "SCRESULT: <pass> <fail>" + per-probe lines to
 * <tmpdir>/result after every probe (this process's teardown SIGSEGVs after
 * a top-level concurrent-crawl run -- pre-existing engine quirk -- so the
 * file, written incrementally, is the only durable channel).
 *
 * Doc claims under test (API.md Crawl, delta-added):
 *   - concurrency default 1, range 1..16; outside throws RangeError
 *   - at 1 next() returns a PLAIN {value,done}; above 1 a Promise
 *   - every crawl carries [Symbol.asyncIterator]; for await works at any
 *     concurrency; for...of works at 1
 *   - the per-host politeness floor is unchanged at concurrency > 1
 *   - the setting round-trips through serialize()/resume()
 *   - in-flight fetches: a page mid-fetch is dropped on resume
 */
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { HTMLParse, Selector, HTMLText } from "dyna:html";
import { HTTPClient } from "dyna:net";
import { Exec, Which } from "dyna:sys";
import { writeFile, readFile, Path } from "dyna:file";

const T = scriptArgs[1];
const P = (n) => T + "/" + n;
const lines = [];
let pass = 0, fail = 0;
/* Persist after EVERY probe: this process's teardown SIGSEGVs after a
 * top-level concurrent-crawl run (pre-existing engine quirk, the reason
 * test_scrape_concurrency.js is two-process), so a final-only write would
 * never land. */
const persist = () => writeFile(new Path(P("result")),
    "SCRESULT: " + pass + " " + fail + "\n" + lines.join("\n") + "\n");
const ok = (c, w, d) => { if (c) { pass++; lines.push("  ok    " + w); }
                          else { fail++; lines.push("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } persist(); };
const info = (s) => { lines.push("  INFO  " + s); persist(); };
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
const finish = () => persist();

if (!Which("python3")) {
    fail++; lines.push("  FAIL  python3 missing");
    finish();
} else {
    writeFile(new Path(P("mock.py")), [
        "import http.server, sys, time, threading as _th, os as _os",
        "_th.Timer(240, lambda: _os._exit(0)).start()",
        "LOG = open(sys.argv[2], 'a', buffering=1)",
        "G={'/a':['/b','/c','/d'], '/b':['/e'], '/c':['/f'], '/d':['/g'], '/e':[], '/f':[], '/g':[]}",
        "class H(http.server.BaseHTTPRequestHandler):",
        "    protocol_version='HTTP/1.1'",
        "    def log_message(self,*a): pass",
        "    def do_GET(self):",
        "        p=self.path",
        "        LOG.write('%.4f %s\\n' % (time.time(), p))",
        "        if p=='/robots.txt': b=b'User-agent: *\\nAllow: /\\n'",
        "        else:",
        "            time.sleep(0.08)",
        "            port=srv.server_address[1]",
        "            links=''.join('<a href=\"http://127.0.0.1:%d%s\">x</a>'%(port,q) for q in G.get(p,[]))",
        "            b=('<html><body><h1>%s</h1>%s</body></html>'%(p,links)).encode()",
        "        self.send_response(200)",
        "        self.send_header('Content-Length',str(len(b)))",
        "        self.end_headers(); self.wfile.write(b)",
        "srv=http.server.ThreadingHTTPServer(('127.0.0.1',0), H)",
        "open(sys.argv[1],'w').write(str(srv.server_address[1]))",
        "srv.serve_forever()",
    ].join("\n"));
    let PORT = 0;
    for (let attempt = 0; attempt < 3 && !PORT; attempt++) {
        sh(`python3 ${P("mock.py")} ${P("port")} ${P("arrivals.log")} >${P("mock.log")} 2>&1 &`);
        for (let i = 0; i < 100 && !PORT; i++) {
            sh("sleep 0.1");
            try { PORT = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) {}
        }
        if (!PORT) sh(`pkill -f '${P("mock.py")}'`);
    }

    const BASE = "http://127.0.0.1:" + PORT;
    const client = new HTTPClient();
    client.setTimeout(8000);
    const mkF = (minDelayMs) => new Fetcher({
        agent: "revconc/1.0", client, minDelayMs: minDelayMs || 0,
        robots: false, allowPrivateHosts: true });
    const ex = () => new Extractor({
        title: { sel: new Selector("h1") },
        links: { sel: new Selector("a"), attr: "href", all: true },
    }, { text: HTMLText });
    const arrivals = () => {
        try {
            return readFile(new Path(P("arrivals.log"))).split("\n")
                .filter((l) => l.trim())
                .map((l) => [parseFloat(l.split(" ")[0]), l.split(" ")[1]]);
        } catch (e) { return []; }
    };
    /* Run one probe block; a thrown error (including the engine's spurious
     * "use of a closed native resource" left by ANOTHER crawl's mid-flight
     * close) marks its probes failed instead of killing the run. */
    const block = async (name, fn) => {
        try { await fn(); }
        catch (e) { ok(false, name + " completed", (e && e.message) || String(e)); }
    };

    /* ---- P72: bounds 1..16, RangeError outside ---- */
    {
        const f = mkF();
        const err = (opts) => { try { new Crawl(f, opts); return null; }
                                catch (e) { return e; } };
        const e0 = err({ concurrency: 0 }), e17 = err({ concurrency: 17 });
        const eneg = err({ concurrency: -1 }), e65537 = err({ concurrency: 65537 });
        ok(e0 && e0.constructor.name === "RangeError" &&
           e17 && e17.constructor.name === "RangeError" &&
           eneg && eneg.constructor.name === "RangeError" &&
           e65537 && e65537.constructor.name === "RangeError",
           "P72 concurrency 0/17/-1/65537 all throw RangeError",
           JSON.stringify([e0 && e0.message, e17 && e17.message]));
        const ok1 = err({ concurrency: 1 }), ok16 = err({ concurrency: 16 });
        ok(ok1 === null && ok16 === null, "P72b concurrency 1 and 16 are accepted");
        const frac = err({ concurrency: 2.5 }), txt = err({ concurrency: "3" });
        info("P72c concurrency 2.5 => " + (frac ? frac.constructor.name + ": " + frac.message : "accepted") +
             "; \"3\" => " + (txt ? txt.constructor.name + ": " + txt.message : "accepted"));
        f.close();
    }

    /* ---- P73: the documented next() SHAPE SWITCH (maxPages 1: no leftover
     * flights, so the close is clean) ---- */
    await block("P73", async () => {
        const f = mkF();
        const c1 = new Crawl(f, { maxPages: 1, concurrency: 1 }).start(BASE + "/a", ex(), HTMLParse);
        const r1 = c1.next();
        ok(r1 && typeof r1.then === "undefined" && "value" in r1 && "done" in r1,
           "P73 at concurrency 1 next() returns a PLAIN {value,done}",
           "got " + (r1 && typeof r1.then === "function" ? "a Promise" : JSON.stringify(Object.keys(r1 || {}))));
        const f2 = mkF();
        const c4 = new Crawl(f2, { maxPages: 1, concurrency: 4 }).start(BASE + "/a", ex(), HTMLParse);
        const r4 = c4.next();
        ok(r4 && typeof r4.then === "function",
           "P73b above concurrency 1 next() returns a Promise",
           "got " + typeof r4);
        const r4b = await r4;
        ok(r4b && "done" in r4b, "P73c and it settles to an IteratorResult");
        const rest = await c4.next();
        ok(rest && rest.done === true, "P73d maxPages 1 drains to done at concurrency 4");
        c1.close(); f.close(); c4.close(); f2.close();
    });

    /* ---- P74/P75: asyncIterator on every crawl; sync iteration at 1 ---- */
    await block("P74", async () => {
        const f = mkF();
        const c = new Crawl(f, { maxPages: 50, maxDepth: 3 }).start(BASE + "/a", ex(), HTMLParse);
        ok(typeof c[Symbol.asyncIterator] === "function",
           "P74 every crawl carries [Symbol.asyncIterator]");
        const urls = [];
        for (const p of c) urls.push(p.url.replace(BASE, ""));
        ok(urls.length === 7, "P75 for...of (sync) drains a concurrency-1 crawl (" + urls.length + ")");
        c.close(); f.close();
    });
    await block("P74b", async () => {
        const f = mkF();
        const c = new Crawl(f, { maxPages: 50, maxDepth: 3, concurrency: 4 })
                       .start(BASE + "/a", ex(), HTMLParse);
        const urls = [];
        for await (const p of c) urls.push(p.url.replace(BASE, ""));
        ok(urls.length === 7, "P74b for await...of drains a concurrent crawl (" + urls.length + ")");
        c.close(); f.close();
    });

    /* ---- P79: maxPages bounds emitted pages at concurrency 4 (before the
     * intentional mid-flight close probes, which poison late fetches) ---- */
    await block("P79", async () => {
        const f = mkF();
        const c = new Crawl(f, { maxPages: 3, maxDepth: 3, concurrency: 4 })
                       .start(BASE + "/a", ex(), HTMLParse);
        let n = 0;
        for await (const p of c) n++;
        ok(n === 3, "P79 maxPages 3 emits exactly 3 pages at concurrency 4 (" + n + ")");
        c.close(); f.close();
    });

    /* ---- P76: per-host politeness floor unchanged at concurrency 4 ---- */
    await block("P76", async () => {
        const mark = arrivals().length;
        const f = mkF(300);
        const c = new Crawl(f, { maxPages: 4, maxDepth: 3, concurrency: 4 })
                       .start(BASE + "/a", ex(), HTMLParse);
        let n = 0;
        for await (const p of c) n++;
        c.close(); f.close();
        sh("sleep 0.2");
        const stamps = arrivals().slice(mark).filter((x) => x[1] !== "/robots.txt").map((x) => x[0]);
        stamps.sort((a, b) => a - b);
        let minGap = Infinity;
        for (let i = 1; i < stamps.length; i++) minGap = Math.min(minGap, (stamps[i] - stamps[i - 1]) * 1000);
        ok(n === 4 && stamps.length >= 4 && minGap >= 240,
           "P76 per-host floor holds at concurrency 4: same-host starts >= 240ms apart (min gap " +
           minGap.toFixed(0) + "ms over " + stamps.length + " fetches)");
    });

    /* ---- P80: async iterator protocol: next() after done ---- */
    await block("P80", async () => {
        const f = mkF();
        const c = new Crawl(f, { maxPages: 1, concurrency: 2 }).start(BASE + "/a", ex(), HTMLParse);
        await c.next();                            /* the one page */
        const r = await c.next();                  /* done:true */
        ok(r && r.done === true, "P80 a bounded concurrent crawl reports done:true");
        let settled = null;
        c.next().then((v) => { settled = v; }, (e) => { settled = "ERR " + e; });
        await new Promise((res) => setTimeout(res, 500));
        ok(settled !== null && settled !== undefined && settled.done === true,
           "P80b next() AFTER done settles again as {done:true} (iterator protocol)",
           settled === null ? "next() after done:true NEVER SETTLES (async iterator hang)"
                            : "settled with " + JSON.stringify(settled) + " (a done:false here is an infinite-drain hazard)");
        c.close(); f.close();
    });

    /* ---- P77/P78: serialize()/resume() round-trip + in-flight pages.
     * The mid-flight close is performed LAST and nothing network-y runs
     * after it: a closed fetcher's late completions crash whatever code is
     * running at the time (pre-existing engine bug, flaky by nature). ---- */
    await block("P77", async () => {
        const mark = arrivals().length;
        const f = mkF();
        const c = new Crawl(f, { maxPages: 50, maxDepth: 3, concurrency: 2 })
                       .start(BASE + "/a", ex(), HTMLParse);
        const first = await c.next();            /* /a emitted */
        await new Promise((res) => setTimeout(res, 30));   /* 2 flights start */
        sh("sleep 0.25");
        const askedBefore = new Set(arrivals().slice(mark).map((x) => x[1]));
        const st = JSON.parse(c.serialize());
        ok(st.concurrency === 2,
           "P77 serialize() carries the concurrency (" + st.concurrency + ")");
        ok(Array.isArray(st.q) && st.q.length >= 1,
           "P77c the frontier still lists undispatched pages (" + JSON.stringify(st.q) + ")");
        /* resume + drain FIRST, while the original is still open */
        const f2 = mkF();
        const c2 = Crawl.resume(f2, JSON.stringify(st), ex(), HTMLParse);
        const r2 = c2.next();
        ok(r2 && typeof r2.then === "function",
           "P77b resume() keeps concurrency " + st.concurrency + " (next() still promise-shaped)");
        const emitted = [];
        const p0 = await r2;
        if (!p0.done && p0.value) emitted.push(p0.value.url.replace(BASE, ""));
        for (let i = 0; i < 20; i++) {           /* bounded drain, never past done */
            const r = await c2.next();
            if (r.done) break;
            if (r.value) emitted.push(r.value.url.replace(BASE, ""));
        }
        const firstPath = first && first.value && first.value.url
            ? "/" + first.value.url.replace(BASE, "").split("/").filter(Boolean).pop() : "?";
        const inFlight = [...askedBefore].filter((u) => u !== "/robots.txt" && u !== firstPath);
        const lost = inFlight.filter((u) => emitted.indexOf(u) === -1);
        const refetched = inFlight.filter((u) => emitted.indexOf(u) !== -1);
        ok(new Set(emitted).size === emitted.length,
           "P78 resume() emits no duplicates (" + emitted.length + " pages)");
        info("P78 state=" + JSON.stringify(st) +
             " in-flight at serialize: " + JSON.stringify(inFlight) +
             "; emitted after resume: " + JSON.stringify(emitted) +
             "; DROPPED=" + JSON.stringify(lost) + " REFETCHED=" + JSON.stringify(refetched));
        ok(lost.length >= 1 && refetched.length === 0,
           "P78b pages mid-fetch at serialize are dropped on resume (documented)",
           "lost=" + JSON.stringify(lost) + " refetched=" + JSON.stringify(refetched));
        c2.close(); f2.close();
        /* SACRIFICIAL LAST: close mid-flight; the pending next() must settle
         * (done or a page that won the race -- which one is timing, so only
         * settlement is asserted, the shape is INFO) */
        const pending = c.next();
        c.close();
        const pr = await pending;
        ok(pr !== undefined && pr !== null && typeof pr === "object",
           "P78c close mid-flight: the pending next() settles");
        info("P78c pending settled with " + JSON.stringify(pr && pr.done) + "/" +
             (pr && pr.value ? "page" : String(pr && pr.value)));
        f.close();
    });

    client.close();
    finish();
    writeFile(new Path(P("done")), "1");
    sh(`pkill -f '${P("mock.py")}'`);
}
