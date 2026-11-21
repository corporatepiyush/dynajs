// covers: dyna:net.TCPServer
// A client whose TLS handshake fails, or whose connect times out, releases the
// event loop: this script keeps both handles, never calls close() on them, and
// must still exit on its own. A regression is a HANG, reported as a timeout.
// A reactor freed under a live client makes its handlers never fire and the
// script end silently; the watchdog turns that silence into a failure. It is
// cleared once both cases have reported, so a leaked hold still shows as a hang.
import { TCPServer } from "dyna:net";
import { suite } from "../lib/kit.js";

const t = suite("test_net_client_fail_exit");
let A = null;
const check = (ok, what) => A.ok(ok, what);
const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
let watchdog = null;
t.before(() => { watchdog = setTimeout(() => { print("FAIL: a handler never fired"); globalThis.std.exit(1); }, 8000); });
t.after(() => { if (watchdog) clearTimeout(watchdog); });

function connectTls(opts, handlers) {
    try { return TCPServer.connect(opts, handlers); } catch (e) {
        if (/no TLS support/.test(e.message))
            throw new Error("test_net_client_fail_exit: TLS (OpenSSL 3.5) is a hard dependency of every native build, and this binary lacks it");
        throw e;
    }
}

test("a failed TLS handshake reaches the handler and holds no loop", () => new Promise((resolve) => {
    const plain = new TCPServer({ port: 0 });
    plain.start({ connect(conn) { conn.write("NOT TLS\r\n"); } });
    let tlsDone = false;
    let tlsErr = null;
    function tlsOver(err) {
        if (tlsDone) return;
        tlsDone = true;
        tlsErr = err;
        plain.close();
        check(typeof tlsErr === "string" && tlsErr.length > 0, "the handshake failure reached the handler (" + tlsErr + ")");
        resolve();
    }
    globalThis.keptTls = connectTls({ host: "127.0.0.1", port: plain.port, tls: { rejectUnauthorized: false } }, {
        connect(conn, err) { tlsOver(err || "connected to a non-TLS peer"); },
        close() { tlsOver("closed without a connect report"); },
    });
}));

test("a timed-out connect reaches the handler and holds no loop", () => new Promise((resolve) => {
    let reported = false;
    globalThis.keptSlow = TCPServer.connect({ host: "203.0.113.1", port: 9, connectTimeoutMs: 300 }, {
        connect(conn, err) {
            if (reported) return;
            reported = true;
            check(typeof err === "string" && err.length > 0, "the failed connect reached the handler (" + err + ")");
            resolve();
        },
    });
}));

await t.run();
