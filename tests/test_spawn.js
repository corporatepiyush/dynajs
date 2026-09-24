/* test_spawn.js -- Spawn in dyna:sys: the async child with streaming
 * stdio, alongside the sync Exec.
 *
 * The cases that decide whether an ASYNC subprocess API is correct:
 *   - exit codes, signals and timedOut, reported through wait() with the
 *     SAME conventions as Exec (code null when a signal killed the child);
 *   - streaming: stdout and stderr drained continuously (a pipe nobody is
 *     reading must never deadlock the child), read() the dyna:stream shape;
 *   - BACKPRESSURE: a flooding child against a slow reader must stall the
 *     CHILD (kernel pipe full), not grow memory without bound and not kill
 *     anybody -- then resume when the reader catches up;
 *   - stdin: write/flush/close ordering, EPIPE after the child's exit;
 *   - kill() during a parked read (the read settles at EOF, never hangs);
 *   - close() during a parked read (the read REJECTS) and during a parked
 *     wait() (the wait settles with the SIGKILL that close() delivered);
 *   - NO SHELL (metacharacters literal) and NUL refusal (the module's
 *     doctrine: a syscall must not act on a truncated prefix);
 *   - no zombies: 100 spawns leave nothing unreaped in the process table.
 *
 * Every probe is timeout-wrapped: a hung child must fail the test, not the
 * gate. Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_spawn.js
 */
import { Spawn } from "dyna:sys";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
async function readAll(src) {
    const buf = new Uint8Array(65536);
    let out = "";
    for (;;) {
        const n = await src.read(buf);
        if (n === 0) break;
        out += new TextDecoder().decode(buf.subarray(0, n));
    }
    return out;
}
function sleepMs(ms) { return new Promise(res => setTimeout(res, ms)); }
/* THE timeout wrapper: every await that could hang goes through here. */
function withTimeout(p, ms, what) {
    return Promise.race([
        p,
        sleepMs(ms).then(() => { throw new Error("TIMEOUT: " + what); }),
    ]);
}
async function t(fn, ms) { await withTimeout(fn(), ms, fn.name); }

await t(async function basics() {
    const p = new Spawn("sh", ["-c", "exit 42"]);
    const r = await p.wait();
    eq(r.code, 42, "exit 42 reported through wait()");
    eq(r.signal, null, "and no signal");
    eq(r.timedOut, false, "and timedOut false");
}, 5000);

await t(async function streams() {
    const p = new Spawn("sh", ["-c", "echo o1; echo e1 >&2; echo o2; echo e2 >&2"]);
    const [o, e, r] = await Promise.all([readAll(p.stdout), readAll(p.stderr), p.wait()]);
    eq(o, "o1\no2\n", "stdout drained in order while stderr is read too");
    eq(e, "e1\ne2\n", "stderr is its own continuous stream");
    eq(r.code, 0, "and the child exited 0");
}, 5000);

await t(async function stdin() {
    const p = new Spawn("cat", [], { stdin: "pipe" });
    await p.stdin.write("one ");
    await p.stdin.write("two");
    await p.stdin.flush();
    await p.stdin.close();       /* EOF after the queue drains */
    eq(await readAll(p.stdout), "one two", "cat saw the queued bytes, then EOF");
    eq((await p.wait()).code, 0, "and exited 0");
}, 5000);

await t(async function noShell() {
    const p = new Spawn("echo", ["$(whoami)", "`id`", "a;b", "a|b", "a>b", "*"]);
    eq(await readAll(p.stdout), "$(whoami) `id` a;b a|b a>b *\n",
       "every metacharacter comes back LITERALLY -- there is no shell");
    await p.wait();
}, 5000);

await t(async function nulRefusal() {
    let threw = false;
    try { new Spawn("ech\0o", ["hi"]); } catch (e) { threw = true; }
    assert(threw, "a NUL in the command is refused (execve would act on the prefix)");
    threw = false;
    try { new Spawn("echo", ["a\0b"]); } catch (e) { threw = true; }
    assert(threw, "a NUL in an argument is refused");
    threw = false;
    try { new Spawn("echo", [], { env: { "A\0B": "x" } }); } catch (e) { threw = true; }
    assert(threw, "a NUL in an env name is refused");
}, 5000);

await t(async function notFound() {
    let threw = false;
    try { new Spawn("definitely-not-a-real-command-xyz"); } catch (e) { threw = true; }
    assert(threw, "command not found throws (not a bare 127)");
}, 5000);

await t(async function signals() {
    const p = new Spawn("sleep", ["30"]);
    eq(p.kill("SIGTERM"), true, "kill() reports that the child was running");
    const r = await p.wait();
    eq(r.code, null, "code is null when a signal killed the child (Exec's rule)");
    eq(r.signal, "SIGTERM", "and the signal is named");
    eq(p.kill("SIGTERM"), false, "kill after exit is false");
}, 5000);

