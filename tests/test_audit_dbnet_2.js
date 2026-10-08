// flags: --std
// tests/test_audit_dbnet_2.js -- the PostgreSQL startup handshake (N3-16).
//
// The client must enforce the real protocol order: an Authentication request
// first, an exchange the client actually completes, and ReadyForQuery only
// after that. A server that skips authentication -- bare ReadyForQuery, or an
// unsolicited AuthenticationOk -- must be REJECTED, because a client that
// believes it is authenticated when the server never authenticated it is the
// wrong direction to fail in.
//
// There is no PostgreSQL server here. Every case drives the client against an
// in-process scripted backend (dyna:net TCPServer) that speaks the wire
// protocol directly, so each message sequence is chosen exactly.
//
//   1. bare ReadyForQuery, no Authentication       -> must FAIL
//   2. Authentication(SCRAM), exchange completes   -> must CONNECT
//   3. Authentication, then an error mid-exchange  -> must FAIL cleanly
//   4. the client offered no password, the server
//      asks for none (AuthenticationOk)            -> must CONNECT
//
// Three more sequences close the windows around those four, each of which was
// accepted on the pre-fix build:
//   5. pg_hba trust: a password is set but the server sends AuthenticationOk
//      with no exchange -- legitimate, and the reason no rule may reject it
//   6. AuthenticationOk, then a cleartext challenge -> the password must
//      never reach the wire
//   7. a second AuthenticationSASL on one connection
//
// Case 2 uses the real wire encoding: standard-alphabet base64 with no
// padding, which is what a real server sends.
//
// flags: --std plus dyna:net + dyna:crypto (build with CONFIG_NATIVE_MODULES=y)
import * as std from "std";
import { TCPServer, PostgreSQL } from "dyna:net";
import { SHA256, HMAC, PBKDF2 } from "dyna:crypto";

let checks = 0, failures = 0;
function check(cond, msg) {
    checks++;
    if (!cond) { failures++; std.err.puts("FAIL: " + msg + "\n"); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --- wire helpers ----------------------------------------------------------

function bytes(s) {
    const a = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i) & 0xff;
    return a;
}
function latin1(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return s;
}
function i32(v) {
    return String.fromCharCode((v >>> 24) & 0xff, (v >>> 16) & 0xff,
                               (v >>> 8) & 0xff, v & 0xff);
}
function i16(v) { return String.fromCharCode((v >>> 8) & 0xff, v & 0xff); }
function rd32(s, at) {
    return ((s.charCodeAt(at) << 24) | (s.charCodeAt(at + 1) << 16) |
            (s.charCodeAt(at + 2) << 8) | s.charCodeAt(at + 3)) >>> 0;
}
function cstr(s) { return s + "\0"; }
function msg(type, body) { return type + i32(body.length + 4) + body; }

const AUTH_OK = msg("R", i32(0));
const READY = msg("Z", "I");
const SELECT1 = msg("T", i16(1) + cstr("one") + i32(0) + i16(0) + i32(23) +
                    i16(-1 & 0xffff) + i32(-1 >>> 0) + i16(0)) +
               msg("D", i16(1) + i32(1) + "1") +
               msg("C", cstr("SELECT 1")) + READY;

// RFC 5802 base64: the STANDARD alphabet, with the trailing '=' padding
// omitted. A 16-byte salt is 22 characters and a 32-byte signature is 43, so
// neither length is a multiple of 4 -- exactly the shapes a real server sends.
const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function b64u(u8) {
    let out = "";
    for (let i = 0; i < u8.length; i += 3) {
        const a = u8[i], b = i + 1 < u8.length ? u8[i + 1] : 0;
        const c = i + 2 < u8.length ? u8[i + 2] : 0;
        const n = (a << 16) | (b << 8) | c;
        out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] +
               (i + 1 < u8.length ? B64[(n >> 6) & 63] : "") +
               (i + 2 < u8.length ? B64[n & 63] : "");
    }
    return out;
}
const b = (s) => bytes(s);
const hmac = (key, data) => HMAC("sha256", key, data);

