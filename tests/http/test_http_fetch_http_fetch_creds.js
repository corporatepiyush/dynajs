// flags: --std
// timeout: 180
// covers: dyna:http.fetch
// slots: 2
// peer-proof: proven 2026-11-28 by tools/peer-proof.js --base-file against the HEAD Python origins under this test's polling waits
// (the HEAD test slept 200 ms per case, which let each origin close before fetch reused the connection; the polling wait does not).
// The variant changes only the waits: fetch retries a GET its reused connection lost before any response byte (RFC 9112 9.3.1),
// pinned by the fetch cases of tests/http/test_http_client_pooled_keepalive.js.
// peer-proof: ports ports.txt -- origins.js publishes three ports on one line (A B C); each gets its own recording proxy, which maps the origins' ports in their 302s to the proxies'
import { fetch } from "dyna:http";
import { spawnHelper } from "../lib/helper.js";
import * as std from "std";
import * as os from "os";
import { suite } from "../lib/kit.js";

// The three rawpeer.js origins start in the kit's before hook (never under
// --list); each case makes its own fetch and reads the hops it caused.
const t = suite("test_http_fetch_creds");
let A = null;
const ok = (c, m) => A.ok(c, m);
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };
const EXE = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";

let why = null, T = null, reap = null, PORTS = [], PA = 0, PB = 0, PC = 0;
t.before(() => {
    T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_http_creds.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
    sh(`rm -rf ${T}; mkdir -p ${T}`);

    const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
    write(`${T}/origins.js`, `import { listen, latin1 } from ${KIT};
import * as file from "dyna:file";
const NL = "\\r\\n";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const PORTS = {}, LOG = [];
function flush() {
    const out = [];
    for (const [name, port, path, head] of LOG) {
        out.push("--- " + name + " port=" + port + " path=" + path);
        for (const ln of head.split(NL).slice(1)) if (ln.trim()) out.push("    " + ln);
        out.push("");
    }
    file.writeFile(scriptArgs[1], out.join("\\n") + "\\n");
}
function bind(name, redirect) {
    const srv = listen(async (c) => {
        const r = await c.readUntil(NL + NL, { timeoutMs: 5000 });
        if (!r.found) { c.close(); return; }
        const head = latin1(r.bytes.subarray(0, r.bytes.length - 4));
        const req = head.split(NL)[0];
        const path = req.includes(" ") ? req.split(" ")[1].split("?")[0] : "/";
        LOG.push([name, PORTS[name], path, head]);
        flush();
        if (redirect && path.startsWith("/redir")) await c.write("HTTP/1.1 302 Found" + NL + "Location: " + redirect(PORTS[name]) + NL + "Content-Length: 0" + NL + "Connection: close" + NL + NL);
        else await c.write("HTTP/1.1 200 OK" + NL + "Content-Length: 2" + NL + NL + "hi");
        await sleep(50);
        c.close();
    });
    PORTS[name] = srv.port;
}
bind("C", (p) => "http://127.0.0.1:" + p + "/final");
bind("B", null);
bind("A", () => "http://127.0.0.1:" + PORTS.B + "/final");
file.writeFile(scriptArgs[2], PORTS.A + " " + PORTS.B + " " + PORTS.C, { atomic: true });
`);

    const startBg = (cmd, tag) => {
        sh(`rm -f ${T}/${tag}.pid`); spawnHelper(`${cmd} >${T}/${tag}.log 2>&1`, { pidFile: `${T}/${tag}.pid` });
    };
    const waitFile = (path) => {
        for (let i = 0; i < 300; i++) {
            if (cat(path).trim() !== "") return true;
            sh("sleep 0.05");
        }
        return false;
    };
    reap = () => sh(`for f in ${T}/*.pid; do [ -f "$f" ] || continue; ` +
        `p=$(cat "$f" 2>/dev/null); case "$p" in ''|*[!0-9]*) continue;; esac; ` +
        `case "$(ps -p $p -o command= 2>/dev/null)" in *"${T}/"*) ` +
        `kill $p 2>/dev/null;; esac; done; rm -rf ${T}`);

    startBg(`${EXE} --std ${T}/origins.js ${T}/log.txt ${T}/ports.txt`, "origins");
    if (!waitFile(`${T}/ports.txt`)) {
        print("  fixture log: " + cat(`${T}/origins.log`).slice(0, 400));
        reap();
        throw new Error("test_http_fetch_creds: origins never bound");
    }
    PORTS = cat(`${T}/ports.txt`).trim().split(/\s+/).map(Number);
    [PA, PB, PC] = PORTS;
});
t.after(() => { if (reap) reap(); });
const test = (n, fn) => t.test(n, (a) => { A = a; if (why) a.skip(why); return fn(a); });

