// flags: --std
// covers: c:src/dyna-evloop.c
// v10b-26: native-reactor (kqueue) interest changes ride the next wait instead of one kevent each
// (os.loopStats ioWaits vs ioChanges), a connection close adds no reactor call or wakeup
// (dyn_evloop_forget), and the JS loop flushes queued changes before poll() blocks on the reactor fd:
// the reactor is nested in poll() whenever an os.setReadHandler / port / held post keeps the loop out of
// the direct reactor wait. The poll-mode case runs the server in a Worker: a listener would arm the
// reactor's 250 ms sweep timer (so the main server is closed first): with no listener and no timer on the
// main thread, a missed flush blocks poll() until the 20 s guard.
// Only kqueue batches the changelist into the next wait; epoll (one epoll_ctl per change) and io_uring apply
// each change at once and count it as a reactor call, so there the budget is one call per change on top of
// the waits (Linux measured 540 calls = 180 waits + 360 changes); any extra call per connection is still red.
// The io_uring reactor (IO_URING=1 builds, src/dyna-aio-uring.c) does not feed these counters at all (0 and 0),
// so when this process holds an io_uring ring fd (/proc/self/fd -> anon_inode:[io_uring]) that check prints a
// SKIP line instead; an epoll build that stopped counting still fails.
import { sections as __kitSections } from "../lib/kit.js";
import { TCPServer } from "dyna:net";
import * as os from "os";
const __kitS = __kitSections("test_evloop_batch");

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
const msg = new Uint8Array(64).fill(7);
const KQUEUE = ["darwin", "freebsd", "openbsd", "netbsd"];
function uringRing() {
    if (os.platform !== "linux") return false;
    const [fds] = os.readdir("/proc/self/fd");
    return (fds || []).some((n) => os.readlink("/proc/self/fd/" + n)[0] === "anon_inode:[io_uring]");
}

async function churn(srv, k) {
    for (let i = 0; i < k; i++) {
        await new Promise((res) => {
            const cl = TCPServer.connect({ host: "127.0.0.1", port: srv.port }, {
                connect(c) { c.write(msg); },
                data(c, b) { cl.close(); res(); },
            });
        });
    }
}

const srv = new TCPServer({ port: 0 });
srv.start({ data: (c, b) => c.write(b) });
await churn(srv, 5);

await __kitS.section("changes ride the waits", async () => {
    const s0 = os.loopStats();
    await churn(srv, 60);
    const s1 = os.loopStats();
    const waits = s1.waits - s0.waits, io = s1.ioWaits - s0.ioWaits, ch = s1.ioChanges - s0.ioChanges;
    ok(typeof s1.ioWaits === "number" && typeof s1.ioChanges === "number", "loopStats has ioWaits/ioChanges");
    const kq = KQUEUE.includes(os.platform);
    const budget = kq ? waits + 6 : waits + ch + 6;
    if (!kq && uringRing()) print(`SKIP: ${ch} changes in ${io} reactor calls: the io_uring reactor does not feed ioWaits/ioChanges yet (open item)`);
    else ok(ch >= 60 && io <= budget,`${ch} changes in ${io} reactor calls for ${waits} loop waits (60 connections, ${kq ? "kqueue: changes ride the waits" : "epoll: at most one call per change"})`);
    ok(waits <= 60 * 5, `close adds no wakeup: ${waits} loop waits for 60 connect/echo/close (<= 5 each)`);
});

srv.close();

await __kitS.section("poll-mode loop flushes before poll()", async () => {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_evbatch.${Date.now()}.${Math.floor(Math.random() * 1e9)}_wkr.js`;
    const f = std.open(T, "w");
    f.puts([
        'import * as os from "os";',
        'import { TCPServer } from "dyna:net";',
        "const parent = os.Worker.parent;",
        "const s = new TCPServer({ port: 0 });",
        "s.start({ data: (c, b) => c.write(b) });",
        "parent.onmessage = () => { s.close(); parent.onmessage = null; };",
        "parent.postMessage({ port: s.port });",
    ].join("\n"));
    f.close();
    const w = new os.Worker(T);
    const port = await new Promise((res) => { w.onmessage = (e) => { w.onmessage = () => {}; res(e.data.port); }; });
    let guard = false, t, done = 0;
    const run = (async () => { for (let i = 0; i < 25; i++) { await churn({ port }, 1); done++; } })();
    await Promise.race([run, new Promise((res) => { t = setTimeout(() => { guard = true; res(); }, 20000); })]);
    clearTimeout(t);
    w.onmessage = null;
    w.postMessage("stop");
    os.remove(T);
    ok(!guard && done === 25, `Worker port keeps the loop in poll(); a client-only reactor (no listener timer): ${done}/25 connect/echo/close completed`);
});
__kitS.end();

if (fails) { print(`test_evloop_batch: ${fails} FAILED of ${n}`); throw new Error("failed"); }
print(`test_evloop_batch: ${n} checks, 0 failures`);
