// flags: --std
// Audit wave 3, batch 8: timers, reactor, Redis subscriptions, os module,
// copyFile, log files, bench, async queues, persisted ML models.
// Each section names its ORACLE (why the expected value is right) and its
// CONTROL (a case that must behave the same before and after the fix).
// Cases marked [red] fail on the build before this batch.
import * as os from "os";
import * as http from "dyna:http";
import * as ml from "dyna:ml";
import { Redis, TCPServer } from "dyna:net";
import { Path, makeTempDir, makeDir, remove, writeFile, removeAll, copyFile, exists, Watcher } from "dyna:file";
import { Logger } from "dyna:log";
import { bench } from "dyna:bench";
import { Semaphore } from "dyna:async";
import { args, Exec } from "dyna:sys";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;

// ---- C1-11: JS timers ---------------------------------------------------
// ORACLE: a timer queue is a priority queue; set and clear are O(log n), so
// the cost per operation cannot grow tenfold when n grows tenfold. The old
// sorted list was linear per operation (measured 2.7 us at 2k, 28 us at 20k).
// CONTROL: firing order is by deadline, ties in creation order; a cleared
// timer never fires.
{
    const per = (n) => {
        const ids = [];
        const t = performance.now();
        for (let i = 0; i < n; i++) ids.push(setTimeout(() => {}, 200000 - i));
        for (const i of ids) clearTimeout(i);
        return (performance.now() - t) / n;
    };
    per(500);
    const small = per(2000), big = per(20000);
    ok(big < small * 4, "[red] C1-11: timer set+clear cost per op is flat (" + (small * 1e6).toFixed(0) + " ns at 2k, " + (big * 1e6).toFixed(0) + " ns at 20k)");

    const order = [];
    const delays = [7, 3, 9, 3, 1, 5, 9, 1, 4];
    delays.forEach((d, i) => setTimeout(() => order.push(d * 100 + i), d));
    const dead = setTimeout(() => order.push(-1), 2);
    clearTimeout(dead);
    await sleep(40);
    const sorted = order.slice().sort((a, b) => a - b);
    ok(order.length === delays.length && order.join() === sorted.join(), "control: timers fire by deadline, ties in creation order (" + order.join() + ")");
    ok(!order.includes(-1), "control: a cleared timer never fires");
}

// ORACLE: the live-timer cap counts LIVE timers. An interval that fires is
// still one timer, so firing must not consume the cap. The old requeue path
// incremented the count on every firing and never decremented it.
// CONTROL: setTimeout works before the firings.
{
    let n = 0;
    ok(kind(() => clearTimeout(setTimeout(() => {}, 0))) === "none", "control: setTimeout works");
    const ids = [];
    for (let k = 0; k < 500; k++) ids.push(setInterval(() => { n++; }, 1));
    const t0 = Date.now();
    while (n < 101000 && Date.now() - t0 < 20000) await sleep(5);
    const r = kind(() => clearTimeout(setTimeout(() => {}, 0)));
    for (const i of ids) clearInterval(i);
    ok(n >= 101000, "the intervals fired more than the 100000 cap (" + n + ")");
    ok(r === "none", "[red] C1-11: interval firings do not consume the live-timer cap (" + r + ")");
}

