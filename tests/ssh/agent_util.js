// Shared helpers for the dyna:ssh agent suites: a raw ssh-agent protocol client over a unix socket (draft-miller-ssh-agent framing: uint32 length,
// then the message) that sends whole or hand-made frames and collects every reply until the agent closes, and a scripted JS agent that answers
// each request through a handler (a reply frame, nothing, or closing the socket) so the dyna:ssh client meets agents that misbehave, hang or die,
// and signs sk-ssh-ed25519@openssh.com requests the way a security key would (flags and counter over SHA256(application) and SHA256(data)).
import * as os from "os";
import { TCPServer } from "dyna:net";
import { SHA256 } from "dyna:hash";
import { Ed25519Sign } from "dyna:crypto";
import { W, R } from "./fake_server.js";

export const AG = { FAILURE: 5, SUCCESS: 6, REQ_IDS: 11, IDS: 12, SIGN: 13, SIGN_RESP: 14, ADD: 17, REMOVE: 18, REMOVE_ALL: 19, LOCK: 22, UNLOCK: 23, ADD_C: 25, EXT: 27 };
const te = new TextEncoder();

export const frame = (body) => new W().str(body).done();

// A unix socket address holds 103 path bytes on macOS (107 on Linux) and a runner's TMPDIR can be deeper than that, so socket paths
// under such a dir are made relative to the working directory (the sockets stay inside the dir; the processes share the cwd).
export function sockDir(dir) {
    if (dir.length + 24 <= 103) return dir;
    const [cwd] = os.getcwd();
    if (cwd && dir.startsWith(cwd + "/") && dir.length - cwd.length + 24 <= 103) return dir.slice(cwd.length + 1);
    throw new Error("no unix-socket-sized path for " + dir + "; run with a shorter TMPDIR");
}
export const cat = (...a) => { const w = new W(); for (const x of a) w.raw(x); return w.done(); };

export function edBlob(pub) { return new W().str("ssh-ed25519").str(pub).done(); }
export function skBlob(pub, app = "ssh:") { return new W().str("sk-ssh-ed25519@openssh.com").str(pub).str(app).done(); }
export function addEd(kp, comment = "raw", type = AG.ADD) { return new W().u8(type).str("ssh-ed25519").str(kp.publicKey).str(cat(kp.privateKey, kp.publicKey)).str(comment).done(); }
export function addSk(pub, comment = "sk", type = AG.ADD) { return new W().u8(type).str("sk-ssh-ed25519@openssh.com").str(pub).str("ssh:").u8(1).str(Uint8Array.of(1, 2, 3, 4)).str("").str(comment).done(); }

export function rawAgent(path) {
    return new Promise((resolve, reject) => {
        let buf = new Uint8Array(0);
        const replies = [];
        const waiters = [];
        let closed = false;
        const pump = () => {
            while (buf.length >= 4) {
                const n = new R(buf).u32();
                if (buf.length < 4 + n) break;
                replies.push(buf.slice(4, 4 + n));
                buf = buf.slice(4 + n);
            }
            while (waiters.length && (replies.length || closed)) waiters.shift()(replies.length ? replies.shift() : null);
        };
        const cli = TCPServer.connect({ path }, {
            connect: (conn, err) => {
                if (!conn) { reject(new Error("agent connect: " + err)); return; }
                resolve({
                    send(bytes) { conn.write(bytes); },
                    next() { return new Promise((r) => { waiters.push(r); pump(); }); },
                    async req(body) { conn.write(frame(body)); return this.next(); },
                    get closed() { return closed; },
                    close() { try { conn.close(); } catch {} try { cli.close(); } catch {} },
                });
            },
            data: (conn, bytes) => { buf = cat(buf, bytes); pump(); },
            close: () => { closed = true; pump(); },
        });
    });
}

export function skSign(seed, app, data, flags = 1, counter = 7) {
    const sig = Ed25519Sign(seed, cat(SHA256(te.encode(app)), new W().u8(flags).u32(counter).done(), SHA256(data)));
    return new W().str("sk-ssh-ed25519@openssh.com").str(sig).u8(flags).u32(counter).done();
}

export function edSign(seed, data) { return new W().str("ssh-ed25519").str(Ed25519Sign(seed, data)).done(); }

export function scriptedAgent(path, handler) {
    const s = new TCPServer({ path });
    const seen = [];
    s.start({
        connect: (conn) => { conn.buf = new Uint8Array(0); },
        data: (conn, bytes) => {
            conn.buf = cat(conn.buf, bytes);
            while (conn.buf.length >= 4) {
                const n = new R(conn.buf).u32();
                if (conn.buf.length < 4 + n) break;
                const msg = conn.buf.slice(4, 4 + n);
                conn.buf = conn.buf.slice(4 + n);
                seen.push(msg[0]);
                const out = handler(msg, new R(msg, 1));
                if (out === "close") { conn.close(); return; }
                if (out) conn.write(out.raw ? out.raw : frame(out));
            }
        },
    });
    return { server: s, seen, close() { try { s.close(); } catch {} } };
}

export function idsAnswer(list) {
    const w = new W().u8(AG.IDS).u32(list.length);
    for (const [blob, comment] of list) w.str(blob).str(comment || "");
    return w.done();
}
