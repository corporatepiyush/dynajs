// tests/lib/tcprec.js UPSTREAM_PORTFILE OWN_PORTFILE LOG [client|server [MAPFILE]] -- a recording TCP proxy (dynajs --std). Waits for the upstream
// peer's port file, listens on an ephemeral port, writes it to OWN_PORTFILE and relays every connection both ways;
// when both sides closed it appends {"in":hex,"out":hex,"t":start ms} (bytes the upstream peer received / sent) to LOG, the
// format tests/lib/rawpeer.js records in-process; with `client` the peer is the one connecting in, so in/out swap. Used by tools/peer-proof.js to transcript a peer it cannot edit.
// Lossless under load: a side whose peer closed is end()ed only once its queue drained (end() throws while bytes are queued
// and close() would drop them), and bytes past the connection's queue cap wait in a JS queue for drain instead of being refused;
// past 16 KiB in flight the sending side is paused, so the proxy keeps the peers' flow control. On SIGTERM it logs the still-open pairs.
// MAPFILE ("REAL=PROXY ...", read once it exists) rewrites 127.0.0.1:REAL to 127.0.0.1:PROXY in what the peer sends, so a redirect
// to a sibling port stays behind that port's proxy; the log keeps the peer's own bytes.
import { TCPServer } from "dyna:net";
import { readPort, cat, hex, recordOnTerm, enc, latin1 } from "./rawpeer.js";
import * as file from "dyna:file";

const [up, own, log, role, mapFile] = scriptArgs.slice(1);
let MAP = null;
function relay(x) {
    if (!mapFile) return x;
    if (!MAP) { try { MAP = file.readFile(mapFile).trim().split(/\s+/).filter(Boolean).map((e) => e.split("=")); } catch { return x; } }
    let s = latin1(x);
    const t = s;
    for (const [real, mine] of MAP) s = s.split("127.0.0.1:" + real).join("127.0.0.1:" + mine);
    return s === t ? x : enc(s);
}
const swap = role === "client";
const port = await readPort(up);
if (!port) throw new Error("tcprec: no upstream port in " + up);
const CAP = 1 << 20;
const HIGH = 16 << 10;
const srv = new TCPServer({ port: 0 });
const pairs = new Map();
function done(p, force) {
    if (p.logged || (!force && (!p.aClosed || !p.bClosed))) return;
    p.logged = true;
    const w = new file.FileWriter(log, { append: true });
    w.write(JSON.stringify(swap ? { in: hex(cat(...p.out)), out: hex(cat(...p.in)), t: p.t0 } : { in: hex(cat(...p.in)), out: hex(cat(...p.out)), t: p.t0 }) + "\n");
    w.close();
}
function side() { return { c: null, q: [], qn: 0, src: null, paused: false, ending: false, closed: false }; }
function flush(s) {
    if (!s.c || s.closed) return;
    while (s.q.length && s.c.bufferedAmount + s.q[0].length <= CAP) {
        const x = s.q.shift();
        try { s.c.write(x); } catch { s.q.unshift(x); return; }
        s.qn -= x.length;
    }
    if (s.paused && !s.qn && s.c.bufferedAmount <= HIGH) { s.paused = false; try { s.src().resume(); } catch {} }
    if (s.ending && !s.q.length && !s.c.bufferedAmount) { s.ending = false; try { s.c.end(); } catch { s.ending = true; } }
}
function send(s, x) {
    s.q.push(x);
    s.qn += x.length;
    flush(s);
    if (!s.paused && s.qn + (s.c ? s.c.bufferedAmount : 0) > HIGH && s.src()) { s.paused = true; try { s.src().pause(); } catch {} }
}
function shut(s) { s.ending = true; flush(s); }
srv.start({
    connect(a) {
        const p = { A: side(), B: side(), in: [], out: [], aClosed: false, bClosed: false, logged: false, t0: Date.now() };
        p.A.c = a;
        p.A.src = () => p.B.c;
        p.B.src = () => a;
        pairs.set(a, p);
        TCPServer.connect({ host: "127.0.0.1", port }, {
            connect(b) {
                if (!b) { p.bClosed = true; p.B.closed = true; try { a.close(); } catch {} done(p); return; }
                p.B.c = b;
                flush(p.B);
            },
            data(b, x) { p.out.push(x); send(p.A, relay(x)); },
            drain() { flush(p.B); },
            close() { p.bClosed = true; p.B.closed = true; if (!p.aClosed) shut(p.A); done(p); },
        });
    },
    data(a, x) { const p = pairs.get(a); p.in.push(x); send(p.B, x); },
    drain(a) { flush(pairs.get(a).A); },
    close(a) {
        const p = pairs.get(a);
        p.aClosed = true;
        p.A.closed = true;
        pairs.delete(a);
        if (!p.bClosed) shut(p.B);
        done(p);
    },
});
recordOnTerm(() => { for (const p of pairs.values()) done(p, true); });
file.writeFile(own, String(srv.port) + "\n", { atomic: true });
