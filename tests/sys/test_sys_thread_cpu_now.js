// covers: dyna:sys.threadCpuNow
// flags: --std
// threadCpuNow() is the calling thread's CPU clock in ms (CLOCK_THREAD_CPUTIME_ID): it is for
// timing a function on a loaded host, where wall time counts every moment the process is
// descheduled and cpuUsage() (getrusage) also counts helper threads and costs an object per
// call. Measured per call (mac, 1e6 calls): threadCpuNow 156 ns, cpuUsage 404 ns,
// performance.now 55 ns. The properties pinned: milliseconds (a CPU-bound loop moves it as much
// as cpuUsage moves user+system), monotonic, no accrual while the thread sleeps, and no accrual
// for another thread's work.
// Mutants (red): CLOCK_MONOTONIC (accrues during sleep), CLOCK_PROCESS_CPUTIME_ID (accrues the
// worker's spin), seconds instead of ms (ratio to cpuUsage off by 1000). Every spin is also bounded
// by wall time, so a wrong unit fails a check instead of spinning past the runner's CPU bound.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_thread_cpu_now");
import { threadCpuNow, cpuUsage } from "dyna:sys";
import { makeTempDir, removeAll, writeFile } from "dyna:file";
import * as os from "os";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        print("  FAIL " + msg);
    }
}
const procMs = () => {
    const c = cpuUsage();
    return (c.user + c.system) * 1000;
};

__kitS.section("a finite non-negative number", () => {
    const a = threadCpuNow();
    ok(typeof a === "number" && Number.isFinite(a) && a >= 0, "a finite non-negative number (" + a + ")");
    let prev = a, mono = true;
    for (let i = 0; i < 100000; i++) {
        const t = threadCpuNow();
        if (t < prev) mono = false;
        prev = t;
    }
    ok(mono, "never goes backwards over 100000 calls");
    ok(prev > a, "100000 calls themselves accrue CPU (" + (prev - a).toFixed(3) + " ms)");
});
__kitS.section("a CPU-bound loop moves it in milliseconds, like cpuUsage user+system", () => {
    let best = Infinity;
    for (let round = 0; round < 3; round++) {
        const t0 = threadCpuNow(), p0 = procMs();
        let x = 0;
        const end = t0 + 120, wallEnd = performance.now() + 3000;
        while (threadCpuNow() < end && performance.now() < wallEnd) for (let i = 0; i < 1000; i++) x += i * i;
        const dt = threadCpuNow() - t0, dp = procMs() - p0;
        best = Math.min(best, Math.abs(dt / dp - 1));
        if (x < 0) print(x);
    }
    ok(best < 0.25, "a CPU-bound loop moves it in milliseconds, like cpuUsage user+system (best |ratio-1| " + best.toFixed(3) + ")");
});
__kitS.section("a sleeping thread accrues nothing", () => {
    const t0 = threadCpuNow(), w0 = performance.now();
    os.sleep(200);
    const dt = threadCpuNow() - t0, dw = performance.now() - w0;
    ok(dw >= 150 && dt < 40, "a sleeping thread accrues nothing (" + dt.toFixed(2) + " ms CPU over " + dw.toFixed(0) + " ms)");
});
await __kitS.section("import * as os from 'os'", async () => {
    const dir = makeTempDir("dyna-thread-cpu-");
    const script = dir.join("spin.js");
    writeFile(script,
        "import * as os from 'os';\n" +
        "import { threadCpuNow } from 'dyna:sys';\n" +
        "os.Worker.parent.onmessage = (e) => {\n" +
        "    const end = threadCpuNow() + e.data, wallEnd = Date.now() + 3000;\n" +
        "    let x = 0;\n" +
        "    while (threadCpuNow() < end && Date.now() < wallEnd) for (let i = 0; i < 1000; i++) x += i;\n" +
        "    os.Worker.parent.postMessage(x > -1 ? 'done' : 'x');\n" +
        "    os.Worker.parent.onmessage = null;\n" +
        "};\n");
    const w = new os.Worker(String(script));
    const t0 = threadCpuNow(), p0 = procMs();
    const got = await new Promise((resolve) => {
        w.onmessage = (e) => resolve(e.data);
        w.postMessage(250);
    });
    w.onmessage = null;
    const dt = threadCpuNow() - t0, dp = procMs() - p0;
    ok(got === "done", "the worker ran");
    ok(dp >= 200, "the process clock saw the worker's 250 ms CPU spin (" + dp.toFixed(1) + " ms)");
    ok(dt < 60, "the main thread's clock did not (" + dt.toFixed(1) + " ms)");
    removeAll(dir);
});
__kitS.end();
print("test_sys_thread_cpu_now: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_sys_thread_cpu_now: " + failed + " failures");
