// flags: --std
// covers: c:src/modules/http/http-app-routes.c
// App dispatch scanned every registered route for each request: the exact/mount pass walked the whole
// table (skipping dynamic patterns) and the dynamic pass matched every pattern in turn. With 1K routes
// that loop was ~10% of a small-response server profile and with 100K it was 97% (9.5K req/s against
// 128K at 1K routes). From 16 routes up the App now keeps an index, rebuilt when routes are added:
// exact paths by hash, static/proxy mounts as an ordered list, and dynamic patterns bucketed by a
// literal first segment, with patterns that start with :param or *rest kept in an ordered generic
// list. Dynamic candidates are the bucket and the generic list merged in registration order, so the
// first registered match still wins and a 405 Allow lists methods in registration order. The
// expected answers below were checked against the old linear dispatch (this file passes on the
// pre-index binary); the scenario then runs with filler routes before and after it and must answer
// the same, and a small app under the threshold keeps the linear path.
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { App } from "dyna:http";
import { Path } from "dyna:file";

const t = suite("net_app_http_route_index");
const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_ridx.${os.getpid()}.${Date.now()}`;
os.mkdir(T, 0o755);
os.mkdir(T + "/sub", 0o755);
os.mkdir(T + "/two", 0o755);
for (const [p, v] of [["/f.txt", "FILE"], ["/sub/f.txt", "SUB-OF-FIRST-MOUNT"], ["/two/f.txt", "SECOND-MOUNT"]]) { const f = std.open(T + p, "w"); f.puts(v); f.close(); }

function build(pad) {
    const app = new App({ port: 0, compress: false });
    for (let i = 0; i < pad; i++) app.get("/pre" + i + "/:x", () => "pre" + i);
    app.get("/:a/x", (r) => "gen-a:" + r.params.a);
    app.get("/u/:b", (r) => "u-b:" + r.params.b);
    app.get("/u/:b/deep", (r) => "u-deep:" + r.params.b);
    app.get("/v/:b", (r) => "v-b:" + r.params.b);
    app.get("/:a/x2", (r) => "gen-x2:" + r.params.a);
    app.post("/:a/y", () => "post-y");
    app.put("/w/:b", () => "put-w");
    app.patch("/:a/y", () => "patch-y");
    app.del("/w/:b", () => "del-w");
    app.get("/", () => "root");
    app.get("/lit/one", () => "lit-one");
    app.get("/lit/one", () => "lit-one-second");
    app.rpc("/rpc", { echo: (v) => "rpc:" + v });
    app.rpc("/rpc2", { which: () => "first" });
    app.rpc("/rpc2", { which: () => "second" });
    app.static("/m", new Path(T));
    app.static("/m/sub", new Path(T + "/two"));
    app.get("/rpc", () => "dyn-rpc-shadowed");
    app.get("/files/:name", (r) => "dyn-file:" + r.params.name);
    app.static("/files", new Path(T));
    app.static("/s", new Path(T));
    app.get("/s/:name", (r) => "dyn-s:" + r.params.name);
    for (let i = 0; i < pad; i++) app.get("/post" + i + "/:x", () => "post" + i);
    app.get("/*rest", (r) => "rest:" + r.params.rest);
    app.start();
    return app;
}

async function ask(port, method, path, body) {
    const opt = { method };
    if (body !== undefined) { opt.body = body; opt.headers = { "Content-Type": "application/json" }; }
    const r = await fetch(`http://127.0.0.1:${port}${path}`, opt);
    const txt = await r.text();
    const allow = r.headers && typeof r.headers.get === "function" ? r.headers.get("allow") : r.headers && (r.headers.Allow || r.headers.allow);
    const hget = (k) => r.headers && typeof r.headers.get === "function" ? r.headers.get(k) : r.headers && r.headers[k];
    return r.status + " " + txt + (allow ? " allow=" + allow : "") + (method === "HEAD" ? " len=" + hget("content-length") : "");
}

const CASES = [
    ["GET", "/u/x", "200 gen-a:u", "a generic pattern registered first beats a later literal bucket"],
    ["GET", "/u/7", "200 u-b:7", "the literal bucket answers when the generic one does not match"],
    ["GET", "/u/7/deep", "200 u-deep:7", "later patterns in one bucket still match"],
    ["GET", "/v/x2", "200 v-b:x2", "a bucket route registered before a generic one wins"],
    ["GET", "/q/x2", "200 gen-x2:q", "generic patterns serve first segments that have no bucket"],
    ["PUT", "/k/y", null, "405 lists POST and PATCH from the generic list, then the catch-all"],
    ["PATCH", "/w/1", null, "405 lists PUT and DELETE from the bucket, then the catch-all"],
    ["POST", "/k/y", "200 post-y", "method picks among generic routes"],
    ["PUT", "/w/1", "200 put-w", "method picks among bucket routes"],
    ["GET", "/", "200 root", "the root pattern"],
    ["GET", "//u//9", "200 u-b:9", "repeated slashes collapse before lookup"],
    ["GET", "/lit/one", "200 lit-one", "duplicate patterns: the first registered wins"],
    ["POST", "/rpc", null, "an exact typed route beats a dynamic one on the same path"],
    ["GET", "/files/f.txt", "200 dyn-file:f.txt", "a dynamic route registered before a mount wins over it"],
    ["GET", "/s/f.txt", "200 dyn-s:f.txt", "dynamic routes win over a mount whatever the order"],
    ["GET", "/s", null, "a mount answers its bare prefix"],
    ["GET", "/zz/top/more", "200 rest:zz/top/more", "a trailing *rest catches the rest"],
    ["GET", "/pre3/q", null, "filler routes answer their own paths"],
    ["POST", "/rpc2", null, "of two exact routes on one path the first registered wins"],
    ["HEAD", "/m/sub/f.txt", "200  len=18", "of two nested mounts the first registered wins (HEAD: GET is the catch-all's)"],
];

