// flags: --std
// timeout: 60
// covers: c:src/modules/libc/libc-wst-adapt.c
// dyna:stream stdin()/stdout()/stderr(): process stdio as web streams (v10b-19: stdin() is one ReadableStream until it closes, errors or is
// cancelled, then a fresh one; stdout()/stderr() return a new WritableStream per call; the read(buf)/write(buf) objects are internal).
// stdin waits for readiness on the shared loop (os.setReadHandler) for pipes, ttys and sockets, so a slow producer never blocks the JS
// thread; a regular file or a non-tty character device (/dev/null) is read directly, since readiness never fires for those on kqueue.
// The fds are never set O_NONBLOCK and never closed. A sink write first flushes the C stdio buffer, so std.out/print output buffered by C stdio keeps its place.
// stdin()/stdout()/stderr() run natively (the internal stdio object is the adapter, class pre-check then identity per call). Since D5 a chunk under 16 KiB is
// coalesced into the per-fd buffer and settles at once; larger chunks go out with the buffer in one writev (test_stdio_coalesce.js pins the buffering);
// EAGAIN on a large chunk parks the rest on the ByteSink's writability chain; errors stick to the sink and to the fd. Regular-file stdin is read straight into the BYOB view or a fresh buffer.
// Child: stdio_streams_child.js.
import { suite } from "../lib/kit.js";
import { Spawn } from "dyna:sys";
import * as std from "std";
import * as os from "os";

const t = suite("stdio_streams");
const T = { timeoutMs: 20000 };
const exe = scriptArgs[0].replace(/[^/]*$/, "") + "stdio_streams_child.js";
const BIN = std.getenv("DYNAJS_BIN") || "./dynajs";
async function readAll(pipe) { let s = ""; const dec = new TextDecoder(); for await (const c of pipe) s += dec.decode(c, { stream: true }); return s; }
function run(mode, opts) {
    const p = new Spawn(self(), ["--std", exe, mode], opts);
    return p;
}
function self() { return BIN; }

t.test("a piped stdin streams as the producer writes; buffered std.out output and console.log keep their order around the sink", async (a) => {
    const p = run("upper", { stdin: "pipe" });
    const out = readAll(p.stdout);
    await p.stdin.write(new TextEncoder().encode("ab"));
    await p.stdin.flush();
    await new Promise((r) => setTimeout(r, 150));
    await p.stdin.write(new TextEncoder().encode("cd"));
    await p.stdin.flush();
    p.stdin.close();
    const s = await out;
    a.eq((await p.wait()).code, 0);
    a.match(s, /^first\n\[AB\]\[CD\]\nlast\n$/, "two separate chunks, in order between the logs");
}, T);

t.test("waiting for a piped stdin does not block the JS thread", async (a) => {
    const p = run("tick", { stdin: "pipe" });
    const out = readAll(p.stdout);
    await new Promise((r) => setTimeout(r, 300));
    await p.stdin.write(new TextEncoder().encode("hey"));
    await p.stdin.flush();
    p.stdin.close();
    a.eq(await out, "read 3 ticks many\n", "timers ran while the read waited");
    await p.wait();
}, T);

t.test("a regular file and /dev/null as stdin", async (a) => {
    const dir = std.getenv("TMPDIR") || ".";
    const f = dir + "/stdio." + os.getpid() + ".txt";
    const fh = std.open(f, "w"); fh.puts("x".repeat(300000)); fh.close();
    const p = new Spawn("/bin/sh", ["-c", "\"$0\" --std \"$1\" count < \"$2\"", self(), exe, f]);
    a.eq(await readAll(p.stdout), "bytes 300000\n");
    await p.wait();
    const q = new Spawn("/bin/sh", ["-c", "\"$0\" --std \"$1\" count < /dev/null", self(), exe]);
    a.eq(await readAll(q.stdout), "bytes 0\n", "/dev/null reads EOF instead of hanging");
    await q.wait();
    os.remove(f);
}, T);

t.test("stderr sink; a closed sink refuses writes; the fd stays open", async (a) => {
    const p = run("err");
    const [o, e] = await Promise.all([readAll(p.stdout), readAll(p.stderr)]);
    await p.wait();
    a.eq(e, "to-stderr");
    a.match(o, /^late write: /);
}, T);