// A scripted backend. `plan` runs once the startup message has been read and
// decides everything the server says; `onMsg` handles each later client frame.
function backend(plan, onMsg) {
    const bufs = new Map();
    const srv = new TCPServer({ port: 0 });
    srv.sawStartup = false;
    srv.messages = [];
    srv.start({
        data: (c, buf) => {
            bufs.set(c, (bufs.get(c) || "") + latin1(buf));
            let s = bufs.get(c);
            if (!srv.sawStartup) {
                if (s.length < 4) return;
                const len = rd32(s, 0);
                if (s.length < len) return;
                const pkt = s.slice(0, len);
                bufs.set(c, s.slice(len));
                if (rd32(pkt, 4) === 80877102) { c.close(); return; }
                srv.sawStartup = true;
                srv.startup = pkt.slice(8);
                plan(c, (t) => c.write(bytes(t)));
                return;
            }
            if (s.length < 5) return;
            const type = s[0], len = rd32(s, 1);
            if (s.length < len + 1) return;
            const body = s.slice(5, len + 1);
            bufs.set(c, s.slice(len + 1));
            srv.messages.push({ type, body });
            if (onMsg) onMsg(c, type, body, (t) => c.write(bytes(t)));
        },
        close: (c) => { bufs.delete(c); },
    });
    return srv;
}

// Run one client against one scripted backend and report what happened.
async function attempt(make, srv, label) {
    let outcome = "threw", detail = "", ready = null, r0 = null;
    const g = make(srv.port);
    try {
        const r = await Promise.race([
            g.query("SELECT 1").then((v) => ({ ok: v }), (e) => ({ err: e })),
            sleep(2000).then(() => ({ timeout: true })),
        ]);
        if (r.timeout) outcome = "hung";
        else if (r.err) { outcome = "failed"; detail = r.err.message; }
        else outcome = "connected";
        try { ready = g.ready; } catch (e) { ready = "threw"; }
        try { r0 = g.parameters; } catch (e) { r0 = null; }
    } catch (e) {
        detail = e.message;
    }
    try { g.close(); } catch (e) { /* already dead */ }
    await sleep(20);
    return { outcome, detail, ready, label, params: r0 };
}

// --- case 1: ReadyForQuery with no Authentication at all -------------------

async function case1() {
    const srv = backend((c, send) => { send(READY); });
    const r = await attempt((p) => new PostgreSQL({ port: p, host: "127.0.0.1", user: "u" }), srv, "bare ReadyForQuery");
    srv.close();
    check(r.outcome === "failed",
          "case 1: a bare ReadyForQuery with no Authentication must be " +
          "rejected (got " + r.outcome + " -- " + r.detail + ")");
    check(/authenticat/i.test(r.detail || ""),
          "case 1: the refusal must name authentication (got " +
          JSON.stringify(r.detail) + ")");
    return r;
}

// --- case 2: a real SCRAM exchange, end to end -----------------------------

async function case2() {
    const password = "correct horse";
    const iters = 4096;
    const st = { step: 0, sig: null };

    const srv = backend(
        (c, send) => { send(msg("R", i32(10) + cstr("SCRAM-SHA-256") + "\0")); },
        (c, type, body, send) => {
            if (type === "Q") { send(SELECT1); return; }
            if (type !== "p") return;
            if (st.step === 0) {
                // SASLInitialResponse: cstring mechanism, int32 length, bytes.
                const z = body.indexOf("\0");
                const mlen = rd32(body, z + 1);
                const cf = body.slice(z + 5, z + 5 + mlen);
                const m = /r=([^,]*)/.exec(cf);
                const nonce = m ? m[1] : "";
                const rnonce = nonce + b64u(b("SERVER"));
                const cfBare = "n=,r=" + nonce;
                const salt = new Uint8Array(16);
                for (let i = 0; i < 16; i++) salt[i] = i;
                const sfirst = "r=" + rnonce + ",s=" + b64u(salt) + ",i=" + iters;
                const salted = PBKDF2({ password: b(password), salt, iterations: iters, length: 32 });
                const ckey = hmac(salted, b("Client Key"));
                const stored = SHA256(ckey);
                const withoutProof = "c=biws,r=" + rnonce;
                const authMsg = cfBare + "," + sfirst + "," + withoutProof;
                const csig = hmac(stored, b(authMsg));
                const proof = new Uint8Array(32);
                for (let i = 0; i < 32; i++) proof[i] = ckey[i] ^ csig[i];
                st.sig = hmac(hmac(salted, b("Server Key")), b(authMsg));
                st.step = 1;
                send(msg("R", i32(11) + sfirst));
            } else if (st.step === 1) {
                st.step = 2;
                send(msg("R", i32(12) + "v=" + b64u(st.sig)) + AUTH_OK + READY);
            }
        });
    const r = await attempt((p) => new PostgreSQL({ port: p, host: "127.0.0.1", user: "u", password }), srv, "full SCRAM");
    srv.close();
    check(r.outcome === "connected",
          "case 2: a completed SCRAM exchange must connect (got " +
          r.outcome + " -- " + r.detail + ")");
    check(r.ready === true, "case 2: ready must be true (got " + r.ready + ")");
    return r;
}

