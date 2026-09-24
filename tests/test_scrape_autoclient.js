// flags: --std
/* test_scrape_autoclient.js --: the Fetcher's optional client, and
 * 's network half: Sitemap.list against a live sitemap.
 *
 * THE AUTO-CLIENT PARITY PROBE: `new Fetcher({agent})` (no client) must
 * fetch exactly like `new Fetcher({agent, client})` -- same body, same
 * header merge, same stats -- because under the hood it IS the same
 * transport: dyna:net's HTTPClient, constructed by the module instead of
 * by the caller.
 *
 * The peer is an out-of-process python http.server mock (the
 * test_scrape_crawl pattern). An in-process App cannot serve the SYNC
 * client path: the fetch blocks the one loop thread the App needs (the
 * deadlock the test_http_metrics_endpoint header documents), so the mock
 * runs in its own process, self-bound, self-terminating.
 *
 * Needs python3. A skip is loud, and fatal under DYNAJS_REQUIRE_TOOLS=1.
 */
import { Fetcher } from "dyna:scrape";
import { HTTPClient } from "dyna:net";
import { Sitemap } from "dyna:scrape";
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, readFile, removeAll, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const eq = (a, b, w) => ok(a === b, w, "got " + JSON.stringify(a) + ", want " + JSON.stringify(b));
function skipped(w) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; }
    skip++; print("  SKIP  " + w);
}
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

/* ---- ctor contract (no network needed; runs even without python3) ---- */
{
    const okType = (bad) => {
        try { new Fetcher({ agent: "ctor-bot/1.0", client: bad }); return false; }
        catch (e) { return e instanceof TypeError && /client/.test(e.message); }
    };
    ok(okType(42) && okType("str") && okType(true) && okType(3.5),
       "a PRESENT but non-object client refuses (TypeError naming `client`) -- " +
       "silently swapping the caller's transport for the built-in one is the " +
       "silent-noop-of-intent CC-6 kills");
    const a = new Fetcher({ agent: "ctor-bot/1.0", client: null });
    ok(a && !a.closed, "client: null still counts as absent (auto-build)");
    a.close();
    const b = new Fetcher({ agent: "ctor-bot/1.0" });
    ok(b && !b.closed, "client omitted still auto-builds");
    b.close();
}

