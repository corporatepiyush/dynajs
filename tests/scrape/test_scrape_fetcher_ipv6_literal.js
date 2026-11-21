// covers: dyna:scrape.Fetcher
// The Fetcher's SSRF gate decides an IP literal by its address alone. A public IPv6
// literal has no dot, so it once fell through to the single-label-host rule and was
// refused as private; now it reaches the client. Refused literals: ::, ::1, fe80::/10,
// fc00::/7, ff00::/8, 2001::/23 (incl. Teredo), 2001:db8::/32, 64:ff9b:1::/48, and ::ffff:0:0/96
// or 64:ff9b::/96 embedding a private IPv4. IPv4 integer literals are judged by address as well.
// A stub client records each request, so no socket is opened.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_scrape_fetcher_ipv6_literal");
import { Fetcher } from "dyna:scrape";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };

const seen = [];
const stub = { request(method, url) { seen.push(method + " " + url); return { status: 200, headers: { "content-type": "text/plain" }, body: "ok" }; } };
const f = new Fetcher({ agent: "dynabot/1.0", client: stub, robots: false, minDelayMs: 0, retries: 0 });

await __kitS.section("GET", async () => {
for (const url of ["http://[2606:4700::1111]/a", "https://[2001:4860:4860::8888]:8443/b?q=1",
                   "http://[64:ff9b::8.8.8.8]/c", "http://[::ffff:8.8.8.8]/d",
                   "http://[2001:200::1]/e", "http://134744072/f"]) {
    let r = null, err = "";
    try { r = await f.get(url); } catch (e) { err = String(e.message); }
    check(r && r.status === 200 && seen.length && seen[seen.length - 1] === "GET " + url,
        url + " is fetched through the client (" + (err || JSON.stringify(seen.slice(-1))) + ")");
}
});

await __kitS.section("is refused before the client", async () => {
for (const url of ["http://[::1]/", "http://[::]/", "http://[fe80::1]/", "http://[fd00::1]/",
                   "http://[2001:db8::1]/", "http://[::ffff:127.0.0.1]/", "http://[64:ff9b::10.0.0.1]/",
                   "http://[::ffff:7f00:1]/", "http://[::ffff:0x7f.1]/",
                   "http://[ff02::1]/", "http://[64:ff9b:1::1]/", "http://[2001::1]/", "http://[2001:0:4136:e378:8000:63bf:3fff:fdd2]/",
                   "http://167772161/"]) {
    const before = seen.length;
    let err = "";
    try { await f.get(url); } catch (e) { err = String(e.message); }
    check(/private\/loopback\/link-local/.test(err) && seen.length === before, url + " is refused before the client (" + err + ")");
}
});
__kitS.end();

f.close();
print("test_scrape_ipv6_literal: " + n + " checks, " + fails + " failed");
if (fails) throw new Error("test_scrape_ipv6_literal: " + fails + " failures");
