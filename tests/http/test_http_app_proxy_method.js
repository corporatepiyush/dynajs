// flags: --std
// timeout: 300
// covers: dyna:http.App
// slots: 2
// reach: dyn_app_proxy_start is called only from dispatch on the raw wire head; app.proxy takes only host/port, so no JS-built method reaches it. scan_request_line (src/modules/http/http-scan.c) sets bad_target for an empty or non-tchar method and dispatch answers 400 before routing, so the proxy keeps only the 24-byte cap the parser lacks
// peer-proof: drop drive out:^(?!HTTP/1\.1 200 OK) -- every request the driver sends goes to the App, whose port is announced in a file written after launch, so the recording proxy never sits in front of it; only the dynajs run records those; the upstream side (it sends only the 200 reply) is compared
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against HEAD with drive.py taking the upstream port file as its second argument, so the recording proxy sits in front of the upstream peer
import * as std from "std";
import { spawnHelper } from "../lib/helper.js";
import * as os from "os";

import { suite } from "../lib/kit.js";

const t = suite("test_http_proxy_method");
let A = null, why = null, T = null, reap = () => {}, rows = {}, UPPORT = 0, P = 0;
const ok = (c, m) => A.ok(c, m);
const fixtureChecks = [];
const test = (n, fn, o) => t.test(n, (a) => { A = a; if (why) a.skip(why); return fn(a); }, o);
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
const hexToStr = (h) => {
    let s = "";
    for (let i = 0; i + 1 < h.length; i += 2)
        s += String.fromCharCode(parseInt(h.substr(i, 2), 16));
    return s;
};

