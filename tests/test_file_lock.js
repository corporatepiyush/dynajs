// flags: --std
import { FileLock, Path, makeTempDir, writeFile, readFile, exists,
         removeAll, chmod } from "dyna:file";
import * as os from "os";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, m) { let t = false; try { fn(); } catch (e) { t = true; } assert(t, m); }

const T = String(makeTempDir("flock"));
const LOCK = T + "/guard.lock";
const exe = "./dynajs";

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
    const pid = os.exec([exe, "--std", "--std", CHILD, lock, held, res, String(retry),
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

throws(() => new FileLock(), "no path");
throws(() => new FileLock(42), "a number is not a path");
throws(() => new FileLock(new Path(LOCK), { retry: -1 }), "negative retry");
throws(() => new FileLock(new Path(LOCK), { retryMs: -1 }), "negative retryMs");
throws(() => new FileLock(new Path(LOCK), { retry: "x" }), "string retry");

{
    const holder = new FileLock(new Path(LOCK), { retry: 0 });
    let c = spawn(LOCK, 0, 0);
    eq(waitFor(c.res, "blocked while held"), "blocked",
       "child with retry:0 is blocked while the parent holds the lock");
    os.waitpid(c.pid, 0);

    holder.close();
    c = spawn(LOCK, 0, 0);
    eq(waitFor(c.res, "acquired after release"), "acquired",
       "the same path is free once the holder releases");
    os.waitpid(c.pid, 0);
}

{
    const c = spawn(LOCK, 0, 400);
    eq(waitFor(c.held, "child held"), "held", "child holds the lock");
    const l = new FileLock(new Path(LOCK), { retry: 20, retryMs: 50 });
    assert(exists(new Path(c.res)),
           "the acquire completed only after the child released (ordering)");
    l.close();
    eq(waitFor(c.res, "child released"), "acquired", "child released the lock");
    os.waitpid(c.pid, 0);
}

{
    const holder = new FileLock(new Path(LOCK), { retry: 0 });
    const c = spawn(LOCK, 40, 0);
    os.sleep(300);
    holder.close();
    eq(waitFor(c.res, "child acquired after release"), "acquired",
       "child with retry acquires once the holder releases");
    os.waitpid(c.pid, 0);
}

{
    const l = new FileLock(new Path(LOCK), { retry: 0 });
    eq(l.withLock(() => 42), 42, "withLock returns the callback's value");
    assert(l.closed === true, "withLock releases and closes the lock");
    throws(() => l.withLock(() => 1), "withLock on a closed lock throws");
    const l2 = new FileLock(new Path(LOCK), { retry: 0 });
    l2.close();
}

{
    const a = new FileLock(new Path(LOCK), { retry: 0 });
    throws(() => new FileLock(new Path(LOCK), { retry: 0 }),
           "a second fd in the SAME process also contends (per-fd flock)");
    a.close();
    const b = new FileLock(new Path(LOCK), { retry: 0 });
    b.close();
}

{
    const l = new FileLock(new Path(LOCK));
    let ran = false;
    l.withLock(() => { l.close(); ran = true; });
    assert(ran, "fn inside withLock ran");
    assert(l.closed, "the lock is closed after fn closed it");
    const l2 = new FileLock(new Path(LOCK), { retry: 0 });
    l2.close();
    assert(true, "the lock is RE-ACQUIRABLE after close-inside-withLock");
}

{
    const RO = T + "/ro.txt";
    writeFile(new Path(RO), "read-only content");
    chmod(new Path(RO), 0o444);
    let l = null;
    try { l = new FileLock(new Path(RO), { retry: 0 }); }
    catch (e) { assert(false, "a 0444 file is lockable (threw: " + e.code + ")"); }
    if (l) { l.withLock(() => {}); assert(true, "read-only lock released"); }
    chmod(new Path(RO), 0o644);
}

removeAll(new Path(T));
if (fails) {
    print("test_file_lock: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_file_lock failed");
}
print("test_file_lock: " + n + " assertions, 0 failures");
