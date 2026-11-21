// timeout: 240
// covers: dyna:http.CookieJar
// CookieJar is the RFC 6265 jar in src/core/httpk (httpk-cookie.c), shared by
// slots: 2
// peer-proof: join srv -- whether the client reuses a keep-alive connection after an early stream close races the peer's separate header and body sends
// peer-proof: drop srv Host: localhost -- /away advertises the peer's own port, so the Python run's localhost hop bypasses the recording proxy
// fetch({cookieJar}), HTTPClient.useCookies(jar) and new Fetcher({cookieJar}).
// The last block pins fetch's redirect method rewrite, which now comes from
// httpk_redirect_plan (fetch standard 4.4 step 12): 301/302 rewrite only
// POST to GET, 303 rewrites everything but HEAD. Before that move fetch
// rewrote PUT/DELETE on 301/302 and turned HEAD into GET on 303 (both RED on
// the old build), and dropped only Content-Type/Content-Encoding.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_http_cookie_jar");
import { CookieJar, HTTPClient } from "dyna:http";
import { spawnHelper } from "../lib/helper.js";
import { Fetcher } from "dyna:scrape";
import { Exec, getEnv } from "dyna:sys";
import { makeTempDir, writeFile, readFile, removeAll, Path } from "dyna:file";

let pass = 0, fail = 0, skip = 0;
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
const skipped = (w) => { if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + w); return; } skip++; print("  SKIP  " + w); };
const throws = (fn, re, w) => { try { fn(); ok(false, w, "no throw"); } catch (e) { ok(re.test(String(e.message)), w, e.message); } };

await __kitS.section("sid=1; Path=/; Secure; HttpOnly", async () => {
    const j = new CookieJar();
    const A = "https://a.example.test/app/page";
    ok(j.setCookie(A, "sid=1; Path=/; Secure; HttpOnly") === "stored", "a Secure cookie from https is stored");
    ok(j.setCookie("http://a.example.test/", "x=1; Secure") === "ignored", "Secure from http is ignored");
    ok(j.setCookie(A, "pref=dark") === "stored", "no Path: the default path is the request's directory");
    ok(j.cookieHeader(A) === "sid=1; pref=dark" || j.cookieHeader(A) === "pref=dark; sid=1", "both apply under /app/", j.cookieHeader(A));
    ok(j.cookieHeader("https://a.example.test/other") === "sid=1", "pref is scoped to /app", j.cookieHeader("https://a.example.test/other"));
    ok(j.cookieHeader("http://a.example.test/") === "", "a Secure cookie never goes over http", j.cookieHeader("http://a.example.test/"));
    ok(j.cookieHeader("https://b.example.test/") === "", "host-only cookies stay on their host");
    ok(j.setCookie(A, "d=1; Domain=example.test; Path=/") === "stored", "Domain covering the request host is stored");
    ok(j.cookieHeader("https://b.example.test/") === "d=1", "a Domain cookie reaches sibling hosts", j.cookieHeader("https://b.example.test/"));
    ok(j.setCookie(A, "e=1; Domain=other.test") === "ignored", "Domain not covering the host is ignored");
    ok(j.setCookie(A, "t=1; Domain=test") === "ignored", "a single-label Domain is ignored");
    ok(j.setCookie(A, "__Host-h=1; Path=/") === "ignored", "__Host- without Secure is ignored");
    ok(j.size === 3, "three cookies held", j.size);
    ok(j.setCookie(A, "pref=; Max-Age=0") === "deleted", "Max-Age=0 deletes");
    ok(j.storeResponse(A, { "Set-Cookie": "k1=v1; Path=/\nk2=v2; Expires=Wed, 21 Oct 2099 07:28:00 GMT; Path=/", "X": "y" }) === 2,
       "storeResponse splits the \"\\n\"-joined Set-Cookie of a response's headers");
    ok(/k1=v1/.test(j.cookieHeader(A)) && /k2=v2/.test(j.cookieHeader(A)), "stored from headers", j.cookieHeader(A));
    j.clear();
    ok(j.size === 0 && j.cookieHeader(A) === "", "clear() empties the jar");
    ok(j.setCookie("ftp://x/", "a=1") === "ignored" && j.cookieHeader("not a url") === "", "a url that is not http(s) stores and matches nothing");
    throws(() => j.setCookie(1, "a=1"), /url must be a string/, "a non-string url is a TypeError");
    throws(() => new CookieJar({ maxCookies: 0 }), /maxCookies/, "maxCookies is range-checked");
    const small = new CookieJar({ maxCookies: 2 });
    small.setCookie(A, "a=1; Path=/"); small.setCookie(A, "b=2; Path=/"); small.setCookie(A, "c=3; Path=/");
    ok(small.size === 2, "maxCookies evicts", small.size);
    throws(() => new HTTPClient().useCookies({}), /CookieJar/, "useCookies refuses a non-jar");
});