t.test("stdin() is one stream until cancelled; cancel settles a pending read done", async (a) => {
    const p = run("same", { stdin: "pipe" });
    const o = await readAll(p.stdout);
    p.stdin.close();
    await p.wait();
    a.eq(o, "true\nafter cancel true fresh true same true\n");
}, T);

const tmpdir = std.getenv("TMPDIR") || ".";
let seq = 0;
function tmpfile(tag) { return tmpdir + "/stdio." + os.getpid() + "." + tag + "." + (seq++); }
function bytesFile(n, seed) {
    const f = tmpfile("in"), b = new Uint8Array(n);
    let x = seed;
    for (let i = 0; i < n; i++) { x = (Math.imul(x, 1103515245) + 12345) >>> 0; b[i] = (x >>> 23) | 1; }
    const fh = std.open(f, "w"); fh.write(b.buffer, 0, n); fh.close();
    return f;
}
function sh(script, ...args) { return new Spawn("/bin/sh", ["-c", script, self(), exe, ...args]); }
const lastJson = (s) => JSON.parse(s.trim().split("\n").pop());

t.test("native stdout path, after start: EPIPE once the reader is gone rejects that write, and the error sticks to the sink across streams and ByteSink writes", async (a) => {
    const p = sh("\"$0\" --std \"$1\" epipe | head -c 6 > /dev/null");
    const e = await readAll(p.stderr);
    await p.wait();
    const r = lastJson(e);
    a.eq(r.m, "stdout: write failed: Broken pipe");
    a.eq(r.sticky, true, "the same error object rejects the next write on the errored stream");
    a.eq(r.fresh, "stdout: write failed: Broken pipe", "a fresh stdout() stream fails the same way");
}, T);

t.test("native stdout path on a non-blocking pipe: EAGAIN and short writes park the rest and keep every byte in order (non-periodic data)", async (a) => {
    const out = tmpfile("nb");
    const p2 = new Spawn(self(), ["--std", exe, "nb", out]);
    const e = await readAll(p2.stderr);
    await p2.wait();
    const r = lastJson(e);
    a.eq(r.code, 0);
    a.eq(r.wfd, true, "found the spawn's non-blocking write end");
    a.eq(r.got, r.total);
    a.eq(r.same, true, "bytes identical after EAGAIN parks");
    os.remove(out);
}, T);

t.test("native stdin path for a regular file: a full chunk keeps its buffer, a half-or-more read zeroes the unread tail, a short read is copied to its exact size", async (a) => {
    const f1 = bytesFile(65536 + 40000, 11), f2 = bytesFile(100, 12);
    const p = sh("\"$0\" --std \"$1\" small < \"$2\"", f1);
    const r1 = lastJson(await readAll(p.stderr));
    await p.wait();
    a.eq(JSON.stringify(r1), JSON.stringify([[65536, 65536, true], [40000, 65536, true]]));
    const q = sh("\"$0\" --std \"$1\" small < \"$2\"", f2);
    const r2 = lastJson(await readAll(q.stderr));
    await q.wait();
    a.eq(JSON.stringify(r2), JSON.stringify([[100, 100, true]]));
    os.remove(f1); os.remove(f2);
}, T);

t.test("native stdin path for a regular file: a BYOB read fills the caller's view at its offset", async (a) => {
    const f = tmpfile("byob"); const fh = std.open(f, "w"); fh.puts("0123456789abcdef"); fh.close();
    const p = sh("\"$0\" --std \"$1\" byob < \"$2\"", f);
    const r = lastJson(await readAll(p.stderr));
    await p.wait();
    a.eq(JSON.stringify(r), JSON.stringify({ n: 10, off: 3, s: "0123456789" }));
    os.remove(f);
}, T);

t.test("cancel with a pending piped-stdin read settles it done, disarms the watch (the process exits with stdin still open) and leaves fd 0 open", async (a) => {
    const p = run("cancel", { stdin: "pipe" });
    const e = await readAll(p.stderr);
    const r = await p.wait();
    a.eq(r.code, 0);
    a.eq(JSON.stringify(lastJson(e)), JSON.stringify({ done: true, fd0: true }));
    p.stdin.close();
}, T);

await t.run();
