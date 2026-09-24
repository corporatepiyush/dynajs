// flags: --std
/* test_spawn_fdlimit.js — the descriptor limit IS the test. A leak of one fd
 * per spawn/close cycle passes a churn at the default limit (thousands of
 * descriptors of headroom) and dies here at 64. The target that runs it
 * lowers the limit; run by hand as:
 *     sh -c 'ulimit -n 64 && exec ./dynajs --std tests/test_spawn_fdlimit.js'
 * One child at a time keeps the suite's own usage (3 pipes + reactor) well
 * inside the limit, so a failure names a leak rather than a small ceiling. */

import { Spawn } from "dyna:sys";

let n = 0, fails = 0;
function eq(a, b, m) {
    n++;
    if (!Object.is(a, b)) { fails++; print("FAIL: " + m + " (got " + a + ", want " + b + ")"); }
}
async function readAll(src, cap) {
    let s = "", total = 0;
    for (;;) {
        const b = new Uint8Array(256);
        const k = await src.read(b);
        if (k === 0) break;
        s += new TextDecoder().decode(b.subarray(0, k));
        total += k;
        if (total > cap) throw new Error("read past the cap");
    }
    return s;
}

for (let i = 0; i < 40; i++) {
    const p = new Spawn("sh", ["-c", "printf ok"]);
    eq((await readAll(p.stdout, 64)), "ok", "cycle " + i + " output");
    const r = await p.wait();
    eq(r.code, 0, "cycle " + i + " exit code");
}
/* stdin-pipe cycles too: the write fd is the other half of the budget. */
for (let i = 0; i < 20; i++) {
    const p = new Spawn("cat", [], { stdin: "pipe" });
    const o = readAll(p.stdout, 64);
    await p.stdin.write("x");
    await p.stdin.flush();
    p.stdin.close();
    eq((await o), "x", "stdin cycle " + i + " echo");
    await p.wait();
}

print("test_spawn_fdlimit: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_spawn_fdlimit failed");
