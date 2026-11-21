// flags: --std
// timeout: 300
// covers: dyna:http.App
// slots: 2
// peer-proof: drop drive out:^GET /api/ -- the driver's client side connects to the App, whose port is announced in a file written after launch, so the recording proxy never sits in front of it; only the dynajs run records those requests
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against HEAD with drive.py taking the upstream port file as its second argument, so the recording proxy sits in front of the upstream peer
import * as std from "std";
import { spawnHelper } from "../lib/helper.js";
import * as os from "os";

import { suite } from "../lib/kit.js";

const t = suite("test_http_proxy_framing");
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
    T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_proxy_frame.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    write(`${T}/srv.js`, `
import { App } from "dyna:http";
import * as std from "std";
const app = new App({ port: 0 });
app.proxy("/api", { host: "127.0.0.1", port: +scriptArgs[2] });
app.start();
{ const f = std.open(scriptArgs[1], "w"); f.puts(String(app.port)); f.close(); }
setInterval(() => {}, 1000);
`);

    write(`${T}/drive.js`, `import { listen, connect, latin1 } from ${KIT};
import * as std from "std";
const T = scriptArgs[1];
const CR = "\\r", LF = "\\n", CRLF = CR + LF;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const out = (line) => { std.out.puts(line + "\\n"); std.out.flush(); };
const OK = "HTTP/1.1 200 OK" + CRLF;
const CHUNKED_TAIL = "0" + CRLF + CRLF;
const CASES = {
    cl_exact: OK + "Content-Length: 2" + CRLF + CRLF + "UP",
    cl0: OK + "Content-Length: 0" + CRLF + CRLF,
    cl_big: OK + "Content-Length: 1024" + CRLF + CRLF + "Z".repeat(1024),
    cl_short: OK + "Content-Length: 2" + CRLF + CRLF + "UPX",
    cl_long: OK + "Content-Length: 5" + CRLF + CRLF + "UP",
    cl_zero_1: OK + "Content-Length: 0" + CRLF + CRLF + "X",
    smuggle: OK + "Content-Length: 2" + CRLF + CRLF + "UPX" + "HTTP/1.1 200 OK" + CRLF + "Content-Length: 4" + CRLF + CRLF + "JUNK",
    cl_plus: OK + "Content-Length: +2" + CRLF + CRLF + "UP",
    cl_junk: OK + "Content-Length: abc" + CRLF + CRLF + "UP",
    cl_empty: OK + "Content-Length:" + CRLF + CRLF + "UP",
    cl_huge: OK + "Content-Length: 999999999" + CRLF + CRLF + "UP",
    cl_osp: OK + "Content-Length:  2  " + CRLF + CRLF + "UP",
    cl_2eq: OK + "Content-Length: 2" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP",
    cl_2diff: OK + "Content-Length: 2" + CRLF + "Content-Length: 3" + CRLF + CRLF + "UP",
    chunked: OK + "Transfer-Encoding: chunked" + CRLF + CRLF + "2" + CRLF + "UP" + CRLF + CHUNKED_TAIL,
    chunked_x: OK + "Transfer-Encoding: chunked" + CRLF + CRLF + "2;ext=1" + CRLF + "UP" + CRLF + "0" + CRLF + "X-T: 1" + CRLF + CRLF,
    te_lower: OK + "transfer-encoding: chunked" + CRLF + CRLF + "2" + CRLF + "UP" + CRLF + CHUNKED_TAIL,
    te_id: OK + "Transfer-Encoding: identity" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP",
    cl_te: OK + "Content-Length: 2" + CRLF + "Transfer-Encoding: chunked" + CRLF + CRLF + "2" + CRLF + "UP" + CRLF + CHUNKED_TAIL,
    nocl: OK + "Connection: close" + CRLF + CRLF + "body to close",
    nocl_empty: OK + "Connection: close" + CRLF + CRLF,
    h10_cl: "HTTP/1.0 200 OK" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP",
    h10_nocl: "HTTP/1.0 200 OK" + CRLF + "Connection: close" + CRLF + CRLF + "body to close",
    s204: "HTTP/1.1 204 No Content" + CRLF + "Content-Length: 0" + CRLF + CRLF,
    s404: "HTTP/1.1 404 Not Found" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP",
    no_head: "garbage with no blank line at all",
    pfx_len: OK + "Content-Length: 2" + CRLF + "Content-Len: 99" + CRLF + CRLF + "UP",
    pfx_l: OK + "Content-Length: 2" + CRLF + "Content-L: 99" + CRLF + CRLF + "UP",
    pfx_long: OK + "Content-Length: 2" + CRLF + "Content-Length-Extra: 99" + CRLF + CRLF + "UP",
    pfx_x: OK + "Content-Length: 2" + CRLF + "X-Content-Length: 99" + CRLF + CRLF + "UP",
    pfx_te: OK + "Content-Length: 2" + CRLF + "Transfer-Encodin: chunked" + CRLF + CRLF + "UP",
    pfx_upper: "HTTP/1.1 200 OK" + CRLF + "CONTENT-LENGTH: 2" + CRLF + "X-A: b" + CRLF + CRLF + "UP",
    pfx_mixed: "HTTP/1.1 200 OK" + CRLF + "CoNtEnT-LeNgTh: 2" + CRLF + "X-A: b" + CRLF + CRLF + "UP",
    cl_in_value: OK + "Content-Length: 2" + CRLF + "X-A: Content-Length: 2" + CRLF + CRLF + "UP",
};

listen(async (c) => {
    const h = await c.readUntil(CRLF + CRLF, { timeoutMs: 4000 });
    const d = latin1(h.bytes);
    const p = d.includes(" ") ? d.split(" ")[1].replace(/^\\/+/, "") : "";
    await c.write(Object.hasOwn(CASES, p) ? CASES[p] : CASES.cl_exact);
    c.close();
}, { portFile: scriptArgs[2] });

let PROXY = 0;
for (let i = 0; i < 600 && !PROXY; i++) {
    const v = parseInt(String(std.loadFile(T + "/proxy.port") || "").trim(), 10);
    if (v > 0) PROXY = v; else await sleep(50);
}
if (!PROXY) { out("NOPROXY\\tERR"); std.exit(0); }

const WS = /^[ \\t\\n\\r\\x0b\\x0c]+|[ \\t\\n\\r\\x0b\\x0c]+$/g;
const pyStrip = (s) => s.replace(WS, "");
const asciiLower = (s) => s.replace(/[A-Z]/g, (ch) => ch.toLowerCase());
const pyInt = (v) => { const t = pyStrip(v); return /^[+-]?[0-9]+$/.test(t) ? Number(t) : null; };
const hexOf = (s) => { let h = ""; for (let i = 0; i < s.length; i++) h += (s.charCodeAt(i) & 255).toString(16).padStart(2, "0"); return h; };
const clOf = (head, dflt) => {
    let cl = dflt, seen = false;
    for (const ln of head.split(CRLF).slice(1)) {
        const k = ln.indexOf(":");
        if (k < 0) continue;
        if (asciiLower(pyStrip(ln.slice(0, k))) === "content-length") { seen = true; const n = pyInt(ln.slice(k + 1)); cl = n === null ? dflt : n; }
    }
    return { cl, seen };
};

async function ask(path) {
    let acc = "";
    let s;
    try { s = await connect(PROXY, "127.0.0.1", { connectTimeoutMs: 5000 }); }
    catch (e) { return "<<" + String(e.message) + ">>"; }
    await s.write("GET /api/" + path + " HTTP/1.1" + CRLF + "Host: victim" + CRLF + "Content-Length: 0" + CRLF + CRLF);
    const h = await s.readUntil(CRLF + CRLF, { timeoutMs: 1500 });
    acc = latin1(h.bytes);
    if (h.timedOut) acc += "<<TIMEOUT>>";
    else if (h.found) {
        const { cl, seen } = clOf(acc.slice(0, acc.length - 4), 0);
        if (cl > 0) { const b = await s.readBytes(cl, { timeoutMs: 1500 }); acc += latin1(b.bytes); if (b.timedOut) acc += "<<TIMEOUT>>"; }
        else if (!seen) { const b = await s.readAll({ timeoutMs: 1500 }); acc += latin1(b.bytes); }
    }
    s.abort();
    return acc;
}
function facts(buf) {
    const i = buf.indexOf(CRLF + CRLF);
    if (i < 0) return ["", -1, ""];
    const parts = buf.slice(0, i).split(CRLF);
    const sp = parts[0].split(" ");
    return [sp.length > 1 ? sp[1] : "", clOf(buf.slice(0, i), -1).cl, buf.slice(i + 4)];
}
for (const name of Object.keys(CASES).sort()) {
    const [st, cl, body] = facts(await ask(name));
    out(name + "\\t" + st + "\\t" + cl + "\\t" + body.length + "\\t" + hexOf(body.slice(0, 200)));
}
{
    const [st, cl, body] = facts(await ask("cl_exact"));
    out("alive\\t" + st + "\\t" + cl + "\\t" + body.length + "\\t" + hexOf(body.slice(0, 200)));
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
    ok(UPPORT > 0, "fixture: the origin bound a port");
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
                if (f.length < 5 || !f[0]) continue;
                rows[f[0]] = { status: f[1], declared: +f[2], actual: +f[3],
                               body: hexToStr(f[4]) };
            }
        }
    }
});
t.after(() => reap());

const r = (n) => rows[n];
const is5xx = (x) => x && x.status && x.status[0] === "5";
// RFC 9110 8.6: a 204 must not carry Content-Length, so the
// relayed 204 is self-consistent with no declared length and an
// empty (bodyless) actual.
const selfConsistent = (x) => !!x && (x.declared === x.actual ||
    (x.status === "204" && x.declared === -1 && x.actual === 0));

test("fixture: the origin and the App bound ports", (a) => {
    for (const [c, m] of fixtureChecks) a.ok(c, m);
});

test("a Content-Length that matches the delivered body is proxied whole", () => {
    for (const n of ["cl_exact", "cl0", "cl_big", "cl_osp"]) {
        const x = r(n);
        ok(!!x && x.status === "200" && selfConsistent(x),
           n + ": a declared length that matches what arrived is proxied " +
           "[status=" + (x ? x.status : "?") + " declared=" + (x ? x.declared : "?") +
           " actual=" + (x ? x.actual : "?") + "]");
    }
    ok(!!r("cl_exact") && r("cl_exact").body === "UP",
       "cl_exact: the body is exactly the two declared bytes");
    ok(!!r("cl_big") && r("cl_big").declared === 1024 && r("cl_big").actual === 1024,
       "cl_big: a 1 KiB body arrives whole and whole only " +
       "[declared=" + (r("cl_big") ? r("cl_big").declared : "?") +
       " actual=" + (r("cl_big") ? r("cl_big").actual : "?") + "]");
});

test("a delivered length that contradicts the declared one is a 502 with no smuggled response", () => {
    for (const n of ["cl_short", "cl_long", "cl_zero_1", "smuggle"]) {
        const x = r(n);
        ok(is5xx(x),
           n + ": a delivered length that contradicts the declared one is a " +
           "named 502, not a quiet truncation [status=" + (x ? x.status : "?") + "]");
        ok(!!x && x.body.indexOf("HTTP/1.") < 0,
           n + ": no second HTTP response is delivered as body [" +
           (x ? x.body.slice(0, 60) : "?") + "]");
    }
});

test("an unusable Content-Length is a 502; an agreeing duplicate stays legal", () => {
    for (const n of ["cl_plus", "cl_junk", "cl_empty", "cl_huge", "cl_2diff"]) {
        ok(is5xx(r(n)),
           n + ": an unusable Content-Length is a named 502 [status=" +
           (r(n) ? r(n).status : "?") + "]");
    }
    ok(!!r("cl_2eq") && r("cl_2eq").status === "200" && selfConsistent(r("cl_2eq")),
       "cl_2eq: an AGREEING duplicate Content-Length stays legal [status=" +
       (r("cl_2eq") ? r("cl_2eq").status : "?") + "]");
});

test("an upstream Transfer-Encoding is refused, not relayed under a new Content-Length", () => {
    for (const n of ["chunked", "chunked_x", "te_lower", "te_id", "cl_te"]) {
        const x = r(n);
        ok(is5xx(x),
           n + ": an upstream Transfer-Encoding is refused rather than " +
           "relayed under a re-declared Content-Length [status=" +
           (x ? x.status : "?") + "]");
    }
});

test("close-delimited and HTTP/1.0 responses, 204 and 404 are relayed self-consistently", () => {
    for (const n of ["nocl", "nocl_empty", "h10_cl", "h10_nocl", "s204", "s404"]) {
        const x = r(n);
        ok(!!x && x.status !== "" && !is5xx(x) && selfConsistent(x),
           n + ": close-delimited / HTTP/1.0 framing still proxies " +
           "[status=" + (x ? x.status : "?") + " declared=" + (x ? x.declared : "?") +
           " actual=" + (x ? x.actual : "?") + "]");
    }
    ok(!!r("nocl") && r("nocl").body === "body to close",
       "nocl: the close-delimited body arrives whole [" +
       (r("nocl") ? r("nocl").body : "?") + "]");
    ok(!!r("s204") && r("s204").status === "204",
       "s204: a 204 is relayed as a 204, not rewritten");
    ok(!!r("s404") && r("s404").status === "404" && r("s404").body === "UP",
       "s404: an upstream 404 and its body are relayed");
});

test("an upstream that never ends its head is a 502 without a hang", () => {
    ok(is5xx(r("no_head")),
       "no_head: an upstream that never terminates its head is a named 502 " +
       "and the suite does not wait on it [status=" +
       (r("no_head") ? r("no_head").status : "?") + "]");
});

test("near-miss header names are ordinary headers; Content-Length is matched in any case", () => {
    for (const n of ["pfx_len", "pfx_l", "pfx_long", "pfx_x", "pfx_te",
                     "cl_in_value"]) {
        const x = r(n);
        ok(!!x && x.status === "200" && x.declared === 2 && x.actual === 2,
           n + ": a near-miss header name is an ordinary header, not the " +
           "framing one [status=" + (x ? x.status : "?") + " declared=" +
           (x ? x.declared : "?") + " actual=" + (x ? x.actual : "?") + "]");
    }
    for (const n of ["pfx_upper", "pfx_mixed"]) {
        const x = r(n);
        ok(!!x && x.status === "200" && x.declared === 2 && x.actual === 2,
           n + ": the real Content-Length is honoured whatever its case " +
           "[declared=" + (x ? x.declared : "?") + "]");
    }
});

test("the App process survives every framing shape", () => {
    const a = r("alive");
    ok(!!a && a.status === "200" && a.body === "UP",
       "the App PROCESS survived every framing shape: the use-after-free " +
       "on the EOF branch killed it (SIGSEGV) for any close-delimited " +
       "response, and a dead process cannot answer a final request " +
       "[status=" + (a ? a.status : "?") + "]");
});

await t.run();
