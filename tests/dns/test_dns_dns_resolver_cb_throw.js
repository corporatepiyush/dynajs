// covers: dyna:dns.DNSResolver
// A throw from a DNSResolver.query callback is swallowed like any other net
// handler throw: COUNTED by net.swallowedHandlerThrows() (the documented only
// diagnostic) and CLEARED from the context. It used to be dropped uncounted
// with the exception left pending.
import { TCPServer, swallowedHandlerThrows } from "dyna:net";
import { DNSResolver } from "dyna:dns";
import { suite } from "../lib/kit.js";

const t = suite("test_net_dns_cb_throw");

t.test("DNS callback throws are counted and cleared", async (a) => {
    const before = swallowedHandlerThrows();
    const dead = new DNSResolver({ server: "127.0.0.1", port: 9, timeoutMs: 150 });
    let fired = 0;
    dead.query("nowhere.test", 1, () => { fired++; throw new Error("boom from a dns callback"); });
    dead.query("elsewhere.test", 1, () => { fired++; throw new TypeError("second"); });
    await new Promise((r) => setTimeout(r, 700));
    let later = "ok";
    try { JSON.parse("{}"); } catch (e) { later = "saw: " + e.message; }
    dead.close();
    const delta = swallowedHandlerThrows() - before;
    a.eq(fired, 2, "both callbacks fire");
    a.eq(delta, 2, "2 callback throws are counted by swallowedHandlerThrows");
    a.eq(later, "ok", "no pending exception leaks into later code");
});

// The TCP CLIENT connect report had the same defect: every other TCP handler
// was counted, a throwing connect handler was freed uncounted.
t.test("a throwing TCP client connect handler is counted", async (a) => {
    const mid = swallowedHandlerThrows();
    let connectFired = 0;
    const cli = TCPServer.connect({ host: "127.0.0.1", port: 9 }, {
        connect() { connectFired++; throw new Error("boom from a connect handler"); },
    });
    await new Promise((r) => setTimeout(r, 400));
    try { cli.close(); } catch (e) { }
    a.eq(connectFired, 1, "the connect handler fires once");
    a.eq(swallowedHandlerThrows() - mid, 1, "a throwing TCP connect handler is counted");
});

await t.run();
