import "./httpc.js";
import { Fetcher } from "dyna:scrape";

let n = 0, fails = 0, skipped = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b));
const throws = (fn, m) => {
  let t = false, msg = "";
  try { fn(); } catch (e) { t = true; msg = String(e.message || e); }
  check(t, m + (t ? "" : " -- did NOT throw"));
  return msg;
};

{
  const m = throws(() => new Fetcher({ agent: "bot/1.0", proxy2: "http://p" }),
             "an unknown option key is refused");
  check(/proxy2/.test(m) && /proxy/.test(m) && /valid/.test(m),
        "the error names the bad key and the valid set");

  let mm = throws(() => new Fetcher({ agent: "b/1.0", proxy: 42 }),
             "a numeric proxy is refused");
  check(/proxy/.test(mm), "error names proxy");
  mm = throws(() => new Fetcher({ agent: "b/1.0", proxy: "" }),
             "an empty proxy is refused");
  throws(() => new Fetcher({ agent: "b/1.0", ca: true }), "a boolean ca is refused");
  throws(() => new Fetcher({ agent: "b/1.0", ca: "" }), "an empty ca is refused");

  throws(() => new Fetcher({ agent: "b/1.0", poolSize: 0 }), "poolSize 0 refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: 65 }), "poolSize 65 refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: "5" }),
         "poolSize \"5\" (string) refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: 1.5 }),
         "poolSize 1.5 (fraction) refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: 64.9 }),
         "poolSize 64.9 (fraction) refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: NaN }), "poolSize NaN refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: Infinity }),
         "poolSize Infinity refused");

  try {
    const f = new Fetcher({ agent: "b/1.0", proxy: "http://127.0.0.1:9",
                            ca: "/etc/ssl/cert.pem", poolSize: 8, minDelayMs: 0 });
    check(f && !f.closed, "constructs WITHOUT a hand-built client (SP-3)");
    const s = f.stats();
    eq(s.proxy, "http://127.0.0.1:9", "stats() echoes the stored proxy");
    eq(s.ca, "/etc/ssl/cert.pem", "stats() echoes the stored ca");
    eq(s.poolSize, 8, "stats() echoes poolSize");
    f.close();
  } catch (e) {
    if (/built-in|dyna:net/.test(String(e.message || e))) {
      skipped += 3;
      print("  SKIP  simple ctor (no dyna:net in this binary)");
    } else throw e;
  }

  const stub = { request() { return { status: 404, headers: {}, body: "" }; } };
  const f2 = new Fetcher({ agent: "b/1.0", client: stub });
  const s2 = f2.stats();
  eq(s2.proxy, null, "unset proxy reports null, not a guess");
  eq(s2.ca, null, "unset ca reports null");
  eq(s2.poolSize, 4, "poolSize defaults to 4");
  f2.close();

  const f3 = new Fetcher({ agent: "b/1.0", client: stub, proxy: null, ca: null,
                           poolSize: 1 });
  eq(f3.stats().poolSize, 1, "poolSize 1 (the floor) constructs");
  f3.close();
  const f4 = new Fetcher({ agent: "b/1.0", client: stub, poolSize: 64 });
  eq(f4.stats().poolSize, 64, "poolSize 64 (the ceiling) constructs");
  f4.close();
}

{
  const seen = [];
  const client = {
    request(method, url, body, headers) {
      seen.push(method + " " + url);
      if (url.endsWith("/robots.txt"))
        return { status: 404, headers: {}, body: "" };
      return { status: 200, headers: { "Content-Type": "text/html" },
               body: "page:" + url };
    }
  };
  const f = new Fetcher({ agent: "bot/1.0", client, robots: false, minDelayMs: 0 });

  (async () => {
    let r = f.get("http://x.test/a");
    eq(r.status, 200, "get returns the response");
    eq(r.body, "page:http://x.test/a", "and the body rode through");
    eq(r.url, "http://x.test/a", "and the response url");
    check(seen.indexOf("GET http://x.test/a") >= 0, "the transport saw one GET");

    const boom = { request() { throw new Error("socket refused"); } };
    const f2 = new Fetcher({ agent: "bot/1.0", client: boom, robots: false,
                             minDelayMs: 0 });
    let rmsg = "";
    try { f2.get("http://x.test/b"); } catch (e) { rmsg = String(e.message || e); }
    check(/socket refused/.test(rmsg), "a transport throw carries the cause");
    f2.close();

    check(typeof f.getAsync === "undefined", "Fetcher has no getAsync: get is the one form");

    f.close();
    print("test_scrape_fetcher_simple: " + n + " checks, " + fails + " failures, " +
          skipped + " skipped");
    if (fails) throw new Error(fails + " failures");
  })();
}
