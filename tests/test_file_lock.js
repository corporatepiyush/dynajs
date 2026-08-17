/* test_file_lock.js — FileLock: flock (Linux+macOS) / LockFileEx
 * (Windows) / O_EXCL fallback, with retry, and withLock (plan 3.9).
 *
 * Single-process tests prove NOTHING here: a lock only matters across
 * processes, so the load-bearing cases spawn a SECOND dynajs and make it
 * contend on the same path. A lock that never blocked would report
 * "acquired" in the blocked-child case below and fail this suite — that
 * is the mutation control.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_file_lock.js
 */
import { FileLock, Path, makeTempDir, writeFile, readFile, exists,
         removeAll } from "dyna:file";
import * as os from "os";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, m) { let t = false; try { fn(); } catch (e) { t = true; } assert(t, m); }

const T = String(makeTempDir("flock"));
const LOCK = T + "/guard.lock";
const exe = "./dynajs";

/* Child: acquires the lock with the given retry policy. Once held it writes
 * "held" to the held-marker file (so the PARENT knows it is holding NOW),
 * optionally keeps the lock for holdMs, then releases and writes
 * "acquired"/"blocked" to the result file. */
const CHILD = T + "/child.js";
writeFile(new Path(CHILD), `import { FileLock } from "dyna:file";
import * as std from "std";
import * as os from "os";
const lock = scriptArgs[1], held = scriptArgs[2], res = scriptArgs[3],
      retry = scriptArgs[4], holdMs = scriptArgs[5];
let r = "acquired";
try {
    const l = new FileLock(lock, { retry: Number(retry), retryMs: 50 });
    let f = std.open(held, "w"); f.puts("held"); f.close();
    if (Number(holdMs) > 0) os.sleep(Number(holdMs));
    l.close();
} catch (e) { r = "blocked"; }
const f = std.open(res, "w"); f.puts(r); f.close();
`);

function spawn(lock, retry, holdMs) {
    const held = T + "/h" + Math.floor(Math.random() * 1e9);
    const res = T + "/r" + Math.floor(Math.random() * 1e9);
    const pid = os.exec([exe, CHILD, lock, held, res, String(retry),
                         String(holdMs)],
                        { usePath: true, block: false });
    return { pid, held, res };
}
function waitFor(path, what, ms = 8000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
        if (exists(new Path(path))) return readFile(new Path(path));
        os.sleep(20);
    }
    throw new Error("timed out waiting for child: " + what);
}

/* ------------------------------------------------ refusals (in-process) */
throws(() => new FileLock(), "no path");
throws(() => new FileLock(42), "a number is not a path");
throws(() => new FileLock(new Path(LOCK), { retry: -1 }), "negative retry");
throws(() => new FileLock(new Path(LOCK), { retryMs: -1 }), "negative retryMs");
throws(() => new FileLock(new Path(LOCK), { retry: "x" }), "string retry");

/* ------------------------ cross-process: contention + release --------- */
{
    const holder = new FileLock(new Path(LOCK), { retry: 0 });
    let c = spawn(LOCK, 0, 0);
    eq(waitFor(c.res, "blocked while held"), "blocked",
       "child with retry:0 is blocked while the parent holds the lock");
    os.waitpid(c.pid, 0);

    holder.close();          /* release the lock */
    c = spawn(LOCK, 0, 0);
    eq(waitFor(c.res, "acquired after release"), "acquired",
       "the same path is free once the holder releases");
    os.waitpid(c.pid, 0);
}

/* ------------------- cross-process: retry waits for a release --------- */
{
    /* child takes the lock, signals "held", then holds it while the parent
       retries. ORDER is the assertion (never durations): the parent's
       acquire must not complete before the child's release marker exists --
       if the lock were not actually held across the process boundary, the
       acquire would return while the child still holds, and the released
       marker would be absent at that moment. */
    const c = spawn(LOCK, 0, 400);
    eq(waitFor(c.held, "child held"), "held", "child holds the lock");
    const l = new FileLock(new Path(LOCK), { retry: 20, retryMs: 50 });
    assert(exists(new Path(c.res)),
           "the acquire completed only after the child released (ordering)");
    l.close();
    eq(waitFor(c.res, "child released"), "acquired", "child released the lock");
    os.waitpid(c.pid, 0);
}

/* --------------------- cross-process: retry succeeds after release ---- */
{
    const holder = new FileLock(new Path(LOCK), { retry: 0 });
    const c = spawn(LOCK, 40, 0);        /* child retries 40x, 50ms apart */
    os.sleep(300);
    holder.close();                      /* release while child retries */
    eq(waitFor(c.res, "child acquired after release"), "acquired",
       "child with retry acquires once the holder releases");
    os.waitpid(c.pid, 0);
}

/* ------------------------------------------------- withLock in-process */
{
    const l = new FileLock(new Path(LOCK), { retry: 0 });
    eq(l.withLock(() => 42), 42, "withLock returns the callback's value");
    assert(l.closed === true, "withLock releases and closes the lock");
    throws(() => l.withLock(() => 1), "withLock on a closed lock throws");
    /* withLock released it: a fresh lock on the same path now succeeds */
    const l2 = new FileLock(new Path(LOCK), { retry: 0 });
    l2.close();
}

/* -------------------- same-process sanity (flock is per-fd) ----------- */
{
    const a = new FileLock(new Path(LOCK), { retry: 0 });
    /* flock is per open-file-description: a second fd on the same path
     * contends even in one process (unlike fcntl locks) */
    throws(() => new FileLock(new Path(LOCK), { retry: 0 }),
           "a second fd in the SAME process also contends (per-fd flock)");
    a.close();
    const b = new FileLock(new Path(LOCK), { retry: 0 });
    b.close();
}

removeAll(new Path(T));
if (fails) {
    print("test_file_lock: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_file_lock failed");
}
print("test_file_lock: " + n + " assertions, 0 failures");