async function runAll(pad) {
    const app = build(pad);
    const out = [];
    for (const [m, p, , ] of CASES) {
        const body = p === "/rpc" ? JSON.stringify({ jsonrpc: "2.0", method: "echo", params: ["v"], id: 1 })
            : p === "/rpc2" ? JSON.stringify({ jsonrpc: "2.0", method: "which", id: 2 }) : undefined;
        out.push(await ask(app.port, m, p, body));
    }
    app.get("/late/:x", () => "late-get");
    app.post("/late/:x", (r) => "late:" + r.params.x);
    out.push(await ask(app.port, "POST", "/late/5"));
    out.push(await ask(app.port, "GET", "/late/5"));
    app.close();
    return out;
}

let ref = null;
t.test("every case answers as the linear dispatch did", async (a) => {
    ref = await runAll(0);
    CASES.forEach(([m, p, want, why], i) => { if (want) a.eq(ref[i], want, why + " (" + m + " " + p + ")"); });
    a.match(ref[5], /^405 .*allow=POST, PATCH, GET$/, "generic 405");
    a.match(ref[6], /^405 .*allow=PUT, DELETE, GET$/, "bucket 405");
    a.match(ref[12], /^200 .*rpc:v/, "rpc answered");
    a.match(ref[15], /^(403|404|200|301)/, "mount answered");
    a.eq(ref[17], "200 rest:pre3/q", "with no filler the catch-all takes it");
    a.match(ref[18], /^200 .*"result":"first"/, "first rpc route answered");
    a.eq(ref[20], "200 late:5", "a route added after start is served");
    a.eq(ref[21], "200 rest:late/5", "and one shadowed by the earlier catch-all stays shadowed");
});

for (const pad of [8, 600]) {
    t.test(`with ${2 * pad} filler routes (index) the answers are identical`, async (a) => {
        if (!ref) ref = await runAll(0);
        const got = await runAll(pad);
        CASES.forEach(([m, p, , why], i) => {
            if (i === 17) a.eq(got[i], "200 pre3", "filler route answers");
            else a.eq(got[i], ref[i], why + " (" + m + " " + p + ")");
        });
        a.eq(got[20], "200 late:5", "the index is rebuilt for a route added after start");
        a.eq(got[21], "200 rest:late/5", "registration order still decides after the rebuild");
    });
}

t.test("an app under 16 routes keeps the same order rules", async (a) => {
    const app = new App({ port: 0, compress: false });
    app.get("/:a/x", (r) => "gen:" + r.params.a);
    app.get("/u/:b", (r) => "u:" + r.params.b);
    app.post("/u/:b", () => "post-u");
    app.get("/lit", () => "lit");
    app.start();
    a.eq(await ask(app.port, "GET", "/u/x"), "200 gen:u");
    a.eq(await ask(app.port, "GET", "/u/9"), "200 u:9");
    a.eq(await ask(app.port, "POST", "/u/9"), "200 post-u");
    a.match(await ask(app.port, "PUT", "/u/9"), /^405 .*allow=GET, POST$/);
    a.eq(await ask(app.port, "GET", "/lit"), "200 lit");
    app.close();
});

t.test("every one of 5000 literal-prefixed routes reaches its own handler", async (a) => {
    const app = new App({ port: 0, compress: false });
    for (let i = 0; i < 5000; i++) app.get("/r" + i + "/:x", (r) => i + ":" + r.params.x);
    app.get("/:any/:x", () => "generic");
    app.start();
    let bad = 0;
    for (let i = 0; i < 5000; i += 97) if ((await ask(app.port, "GET", "/r" + i + "/v" + i)) !== "200 " + i + ":v" + i) bad++;
    a.eq(bad, 0);
    a.eq(await ask(app.port, "GET", "/r5000/v"), "200 generic", "an unknown first segment falls to the generic list");
    a.eq(await ask(app.port, "GET", "/r12"), "404 {\"error\":\"not found\"}", "a bucket hit that matches no pattern is 404");
    app.close();
});

await t.run();
for (const p of ["/f.txt", "/sub/f.txt", "/two/f.txt", "/sub", "/two", ""]) os.remove(T + p);
