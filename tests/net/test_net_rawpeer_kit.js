// covers: integration dyna:net.TCPServer -- proves the wire-peer test libraries tests/lib/rawpeer.js and tests/lib/tcprec.js (no d.ts symbol of their own) over raw dyna:net TCP
// slots: 2
// timeout: 120
// The NO-PYTHON wire-peer kit: a rawpeer.js echo origin in its own process, tests/lib/tcprec.js in front of it, and this
// process as the counterpart. The origin's own RAWPEER_REC transcript and the proxy's transcript must be byte-identical
// once sorted (the equivalence tools/peer-proof.js relies on to compare a Python peer seen through the proxy with its
// dynajs replacement recording itself). Also: readUntil across chunk splits, max, timeout and EOF results; 256-byte,
// 1 MiB and 10 MiB binary payloads; readUntil includes the needle; a counterpart FIN ends the connection (reply before it).
// S0b: the recorders hex()ed through a JS string loop, seconds per 6 MiB under load (a native toHex now).
// Under run2 the 1 MiB relay came back short (801828 bytes): when the origin closed, tcprec end()ed the client side
// while bytes were still queued, end() threw, and the close() fallback dropped the queue. The 10 MiB case pauses its reader
// so that queue is always full when the origin closes (and passes the 4 MiB per-connection cap), making the loss deterministic.
import * as file from "dyna:file";
import * as sys from "dyna:sys";
import { TCPServer } from "dyna:net";
import { suite } from "../lib/kit.js";
import { spawnHelper } from "../lib/helper.js";
import { connect, readPort, enc, latin1, find, cat, hex } from "../lib/rawpeer.js";

const ROOT = String(sys.Exec("/bin/pwd", []).stdout).trim();
const T = String(file.makeTempDir());
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const pattern = (n) => new Uint8Array(n).map((_, i) => (i * 7 + (i >> 16)) & 255);
const same = (x, y) => x.length === y.length && hex(x.subarray(0, 64)) === hex(y.subarray(0, 64)) && hex(x.subarray(-64)) === hex(y.subarray(-64)) && hex(x.subarray(x.length >> 1, (x.length >> 1) + 64)) === hex(y.subarray(y.length >> 1, (y.length >> 1) + 64));
let h1 = null, h2 = null, port = 0;

const ECHO = `import { listen, latin1 } from ${JSON.stringify(ROOT + "/tests/lib/rawpeer.js")};
listen(async (c) => {
    const head = await c.readUntil("\\n", { timeoutMs: 5000, max: 4 << 20 });
    const mode = latin1(head.bytes);
    if (mode === "big\\n") { const r = await c.readUntil("END", { timeoutMs: 20000 }); await c.write(r.found ? r.bytes.subarray(0, r.bytes.length - 3) : "not-found"); c.close(); return; }
    if (mode === "nowait\\n") { c.write(new Uint8Array(2 << 20).fill(0x5a)); c.close(); return; }
    if (mode === "fin\\n") { const r = await c.readAll({ timeoutMs: 5000 }); await c.write("after-fin:" + r.bytes.length + ":" + r.eof); c.close(); return; }
    if (mode === "silent\\n") { const r = await c.readAll({ timeoutMs: 300 }); await c.write("timed:" + r.timedOut + ":" + r.eof); c.close(); return; }
    await c.write("echo:" + mode);
    c.close();
}, { portFile: scriptArgs[1] });
`;

async function ask(bytes, { fin = false, pauseMs = 0 } = {}) {
    const c = await connect(port);
    if (pauseMs) c.conn.pause();
    for (const b of bytes) { await c.write(b); await sleep(5); }
    if (fin) c.end();
    if (pauseMs) { await sleep(pauseMs); c.conn.resume(); }
    const r = await c.readAll({ timeoutMs: 20000 });
    c.close();
    return r;
}

const t = suite("net_rawpeer_kit");
t.before(async () => {
    file.writeFile(T + "/echo.js", ECHO);
    h1 = spawnHelper(`RAWPEER_REC=${T} ${ROOT}/dynajs --std ${T}/echo.js ${T}/echo.port.up > ${T}/echo.log 2>&1`);
    h2 = spawnHelper(`${ROOT}/dynajs --std ${ROOT}/tests/lib/tcprec.js ${T}/echo.port.up ${T}/echo.port ${T}/proxy.rec > ${T}/rec.log 2>&1`);
    port = await readPort(T + "/echo.port", { timeoutMs: 60000 });
    if (!port) {
        const peek = (f) => { try { return JSON.stringify(file.readFile(T + "/" + f).slice(-400)); } catch (e) { return "(" + e.message + ")"; } };
        throw new Error("the proxy never published its port; echo.port.up=" + peek("echo.port.up") + " echo.log=" + peek("echo.log") + " rec.log=" + peek("rec.log"));
    }
});
t.after(() => {
    if (h1) h1.stop();
    if (h2) h2.stop();
    file.removeAll(T);
});

