import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { HTMLParse, Selector, HTMLText } from "dyna:html";
import { HTTPClient } from "dyna:net";
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, readFile, removeAll, Path } from "dyna:file";

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
    skipped("python3 missing -- the link graph cannot be served");
    print("test_scrape_crawl: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_scrape_crawl: " + fail + " failures");
} else {
    const T = makeTempDir("crawl");
    const P = (n) => T + "/" + n;
    writeFile(new Path(P("mock.py")), [
        "import http.server, sys",
        "import threading as _th, os as _os",
        "# SELF-BOUND. The test's pkill is NOT enough: a test killed by a",
        "# timeout never reaches its cleanup, and this then outlives it.",
        "# Measured: three of these were found still running after 5h.",
        "_th.Timer(180, lambda: _os._exit(0)).start()",
        "G={'/a':['/b','/c'], '/b':['/d'], '/c':['/d'], '/d':['/a']}",
        "hits={}",
        "class H(http.server.BaseHTTPRequestHandler):",
        "    protocol_version='HTTP/1.1'",
        "    def log_message(self,*a): pass",
        "    def do_GET(self):",
        "        p=self.path; hits[p]=hits.get(p,0)+1",
        "        if p=='/robots.txt':",
        "            b=b'User-agent: *\\nAllow: /\\n'",
        "        elif p=='/hits':",
        "            import json; b=json.dumps(hits).encode()",
        "        else:",
        "            port=srv.server_address[1]",
        "            links=''.join('<a href=\"http://127.0.0.1:%d%s\">x</a>'%(port,q) for q in G.get(p,[]))",
        "            links+='<a href=\"http://example.invalid/off\">off</a>'",
        "            b=('<html><body><h1>%s</h1>%s</body></html>'%(p,links)).encode()",
        "        self.send_response(200)",
        "        self.send_header('Content-Length',str(len(b)))",
        "        self.end_headers(); self.wfile.write(b)",
        "srv=http.server.ThreadingHTTPServer(('127.0.0.1',0), H)",
        "open(sys.argv[1],'w').write(str(srv.server_address[1]))",
        "srv.serve_forever()",
    ].join("\n"));
    sh(`python3 ${P("mock.py")} ${P("port")} >${P("mock.log")} 2>&1 &`);
    let PORT = 0;
    for (let i = 0; i < 60 && !PORT; i++) {
        sh("sleep 0.1");
        try { PORT = parseInt(readFile(new Path(P("port"))), 10) || 0; }
        catch (e) {  }
    }
    const BASE = "http://127.0.0.1:" + PORT;

    const client = new HTTPClient();
    client.setTimeout(5000);
    const mkF = () => new Fetcher({ agent: "dynabot/1.0 (+https://example.test/bot)",
                                    client, minDelayMs: 0, robots: false,
                                    allowPrivateHosts: true });
    const ex = () => new Extractor({
        title: { sel: new Selector("h1") },
        links: { sel: new Selector("a"), attr: "href", all: true },
    }, { text: HTMLText });

    const drain = (c) => { const out = []; for (const p of c) out.push(p); return out; };

    {
        const f = mkF();
        const pages = drain(new Crawl(f, { maxPages: 50, maxDepth: 5 }).start(BASE + "/a", ex(), HTMLParse));
        const urls = pages.map((p) => p.url.replace(BASE, ""));
        ok(pages.length === 4, "the 4-page graph is fully traversed (got " +
           pages.length + ": " + urls.join(",") + ")");
        ok(urls.indexOf("/a") === 0, "the seed is first");
        ok(pages.every((p) => p.status === 200), "every page reports its status");
        ok(pages[0].value && pages[0].value.title === "/a",
           "the extractor's value rides along on the page");
        f.close();
    }

    {
        const f = mkF();
        const pages = drain(new Crawl(f, { maxPages: 50, maxDepth: 9 }).start(BASE + "/a", ex(), HTMLParse));
        ok(pages.length === 4, "the /d -> /a cycle terminates (" + pages.length +
           " pages, not the bound of 50)");
        const seen = new Set(pages.map((p) => p.url));
        ok(seen.size === pages.length, "and no url is emitted twice");
        f.close();
    }

    {
        const f = mkF();
        const pages = drain(new Crawl(f, { maxPages: 50, maxDepth: 1 }).start(BASE + "/a", ex(), HTMLParse));
        const urls = pages.map((p) => p.url.replace(BASE, "")).sort().join(",");
        ok(urls === "/a,/b,/c", "maxDepth:1 stops before /d (got " + urls + ")");
        ok(Math.max(...pages.map((p) => p.depth)) === 1, "and no page reports depth > 1");
        f.close();
    }

    {
        const f = mkF();
        const pages = drain(new Crawl(f, { maxPages: 2, maxDepth: 9 }).start(BASE + "/a", ex(), HTMLParse));
        ok(pages.length === 2, "maxPages:2 emits exactly 2 (got " + pages.length + ")");
        f.close();
    }

    {
        const f = mkF();
        const pages = drain(new Crawl(f, { maxPages: 50, maxDepth: 9 }).start(BASE + "/a", ex(), HTMLParse));
        ok(pages.every((p) => p.url.indexOf("127.0.0.1") >= 0),
           "sameHost keeps the crawl on the seed host");
        const g = mkF();
        let tried = false;
        try {
            for (const p of new Crawl(g, { maxPages: 6, maxDepth: 1, sameHost: false })
                                .start(BASE + "/a", ex(), HTMLParse)) {
                if (p.url.indexOf("example.invalid") >= 0) tried = true;
            }
        } catch (e) { tried = true; }
        ok(tried, "CONTROL: with sameHost:false the off-host link IS attempted");
        f.close(); g.close();
    }

    {
        const f = mkF();
        const it = new Crawl(f, { maxPages: 50, maxDepth: 9 }).start(BASE + "/a", ex(), HTMLParse);
        const before = JSON.parse(mkF().get(BASE + "/hits").body);
        it.next();
        const after = JSON.parse(mkF().get(BASE + "/hits").body);
        const d = (after["/a"] || 0) - (before["/a"] || 0);
        ok(d === 1, "one next() causes exactly one page fetch (delta " + d + ")");
        ok((after["/d"] || 0) === (before["/d"] || 0),
           "and nothing deeper was fetched eagerly");
        f.close();
    }

    {
        const f = mkF();
        const pages = drain(new Crawl(f, { maxPages: 50, maxDepth: 9 }).start(BASE + "/a"));
        ok(pages.length === 1, "without an extractor only the seed is fetched " +
           "(links come from the extractor, not a second HTML scanner)");
        f.close();
    }

    sh(`pkill -f '${P("mock.py")}' 2>/dev/null`);
    removeAll(new Path(T));
    print("test_scrape_crawl: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_scrape_crawl: " + fail + " failures");
}
