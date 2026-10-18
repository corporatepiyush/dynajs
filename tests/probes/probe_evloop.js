// Event-loop probe: what one turn of the loop costs, by source. Prints
// machine-readable lines "probe <name> <value> <unit>" so two builds diff.
// Run: ./dynajs --std tests/probes/probe_evloop.js [name-filter]
import * as os from "os";
import * as std from "std";
import { rusage } from "dyna:sys";

const only = scriptArgs[1] || "";
const out = (name, value, unit) => print(`probe ${name} ${typeof value === "number" ? +value.toPrecision(4) : value} ${unit}`);
const now = () => performance.now();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const want = (name) => !only || name.includes(only);
// loop wake-ups: os.loopStats().waits counts blocking waits entered (exact);
// rusage().nvcsw is the kernel's count where the kernel keeps one (not macOS).
const waits = () => (typeof os.loopStats === "function" ? os.loopStats().waits : rusage().nvcsw);

if (want("timer_zero_chain")) {
    const N = 200000;
    let i = 0;
    const t0 = now();
    await new Promise((done) => { const step = () => { if (++i < N) setTimeout(step, 0); else done(); }; setTimeout(step, 0); });
    out("timer_zero_chain", (now() - t0) * 1e6 / N, "ns/timer");
}

if (want("timer_burst")) {
    // N timers all due at once: the cost of draining an expired batch
    const N = 100000;
    let fired = 0;
    const t0 = now();
    await new Promise((done) => { for (let k = 0; k < N; k++) setTimeout(() => { if (++fired === N) done(); }, 1); });
    out("timer_burst", (now() - t0) * 1e6 / N, "ns/timer");
}

if (want("timer_spread")) {
    const N = 20000;
    let fired = 0, late = 0, worst = 0;
    const t0 = now();
    await new Promise((done) => {
        for (let k = 0; k < N; k++) {
            const d = (k * 7919) % 40;
            const due = t0 + d;
            setTimeout(() => { const l = now() - due; late += l; if (l > worst) worst = l; if (++fired === N) done(); }, d);
        }
    });
    out("timer_spread_mean_late", late / N, "ms");
    out("timer_spread_worst_late", worst, "ms");
}

if (want("microtask_chain")) {
    const N = 2000000;
    let p = Promise.resolve(0);
    const t0 = now();
    for (let k = 0; k < N; k++) p = p.then((v) => v + 1);
    await p;
    out("microtask_chain", (now() - t0) * 1e6 / N, "ns/job");
}

if (want("async_await_loop")) {
    const N = 2000000;
    const f = async (v) => v + 1;
    const t0 = now();
    let v = 0;
    for (let k = 0; k < N; k++) v = await f(v);
    out("async_await_loop", (now() - t0) * 1e6 / N, "ns/await");
}

for (const NF of [8, 256]) {
    const name = "fd_ready_all_" + NF;
    if (!want(name)) continue;
    // NF pipes with a read handler each; every round makes ALL of them readable.
    const pipes = [];
    for (let k = 0; k < NF; k++) pipes.push(os.pipe());
    const buf = new Uint8Array(8);
    const ROUNDS = NF > 64 ? 40 : 2000;
    let seen = 0, resolve;
    for (const [r] of pipes) os.setReadHandler(r, () => { os.read(r, buf.buffer, 0, 8); if (++seen % NF === 0) resolve(); });
    const t0 = now();
    for (let round = 0; round < ROUNDS; round++) {
        const p = new Promise((res) => { resolve = res; });
        for (const [, w] of pipes) os.write(w, buf.buffer, 0, 1);
        await p;
    }
    out(name, (now() - t0) * 1e6 / (ROUNDS * NF), "ns/event");
    for (const [r, w] of pipes) { os.setReadHandler(r, null); os.close(r); os.close(w); }
}

