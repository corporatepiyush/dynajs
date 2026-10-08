// The App request path keeps its contracts after the hot-path work: built-in
// HTTP metrics survive Metrics.reset() (cached slots are re-resolved, never
// reused stale), route lookup allocates nothing for routes that do not match
// yet still decodes params and reports 405/404 exactly, and repeated request
// headers are still joined.
import { suite } from "./kit.js";
import { App, Metrics, TCPServer } from "dyna:net";

const t = suite("http_hot_path");
const app = new App({ port: 0 });
for (let i = 0; i < 30; i++) app.get("/filler/" + i + "/:id", () => ({ filler: i }));
app.get("/hello", () => ({ ok: true }));
app.get("/user/:id/post/:pid", (req) => ({ id: req.params.id, pid: req.params.pid, q: req.query }));
app.post("/user/:id/post/:pid", (req) => ({ posted: req.params.id }));
app.get("/files/*rest", (req) => ({ rest: req.params.rest }));
app.get("/echo-headers", (req) => req.headers);
app.get("/shape/:id", (req) => {
    const d = (k) => { const x = Object.getOwnPropertyDescriptor(req, k); return x.writable && x.enumerable && x.configurable && !x.get; };
    const keys = Reflect.ownKeys(req);
    const plain = keys.every(d);
    req.extra = 1;
    delete req.body;
    return { keys, plain, proto: Object.getPrototypeOf(req) === Object.prototype, after: Object.keys(req) };
});
app.start();
t.after(() => app.close());
const B = "http://127.0.0.1:" + app.port;
const count = (name) => {
    const m = new RegExp("^" + name + "(?:\\{[^}]*\\})? (\\S+)$", "m").exec(Metrics.scrape());
    return m ? Number(m[1]) : null;
};

t.test("params are decoded for the matching route only", async (a) => {
    a.deep(await (await fetch(B + "/user/a%20b/post/7?x=1&y=two")).json(), { id: "a b", pid: "7", q: { x: "1", y: "two" } });
    a.deep(await (await fetch(B + "/filler/29/zz")).json(), { filler: 29 });
    a.deep(await (await fetch(B + "/files/a/b%2Fc/d.txt")).json(), { rest: "a/b/c/d.txt" });
    a.deep(await (await fetch(B + "/files/")).json(), { rest: "" });
});

t.test("method mismatch is 405 with the allowed methods; unknown path is 404", async (a) => {
    const r = await fetch(B + "/user/1/post/2", { method: "DELETE" });
    a.eq(r.status, 405);
    a.deep((r.headers.get("allow") || "").split(", ").sort(), ["GET", "POST"]);
    a.eq((await fetch(B + "/hello", { method: "POST", body: "x" })).status, 405);
    a.eq((await fetch(B + "/user/1/post")).status, 404);
    a.eq((await fetch(B + "/user//post/2")).status, 404, "an empty :param segment does not match");
    a.eq((await fetch(B + "/nope")).status, 404);
});

t.test("the request object is a plain object with the documented keys", async (a) => {
    for (let i = 0; i < 3; i++)
        a.deep(await (await fetch(B + "/shape/" + i)).json(), {
            keys: ["method", "path", "params", "query", "headers", "body"], plain: true, proto: true,
            after: ["method", "path", "params", "query", "headers", "extra"],
        });
});

t.test("the same method on another matching pattern still dispatches", async (a) => {
    a.deep(await (await fetch(B + "/user/9/post/1", { method: "POST", body: "x" })).json(), { posted: "9" });
});

t.test("repeatable request headers are joined; names are lower-cased", async (a) => {
    const text = await new Promise((resolve, reject) => {
        let got = "";
        const timer = setTimeout(() => { h.close(); reject(new Error("no reply")); }, 4000);
        const h = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
            connect: (conn) => conn.write("GET /echo-headers HTTP/1.1\r\nHost: x\r\nAccept: one\r\nCookie: a=1\r\naccept: two\r\nCOOKIE: b=2\r\nX-Single: s\r\nConnection: close\r\n\r\n"),
            data: (conn, b) => { got += String.fromCharCode.apply(null, b); },
            close: () => { clearTimeout(timer); resolve(got); },
        });
    });
    const h = JSON.parse(text.slice(text.indexOf("\r\n\r\n") + 4));
    a.eq(h["accept"], "one, two");
    a.eq(h["cookie"], "a=1; b=2");
    a.eq(h["host"], "x");
    a.eq(h["x-single"], "s");
    a.eq(h["X-Single"], undefined);
});

t.test("built-in HTTP counters keep counting across Metrics.reset()", async (a) => {
    await fetch(B + "/hello");
    const before = count("dyn_http_responses_total");
    a.ok(before !== null && before >= 1, "counter present before reset: " + before);
    await fetch(B + "/hello");
    a.eq(count("dyn_http_responses_total"), before + 1, "one request, one increment");
    Metrics.reset();
    a.eq(count("dyn_http_responses_total"), null, "reset empties the registry");
    Metrics.counter("unrelated_first_slot", 5);
    await fetch(B + "/hello");
    await fetch(B + "/hello");
    a.eq(count("dyn_http_responses_total"), 2, "re-registered and counted after the reset");
    a.eq(count("unrelated_first_slot"), 5, "a stale slot index did not land on another metric");
    a.ok(count("dyn_http_request_duration_seconds_count") >= 1, "histogram re-registered too");
});

await t.run();
