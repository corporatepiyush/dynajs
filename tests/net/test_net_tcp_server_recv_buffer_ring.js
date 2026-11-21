// covers: dyna:net.TCPServer
// flags: --std
// timeout: 120
// On the io_uring reactor every TCP connection receives through one shared ring of 64 provided
// buffers. With more connections than buffers busy at once, a connection's multishot receive
// ends with ENOBUFS. The reactor re-armed it at most 64 times in a row, then handed -ENOBUFS to
// the connection, which closed it: under load a healthy peer lost its connection and the bytes
// still in flight. ENOBUFS now always re-arms, as the UDP path and the HTTP server ring do.
// A child process opens K connections and streams a known pattern on all of them for 3 s, then
// closes them and records what it wrote; the server checks every connection's byte count against
// that record and samples the pattern. Elsewhere (epoll, kqueue) this is a plain many-connection
// integrity check.
// Mutant (red on uring): the old give-up after 64 re-arms.
import * as os from "os";
import { TCPServer } from "dyna:net";
import { args } from "dyna:sys";
import { makeTempDir, readFile, removeAll, writeFile } from "dyna:file";

import { suite } from "../lib/kit.js";

const t = suite("test_net_tcp_server_recv_buffer_ring");
let A = null;
const ok = (c, m) => A.ok(c, m);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const K = 128, STREAM_MS = 3000, CH = 65536;
const pat = (idx, i) => (idx * 7 + i) & 255;
const conns = new Map();
const done = [];
let srv = null, dir = null, child, totals;
t.before(() => {
srv = new TCPServer({ port: 0 });
srv.start({
    data: (c, b) => {
        let st = conns.get(c);
        if (!st) { st = { idx: -1, got: 0, bad: 0, head: [] }; conns.set(c, st); }
        let off = 0;
        while (st.head.length < 2 && off < b.length) st.head.push(b[off++]);
        if (st.idx < 0 && st.head.length === 2) st.idx = st.head[0] | (st.head[1] << 8);
        for (let i = off; i < b.length; i += 97) if (b[i] !== pat(st.idx, st.got + i - off)) st.bad++;
        if (b.length > off && b[b.length - 1] !== pat(st.idx, st.got + b.length - 1 - off)) st.bad++;
        st.got += b.length - off;
    },
    close: (c) => {
        const st = conns.get(c) || { idx: -1, got: 0, bad: 0 };
        done.push(st);
        conns.delete(c);
    },
});
dir = makeTempDir("dyna-tcp-ring-");
child = dir.join("streams.js"); totals = dir.join("totals.json");
writeFile(child,
    "import { TCPServer } from 'dyna:net';\n" +
    "import { Path, writeFile } from 'dyna:file';\n" +
    "const [port, K, MS, CH] = scriptArgs.slice(1, 5).map(Number), out = scriptArgs[5];\n" +
    "const base = new Uint8Array(CH + 256);\n" +
    "for (let i = 0; i < base.length; i++) base[i] = i & 255;\n" +
    "const sent = new Array(K).fill(0), end = Date.now() + MS;\n" +
    "let open = K;\n" +
    "const finish = () => { if (--open === 0) writeFile(new Path(out), JSON.stringify(sent)); };\n" +
    "for (let idx = 0; idx < K; idx++) {\n" +
    "  const pump = (c) => {\n" +
    "    while (Date.now() < end && c.bufferedAmount < 4 * CH) {\n" +
    "      const o = (idx * 7 + sent[idx]) & 255;\n" +
    "      c.write(base.subarray(o, o + CH));\n" +
    "      sent[idx] += CH;\n" +
    "    }\n" +
    "    if (Date.now() >= end && c.bufferedAmount === 0) { c.close(); finish(); }\n" +
    "  };\n" +
    "  TCPServer.connect({ host: '127.0.0.1', port }, {\n" +
    "    connect: (c) => { if (!c) { finish(); return; } c.write(new Uint8Array([idx & 255, idx >> 8])); pump(c); },\n" +
    "    drain: pump,\n" +
    "  });\n" +
    "}\n");
});
t.after(() => {
    if (srv) srv.close();
    if (dir) removeAll(dir);
});

const STREAM = "the streaming child writes on " + K + " connections for " + STREAM_MS + " ms and exits cleanly";
let sent = [];
test(STREAM, async () => {
const pid = os.exec([args()[0], "--std", String(child), String(srv.port), String(K), String(STREAM_MS), String(CH), String(totals)], { block: false });
ok(pid > 0, "the streaming child started");
let status = null;
for (const end = Date.now() + STREAM_MS + 60000; Date.now() < end;) {
    const [p, st] = os.waitpid(pid, os.WNOHANG);
    if (p === pid) { status = st; break; }
    await sleep(20);
}
ok(status === 0, "the streaming child wrote for " + STREAM_MS + " ms and exited cleanly (status " + status + ")");
for (const end = Date.now() + 20000; Date.now() < end && done.length < K;) await sleep(20);
try { sent = JSON.parse(readFile(totals)); } catch (e) {}
print("  streamed " + (sent.reduce((a, b) => a + b, 0) / 1048576).toFixed(0) + " MiB over " + K + " connections");
}, { timeoutMs: STREAM_MS + 90000 });
test("every connection the child streamed on arrives and closes", () => {
ok(sent.length === K && done.length === K, `every connection the child streamed on arrived and closed (${done.length}/${K}, totals for ${sent.length})`);
}, { dependsOn: [STREAM] });
test("no connection loses bytes", () => {
const short = done.filter((s) => s.idx < 0 || s.got !== sent[s.idx]);
ok(short.length === 0, `no connection lost bytes (${short.length} short; first got ${short.length ? short[0].got : 0} of ${short.length ? sent[short[0].idx] : 0})`);
}, { dependsOn: [STREAM] });
test("the sampled pattern is intact on every connection", () => {
const corrupt = done.filter((s) => s.bad > 0);
ok(corrupt.length === 0, `the sampled pattern is intact on every connection (${corrupt.length} corrupt)`);
}, { dependsOn: [STREAM] });

await t.run();
