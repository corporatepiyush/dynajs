/* test_dns_rebinding.js — T8 from SECURITY_COMPAT_PLAN.md.
 *
 * The Fetcher's SSRF gate is NAME-based by design: it refuses private NAME
 * forms (loopback, RFC 1918, link-local, single-label) on every redirect
 * hop, but a public name whose DNS RESOLVES into private space passes the
 * gate — resolution belongs to the injected client. This test locks that
 * admitted boundary as executable spec: a mock client that resolves
 * "evil.example" to 127.0.0.1 and the Fetcher still fetches it, proving
 * (a) the name-gate does not claim more than it promises, and (b) the
 * documented mitigation (validate at the transport, not the name) is the
 * correct integration point for deployments that need it.
 *
 * When P3 (a connect-time resolved-address callback) lands, this test's
 * `connectsTo` ledger is the exact hook it fills; flip the assertion then.
 */
import { Fetcher } from "dyna:scrape";

let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL  " + w); } };

/* A mock client that plays the rebinding attacker: "evil.example" is a
   public-sounding name, but the transport "resolves" it to loopback. */
const connectsTo = [];
const rebinding_client = {
  request(method, url, body, headers) {
    if (url.endsWith("/robots.txt"))
      return { status: 404, headers: {}, body: "" };
    connectsTo.push(url);
    return { status: 200, headers: {}, body: "<h1>gotcha</h1>" };
  },
};

const f = new Fetcher({ agent: "t8/1.0", client: rebinding_client,
                        minDelayMs: 0, robots: false });

/* The NAME gate refuses literal private hosts — verify the gate IS armed. */
let refused = false;
try { f.get("http://127.0.0.1/secret"); }
catch (e) { refused = /private|loopback/i.test(String(e)); }
ok(refused, "literal loopback refused by name");

let refused2 = false;
try { f.get("http://10.0.0.1/x"); }
catch (e) { refused2 = /private/i.test(String(e)); }
ok(refused2, "literal RFC1918 refused by name");

/* A single-label host is refused too. */
let refused3 = false;
try { f.get("http://intranet/x"); }
catch (e) { refused3 = /private|single/i.test(String(e)); }
ok(refused3, "single-label host refused by name");

/* ...but the REBINDING name passes to the client (the admitted boundary). */
const r = f.get("http://evil.example/page");
ok(r.status === 200, "public-name-to-private-IP passes (documented boundary)");
ok(connectsTo.length === 1 && connectsTo[0].includes("evil.example"),
   "the mock client saw the request (transport is the hook point)");
f.close();

/* Redirect hops re-run the name gate: a redirect to a private NAME form is
   refused even when the original host was public. */
const connects2 = [];
const redirect_client = {
  request(method, url) {
    if (url.endsWith("/robots.txt")) return { status: 404, headers: {}, body: "" };
    connects2.push(url);
    if (url.endsWith("/start"))
      return { status: 302, headers: { Location: "http://192.168.1.1/admin" }, body: "" };
    return { status: 200, headers: {}, body: "" };
  },
};
const f2 = new Fetcher({ agent: "t8/1.0", client: redirect_client,
                         minDelayMs: 0, robots: false });
let hopRefused = false;
try { f2.get("http://evil.example/start"); }
catch (e) { hopRefused = /private/i.test(String(e)); }
ok(hopRefused, "redirect to private-name form refused on the hop");
f2.close();

print("test_dns_rebinding: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_dns_rebinding: " + fail + " failures");