t.test("enc, latin1, find, cat and hex edges", (a) => {
    a.ok(latin1(enc("\x00\xff\r\nA")) === "\x00\xff\r\nA" && enc("Ł")[0] === 0x41, "enc/latin1 round-trip bytes and keep the low byte of wider chars");
    a.ok(find(enc("abcabd"), "abd") === 3 && find(enc("ab"), "abc") === -1 && find(enc("aaa"), "", 1) === 1, "find: match, needle longer than hay, empty needle");
    a.eq(hex(cat("a", new Uint8Array([0, 255]), "")), "6100ff", "cat joins strings and bytes; hex is lowercase two-digit");
});
t.test("readPort waits for a whole port number in 1..65535", async (a) => {
    const p = T + "/half.port";
    file.writeFile(p, "");
    setTimeout(() => file.writeFile(p, "12abc"), 60);
    setTimeout(() => file.writeFile(p, "70000"), 120);
    setTimeout(() => file.writeFile(p, "54321\n"), 180);
    a.eq(await readPort(p, { timeoutMs: 5000 }), 54321, "empty, non-numeric and out-of-range contents are retried, not returned");
    a.eq(await readPort(T + "/missing.port", { timeoutMs: 50 }), 0, "a file that never appears resolves 0");
}, { timeoutMs: 10000 });
t.test("the proxy publishes its port behind the origin's", (a) => {
    a.ok(port > 0, "proxy port " + port);
});
t.test("a request split across writes reassembles at readUntil", async (a) => {
    const r = await ask(["hel", "lo\n"]);
    a.ok(latin1(r.bytes) === "echo:hello\n" && r.eof, "reply " + JSON.stringify(latin1(r.bytes)) + " eof " + r.eof);
}, { timeoutMs: 30000 });
t.test("a needle split across chunks is found", async (a) => {
    const r = await ask(["big\nab", "EN", "D"]);
    a.eq(latin1(r.bytes), "ab");
}, { timeoutMs: 30000 });
t.test("all 256 byte values pass both ways", async (a) => {
    const bin = new Uint8Array(256).map((_, i) => i);
    const r = await ask([cat("big\n", bin, "END")]);
    a.eq(hex(r.bytes), hex(bin));
}, { timeoutMs: 30000 });
t.test("a 1 MiB payload is relayed whole", async (a) => {
    const mb = pattern(1 << 20);
    const r = await ask([cat("big\n", mb, "END")]);
    a.ok(same(r.bytes, mb), "relayed " + r.bytes.length + " of " + mb.length);
}, { timeoutMs: 30000 });
t.test("10 MiB to a paused reader survive the origin's close and the 4 MiB queue cap", async (a) => {
    const big = pattern(10 << 20);
    const r = await ask([cat("big\n", big, "END")], { pauseMs: 800 });
    a.ok(same(r.bytes, big) && r.eof, "relayed " + r.bytes.length + " of " + big.length + " eof " + r.eof);
}, { timeoutMs: 60000 });
t.test("a write not awaited before close() still arrives whole", async (a) => {
    const r = await ask(["nowait\n"], { pauseMs: 500 });
    a.ok(r.bytes.length === 2 << 20 && r.bytes.every((x) => x === 0x5a) && r.eof, "got " + r.bytes.length + " of " + (2 << 20) + " eof " + r.eof);
}, { timeoutMs: 30000 });
t.test("end() sends FIN; in event mode the FIN closes the connection, so a peer replies before it", async (a) => {
    const r = await ask(["fin\n", "xyz"], { fin: true });
    a.ok(r.bytes.length === 0 && r.eof, "reply " + JSON.stringify(latin1(r.bytes)) + " eof " + r.eof);
}, { timeoutMs: 30000 });
t.test("readAll with a timeout resolves timedOut instead of throwing", async (a) => {
    const r = await ask(["silent\n"]);
    a.eq(latin1(r.bytes), "timed:true:false");
}, { timeoutMs: 30000 });
t.test("a plain dyna:net client sees the same relay", async (a) => {
    const got = await new Promise((res) => {
        const parts = [];
        TCPServer.connect({ host: "127.0.0.1", port, connectTimeoutMs: 5000 }, {
            connect(c) { if (!c) { res(null); return; } c.write("raw\n"); },
            data(c, x) { parts.push(x); },
            close() { res(cat(...parts)); },
        });
    });
    a.eq(got === null ? "connect failed" : latin1(got), "echo:raw\n");
}, { timeoutMs: 30000 });
const CONNS = ["a request split across writes reassembles at readUntil", "a needle split across chunks is found", "all 256 byte values pass both ways", "a 1 MiB payload is relayed whole",
    "10 MiB to a paused reader survive the origin's close and the 4 MiB queue cap", "a write not awaited before close() still arrives whole", "end() sends FIN; in event mode the FIN closes the connection, so a peer replies before it",
    "readAll with a timeout resolves timedOut instead of throwing", "a plain dyna:net client sees the same relay"];
t.test("the in-process transcript and the proxy transcript are byte-identical", async (a) => {
    const canon = (p) => { try { return file.readFile(p).split("\n").filter(Boolean).map((l) => { const o = JSON.parse(l); return o.in + "|" + o.out; }).sort(); } catch { return []; } };
    for (let end = Date.now() + 15000; Date.now() < end && (canon(T + "/echo.rec").length < CONNS.length || canon(T + "/proxy.rec").length < CONNS.length);) await sleep(25);
    const self = canon(T + "/echo.rec"), prox = canon(T + "/proxy.rec");
    a.ok(self.length === CONNS.length && prox.length === CONNS.length, "both recorders logged " + CONNS.length + " connections: " + self.length + " / " + prox.length);
    a.ok(self.join("\n") === prox.join("\n"), "the transcripts differ");
    a.ok(self.some((l) => l.startsWith(hex(enc("fin\nxyz")))), "in = bytes the origin received");
}, { timeoutMs: 30000, dependsOn: CONNS });
await t.run();
