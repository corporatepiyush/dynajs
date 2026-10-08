import { Fetcher } from "dyna:scrape";
import { HTTPClient } from "dyna:net";
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, readFile, removeAll, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function throws(fn, w) {
    let t = false, m = "";
    try { fn(); } catch (e) { t = true; m = String(e.message || e); }
    ok(t, w, t ? "" : "did NOT throw");
    return m;
}
function skipped(w) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; }
    skip++; print("  SKIP  " + w);
}
const sh = (c) => Exec("/bin/sh", ["-c", c]).code;

{
    const c = new HTTPClient();
    let m = throws(() => new Fetcher({}), "a Fetcher without `agent` is refused");
    ok(/agent/i.test(m) && /default/i.test(m),
       "and the error explains WHY there is no default agent");
    {
        const auto = new Fetcher({ agent: "bot/1.0" });
        ok(auto && !auto.closed, "a Fetcher WITHOUT `client` now auto-builds one (SP-3)");
        auto.close();
    }
    throws(() => new Fetcher({ agent: "", client: c }),
           "an empty agent is refused, not accepted as present");
    c.close();
}

{
    const never = { request() { throw new Error("the request was issued"); } };
    const f = new Fetcher({ agent: "dynabot/1.0", client: never, robots: false,
                            allowPrivateHosts: true,
                            headers: { "X-Note": "a\r\nX-Evil: 1" } });
    let m = throws(() => f.get("http://127.0.0.1:9/x"),
                   "a CR/LF in a header value is refused");
    ok(/header/i.test(m) && /X-Note/i.test(m),
       "and the error names the offending header");
    throws(() => f.get("http://127.0.0.1:9/x"),
           "every request re-checks (the value is not smuggled through on a retry)");
    f.close();
    const g = new Fetcher({ agent: "dynabot/1.0", client: never, robots: false,
                            allowPrivateHosts: true,
                            headers: { "X-Note": "perfectly fine" } });
    m = throws(() => g.get("http://127.0.0.1:9/x"),
               "CONTROL: a clean header value is not refused");
    ok(/issued/.test(m),
       "CONTROL: the stub client saw the request, so the guard did not overfire");
    g.close();
}

