// flags: --std
// A response that takes LONGER than idleTimeoutMs to download must complete as
// long as the peer keeps reading: bytes the peer accepts are progress. Before
// this was pinned, both servers cut a steady 2 MB/s download at the idle
// timeout (default 30 s -- every download slower than that was truncated).
// The defence the timeout exists for is pinned beside it: a peer that asks for
// the same body and then reads NOTHING is still disconnected, and so is one that
// drips 8 KiB/s: the kernel's send and receive buffers (about 1 MB on loopback)
// release progress to the server in large chunks, so the dripper's writes never
// reach it within the idle period.
// The reader is throttled by its READ PACE, never by a tiny SO_RCVBUF: a receive
// buffer below one loopback segment (64 KiB on Linux) puts TCP into zero-window
// probing and stalls for seconds with both servers correct.
import * as std from "std";
import * as os from "os";
import { Path, makeTempDir, removeAll, writeFile, readFile, exists } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };

const T = String(makeTempDir("slowdl"));
// 16 MiB: more than the kernel's send buffer plus the client's receive buffer
// can absorb, so the server really holds a queue for longer than the timeout.
const SIZE = 16 * 1024 * 1024;
writeFile(new Path(T + "/srv.js"), `
import { HTTPServerAsync } from "dyna:net";
import { App } from "dyna:http";
import * as std from "std";
import * as os from "os";
const BODY = "p".repeat(${SIZE});
const s = new HTTPServerAsync({ port: 0, idleTimeoutMs: 1000,
    routes: { "/b": { status: 200, contentType: "text/plain", body: BODY } } });
s.start();
const app = new App({ port: 0, idleTimeoutMs: 1000 });
app.get("/b", () => BODY);
app.start();
const f = std.open("${T}/ports.part", "w"); f.puts(s.port + " " + app.port); f.close();
os.rename("${T}/ports.part", "${T}/ports");
`);
writeFile(new Path(T + "/cli.py"), `
import socket, sys, time, json
ports = open("${T}/ports").read().split()
want = ${SIZE}
R = {}
REQ = b"GET /b HTTP/1.1\\r\\nHost: a\\r\\n\\r\\n"
for name, port in zip(("async", "app"), ports):
    s = socket.create_connection(("127.0.0.1", int(port)))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
    s.sendall(REQ)
    got = 0; t0 = time.time()
    s.settimeout(10)
    try:
        while got < want:
            d = s.recv(131072)
            if not d: break
            got += len(d); time.sleep(0.03)
    except Exception: pass
    R[name + "_steady"] = {"got": got, "secs": round(time.time() - t0, 2)}
    s.close()
    s = socket.create_connection(("127.0.0.1", int(port)))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
    s.sendall(REQ)
    time.sleep(4.0)
    got = 0; eof = False
    s.settimeout(3)
    try:
        while True:
            d = s.recv(65536)
            if not d: eof = True; break
            got += len(d)
    except Exception: pass
    R[name + "_silent"] = {"got": got, "eof": eof}
    s.close()
    s = socket.create_connection(("127.0.0.1", int(port)))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
    s.sendall(REQ)
    got = 0; eof = False; t0 = time.time()
    s.settimeout(3)
    try:
        while time.time() - t0 < 5.0:
            d = s.recv(2048)
            if not d: eof = True; break
            got += len(d); time.sleep(0.25)
        while not eof:
            d = s.recv(1048576)
            if not d: eof = True; break
            got += len(d)
    except Exception: pass
    R[name + "_drip"] = {"got": got, "eof": eof}
    s.close()
open("${T}/out.part", "w").write(json.dumps(R))
import os as _os
_os.rename("${T}/out.part", "${T}/out")
`);

const srv = os.exec(["./dynajs", "--std", T + "/srv.js"], { block: false });
let up = false;
for (let i = 0; i < 200 && !up; i++) { up = exists(new Path(T + "/ports")); if (!up) os.sleep(25); }
ok(up, "the server child published its ports");
if (up) {
    os.exec(["python3", T + "/cli.py"]);
    let R = {};
    try { R = JSON.parse(readFile(new Path(T + "/out"))); } catch (e) { ok(false, "the client wrote no result: " + e.message); }
    for (const name of ["async", "app"]) {
        const st = R[name + "_steady"] || {}, si = R[name + "_silent"] || {};
        ok(st.got >= SIZE, name + ": a steady reader gets the whole body though it takes longer than idleTimeoutMs (got " +
           st.got + " of " + SIZE + " in " + st.secs + "s)");
        ok(st.secs > 1.5, name + ": the download really outlasted the 1s idle timeout (" + st.secs + "s)");
        ok(si.eof === true && si.got < SIZE, name + ": a peer that reads nothing is still disconnected (eof=" +
           si.eof + ", got " + si.got + ")");
        const dr = R[name + "_drip"] || {};
        ok(dr.eof === true && dr.got < SIZE, name + ": a peer that drips 8 KiB/s is disconnected (eof=" +
           dr.eof + ", got " + dr.got + ")");
    }
}
os.kill(srv, 9);
os.waitpid(srv, 0);
removeAll(new Path(T));
print("test_http_slow_download: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_http_slow_download failed");
