import { Fetcher } from "dyna:scrape";

let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL  " + w); } };

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

let refused = false;
try { f.get("http://127.0.0.1/secret"); }
catch (e) { refused = /private|loopback/i.test(String(e)); }
ok(refused, "literal loopback refused by name");

let refused2 = false;
try { f.get("http://10.0.0.1/x"); }
catch (e) { refused2 = /private/i.test(String(e)); }
ok(refused2, "literal RFC1918 refused by name");

let refused3 = false;
try { f.get("http://intranet/x"); }
catch (e) { refused3 = /private|single/i.test(String(e)); }
ok(refused3, "single-label host refused by name");

const r = f.get("http://evil.example/page");
ok(r.status === 200, "public-name-to-private-IP passes (documented boundary)");
ok(connectsTo.length === 1 && connectsTo[0].includes("evil.example"),
   "the mock client saw the request (transport is the hook point)");
f.close();

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