// ---- C1-18 / C1-01: a burst of pooled exchanges neither blocks the caller
// nor starves timers ---------------------------------------------------------
// ORACLE: a pending request promises not to block its caller, and a loop that
// serves two sources gives each a turn. 48 exchanges of 8 MiB each go to the
// worker pool at once: more than it holds, and each completion costs
// milliseconds on the loop thread. So (1) issuing them returns at once -- the
// overflow used to run whole exchanges inside the call, 69 ms with no timer
// able to fire -- and (2) a 5 ms interval ticks between the first completion
// and the last; before, completions were delivered back to back (0 ticks).
// The peer is a python server in another process, so nothing here depends on
// this loop serving its own requests.
// CONTROL: every exchange resolves with the whole body.
if (Exec("/bin/sh", ["-c", "python3 -c 'import http.server, socketserver'"], { timeoutMs: 20000 }).code !== 0) {
    console.log("SKIP (python3 absent): C1-18 / C1-01 pooled exchange burst");
} else {
    writeFile(new Path(tp("big.py")), `import http.server, socketserver
BODY = b"x" * (8 << 20)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_GET(self):
        self.send_response(200); self.send_header("Content-Length", str(len(BODY))); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(BODY)
    def log_message(self, *a): pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True; request_queue_size = 128
s = S(("127.0.0.1", 0), H); print(s.server_address[1], flush=True); s.serve_forever()
`);
    writeFile(new Path(tp("burst.mjs")), `import { HTTPClient } from "dyna:net";
const port = scriptArgs[1], N = 48;
const pend = (c, u) => (c.getAsync ? c.getAsync(u) : c[Symbol.for("dyna.http.request")]("GET", u));
let ticks = 0, first = -1, last = -1, done = 0, bytes = 0;
const iv = setInterval(() => { ticks++; }, 5);
const cs = [], ps = [], t0 = Date.now();
for (let i = 0; i < N; i++) { const c = new HTTPClient(); cs.push(c); ps.push(pend(c, "http://127.0.0.1:" + port + "/").then((r) => { if (first < 0) first = ticks; last = ticks; done++; bytes += r.body.length; })); }
const issued = Date.now() - t0;
await Promise.all(ps);
clearInterval(iv);
for (const c of cs) c.close();
console.log(JSON.stringify({ done, bytes, between: last - first, issued }));
`);
    const BIN8 = args()[0].startsWith("/") ? args()[0] : String(Path.cwd()) + "/" + args()[0];
    const r = Exec("/bin/sh", ["-c", `cd '${tp("")}' && (python3 big.py > port.txt 2> py.err & echo $! > py.pid); for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do [ -s port.txt ] && break; sleep 0.2; done; '${BIN8}' burst.mjs "$(cat port.txt)"; rc=$?; kill "$(cat py.pid)" 2>/dev/null; exit $rc`], { timeoutMs: 90000, encoding: "utf8" });
    let o = {};
    try { o = JSON.parse(r.stdout.trim().split("\n").pop()); } catch (e) { o = { err: (r.stdout + r.stderr).slice(0, 200) }; }
    ok(o.done === 48 && o.bytes === 48 * (8 << 20), "control: all 48 pooled exchanges resolved in full (" + JSON.stringify(o).slice(0, 160) + ")");
    ok(o.issued < 30, "[red] C1-01: issuing more exchanges than the pool holds does not run them inside the call (" + o.issued + " ms)");
    ok(o.between >= 2, "[red] C1-18: interval ticks land between completions (" + o.between + " ticks between the first and the last)");
}

// ---- C1-22: dyna:async queues -------------------------------------------
// ORACLE: handing a permit to the next waiter is a queue pop; its cost cannot
// depend on how many waiters remain. Array.prototype.shift is linear in this
// engine (1.4 us at 10k elements, 15 us at 100k), which made it so.
// CONTROL: waiters are served in FIFO order.
{
    const drain = async (n) => {
        const s = new Semaphore(1);
        const rel = [await s.acquire()];
        const seen = [];
        for (let i = 0; i < n; i++) s.acquire().then((r) => { seen.push(i); rel.push(r); });
        const t = performance.now();
        for (let i = 0; i <= n; i++) { rel[i](); await null; await null; }
        return { per: (performance.now() - t) / n, seen };
    };
    await drain(500);
    const a = await drain(2000), b = await drain(40000);
    ok(b.per < a.per * 4, "C1-22: semaphore hand-off cost is flat (" + (a.per * 1e6).toFixed(0) + " ns at 2k, " + (b.per * 1e6).toFixed(0) + " ns at 40k)");
    ok(a.seen.length === 2000 && a.seen.every((v, i) => v === i), "control: waiters are served first come, first served");
}

