/* test_http_tls.js -- HTTPClient over TLS (design 16 milestone 3).
 *
 * The client refused https:// outright before this ("no TLS in this
 * plain-socket client"), which also made Fetcher and Crawl http-only.
 *
 * Every BAD endpoint must fail AND name its own check -- "handshake failed"
 * is not actionable, "certificate has expired" is. The plaintext CONTROL must
 * not move, since the same two I/O helpers now carry both schemes.
 *
 * Needs the network. A skip is loud, and fatal under DYNAJS_REQUIRE_TOOLS=1.
 */
import { HTTPClient } from "dyna:net";
import { Fetcher } from "dyna:scrape";
import { getEnv } from "dyna:sys";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function skipped(w) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; }
    skip++; print("  SKIP  " + w);
}
/* {status} on success, {err} on refusal, {net:true} when unreachable. */
function get(c, url) {
    try { const r = c.get(url); return { status: r.status, len: r.body.length }; }
    catch (e) {
        const m = String(e.message || e);
        if (/resolution|connection failed|timed out/i.test(m)) return { net: true, err: m };
        return { err: m };
    }
}

const c = new HTTPClient();
c.setTimeout(15000);

print("--- https works, and http is unchanged ---");
{
    const r = get(c, "https://badssl.com/");
    if (r.net) skipped("badssl.com (no network)");
    else {
        ok(r.status === 200, "GET https:// returns 200 (got " + (r.status || r.err) + ")");
        ok(r.len > 0, "and a body arrives over TLS (" + r.len + " bytes)");
    }
    const p = get(c, "http://example.com/");
    if (p.net) skipped("example.com (no network)");
    else ok(p.status === 200, "CONTROL: plaintext http still works -- the same " +
            "two I/O helpers now carry both schemes", p.err);
}

print("--- every bad certificate is refused, NAMING its check ---");
for (const [host, want] of [["expired.badssl.com", /expired/i],
                            ["wrong.host.badssl.com", /host|match/i],
                            ["self-signed.badssl.com", /self-signed/i],
                            ["untrusted-root.badssl.com", /certificate chain|issuer/i]]) {
    const r = get(c, "https://" + host + "/");
    if (r.net) { skipped(host); continue; }
    if (r.status) { fail++; print("  FAIL  " + host + " CONNECTED -- the check is not live"); continue; }
    ok(want.test(r.err), host + " refused for its OWN reason (" +
       r.err.replace(/^HTTP GET \S+ failed: /, "").slice(0, 46) + ")");
}

print("--- a 1.2-only host still works (the floor is 1.2, not 1.3) ---");
{
    const r = get(c, "https://tls-v1-2.badssl.com:1012/");
    if (r.net) skipped("tls-v1-2.badssl.com");
    else ok(r.status === 200, "TLS 1.2 endpoint connects", r.err);
}

print("--- Fetcher inherits https, so Crawl does too ---");
{
    const f = new Fetcher({ agent: "dynajs-test/1.0 (+https://example.test/bot)",
                            client: c, minDelayMs: 0, robots: false });
    let r;
    try { r = f.get("https://badssl.com/"); } catch (e) { r = { err: String(e.message) }; }
    if (r.err && /resolution|connection/i.test(r.err)) skipped("Fetcher over https");
    else ok(r.status === 200 && r.body.length > 0,
            "Fetcher fetches an https url (" + (r.status || r.err) + ")");
    f.close();
}

c.close();
print("test_http_tls: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
if (fail) throw new Error("test_http_tls: " + fail + " failures");
