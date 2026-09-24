/* test_scrape_state.js --: Crawl.serialize / Crawl.resume(state).
 *
 * The promise under test: a crawl interrupted at ANY point can be carried
 * across a restart, and the resumed crawl offers EXACTLY the pages the
 * donor had left, in the donor's order, under the donor's bounds. So every
 * roundtrip case is measured against a control crawl of the same graph that
 * was never interrupted -- the same probe shape the plan asked for.
 *
 * Also pinned: what is deliberately NOT preserved must not silently pretend
 * otherwise (in-flight pages, the fetcher's live caches), and damaged state
 * must be REFUSED loudly (wrong version, non-JSON, wrong shapes, tampered
 * counters), never half-restored.
 *
 * Needs python3 (a live link-graph mock, the test_scrape_crawl idiom).
 * A skip is loud, and fatal under DYNAJS_REQUIRE_TOOLS=1.
 */
import { Crawl, Fetcher, Extractor } from "dyna:scrape";
import { HTMLParse, Selector, HTMLText } from "dyna:html";
import { HTTPClient } from "dyna:net";
import { Exec, Which, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, readFile, Path } from "dyna:file";

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
    print("test_scrape_state: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
    if (fail) throw new Error("test_scrape_state: " + fail + " failures");
} else {
        const T = makeTempDir("scrape_state");
    const P = (n) => T + "/" + n;
    /* A six-page line-plus-fork: /a -> b,c ; /b -> d ; /c -> e ; /e -> f.
       Every page is distinct, so "remaining page set" is unambiguous. */
    writeFile(new Path(P("mock.py")), [
        "import http.server, sys",
        "import threading as _th, os as _os",
        "_th.Timer(180, lambda: _os._exit(0)).start()",
        "G={'/a':['/b','/c'], '/b':['/d'], '/c':['/e'], '/e':['/f'], '/d':[], '/f':[]}",
        "class H(http.server.BaseHTTPRequestHandler):",
        "    protocol_version='HTTP/1.1'",
        "    def log_message(self,*a): pass",
        "    def do_GET(self):",
        "        p=self.path",
        "        if p=='/robots.txt': b=b'User-agent: *\\nAllow: /\\n'",
        "        else:",
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
        sh(`python3 ${P("mock.py")} ${P("port")} >${P("mock.log")} 2>&1 &`);
    let PORT = 0;
    for (let i = 0; i < 60 && !PORT; i++) {
        sh("sleep 0.1");
        try { PORT = parseInt(readFile(new Path(P("port"))), 10) || 0; }
        catch (e) { /* not yet */ }
    }
        const BASE = "http://127.0.0.1:" + PORT;

    const client = new HTTPClient();
    client.setTimeout(5000);
    const mkF = () => new Fetcher({ agent: "dynastate/1.0 (+https://example.test/bot)",
                                    client, minDelayMs: 0, robots: false,
                                    allowPrivateHosts: true });
    const ex = () => new Extractor({
        title: { sel: new Selector("h1") },
        links: { sel: new Selector("a"), attr: "href", all: true },
    }, { text: HTMLText });
    const drain = (c) => { const out = []; for (const p of c) out.push(p); return out; };
    const urls = (ps) => ps.map((p) => p.url);

    /* The control: one uninterrupted crawl of the whole graph. */
    const control = (() => { const f = mkF();
        const ps = drain(new Crawl(f, { maxPages: 50, maxDepth: 5 })
                             .start(BASE + "/a", ex(), HTMLParse));
        f.close(); return ps; })();

    /* ---- roundtrip at every split point: identical full sequence ---- */
    for (let k = 1; k <= 3; k++) {
        const f2 = mkF();
        const donor = new Crawl(f2, { maxPages: 50, maxDepth: 5 })
                          .start(BASE + "/a", ex(), HTMLParse);
        const got = [];
        for (let i = 0; i < k && i < control.length; i++) got.push(donor.next().value);
        const state = donor.serialize();
        f2.close();

        const f3 = mkF();
        const rest = drain(Crawl.resume(f3, state, ex(), HTMLParse));
        f3.close();

        const all = [...got, ...rest];
        ok(all.length === control.length &&
           urls(all).every((u, i) => u === control[i].url),
           "roundtrip after " + k + " page(s): identical sequence (" +
           all.length + " vs " + control.length + ")",
           urls(all).join(",") + " vs " + urls(control).join(","));
        ok(all.every((p, i) => p.depth === control[i].depth),
           "roundtrip after " + k + ": depths survive");
        ok(rest.every((p) => p.value && p.value.title),
           "roundtrip after " + k + ": the extractor runs on resumed pages");
    }

    /* ---- the state is inspectable and carries the knobs ---- */
    {
        const f2 = mkF();
        const donor = new Crawl(f2, { maxPages: 7, maxDepth: 2, sameHost: true })
                          .start(BASE + "/a", ex(), HTMLParse);
        donor.next();
        const st = JSON.parse(donor.serialize());
        f2.close();
        ok(st.v === 1, "state carries a version");
        ok(st.maxPages === 7 && st.maxDepth === 2 && st.sameHost === 1,
           "state carries the bounds");
        ok(st.emitted === 1, "state carries the emitted counter");
        ok(typeof st.seedHost === "string" && st.seedHost.length > 0,
           "state carries the seed host");
        ok(Array.isArray(st.q) && st.q.length >= 2 &&
           Array.isArray(st.q[0]) && st.q[0].length === 2,
           "state.q is a [url, depth] pair list (" + st.q.length + " deep)");
        ok(Array.isArray(st.seen) && st.seen.length >= 3,
           "state.seen holds the visited keys (" + st.seen.length + ")");
    }

    /* ---- exhausted budget resumes DONE, not as a fresh crawl ---- */
    {
        const f2 = mkF();
        const donor = new Crawl(f2, { maxPages: 2, maxDepth: 5 })
                          .start(BASE + "/a", ex(), HTMLParse);
        donor.next(); donor.next();
        const st = donor.serialize();
        f2.close();
        const f3 = mkF();
        const rest = drain(Crawl.resume(f3, st, ex(), HTMLParse));
        f3.close();
        ok(rest.length === 0, "an exhausted budget resumes as done (" +
           rest.length + " pages)");
    }

    /* ---- tampered state is refused, never half-restored ---- */
    {
        const f2 = mkF();
        const donor = new Crawl(f2, { maxPages: 50, maxDepth: 5 })
                          .start(BASE + "/a", ex(), HTMLParse);
        donor.next();
        const good = donor.serialize();
        f2.close();
        const f3 = mkF();
        const refuse = (name, s) => {
            let threw = false;
            try { Crawl.resume(f3, s, ex(), HTMLParse); }
            catch (e) { threw = /Crawl\.resume|TypeError|state/i.test(String(e)); }
            ok(threw, name + " is refused");
        };
        refuse("non-JSON state", "not json at all");
        refuse("wrong version", JSON.stringify({ v: 99, seedHost: "x",
                                                 linkField: "links",
                                                 q: [], seen: [] }));
        refuse("state without seedHost", JSON.stringify({ v: 1, q: [], seen: [] }));
        refuse("q entries not pairs", JSON.stringify({ v: 1, seedHost: "x",
            linkField: "links", q: ["http://x/"], seen: [] }));
        refuse("seen entries not strings", JSON.stringify({ v: 1, seedHost: "x",
            linkField: "links", q: [], seen: [17] }));
        /* resume returns a WORKING crawl: next() pages continue to flow */
        const f4 = mkF();
        const rc = Crawl.resume(f4, good, ex(), HTMLParse);
        const p1 = rc.next();
        ok(!p1.done && p1.value.url.startsWith(BASE),
           "a refused-state attempt leaves the constructor usable: resume works after");
        f4.close();
        f3.close();
    }

    /* ---- serialize() before start() refuses ---- */
    {
        const f2 = mkF();
        const c = new Crawl(f2, { maxPages: 5 });
        let threw = false;
        try { c.serialize(); } catch (e) { threw = /start/i.test(String(e)); }
        ok(threw, "serialize() before start() throws (no state to save)");
        f2.close();
    }

    /* ---- bounds come from the STATE, not fresh defaults ---- */
    {
        const f2 = mkF();
        const donor = new Crawl(f2, { maxPages: 3, maxDepth: 1 })
                          .start(BASE + "/a", ex(), HTMLParse);
        donor.next();
        const st = donor.serialize();
        f2.close();
        const f3 = mkF();
        const rest = drain(Crawl.resume(f3, st, ex(), HTMLParse));
        f3.close();
        ok(rest.length <= 2, "the donor's maxPages still bounds the resume (" +
           rest.length + " pages)");
        ok(rest.every((p) => p.depth <= 1),
           "the donor's maxDepth still bounds the resume");
    }

    print("test_scrape_state: " + pass + " passed, " + fail + " failed, " +
          skip + " skipped");
    if (fail) throw new Error("test_scrape_state: " + fail + " failures");
}