if (!Which("python3")) {
    skipped("python3 missing -- the mock peer cannot run, and no policy is proved");
    print("test_scrape_fetcher: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_scrape_fetcher: " + fail + " failures");
} else {
    const T = makeTempDir("fetcher");
    const P = (n) => T + "/" + n;
    writeFile(new Path(P("mock.py")), [
        "import http.server, sys, threading",
        "import threading as _th, os as _os",
        "# SELF-BOUND. The test's pkill is NOT enough: a test killed by a",
        "# timeout never reaches its cleanup, and this then outlives it.",
        "# Measured: three of these were found still running after 5h.",
        "_th.Timer(180, lambda: _os._exit(0)).start()",
                "hits={}",
        "MODE = sys.argv[3] if len(sys.argv) > 3 else 'std'",
        "class H(http.server.BaseHTTPRequestHandler):",
        "    protocol_version='HTTP/1.1'",
        "    def log_message(self,*a): pass",
        "    def reply(self, code, body=b'', extra=None):",
        "        self.send_response(code)",
        "        self.send_header('Content-Length', str(len(body)))",
        "        for k,v in (extra or {}).items(): self.send_header(k,v)",
        "        self.end_headers()",
        "        if body: self.wfile.write(body)",
        "    def do_GET(self):",
        "        p=self.path",
        "        hits[p]=hits.get(p,0)+1",
        "        if p=='/robots.txt':",
        "            if MODE=='open': return self.reply(200, b'User-agent: *\\nAllow: /\\n')",
        "            if MODE=='dead': return self.reply(302, b'', {'Location': 'http://127.0.0.1:9/robots.txt'})",
        "            return self.reply(200, b'User-agent: *\\nDisallow: /private\\nCrawl-delay: 1\\n')",
        "        if p=='/hits':",
        "            import json; return self.reply(200, json.dumps(hits).encode())",
        "        if p=='/private': return self.reply(200, b'SECRET')",
        "        if p.startswith('/retry'):",
        "            if hits[p] < 3: return self.reply(429, b'slow down', {'Retry-After':'1'})",
        "            return self.reply(200, b'ok after retries')",
        "        if p.startswith('/redir/'):",
        "            n=int(p.split('/')[2])",
        "            if n>0: return self.reply(302, b'', {'Location': 'http://127.0.0.1:%d/redir/%d'%(srv.server_address[1],n-1)})",
        "            return self.reply(200, b'arrived')",
        "        if p=='/big': return self.reply(200, b'x'*20000)",
        "        return self.reply(200, b'hello')",
        "srv=http.server.ThreadingHTTPServer(('127.0.0.1',0), H)",
        "PORT=srv.server_address[1]",
        "open(sys.argv[2],'w').write(str(PORT))",
        "srv.serve_forever()",
    ].join("\n"));
    sh(`python3 ${P("mock.py")} x ${P("port")} std >${P("mock.log")} 2>&1 &`);
    sh(`python3 ${P("mock.py")} x ${P("port2")} open >${P("mock2.log")} 2>&1 &`);
    sh(`python3 ${P("mock.py")} x ${P("port3")} dead >${P("mock3.log")} 2>&1 &`);
    let PORT = 0, PORT2 = 0, PORT3 = 0;
    for (let i = 0; i < 60 && !(PORT && PORT2 && PORT3); i++) {
        sh("sleep 0.1");
        try { PORT = parseInt(readFile(new Path(P("port"))), 10) || 0; }
        catch (e) {  }
        try { PORT2 = parseInt(readFile(new Path(P("port2"))), 10) || 0; }
        catch (e) {}
        try { PORT3 = parseInt(readFile(new Path(P("port3"))), 10) || 0; }
        catch (e) {}
    }
    if (!PORT) {
        skipped("the mock peer did not start");
        print("test_scrape_fetcher: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
        if (fail) throw new Error("test_scrape_fetcher: " + fail + " failures");
    }
    const BASE = "http://127.0.0.1:" + PORT;
    const BASE2 = "http://127.0.0.1:" + PORT2;
    const BASE3 = "http://127.0.0.1:" + PORT3;

    const client = new HTTPClient();
    client.setTimeout(5000);
    const mk = (o) => new Fetcher(Object.assign(
        { agent: "dynabot/1.0 (+https://example.test/bot)", client, minDelayMs: 0,
          allowPrivateHosts: true }, o));

    {
        const f = mk({});
        const r = f.get(BASE + "/private");
        ok(r.skippedByRobots === true && r.status === 0,
           "a Disallowed path is SKIPPED, not fetched");
        ok(f.stats().skippedByRobots === 1, "and stats counts it");
        const r2 = f.get(BASE + "/allowed");
        ok(r2.status === 200, "CONTROL: an allowed path on the same host fetches");
        const h = JSON.parse(f.get(BASE + "/hits").body);
        ok(!h["/private"], "the server never received a request for /private " +
           "(so the skip happened BEFORE the socket, not after)");
        f.close();
    }

    {
        const f = mk({ robots: false });
        const r = f.get(BASE + "/private");
        ok(r.status === 200 && /SECRET/.test(r.body),
           "with robots:false the same path IS fetched -- so the skip above " +
           "was the robots gate and not something else");
        f.close();
    }

    {
        const f = mk({ minDelayMs: 0 });
        f.get(BASE + "/a");
        const t0 = Date.now();
        f.get(BASE + "/b");
        const dt = Date.now() - t0;
        ok(dt >= 900, "Crawl-delay:1 RAISES a 0ms floor (waited " + dt + "ms)");
        f.close();

        const g = mk({ minDelayMs: 3000, robots: false });
        g.get(BASE + "/a");
        const s0 = Date.now();
        g.get(BASE + "/b");
        ok(Date.now() - s0 >= 2800,
           "and a larger caller floor is not lowered by anything");
        g.close();
    }

    {
        const f = mk({ retries: 5 });
        const t0 = Date.now();
        const r = f.get(BASE + "/retry");
        const dt = Date.now() - t0;
        ok(r.status === 200 && /after retries/.test(r.body),
           "a 429 is retried until it succeeds");
        ok(f.stats().retried >= 2, "and stats counts the retries (" +
           f.stats().retried + ")");
        ok(dt >= 1800, "Retry-After:1 was honoured twice (" + dt + "ms), " +
           "not the exponential curve, which would have been faster here");
        f.close();
    }

    {
        const f = mk({ retries: 0 });
        const r = f.get(BASE + "/retry2");
        ok(r.status === 429, "with retries:0 the 429 is returned as-is");
        f.close();
    }

    {
        const f = mk({ maxRedirects: 5 });
        const r = f.get(BASE + "/redir/3");
        ok(r.status === 200 && /arrived/.test(r.body),
           "a 3-hop redirect chain is followed to the end");
        ok(/redir\/0/.test(r.url), "and the reported url is the FINAL one (" +
           r.url + ")");
        const m = throws(() => f.get(BASE + "/redir/9"),
                         "a chain past maxRedirects is refused");
        ok(/redirect/i.test(m), "and the error names redirects");
        f.close();
    }

    {
        const f = mk({ maxBodyBytes: 1000 });
        const m = throws(() => f.get(BASE + "/big"),
                         "a body past maxBodyBytes is refused");
        ok(/maxBodyBytes|exceeds/i.test(m), "and the error names the cap");
        const g = mk({ maxBodyBytes: 100000 });
        ok(g.get(BASE + "/big").status === 200,
           "CONTROL: the same body under a larger cap is accepted");
        f.close(); g.close();
    }

    {
        const f = mk({});
        f.get(BASE + "/allowed");
        const s = f.stats();
        ok(s.fetched >= 1 && s.bytes > 0, "stats reports fetched and bytes");
        f.close();
        throws(() => f.get(BASE + "/allowed"), "get() after close() is refused");
    }

    {
        const strict = new Fetcher({ agent: "dynabot/1.0", client, minDelayMs: 0,
                                     robots: false });
        const m = throws(() => strict.get(BASE + "/allowed"),
                         "a loopback URL is refused without allowPrivateHosts");
        ok(/private\/loopback/i.test(m), "and the error names the class of host");
        ok(/allowPrivateHosts/.test(m), "and names the opt-in");
        strict.close();
        const opted = mk({ allowPrivateHosts: true });
        ok(opted.get(BASE + "/allowed").status === 200,
           "CONTROL: allowPrivateHosts: true still fetches the mock");
        opted.close();
    }

    {
        const f = mk({});
        ok(f.get(BASE2 + "/private").status === 200,
           "PIN: robots are per ORIGIN -- :PORT2's robots allow /private");
        ok(f.get(BASE + "/private").skippedByRobots === true,
           "CONTROL: :PORT's robots still disallow /private");
        f.close();
    }
    {
        const f = mk({ robotsTtlMs: 3600000 });
        f.get(BASE3 + "/allowed");
        f.get(BASE3 + "/allowed");
        const probe = mk({ robots: false });
        const h = JSON.parse(probe.get(BASE3 + "/hits").body);
        probe.close();
        ok(h["/robots.txt"] === 1,
           "S4: a robots network error is remembered for the TTL (" +
           h["/robots.txt"] + " loads for 2 fetches)");
        ok(f.stats().skippedByRobots === 2, "and the failure stays fail-closed (both page fetches skipped)");
        f.close();
    }

    sh(`pkill -f '${P("mock.py")}' 2>/dev/null`);
    removeAll(new Path(T));
    print("test_scrape_fetcher: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_scrape_fetcher: " + fail + " failures");
}