// ---- P1-21: os module ---------------------------------------------------
// ORACLE: kill(2): pid 0 is the caller's whole process group and pid -1 is
// every process the caller may signal; neither is ever what a script means.
// A C string ends at its first NUL, so an argument containing one would be
// silently shortened. POSIX: a new program starts with default dispositions
// unless the parent ignored a signal, and this runtime ignores SIGPIPE.
// CONTROL: signal 0 to our own pid succeeds; a plain exec returns 0.
{
    ok(os.kill(os.getpid(), 0) === 0, "control: os.kill(self, 0) succeeds");
    ok(kind(() => os.kill(-1, 0)) === "RangeError", "[red] P1-21: os.kill(-1, sig) is refused");
    ok(kind(() => os.kill(0, 0)) === "RangeError", "[red] P1-21: os.kill(0, sig) is refused");
    ok(kind(() => os.kill(os.getpid(), 4096)) === "RangeError", "[red] P1-21: an out-of-range signal number is refused");
    ok(os.exec(["/bin/sh", "-c", "exit 0"]) === 0, "control: os.exec runs a command");
    ok(kind(() => os.exec(["/bin/sh", "-c", "exit 0\0 ignored"])) === "TypeError", "[red] P1-21: os.exec refuses an argument containing NUL");
    ok(kind(() => os.exec(["/bin/sh", "-c", "exit 0"], { cwd: "/tmp\0x" })) === "TypeError", "[red] P1-21: os.exec refuses a cwd containing NUL");
    const rc = os.exec(["/bin/sh", "-c", "kill -PIPE $$; exit 0"]);
    ok(rc === -os.SIGPIPE, "[red] P1-21: an os.exec child has the default SIGPIPE disposition (" + rc + ")");
}

// ---- P1-28: copyFile ----------------------------------------------------
// ORACLE: copyFile copies a regular file. A device has no length: /dev/zero
// would be copied until the disk is full, and a FIFO blocks the thread.
// CONTROL: a regular file is copied byte for byte.
{
    writeFile(new Path(tp("src.txt")), "hello copy");
    copyFile(new Path(tp("src.txt")), new Path(tp("dst.txt")));
    ok(exists(new Path(tp("dst.txt"))), "control: copyFile copies a regular file");
    const k = kind(() => copyFile(new Path("/dev/null"), new Path(tp("devnull.out"))));
    ok(k === "TypeError", "[red] P1-28: copyFile refuses a device as its source (" + k + ")");
    ok(!exists(new Path(tp("devnull.out"))), "[red] P1-28: and creates no destination for it");
}

// ---- P1-18: Watcher releases what it no longer watches -------------------
// ORACLE: a watcher holds one descriptor per watched entry on kqueue. An
// entry that has been deleted has nothing left to watch, so its descriptor
// must be released; otherwise a directory with file churn climbs to the cap
// and then silently stops arming new files. On inotify the count is
// directories only and is unaffected.
// CONTROL: the watcher reports both the creations and the deletions.
{
    const root = tp("watched");
    makeDir(new Path(root), { recursive: true });
    const w = new Watcher(new Path(root), { debounceMs: 20 });
    let events = 0;
    w.start(() => { events++; });
    const base = w.stats().directories;
    for (let i = 0; i < 40; i++) writeFile(new Path(root + "/f" + i), "x");
    await sleep(250);
    const peak = w.stats().directories;
    for (let i = 0; i < 40; i++) remove(new Path(root + "/f" + i));
    await sleep(250);
    const after = w.stats().directories;
    ok(events >= 80, "control: the watcher reported the creations and deletions (" + events + ")");
    ok(after === base, "[red] P1-18: descriptors of deleted entries are released (" + base + " -> " + peak + " -> " + after + ")");
    if (w.stop) w.stop();
    if (w.close) w.close();
}

// ---- P1-16: log file mode -----------------------------------------------
// ORACLE: application logs carry request data; a file created for them must
// not be readable by every local user. 0640 keeps the group (a log shipper).
// CONTROL: the line is written.
{
    const lp = tp("app.log");
    const L = new Logger({ dest: lp });
    L.info("mode probe");
    if (L.flush) L.flush();
    const [st, err] = os.stat(lp);
    ok(err === 0 && st.size > 0, "control: the logger wrote its line");
    ok(err === 0 && (st.mode & 0o007) === 0, "[red] P1-16: a new log file is not world-readable (mode " + (st.mode & 0o777).toString(8) + ")");
    if (L.close) L.close();
}

// ---- P1-24: bench -------------------------------------------------------
// ORACLE: a timing shorter than the clock's tick is not a measurement. With
// one clock pair per call, half the samples of an empty body are exactly 0,
// so the median is 0. Batching until a sample spans 0.1 ms makes the median a
// positive time per call.
// CONTROL: a body of about a millisecond is reported near a millisecond.
{
    const r = bench("empty", () => {}, { timeMs: 120, warmupMs: 10 });
    ok(r.p50Ms > 0, "[red] P1-24: the median of an empty body is a positive time, not a clock artefact (" + r.p50Ms + ")");
    ok(r.iters > 1000 && r.opsPerSec > 1000, "control: the empty body ran many times (" + r.iters + ")");
    const slow = bench("slow", () => { const e = performance.now() + 1; while (performance.now() < e); }, { timeMs: 60, warmupMs: 5 });
    ok(slow.p50Ms > 0.9 && slow.p50Ms < 5, "control: a 1 ms body is reported as about 1 ms (" + slow.p50Ms.toFixed(3) + ")");
}