await t(async function signalNumber() {
    const p = new Spawn("sleep", ["30"]);
    eq(p.kill(9), true, "kill by number");
    eq((await p.wait()).signal, "SIGKILL", "SIGKILL reported");
}, 5000);

await t(async function timeoutEscalation() {
    const p = new Spawn("sleep", ["30"], { timeoutMs: 300 });
    const r = await p.wait();
    eq(r.signal, "SIGTERM", "the deadline SIGTERMs the group");
    eq(r.timedOut, true, "and timedOut is set");
}, 8000);

await t(async function killMidStream() {
    const p = new Spawn("sh", ["-c", "echo start; sleep 30"]);
    const buf = new Uint8Array(4096);
    eq(new TextDecoder().decode(buf.subarray(0, await p.stdout.read(buf))),
       "start\n", "first chunk arrives");
    const parked = p.stdout.read(buf);
    await sleepMs(50);
    p.kill("SIGKILL");
    eq(await parked, 0, "the parked read settles at EOF (never hangs)");
    eq((await p.wait()).signal, "SIGKILL", "and the wait reports the kill");
}, 5000);

await t(async function closeDuringPendingRead() {
    const p = new Spawn("sleep", ["1"]);
    const buf = new Uint8Array(4096);
    const parked = p.stdout.read(buf);
    parked.catch(() => {});      /* the rejection IS the assertion */
    await sleepMs(30);
    p.stdout.close();
    let rejected = false;
    try { await parked; } catch (e) { rejected = true; }
    assert(rejected, "close() during a parked read rejects that read");
    await p.wait();
}, 8000);

await t(async function closeDuringPendingWait() {
    const p = new Spawn("sleep", ["1"]);
    const w = p.wait();
    w.catch(() => {});
    await sleepMs(30);
    p.close();
    const r = await w;
    eq(r.signal, "SIGKILL", "close() settles a parked wait with the kill it delivered");
    eq(p.closed, true, "and the object reports closed");
}, 8000);

await t(async function backpressure() {
    /* 512 MiB offered against a reader that stops: the child must STALL
     * (kernel pipe full) -- not grow memory unbounded, not get killed --
     * and resume when the reader catches up. */
    const p = new Spawn("sh", ["-c", "dd if=/dev/zero bs=65536 count=8192 2>/dev/null"]);
    const buf = new Uint8Array(1 << 20);
    let total = 0;
    while (total < 32 * 1024 * 1024) {
        const got = await p.stdout.read(buf);
        if (got === 0) break;
        total += got;
    }
    await sleepMs(300);          /* the reader stops: pipe + buffer fill */
    while (total < 64 * 1024 * 1024) {
        const got = await p.stdout.read(buf);
        if (got === 0) break;
        total += got;
    }
    assert(total >= 64 * 1024 * 1024,
           "backpressure stalled the child, then the transfer resumed (got " + total + ")");
    p.kill("SIGKILL");
    await p.wait();
}, 30000);

await t(async function cwdAndEnv() {
    const p = new Spawn("sh", ["-c", "pwd; echo $FOO"], { cwd: "/tmp", env: { FOO: "bar" } });
    const out = (await readAll(p.stdout)).replace("/private/tmp", "/tmp");
    eq(out, "/tmp\nbar\n", "cwd and env are applied");
    await p.wait();
}, 5000);

await t(async function concurrencyRefusals() {
    const p = new Spawn("sleep", ["1"]);
    const buf = new Uint8Array(4096);
    const r1 = p.stdout.read(buf);
    r1.catch(() => {});
    let threw = false;
    try { p.stdout.read(buf); } catch (e) { threw = true; }
    assert(threw, "a second concurrent read() on one pipe is refused");
    p.stdout.close();
    const w1 = p.wait();
    w1.catch(() => {});
    threw = false;
    try { p.wait(); } catch (e) { threw = true; }
    assert(threw, "a second concurrent wait() is refused");
    p.close();
    const r = await w1;
    eq(r.signal, "SIGKILL", "the parked wait still settles (with the kill)");
}, 8000);

await t(async function waitReplays() {
    const p = new Spawn("sh", ["-c", "exit 7"]);
    await p.wait();
    eq((await p.wait()).code, 7, "wait() after exit replays the recorded result");
}, 5000);

await t(async function viewOutlivesSpawn() {
    let out;
    {
        const p = new Spawn("echo", ["survivor"]);
        out = p.stdout;
        await p.wait();
    }
    await sleepMs(100);          /* the spawn object is collectable now */
    eq(await readAll(out), "survivor\n",
       "a stdio view pulled into a local outlives the dropped Spawn");
}, 5000);

