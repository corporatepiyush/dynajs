// flags: --std
// timeout: 300
// covers: dyna:http.App
// slots: 2
import { HTTPClient } from "dyna:http";
import { spawnHelper } from "../lib/helper.js";
import * as std from "std";
import * as os from "os";
import { suite } from "../lib/kit.js";

// The rawpeer.js origin, the App child and the rawpeer.js prober all run in the kit's before
// hook (never under --list); each case reads one door's verdicts.
const t = suite("test_http_status_range");
let A = null;
const ok = (c, m) => A.ok(c, m);
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

let why = null, T = null, ORIGIN = 0, APP = 0, SRV = 0, nofix = false, reap = null, appLog = "", evilLog = "";
const rows = { dyn: {}, static: {} };
t.before(() => {
    T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_status.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/srv.js`, `
import { App, HTTPServerAsync } from "dyna:http";
import * as std from "std";

const app = new App({ port: 0 });
/* The DYNAMIC door: a handler-supplied envelope status, at response time. */
app.get("/env", (req) => ({ status: +(req.query && req.query.s) || 200, body: "hi" }));
app.start();
{ const f = std.open(scriptArgs[1], "w"); f.puts(String(app.port)); f.close(); }

/* The STATIC door: a status fixed at REGISTRATION, on the server that owns
   dyn_route_copy. It refuses there, so a refused value can never reach the
   wire -- which is why this door is pinned by its THROW, not by a request. */
const routes = {};
for (const s of [200, 404, 500, 599]) routes["/st" + s] = { status: s, body: "hi" };
const srv = new HTTPServerAsync({ port: 0, routes });
srv.start();
{ const f = std.open(scriptArgs[2], "w"); f.puts(String(srv.port)); f.close(); }
setInterval(() => {}, 1000);
`);

    const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
    write(`${T}/evil.js`, `import { listen, latin1 } from ${KIT};
const CASES = {
    "/s100": 100, "/s101": 101, "/s199": 199, "/s200": 200, "/s226": 226,
    "/s301": 301, "/s308": 308, "/s400": 400, "/s404": 404, "/s418": 418,
    "/s451": 451, "/s499": 499, "/s500": 500, "/s502": 502, "/s503": 503,
    "/s598": 598, "/s599": 599,
    "/s600": 600, "/s601": 601, "/s649": 649, "/s700": 700, "/s999": 999,
    "/s1000": 1000, "/s1200": 1200, "/s1999": 1999, "/s2000": 2000, "/s99999": 99999,
    "/s100_then_200": 100, "/s103_then_200": 103,
};
const INTERIM = ["/s100_then_200", "/s103_then_200"];
const NL = "\\r\\n";
listen(async (c) => {
    const r = await c.readUntil(NL + NL, { timeoutMs: 4000 });
    if (r.timedOut) return;
    const d = latin1(r.bytes);
    const p = d.includes(" ") ? d.split(" ")[1] : "/";
    const st = Object.prototype.hasOwnProperty.call(CASES, p) ? CASES[p] : 200;
    await c.write(INTERIM.includes(p)
        ? "HTTP/1.1 " + st + " Continue" + NL + "Content-Length: 0" + NL + NL + "HTTP/1.1 200 OK" + NL + "Content-Length: 2" + NL + NL + "hi"
        : "HTTP/1.1 " + st + " Weird" + NL + "Content-Length: 2" + NL + NL + "hi");
}, { portFile: scriptArgs[1] });
`);

    write(`${T}/probe.js`, `import { connect, latin1 } from ${KIT};
import * as file from "dyna:file";
const T = scriptArgs[1], NL = "\\r\\n";
const port = (n) => { try { return parseInt(String(file.readFile(T + "/" + n)).trim(), 10) || 0; } catch { return 0; } };
const APP = port("app.port"), SRV = port("srv.port");
async function ask(p, path) {
    const c = await connect(p, "127.0.0.1", { connectTimeoutMs: 5000 });
    await c.write("GET " + path + " HTTP/1.1" + NL + "Host: v" + NL + "Connection: close" + NL + NL);
    const r = await c.readAll({ timeoutMs: 2000 });
    c.close();
    const f = (latin1(r.bytes) + (r.timedOut ? "<<TIMEOUT>>" : "")).split(NL)[0].split(" ");
    return f.length > 1 ? f[1] : "?";
}
if (APP === 0 || SRV === 0) print("NOFIXTURE");
else {
    for (const s of [200, 404, 500, 599, 600, 601, 700, 999]) print("dyn\t" + s + "\t" + await ask(APP, "/env?s=" + s));
    for (const s of [200, 404, 500, 599]) print("static\t" + s + "\t" + await ask(SRV, "/st" + s));
}
`);


    const startBg = (cmd, tag) => {
        sh(`rm -f ${T}/${tag}.pid`); spawnHelper(`${cmd} >${T}/${tag}.log 2>&1`, { pidFile: `${T}/${tag}.pid` });
    };
    const waitPort = (tag) => {
        for (let i = 0; i < 400; i++) {
            const p = parseInt((cat(`${T}/${tag}.port`) || "").trim(), 10) || 0;
            if (p > 0) return p;
            sh("sleep 0.05");
        }
        return 0;
    };
    reap = () => sh(`for f in ${T}/*.pid; do [ -f "$f" ] || continue; ` +
        `p=$(cat "$f" 2>/dev/null); case "$p" in ''|*[!0-9]*) continue;; esac; ` +
        `case "$(ps -p $p -o command= 2>/dev/null)" in *"${T}/"*) ` +
        `kill $p 2>/dev/null;; esac; done; rm -rf ${T}`);

    startBg(`${EXE} --std ${T}/evil.js ${T}/evil.port`, "evil");
    ORIGIN = waitPort("evil");
    if (ORIGIN === 0) { evilLog = cat(`${T}/evil.log`).slice(0, 400); return; }
    startBg(`${EXE} --std ${T}/srv.js ${T}/app.port ${T}/srv.port`, "app");
    APP = waitPort("app");
    SRV = waitPort("srv");
    if (APP === 0 || SRV === 0) { appLog = cat(`${T}/app.log`).slice(0, 600); return; }
    sh(`${EXE} --std ${T}/probe.js ${T} > ${T}/probe.out 2>&1`);
    for (const line of cat(`${T}/probe.out`).split("\n")) {
        const f = line.split("\t");
        if (f[0] === "NOFIXTURE") nofix = true;
        if (f.length < 3 || !rows[f[0]]) continue;
        rows[f[0]][f[1]] = f[2];
    }
});
t.after(() => { if (reap) reap(); });

const get = async (path) => {
    try {
        const r = await new HTTPClient().get(`http://127.0.0.1:${ORIGIN}${path}`);
        return { ok: true, status: r.status };
    } catch (e) {
        return { ok: false, err: String(e && e.message || e) };
    }
};
const test = (n, fn, need) => t.test(n, (a) => {
    A = a;
    if (why) a.skip(why);
    if (need === "origin" && ORIGIN === 0) a.must(false, "the origin fixture never bound a port: " + evilLog);
    if (need === "app" && (APP === 0 || SRV === 0)) a.must(false, "the App fixture never bound its ports: " + appLog);
    return fn(a);
});

test("fixture: the origin, the App and the static server bind ports and the prober reaches them", async () => {
    ok(ORIGIN > 0, "fixture: the origin bound a port");
    ok(APP > 0, "fixture: the App bound a port");
    ok(SRV > 0, "fixture: the static HTTPServer bound a port");
    ok(!nofix, "fixture: the prober reached both server doors");
});

test("client: statuses 200-599 are accepted and reported as themselves", async () => {
    for (const s of [200, 226, 301, 308, 400, 404, 418, 451, 499, 500, 502,
                     503, 598, 599]) {
        const r = await get("/s" + s);
        ok(r.ok && r.status === s,
           "client: status " + s + " is accepted and reported as itself " +
           "[got " + (r.ok ? r.status : "threw: " + r.err) + "]");
    }
    ok((await get("/s599")).ok && (await get("/s599")).status === 599,
       "client: 599 is the LAST accepted status");
}, "origin");

test("client: statuses 600-999 are refused", async () => {
    for (const s of [600, 601, 649, 700, 999]) {
        const r = await get("/s" + s);
        ok(!r.ok,
           "client: status " + s + " is REFUSED -- 600-999 is IANA " +
           "private-use space, not a status this engine can mean " +
           "[got " + (r.ok ? "status " + r.status : "threw: " + r.err) + "]");
    }
}, "origin");

test("client: statuses of four or more digits are refused", async () => {
    for (const s of [1000, 1200, 1999, 2000, 99999]) {
        const r = await get("/s" + s);
        ok(!r.ok,
           "client: a " + String(s).length + "-digit status is still refused " +
           "by M-03's digit rule, not by the range [got " +
           (r.ok ? "status " + r.status : "threw: " + r.err) + "]");
    }
}, "origin");

test("client: an interim 1xx is skipped for the final response", async () => {
    for (const p of ["/s100_then_200", "/s103_then_200"]) {
        const r = await get(p);
        ok(r.ok && r.status === 200,
           "client: " + p + " still skips the interim and reports the FINAL " +
           "response [got " + (r.ok ? r.status : "threw: " + r.err) + "]");
    }
}, "origin");

test("client control: an ordinary 200 works end to end", async () => {
    const good = await get("/s200");
    ok(good.ok && good.status === 200 && (await get("/s200")).ok,
       "client control: an ordinary 200 still works end to end");
}, "origin");

test("App envelope door: statuses 200-599 are served as themselves", async () => {
    for (const s of [200, 404, 500, 599]) {
        ok(rows.dyn[s] === String(s),
           "App envelope door: status " + s + " is served as itself " +
           "[got " + JSON.stringify(rows.dyn[s]) + "]");
    }
}, "app");

test("App envelope door: statuses 600 and above never reach the wire as themselves", async () => {
    for (const s of [600, 601, 700, 999]) {
        ok(rows.dyn[s] !== String(s),
           "App envelope door: status " + s + " never reaches the wire as " +
           "itself [got " + JSON.stringify(rows.dyn[s]) + "]");
    }
}, "app");

test("static route door: registered statuses are served as themselves", async () => {
    for (const s of [200, 404, 500, 599]) {
        ok(rows.static[s] === String(s),
           "static route door: a status of " + s + " registered and is " +
           "served as itself [got " + JSON.stringify(rows.static[s]) + "]");
    }
}, "app");

await t.run();