// ---- N1-22: App registration methods chain ------------------------------
// ORACLE: dynajs.d.ts declares rpc/ws/sse as returning `this`.
// CONTROL: get() chains.
{
    const a = new http.App();
    ok(a.get("/a", () => "x") === a, "control: App.get returns the app");
    ok(a.rpc("/r", { f() { return 1; } }) === a, "[red] N1-22: App.rpc returns the app");
    ok(a.sse("/e", () => {}) === a, "[red] N1-22: App.sse returns the app");
    ok(a.ws("/w", { message() {} }) === a, "[red] N1-22: App.ws returns the app");
}

// ---- N1-20: App.proxy ----------------------------------------------------
// ORACLE: RFC 9112 3.2: a request to an origin server uses origin-form, a
// target that starts with "/". A route prefix ending in "/" used to leave the
// remainder "http://evil.example/x" as the upstream target, which is
// absolute-form and names another authority. And an upstream exchange exists
// only for its client: when the client connection goes away (here: the idle
// sweep), the upstream socket must be closed with it, not left open until the
// upstream chooses to speak.
// CONTROL: the upstream received exactly one request.
{
    let upConns = 0, upClosed = 0, upReq = "";
    const silent = new TCPServer({ port: 0 });
    silent.start({ connect: () => { upConns++; }, data: (c, b) => { upReq += new TextDecoder().decode(b); }, close: () => { upClosed++; } });
    const app = new http.App({ port: 0, idleTimeoutMs: 400 });
    app.proxy("/api/", { host: "127.0.0.1", port: silent.port });
    app.start();
    let clientClosed = false;
    const cl = TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
        connect: (c) => c.write("GET /api/http://evil.example/x HTTP/1.1\r\nHost: a\r\n\r\n"),
        close: () => { clientClosed = true; },
    });
    const t0 = Date.now();
    while (!(clientClosed && upClosed) && Date.now() - t0 < 6000) await sleep(50);
    const line = upReq.split("\r\n")[0];
    ok(upConns === 1 && /^GET \S+ HTTP\/1\.1$/.test(line), "control: the upstream received one request (" + line + ")");
    ok(line.split(" ")[1].startsWith("/"), "[red] N1-20: the upstream target is origin-form (" + line + ")");
    ok(clientClosed, "control: the idle sweep closed the waiting client");
    ok(upClosed === 1, "[red] N1-20: the upstream connection is closed with its client (" + upClosed + ")");
    cl.close(); app.close(); silent.close();
}

