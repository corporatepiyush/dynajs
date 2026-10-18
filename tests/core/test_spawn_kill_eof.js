// After a fatal kill(), reads reach EOF once the direct child is reaped even
// when a surviving descendant still holds the pipe's write end.
import { suite } from "../lib/kit.js";
import { Spawn } from "dyna:sys";

const t = suite("spawn_kill_eof");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// Resolves "HUNG" after ms unless p settles first; the timer never outlives the race.
function orHung(p, ms) {
    let timer;
    const late = new Promise((r) => { timer = setTimeout(() => r("HUNG"), ms); });
    return Promise.race([p, late]).finally(() => clearTimeout(timer));
}

async function readAll(pipe) {
    const buf = new Uint8Array(4096);
    let out = "";
    for (;;) {
        const n = await pipe.read(buf);
        if (n === 0) return out;
        out += String.fromCharCode.apply(null, buf.subarray(0, n));
    }
}

// The grandchild ignores the catchable signals and holds stdout/stderr open for
// 8 s: longer than the 5 s deadline, short enough to leave no stray process.
// `trap "" SIG` in the subshell is inherited as SIG_IGN across exec.
const HOLDER = 'echo ready; (trap "" TERM; exec sleep 8) & wait';

// Only TERM is exercised: a runner that backgrounds the suite hands it INT and
// QUIT already ignored, and nohup does the same for HUP, so the shell would not die.
for (const sig of ["SIGTERM", "TERM", 15]) {
    t.test("kill(" + JSON.stringify(sig) + ") settles stdout and stderr reads", async (a) => {
        const child = new Spawn("/bin/sh", ["-c", HOLDER]);
        const buf = new Uint8Array(64);
        const n = await child.stdout.read(buf);
        a.eq(String.fromCharCode.apply(null, buf.subarray(0, n)), "ready\n");
        const t0 = Date.now();
        const outP = readAll(child.stdout), errP = readAll(child.stderr);
        a.ok(child.kill(sig));
        const res = await child.wait();
        a.ok(res.signal !== null || res.code !== 0, "the shell died from the signal");
        const got = await orHung(Promise.all([outP, errP]), 5000);
        a.ne(got, "HUNG", "reads settled without waiting for the pipe holder");
        a.ok(Date.now() - t0 < 5000, "settled in " + (Date.now() - t0) + " ms");
        child.close();
    }, { timeoutMs: 15000 });
}

t.test("a non-fatal signal does not cut the pipe", async (a) => {
    const child = new Spawn("/bin/sh", ["-c", 'trap "" USR1; echo one; sleep 0.4; echo two']);
    // "one" is printed after the trap is installed: wait for it rather than for
    // a fixed delay, or a slow exec takes the signal before it is ignored.
    const first = new Uint8Array(64);
    const k = await child.stdout.read(first);
    const head = String.fromCharCode.apply(null, first.subarray(0, k));
    a.ok(child.kill("SIGUSR1"));
    a.eq(head + await readAll(child.stdout), "one\ntwo\n", "output after the signal still arrives");
    a.eq((await child.wait()).code, 0);
    child.close();
}, { timeoutMs: 10000 });

t.test("a normal exit still delivers everything written before it", async (a) => {
    const child = new Spawn("/bin/sh", ["-c", "printf abc; printf def 1>&2"]);
    a.eq(await readAll(child.stdout), "abc");
    a.eq(await readAll(child.stderr), "def");
    a.eq((await child.wait()).code, 0);
    child.close();
});

t.test("kill settles a read that buffered output before the signal", async (a) => {
    const child = new Spawn("/bin/sh", ["-c", 'echo first; echo up 1>&2; (trap "" TERM; exec sleep 8) & wait']);
    // stderr says the stdout line has been written; stdout itself stays unread.
    const up = new Uint8Array(16);
    await child.stderr.read(up);
    await sleep(50);
    child.kill();
    await child.wait();
    const got = await orHung(readAll(child.stdout), 5000);
    a.eq(got, "first\n", "buffered bytes are delivered, then EOF");
    child.close();
}, { timeoutMs: 15000 });

await t.run();