test("fixture: three distinct origin ports", () => {
    ok(PA > 0 && PB > 0 && PC > 0 && PA !== PB && PB !== PC && PA !== PC,
       "fixture: three DISTINCT ports, so each hop's origin change is real [" +
       PORTS.join(",") + "]");
});

    const H = {
        "authorization": "Basic Q1JFVFRPUkE=",
        "cookie": "sid=SECRET",
        "cookie2": "$Version=1",
        "proxy-authorization": "Basic UFJPWktQ1JFVA==",
        "x-keep": "kept",
    };
    const CREDS = ["authorization", "cookie", "cookie2", "proxy-authorization"];

    const hops = () => {
        const out = [];
        for (const block of cat(`${T}/log.txt`).split(/^--- /m)) {
            if (block.trim() === "") continue;
            const m = /^(\S+) port=(\d+) path=(\S*)/.exec(block);
            if (!m) continue;
            const hdrs = {};
            for (const line of block.split("\n").slice(1)) {
                const c = line.indexOf(":");
                if (c > 0) hdrs[line.slice(0, c).trim().toLowerCase()] = line.slice(c + 1).trim();
            }
            out.push({ name: m[1], port: parseInt(m[2], 10), path: m[3], hdrs });
        }
        return out;
    };
    const hdrAt = (h, name) => {
        const want = name.toLowerCase();
        for (const k of Object.keys(h.hdrs)) if (k === want) return h.hdrs[k];
        return undefined;
    };

    const mark = () => hops().length;
    const settle = async (m, n) => { for (let i = 0; i < 150 && hops().length - m < n; i++) await new Promise((res) => setTimeout(res, 20)); };
    const since = (m) => hops().slice(m);

test("a cross-origin redirect drops every credential header and keeps the rest", async () => {
    const mCross = mark();
    try {
        const r = await fetch(`http://127.0.0.1:${PA}/redir`, { headers: H });
        ok(r.status === 200, "cross-origin: the redirect chain completes [" + r.status + "]");
    } catch (e) {
        ok(false, "cross-origin: fetch threw [" + String(e && e.message || e) + "]");
    }
    await settle(mCross, 2);
    {
        const hs = since(mCross);
        ok(hs.length === 2,
           "cross-origin: both hops were seen by the servers [" + hs.length + "]");
        const first = hs[0], second = hs[1];
        ok(first && first.name === "A" && first.path === "/redir",
           "cross-origin: hop 0 is origin A /redir [" + JSON.stringify(first && first.path) + "]");
        ok(second && second.name === "B" && second.path === "/final",
           "cross-origin: hop 1 is origin B /final -- a different origin [" +
           JSON.stringify(second && second.name) + "]");
        for (const c of CREDS)
            ok(first && hdrAt(first, c) === H[c],
               "cross-origin: hop 0 actually sent " + c + " [" +
               (first ? String(hdrAt(first, c)) : "?") + "]");
        for (const c of CREDS)
            ok(second && hdrAt(second, c) === undefined,
               "M02: origin B never sees " + c + " -- got " +
               (second ? String(hdrAt(second, c)) : "?"));
        ok(second && hdrAt(second, "x-keep") === "kept",
           "cross-origin: a NON-credential header still survives [" +
           (second ? String(hdrAt(second, "x-keep")) : "?") + "]");
    }

});

test("a same-origin redirect preserves the credential headers", async () => {
    const mSame = mark();
    try {
        const r = await fetch(`http://127.0.0.1:${PC}/redir`, { headers: H });
        ok(r.status === 200, "same-origin: the redirect chain completes [" + r.status + "]");
    } catch (e) {
        ok(false, "same-origin: fetch threw [" + String(e && e.message || e) + "]");
    }
    await settle(mSame, 2);
    {
        const hs = since(mSame);
        ok(hs.length === 2, "same-origin: both hops were seen [" + hs.length + "]");
        const second = hs[1];
        ok(hs[0] && hs[1] && hs[0].port === hs[1].port,
           "same-origin: both hops are on the SAME port [" +
           (hs[0] ? hs[0].port : "?") + "/" + (hs[1] ? hs[1].port : "?") + "]");
        for (const c of CREDS)
            ok(second && hdrAt(second, c) === H[c],
               "M02: same-origin PRESERVES " + c + " -- got " +
               (second ? String(hdrAt(second, c)) : "?"));
    }

});

test("a direct request sends the credential headers", async () => {
    const mDirect = mark();
    try {
        const r = await fetch(`http://127.0.0.1:${PB}/final`, { headers: H });
        ok(r.status === 200, "direct: a plain request completes [" + r.status + "]");
    } catch (e) {
        ok(false, "direct: fetch threw [" + String(e && e.message || e) + "]");
    }
    await settle(mDirect, 1);
    {
        const hs = since(mDirect);
        const only = hs[0];
        ok(hs.length === 1, "direct: exactly one hop was seen [" + hs.length + "]");
        for (const c of CREDS)
            ok(only && hdrAt(only, c) === H[c],
               "direct: a non-redirect request still sends " + c + " -- got " +
               (only ? String(hdrAt(only, c)) : "?"));
    }

});

await t.run();
