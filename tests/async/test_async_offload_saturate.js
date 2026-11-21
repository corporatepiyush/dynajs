// flags: --std --io-threads 1
// covers: dyna:async.offload
// Owner ask E, wave 4: offload() under pool saturation. With --io-threads 1 the fiber channel holds 8 jobs, so 240
// concurrent offloads (CPU class: gzip, SHA-256; IO class: readBytes, writeFile) overflow it: a fiber that finds
// the channel full waits in a queue (offload() itself returns at once) and is resubmitted when an earlier job
// comes back to the loop. Every result must equal the direct call and every promise
// must settle (a fiber lost from the wait queue hangs the suite until the runner's timeout). Last, the process exits
// with std.exit() while offloaded jobs are still running: no crash, no hang, no sanitizer report.
// Mutation: dropping the requeue in dyn_fiber_start_waiting or the wait-queue insert in dyn_fiber_drive loses
// fibers (timeout, red); running a saturated call inline instead of queueing it makes offload() itself slow (red).
import { suite } from "../lib/kit.js";
import { text, equalValue } from "../lib/offload_check.js";
import { offload } from "dyna:async";
import { gzip } from "dyna:compress";
import { SHA256Hex } from "dyna:hash";
import { Bcrypt } from "dyna:crypto";
import { readBytes, writeFile, makeTempDir, removeAll, Path } from "dyna:file";
import * as std from "std";

const t = suite("async.offload.saturate");
const dir = makeTempDir("dj_offsat_");
const N = 60;
const inputs = Array.from({ length: N }, (_, i) => text((64 << 10) + i * 977, i));
const paths = inputs.map((_, i) => new Path(dir, "f" + i));
inputs.forEach((u, i) => writeFile(paths[i], u));

t.test("240 concurrent offloads on a 1-thread pool: every result equals the direct call", async (a) => {
    const jobs = [];
    for (let i = 0; i < N; i++) {
        jobs.push(offload(gzip, inputs[i]).then((r) => ["gzip", i, equalValue(r, gzip(inputs[i]))]));
        jobs.push(offload(SHA256Hex, inputs[i]).then((r) => ["sha", i, r === SHA256Hex(inputs[i])]));
        jobs.push(offload(readBytes, paths[i]).then((r) => ["read", i, equalValue(r, inputs[i])]));
        jobs.push(offload(writeFile, new Path(dir, "w" + i), inputs[i]).then((r) => ["write", i, r === inputs[i].length && equalValue(readBytes(new Path(dir, "w" + i)), inputs[i])]));
    }
    const out = await Promise.all(jobs);
    const bad = out.filter(([, , ok]) => !ok).map(([k, i]) => k + i);
    a.eq(out.length, 4 * N, "every promise settled");
    a.eq(bad.join(","), "", "every result equals the direct call");
});

t.test("a saturated pool queues the call instead of running it on the loop: offload() returns at once", async (a) => {
    const big = Array.from({ length: 40 }, (_, i) => text(512 << 10, 100 + i));
    let direct = performance.now();
    for (const u of big) gzip(u);
    direct = performance.now() - direct;
    let ticks = 0;
    const iv = setInterval(() => ticks++, 1);
    let issue = performance.now();
    const all = big.map((u) => offload(gzip, u));
    issue = performance.now() - issue;
    await Promise.all(all);
    clearInterval(iv);
    a.ok(issue < direct / 2, `issuing 40 offloads took ${issue.toFixed(1)} ms against ${direct.toFixed(1)} ms of direct work`);
    a.ok(ticks > 0, `timer ticks during the run: ${ticks}`);
});

t.after(() => removeAll(dir));
await t.run();

for (let i = 0; i < 8; i++) offload(Bcrypt.hash, "pw" + i, 12);
setTimeout(() => std.exit(0), 5);
