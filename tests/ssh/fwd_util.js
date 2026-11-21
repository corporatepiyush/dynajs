// Shared helpers for the dyna:ssh port-forwarding suites: TCP targets (a half-open echo that writes every byte back before its FIN, and a half-close target that answers "got N" after the
// peer's FIN), the sockets accepted on a port (lsof), a stall target that never reads, a burst target that writes N bytes then FINs, a TCP client that sends bytes, half-closes and reads to EOF (optionally slowly), the process fd count, and a non-loopback local address.
import * as os from "os";
import * as sys from "dyna:sys";
import { TCPServer } from "dyna:net";

export function echoTarget() {
    const s = new TCPServer({ port: 0 });
    s.start({
        connect: (conn) => {
            const { readable, writable } = conn.streams({ allowHalfOpen: true });
            (async () => {
                const r = readable.getReader();
                const w = writable.getWriter();
                for (;;) { const { value, done } = await r.read(); if (done) break; await w.write(value); }
                await w.close();
            })().catch(() => {});
        },
    });
    return s;
}

export function halfTarget() {
    const s = new TCPServer({ port: 0 });
    s.start({
        connect: (conn) => {
            const { readable, writable } = conn.streams({ allowHalfOpen: true });
            (async () => {
                let n = 0;
                const r = readable.getReader();
                for (;;) { const { value, done } = await r.read(); if (done) break; n += value.byteLength; }
                const w = writable.getWriter();
                await w.write("got " + n);
                await w.close();
            })().catch(() => {});
        },
    });
    return s;
}

export function burstTarget(n) {
    const s = new TCPServer({ port: 0 });
    s.start({
        connect: (conn) => {
            const { writable } = conn.streams({ allowHalfOpen: true });
            (async () => {
                const w = writable.getWriter();
                const u = new Uint8Array(n);
                for (let i = 0; i < n; i++) u[i] = (i * 7) & 255;
                await w.write(u);
                await w.close();
            })().catch(() => {});
        },
    });
    return s;
}

export function stallTarget() {
    const s = new TCPServer({ port: 0 });
    const held = [];
    s.start({ connect: (conn) => { held.push(conn.streams({ allowHalfOpen: true })); } });
    return s;
}

export function exchange(port, data, opts = {}) {
    return new Promise((resolve) => {
        const chunks = [];
        let settled = false;
        const done = (err) => {
            if (settled) return;
            settled = true;
            let n = 0;
            for (const c of chunks) n += c.byteLength;
            const out = new Uint8Array(n);
            let o = 0;
            for (const c of chunks) { out.set(c, o); o += c.byteLength; }
            resolve({ bytes: out, error: err || null });
        };
        const timer = setTimeout(() => done("timeout"), opts.timeoutMs || 20000);
        TCPServer.connect({ host: opts.host || "127.0.0.1", port }, {
            connect: (conn, err) => {
                if (!conn) { clearTimeout(timer); done(err || "connect failed"); return; }
                const { readable, writable } = conn.streams({ allowHalfOpen: true });
                (async () => {
                    const w = writable.getWriter();
                    if (data && data.byteLength !== 0) await w.write(data);
                    if (!opts.keepOpen) await w.close();
                })().catch(() => {});
                (async () => {
                    const r = readable.getReader();
                    for (;;) { const { value, done: d } = await r.read(); if (d) break; chunks.push(value); if (opts.slowMs) await new Promise((res) => setTimeout(res, opts.slowMs)); }
                    clearTimeout(timer);
                    done(null);
                })().catch((e) => { clearTimeout(timer); done(String(e)); });
            },
            close: () => { setTimeout(() => { clearTimeout(timer); done(null); }, 50); },
        });
    });
}

export function fdCount() {
    return os.readdir("/dev/fd")[0].length;
}

export function lanAddress() {
    for (const [cmd, args] of [["ipconfig", ["getifaddr", "en0"]], ["ipconfig", ["getifaddr", "en1"]], ["hostname", ["-I"]]]) {
        if (!sys.Which(cmd)) continue;
        const r = sys.Exec(cmd, args);
        const a = r.code === 0 ? r.stdout.trim().split(/\s+/)[0] : "";
        if (/^\d+\.\d+\.\d+\.\d+$/.test(a) && !a.startsWith("127.")) return a;
    }
    return null;
}

export function acceptedSockets(port) {
    if (!sys.Which("lsof")) return null;
    const r = sys.Exec("lsof", ["-nP", "-iTCP:" + port, "-a", "-p", String(sys.pid())]);
    return r.stdout.split("\n").filter((l) => l.includes(":" + port + "->")).length;
}

export function text(u8) {
    return new TextDecoder().decode(u8);
}

export function boundAddress(port) {
    if (!sys.Which("lsof")) return null;
    const r = sys.Exec("lsof", ["-nP", "-iTCP:" + port, "-sTCP:LISTEN", "-a", "-p", String(sys.pid())]);
    const m = /TCP (\S+):\d+ \(LISTEN\)/.exec(r.stdout);
    return m ? m[1] : null;
}
