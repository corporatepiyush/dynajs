// flags: --std
// timeout: 60
// covers: c:src/modules/libc/libc-stdout.c
// D5 (owner decision 2026-11-21): dyna:stream stdout()/stderr() coalesce small writes like bun. A chunk under 16 KiB is copied into one
// process-wide buffer per fd (at most 64 KiB) and its write resolves at once; the buffer goes out when it fills, when the event loop turns
// or goes idle, before a chunk of 16 KiB or more (one writev of buffer + chunk), on writer.close(), before console/print/std.out/std.err/
// os.write/os.close/os.dup2 on fd 1 or 2, before spawn/exec, before an uncaught error is printed, at std.exit and at process exit.
// Writing to one fd first flushes the other's buffer (stdout before stderr, like bun), so the bytes keep call order on a shared terminal.
// Errors stick to the fd: a later write, close() or a fresh stdout() rejects with "stdout: write failed: <strerror>"; an error no write
// reported is printed at exit as "TypeError: stdout: write failed: ..." with exit status 1 (as an uncaught rejected write did before).
// Each case runs a child (stdio_coalesce_child.js) and checks its bytes. Mutants: no flush at exit, a large write that skips the buffer, a
// swallowed flush error.
import { suite } from "../lib/kit.js";
import { Spawn } from "dyna:sys";
import * as std from "std";
import * as os from "os";
import { pattern, SIZES } from "./stdio_coalesce_child.js";

const t = suite("stdio_coalesce");
const T = { timeoutMs: 20000 };
const exe = scriptArgs[0].replace(/[^/]*$/, "") + "stdio_coalesce_child.js";
const BIN = std.getenv("DYNAJS_BIN") || "./dynajs";
async function readBytes(pipe) { const parts = []; let n = 0; for await (const c of pipe) { parts.push(c); n += c.length; } const out = new Uint8Array(n); let o = 0; for (const c of parts) { out.set(c, o); o += c.length; } return out; }
const dec = (b) => new TextDecoder().decode(b);
function child(mode, ...args) { return new Spawn(BIN, ["--std", exe, mode, ...args]); }
function sh(script, ...args) { return new Spawn("/bin/sh", ["-c", script, BIN, exe, ...args]); }
async function run(p) { const [o, e] = await Promise.all([readBytes(p.stdout), readBytes(p.stderr)]); const r = await p.wait(); return { out: o, err: dec(e), code: r.code }; }
const lastJson = (s) => JSON.parse(s.trim().split("\n").pop());

t.test("mixed small and large writes (some not awaited) reach the pipe byte-exact and in order", async (a) => {
    const r = await run(child("mixed"));
    const total = SIZES.reduce((x, y) => x + y, 0), want = pattern(total, 99);
    a.eq(r.code, 0);
    a.eq(r.err, "");
    a.eq(r.out.length, total);
    let bad = -1;
    for (let i = 0; i < total && bad < 0; i++) if (r.out[i] !== want[i]) bad = i;
    a.eq(bad, -1, "first differing byte");
}, T);

t.test("console.log, print, std.out, std.printf, os.write and a second stdout() keep call order with buffered writes", async (a) => {
    const r = await run(child("console"));
    a.eq(r.code, 0);
    a.eq(dec(r.out), "ab\ncdef\nghijkl" + "m".repeat(20000) + "n\no");
}, T);

t.test("std.exit flushes buffered bytes with no loop turn, and keeps its status", async (a) => {
    const r = await run(child("exit"));
    a.eq(dec(r.out), "exit-bytes\n");
    a.eq(r.code, 0);
    const r7 = await run(child("exit", "7"));
    a.eq(dec(r7.out), "exit-bytes\n");
    a.eq(r7.code, 7);
}, T);

t.test("an uncaught throw and an unhandled rejection: buffered stdout lands before the error on a shared fd", async (a) => {
    const r = await run(sh("\"$0\" --std \"$1\" throw 2>&1"));
    a.eq(r.code, 1);
    a.match(dec(r.out), /^before-throw\nError: boom\n/);
    const q = await run(sh("\"$0\" --std \"$1\" reject 2>&1"));
    a.eq(q.code, 1);
    a.match(dec(q.out), /^before-reject\nPossibly unhandled promise rejection: Error: nobody-catches\n/);
}, T);

t.test("an unawaited write at the end of the script is flushed when the loop goes idle", async (a) => {
    const r = await run(child("end"));
    a.eq(dec(r.out), "tail\n");
    a.eq(r.code, 0);
}, T);

t.test("stdout and stderr on one fd keep call order (stdout is flushed before stderr is written)", async (a) => {
    const r = await run(sh("\"$0\" --std \"$1\" order 2>&1"));
    a.eq(r.code, 0);
    a.eq(dec(r.out), "o1e1o2\ne2\no3o4e3e4o5e5o6e6");
}, T);

t.test("a small write resolves with its bytes still buffered; a loop turn and close() put them on the fd", async (a) => {
    const f = (std.getenv("TMPDIR") || ".") + "/coalesce." + os.getpid() + ".out";
    const r = await run(sh("\"$0\" --std \"$1\" file \"$2\" > \"$2\"", f));
    a.eq(r.code, 0);
    a.eq(JSON.stringify(lastJson(r.err)), JSON.stringify([0, 3, 3, 5]));
    a.eq(std.loadFile(f), "abcde");
    os.remove(f);
}, T);

t.test("EPIPE: a buffered write resolves, the failed flush rejects the next write and a fresh stdout() with the same error", async (a) => {
    const r = await run(sh("\"$0\" --std \"$1\" epipe | head -c 6 > /dev/null"));
    a.eq(r.code, 0);
    a.eq(JSON.stringify(lastJson(r.err)), JSON.stringify({ r1: "ok", r2: "stdout: write failed: Broken pipe", r3: "stdout: write failed: Broken pipe" }));
}, T);

t.test("EPIPE: close() reports the buffered write's error", async (a) => {
    const r = await run(sh("\"$0\" --std \"$1\" epipe-close | head -c 6 > /dev/null"));
    a.eq(JSON.stringify(lastJson(r.err)), JSON.stringify({ c: "stdout: write failed: Broken pipe" }));
}, T);

t.test("EPIPE that no write reported: printed at exit, exit status 1", async (a) => {
    const r = await run(sh("\"$0\" --std \"$1\" epipe-exit | head -c 6 > /dev/null; exit 0"));
    a.eq(r.err, "TypeError: stdout: write failed: Broken pipe\n");
    const s = await run(sh("set -o pipefail 2>/dev/null; \"$0\" --std \"$1\" epipe-exit | head -c 6 > /dev/null; echo rc=$? >&2"));
    a.match(s.err, /rc=1\n$/);
}, T);

t.test("EAGAIN while flushing the buffer (non-blocking pipe, reader asleep, only small chunks) waits for POLLOUT and loses no byte", async (a) => {
    const out = (std.getenv("TMPDIR") || ".") + "/coalesce." + os.getpid() + ".nb";
    const r = await run(child("nbsmall", out));
    const j = lastJson(r.err);
    a.eq(j.wfd, true, "found the spawn's non-blocking write end");
    a.eq(j.code, 0);
    a.eq(j.got, j.total);
    a.eq(j.same, true, "bytes identical");
    a.eq(j.waited, true, "the flush waited for the sleeping reader");
    os.remove(out);
}, T);

await t.run();
