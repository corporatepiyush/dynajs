// flags: --std
// The event loop wakes for work and for nothing else, and one wake-up serves
// everything that is ready. os.loopStats() (dynajs.d.ts, module "os") is the
// instrument: `waits` counts blocking waits entered, so a loop that polls on a
// clock or pays one wait per event shows up as a count, not as a duration.
import * as os from "os";
import * as std from "std";
import { args, Exec } from "dyna:sys";
import { TCPServer } from "dyna:net";

let n = 0, failed = 0;
function ok(cond, what, detail) {
    n++;
    if (!cond) { failed++; print("  FAIL  " + what + (detail === undefined ? "" : "  [" + detail + "]")); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const stats = () => os.loopStats();
const tmp = (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "");
let seq = 0;
function workerFile(body) {
    const p = `${tmp}/dynajs_evloop_${os.getpid()}_${seq++}.js`;
    const f = std.open(p, "w");
    f.puts(body);
    f.close();
    return p;
}

// 1. An idle process with a signal handler installed does not tick.
{
    os.signal(os.SIGUSR2, () => {});
    const a = stats().waits;
    await sleep(400);
    const d = stats().waits - a;
    os.signal(os.SIGUSR2, null);
    ok(d <= 3, "a 400 ms idle wait with a signal handler installed enters the kernel a handful of times, not on a clock", d);
}

// 2. A signal is delivered while the loop is blocked on a far-away timer: the
//    handler runs first. ORDER, not duration.
{
    const order = [];
    const far = setTimeout(() => order.push("timer"), 5000);
    await new Promise((done) => {
        os.signal(os.SIGUSR1, () => { order.push("signal"); done(); });
        os.exec(["/bin/sh", "-c", `sleep 0.05; kill -USR1 ${os.getpid()}`], { block: false });
    });
    clearTimeout(far);
    os.signal(os.SIGUSR1, null);
    ok(order[0] === "signal", "a signal from another process wakes a loop blocked on a 5 s timer", order.join(","));
    ok(stats().signals >= 1, "loopStats counts the signal handler run", stats().signals);
}

// 3. Every ready fd handler is served by ONE wake-up, with microtasks drained
//    between handlers exactly as if each had its own turn.
{
    const NF = 48;
    const pipes = [];
    for (let k = 0; k < NF; k++) pipes.push(os.pipe());
    const buf = new Uint8Array(4);
    const order = [];
    let seen = 0, done;
    const all = new Promise((r) => { done = r; });
    pipes.forEach(([r], i) => os.setReadHandler(r, () => {
        os.read(r, buf.buffer, 0, 4);
        order.push("h" + i);
        Promise.resolve().then(() => order.push("m" + i));
        if (++seen === NF) done();
    }));
    for (const [, w] of pipes) os.write(w, buf.buffer, 0, 1);
    const a = stats();
    await all;
    const b = stats();
    ok(b.fdEvents - a.fdEvents === NF, "all " + NF + " ready handlers ran", b.fdEvents - a.fdEvents);
    ok(b.waits - a.waits <= 3, NF + " ready fds cost a few waits, not one each", b.waits - a.waits);
    let interleaved = order.length >= 2 * NF - 1;
    for (let i = 0; i < NF - 1 && interleaved; i++) {
        const h = order.indexOf("h" + i), m = order.indexOf("m" + i), hn = order.indexOf("h" + (i + 1));
        interleaved = h >= 0 && m > h && hn > m;
    }
    ok(interleaved, "each handler's microtask runs before the next handler", order.slice(0, 8).join(","));
    for (const [r, w] of pipes) { os.setReadHandler(r, null); os.close(r); os.close(w); }
}

// 4. A handler that removes a later, also-ready handler: the removed one never fires.
{
    const [r1, w1] = os.pipe(), [r2, w2] = os.pipe(), [r3, w3] = os.pipe();
    const buf = new Uint8Array(4);
    const calls = [];
    let done;
    const fin = new Promise((r) => { done = r; });
    os.setReadHandler(r1, () => { os.read(r1, buf.buffer, 0, 4); calls.push(1); os.setReadHandler(r2, null); os.close(r2); });
    os.setReadHandler(r2, () => { calls.push(2); });
    os.setReadHandler(r3, () => { os.read(r3, buf.buffer, 0, 4); calls.push(3); done(); });
    os.write(w1, buf.buffer, 0, 1);
    os.write(w2, buf.buffer, 0, 1);
    os.write(w3, buf.buffer, 0, 1);
    await fin;
    await sleep(20);
    ok(calls.join(",") === "1,3", "a handler cleared by an earlier handler in the same wake-up does not run", calls.join(","));
    os.setReadHandler(r1, null);
    os.setReadHandler(r3, null);
    for (const fd of [r1, w1, w2, r3, w3]) os.close(fd);
}

// 5. Worker messages: a burst is delivered in order, many per wake-up, each
//    message's microtask before the next message; clearing onmessage mid-burst
//    stops delivery at once.
{
    const N = 600;
    const path = workerFile(`import * as os from "os"; const p = os.Worker.parent;
p.onmessage = (e) => { for (let i = 0; i < ${N}; i++) p.postMessage(i); p.onmessage = null; };`);
    const w = new os.Worker(path);
    const order = [];
    let got = 0, inOrder = true, done;
    const fin = new Promise((r) => { done = r; });
    w.onmessage = (e) => {
        if (e.data !== got) inOrder = false;
        if (got < 4) { order.push("d" + got); const k = got; Promise.resolve().then(() => order.push("m" + k)); }
        if (++got === N) done();
    };
    const a = stats();
    w.postMessage("go");
    await fin;
    const b = stats();
    ok(inOrder, "a burst of " + N + " worker messages arrives in order");
    ok(b.messages - a.messages === N, "loopStats counted every delivered message", b.messages - a.messages);
    ok(b.waits - a.waits < N / 4, "the burst cost far fewer waits than messages", b.waits - a.waits);
    ok(order.join(",") === "d0,m0,d1,m1,d2,m2,d3,m3", "each message's microtask runs before the next message", order.join(","));
    w.onmessage = null;
    os.remove(path);

    const path2 = workerFile(`import * as os from "os"; const p = os.Worker.parent;
p.onmessage = (e) => { for (let i = 0; i < 200; i++) p.postMessage(i); p.onmessage = null; };`);
    const w2 = new os.Worker(path2);
    let seen = 0;
    await new Promise((r) => {
        w2.onmessage = () => { if (++seen === 10) { w2.onmessage = null; r(); } };
        w2.postMessage("go");
    });
    await sleep(50);
    ok(seen === 10, "clearing onmessage inside a burst stops delivery at that message", seen);
    os.remove(path2);
}

// 6. A live but idle Worker does not make its parent tick, and the parent
//    learns that a Worker thread ended without polling for it.
{
    const path = workerFile(`import * as os from "os"; const p = os.Worker.parent; p.onmessage = (e) => { p.onmessage = null; };`);
    const w = new os.Worker(path);
    w.onmessage = () => {};
    await sleep(60);
    const a = stats().waits;
    await sleep(400);
    const d = stats().waits - a;
    ok(d <= 3, "400 ms beside a live idle Worker costs a handful of waits, not one every 20 ms", d);
    const b = stats().waits;
    w.postMessage("bye");
    await sleep(300);
    const d2 = stats().waits - b;
    ok(d2 <= 5, "a Worker ending does not start a polling interval in its parent", d2);
    w.onmessage = null;
    os.remove(path);
}

// 7. A process whose only live thing is a Worker that then ends must exit:
//    nothing else will wake its loop, so this hangs if the ending is not
//    announced. Judged by the child's exit, bounded by Exec's timeout.
{
    const worker = workerFile(`let x = 1 + 1;`);
    const main = workerFile(`import * as os from "os"; const w = new os.Worker(${JSON.stringify(worker)}); w.onmessage = () => {}; print("armed");`);
    const r = Exec(args()[0], ["--std", main], { timeoutMs: 15000, encoding: "utf8" });
    ok(/armed/.test(r.stdout || ""), "the child armed its Worker", String(r.stdout).slice(0, 60) + String(r.stderr).slice(0, 120));
    ok(r.code === 0 && !r.timedOut, "a process waiting only on a Worker exits when that Worker ends", "code " + r.code + " timedOut " + r.timedOut + " signal " + r.signal);
    os.remove(worker);
    os.remove(main);
}

// 8. With a network reactor live the loop blocks in the reactor itself; the
//    wake pipe is registered there too, so a signal handler or a live Worker
//    still adds no clock and a signal still arrives at once.
{
    const srv = new TCPServer({ port: 0 });
    srv.start({});
    os.signal(os.SIGUSR2, () => {});
    await sleep(30);
    const a = stats().waits;
    await sleep(400);
    const d = stats().waits - a;
    ok(d <= 3, "a 400 ms idle wait in the reactor with a signal handler installed is not a 50 ms clock", d);
    const path = workerFile(`import * as os from "os"; const p = os.Worker.parent; p.onmessage = (e) => { p.onmessage = null; };`);
    const w = new os.Worker(path);
    w.onmessage = () => {};
    await sleep(60);
    const b = stats().waits;
    await sleep(400);
    const d2 = stats().waits - b;
    ok(d2 <= 3, "a live idle Worker beside the reactor costs a handful of waits, not one every 20 ms", d2);
    w.postMessage("bye");
    w.onmessage = null;
    os.signal(os.SIGUSR2, null);
    const order = [];
    const far = setTimeout(() => order.push("timer"), 5000);
    await new Promise((done) => {
        os.signal(os.SIGUSR1, () => { order.push("signal"); done(); });
        os.exec(["/bin/sh", "-c", `kill -USR1 ${os.getpid()}`], { block: false });
    });
    clearTimeout(far);
    os.signal(os.SIGUSR1, null);
    ok(order[0] === "signal", "a signal wakes a loop blocked in the reactor", order.join(","));
    srv.close();
    os.remove(path);
}

print("test_evloop_wake: " + (n - failed) + " passed, " + failed + " failed");
if (failed) throw new Error("test_evloop_wake: " + failed + " failures");
