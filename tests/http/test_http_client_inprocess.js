// covers: integration dyna:http.HTTPClient dyna:http.App -- the client calling an App in the same process (T1)
// T1 (2026-11-26, owner "TRAPS"): HTTPClient was a blocking client, so it could never reach an App in
// the same process (the App's handlers need the thread the client held). get/post/request/getStream now
// return Promises settled on the event loop's reactor, the blocking methods are gone (clean break), and
// getStream's body is read on the reactor too: read() parks a promise that the next arriving bytes
// settle. These cases call an App in this very process and pin the reactor contract: the loop keeps
// running while exchanges and reads are pending, a stream may idle between reads longer than the
// per-read timeout (the timeout runs only while a read is pending), one pending read at a time,
// close() and disconnect() settle a pending read, and maxBody bounds the streamed total.
// Waits are condition waits on events (no sleeps standing in for "long enough").
// An opened stream (head received, so the server has accepted it) gives back its slot in the
// 64-connections-per-origin cap: long-lived SSE streams never starve plain requests.
// Mutants: get bound to the old blocking exchange (the first case hangs), the per-read idle deadline
// enforced while no read is pending (the idle-between-reads case is cut), the slot kept by an opened
// stream (the 70-streams case waits out its deadline).
import { suite } from "../lib/kit.js";
import { App, HTTPClient } from "dyna:http";

const t = suite("http_client_inprocess");

function sseApp() {
    const st = { conns: [], opened: 0 };
    const app = new App({ port: 0, idleTimeoutMs: 20000 });
    app.get("/a", () => "hello");
    app.post("/echo", (req) => req.body);
    app.get("/big/:n", (req) => "y".repeat(+req.params.n));
    app.get("/slow", () => new Promise((res) => setTimeout(() => res("late"), 30)));
    app.sse("/events", { open: (c) => { st.conns.push(c); st.opened++; }, close: () => {} });
    app.start();
    st.app = app;
    st.base = "http://127.0.0.1:" + app.port;
    return st;
}

const until = async (cond, what, ms = 10000) => {
    const end = Date.now() + ms;
    while (!cond()) {
        if (Date.now() > end) throw new Error("condition never held: " + what);
        await new Promise((r) => setTimeout(r, 1));
    }
};

t.test("get, post, request and getStream return Promises that an App in this process answers", async (a) => {
    const s = sseApp(), c = new HTTPClient();
    const pg = c.get(s.base + "/a");
    a.ok(pg instanceof Promise, "get returns a Promise");
    a.eq((await pg).body, "hello");
    a.eq((await c.post(s.base + "/echo", "payload")).body, "payload");
    a.eq((await c.request("DELETE", s.base + "/a")).status, 405);
    const ps = c.getStream(s.base + "/big/70000");
    a.ok(ps instanceof Promise, "getStream returns a Promise");
    const st = await ps;
    const buf = new Uint8Array(8192);
    let n = 0, k;
    while ((k = await st.read(buf)) > 0) n += k;
    a.eq(n, 70000, "the streamed body arrives whole");
    st.close();
    c.close(); s.app.close();
});

t.test("the loop keeps running while an exchange is pending", async (a) => {
    const s = sseApp(), c = new HTTPClient();
    let ticks = 0, settled = false;
    const p = c.get(s.base + "/slow").then((r) => { settled = r.body; });
    const timer = setInterval(() => { ticks++; }, 1);
    await p;
    clearInterval(timer);
    a.eq(settled, "late");
    a.ok(ticks > 0, "timers fired while the request was pending (" + ticks + ")");
    c.close(); s.app.close();
});

t.test("100 concurrent requests from one client to the App all resolve", async (a) => {
    const s = sseApp(), c = new HTTPClient();
    const rs = await Promise.all(Array.from({ length: 100 }, (_, i) => c.get(s.base + "/big/" + i)));
    a.ok(rs.every((r, i) => r.status === 200 && r.body.length === i), "every body has its own length");
    c.close(); s.app.close();
});

