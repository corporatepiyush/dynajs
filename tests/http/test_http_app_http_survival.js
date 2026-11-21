// flags: --std
// covers: dyna:http.App
// The App server must survive a battery of malformed HTTP and WebSocket input:
// slots: 2
// peer-proof: prefix attack -- the App closes mid-send on the 2 MB header block and the 2 MB WS message, so how much of either send gets through is timing
// after EVERY attack a well-formed RPC call has to come back with the right
// answer. Each attack is aimed at a different parser limit (length fields,
// header blocks, nesting, frame headers); "survived" is the property, not the
// exact status code, which other suites pin.
import * as os from "os";
import { spawnHelper } from "../lib/helper.js";
import * as std from "std";
import { suite } from "../lib/kit.js";

// The App child and the rawpeer.js attacker both run in the kit's before hook
// (never under --list); each case reads one attack's outcome from the report.
const t = suite("test_http_survival");
let A = null;
const ok = (c, w) => A.ok(c, w);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const T = `${std.getenv("TMPDIR") || "/tmp"}/_dyna_httpsurv.${Date.now()}.${Math.floor(Math.random() * 1e9)}`;
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };
const write = (p, s) => { const f = std.open(p, "w"); f.puts(s); f.close(); };

const ATTACKS = [
    "giant Content-Length, tiny body",
    "negative Content-Length",
    "30-digit Content-Length",
    "header block never terminated, then close",
    "2 MB header block",
    "8 KB request path",
    "NUL bytes in the path",
    "unknown method",
    "500 pipelined requests in one write",
    "slowloris: one byte then idle",
    "truncated JSON-RPC body",
    "JSON nested 10000 deep",
    "__proto__ as the method",
    "static traversal to /etc/passwd",
    "upload with a spoofed type",
    "WS upgrade without a key",
    "WS unmasked client frame",
    "WS 64-bit declared length",
    "WS 2 GiB declared length, 8 bytes sent",
    "WS ping over 125 bytes",
    "WS reserved opcode 0x5",
    "WS 2 MB message"
];
let why = null, srvPid = 0, port = 0, R = [], attErr = null, made = false;
t.before(() => {
    made = true;
    sh(`rm -rf ${T}; mkdir -p ${T}/root ${T}/up`);
    write(`${T}/root/ok.txt`, "hi\n");
    write(`${T}/srv.js`, `import { App } from "dyna:http";
import { Path } from "dyna:file";
import { pid } from "dyna:sys";
import * as std from "std";
const app = new App({ port: 0, idleTimeoutMs: 3000 });
app.rpc("/rpc", { echo: (p) => p, add: (a, b) => a + b });
app.static("/s", new Path("${T}/root"));
app.upload("/u", { dir: new Path("${T}/up"), maxFileSize: 4096 }, () => ({ ok: 1 }));
app.ws("/ws", { open: () => {}, message: (c, d, bin) => c.send(bin ? d : "e:" + d), close: () => {} });
app.start();
const f = std.open("${T}/srv.pid", "w"); f.puts(pid() + " " + app.port); f.close();
`);

    const KIT = JSON.stringify(String(os.realpath("tests/lib/rawpeer.js")[0]));
    write(`${T}/attack.js`, `import { connect, cat, find, latin1 } from ${KIT};
const P = Number(scriptArgs[1]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function raw(payload, wait = 0.25) {
    let c;
    try { c = await connect(P, "127.0.0.1", { connectTimeoutMs: 2000 }); } catch { return new Uint8Array(0); }
    let buf = new Uint8Array(0);
    if (await c.write(payload)) buf = (await c.readAll({ timeoutMs: wait * 1000 })).bytes;
    c.abort();
    return buf;
}
async function alive() {
    const body = '{"jsonrpc":"2.0","method":"add","params":[2,3],"id":1}';
    const req = "POST /rpc HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\nContent-Length: " + body.length + "\\r\\n\\r\\n" + body;
    for (let i = 0; i < 10; i++) {
        if (find(await raw(req, 1.0), '"result":5') >= 0) return true;
        await sleep(100);
    }
    return false;
}
function wsFrame(op, actual, declared, mask = true) {
    const m = mask ? 0x80 : 0;
    let h;
    if (declared < 126) h = new Uint8Array([0x80 | op, m | Number(declared)]);
    else if (declared <= 0xffff) h = new Uint8Array([0x80 | op, m | 126, Number(declared) >> 8, Number(declared) & 255]);
    else { h = new Uint8Array(10); h[0] = 0x80 | op; h[1] = m | 127; new DataView(h.buffer).setBigUint64(2, BigInt(declared)); }
    const key = new Uint8Array([1, 2, 3, 4]);
    const pay = new Uint8Array(actual).fill(0x41);
    return mask ? cat(h, key, pay.map((b, i) => b ^ key[i & 3])) : cat(h, pay);
}
async function wsAttack(frame, wait = 0.3) {
    let c;
    try { c = await connect(P, "127.0.0.1", { connectTimeoutMs: 2000 }); } catch (e) { return "error: " + e.message; }
    const k = btoa(String.fromCharCode(...Array.from({ length: 16 }, () => Math.floor(Math.random() * 256))));
    await c.write("GET /ws HTTP/1.1\\r\\nHost: x\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\nSec-WebSocket-Key: " + k + "\\r\\nSec-WebSocket-Version: 13\\r\\n\\r\\n");
    const r = await c.readUntil("\\r\\n\\r\\n", { timeoutMs: 1000 });
    if (r.timedOut) { c.abort(); return "error: timed out"; }
    if (!r.found || latin1(r.bytes).split("\\r\\n", 1)[0].indexOf(" 101 ") < 0) { c.abort(); return "no-handshake"; }
    await c.write(frame);
    await sleep(wait * 1000);
    c.abort();
    return "sent";
}
const CRLF = "\\r\\n";
const R = [];
async function attack(name, fn, ws = false) {
    const got = await fn();
    R.push({ name, alive: await alive(), ws: ws ? got : null });
}
if (!(await alive())) {
    print(JSON.stringify([{ name: "server not up", alive: false, ws: null }]));
} else {
    await attack("giant Content-Length, tiny body", () => raw("POST /rpc HTTP/1.1\\r\\nContent-Length: 999999999999999999\\r\\n\\r\\n{}"));
    await attack("negative Content-Length", () => raw("POST /rpc HTTP/1.1\\r\\nContent-Length: -1\\r\\n\\r\\n{}"));
    await attack("30-digit Content-Length", () => raw("POST /rpc HTTP/1.1\\r\\nContent-Length: " + "9".repeat(30) + "\\r\\n\\r\\n{}"));
    await attack("header block never terminated, then close", () => raw("GET /rpc HTTP/1.1\\r\\nX: " + "a".repeat(500)));
    await attack("2 MB header block", () => raw("GET /rpc HTTP/1.1\\r\\n" + ("X: " + "a".repeat(100) + CRLF).repeat(20000) + CRLF, 1.0));
    await attack("8 KB request path", () => raw("GET /" + "a".repeat(8192) + " HTTP/1.1\\r\\n\\r\\n"));
    await attack("NUL bytes in the path", () => raw("GET /rpc\\x00\\x00 HTTP/1.1\\r\\n\\r\\n"));
    await attack("unknown method", () => raw("FOOBAR /rpc HTTP/1.1\\r\\n\\r\\n"));
    await attack("500 pipelined requests in one write", () => raw("GET /rpc HTTP/1.1\\r\\n\\r\\n".repeat(500), 1.0));
    await attack("slowloris: one byte then idle", () => raw("G", 0.15));
    await attack("truncated JSON-RPC body", () => raw('POST /rpc HTTP/1.1\\r\\nContent-Length: 10\\r\\n\\r\\n{"jsonrpc'));
    await attack("JSON nested 10000 deep", () => raw("POST /rpc HTTP/1.1\\r\\nContent-Length: 20000\\r\\n\\r\\n" + "[".repeat(10000) + "]".repeat(10000), 1.0));
    await attack("__proto__ as the method", () => raw('POST /rpc HTTP/1.1\\r\\nContent-Length: 45\\r\\n\\r\\n{"jsonrpc":"2.0","method":"__proto__","id":1}'));
    await attack("static traversal to /etc/passwd", () => raw("GET /s/../../../../etc/passwd HTTP/1.1\\r\\n\\r\\n"));
    await attack("upload with a spoofed type", () => raw("POST /u HTTP/1.1\\r\\nContent-Type: image/png\\r\\nContent-Length: 3\\r\\n\\r\\nabc"));
    await attack("WS upgrade without a key", () => raw("GET /ws HTTP/1.1\\r\\nUpgrade: websocket\\r\\nConnection: Upgrade\\r\\n\\r\\n"));
    await attack("WS unmasked client frame", () => wsAttack(wsFrame(1, 5, 5, false)), true);
    await attack("WS 64-bit declared length", () => wsAttack(wsFrame(2, 8, 0xFFFFFFFFFFFFFF00n)), true);
    await attack("WS 2 GiB declared length, 8 bytes sent", () => wsAttack(wsFrame(2, 8, 0x7FFFFFFF)), true);
    await attack("WS ping over 125 bytes", () => wsAttack(wsFrame(9, 200, 200)), true);
    await attack("WS reserved opcode 0x5", () => wsAttack(wsFrame(5, 4, 4)), true);
    await attack("WS 2 MB message", () => wsAttack(wsFrame(1, 2 << 20, 2 << 20), 0.8), true);
    print(JSON.stringify(R));
}
`);

    const exe = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
    spawnHelper(`${exe} --std ${T}/srv.js >${T}/srv.log 2>&1`);
    for (let i = 0; i < 200 && !cat(`${T}/srv.pid`).trim(); i++) os.sleep(25);
    [srvPid, port] = cat(`${T}/srv.pid`).trim().split(" ").map((x) => parseInt(x, 10));
    if (port > 0) {
        sh(`${exe} --std ${T}/attack.js ${port} > ${T}/att.json 2>${T}/att.err`);
        try { R = JSON.parse(cat(`${T}/att.json`)); }
        catch (e) { attErr = true; }
    }
});
t.after(() => {
    if (srvPid > 0) sh(`kill ${srvPid} 2>/dev/null`);
    if (made) sh(`rm -rf ${T}`);
});
const test2 = (n, fn) => test(n, (a) => { if (why) a.skip(why); return fn(a); });

test2("the server starts, reports its port and the attacker reports every attack", () => {
    ok(srvPid > 0 && port > 0, "server started and reported its port (log: " + cat(`${T}/srv.log`).slice(0, 160) + ")");
    if (attErr) ok(false, "attacker produced JSON (stderr: " + cat(`${T}/att.err`).slice(0, 200) + ")");
    ok(R.length === 22, "all 22 attacks ran (got " + R.length + ")");
});

for (const name of ATTACKS)
    test2("the server survives: " + name, () => {
        const r = R.find((x) => x.name === name);
        if (!r) { ok(false, "the attacker reported " + name); return; }
        ok(r.alive, "server answers a valid RPC after: " + r.name);
        if (r.ws !== null)
            ok(r.ws === "sent", "the WebSocket handshake completed, so the frame really reached the parser: " + r.name + " (" + r.ws + ")");
    });

test2("the server process is still running at the end", () => {
    ok(sh(`kill -0 ${srvPid} 2>/dev/null`) === 0, "server process is still running at the end");
});

await t.run();
