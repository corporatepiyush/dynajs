// flags: --std
// covers: contract dyna:http.CookieJar dyna:http.HTTPClient.useCookies dyna:http.HTTPClient.get -- rule 18 rows for the JS inputs that reach src/core/httpk: hostile conversions, Proxies, NUL and CTL bytes, limit edges, GC in flight
// fz5n2. Each CookieJar input is a string or an options object: a non-string url or line throws TypeError without running
// its toString; a getter, valueOf or Proxy trap that throws propagates its own error and leaves no stale exception
// (storeResponse used to swallow a throwing Set-Cookie getter and its Proxy get trap, returning 0 with the error still
// pending, so the next native threw it). A url holding a NUL used to be cut at the NUL ("http://a.test\0.evil.test/"
// stored for a.test); now it stores nothing. A Set-Cookie line with any CTL but HTAB is ignored (RFC 6265bis 5.6 step 1; a NUL
// in an attribute used to store). 29 Feb of a common year is no date (RFC 6265 5.1.1), so Expires is ignored and the
// cookie stays a session cookie; it used to read as 1 Mar and delete the cookie. A chunked trailer line that starts with
// a bare LF or NUL fails the response (RFC 9112 7.1.2); it used to end the body. Forced allocation failure and the
// one-shot interrupt live in tests/http/test_http_cookiejar_api.c (they need an embedder).
import * as std from "std";
import { CookieJar, HTTPClient } from "dyna:http";
import { TCPServer } from "dyna:net";
import { suite } from "../lib/kit.js";

const t = suite("test_http_cookiejar_adversarial");
const HOST = "127.0.0.1", U = "http://a.test/", TMO = { timeoutMs: 4000 };
const SENT = new Error("sentinel");
const hostile = () => { const o = { calls: 0 }; o.v = { toString() { o.calls++; throw SENT; }, valueOf() { o.calls++; throw SENT; } }; return o; };
const err = (f) => { try { f(); return "returned"; } catch (e) { return e === SENT ? "sentinel" : e && e.name || String(e); } };
const clean = () => { try { JSON.parse("{"); return "no throw"; } catch (e) { return e instanceof SyntaxError ? "clean" : "stale " + e; } };

function gcStorm() {
    std.gc();
    for (const ms of [0, 2, 5, 10, 20, 40, 90]) setTimeout(() => std.gc(), ms);
}

function fakeHttp(reply) {
    const srv = new TCPServer({ port: 0 });
    const heads = [];
    const dec = new TextDecoder();
    srv.start({
        data(c, b) {
            c.buf = (c.buf || "") + dec.decode(b);
            const end = c.buf.indexOf("\r\n\r\n");
            if (end < 0) return;
            heads.push(c.buf.slice(0, end));
            c.buf = "";
            c.write(new TextEncoder().encode(reply(heads.length)));
        },
    });
    return { srv, heads, url: "http://" + HOST + ":" + srv.port + "/" };
}

t.test("url: a non-string url throws TypeError and never runs its toString", (a) => {
    const j = new CookieJar(), h = hostile();
    a.eq(err(() => j.setCookie(h.v, "a=1")), "TypeError", "setCookie");
    a.eq(err(() => j.cookieHeader(h.v)), "TypeError", "cookieHeader");
    a.eq(err(() => j.storeResponse(h.v, { "set-cookie": "a=1" })), "TypeError", "storeResponse");
    a.eq(h.calls, 0, "toString calls");
    a.eq(clean(), "clean", "no stale exception");
});

t.test("url: a url holding a NUL stores and reads nothing (it used to be cut at the NUL)", (a) => {
    const j = new CookieJar();
    a.eq(j.setCookie("http://a.test\0.evil.test/", "a=1"), "ignored", "setCookie");
    a.eq(j.storeResponse("http://a.test\0/", { "set-cookie": "b=1" }), 0, "storeResponse");
    a.eq(j.size, 0, "size");
    j.setCookie(U, "c=1");
    a.eq(j.cookieHeader("http://a.test/\0"), "", "cookieHeader with a NUL");
    a.eq(j.cookieHeader(U), "c=1", "cookieHeader without");
});

t.test("line: a non-string Set-Cookie throws TypeError and never runs its toString", (a) => {
    const j = new CookieJar(), h = hostile();
    a.eq(err(() => j.setCookie(U, h.v)), "TypeError", "setCookie");
    a.eq(j.storeResponse(U, { "set-cookie": h.v, "Set-Cookie": 7 }), 0, "storeResponse skips non-strings");
    a.eq(h.calls, 0, "toString calls");
});

t.test("line: a CTL anywhere but HTAB ignores the whole line (RFC 6265bis 5.6 step 1)", (a) => {
    const j = new CookieJar();
    for (const l of ["a=1\0b", "b=1; Path=/\0x", "c=1; Secure\x7f", "d=1; Domain=a.test\x01", "e=1\r", "f=1; Path=/\nX"])
        a.eq(j.setCookie(U, l), "ignored", JSON.stringify(l));
    a.eq(j.setCookie(U, "g=1;\tPath=/"), "stored", "HTAB between attributes");
    a.eq(j.storeResponse(U, { "set-cookie": "h=1\0\ni=2" }), 1, "one bad line of two");
    a.eq(j.cookieHeader(U), "g=1; i=2", "header");
});

t.test("line: limit edges of name plus value (4096 bytes, RFC 6265bis 5.6 step 1)", (a) => {
    const j = new CookieJar();
    a.eq(j.setCookie(U, "a=" + "x".repeat(4095)), "stored", "4096");
    a.eq(j.setCookie(U, "b=" + "x".repeat(4096)), "ignored", "4097");
    a.eq(j.setCookie(U, ""), "ignored", "empty");
    a.eq(j.setCookie(U, "=v"), "stored", "empty name");
});