t.test("a pending read parks on the reactor and resolves when the server sends", async (a) => {
    const s = sseApp(), c = new HTTPClient();
    const st = await c.getStream(s.base + "/events");
    a.eq(st.status, 200);
    await until(() => s.conns.length === 1, "the SSE connection opened");
    const buf = new Uint8Array(256);
    let got = -1;
    const pr = st.read(buf).then((n) => { got = n; });
    let ticks = 0;
    const timer = setInterval(() => { ticks++; }, 1);
    await until(() => ticks >= 3, "timers run while the read is pending");
    a.eq(got, -1, "the read is still pending: nothing was sent yet");
    s.conns[0].send("ping");
    await pr;
    clearInterval(timer);
    a.ok(got > 0 && new TextDecoder().decode(buf.subarray(0, got)).includes("data: ping"), "the read resolved with the event");
    st.close();
    c.close(); s.app.close();
});

t.test("a stream may idle between reads longer than the per-read timeout", async (a) => {
    const s = sseApp(), c = new HTTPClient();
    c.setTimeout(150);
    const st = await c.getStream(s.base + "/events");
    await until(() => s.conns.length === 1, "the SSE connection opened");
    s.conns[0].send("one");
    const buf = new Uint8Array(256);
    a.ok(await st.read(buf) > 0, "first event read");
    const idleUntil = Date.now() + 600;
    await until(() => Date.now() >= idleUntil, "the consumer idles 4x the per-read timeout without a pending read");
    s.conns[0].send("two");
    let n = 0, err = null;
    try { n = await st.read(buf); } catch (e) { err = e; }
    a.ok(err === null && n > 0, "the stream survived the idle gap: " + (err ? err.message : n));
    st.close();
    c.close(); s.app.close();
});

t.test("one pending read at a time; close() settles the pending read with 0", async (a) => {
    const s = sseApp(), c = new HTTPClient();
    const st = await c.getStream(s.base + "/events");
    const buf = new Uint8Array(64);
    const p1 = st.read(buf);
    let second = null;
    try { await st.read(new Uint8Array(8)); } catch (e) { second = e; }
    a.ok(second instanceof TypeError && /already pending/.test(second.message), "a second concurrent read rejects TypeError");
    st.close();
    a.eq(await p1, 0, "close() settles the pending read with 0");
    a.ok(st.closed, "and the stream reports closed");
    c.close(); s.app.close();
});

t.test("disconnect() rejects a pending stream read; the client keeps working", async (a) => {
    const s = sseApp(), c = new HTTPClient();
    const st = await c.getStream(s.base + "/events");
    const pr = st.read(new Uint8Array(64)).then(() => null, (e) => e);
    c.disconnect();
    const e = await pr;
    a.ok(e instanceof RangeError, "the pending read rejects: " + e);
    a.eq((await c.get(s.base + "/a")).body, "hello", "a request after disconnect() runs normally");
    st.close();
    c.close(); s.app.close();
});

t.test("maxBody bounds the streamed total: read() rejects once the body passes it", async (a) => {
    const s = sseApp(), c = new HTTPClient(4096);
    const st = await c.getStream(s.base + "/events");
    await until(() => s.conns.length === 1, "the SSE connection opened");
    for (let i = 0; i < 10; i++) s.conns[0].send("x".repeat(1000));
    const buf = new Uint8Array(512);
    let total = 0, err = null;
    try { for (;;) { const n = await st.read(buf); if (!n) break; total += n; } } catch (e) { err = e; }
    a.ok(err instanceof RangeError && /maxBodyBytes/.test(err.message), "RangeError naming maxBodyBytes: " + err);
    a.ok(total <= 4096 + 8192, "bytes handed out before the cut stay near the cap (" + total + ")");
    st.close();
    a.ok((await c.getStream(s.base + "/big/5000").then(() => null, (e) => e)) instanceof RangeError,
        "a declared Content-Length over maxBody rejects getStream itself");
    c.close(); s.app.close();
});

t.test("an opened stream gives back its connection slot: 70 open streams to one origin", async (a) => {
    const s = sseApp(), c = new HTTPClient();
    const sts = await Promise.all(Array.from({ length: 70 }, () => c.getStream(s.base + "/events")));
    a.eq(sts.filter((x) => x.status === 200).length, 70, "all 70 streams opened past the 64-per-origin cap");
    a.eq((await c.get(s.base + "/a")).body, "hello", "and a plain request still gets a slot");
    for (const x of sts) x.close();
    c.close(); s.app.close();
}, { timeoutMs: 20000 });

await t.run();
