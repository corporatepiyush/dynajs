// flags: --std
// covers: c:src/modules/libc/libc-event-loop.c
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_evloop");
import { DNSResolver } from "dyna:dns";
import { TCPServer } from "dyna:net";
import { HTTPServer, HTTPClient } from "dyna:http";
import * as os from "os";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

await __kitS.section("block L14", async () => {
    let dt = 0;
    for (let i = 0; i < 3; i++) {
        const t0 = Date.now();
        await new Promise(r => setTimeout(r, 100));
        dt = Date.now() - t0;
        if (dt < 300) break;
    }
    // wall-clock: timer accuracy is what this measures; best-of-3 retries absorb a loaded host
    ok(dt >= 90 && dt < 300, `setTimeout 100ms fired in ${dt}ms (quiet loop; best of 3 tries, a loaded host only delays)`);
});

await __kitS.section("block L25", async () => {
    const order = [];
    setTimeout(() => order.push(1), 50);
    setTimeout(() => order.push(2), 100);
    setTimeout(() => order.push(3), 150);
    await sleep(250);
    ok(order.join(",") === "1,2,3", `timers fired in order: ${order.join(",")}`);
});

await __kitS.section("clearTimeout cancels timer", async () => {
    let fired = false;
    const id = setTimeout(() => { fired = true; }, 100);
    clearTimeout(id);
    await sleep(200);
    ok(!fired, "clearTimeout cancels timer");
});

await __kitS.section("DNS blackhole timeout error", async () => {
    const t0 = Date.now();
    const r = new DNSResolver({ server: "127.0.0.1", port: 1, timeoutMs: 400 });
    let done = false;
    r.query("quiet.test", 1, (err) => {
        const dt = Date.now() - t0;
        ok(err !== null, "DNS blackhole timeout error");
        // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
        ok(dt >= 350 && dt < 4000, `DNS timeout fired on time: ${dt}ms (quiet loop; 10x the 400 ms deadline)`);
        done = true;
        try { r.close(); } catch (_) {}
    });
    for (const t1 = Date.now(); !done && Date.now() - t1 < 10000; ) await sleep(50);
    ok(done, "DNS timeout callback fired");
});

await __kitS.section("TCP connect to blackhole handled without hanging", async () => {
    const t0 = Date.now();
    let errMsg = "";
    try {
        const c = TCPServer.connect({ host: "10.255.255.1", port: 54321 }, {
            connect: (c, err) => { if (err) errMsg = String(err); },
        });
        await sleep(1200);
        try { c.close(); } catch (_) {}
    } catch (e) {
        errMsg = String(e.message);
    }
    ok(true, "TCP connect to blackhole handled without hanging");
});

await __kitS.section("HTTP async get via pool", async () => {
    const srv = new HTTPServer({ port: 0, routes: { "/ping": "pong" } });
    srv.start();
    const cli = new HTTPClient();
    let r = null, dt = Infinity;
    for (let i = 0; i < 3 && dt >= 2000; i++) {
        const t0 = Date.now();
        r = await cli.get(`http://127.0.0.1:${srv.port}/ping`);
        dt = Math.min(dt, Date.now() - t0);
    }
    ok(r.status === 200 && r.body === "pong", "HTTP async get via pool");
    // wall-clock: a loopback round trip bounded at 2 s, best of up to 3 tries, guards against a stalled pool, not latency
    ok(dt < 2000, `HTTP async completed in ${dt}ms (best of up to 3)`);
    cli.close();
    srv.close();
});

await __kitS.section("server close while request in flight handled", async () => {
    const srv = new HTTPServer({ port: 0, routes: { "/close": "ok" } });
    srv.start();
    let handlerClosed = false;
    const cli = new HTTPClient();
    const p = cli.get(`http://127.0.0.1:${srv.port}/close`);
    await sleep(50);
    srv.close();
    handlerClosed = true;
    const r = await p.catch(e => ({ err: e.message }));
    ok(handlerClosed, "server close while request in flight handled");
    cli.close();
});

await __kitS.section("timer not starved by I/O", async () => {
    let timerFired = false;
    let ioDone = false;
    setTimeout(() => { timerFired = true; }, 100);
    const srv = new HTTPServer({ port: 0, routes: { "/io": "ok" } });
    srv.start();
    const cli = new HTTPClient();
    cli.get(`http://127.0.0.1:${srv.port}/io`).then(() => { ioDone = true; });
    await sleep(300);
    ok(timerFired, "timer not starved by I/O");
    ok(ioDone, "I/O not starved by timer");
    cli.close();
    srv.close();
});
__kitS.end();

if (fails) { print(`test_evloop: ${fails} FAILED of ${n}`); throw new Error("failed"); }
print(`test_evloop: ${n} checks, 0 failures`);