// --- case 3: an error part-way through the exchange ------------------------

async function case3() {
    const srv = backend(
        (c, send) => { send(msg("R", i32(10) + cstr("SCRAM-SHA-256") + "\0")); },
        (c, type, body, send) => {
            if (type !== "p") return;
            send(msg("E", "S" + cstr("FATAL") + "C" + cstr("28P01") +
                        "M" + cstr("password authentication failed") + "\0"));
            send(READY);
        });
    const r = await attempt((p) => new PostgreSQL({ port: p, host: "127.0.0.1", user: "u", password: "secret" }), srv, "SCRAM then error");
    srv.close();
    check(r.outcome === "failed",
          "case 3: an error mid-exchange must fail the connection (got " +
          r.outcome + " -- " + r.detail + ")");
    return r;
}

// --- case 4: the client offered no password, the server asks for none -------

async function case4() {
    const srv = backend(
        (c, send) => {
            send(msg("S", cstr("server_version") + cstr("16.0")) + AUTH_OK + READY);
        },
        (c, type, body, send) => { if (type === "Q") send(SELECT1); });
    const r = await attempt((p) => new PostgreSQL({ port: p, host: "127.0.0.1", user: "u" }), srv, "no auth requested");
    srv.close();
    check(r.outcome === "connected",
          "case 4: a client that asked for no auth must accept " +
          "AuthenticationOk (got " + r.outcome + " -- " + r.detail + ")");
    check(r.ready === true, "case 4: ready must be true (got " + r.ready + ")");
    return r;
}

// --- case 5: a password is configured and the server uses `trust` ----------
//
// pg_hba `trust` sends AuthenticationOk as the first message even though the
// client offered a password, so the client MUST accept it: the wire cannot
// tell a trust server from an impostor, and refusing here breaks a
// legitimate and common PostgreSQL configuration. The guarantee this case
// pins is the one that IS decidable -- once AuthenticationOk is accepted,
// no further challenge may be answered (case 6). What remains undecidable on
// the wire is filed as a ticket, not silently "fixed" into a regression.

async function case5() {
    const srv = backend(
        (c, send) => { send(AUTH_OK + READY); },
        (c, type, body, send) => { if (type === "Q") send(SELECT1); });
    const r = await attempt((p) => new PostgreSQL({ port: p, host: "127.0.0.1", user: "u", password: "secret" }), srv, "trust auth with a password set");
    srv.close();
    check(r.outcome === "connected",
          "case 5: pg_hba trust (AuthenticationOk with a password set) must " +
          "still connect -- refusing it is a compatibility regression (got " +
          r.outcome + " -- " + r.detail + ")");
    return r;
}

// --- case 6: AuthenticationOk, then a challenge for the password ------------

async function case6() {
    let leaked = null;
    const srv = backend(
        (c, send) => { send(AUTH_OK + msg("R", i32(3)) + READY); },
        (c, type, body, send) => {
            if (type === "p") leaked = body;
            if (type === "Q") send(SELECT1);
        });
    const r = await attempt((p) => new PostgreSQL({ port: p, host: "127.0.0.1", user: "u", password: "secret", insecureAuth: true }), srv, "challenge after AuthenticationOk");
    srv.close();
    check(leaked === null,
          "case 6: the password must never reach the wire after " +
          "AuthenticationOk (got " + JSON.stringify(leaked) + ")");
    check(r.outcome === "failed",
          "case 6: a challenge after AuthenticationOk must fail the " +
          "connection (got " + r.outcome + " -- " + r.detail + ")");
    return r;
}