t.test("storeResponse: a throwing Set-Cookie getter propagates and leaves no stale exception", (a) => {
    const j = new CookieJar();
    a.eq(err(() => j.storeResponse(U, { get "set-cookie"() { throw SENT; } })), "sentinel", "getter");
    a.eq(clean(), "clean", "no stale exception");
    a.eq(err(() => j.storeResponse(U, { "Set-Cookie": "a=1", get "set-cookie"() { throw SENT; } })), "sentinel", "after a stored line");
    a.eq(j.size, 1, "the line before the throw stays stored");
});

t.test("storeResponse: Proxy traps propagate; a well-behaved Proxy stores", (a) => {
    const j = new CookieJar();
    const traps = { ownKeys() { throw SENT; } };
    a.eq(err(() => j.storeResponse(U, new Proxy({ "set-cookie": "a=1" }, traps))), "sentinel", "ownKeys");
    a.eq(err(() => j.storeResponse(U, new Proxy({ "set-cookie": "a=1" }, { getOwnPropertyDescriptor() { throw SENT; } }))), "sentinel", "getOwnPropertyDescriptor");
    a.eq(err(() => j.storeResponse(U, new Proxy({ "set-cookie": "a=1" }, { get() { throw SENT; } }))), "sentinel", "get");
    a.eq(clean(), "clean", "no stale exception");
    let gets = 0;
    a.eq(j.storeResponse(U, new Proxy({ "set-cookie": "b=1\nc=2", x: "y" }, { get(o, k) { gets++; return o[k]; } })), 2, "well-behaved");
    a.eq(gets, 1, "get runs once, for the Set-Cookie key only");
});

t.test("maxCookies: limit edges and hostile conversions", (a) => {
    for (const m of [0, -1, 1e6 + 1, NaN, Infinity, -Infinity, 2 ** 53, -0])
        a.eq(err(() => new CookieJar({ maxCookies: m })), "RangeError", String(m));
    for (const m of [1, 1e6, "5", 1.9, undefined])
        a.eq(err(() => new CookieJar({ maxCookies: m })), "returned", String(m));
    const h = hostile();
    a.eq(err(() => new CookieJar({ maxCookies: h.v })), "sentinel", "valueOf");
    a.eq(h.calls, 1, "valueOf runs once");
    a.eq(err(() => new CookieJar({ get maxCookies() { throw SENT; } })), "sentinel", "getter");
    a.eq(err(() => new CookieJar(new Proxy({}, { get() { throw SENT; } }))), "sentinel", "Proxy get");
    a.eq(err(() => new CookieJar("x")), "TypeError", "string options");
    a.eq(clean(), "clean", "no stale exception");
});

t.test("maxCookies: max keeps max cookies, max+1 evicts the oldest", (a) => {
    const j = new CookieJar({ maxCookies: 2 });
    a.eq(j.setCookie(U, "a=1") + j.setCookie(U, "b=1"), "storedstored", "max");
    a.eq(j.size, 2, "size at max");
    j.setCookie(U, "c=1");
    a.eq(j.size, 2, "size at max+1");
    a.eq(j.cookieHeader(U), "b=1; c=1", "oldest evicted");
});

t.test("Expires: 29 Feb of a common year is no date, so the cookie stays a session cookie", (a) => {
    const j = new CookieJar();
    a.eq(j.setCookie(U, "a=1; Expires=Wed, 29 Feb 2023 00:00:00 GMT"), "stored", "2023 (used to read as 1 Mar 2023 and delete)");
    a.eq(j.setCookie(U, "b=1; Expires=29 Feb 2100 00:00:00"), "stored", "2100 is a common year");
    a.eq(j.setCookie(U, "c=1; Expires=Thu, 29 Feb 2024 00:00:00 GMT"), "deleted", "2024 is a leap year, in the past");
    a.eq(j.setCookie(U, "d=1; Expires=Tue, 29 Feb 2400 00:00:00 GMT"), "stored", "2400 is a leap year, in the future");
    a.eq(j.cookieHeader(U), "a=1; b=1; d=1", "header");
});

t.test("useCookies: a jar only the client holds survives a GC storm between requests", async (a) => {
    const f = fakeHttp((n) => "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n" + (n === 1 ? "Set-Cookie: s=1\r\n" : "") + "\r\nok");
    try {
        const c = new HTTPClient();
        c.useCookies(new CookieJar());
        gcStorm();
        await c.get(f.url);
        for (let i = 0; i < 5; i++) { std.gc(); await new Promise((r) => setTimeout(r, 5)); }
        await c.get(f.url);
        a.match(f.heads[1] || "", /\r\ncookie: s=1/i, "second request carries the cookie");
        c.close();
    } finally { f.srv.close(); }
}, TMO);

t.test("chunked: a trailer line starting with a bare LF or NUL fails the response", async (a) => {
    for (const [tr, want] of [["T: v\r\n\r\n", "ok"], ["\nT: v\r\n\r\n", "rejected"], ["\0T: v\r\n\r\n", "rejected"]]) {
        const f = fakeHttp(() => "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n2\r\nok\r\n0\r\n" + tr);
        const c = new HTTPClient();
        gcStorm();
        try {
            a.eq(await c.get(f.url).then(() => "ok", () => "rejected"), want, JSON.stringify(tr));
        } finally { c.close(); f.srv.close(); }
    }
}, TMO);
await t.run();
