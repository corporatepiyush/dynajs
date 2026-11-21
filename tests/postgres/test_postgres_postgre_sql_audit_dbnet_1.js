// flags: --std
// covers: dyna:postgres.PostgreSQL
// tests/postgres/test_postgres_postgre_sql_audit_dbnet_1.js -- the PostgreSQL client keep-alive
// (N3-08) must hold the wrapper only while an operation is in flight, and must
// never leave a self-reference behind that outlives the runtime.
//
// Two halves, and the test is red if either is broken:
//
//   1. FORGET WHILE IN FLIGHT.  A client nobody holds must not be collected
//      before its reply arrives: the promise still settles.
//   2. THE CLIENT IS ACTUALLY RELEASED.  A client created and dropped with no
//      operation in flight must be finalized (a FinalizationRegistry token
//      fires), and a process that ends with such clients -- or with clients
//      whose command never got a reply -- must shut down with a clean exit
//      instead of aborting in JS_FreeRuntime on a self-reference cycle.
//
// flags: --std plus dyna:net + dyna:sys (build with CONFIG_NATIVE_MODULES=y)
// Each wait below runs std.gc(): the self-hold taken while an operation is in
// flight is a root, never a marked edge, since a marked self-reference is a
// cycle the collector frees with the reply still pending (red on gcc builds,
// where an allocation-triggered GC landed inside the 400 ms window; mutant:
// mark r->self or g->self in the gc_mark hook).
// Pass the binary under test as scriptArgs[1] to check a second build:
//     ./dynajs --std tests/postgres/test_postgres_postgre_sql_audit_dbnet_1.js /path/to/other/dynajs
// Split from tests/net/test_audit_dbnet_1.js (B1e: one module per suite); the
// Redis half is tests/redis/test_redis_redis_audit_dbnet_1.js.
import * as std from "std";
import { Spawn, args } from "dyna:sys";
import { TCPServer } from "dyna:net";
import { PostgreSQL } from "dyna:postgres";
import { suite } from "../lib/kit.js";

const SELF = scriptArgs[0];
const BIN = scriptArgs[1] || args()[0];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let A = null;
const check = (cond, msg) => A.ok(cond, msg);

function silentMock() {
    const s = new TCPServer({ port: 0 });
    s.start({ connect: () => {} });
    return s;
}

async function readAll(src) {
    const buf = new Uint8Array(65536);
    let out = "";
    for (;;) {
        const k = await src.read(buf);
        if (k === 0) break;
        out += new TextDecoder().decode(buf.subarray(0, k));
    }
    return out;
}

// --- half 1b: a client nobody holds survives GC while its operation waits ------

async function inFlightSurvivesGc(make, start, label) {
    const seen = [];
    const reg = new FinalizationRegistry((held) => { seen.push(held); });
    let settled = "none";
    const w = (() => {
        const c = make();
        reg.register(c, label);
        start(c).then(() => { settled = "resolved"; }, (e) => { settled = e.message; });
        return new WeakRef(c);
    })();
    for (let i = 0; i < 6; i++) {
        std.gc();
        await sleep(5);
    }
    const c = w.deref();
    check(seen.length === 0 && c !== undefined, label + ": an unheld client " +
        "with an operation in flight survives GC (finalized " + seen.length + ")");
    if (c) c.close();
    for (let i = 0; i < 20 && settled === "none"; i++) await sleep(5);
    check(settled !== "none" && settled !== "resolved", label +
        ": close() rejects the pending operation (got " + settled + ")");
}

// --- half 2: a dropped client with nothing in flight must be finalized -------

async function dropIsFinalized(make, label) {
    const seen = [];
    const reg = new FinalizationRegistry((held) => { seen.push(held); });
    for (let i = 0; i < 8; i++) {
        let c = make();
        reg.register(c, label + i);
        c = null;
    }
    for (let i = 0; i < 6; i++) {
        std.gc();
        await sleep(5);
    }
    check(seen.length === 8, label + ": 8 dropped clients were finalized " +
        "(saw " + seen.length + "); a self-reference the release path cannot " +
        "reach keeps them alive forever");
}

// --- half 2, continued: the process must still be able to shut down ----------

async function child(mode) {
    const quiet = silentMock();
    if (mode === "drop") {
        for (let i = 0; i < 12; i++) {
            new PostgreSQL({ port: quiet.port, host: "127.0.0.1", user: "u" });
        }
    } else {
        // Busy at exit: a command that never gets a reply, on a client nobody
        // holds. This is the shape that aborted in JS_FreeRuntime.
        new PostgreSQL({ port: quiet.port, host: "127.0.0.1", user: "u" })
            .query("SELECT 1").catch(() => {});
    }
    await sleep(30);
    std.gc();
    print("child " + mode + " ok");
    std.exit(0);
}

async function runChild(mode) {
    const p = new Spawn(BIN, ["--std", SELF, "--child", mode]);
    const [o, e, r] = await Promise.all([
        readAll(p.stdout), readAll(p.stderr), p.wait()]);
    const noisy = /AddressSanitizer|LeakSanitizer|runtime error|Assertion|assert failed|Segmentation|abort/i;
    const hit = e.match(noisy);
    check(r.code === 0 && r.signal === null,
        "child " + mode + ": clean exit (got code=" + r.code +
        " signal=" + r.signal + ")");
    check(o.indexOf("child " + mode + " ok") >= 0,
        "child " + mode + ": reached the end (stdout: " +
        JSON.stringify(o.slice(0, 200)) + ")");
    check(!hit, "child " + mode + ": stderr clean (found: " +
        (hit ? e.slice(Math.max(0, hit.index - 100), hit.index + 160).replace(/\n/g, " | ") : "nothing") + ")");
}


if (scriptArgs[1] === "--child") {
    child(scriptArgs[2]).catch((e) => {
        std.err.puts("child threw: " + (e && e.stack ? e.stack : e) + "\n");
        std.exit(1);
    });
} else {
    // Each case starts its own mock (or child process) and closes it; nothing
    // listens under --list.
    const t = suite("test_postgres_postgre_sql_audit_dbnet_1");
    const test = (n, fn, o) => t.test(n, (a) => { A = a; return fn(a); }, o);
    test("an unheld PostgreSQL client survives GC while a query waits", async () => {
        const quiet = silentMock();
        await inFlightSurvivesGc(
            () => new PostgreSQL({ port: quiet.port, host: "127.0.0.1", user: "u" }),
            (c) => c.query("SELECT 1"), "pg");
        quiet.close();
    });
    test("dropped PostgreSQL clients are finalized", async () => {
        const quiet = silentMock();
        await dropIsFinalized(
            () => new PostgreSQL({ port: quiet.port, host: "127.0.0.1", user: "u" }),
            "pg");
        quiet.close();
    });
    test("a process ending with dropped clients exits cleanly", async () => {
        await runChild("drop");
    });
    test("a process ending with clients awaiting a reply exits cleanly", async () => {
        await runChild("busy");
    });
    await t.run();
}