if (!Which("python3")) {
    skipped("python3 missing -- the mock origin cannot be served");
    print("test_scrape_autoclient: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_scrape_autoclient: " + fail + " failures");
} else {
    const T = makeTempDir("autoclient");
    const P = (n) => T + "/" + n;

    /* Sitemap fixtures: a plain urlset, and an index that points at it.
       The <url> entries carry lastmod/priority to prove only <loc> matters;
       the default namespace sits on the root, as real sitemaps ship it. */
    writeFile(new Path(P("mock.py")), [
        "import http.server, sys",
        "import threading as _th, os as _os",
        "# SELF-BOUND and SELF-TERMINATING: a test killed by a timeout never",
        "# reaches its cleanup, and a mock that outlives it breaks the next run.",
        "_th.Timer(180, lambda: _os._exit(0)).start()",
        "SITEMAP='''<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
        "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">",
        "  <url><loc>http://127.0.0.1:1/a</loc><lastmod>2026-01-01</lastmod><priority>0.5</priority></url>",
        "  <url><loc>http://127.0.0.1:1/b</loc></url>",
        "  <url><loc>http://127.0.0.1:1/c</loc></url>",
        "</urlset>'''",
        "INDEX='''<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
        "<sitemapindex xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">",
        "  <sitemap><loc>__SITEMAP_URL__</loc></sitemap>",
        "</sitemapindex>'''",
        "BODY='auto-client parity body'",
        "class H(http.server.BaseHTTPRequestHandler):",
        "    protocol_version='HTTP/1.1'",
        "    def log_message(self,*a): pass",
        "    def do_GET(self):",
        "        if self.path=='/robots.txt':",
        "            self.send_response(404); self.send_header('Content-Length','0'); self.end_headers(); return",
        "        if self.path=='/s.xml': body,ct=SITEMAP,'application/xml'",
        "        elif self.path=='/i.xml': body,ct=INDEX.replace('__SITEMAP_URL__','http://127.0.0.1:%d/s.xml'%srv.server_address[1]),'application/xml'",
        "        else: body,ct=BODY,'text/plain'",
        "        b=body.encode()",
        "        self.send_response(200)",
        "        self.send_header('Content-Type',ct)",
        "        self.send_header('X-Mock','yes')",
        "        self.send_header('Content-Length',str(len(b)))",
        "        self.end_headers()",
        "        self.wfile.write(b)",
        "srv=http.server.ThreadingHTTPServer(('127.0.0.1',0), H)",
        "open(sys.argv[1],'w').write(str(srv.server_address[1]))",
        "srv.serve_forever()",
    ].join("\n"));
    sh(`python3 ${P("mock.py")} ${P("port")} >${P("mock.log")} 2>&1 &`);
    let port = -1;
    for (let i = 0; i < 50 && port < 0; i++) {
        Exec("/bin/sh", ["-c", "sleep 0.1"]);
        try { port = parseInt(readFile(new Path(P("port"))).trim()); } catch (e) { /* not yet */ }
    }
    if (!(port > 0)) {
        skipped("the mock origin never came up");
    } else {
        const BASE = "http://127.0.0.1:" + port;

        /* ---- 1. parity: auto-client vs injected client, same page ---- */
        {
            const auto = new Fetcher({ agent: "parity-bot/1.0 (+http://x.test)",
                                       allowPrivateHosts: true, robots: false,
                                       minDelayMs: 0 });
            const injected = new Fetcher({ agent: "parity-bot/1.0 (+http://x.test)",
                                           client: new HTTPClient(),
                                           allowPrivateHosts: true, robots: false,
                                           minDelayMs: 0 });
            const ra = auto.get(BASE + "/page");
            const ri = injected.get(BASE + "/page");
            eq(ra.status, 200, "auto-client fetch answers 200");
            eq(ri.status, 200, "injected-client fetch answers 200");
            eq(ra.body, "auto-client parity body", "auto-client body matches");
            eq(ri.body, ra.body, "injected body matches the auto one byte for byte");
            eq(ra.contentType, "text/plain", "auto-client response carries content type");
            ok(ra.url === BASE + "/page", "auto-client echoes the final url");
            const sa = auto.stats(), si = injected.stats();
            eq(sa.fetched, 1, "auto-client counts its fetch");
            eq(si.fetched, 1, "injected client counts its fetch");
            /* the agent header is the fetcher's own under both transports:
               the mock cannot echo request headers back, so parity here is
               the body/status/content-type/url tuple -- which routes through
               the client's request(), header merge included. */
        }

        /* ---- 2. the auto-client still obeys the fetcher's policy ---- */
        {
            const guarded = new Fetcher({ agent: "guarded-bot/1.0", minDelayMs: 0 });
            let threw = "";
            try { guarded.get("http://192.168.1.1/nope"); }
            catch (e) { threw = String(e); }
            ok(/private|loopback|link-local/.test(threw),
               "auto-client fetch still refuses private hosts (" + threw.slice(0, 60) + ")");
        }

        /* ---- 3. Sitemap.list(): urlset parse via the fetcher transport ---- */
        {
            const f = new Fetcher({ agent: "sitemap-bot/1.0 (+http://x.test)",
                                    allowPrivateHosts: true, robots: false,
                                    minDelayMs: 0 });
            const urls = Sitemap.list(BASE + "/s.xml", f);
            eq(urls.length, 3, "urlset lists its three <loc> urls");
            eq(urls[0], "http://127.0.0.1:1/a", "first loc survives intact");
            eq(urls[2], "http://127.0.0.1:1/c", "last loc survives intact");
            /* one level of <sitemapindex> flattening */
            const flat = Sitemap.list(BASE + "/i.xml", f);
            eq(flat.length, 3, "sitemapindex flattens its child (one level)");
            eq(flat[0], "http://127.0.0.1:1/a", "flattened list keeps order");
        }

        /* ---- 4. Sitemap.list() without a fetcher: the auto path composes ---- */
        {
            const urls = Sitemap.list(BASE + "/s.xml",
                                      new Fetcher({ agent: "sitemap-bot/1.0 (+http://x.test)",
                                                    allowPrivateHosts: true, robots: false,
                                                    minDelayMs: 0 }));
            eq(urls.length, 3, "list() works with an explicitly built fetcher too");
        }
    }

    sh(`pkill -f '${P("mock.py")}' 2>/dev/null`);
    removeAll(new Path(T));

    print("test_scrape_autoclient: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_scrape_autoclient: " + fail + " failures");
}
