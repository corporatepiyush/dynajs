// tests/lib/rawpeer.js -- scripted raw-TCP peers (a hostile origin, a byte-exact client) for tests that need a wire
// counterpart outside the engine under test. A peer is its own dynajs process (spawnHelper `${EXE} --std peer.js ...`),
// built on dyna:net TCPServer data events only (never dyna:http), so a bug in the HTTP layer cannot mask itself.
//
//   import { listen, connect, enc, cat, find, latin1 } from "<repo>/tests/lib/rawpeer.js";
//   listen(async (c) => { const r = await c.readUntil("\r\n\r\n", { timeoutMs: 4000 }); await c.write("HTTP/1.1 200 OK\r\n..."); c.close(); },
//          { portFile: scriptArgs[1] });
//   const c = await connect(port); await c.write(bytes); const r = await c.readAll({ timeoutMs: 2000 });
//
// readUntil/readAll/readBytes(n) resolve { bytes, found, eof, timedOut }: a timeout is a result, not a throw. write() resolves once the
// kernel took the bytes (written in 256 KiB pieces under the connection's queue cap); end() half-closes (FIN) and close() closes,
// each once every write() call finished and the queue drained, so no queued byte is dropped; abort() closes at once. With RAWPEER_REC=<dir> every connection appends one
// canonical line {"in":hex,"out":hex,"t":start ms} (bytes this peer received / sent, concatenated) to <dir>/<script basename>.rec when it
// closes, and a listen() peer records its still-open connections on SIGTERM (spawnHelper's stop()); tests/lib/tcprec.js writes the same format in front of any other peer, and tools/peer-proof.js diffs the two.
import { TCPServer } from "dyna:net";
import * as sys from "dyna:sys";
import * as file from "dyna:file";

const REC = sys.env().RAWPEER_REC || "";
const PIECE = 256 << 10;
const LIVE = new Set();
export function recordOnTerm(flush) {
    Promise.all([import("os"), import("std")]).then(([os, std]) => os.signal(15, () => { try { flush(); } finally { std.exit(0); } })).catch(() => {});
}
const SELF = (typeof scriptArgs !== "undefined" && scriptArgs[0] ? String(scriptArgs[0]) : "peer").replace(/^.*\//, "").replace(/\.[a-z]+$/, "");

export function enc(s) {
    if (s instanceof Uint8Array) return s;
    const u = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255;
    return u;
}
export function latin1(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i += 8192) s += String.fromCharCode.apply(null, u8.subarray(i, i + 8192));
    return s;
}
export function cat(...parts) {
    const bs = parts.map(enc);
    const out = new Uint8Array(bs.reduce((n, b) => n + b.length, 0));
    let o = 0;
    for (const b of bs) { out.set(b, o); o += b.length; }
    return out;
}
export function find(hay, needle, from = 0) {
    const n = enc(needle);
    outer: for (let i = from; i + n.length <= hay.length; i++) {
        for (let k = 0; k < n.length; k++) if (hay[i + k] !== n[k]) continue outer;
        return i;
    }
    return -1;
}
export function hex(u8) {
    return u8.toHex();
}
export function record(dir, name, inBytes, outBytes, t = 0) {
    const w = new file.FileWriter(dir + "/" + name + ".rec", { append: true });
    w.write(JSON.stringify({ in: hex(inBytes), out: hex(outBytes), t }) + "\n");
    w.close();
}