if (want("fd_one_of_256")) {
    // 256 registered handlers, one becomes ready per round: the cost a quiet fd adds
    const NF = 256, ROUNDS = 5000;
    const pipes = [];
    for (let k = 0; k < NF; k++) pipes.push(os.pipe());
    const buf = new Uint8Array(8);
    let resolve;
    for (const [r] of pipes) os.setReadHandler(r, () => { os.read(r, buf.buffer, 0, 8); resolve(); });
    const t0 = now();
    for (let round = 0; round < ROUNDS; round++) {
        const p = new Promise((res) => { resolve = res; });
        os.write(pipes[round % NF][1], buf.buffer, 0, 1);
        await p;
    }
    out("fd_one_of_256", (now() - t0) * 1e6 / ROUNDS, "ns/event");
    for (const [r, w] of pipes) { os.setReadHandler(r, null); os.close(r); os.close(w); }
}

if (want("signal_latency")) {
    const N = 40;
    let total = 0, worst = 0;
    for (let k = 0; k < N; k++) {
        const t0 = now();
        await new Promise((done) => {
            os.signal(os.SIGUSR1, () => { const d = now() - t0; total += d; if (d > worst) worst = d; done(); });
            os.kill(os.getpid(), os.SIGUSR1);
        });
    }
    os.signal(os.SIGUSR1, null);
    out("signal_latency_mean", total / N, "ms");
    out("signal_latency_worst", worst, "ms");
}

if (want("idle_wakeups_signal_handler")) {
    // a process that only waits, with a signal handler installed: how often does it wake?
    os.signal(os.SIGUSR2, () => {});
    const a = waits();
    await sleep(1000);
    const b = waits();
    os.signal(os.SIGUSR2, null);
    out("idle_wakeups_signal_handler", (b - a), "wakeups/s");
}

if (want("idle_wakeups_plain")) {
    const a = waits();
    await sleep(1000);
    const b = waits();
    out("idle_wakeups_plain", (b - a), "wakeups/s");
}

const workerSrc = (body) => {
    const p = "/tmp/dynajs_probe_worker_" + os.getpid() + "_" + Math.floor(now() * 1000) + ".js";
    const f = std.open(p, "w");
    f.puts(body);
    f.close();
    return p;
};

if (want("worker_pingpong")) {
    const path = workerSrc(`import * as os from "os"; const p = os.Worker.parent; p.onmessage = (e) => { if (e.data === "bye") { p.onmessage = null; return; } p.postMessage(e.data + 1); };`);
    const w = new os.Worker(path);
    const N = 20000;
    const t0 = now();
    await new Promise((done) => {
        w.onmessage = (e) => { if (e.data >= N) done(); else w.postMessage(e.data); };
        w.postMessage(0);
    });
    out("worker_pingpong", (now() - t0) * 1e6 / N, "ns/roundtrip");
    w.postMessage("bye");
    w.onmessage = null;
    os.remove(path);
}

if (want("worker_flood")) {
    const N = 100000;
    const path = workerSrc(`import * as os from "os"; const p = os.Worker.parent; p.onmessage = (e) => { for (let i = 0; i < ${N}; i++) p.postMessage(i); p.onmessage = null; };`);
    const w = new os.Worker(path);
    let got = 0;
    const t0 = now();
    await new Promise((done) => { w.onmessage = () => { if (++got === N) done(); }; w.postMessage("go"); });
    out("worker_flood", (now() - t0) * 1e6 / N, "ns/message");
    w.onmessage = null;
    os.remove(path);
}

if (want("idle_wakeups_live_worker")) {
    const path = workerSrc(`import * as os from "os"; const p = os.Worker.parent; p.onmessage = (e) => { p.onmessage = null; };`);
    const w = new os.Worker(path);
    w.onmessage = () => {};
    await sleep(50);
    const a = waits();
    await sleep(1000);
    const b = waits();
    out("idle_wakeups_live_worker", (b - a), "wakeups/s");
    w.postMessage("bye");
    w.onmessage = null;
    os.remove(path);
}

if (want("worker_exit_latency")) {
    // a worker that finishes at once: how long until the parent can observe that it is gone
    const path = workerSrc(`import * as os from "os"; os.Worker.parent.postMessage("done");`);
    const N = 10;
    let total = 0;
    for (let k = 0; k < N; k++) {
        const t0 = now();
        const w = new os.Worker(path);
        await new Promise((done) => { w.onmessage = () => { w.onmessage = null; done(); }; });
        total += now() - t0;
    }
    out("worker_spawn_to_first_message", total / N, "ms");
    os.remove(path);
}
