// covers: integration dyna:http.HTTPClient dyna:http.fetch -- the per-origin in-flight cap and its dyna meta option
// T1 owner decision (2026-11-26): the client keeps at most 64 exchanges per origin in flight per thread
// (RFC 9112 9.4; a default listen backlog is 128 on macOS) and queues the rest FIFO. The dyna meta
// option raises or removes that cap per client: new HTTPClient(maxBody, { dyna: { maxPerOrigin } }) and
// fetch(url, { dyna: { maxPerOrigin } }), an integer in [1, 2^31-1] or Infinity, strict bags. The
// origin table is shared per thread, so each exchange is admitted while the origin's total in flight
// is under ITS OWN cap: an Infinity caller never queues, and it never lifts a capped caller past its
// cap. fetch() keeps the option across redirect hops (one client per fetch).
// Mutants: the option ignored (cases 2, 3, 5 red), Infinity read as 64 (cases 3, 5 red), the range
// check removed (validation case red), the queue pumped with the releasing exchange's cap instead of
// the waiting one's (mixed-caps case red).
import { suite } from "../lib/kit.js";
import { TCPServer } from "dyna:net";
import { HTTPClient } from "dyna:http";

const t = suite("http_client_origin_cap");
const OK = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
const bytes = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };

function peer(answer) {
    const srv = new TCPServer({ port: 0 });
    const st = { srv, requests: 0, heads: [], held: [], url: null };
    srv.start({
        connect(conn) { conn.buf = ""; },
        data(conn, b) {
            conn.buf += String.fromCharCode.apply(null, b);
            for (;;) {
                const end = conn.buf.indexOf("\r\n\r\n");
                if (end < 0) return;
                const head = conn.buf.slice(0, end + 4);
                conn.buf = conn.buf.slice(end + 4);
                st.requests++; st.heads.push(head);
                const out = answer(head);
                if (typeof out === "string") conn.write(bytes(out));
                else st.held.push({ conn, head });
            }
        },
    });
    st.url = "http://127.0.0.1:" + srv.port + "/";
    st.release = (pred) => { const i = st.held.findIndex((h) => pred(h.head)); const h = st.held.splice(i, 1)[0]; h.conn.write(bytes(OK)); };
    st.releaseAll = () => { while (st.held.length) st.held.shift().conn.write(bytes(OK)); };
    return st;
}

const until = async (cond, what, ms = 15000) => {
    const end = Date.now() + ms;
    while (!cond()) {
        if (Date.now() > end) throw new Error("condition never held: " + what);
        await new Promise((r) => setTimeout(r, 2));
    }
};
const count = (p, tag) => p.heads.filter((h) => h.includes(tag)).length;

t.test("dyna.maxPerOrigin is strictly validated on HTTPClient and fetch", async (a) => {
    const bad = (o) => { try { new HTTPClient(undefined, o).close(); return "ok"; } catch (e) { return e.name; } };
    for (const v of [1, 64, 1000, 2147483647, Infinity]) a.eq(bad({ dyna: { maxPerOrigin: v } }), "ok", String(v));
    a.eq(bad(undefined), "ok"); a.eq(bad({}), "ok"); a.eq(bad({ dyna: {} }), "ok");
    for (const v of [0, -1, 1.5, 2147483648, -Infinity, NaN]) a.eq(bad({ dyna: { maxPerOrigin: v } }), "RangeError", String(v));
    for (const v of ["5", null, true, {}]) a.eq(bad({ dyna: { maxPerOrigin: v } }), "TypeError", JSON.stringify(v));
    a.eq(bad({ dyna: { bogus: 1 } }), "TypeError", "unknown meta key");
    a.eq(bad({ bogus: 1 }), "TypeError", "unknown option key");
    a.eq(bad({ dyna: 3 }), "TypeError", "dyna not an object");
    let e = null;
    try { await fetch("http://127.0.0.1:9/", { dyna: { maxPerOrigin: 0 } }); } catch (x) { e = x; }
    a.ok(e instanceof RangeError, "fetch rejects a bad dyna.maxPerOrigin: " + e);
    e = null;
    try { await fetch("http://127.0.0.1:9/", { dyna: { nope: 1 } }); } catch (x) { e = x; }
    a.ok(e instanceof TypeError, "fetch rejects an unknown dyna key: " + e);
});

