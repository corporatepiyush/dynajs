// flags: --std
// timeout: 60
// covers: integration c:src/modules/net-tcp/net-tcp-module.c c:src/modules/net-tcp/net-tcp-udp.c c:src/dyna-nat.c c:src/modules/http/http-server.c c:src/modules/http/http-server-async.c c:src/modules/proxy/proxy-module.c c:src/modules/dns/dns-server.c -- one start()-to-close() GC-root contract shared by every server class
// v10b-29: a server whose only JS reference sat in a cycle (the awaiting async function -> the promise its close/data handler resolves ->
// the handlers object -> the server) was collected by the cycle collector while it was delivering: gc_free_cycles freed the handler
// function that was running (a heap-use-after-free in js_closure2 under ASan, a segfault in release). Case 1 runs std.gc() inside the data
// handler once the cycle is otherwise unreachable, so an unpinned server frees the running handler deterministically.
// v10b-30 (owner decision, node semantics): a started listening server (TCPServer, IpcServer, UDPSocket, HTTPServer, HTTPServerAsync)
// is a GC root from start() until close() (or stop() for the HTTP servers), even when its handle is dropped; an unstarted one, and a
// closed or stopped one, stays collectable. Other handles keep their GC behaviour.
// net-4: DNSServer, TCPProxy and dyna:http App joined the same contract (they were collected once their handle was dropped,
// so an App started inside a block stopped serving and the process exited after its next GC). Mutant (red): any one of the three
// start() functions without its dyn_res_pin.
import { suite } from "../lib/kit.js";
import { TCPServer, UDPSocket } from "dyna:net";
import { TCPProxy } from "dyna:proxy";
import { DNSServer, DNSResolver } from "dyna:dns";
import { HTTPServer, HTTPServerAsync, App } from "dyna:http";
import * as std from "std";

const t = suite("tcp_gc_root");
const T = { timeoutMs: 15000 };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const gcs = async () => { for (let k = 0; k < 4; k++) { std.gc(); await sleep(10); } };

async function serveOnce() {
    const srv = new TCPServer({ port: 0 });
    let done;
    const got = new Promise((r) => { done = r; });
    srv.start({
        data(c, b) {
            std.gc();
            const sum = [...new Uint8Array(b)].reduce((x, y) => x + y, 0);
            const later = () => sum;
            c.close();
            done(later());
        },
    });
    TCPServer.connect({ host: "127.0.0.1", port: srv.port }, { connect(c) { c.write(new Uint8Array([1, 2, 3, 4])); } });
    const v = await got;
    srv.close();
    return v;
}

function tcpReach(port) {
    return new Promise((r) => {
        const g = setTimeout(() => r("timeout"), 2000);
        TCPServer.connect({ host: "127.0.0.1", port }, {
            connect(c, e) { clearTimeout(g); r(e ? "ERR " + e : "OK"); c.close(); },
            close() { clearTimeout(g); r("CLOSED"); },
        });
    });
}

