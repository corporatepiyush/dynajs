// tests/lib/teardown_churn.js -- runtime teardown with live handles, one module
// per suite. Split from tests/net/test_net_teardown_churn.js (B1e): each module's
// suite passes the handles it owns; the silent peer they all aim at is a
// dyna:net TCPServer fixture. A suite calls
//     await teardownChurn("test_<m>_..._teardown_churn", { cyclic, plain }, suite)
// where cyclic(port) opens handles whose error/settle callbacks re-enter the
// handle (they schedule more work or close it) and returns them, and plain(port)
// opens the same handles with inert callbacks. The suite re-runs itself as a
// child (--child MODE CYCLES) and checks each teardown door: close, gc, exit,
// uncaught, kill, uncaught-cyclic -- exit shape plus a sanitizer-clean stderr.
import * as std from "std";
import * as os from "os";
import { Spawn, args } from "dyna:sys";
import { TCPServer } from "dyna:net";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function closeAll(hs) {
    for (const h of hs) { try { h.close(); } catch (_) {} }
}

async function child(kinds, mode, cycles) {
    const mock = new TCPServer();
    mock.start({ connect: () => {} });
    const mp = mock.port;
    for (let i = 0; i < cycles; i++) {
        const hs = kinds.cyclic(mp);
        await sleep(2);
        closeAll(hs);
    }
    if (mode === "close") {
        const hs = kinds.cyclic(mp);
        await sleep(2);
        closeAll(hs);
        mock.close();
        return 0;
    }
    const live = kinds.cyclic(mp);
    await sleep(2);
    if (mode === "gc") {
        void live;
        std.gc();
        std.gc();
        return 0;
    }
    if (mode === "kill") {
        os.kill(os.getpid(), os.SIGTERM);
        return 99;
    }
    if (mode === "exit") {
        std.exit(0);
        return 0;
    }
    return 0;
}

function uncaught(kinds, cycles, cyclic) {
    const mock = new TCPServer();
    mock.start({ connect: () => {} });
    const mp = mock.port;
    const one = cyclic ? kinds.cyclic : kinds.plain;
    for (let i = 0; i < cycles; i++) one(mp);
    one(mp);
    throw new Error(cyclic ? "teardown churn: uncaught with cyclic live resources"
                           : "teardown churn: uncaught with live resources");
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

const EXPECT = {
    exit: { code: 0, signal: null },
    uncaught: { code: 1, signal: null },
    gc: { code: 0, signal: null },
    close: { code: 0, signal: null },
    kill: { code: null, signal: "SIGTERM" },
    "uncaught-cyclic": { code: 1, signal: null },
};

export async function teardownChurn(name, kinds, suite) {
    const SELF = scriptArgs[0];
    if (scriptArgs[1] === "--child") {
        const cmode = scriptArgs[2] || "exit";
        const ccycles = parseInt(scriptArgs[3] || "25", 10);
        if (cmode === "uncaught" || cmode === "uncaught-cyclic") {
            uncaught(kinds, ccycles, cmode === "uncaught-cyclic");
            std.exit(77);
        }
        try {
            std.exit(await child(kinds, cmode, ccycles));
        } catch (e) {
            std.err.puts("child threw: " + (e && e.stack ? e.stack : e) + "\n");
            std.exit(1);
        }
        return;
    }
    const t = suite(name);
    const cycles = parseInt(scriptArgs[1] || "25", 10);
    for (const mode of ["close", "gc", "exit", "uncaught", "kill", "uncaught-cyclic"])
        t.test("teardown by " + mode + " has the expected exit shape and a clean stderr", async (a) => {
            const p = new Spawn(args()[0], ["--std", SELF, "--child", mode, String(cycles)]);
            const [o, e, r] = await Promise.all([readAll(p.stdout), readAll(p.stderr), p.wait()]);
            const want = EXPECT[mode];
            const okShape = a.ok(r.code === want.code && r.signal === want.signal,
                mode + ": exit shape (got code=" + r.code + " signal=" + r.signal +
                ", want code=" + want.code + " signal=" + want.signal + ")");
            const bad = /AddressSanitizer|LeakSanitizer|runtime error|Assertion|assert failed|Segmentation|abort/i;
            const hit = e.match(bad);
            const okErr = a.ok(!hit, mode + ": child stderr is clean (found: " +
                (hit ? e.slice(Math.max(0, hit.index - 120), hit.index + 200).replace(/\n/g, " | ") : "nothing") + ")");
            if ((!okShape || !okErr) && o) std.err.puts("child stdout [" + mode + "]: " + o + "\n");
        });
    const WATCH_MS = parseInt(std.getenv("TEARDOWN_WATCH_MS") || "120000", 10);
    const started = Date.now();
    const watchdog = setInterval(() => {
        if (Date.now() - started < WATCH_MS) return;
        clearInterval(watchdog);
        std.err.puts("FAIL: " + name + " stalled: no child finished for " + WATCH_MS +
                     "ms -- a teardown wedged the exit path\n");
        std.exit(2);
    }, 5000);
    try { await t.run(); } finally { clearInterval(watchdog); }
}
