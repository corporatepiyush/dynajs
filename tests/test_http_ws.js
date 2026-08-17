/* test_http_ws.js -- the WebSocket frame path: unmasking, the control-frame
 * budget and the fragment-count cap.
 *
 * None of this had any coverage. The unmask was changed from a byte loop to an
 * 8-byte one, and a masking bug produces GARBAGE rather than a crash, so a test
 * that only checks "the server stayed up" would pass against a broken engine.
 * Every payload here is therefore compared byte for byte, and the sizes
 * straddle the 8-byte block (a length that is a multiple of 8 never exercises
 * the tail loop).
 *
 * THE SERVER RUNS IN A CHILD PROCESS for the reason test_http_security.js
 * documents: App handlers run on the JS thread, so an in-process client
 * deadlocks and every assertion passes vacuously.
 *
 * Skips cleanly without python3, which is only used as a raw socket client --
 * curl cannot speak WebSocket.
 */
import * as os from "os";
import * as std from "std";

const PORT = 18311;
const T = "/tmp/_dyna_ws";
let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL: " + w); } };
const sh = (c) => os.exec(["/bin/sh", "-c", c], { usePath: true });
const cat = (p) => { const f = std.open(p, "r"); if (!f) return ""; const s = f.readAsString(); f.close(); return s; };

