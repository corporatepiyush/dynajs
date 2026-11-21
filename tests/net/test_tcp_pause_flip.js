// covers: integration dyna:net.TCPServer c:src/dyna-evloop.c -- pause/resume flips inside one turn reach the kqueue batch
// net-8 (2026-11-21): pause(), resume(), pause() on a connection in one turn left the kernel watching the
// socket: the queued EV_DELETE was overwritten by the resume's EV_ADD, and the second pause then dropped
// the pending entry as if the knote had never been added (see tests/core/test_evloop_interest_flip.c). The
// paused connection's readable data, or the peer's RST, woke every poll at once: the loop spun at 100% of a
// core while nothing was delivered, which showed up as a "hang" of the paused-then-reset connection.
// Pinned: while paused after the flips, the process stays idle (CPU well under the wall time) and no data
// event fires; resume() delivers the held data; the peer closing while paused is reported once after resume (dyna has no RST
// client; a FIN leaves the same readable-EOF knote as the RST of the original report).
// Mutant (red): the kqueue drop path back to unconditional (CPU ~= wall time while paused).
import { TCPServer } from "dyna:net";
import { perfCheck } from "../lib/perf.js";
import { rusage } from "dyna:sys";
import { suite } from "../lib/kit.js";

const t = suite("test_tcp_pause_flip");
const URING = await import("dyna:uring").then(() => true, () => false);
const PAUSE_SKIP = "pause() is a no-op on the io_uring backend (dynajs.d.ts, TCPConnection.pause)";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const cpuMs = () => { const r = rusage(); return (r.user + r.system) * 1000; };
const dial = (port) => new Promise((r, j) => TCPServer.connect({ host: "127.0.0.1", port }, {
    connect(c, e) { if (e) j(new Error(String(e))); else r(c); },
}));

for (const flips of [["pause", "resume", "pause"], ["pause", "resume", "pause", "resume", "pause"]]) {
    t.test(flips.join(",") + " in one turn: idle while paused, data after resume", async (a) => {
        if (URING) return a.skip(PAUSE_SKIP);
        const got = [];
        let server = null;
        const s = new TCPServer({ port: 0 });
        s.start({ data(c, b) { got.push(new TextDecoder().decode(b)); if (!server) { server = c; for (const f of flips) c[f](); } } });
        const client = await dial(s.port);
        client.write("one");
        for (let i = 0; i < 100 && !server; i++) await sleep(5);
        a.ok(server !== null && server.paused, "the connection is paused after the flips");
        client.write("two");
        await sleep(30);
        const c0 = cpuMs(), w0 = Date.now();
        await sleep(300);
        const used = cpuMs() - c0, wall = Date.now() - w0;
        // wall-clock: an idle-vs-spin check; a spinning loop burns CPU equal to the wall time, an idle one near zero, so load cannot make 0.3 ambiguous
        perfCheck(a.ok.bind(a), used < wall * 0.3, `paused for ${wall} ms used ${used.toFixed(1)} ms of CPU (a stale kernel watch spins: ~${wall})`);
        a.eq(got.join("|"), "one", "no data event while paused");
        server.resume();
        for (let i = 0; i < 100 && got.length < 2; i++) await sleep(5);
        a.eq(got.join("|"), "one|two", "resume delivers the held data");
        client.close();
        s.close();
    });
}

t.test("the peer closing while paused after the flips is reported once after resume, without spinning", async (a) => {
    if (URING) return a.skip(PAUSE_SKIP);
    let server = null, closes = 0;
    const s = new TCPServer({ port: 0 });
    s.start({ data(c) { if (!server) { server = c; c.pause(); c.resume(); c.pause(); } }, close() { closes++; } });
    const client = await dial(s.port);
    client.write("x");
    for (let i = 0; i < 100 && !server; i++) await sleep(5);
    client.close();
    await sleep(30);
    const c0 = cpuMs(), w0 = Date.now();
    await sleep(250);
    const used = cpuMs() - c0, wall = Date.now() - w0;
    // wall-clock: an idle-vs-spin check; a spinning loop burns CPU equal to the wall time, an idle one near zero, so load cannot make 0.3 ambiguous
    perfCheck(a.ok.bind(a), used < wall * 0.3, `paused after the peer went away: ${used.toFixed(1)} ms CPU over ${wall} ms`);
    server.resume();
    for (let i = 0; i < 200 && closes === 0; i++) await sleep(5);
    a.eq(closes, 1, "close fires once after resume");
    s.close();
});

await t.run();