const upstream = new TCPServer({ port: 0 });
upstream.start({});
let udpGot = 0;
const kinds = {
    TCPServer: { make() { const s = new TCPServer({ port: 0 }); s.start({}); return s; }, reach: tcpReach, end(s) { s.close(); } },
    UDPSocket: {
        make() { const s = new UDPSocket({ port: 0, host: "127.0.0.1" }); s.start({ message() { udpGot++; } }); return s; },
        async reach(port) { const before = udpGot; const c = new UDPSocket({ port: 0, host: "127.0.0.1" }); c.send("x", "127.0.0.1", port); for (let i = 0; i < 50 && udpGot === before; i++) await sleep(10); c.close(); return udpGot > before ? "OK" : "lost"; },
        end(s) { s.close(); },
    },
    HTTPServer: { make() { const s = new HTTPServer({ port: 0, routes: { "/": "hi" } }); s.start(); return s; }, reach: tcpReach, end(s) { s.stop(); } },
    HTTPServerAsync: { make() { const s = new HTTPServerAsync({ port: 0, routes: { "/": "hi" } }); s.start(); return s; }, reach: tcpReach, end(s) { s.stop(); } },
    App: { make() { const s = new App({ port: 0, host: "127.0.0.1" }); s.get("/", () => "hi"); s.start(); return s; }, reach: tcpReach, end(s) { s.close(); } },
    TCPProxy: { make() { const s = new TCPProxy({ port: 0, host: "127.0.0.1", upstream: { host: "127.0.0.1", port: upstream.port } }); s.start(); return s; }, reach: tcpReach, end(s) { s.close(); } },
    DNSServer: {
        make() { const s = new DNSServer({ port: 0, host: "127.0.0.1" }); s.start(() => "1.2.3.4"); return s; },
        async reach(port) {
            const r = new DNSResolver({ server: "127.0.0.1", port, timeoutMs: 1500 });
            try { const recs = await r.lookup("pin.test"); return recs.length === 1 && recs[0].address === "1.2.3.4" ? "OK" : "bad " + JSON.stringify(recs); }
            catch (e) { return "ERR " + e; } finally { r.close(); }
        },
        end(s) { s.close(); },
    },
};

t.test("a server with a live connection, reachable only through its own handlers survives a GC inside its data handler", async (a) => {
    let guard;
    const v = await Promise.race([serveOnce(), new Promise((r) => { guard = setTimeout(() => r("timeout"), 5000); })]);
    clearTimeout(guard);
    a.eq(v, 10, "the data handler ran to completion and its promise resolved");
}, T);

for (const [name, k] of Object.entries(kinds)) {
    t.test(name + ": a started server whose handle is dropped stays alive and serving until close()", async (a) => {
        let fin = 0;
        const fr = new FinalizationRegistry(() => { fin++; });
        const refs = [];
        (() => { for (let i = 0; i < 3; i++) { const s = k.make(); fr.register(s, i); refs.push({ w: new WeakRef(s), port: s.port }); } })();
        await gcs();
        a.eq(fin, 0, "no dropped started server was collected");
        for (const { w, port } of refs) {
            a.eq(await k.reach(port), "OK", "the dropped server still serves");
            const s = w.deref();
            a.eq(s !== undefined, true, "its handle is still reachable through a WeakRef");
            if (s) s.close();
        }
    }, T);
    t.test(name + ": a started server that was " + (name.startsWith("HTTP") ? "stopped" : "closed") + " and dropped is collected", async (a) => {
        let fin = 0;
        const fr = new FinalizationRegistry(() => { fin++; });
        (() => { for (let i = 0; i < 5; i++) { const s = k.make(); fr.register(s, i); k.end(s); } })();
        await gcs();
        a.eq(fin, 5, "every ended server was collected (the pin went with close/stop)");
    }, T);
}

t.test("TCPServer: a dropped server whose connections ended is still alive; close() through a WeakRef releases it", async (a) => {
    let fin = 0, ended = 0;
    const fr = new FinalizationRegistry(() => { fin++; });
    const refs = [];
    await (async function () {
        for (let i = 0; i < 5; i++) {
            const s = new TCPServer({ port: 0 });
            const gone = new Promise((r) => s.start({ close() { ended++; r(); } }));
            TCPServer.connect({ host: "127.0.0.1", port: s.port }, { connect(c) { c.close(); } });
            await gone;
            fr.register(s, i);
            refs.push(new WeakRef(s));
        }
    })();
    await gcs();
    a.eq(ended, 5, "every server saw its connection end");
    a.eq(fin, 0, "no started server was collected while open");
    (() => { for (const w of refs) { const s = w.deref(); if (s) s.close(); } })();
    await gcs();
    a.eq(fin, 5, "after close() every dropped server was collected");
}, T);

await t.run();
upstream.close();
