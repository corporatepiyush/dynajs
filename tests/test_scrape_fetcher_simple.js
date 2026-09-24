/* test_scrape_fetcher_simple.js --: the Fetcher simple-options ctor and
 * the async one-page helper.
 *
 * The simple bag ({proxy, ca, poolSize, headers, minDelayMs}) must construct
 * WITHOUT a hand-built client, refuse every bad value it names (a wrong-typed
 * proxy is the caller's intent misspelled, never a silent default), obey the
 * strict-bag rule (unknown key throws naming key AND valid set), and echo
 * what it stored through stats() -- recorded and observable, never silently
 * ignored. getAsync settles as a Promise: a transport throw becomes a
 * REJECTION, never a sync throw. All peers are mocks: policy is proved by
 * what the mock saw, and no network is touched. Pure dyna:scrape.
 */
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

/* ---- the simple bag constructs without a client ---------------------- */
{
  /* Strict bag: an unknown key names itself AND the valid set. */
  const m = throws(() => new Fetcher({ agent: "bot/1.0", proxy2: "http://p" }),
             "an unknown option key is refused");
  check(/proxy2/.test(m) && /proxy/.test(m) && /valid/.test(m),
        "the error names the bad key and the valid set");

  /* proxy / ca: plain non-empty strings; anything else is a TypeError. */
  let mm = throws(() => new Fetcher({ agent: "b/1.0", proxy: 42 }),
             "a numeric proxy is refused");
  check(/proxy/.test(mm), "error names proxy");
  mm = throws(() => new Fetcher({ agent: "b/1.0", proxy: "" }),
             "an empty proxy is refused");
  throws(() => new Fetcher({ agent: "b/1.0", ca: true }), "a boolean ca is refused");
  throws(() => new Fetcher({ agent: "b/1.0", ca: "" }), "an empty ca is refused");

  /* poolSize: integer 1..64 */
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: 0 }), "poolSize 0 refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: 65 }), "poolSize 65 refused");
  /* strict integer: no coercion, no truncation (1.5 must not become 1) */
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: "5" }),
         "poolSize \"5\" (string) refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: 1.5 }),
         "poolSize 1.5 (fraction) refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: 64.9 }),
         "poolSize 64.9 (fraction) refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: NaN }), "poolSize NaN refused");
  throws(() => new Fetcher({ agent: "b/1.0", poolSize: Infinity }),
         "poolSize Infinity refused");

  /* The SIMPLE form: no client at all. The auto-built client needs
     dyna:net, which the slim SCRAPE binary does not carry -- there the
     construction refuses by design and these two checks skip loudly. */
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

  /* null counts as absent (the module-wide bag convention) */
  const f3 = new Fetcher({ agent: "b/1.0", client: stub, proxy: null, ca: null,
                           poolSize: 1 });
  eq(f3.stats().poolSize, 1, "poolSize 1 (the floor) constructs");
  f3.close();
  const f4 = new Fetcher({ agent: "b/1.0", client: stub, poolSize: 64 });
  eq(f4.stats().poolSize, 64, "poolSize 64 (the ceiling) constructs");
  f4.close();
}

/* ---- getAsync settles as a promise ---------------------------------- */
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
    let p = f.getAsync("http://x.test/a");
    check(p && typeof p.then === "function", "getAsync returns a thenable");
    let r = await p;
    eq(r.status, 200, "resolves with the response");
    eq(r.body, "page:http://x.test/a", "and the body rode through");
    eq(r.url, "http://x.test/a", "and the response url");
    check(seen.indexOf("GET http://x.test/a") >= 0, "the transport saw one GET");

    /* a transport throw is a REJECTION, never a sync throw */
    const boom = { request() { throw new Error("socket refused"); } };
    const f2 = new Fetcher({ agent: "bot/1.0", client: boom, robots: false,
                             minDelayMs: 0 });
    let syncThrew = false, rejected = false, rmsg = "";
    try {
      const p2 = f2.getAsync("http://x.test/b");
      try { await p2; } catch (e) { rejected = true; rmsg = String(e.message || e); }
    } catch (e) { syncThrew = true; }
    check(!syncThrew, "getAsync NEVER throws synchronously");
    check(rejected, "a transport throw surfaces as a rejection");
    check(/socket refused/.test(rmsg), "the rejection carries the cause");
    f2.close();

    /* argument contract rejects the same way (a rejection, not a throw) */
    let badArg = false;
    try { await f.getAsync(); } catch (e) { badArg = true; }
    check(badArg, "getAsync() without a url rejects");

    f.close();
    print("test_scrape_fetcher_simple: " + n + " checks, " + fails + " failures, " +
          skipped + " skipped");
    if (fails) throw new Error(fails + " failures");
  })();
}
