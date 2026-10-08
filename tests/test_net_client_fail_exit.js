// flags: --std
// A client whose TLS handshake fails, or whose connect times out, releases the
// event loop: this script keeps both handles, never calls close() on them, and
// must still exit on its own. A regression is a HANG, reported as a timeout.
import { TCPServer } from "dyna:net";

// A reactor freed under a live client makes its handlers never fire and the
// script end silently; the watchdog turns that silence into a failure. It is
// cleared once both cases have reported, so a leaked hold still shows as a hang.
const watchdog = setTimeout(() => { print("FAIL: a handler never fired"); globalThis.std.exit(1); }, 8000);
let passed = 0, failed = 0;
const check = (ok, what) => { if (ok) passed++; else { failed++; print("FAIL: " + what); } };

// A plain-text peer: the TLS client reads "NOT TLS" where a ServerHello belongs.
const plain = new TCPServer({ port: 0 });
plain.start({ connect(conn) { conn.write("NOT TLS\r\n"); } });

let tlsErr = null, tlsDone = false;
function tlsOver(err) {
    if (tlsDone) return;
    tlsDone = true;
    tlsErr = err;
    plain.close();
    check(typeof tlsErr === "string" && tlsErr.length > 0, "the handshake failure reached the handler (" + tlsErr + ")");
    timeoutCase();
}
function connectTls(opts, handlers) {
    try { return TCPServer.connect(opts, handlers); } catch (e) {
        if (!/no TLS support/.test(e.message)) throw e;
        print("test_net_client_fail_exit: SKIP the TLS case (this build has no TLS)");
        tlsDone = true;
        plain.close();
        timeoutCase();
        return null;
    }
}
globalThis.keptTls = connectTls({ host: "127.0.0.1", port: plain.port, tls: { rejectUnauthorized: false } }, {
    connect(conn, err) { tlsOver(err || "connected to a non-TLS peer"); },
    close() { tlsOver("closed without a connect report"); },
});

// 203.0.113.0/24 is TEST-NET-3 (RFC 5737): unroutable, so the connect either
// times out at 300 ms or is refused at once by a host with no route. Both are
// an error report followed by a release.
function timeoutCase() {
    let reported = false;
    globalThis.keptSlow = TCPServer.connect({ host: "203.0.113.1", port: 9, connectTimeoutMs: 300 }, {
        connect(conn, err) {
            if (reported) return;
            reported = true;
            check(typeof err === "string" && err.length > 0, "the failed connect reached the handler (" + err + ")");
            clearTimeout(watchdog);
            print("test_net_client_fail_exit: " + passed + " passed, " + failed + " failed");
            if (failed) globalThis.std.exit(1);
        },
    });
}
