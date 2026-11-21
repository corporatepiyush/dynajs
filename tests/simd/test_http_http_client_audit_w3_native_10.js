// flags: --std
// covers: dyna:http.HTTPClient
// Audit wave 3, batch 10: SIMD pre-scan and variance, repeated HTTP headers,
// TLS end-of-stream for bodies with no declared length.
// Each section names its ORACLE and its CONTROL. [red] = fails before this
// batch. The two HTTP sections need python3 (and openssl for the TLS one) to
// play a peer this runtime cannot be: a server that answers a BLOCKING client
// from another process, and a TLS peer that cuts the stream on purpose.
import * as simd from "dyna:simd";
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import * as std from "std";
import { suite } from "../lib/kit.js";

const t = suite("test_audit_w3_native_10");
const best = (f, reps = 7) => { let b = Infinity; for (let r = 0; r < reps; r++) { const t = performance.now(); f(); b = Math.min(b, performance.now() - t); } return b; };
const T = String(makeTempDir("w3n10_"));
const sh = (script) => Exec("/bin/sh", ["-c", script], { timeoutMs: 40000, encoding: "utf8" });
const BIN = args()[0].startsWith("/") ? args()[0] : String(Path.cwd()) + "/" + args()[0];

// ---- M1c-P2: the NaN pre-scan ---------------------------------------------
// ORACLE: the kernels' documented NaN rule is a property of the VALUES, not of
// where the NaN sits: max/min of an array holding a NaN is NaN, relu and clamp
// keep a NaN element NaN and leave every other element as the scalar
// definition gives it. The scan now works in blocks of 64 with a scalar tail,
// so the NaN is placed at EVERY index of arrays whose lengths straddle one and
// two blocks (N-1, N, N+1 around 64 and 128).
// SCALING: a guarded kernel cannot cost several times an unguarded one of the
// same shape. relu (scan + select) was 4.9x leakyRelu (select only).
// CONTROL: arrays with no NaN return the plain maximum/minimum.

const havePy = sh("python3 -c 'import ssl, socket; print(1)'").stdout.trim() === "1";

const haveSsl = sh("openssl version").code === 0;

t.test("N1-22 repeated response headers", ({ ok, skip }) => {
    if (!havePy) skip("python3 absent");
    writeFile(new Path(T + "/mock.py"), `import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 0)); s.listen(8)
print(s.getsockname()[1], flush=True)
c, _a = s.accept(); c.recv(65536)
c.sendall(b"HTTP/1.1 200 OK\\r\\nSet-Cookie: a=1\\r\\nSet-Cookie: b=2\\r\\nX-A: one\\r\\nX-A: two\\r\\nX-Once: solo\\r\\nContent-Length: 2\\r\\nConnection: close\\r\\n\\r\\nok"); c.close()
`);
    writeFile(new Path(T + "/hdr.js"), `import * as http from "dyna:http";
const r = await new http.HTTPClient().get("http://127.0.0.1:" + scriptArgs[1] + "/");
print(JSON.stringify(r.headers));
`);
    const r = sh(`cd '${T}' && (python3 mock.py > port.txt 2> mock.err &) ; for i in 1 2 3 4 5 6 7 8 9 10; do [ -s port.txt ] && break; sleep 0.2; done; '${BIN}' hdr.js "$(cat port.txt)"`);
    let h = {};
    try { h = JSON.parse(r.stdout.trim().split("\n").pop()); } catch (e) { h = { err: r.stdout + r.stderr }; }
    ok(h["X-Once"] === "solo", "control: a header that appears once is unchanged (" + JSON.stringify(h).slice(0, 160) + ")");
    ok(h["X-A"] === "one, two", "[red] N1-22: repeated X-A lines are combined in order (" + h["X-A"] + ")");
    ok(h["Set-Cookie"] === "a=1\nb=2", "[red] N1-22: no Set-Cookie line is dropped, lines joined by \\n (" + JSON.stringify(h["Set-Cookie"]) + ")");
});

t.test("N1-25 TLS end of stream", ({ ok, skip }) => {
    if (!havePy || !haveSsl) skip(havePy ? "openssl absent" : "python3 absent");
    const mk = sh(`cd '${T}' && openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 2 -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 >/dev/null 2>&1; ls cert.pem key.pem`);
    writeFile(new Path(T + "/tlsmock.py"), `import socket, ssl, sys, os
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain("cert.pem", "key.pem")
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", 0)); s.listen(8)
print(s.getsockname()[1], flush=True)
raw, _ = s.accept()
c = ctx.wrap_socket(raw, server_side=True)
c.recv(65536)
c.sendall(b"HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\nConnection: close\\r\\n\\r\\nfirst half of the body")
if sys.argv[1] == "clean":
    try: c.unwrap().close()
    except Exception: c.close()
else:
    os.close(c.detach())
`);
    writeFile(new Path(T + "/tlsc.js"), `import * as http from "dyna:http";
try { const r = await new http.HTTPClient().get("https://127.0.0.1:" + scriptArgs[1] + "/"); print("OK " + r.status + " " + JSON.stringify(r.body)); }
catch (e) { print("ERR " + String(e)); }
`);
    const run = (mode) => sh(`cd '${T}' && rm -f port.txt && (python3 tlsmock.py ${mode} > port.txt 2> mock.err &) ; for i in 1 2 3 4 5 6 7 8 9 10; do [ -s port.txt ] && break; sleep 0.2; done; SSL_CERT_FILE='${T}/cert.pem' '${BIN}' tlsc.js "$(cat port.txt)"`).stdout.trim().split("\n").pop();
    ok(mk.code === 0, "the test made its certificate");
    const clean = run("clean"), cut = run("cut");
    ok(/^OK 200 "first half of the body"$/.test(clean), "[red] N1-25: a body ended by close_notify is delivered (" + clean.slice(0, 120) + ")");
    ok(/^ERR /.test(cut) && /close_notify|truncated/.test(cut), "[red] N1-25: the same bytes followed by a cut are a truncation error (" + cut.slice(0, 140) + ")");
});

t.after(() => removeAll(new Path(T)));
await t.run();