class Peer {
    constructor() {
        this.conn = null;
        this.buf = [];
        this.eof = false;
        this.wake = null;
        this.drained = [];
        this.writing = 0;
        this.ending = false;
        this.closing = false;
        this.got = [];
        this.sent = [];
        this.recorded = false;
        this.t0 = Date.now();
        LIVE.add(this);
    }
    _push(b) { this.buf.push(b); this.got.push(b); this._kick(); }
    _end() {
        this.eof = true;
        this._kick();
        this._drained();
        this._record();
    }
    _record() {
        LIVE.delete(this);
        if (REC && !this.recorded) { this.recorded = true; record(REC, SELF, cat(...this.got), cat(...this.sent), this.t0); }
    }
    _kick() { if (this.wake) { const w = this.wake; this.wake = null; w(); } }
    _wait(ms) {
        return new Promise((res) => {
            const t = ms > 0 ? setTimeout(() => { this.wake = null; res(false); }, ms) : null;
            this.wake = () => { if (t) clearTimeout(t); res(true); };
        });
    }
    async read({ timeoutMs = 0 } = {}) {
        while (!this.buf.length && !this.eof) if (!(await this._wait(timeoutMs)) && !this.buf.length) return { bytes: null, timedOut: true };
        return { bytes: this.buf.length ? this.buf.shift() : null, timedOut: false };
    }
    async _until(stop, timeoutMs, max) {
        const end = timeoutMs > 0 ? Date.now() + timeoutMs : 0;
        let acc = new Uint8Array(0), seen = 0;
        for (;;) {
            if (this.buf.length) { acc = cat(acc, ...this.buf); this.buf = []; }
            const at = stop ? stop(acc, seen) : -1;
            seen = acc.length;
            if (at >= 0) {
                if (at < acc.length) this.buf.unshift(acc.subarray(at));
                return { bytes: acc.subarray(0, at), found: true, eof: false, timedOut: false };
            }
            if (max && acc.length >= max) return { bytes: acc, found: false, eof: false, timedOut: false };
            if (this.eof) return { bytes: acc, found: false, eof: true, timedOut: false };
            const left = end ? end - Date.now() : 0;
            if (end && left <= 0) return { bytes: acc, found: false, eof: false, timedOut: true };
            await this._wait(end ? left : 0);
        }
    }
    readUntil(needle, { timeoutMs = 0, max = 0 } = {}) {
        const n = enc(needle);
        return this._until((acc, seen) => { const i = find(acc, n, Math.max(0, seen - n.length + 1)); return i < 0 ? -1 : i + n.length; }, timeoutMs, max);
    }
    readAll({ timeoutMs = 0, max = 0 } = {}) { return this._until(null, timeoutMs, max); }
    readBytes(n, { timeoutMs = 0 } = {}) { return this._until((acc) => (acc.length >= n ? n : -1), timeoutMs, 0); }
    async write(data) {
        const b = enc(data);
        if (!this.conn || this.eof) return false;
        this.sent.push(b);
        this.writing++;
        try { return await this._write(b); } finally { this.writing--; this._settle(); }
    }
    async _write(b) {
        let off = 0;
        do {
            while (!this.eof && this.conn.bufferedAmount > PIECE) await this._drain();
            if (this.eof) return false;
            try { this.conn.write(b.subarray(off, off + PIECE)); } catch { return false; }
            off += PIECE;
        } while (off < b.length);
        while (!this.eof && this.conn.bufferedAmount) await this._drain();
        return !this.eof;
    }
    _drain() { return new Promise((res) => this.drained.push(res)); }
    _drained() {
        const w = this.drained;
        this.drained = [];
        for (const d of w) d();
        this._settle();
    }
    _settle() {
        if (this.eof || !this.conn || this.writing || this.conn.bufferedAmount) return;
        if (this.ending) { this.ending = false; try { this.conn.end(); } catch {} }
        if (this.closing) this.abort();
    }
    end() { if (this.conn && !this.eof && (this.writing || this.conn.bufferedAmount)) this.ending = true; else try { this.conn.end(); } catch {} }
    close() { if (this.conn && !this.eof && (this.writing || this.conn.bufferedAmount)) this.closing = true; else this.abort(); }
    abort() { try { this.conn.close(); } catch {} this._end(); }
}

function handlers(peers, onPeer, onFail) {
    return {
        connect(c, err) {
            if (!c) { if (onFail) onFail(err); return; }
            const p = new Peer();
            p.conn = c;
            peers.set(c, p);
            onPeer(p);
        },
        data(c, b) { const p = peers.get(c); if (p) p._push(b); },
        drain(c) { const p = peers.get(c); if (p) p._drained(); },
        close(c) { const p = peers.get(c); if (p) { peers.delete(c); p._end(); } },
    };
}

export function listen(onConn, { port = 0, portFile = "" } = {}) {
    const srv = new TCPServer({ port });
    const peers = new Map();
    srv.start(handlers(peers, (p) => {
        Promise.resolve().then(() => onConn(p)).catch(() => {}).finally(() => { if (!p.eof) p.close(); });
    }));
    if (REC) recordOnTerm(() => { for (const p of [...LIVE]) p._record(); });
    if (portFile) file.writeFile(portFile, String(srv.port) + "\n", { atomic: true });
    return srv;
}

export function connect(port, host = "127.0.0.1", { connectTimeoutMs = 5000 } = {}) {
    return new Promise((res, rej) => {
        const peers = new Map();
        TCPServer.connect({ host, port, connectTimeoutMs }, handlers(peers, res, (e) => rej(new Error("connect: " + e))));
    });
}

export function readPort(path, { timeoutMs = 20000 } = {}) {
    const end = Date.now() + timeoutMs;
    for (;;) {
        let s = "";
        try { s = file.readFile(path); } catch {}
        const m = /^\s*(\d{1,5})\s*$/.exec(String(s));
        const n = m ? Number(m[1]) : 0;
        if (n >= 1 && n <= 65535) return Promise.resolve(n);
        if (Date.now() > end) return Promise.resolve(0);
        return new Promise((r) => setTimeout(r, 25)).then(() => readPort(path, { timeoutMs: end - Date.now() }));
    }
}
