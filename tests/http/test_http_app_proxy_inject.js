// flags: --std
// timeout: 300
// covers: dyna:http.App
// slots: 2
// peer-proof: drop drive out:^GET /api/ -- the driver's client side connects to the App, whose port is announced in a file written after launch, so the recording proxy never sits in front of it; only the dynajs run records those requests
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against HEAD with drive.py taking the upstream port file as its second argument, so the recording proxy sits in front of the hostile origin
import * as std from "std";
import { spawnHelper } from "../lib/helper.js";
import * as os from "os";

import { suite } from "../lib/kit.js";

const t = suite("test_http_proxy_inject");
let A = null, why = null, T = null, reap = () => {}, rows = {}, UPPORT = 0, P = 0;
const ok = (c, m) => A.ok(c, m);
const fixtureChecks = [];
const test = (n, fn, o) => t.test(n, (a) => { A = a; if (why) a.skip(why); return fn(a); }, o);
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const hexToStr = (h) => {
    let s = "";
    for (let i = 0; i + 1 < h.length; i += 2)
        s += String.fromCharCode(parseInt(h.substr(i, 2), 16));
    return s;
};
const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

t.before(() => {
    if (why) return;
    A = { ok: (c, m) => { fixtureChecks.push([c, m]); return c; } };
    T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_proxy_inj.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
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
const B = (...c) => String.fromCharCode(...c);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const out = (line) => { std.out.puts(line + "\\n"); std.out.flush(); };
const head = (body, extra = "") => "HTTP/1.1 200 OK" + CRLF + "Content-Length: 2" + CRLF + extra + CRLF + body;
const CASES = {};
for (const c of [...Array(0x20).keys(), 0x7f]) CASES["c0_" + c.toString(16).padStart(2, "0")] = head("UP", "X-Evil: a" + B(c) + "b" + CRLF);
CASES.crlf_pair = head("UP", "X-Evil: a" + CRLF + "X-Second: b" + CRLF);
CASES.dblcrlf = head("UP", "X-Evil: a" + CRLF);
CASES.c1_80 = head("UP", "X-Evil: a" + B(0xc2, 0x80) + "b" + CRLF);
CASES.c1_85 = head("UP", "X-Evil: a" + B(0xc2, 0x85) + "b" + CRLF);
CASES.c1_9f = head("UP", "X-Evil: a" + B(0xc2, 0x9f) + "b" + CRLF);
CASES.u2028 = head("UP", "X-Evil: a" + B(0xe2, 0x80, 0xa8) + "b" + CRLF);
CASES.overlong = head("UP", "X-Evil: a" + B(0xc0, 0xaf) + "b" + CRLF);
CASES.surrogate = head("UP", "X-Evil: a" + B(0xed, 0xa0, 0x80) + "b" + CRLF);
CASES.bare_e9 = head("UP", "X-Evil: caf" + B(0xe9) + "X" + CRLF);
for (const n of [255, 256, 257, 500, 4000]) CASES["len" + n] = head("UP", "X-Evil: " + "a".repeat(n) + CRLF);
CASES.st_barelf = "HTTP/1.1 200 O" + LF + "K" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP";
CASES.st_barecr = "HTTP/1.1 200 O" + CR + "K" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP";
CASES.st_ctl = "HTTP/1.1 200 O" + B(1) + "K" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP";
CASES.st_4digit = "HTTP/1.1 2000 W" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP";
CASES.st_nospace = "HTTP/1.1200 W" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP";
CASES.st_ok = "HTTP/1.1 404 Not Found" + CRLF + "Content-Length: 2" + CRLF + CRLF + "UP";
CASES.name_ctl = head("UP", "X-" + B(1) + "Evil: a" + CRLF);
CASES.name_sp = head("UP", "X Evil: a" + CRLF);
CASES.name_nl = head("UP", "X" + LF + "Evil: a" + CRLF);
CASES.name_high = head("UP", "X" + B(0xe9) + "vil: a" + CRLF);
CASES.name_fold = head("UP", "X-A: a" + CRLF + "\\tX-B: b" + CRLF);
CASES.fold_nocolon = head("UP", "X-A: a" + CRLF + "\\tcont" + CRLF);
CASES.hop = head("UP", "Connection: keep-alive" + CRLF + "Keep-Alive: timeout=5" + CRLF + "Proxy-Authenticate: Basic" + CRLF + "X-Keep: kept" + CRLF);
CASES.hop_te = head("UP", "Transfer-Encoding: identity" + CRLF);
CASES.nocolon = head("UP", "X-Evil-a" + CRLF);
CASES.legal = head("UP", "X-Evil: normal-value" + CRLF);
CASES.legal_tok = head("UP", "X_Evil.1: a:b/c!d$e&f'g*h+i-j%k^l" + B(96) + "m|n~o" + CRLF);
CASES.legal_empty = head("UP", "X-Evil:" + CRLF);
CASES.legal_obs = head("UP", "X-Evil: caf" + B(0xe9) + CRLF);
CASES.legal_url = head("UP", "Location: http://h:1/p?q=1&r=2" + CRLF);
CASES.real_cookie = head("UP", "X-Evil: a" + CRLF + "Set-Cookie: real=1" + CRLF);

listen(async (c) => {
    const h = await c.readUntil(CRLF + CRLF, { timeoutMs: 4000 });
    const parts = h.timedOut ? [] : latin1(h.bytes).split(" ");
    if (parts.length < 2) { c.close(); return; }
    const p = parts[1].replace(/^\\/+/, "");
    await c.write(Object.hasOwn(CASES, p) ? CASES[p] : CASES.legal);
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
const hexOf = (s) => { let h = ""; for (let i = 0; i < s.length; i++) h += (s.charCodeAt(i) & 255).toString(16).padStart(2, "0"); return h; };
const utf8 = (s) => { let r = ""; for (const ch of s) { const c = ch.codePointAt(0); r += c < 0x80 ? ch : c < 0x800 ? String.fromCharCode(0xc0 | (c >> 6), 0x80 | (c & 63)) : String.fromCharCode(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63)); } return r; };

async function ask(path) {
    const s = await connect(PROXY, "127.0.0.1", { connectTimeoutMs: 5000 });
    await s.write("GET /api/" + path + " HTTP/1.1" + CRLF + "Host: victim" + CRLF + "Content-Length: 0" + CRLF + CRLF);
    let acc = "";
    const h = await s.readUntil(CRLF + CRLF, { timeoutMs: 1500 });
    acc = latin1(h.bytes);
    if (h.timedOut) acc += "<<TIMEOUT>>";
    else if (h.found) {
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
function facts(buf) {
    const i = buf.indexOf(CRLF + CRLF);
    if (i < 0) return ["", 99, 0, [], ""];
    const lines = buf.slice(0, i).split(CRLF), body = buf.slice(i + 4);
    let bare = 0;
    for (const ln of lines) for (const ch of ln) if (ch === CR || ch === LF) bare++;
    const names = lines.slice(1).map((ln) => ln.includes(":") ? asciiLower(pyStrip(ln.split(":")[0])) : "<none>");
    const parts = lines[0].split(" ");
    return [parts.length > 1 ? parts[1] : "", bare, names.length, names, body];
}
for (const name of Object.keys(CASES).sort()) {
    let f;
    try { f = facts(await ask(name)); }
    catch (e) { out(name + "\\tERR\\t1\\t0\\t-\\t" + hexOf(utf8(String(e.message)))); continue; }
    const [st, bare, nn, names, body] = f;
    out(name + "\\t" + utf8(st) + "\\t" + bare + "\\t" + nn + "\\t" + hexOf(utf8(names.join(","))) + "\\t" + hexOf(body.slice(0, 400)));
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
    ok(UPPORT > 0, "fixture: the hostile origin bound a port");
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
            for (let i = 0; i < 600; i++) {
                if ((cat(`${T}/drv.log`) || "").indexOf("DONE") >= 0 ||
                    (cat(`${T}/drv.log`) || "").indexOf("NOPROXY") >= 0) break;
                sh("sleep 0.05");
            }
            const out = cat(`${T}/drv.log`);
            rows = {};
            for (const line of out.split("\n")) {
                const f = line.split("\t");
                if (f[0] === "DONE") continue;
                if (f.length < 6 || !f[0]) continue;
                rows[f[0]] = {
                    status: f[1],
                    bare: +f[2],
                    nlines: +f[3],
                    names: f[4] === "-" ? [] : hexToStr(f[4]).split(","),
                    body: hexToStr(f[5]),
                };
            }
        }
    }
});
t.after(() => reap());

let r = null;
const is5xx = (r) => r && r.status && r.status[0] === "5";
const cookieLines = (r) => (r ? r.names.filter((n) => n === "set-cookie").length : 0);

test("fixture: the origin and the App bound ports", (a) => {
    for (const [c, m] of fixtureChecks) a.ok(c, m);
});

test("a C0 byte or DEL in an upstream header value is a 5xx, never a split head", () => {
    for (let c = 0x00; c <= 0x1f; c++) {
        const n = "c0_" + (c < 16 ? "0" : "") + c.toString(16);
        const r = rows[n];
        ok(!!r, "value 0x" + c.toString(16) + ": the case ran");
        if (!r) continue;
        ok(cookieLines(r) === 0, "value 0x" + c.toString(16).padStart(2, "0") +
           ": no Set-Cookie line reaches the client [" + r.names.join(",") + "]");
        ok(r.bare === 0, "value 0x" + c.toString(16).padStart(2, "0") +
           ": the head has no bare CR or LF in a line");
        ok(is5xx(r), "value 0x" + c.toString(16).padStart(2, "0") +
           ": refused with a named 5xx, not a repaired head [status=" + r.status + "]");
    }
    r = rows["c0_7f"];
    ok(!!r && cookieLines(r) === 0 && r.bare === 0 && is5xx(r),
       "value DEL: refused, no split, named 5xx [" + (r ? r.status + " " + r.names.join(",") : "missing") + "]");
});

test("a real CRLF pair is two headers and a double CRLF ends the head", () => {
    r = rows["crlf_pair"];
    ok(!!r && r.status === "200" && r.bare === 0 && r.nlines === 4,
       "CRLF pair: the upstream sent two headers and BOTH are relayed " +
       "[status=" + (r ? r.status : "?") + " lines=" + (r ? r.names.join(",") : "?") + "]");
    r = rows["dblcrlf"];
    ok(!!r && r.status === "200" && r.nlines === 3 && r.body === "UP",
       "double CRLF: the head ends where the upstream's ended, and the " +
       "rest is body, not a header [" +
       (r ? r.status + " " + r.names.join(",") + " body=" + r.body : "?") + "]");
});

test("high bytes in a value stay on one line", () => {
    for (const n of ["c1_80", "c1_85", "c1_9f", "u2028", "overlong",
                     "surrogate", "bare_e9", "legal_obs"]) {
        r = rows[n];
        ok(!!r && r.status === "200" && r.bare === 0 && cookieLines(r) === 0 &&
           r.names.indexOf("x-evil") >= 0,
           n + ": a high byte in a value is one line, not a split " +
           "[status=" + (r ? r.status : "?") + " lines=" + (r ? r.names.join(",") : "?") + "]");
    }
});

test("long header values are relayed, not refused for length", () => {
    for (const n of ["len255", "len256", "len257", "len500", "len4000"]) {
        r = rows[n];
        ok(!!r && r.status === "200" && r.names.indexOf("x-evil") >= 0,
           n + ": a long value is relayed, not refused for its length " +
           "[status=" + (r ? r.status : "?") + "]");
    }
});

test("a malformed upstream status line is refused; a legal one is relayed", () => {
    for (const n of ["st_barelf", "st_barecr", "st_ctl", "st_4digit", "st_nospace"]) {
        r = rows[n];
        ok(!!r && is5xx(r) && r.bare === 0 && cookieLines(r) === 0,
           n + ": a malformed status line is refused, not relayed " +
           "[status=" + (r ? r.status : "?") + "]");
    }
    r = rows["st_ok"];
    ok(!!r && r.status === "404" && r.bare === 0,
       "status line control: 404 and its reason phrase are relayed");
});

test("a header name that is not a token is refused", () => {
    for (const n of ["name_ctl", "name_sp", "name_nl", "name_high", "name_fold"]) {
        r = rows[n];
        ok(!!r && is5xx(r) && r.bare === 0 && cookieLines(r) === 0,
           n + ": a name that is not a token is refused " +
           "[status=" + (r ? r.status : "?") + " lines=" + (r ? r.names.join(",") : "?") + "]");
    }
});

test("ordinary responses pass; hop-by-hop headers and colon-less lines are dropped; real cookies are relayed", () => {
    for (const n of ["legal", "legal_tok", "legal_empty", "legal_url"]) {
        r = rows[n];
        ok(!!r && r.status === "200" && r.bare === 0,
           n + ": an ordinary upstream response is proxied unchanged " +
           "[status=" + (r ? r.status : "?") + "]");
    }
    r = rows["hop"];
    ok(!!r && r.status === "200" && r.nlines === 3 &&
       r.names.join(",") === "x-keep,content-length,connection",
       "hop-by-hop: the ordinary header survives, the upstream's hop-by-hop " +
       "headers do not [" + (r ? r.status + " " + r.names.join(",") : "?") + "]");
    r = rows["hop_te"];
    ok(!!r && is5xx(r),
       "an upstream Transfer-Encoding (even `identity`) is refused, not " +
       "relayed under a re-declared Content-Length [" + (r ? r.status : "?") + "]");
    r = rows["fold_nocolon"];
    ok(!!r && r.status === "200" && r.names.indexOf("<none>") < 0 &&
       r.names.indexOf("x-a") >= 0,
       "an obs-fold line with no colon is dropped, never emitted as one " +
       "[" + (r ? r.status + " " + r.names.join(",") : "?") + "]");
    r = rows["nocolon"];
    ok(!!r && r.status === "200" && r.names.indexOf("<none>") < 0,
       "a header line with no colon is dropped, never emitted as one " +
       "[" + (r ? r.names.join(",") : "?") + "]");
    r = rows["real_cookie"];
    ok(!!r && r.status === "200" && cookieLines(r) === 1,
       "a GENUINE upstream Set-Cookie is relayed: at the HTTP layer the " +
       "upstream sent a header, and a proxy that dropped it would be " +
       "broken, not hardened [" + (r ? r.status + " " + r.names.join(",") : "?") + "]");
});

test("the App survives every hostile response head", () => {
    r = rows["legal"];
    ok(!!r && r.status === "200",
       "the App survived every hostile response head (a dead App reads as " +
       "no response at all)");
});

await t.run();
