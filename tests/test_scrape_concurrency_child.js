/* test_scrape_concurrency_child.js -- the scenario runner, spawned by
 * test_scrape_concurrency.js with the shared tmpdir as argv[1]. Writes
 * "SCRESULT: <pass> <fail>" into <tmpdir>/result as its verdict. The parent
 * reads that FILE: this process's teardown asserts on long-lived
 * async-iterator records at shutdown, and the engine's stdout buffer is
 * lost on that abort -- the file is the only durable channel.
 */
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { HTMLParse, Selector, HTMLText } from "dyna:html";
import { HTTPClient } from "dyna:net";
import { Exec } from "dyna:sys";
import { writeFile, readFile, Path } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const T = scriptArgs[1];
const P = (n) => T + "/" + n;
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;
writeFile(new Path(P("mock.py")), [
    "import http.server, sys, time, threading, threading as _th, os as _os",
    "_th.Timer(180, lambda: _os._exit(0)).start()",
    "G={'/a':['/b','/c','/d'], '/b':['/e'], '/c':['/f'], '/d':['/g'], '/e':[], '/f':[], '/g':[]}",
    "arrivals={}",
    "lock=threading.Lock()",
    "class H(http.server.BaseHTTPRequestHandler):",
    "    protocol_version='HTTP/1.1'",
    "    def log_message(self,*a): pass",
    "    def do_GET(self):",
    "        p=self.path",
    "        with lock: arrivals[p]=arrivals.get(p,0)+1",
    "        if p=='/robots.txt': b=b'User-agent: *\\nAllow: /\\n'",
    "        else:",
    "            time.sleep(0.12)",
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
    sh(`python3 ${P("mock.py")} ${P("port")} >${P("mock.log")} 2>&1 &`);
    for (let i = 0; i < 100 && !PORT; i++) {
        sh("sleep 0.1");
        try { PORT = parseInt(readFile(new Path(P("port"))), 10) || 0; } catch (e) {}
    }
    if (!PORT) sh(`pkill -f '${P("mock.py")}'`);
}
const finish = () => writeFile(new Path(P("result")),
                               "SCRESULT: " + pass + " " + fail);
if (!PORT) {
    finish();
} else {
const BASE = "http://127.0.0.1:" + PORT;
const client = new HTTPClient();
client.setTimeout(8000);
const mkF = () => new Fetcher({ agent: "dynaconc/1.0 (+https://example.test/bot)",
                                client, minDelayMs: 0, robots: false,
                                allowPrivateHosts: true });
const ex = () => new Extractor({
    title: { sel: new Selector("h1") },
    links: { sel: new Selector("a"), attr: "href", all: true },
}, { text: HTMLText });

/* the serial control: same graph, no concurrency */
const ctrlUrls = (() => {
    const f = mkF();
    const c = new Crawl(f, { maxPages: 50, maxDepth: 3 }).start(BASE + "/a", ex(), HTMLParse);
    const urls = [];
    for (const p of c) urls.push(p.url.replace(BASE, ""));
    c.close(); f.close(); return urls;
})();
ok(ctrlUrls.length === 7, "control: the 7-page graph crawls serially (" + ctrlUrls.length + ")");

/* parallel: same set, wall clock shows the overlap */
{
    const f = mkF();
    const c = new Crawl(f, { maxPages: 50, maxDepth: 3, concurrency: 4 })
                   .start(BASE + "/a", ex(), HTMLParse);
    const t0 = Date.now();
    const pages = [];
    for await (const p of c) pages.push(p);
    const ms = Date.now() - t0;
    const set = new Set(pages.map(p => p.url.replace(BASE, "")));
    ok(pages.length === 7, "parallel: 7 pages emitted (" + pages.length + ")");
    ok(set.size === 7 && ctrlUrls.every(u => set.has(u)),
       "parallel: the page SET equals the serial control's");
    ok(pages.every(p => p.status === 200 && p.value && p.value.title),
       "parallel: every page carries status + extracted value");
    ok(ms < 7 * 110, "parallel: wall clock " + ms + "ms shows overlap " +
       "(serial would exceed " + (7 * 110) + "ms)");
    c.close(); f.close();
}

/* per-host politeness: the floor serializes same-host STARTS */
{
    const f = new Fetcher({ agent: "dynaconc/1.0", client, minDelayMs: 250,
                            robots: false, allowPrivateHosts: true });
    const c = new Crawl(f, { maxPages: 3, maxDepth: 1, concurrency: 3 })
                   .start(BASE + "/a", ex(), HTMLParse);
    const t0 = Date.now();
    let n = 0;
    for await (const p of c) n++;
    const ms = Date.now() - t0;
    ok(n === 3, "polite: 3 pages within the budget (" + n + ")");
    ok(ms >= 500, "polite: same-host fetches start a floor apart " +
       "(" + ms + "ms >= 500ms for 3 x 250ms floors)");
    c.close(); f.close();
}

/* budget: maxPages bounds EMITTED pages under concurrency */
{
    const f = mkF();
    const c = new Crawl(f, { maxPages: 3, maxDepth: 3, concurrency: 4 })
                   .start(BASE + "/a", ex(), HTMLParse);
    let n = 0;
    for await (const p of c) n++;
    ok(n === 3, "budget: maxPages 3 yields exactly 3 pages (" + n + ")");
    c.close(); f.close();
}

/* serialize DURING concurrency; close DURING in-flight */
{
    const f = mkF();
    const c = new Crawl(f, { maxPages: 50, maxDepth: 3, concurrency: 2 })
                   .start(BASE + "/a", ex(), HTMLParse);
    await c.next();                       /* one page done */
    await new Promise(res => setTimeout(res, 40));  /* flights start */
    const st = c.serialize();
    const parsed = JSON.parse(st);
    ok(parsed.concurrency === 2,
       "serialize mid-flight: the state carries the concurrency");
    ok(Array.isArray(parsed.q) && parsed.q.length >= 1,
       "serialize mid-flight: the frontier is valid JSON with pages left " +
       "(" + parsed.q.length + ")");
    const pending = c.next();
    c.close();
    const r = await pending;
    ok(r.done === true, "close mid-flight: the pending next() settles as done");
    f.close();
}

client.close();
finish();
}