t.test("maxPerOrigin: 2 keeps two exchanges in flight and queues the rest", async (a) => {
    const p = peer(() => undefined);
    const c = new HTTPClient(undefined, { dyna: { maxPerOrigin: 2 } });
    const jobs = Array.from({ length: 5 }, (_, i) => c.get(p.url + "?i=" + i));
    await until(() => p.requests === 2, "two requests reach the peer");
    p.release(() => true);
    await until(() => p.requests === 3, "a freed slot admits the next one");
    a.ok(p.heads[2].includes("?i=2 "), "in arrival order");
    p.releaseAll();
    await until(() => p.requests === 5, "the rest go out as slots free");
    p.releaseAll();
    a.eq((await Promise.all(jobs)).filter((r) => r.body === "ok").length, 5);
    c.close(); p.srv.close();
}, { timeoutMs: 30000 });

t.test("maxPerOrigin: Infinity puts 100 exchanges in flight at once", async (a) => {
    const p = peer(() => undefined);
    const c = new HTTPClient(undefined, { dyna: { maxPerOrigin: Infinity } });
    const jobs = Array.from({ length: 100 }, () => c.get(p.url));
    await until(() => p.requests === 100, "all 100 reach the peer with no queueing");
    p.releaseAll();
    a.eq((await Promise.all(jobs)).length, 100);
    c.close(); p.srv.close();
}, { timeoutMs: 30000 });

t.test("mixed caps on one origin: an Infinity client never queues and never lifts a capped client", async (a) => {
    const p = peer((head) => (head.includes("?sentinel ") ? OK : undefined));
    const capped = new HTTPClient(undefined, { dyna: { maxPerOrigin: 2 } });
    const free = new HTTPClient(undefined, { dyna: { maxPerOrigin: Infinity } });
    const aj = Array.from({ length: 5 }, () => capped.get(p.url + "?a"));
    await until(() => count(p, "?a ") === 2, "the capped client puts 2 in flight");
    const bj = Array.from({ length: 3 }, () => free.get(p.url + "?b"));
    await until(() => count(p, "?b ") === 3, "the Infinity client is not queued behind the capped one");
    for (let i = 0; i < 3; i++) p.release((h) => h.includes("?b "));
    await Promise.all(bj);
    a.eq((await free.get(p.url + "?sentinel")).body, "ok");
    a.eq(count(p, "?a "), 2, "freeing the Infinity client's exchanges did not admit the capped client's queue");
    p.release((h) => h.includes("?a "));
    await until(() => count(p, "?a ") === 3, "a capped slot freeing admits the next capped request");
    p.releaseAll();
    await until(() => count(p, "?a ") === 5, "the capped queue drains");
    p.releaseAll();
    await Promise.all(aj);
    capped.close(); free.close(); p.srv.close();
}, { timeoutMs: 30000 });

t.test("fetch takes dyna.maxPerOrigin and keeps it across a redirect hop", async (a) => {
    const p = peer((head) => (head.includes("?r ") ? "HTTP/1.1 302 Found\r\nLocation: /?h\r\nContent-Length: 0\r\n\r\n" : undefined));
    const jobs = Array.from({ length: 70 }, () => fetch(p.url + "?r", { dyna: { maxPerOrigin: Infinity } }).then((r) => r.text()));
    await until(() => count(p, "?h ") === 70, "all 70 redirected requests are in flight past the default cap of 64");
    p.releaseAll();
    a.eq((await Promise.all(jobs)).filter((s) => s === "ok").length, 70);
    p.srv.close();
}, { timeoutMs: 30000 });

await t.run();
