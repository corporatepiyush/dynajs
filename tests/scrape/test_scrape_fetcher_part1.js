// Part of the former tests/scrape/test_scrape_fetcher.js, split so the parts run in parallel.
import { Fetcher } from "dyna:scrape";
import { HTTPClient } from "dyna:net";
import { Exec, getEnv } from "dyna:sys";

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

print("test_scrape_fetcher_part1: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
if (fail) throw new Error("test_scrape_fetcher_part1: " + fail + " failures");
