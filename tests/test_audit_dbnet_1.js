// flags: --std
// tests/test_audit_dbnet_1.js -- the Redis/PostgreSQL client keep-alive
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
// Pass the binary under test as scriptArgs[1] to check a second build:
//     ./dynajs --std tests/test_audit_dbnet_1.js /path/to/other/dynajs
import * as std from "std";
import { Spawn, args } from "dyna:sys";
import { TCPServer, Redis, PostgreSQL } from "dyna:net";

const SELF = scriptArgs[0];
const BIN = scriptArgs[1] || args()[0];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let checks = 0;
let failures = 0;

function check(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        std.err.puts("FAIL: " + msg + "\n");
    }
}

function redisMock() {
    const s = new TCPServer({ port: 0 });
    s.start({ data: (conn, bytes) => { conn.write("+PONG\r\n"); } });
    return s;
}

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

// --- half 1: forgetting the client mid-operation must not lose the reply ------

async function forgetSettles(s) {
    let got = "none";
    let red = "none";
    // No reference to the client survives this expression: the reply must
    // still reach the promise, or the finalizer silently drops the pending.
    new Redis({ port: s.port, host: "127.0.0.1" })
        .command("PING")
        .then((v) => { got = v; }, (e) => { red = e.message; });
    for (let i = 0; i < 40 && got === "none" && red === "none"; i++)
        await sleep(10);
    check(got === "PONG", "forgotten Redis client resolved (got " + got +
        ", rejected " + red + ")");
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

async function inProcess() {
    const s = redisMock();
    await forgetSettles(s);
    s.close();

    const quiet = silentMock();
    await dropIsFinalized(
        () => new Redis({ port: quiet.port, host: "127.0.0.1" }), "redis");
    await dropIsFinalized(
        () => new PostgreSQL({ port: quiet.port, host: "127.0.0.1", user: "u" }),
        "pg");
    quiet.close();
}

// --- half 2, continued: the process must still be able to shut down ----------

async function child(mode) {
    const quiet = silentMock();
    if (mode === "drop") {
        for (let i = 0; i < 12; i++) {
            new Redis({ port: quiet.port, host: "127.0.0.1" });
            new PostgreSQL({ port: quiet.port, host: "127.0.0.1", user: "u" });
        }
    } else {
        // Busy at exit: a command that never gets a reply, on a client nobody
        // holds. This is the shape that aborted in JS_FreeRuntime.
        new Redis({ port: quiet.port, host: "127.0.0.1" })
            .command("PING").catch(() => {});
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

async function main() {
    await inProcess();
    for (const mode of ["drop", "busy"])
        await runChild(mode);
    print("test_audit_dbnet_1: checks=" + checks + " fails=" + failures);
    std.exit(failures ? 1 : 0);
}

if (scriptArgs[1] === "--child") {
    child(scriptArgs[2]).catch((e) => {
        std.err.puts("child threw: " + (e && e.stack ? e.stack : e) + "\n");
        std.exit(1);
    });
} else {
    main().catch((e) => {
        std.err.puts("parent error: " + (e && e.stack ? e.stack : e) + "\n");
        std.exit(1);
    });
}