t.before(() => {
    if (why) return;
    A = { ok: (c, m) => { fixtureChecks.push([c, m]); return c; } };
    T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_proxy_meth.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/srv.js`, `
import { App } from "dyna:http";
import * as std from "std";
const app = new App({ port: 0 });
app.proxy("/api", { host: "127.0.0.1", port: +scriptArgs[2] });
/* A normal route with its own method door, so the two paths can be compared
   on the SAME bytes rather than argued about. */
app.get("/echo", req => ({ body: { ok: 1 } }));
app.start();
{ const f = std.open(scriptArgs[1], "w"); f.puts(String(app.port)); f.close(); }
setInterval(() => {}, 1000);
`);

    write(`${T}/drive.js`, `import { listen, connect, latin1 } from ${KIT};
import * as std from "std";
const T = scriptArgs[1];
const CR = "\\r", LF = "\\n", CRLF = CR + LF;
const B = (...c) => String.fromCharCode(...c);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const out = (line) => { std.out.puts(line + "\\n"); std.out.flush(); };
const OKBODY = "HTTP/1.1 200 OK" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP";
const CASES = [
    ["get", "GET"], ["post", "POST"], ["head", "HEAD"], ["put", "PUT"], ["delete", "DELETE"], ["patch", "PATCH"], ["options", "OPTIONS"],
    ["propfind", "PROPFIND"], ["m-search", "M-SEARCH"], ["m24", "A".repeat(24)], ["m25", "A".repeat(25)], ["m0", ""], ["bogus", "BOGUS"],
    ["tchar", "!#$%&'*+-.^_" + B(96) + "|~09azAZ"],
    ["nul", "G" + B(0) + "T"], ["cr", "G" + CR + "T"], ["lf", "G" + LF + "T"], ["sp", "G T"], ["tab", "G" + B(9) + "T"],
    ["del", "G" + B(127) + "T"], ["hi", "G" + B(127) + "T"], ["soh", "G" + B(1) + "T"], ["vt", "G" + B(11) + "T"], ["ff", "G" + B(12) + "T"], ["us", "G" + B(31) + "T"],
];
for (const c of [...Array(0x20).keys(), 0x7f]) CASES.push(["c0_" + c.toString(16).padStart(2, "0"), "GET" + B(c) + "x"]);

const seen = [];
listen(async (c) => {
    const h = await c.readUntil(CRLF + CRLF, { timeoutMs: 4000 });
    seen.push(latin1(h.bytes));
    await c.write(OKBODY);
    c.close();
}, { portFile: scriptArgs[2] });

let PROXY = 0;
for (let i = 0; i < 600 && !PROXY; i++) {
    const v = parseInt(String(std.loadFile(T + "/proxy.port") || "").trim(), 10);
    if (v > 0) PROXY = v; else await sleep(50);
}
if (!PROXY) { out("NOPROXY\\tERR\\t-"); std.exit(0); }

const WS = /^[ \\t\\n\\r\\x0b\\x0c]+|[ \\t\\n\\r\\x0b\\x0c]+$/g;
const pyStrip = (s) => s.replace(WS, "");
const asciiLower = (s) => s.replace(/[A-Z]/g, (ch) => ch.toLowerCase());
const hexOf = (s) => { let h = ""; for (let i = 0; i < s.length; i++) h += (s.charCodeAt(i) & 255).toString(16).padStart(2, "0"); return h; };
const statusOf = (acc) => { const sp = acc.split(CRLF)[0].split(" "); return sp.length > 1 ? sp[1] : ""; };

async function exchange(req, withBody) {
    let s;
    try { s = await connect(PROXY, "127.0.0.1", { connectTimeoutMs: 5000 }); }
    catch (e) { return "<<" + String(e.message) + ">>"; }
    await s.write(req);
    const h = await s.readUntil(CRLF + CRLF, { timeoutMs: 1500 });
    let acc = latin1(h.bytes);
    if (h.timedOut) acc += "<<TIMEOUT>>";
    else if (h.found && withBody) {
        let cl = 0;
        for (const ln of acc.slice(0, acc.length - 4).split(CRLF).slice(1)) {
            const k = ln.indexOf(":");
            if (k < 0) continue;
            if (asciiLower(pyStrip(ln.slice(0, k))) === "content-length") { const v = pyStrip(ln.slice(k + 1)); cl = /^[+-]?[0-9]+$/.test(v) ? Number(v) : 0; }
        }
        if (cl > 0) { const b = await s.readBytes(cl, { timeoutMs: 1500 }); acc += latin1(b.bytes); if (b.timedOut) acc += "<<TIMEOUT>>"; }
    }
    s.abort();
    return acc;
}
const ask = async (m) => statusOf(await exchange(m + " /api/thing HTTP/1.1" + CRLF + "Host: victim" + CRLF + "Content-Length: 0" + CRLF + CRLF, true));
const askLocal = async (m) => statusOf(await exchange(m + " /echo HTTP/1.1" + CRLF + "Host: v" + CRLF + "Content-Length: 0" + CRLF + CRLF, false));

for (const [name, method] of CASES) {
    const before = seen.length;
    const st = await ask(method);
    let got = "";
    for (const d of seen.slice(before)) got = d;
    const lo = await askLocal(method);
    out(name + "\\t" + st + "\\t" + hexOf(got.slice(0, 40)) + "\\t" + lo);
}
out("DONE\\t-");
std.exit(0);
`);

    const startBg = (cmd, tag) => {
        sh(`rm -f ${T}/${tag}.pid`); spawnHelper(`${cmd} >${T}/${tag}.log 2>&1`, { pidFile: `${T}/${tag}.pid` });
    };
    const waitPort = (tag) => {
        for (let i = 0; i < 300; i++) {
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

    startBg(`${EXE} --std ${T}/drive.js ${T} ${T}/up.port`, "drv");
    for (let i = 0; i < 200; i++) {
        if (parseInt((cat(`${T}/up.port`) || "").trim(), 10) > 0) break;
        sh("sleep 0.05");
    }
    UPPORT = parseInt((cat(`${T}/up.port`) || "").trim(), 10) || 0;
    ok(UPPORT > 0, "fixture: the recording origin bound a port");
    if (UPPORT === 0) {
        print("  driver log: " + cat(`${T}/drv.log`).slice(0, 400));
        reap();
    } else {
        startBg(`${EXE} --std ${T}/srv.js ${T}/app.port ${UPPORT}`, "app");
        P = waitPort("app");
        ok(P > 0, "fixture: the App bound an ephemeral port");
        if (P === 0) {
            print("  app log: " + cat(`${T}/app.log`).slice(0, 400));
        } else {
            write(`${T}/proxy.port`, String(P));
            for (let i = 0; i < 800; i++) {
                const l = cat(`${T}/drv.log`);
                if (l.indexOf("DONE") >= 0 || l.indexOf("NOPROXY") >= 0) break;
                sh("sleep 0.05");
            }
            rows = {};
            for (const line of cat(`${T}/drv.log`).split("\n")) {
                const f = line.split("\t");
                if (f[0] === "DONE") continue;
                if (f.length < 4 || !f[0]) continue;
                rows[f[0]] = { status: f[1], upstream: hexToStr(f[2]),
                               local: f[3] };
            }
        }
    }
});
t.after(() => reap());

const r = (n) => rows[n];
const notForwarded = (n) => {
    const x = r(n);
    return !!x && x.upstream === "";
};
function isBad(x) { return !!x && x.status && x.status[0] === "4"; }

test("fixture: the origin and the App bound ports", (a) => {
    for (const [c, m] of fixtureChecks) a.ok(c, m);
});

test("well-formed methods, including unregistered tokens and the 24-byte cap, are proxied verbatim", () => {
    ok(!!r("get") && r("get").local === "200",
       "get: the route that DOES list GET answers 200 on the normal path");
    for (const n of ["get", "post", "head", "put", "delete", "patch",
                     "options", "propfind", "m-search", "tchar", "m24"]) {
        const x = r(n);
        ok(!!x && x.status === "200" && x.upstream.indexOf(x.upstream) === 0 &&
           x.upstream.length > 0,
           n + ": a well-formed method is proxied and reaches the upstream " +
           "[status=" + (x ? x.status : "?") + " upstream=" +
           JSON.stringify((x ? x.upstream : "").slice(0, 30)) + "]");
    }
    ok(!!r("get") && r("get").upstream.indexOf("GET /thing HTTP/1.1") === 0,
       "get: the upstream received exactly the re-serialised request line " +
       "[" + JSON.stringify(r("get") ? r("get").upstream : "?") + "]");
    ok(!!r("propfind") && r("propfind").upstream.indexOf("PROPFIND") === 0,
       "propfind: an UNREGISTERED but well-formed token is forwarded -- a " +
       "proxy that dropped methods it does not know would break WebDAV");
    ok(!!r("m24") && r("m24").status === "200",
       "m24: a 24-byte method is at the cap and still proxied");
    ok(isBad(r("m25")), "m25: a 25-byte method is past the cap and refused");

});

test("a method that is not a token is refused and nothing reaches the upstream", () => {
    for (const n of ["nul", "cr", "lf", "sp", "tab", "del", "soh", "vt",
                     "ff", "us", "m0"]) {
        ok(isBad(r(n)),
           n + ": a method that is not a token is refused [status=" +
           (r(n) ? r(n).status : "?") + "]");
        ok(notForwarded(n),
           n + ": ...and NOTHING reached the upstream [upstream=" +
           JSON.stringify(r(n) ? r(n).upstream : "?") + "]");
    }
});

test("every C0 byte and DEL appended to a method is refused and not forwarded", () => {
    for (let c = 0x00; c <= 0x1f; c++) {
        const n = "c0_" + (c < 16 ? "0" : "") + c.toString(16);
        ok(isBad(r(n)) && notForwarded(n),
           "method with 0x" + c.toString(16).padStart(2, "0") +
           " appended: refused AND not forwarded [status=" +
           (r(n) ? r(n).status : "?") + " upstream=" +
           JSON.stringify(r(n) ? r(n).upstream : "?") + "]");
    }
    ok(isBad(r("c0_7f")) && notForwarded("c0_7f"),
       "method with DEL appended: refused AND not forwarded");
});

test("the proxy and the normal route agree on every refused method", () => {
    for (const n of ["nul", "cr", "lf", "sp", "tab", "del", "soh", "vt",
                     "ff", "us", "m0", "c0_01", "c0_0a", "c0_7f"]) {
        const x = r(n);
        ok(!!x && isBad(x) && isBad({ status: x.local }),
           n + ": the proxy AND the normal route agree -- both refuse " +
           "[proxy=" + (x ? x.status : "?") + " normal=" + (x ? x.local : "?") + "]");
    }
});

test("an unlisted well-formed method is 405 on a route but forwarded by the proxy", () => {
    for (const n of ["post", "put", "delete", "options", "bogus"]) {
        const x = r(n);
        ok(!!x && x.local === "405" && x.status === "200",
           n + ": a well-formed method the route does not list is a 405 on " +
           "the normal route, while the proxy FORWARDS it -- forwarding a " +
           "method you do not recognise is the proxy's job " +
           "[normal=" + (x ? x.local : "?") + " proxy=" + (x ? x.status : "?") + "]");
    }
});

await t.run();