async function live() {
    const T = makeTempDir("ckjar");
    const P = (n) => T + "/" + n;
    const ROOT = String(Exec("/bin/pwd", []).stdout).trim();
    const KIT = JSON.stringify(ROOT + "/tests/lib/rawpeer.js");
    writeFile(new Path(P("srv.js")), `import { listen, latin1 } from ${KIT};
const PHRASE = { 200: "OK", 301: "Moved Permanently", 302: "Found", 303: "See Other", 307: "Temporary Redirect", 308: "Permanent Redirect" };
const srv = listen(async (c) => {
    for (;;) {
        const h = await c.readUntil("\\r\\n\\r\\n");
        if (!h.found) return;
        const lines = latin1(h.bytes).split("\\r\\n");
        const [method, path, ver] = lines[0].split(" ");
        const hdrs = [];
        for (const l of lines.slice(1)) { const i = l.indexOf(":"); if (i > 0) hdrs.push([l.slice(0, i).toLowerCase(), l.slice(i + 1).trim()]); }
        const get = (k) => { const x = hdrs.find((e) => e[0] === k); return x ? x[1] : ""; };
        const n = parseInt(get("content-length") || "0", 10) || 0;
        if (n) await c.readBytes(n, { timeoutMs: 5000 });
        const ck = hdrs.filter((e) => e[0] === "cookie").map((e) => e[1]).join(" + ");
        const out = async (code, body = "", extra = []) => {
            let s = "HTTP/1.1 " + code + " " + (PHRASE[code] || "") + "\\r\\nServer: rawpeer\\r\\nDate: " + new Date().toUTCString() + "\\r\\n";
            for (const [k, v] of extra) s += k + ": " + v + "\\r\\n";
            s += "Content-Length: " + body.length + "\\r\\n\\r\\n";
            await c.write(s);
            if (method !== "HEAD" && body) await c.write(body);
        };
        if (path === "/login") await out(302, "", [["Set-Cookie", "sid=abc; Path=/"], ["Location", "/home"]]);
        else if (path === "/away") await out(302, "", [["Set-Cookie", "sid=abc; Path=/"], ["Location", "http://localhost:" + srv.port + "/echo"]]);
        else if (path.startsWith("/r/")) await out(parseInt(path.slice(3), 10), "", [["Location", "/echo"]]);
        else if (path === "/logout") await out(200, "bye", [["Set-Cookie", "sid=; Max-Age=0; Path=/"]]);
        else if (path === "/self" && ck.indexOf("gate=1") < 0) await out(302, "", [["Set-Cookie", "gate=1; Path=/"], ["Location", "/self"]]);
        else await out(200, method + "|" + ck + "|" + get("content-type"));
        if (ver !== "HTTP/1.1" || get("connection").toLowerCase() === "close") { c.close(); return; }
    }
}, { portFile: scriptArgs[1] });
`);
    const peer = spawnHelper(ROOT + "/dynajs --std " + P("srv.js") + " " + P("port") + " >/dev/null 2>&1");
    let port = 0;
    for (let i = 0; i < 100 && !port; i++) {
        try { port = Number(String(readFile(new Path(P("port")))).trim()); }
        catch (e) { await new Promise((r) => setTimeout(r, 50)); }
    }
    if (!port) { ok(false, "the srv.js peer published its port"); peer.stop(); removeAll(new Path(T)); return; }
    const B = "http://127.0.0.1:" + port;
    try {
        {
            const jar = new CookieJar();
            const r = await fetch(B + "/login", { cookieJar: jar });
            ok((await r.text()) === "GET|sid=abc|", "fetch: a cookie set on a redirect rides the next same-origin hop");
            ok(jar.size === 1, "fetch: the jar holds it after the exchange", jar.size);
            const r2 = await fetch(B + "/away", { cookieJar: jar });
            ok((await r2.text()) === "GET||", "fetch: a host-only cookie does not follow a redirect to another host");
            const r5 = await fetch(B + "/self", { cookieJar: new CookieJar() });
            ok(r5.status === 200 && (await r5.text()) === "GET|gate=1|", "fetch: a redirect to the same url is followed (a cookie gate), not refused as a loop");
            await (await fetch(B + "/logout", { cookieJar: jar })).text();
            ok(jar.size === 0, "fetch: Max-Age=0 from the server deletes", jar.size);
            const r3 = await fetch(B + "/echo", { cookieJar: jar, headers: { Cookie: "mine=1" } });
            jar.setCookie(B + "/", "sid=zzz; Path=/");
            const r4 = await fetch(B + "/echo", { cookieJar: jar, headers: { Cookie: "mine=1" } });
            ok((await r3.text()) === "GET|mine=1|" && (await r4.text()) === "GET|mine=1|", "fetch: an explicit Cookie header wins over the jar");
        }
        {
            const jar = new CookieJar();
            const c = new HTTPClient();
            c.useCookies(jar);
            const a = await c.get(B + "/login");
            ok(a.status === 302 && jar.size === 1, "HTTPClient: a response's Set-Cookie lands in the jar", a.status + " " + jar.size);
            ok((await c.get(B + "/echo")).body === "GET|sid=abc|", "HTTPClient: the jar's cookie goes out on the next request");
            const st = await c.getStream(B + "/echo");
            const buf = new Uint8Array(64); const n = await st.read(buf); st.close();
            ok(String.fromCharCode(...buf.subarray(0, n)) === "GET|sid=abc|", "HTTPClient.getStream sends it too");
            const pr = await c[Symbol.for("dyna.http.request")]("GET", B + "/echo", undefined, {}, true);
            ok(String.fromCharCode(...new Uint8Array(pr.bodyBytes)) === "GET|sid=abc|", "HTTPClient: the async path sends it too");
            c.useCookies(null);
            ok((await c.get(B + "/echo")).body === "GET||", "useCookies(null) detaches the jar");
            c.close();
        }
        {
            const jar = new CookieJar();
            jar.setCookie(B + "/", "sid=fj; Path=/");
            const f = new Fetcher({ agent: "jar-test/1.0", robots: false, allowPrivateHosts: true, retries: 0, minDelayMs: 0, cookieJar: jar });
            ok((await f.get(B + "/echo")).body === "GET|sid=fj|", "Fetcher: get() sends the jar's cookie");
            const s0 = await f.getStream(B + "/echo");
            const b0 = new Uint8Array(64); const n0 = await s0.read(b0); s0.close();
            ok(String.fromCharCode(...b0.subarray(0, n0)) === "GET|sid=fj|", "Fetcher: getStream sends the jar's cookie");
            const st = await f.getStream(B + "/login");
            st.close();
            ok(jar.size === 1 && /sid=abc/.test(jar.cookieHeader(B + "/")), "Fetcher: getStream stores Set-Cookie", jar.cookieHeader(B + "/"));
            f.close();
            throws(() => new Fetcher({ agent: "x/1", client: new HTTPClient(), cookieJar: jar }), /useCookies/, "Fetcher: cookieJar with an injected client is refused");
        }
        {
            const m = async (status, method) => (await (await fetch(B + "/r/" + status, { method, body: method === "GET" || method === "HEAD" ? undefined : "x", headers: { "Content-Type": "text/plain" } })).text());
            ok((await m(301, "POST")) === "GET||", "fetch: 301 + POST becomes GET without body headers");
            ok((await m(302, "PUT")) === "PUT||text/plain", "fetch: 302 + PUT keeps PUT and its body (fetch 4.4)");
            ok((await m(301, "DELETE")) === "DELETE||text/plain", "fetch: 301 + DELETE keeps DELETE");
            ok((await m(303, "PUT")) === "GET||", "fetch: 303 + PUT becomes GET");
            const h = await fetch(B + "/r/303", { method: "HEAD" });
            ok(h.status === 200 && (await h.text()) === "", "fetch: 303 + HEAD stays HEAD (no body read)");
            ok((await m(307, "POST")) === "POST||text/plain", "fetch: 307 keeps method and body");
        }
    } finally {
        peer.stop();
        removeAll(new Path(T));
    }
}

await __kitS.section("live", async () => { await live(); });
__kitS.end();
print("test_http_cookie_jar: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
if (fail) throw new Error("test_http_cookie_jar: " + fail + " failures");