// ---- N3-11 / N3-07: Redis -----------------------------------------------
// ORACLE (options): an option is sent to the server as a C string; a NUL
// would silently shorten a password or a host.
// ORACLE (subscriptions): on RESP2 a subscribed connection receives
// ["message", channel, payload] arrays that are not replies. Whether the
// connection is subscribed is the SERVER's state; the confirmation's third
// element is its count. UNSUBSCRIBE of a channel that was never subscribed
// leaves the count unchanged, so a later message must still be routed to the
// push handler and must never resolve a command.
// ORACLE (pipeline): a subscribe verb has one confirmation per channel and on
// RESP3 they arrive as pushes, so a pipeline cannot match them positionally.
// CONTROL: PING resolves with PONG; the mock counts what reaches the wire.
{
    ok(kind(() => new Redis({ host: "127.0.0.1", port: 1, password: "a\0b" })) === "TypeError", "[red] N3-11: a Redis option containing NUL is refused");

    const wire = [];
    const srv = new TCPServer({ port: 0 });
    srv.start({
        data: (conn, bytes) => {
            const text = new TextDecoder().decode(bytes);
            const cmds = text.split(/(?=\*\d+\r\n)/).filter((c) => c.length);
            for (const c of cmds) {
                const parts = c.split("\r\n").filter((l, i, a) => i > 0 && a[i - 1].startsWith("$"));
                const verb = (parts[0] || "").toUpperCase();
                wire.push(verb);
                if (verb === "HELLO") conn.write("-ERR unknown command 'HELLO'\r\n");
                else if (verb === "SUBSCRIBE") conn.write("*3\r\n$9\r\nsubscribe\r\n$1\r\na\r\n:1\r\n");
                else if (verb === "UNSUBSCRIBE") conn.write("*3\r\n$11\r\nunsubscribe\r\n$1\r\nb\r\n:1\r\n");
                else if (verb === "PING") conn.write("*3\r\n$7\r\nmessage\r\n$1\r\na\r\n$1\r\nx\r\n+PONG\r\n");
                else conn.write("+OK\r\n");
            }
        },
    });
    const r = new Redis({ host: "127.0.0.1", port: srv.port, commandTimeoutMs: 3000 });
    const pushes = [];
    r.on("push", (m) => pushes.push(m));
    r.on("error", () => {});
    let ping = "unset";
    try {
        await r.command("SUBSCRIBE", "a");
        await r.command("UNSUBSCRIBE", "b");
        ping = await r.command("PING");
    } catch (e) { ping = "threw " + e; }
    ok(ping === "PONG", "[red] N3-07: a message after a foreign UNSUBSCRIBE does not resolve the next command (" + JSON.stringify(ping) + ")");
    ok(pushes.length === 1 && String(pushes[0][0]) === "message", "[red] N3-07: it is delivered to the push handler (" + pushes.length + ")");
    const before = wire.length;
    let pk = "none";
    try { await r.pipeline([["SUBSCRIBE", "z"], ["PING"]]); } catch (e) { pk = e.constructor.name; }
    ok(pk === "TypeError" && wire.length === before, "[red] N3-07: a subscribe verb inside pipeline() is refused before anything is sent (" + pk + ")");
    r.close();
    srv.close();
}

// ---- M1b-07: persisted tree models --------------------------------------
// ORACLE: the constructor enforces subsample in (0, 1], so a record holding
// NaN there was not written by this library; and a record's family flags must
// agree with the class asked to load it, or X.deserialize returns an X that
// behaves as another family. The record is sealed with CRC-32C, so the test
// re-seals after each mutation: only the range check can refuse it.
// CONTROL: the untouched record loads and predicts as the original.
{
    const Tb = new Uint32Array(256);
    for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = (c & 1) ? (c >>> 1) ^ 0x82F63B78 : c >>> 1; Tb[i] = c >>> 0; }
    const crc = (b, n) => { let c = 0xFFFFFFFF; for (let i = 0; i < n; i++) c = Tb[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
    const reseal = (b) => new DataView(b.buffer, b.byteOffset).setUint32(b.length - 4, crc(b, b.length - 4), true);
    const X = [], y = [];
    for (let i = 0; i < 24; i++) { const row = [Math.sin(i * 1.3), Math.cos(i * 0.7), (i % 5) - 2]; X.push(row); y.push(row[0] * 2 - row[1]); }
    const m = new ml.GradientBoostingRegressor({ nEstimators: 3, subsample: 0.75 }).fit(X, y);
    const rec = new Uint8Array(m.serialize());
    const back = ml.GradientBoostingRegressor.deserialize(rec);
    ok(JSON.stringify(Array.from(back.predict(X))) === JSON.stringify(Array.from(m.predict(X))), "control: an untouched record round-trips");

    const dv = new DataView(rec.buffer, rec.byteOffset, rec.byteLength);
    let at = -1;
    for (let i = 0; i + 8 <= rec.length - 4; i++) if (dv.getFloat64(i, true) === 0.75) { at = i; break; }
    ok(at > 0, "the test located the subsample field (offset " + at + ")");
    const bad = rec.slice();
    new DataView(bad.buffer).setFloat64(at, NaN, true);
    reseal(bad);
    ok(kind(() => ml.GradientBoostingRegressor.deserialize(bad)) === "TypeError", "[red] M1b-07: a record with subsample NaN is refused");
    const neg = rec.slice();
    new DataView(neg.buffer).setFloat64(at, 7.5, true);
    reseal(neg);
    ok(kind(() => ml.GradientBoostingRegressor.deserialize(neg)) === "TypeError", "[red] M1b-07: a record with subsample 7.5 is refused");
}

removeAll(new Path(String(T)));
console.log("test_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_audit_w3_native_8: " + fail + " failures");