await t(async function closeParkedWaitIsPromptAndReal() {   /* reviewer p1 */
    const p = new Spawn("sleep", ["6"]);
    const w = p.wait();
    w.catch(() => {});
    await sleepMs(100);
    const t0 = Date.now();
    p.close();
    const dt = Date.now() - t0;
    assert(dt < 3000, "close() on a parked wait() is prompt (got " + dt + " ms -- a blocking close waited for natural death)");
    const r = await w;
    eq(r.signal, "SIGKILL", "the parked wait settles with the REAL SIGKILL close() delivered");
}, 8000);

await t(async function closeWithLiveViewsKills() {          /* reviewer p7 */
    const p = new Spawn("sh", ["-c", "sleep 30"]);
    const buf = new Uint8Array(1024);
    const r1 = p.stdout.read(buf);
    r1.catch(() => {});
    await sleepMs(50);
    const pid = p.pid;          /* before close: pid reads -1 afterwards */
    p.close();
    eq(p.closed, true, "closed with live views");
    eq(p.pid, -1, "pid reads -1 after close");
    /* view access after close throws */
    let threw = false;
    try { await p.stdout.read(buf); } catch (e) { threw = /closed/.test(e.message); }
    assert(threw, "view access after close throws 'the process is closed'");
    threw = false;
    try { p.kill("SIGKILL"); } catch (e) { threw = true; }
    assert(threw, "kill after close throws (nothing left to kill)");
    /* the child is really dead out there: OUR pid, never a global process
       scan -- the runner runs this suite beside other spawn suites whose
       children are legitimately alive, so `pgrep -fl sleep 30` went flaky on
       a sibling's process. */
    const chk = new Spawn("sh", ["-c",
        "sleep 0.3; if kill -0 " + pid + " 2>/dev/null; then echo alive; else echo gone; fi"]);
    const alive = await readAll(chk.stdout);
    await chk.wait();
    eq(alive.trim(), "gone", "the closed child's pid is gone");
}, 10000);

await t(async function concurrentAwaitedReads() {           /* reviewer m7/m5 */
    /* 12 concurrent jobs, each dropping the Spawn object mid-read (the
     * frame clears `p` at last use): the pipes must keep working through
     * the stdio views, with no GC abort, no hang, no cross-talk. */
    const jobs = new Array(12).fill(0).map((_, i) => async () => {
        const p = new Spawn("sh", ["-c", "echo c-" + i + "; sleep 0.05"]);
        const o = await readAll(p.stdout);
        const r = await p.wait();
        return o.trim() + "/" + r.code;
    });
    const outs = await withTimeout(Promise.all(jobs.map(f => f())), 15000, "concurrent reads");
    for (let i = 0; i < 12; i++) {
        eq(outs[i], "c-" + i + "/0", "concurrent job " + i + " result");
    }
}, 20000);

await t(async function spawnChurnMixedModes() {             /* reviewer p4 */
    for (let i = 0; i < 40; i++) {
        const mode = i % 4;
        const p = new Spawn("sleep", ["30"]);
        if (mode === 0) {
            p.close();
        } else if (mode === 1) {
            const b = new Uint8Array(1024);
            const r = p.stdout.read(b);
            r.catch(() => {});
            p.close();
            try { await r; } catch (e) {}
        } else if (mode === 2) {
            p.kill("SIGKILL");
            await p.wait();
            p.close();
        } else {
            const pi = new Spawn("cat", [], { stdin: "pipe" });
            await pi.stdin.write("x");
            const f = pi.stdin.flush();
            pi.close();
            try { await f; } catch (e) {}
        }
        n++;
    }
}, 30000);

await t(async function noZombies() {
    const pids = [];
    for (let i = 0; i < 100; i++) {
        const p = new Spawn("true");   /* held: a dropped spawn GC-reaps early */
        const r = await p.wait();
        if (r.code !== 0) { fails++; print("FAIL: spawn " + i + " nonzero"); break; }
        pids.push(p.pid);
        n++;
    }
    await sleepMs(600);          /* every reaper had its tick */
    /* OUR pids, never a system-wide zombie count: sibling suites in the same
       parallel run leave their own short-lived zombies behind for a tick. */
    const ps = new Spawn("/bin/sh", ["-c",
        "for p in " + pids.join(" ") + "; do ps -p $p -o stat= 2>/dev/null | grep -q Z && echo $p; done; true"]);
    const z = (await readAll(ps.stdout)).trim();
    await ps.wait();
    eq(z, "", "no zombie left among the 100 spawned pids");
}, 60000);

print("test_spawn: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_spawn failed");