// --- case 7: a second AuthenticationSASL on one connection ------------------

async function case7() {
    let answered = 0;
    const srv = backend(
        (c, send) => { send(msg("R", i32(10) + cstr("SCRAM-SHA-256") + "\0")); },
        (c, type, body, send) => {
            if (type !== "p") return;
            const z = body.indexOf("\0");
            const mlen = rd32(body, z + 1);
            const isFirst = body.slice(0, z) === "SCRAM-SHA-256";
            if (isFirst) answered++;
            // A real server-first would need the full proof; the point here is
            // only how many times the client STARTS the exchange, so answer
            // with a server-first the client will reject on its own, then offer
            // the whole exchange again.
            const cf = body.slice(z + 5, z + 5 + mlen);
            const m = /r=([^,]*)/.exec(cf);
            const nonce = m ? m[1] : "";
            const salt = new Uint8Array(16);
            for (let i = 0; i < 16; i++) salt[i] = i;
            send(msg("R", i32(11) + "r=" + nonce + b64u(b("SERVER")) +
                     ",s=" + b64u(salt) + ",i=4096"));
            send(msg("R", i32(10) + cstr("SCRAM-SHA-256") + "\0"));
        });
    const r = await attempt((p) => new PostgreSQL({ port: p, host: "127.0.0.1", user: "u", password: "secret" }), srv, "SCRAM restarted");
    srv.close();
    check(answered === 1,
          "case 7: the client must refuse a second AuthenticationSASL on one " +
          "connection (it started the exchange " + answered + " times)");
    check(/restart/i.test(r.detail || ""),
          "case 7: the refusal must name the restart (got " +
          JSON.stringify(r.detail) + ")");
    return r;
}

// --- case 8: an unbounded ParameterStatus stream (N3-19) --------------------
//
// A server can send ParameterStatus forever; each one adds an own property to
// the client's `params` object. There is a default cap.

async function case8() {
    let out = "";
    for (let i = 0; i < 4000; i++)
        out += msg("S", cstr("k" + i) + cstr("v"));
    const srv = backend(
        (c, send) => { send(out + AUTH_OK + READY); },
        (c, type, body, send) => { if (type === "Q") send(SELECT1); });
    const r = await attempt((p) => new PostgreSQL({ port: p, host: "127.0.0.1", user: "u" }), srv, "4000 ParameterStatus messages");
    srv.close();
    check(r.outcome === "failed",
          "case 8: an unbounded ParameterStatus stream must be capped (got " +
          r.outcome + " -- " + r.detail + ")");
    check(/ParameterStatus/i.test(r.detail || ""),
          "case 8: the refusal must name ParameterStatus (got " +
          JSON.stringify(r.detail) + ")");
    return r;
}

// --- case 9: a server that actually respects the cap still connects ---------

async function case9() {
    let out = "";
    for (let i = 0; i < 8; i++)
        out += msg("S", cstr("server_" + i) + cstr("v" + i));
    const srv = backend(
        (c, send) => { send(out + AUTH_OK + READY); },
        (c, type, body, send) => { if (type === "Q") send(SELECT1); });
    const r = await attempt((p) => new PostgreSQL({ port: p, host: "127.0.0.1", user: "u" }), srv, "8 ParameterStatus messages");
    let count = -1;
    try { count = Object.keys(r.params || {}).length; } catch (e) { /* n/a */ }
    srv.close();
    check(r.outcome === "connected",
          "case 9: an ordinary ParameterStatus run must still connect (got " +
          r.outcome + " -- " + r.detail + ")");
    check(count === 8, "case 9: all 8 parameters are readable (got " + count + ")");
    return r;
}

async function main() {
    const cases = [[1, case1], [2, case2], [3, case3], [4, case4],
                   [5, case5], [6, case6], [7, case7],
                   [8, case8], [9, case9]];
    for (const [i, f] of cases) {
        const r = await f();
        std.err.puts("  case" + i + " " + r.label + " -> " + r.outcome +
                     " :: " + r.detail + "\n");
    }
    print("test_audit_dbnet_2: checks=" + checks + " fails=" + failures);
    if (failures) std.exit(1);
    std.exit(0);
}
main();