if (sh("command -v python3 >/dev/null 2>&1") !== 0) {
    print("test_http_ws: SKIP (python3 not available)");
} else {
    sh(`rm -rf ${T}; mkdir -p ${T}`);
    const srv = std.open(`${T}/srv.js`, "w");
    srv.puts(`import { App } from "dyna:net";
const app = new App({ port: ${PORT} });
app.ws("/ws", { open: () => {}, message: (c, d, bin) => c.send(d, bin), close: () => {} });
app.start();
`);
    srv.close();
    const exe = os.realpath ? (os.realpath("./dynajs")[0] || "./dynajs") : "./dynajs";
    sh(`${exe} ${T}/srv.js >${T}/srv.log 2>&1 & sleep 1`);

    const py = std.open(`${T}/c.py`, "w");
    py.puts(`import socket, os, base64, struct, sys, hashlib
HOST, PORT = "127.0.0.1", ${PORT}
def hs(s):
    k = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\\r\\nHost: h\\r\\nUpgrade: websocket\\r\\n"
               "Connection: Upgrade\\r\\nSec-WebSocket-Key: %s\\r\\n"
               "Sec-WebSocket-Version: 13\\r\\n\\r\\n" % k).encode())
    buf = b""
    while b"\\r\\n\\r\\n" not in buf:
        d = s.recv(4096)
        if not d: raise SystemExit("handshake closed")
        buf += d
    return buf
def frame(op, payload, fin=True):
    b0 = (0x80 if fin else 0) | op
    m = os.urandom(4)
    p = bytes(payload[i] ^ m[i & 3] for i in range(len(payload)))
    n = len(payload)
    if n < 126:   h = struct.pack("!BB", b0, 0x80 | n)
    elif n < 65536: h = struct.pack("!BBH", b0, 0x80 | 126, n)
    else:         h = struct.pack("!BBQ", b0, 0x80 | 127, n)
    return h + m + p
def readframe(s):
    def rd(n):
        b = b""
        while len(b) < n:
            d = s.recv(n - len(b))
            if not d: return None
            b += d
        return b
    h = rd(2)
    if h is None: return None, None
    op = h[0] & 0x0f; ln = h[1] & 0x7f
    if ln == 126: ln = struct.unpack("!H", rd(2))[0]
    elif ln == 127: ln = struct.unpack("!Q", rd(8))[0]
    return op, (rd(ln) if ln else b"")
mode = sys.argv[1]
s = socket.create_connection((HOST, PORT), 5); s.settimeout(5); hs(s)
if mode == "echo":
    okall = True
    for n in (1, 7, 8, 9, 63, 64, 65, 1000, 100003):
        payload = bytes((i * 37 + n) & 0xff for i in range(n))
        s.sendall(frame(0x2, payload))
        op, got = readframe(s)
        if got != payload:
            print("MISMATCH len=%d" % n); okall = False; break
    print("ECHO_OK" if okall else "ECHO_BAD")
def drain(s, deadline=3.0):
    """Count frames the server sends back, then stop. Returns (count, eof).

    ASSERT THE DEFENCE, NOT THE TRANSPORT. An earlier version required EOF and
    passed on macOS while failing on Linux -- where closing a socket that still
    has unread data sends RST rather than FIN, so a pure reader may simply time
    out. The server was correct in both cases: it answered exactly
    DYN_WS_CTL_BUDGET pings and stopped. What matters is that the work is
    bounded, which is portable; when the peer notices is not."""
    import time
    end = time.time() + deadline
    n = 0
    s.settimeout(0.5)
    while time.time() < end:
        try:
            op, d = readframe(s)
            if op is None: return n, True
            n += 1
        except socket.timeout: return n, False
        except OSError: return n, True
    return n, False
if mode == "pingflood":
    SENT = 500
    try:
        for i in range(SENT): s.sendall(frame(0x9, b"p"))
    except (BrokenPipeError, ConnectionResetError, OSError): pass
    pongs, eof = drain(s)
    # bounded work is the property: far fewer pongs than pings, and the server
    # must not have answered every one of them.
    print("PING_BOUNDED %d" % pongs if pongs < SENT // 2 else "PING_UNBOUNDED %d" % pongs)
elif mode == "bigping":
    # RFC 6455 5.5: a control frame carries at most 125 octets. Without that
    # rule the pong echoes the ping, so this is 200 KiB in and 200 KiB back.
    try:
        s.sendall(frame(0x9, b"P" * 200000))
    except (BrokenPipeError, ConnectionResetError, OSError): pass
    pongs, eof = drain(s, 2.0)
    print("BIGPING_REFUSED" if pongs == 0 else "BIGPING_ECHOED %d" % pongs)
elif mode == "smallping":
    # and a legal one is still answered, so the refusal is not blanket
    try:
        s.sendall(frame(0x9, b"P" * 125))
    except (BrokenPipeError, ConnectionResetError, OSError): pass
    pongs, eof = drain(s, 2.0)
    print("SMALLPING_OK" if pongs >= 1 else "SMALLPING_DROPPED")
elif mode == "fragctl":
    # a control frame is never fragmented; FIN=0 on a ping must fail the
    # connection rather than be treated as the start of a message
    try:
        s.sendall(frame(0x9, b"p", fin=False))
        s.sendall(frame(0x2, b"after"))
    except (BrokenPipeError, ConnectionResetError, OSError): pass
    n, eof = drain(s, 2.0)
    print("FRAGCTL_REFUSED" if n == 0 else "FRAGCTL_ACCEPTED %d" % n)
elif mode == "fragflood":
    try:
        s.sendall(frame(0x2, b"a", fin=False))
        for i in range(20000): s.sendall(frame(0x0, b"a", fin=False))
        s.sendall(frame(0x0, b"a", fin=True))     # ask for delivery
    except (BrokenPipeError, ConnectionResetError, OSError): pass
    echoes, eof = drain(s)
    # the over-long message must never be delivered back
    print("FRAG_REFUSED" if echoes == 0 else "FRAG_DELIVERED %d" % echoes)
`);
    py.close();

    sh(`python3 ${T}/c.py echo > ${T}/echo.out 2>&1; true`);
    const echo = cat(`${T}/echo.out`).trim();
    ok(echo.indexOf("ECHO_OK") >= 0, "masked payloads echo byte-for-byte (got " + echo + ")");

    /* RFC 6455 5.5. Without the length rule a ping is a reflector: the payload
       comes back in the pong, so the control budget bounds the COUNT and
       nothing bounds the bytes. */
    sh(`python3 ${T}/c.py bigping > ${T}/bp.out 2>&1; true`);
    const bp = cat(`${T}/bp.out`).trim();
    ok(bp.indexOf("BIGPING_REFUSED") >= 0,
       "a control frame over 125 octets fails the connection rather than being " +
       "echoed back (got " + bp + ")");

    sh(`python3 ${T}/c.py smallping > ${T}/sp.out 2>&1; true`);
    const sp = cat(`${T}/sp.out`).trim();
    ok(sp.indexOf("SMALLPING_OK") >= 0,
       "and a 125-octet ping is still answered, so the refusal is not blanket " +
       "(got " + sp + ")");

    sh(`python3 ${T}/c.py fragctl > ${T}/fc.out 2>&1; true`);
    const fc = cat(`${T}/fc.out`).trim();
    ok(fc.indexOf("FRAGCTL_REFUSED") >= 0,
       "a fragmented control frame fails the connection (got " + fc + ")");

    sh(`python3 ${T}/c.py pingflood > ${T}/ping.out 2>&1; true`);
    const ping = cat(`${T}/ping.out`).trim();
    ok(ping.indexOf("PING_BOUNDED") >= 0, "ping flood answered a bounded number of times (got " + ping + ")");

    sh(`python3 ${T}/c.py fragflood > ${T}/frag.out 2>&1; true`);
    const frag = cat(`${T}/frag.out`).trim();
    ok(frag.indexOf("FRAG_REFUSED") >= 0, "over-long fragment chain is never delivered (got " + frag + ")");

    sh(`pkill -f "${T}/srv.js" 2>/dev/null; rm -rf ${T}`);
    print("test_http_ws: " + pass + " passed, " + fail + " failed");
    if (fail) throw new Error("test_http_ws: " + fail + " failures");
}
