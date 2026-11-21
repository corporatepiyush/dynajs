// covers: integration c:src/modules/libc/libc-event-loop.c c:src/mm/js_malloc.inc.c c:src/builtins/builtin-promise.c -- job drain loop stop point and job entry recycling
// v10b-23. The await and loop drivers run pending jobs in one loop (JS_ExecutePendingJobsUntil) instead of one job per
// outer iteration. The two -I cases (the CLI
// awaited module a, then ran b) went with the -I flag (owner, 2026-11-28). Job entries are recycled through a bounded free list; a deep chain and a wide
// fan-out of jobs keep FIFO order across the recycling.
import { args, Exec } from "dyna:sys";
import { suite } from "../lib/kit.js";

const t = suite("job_drain_stop");

t.test("deep chains and wide fan-outs keep FIFO order through recycled job entries", async (a) => {
    const log = [];
    const N = 2000;
    const ps = [];
    for (let i = 0; i < N; i++) ps.push(Promise.resolve(i).then((v) => { log.push(v); return v; }));
    let p = Promise.resolve(0);
    for (let i = 0; i < N; i++) p = p.then((v) => v + 1);
    const all = await Promise.all(ps);
    a.eq(await p, N);
    a.eq(all.length, N);
    let ok = true;
    for (let i = 0; i < N; i++) if (log[i] !== i) ok = false;
    a.ok(ok, "fan-out order");
});

await t.run();
