// flags: --std
// A throw from a DNSResolver.query callback is swallowed like any other net
// handler throw: COUNTED by net.swallowedHandlerThrows() (the documented only
// diagnostic) and CLEARED from the context. It used to be dropped uncounted
// with the exception left pending.
import { DNSResolver, TCPServer, swallowedHandlerThrows } from "dyna:net";

const before = swallowedHandlerThrows();
const dead = new DNSResolver({ server: "127.0.0.1", port: 9, timeoutMs: 150 });
let fired = 0;
dead.query("nowhere.test", 1, () => { fired++; throw new Error("boom from a dns callback"); });
dead.query("elsewhere.test", 1, () => { fired++; throw new TypeError("second"); });

await new Promise((r) => setTimeout(r, 700));
let later = "ok";
try { JSON.parse("{}"); } catch (e) { later = "saw: " + e.message; }
dead.close();

// The TCP CLIENT connect report had the same defect: every other TCP handler
// was counted, a throwing connect handler was freed uncounted.
const mid = swallowedHandlerThrows();
let connectFired = 0;
const cli = TCPServer.connect({ host: "127.0.0.1", port: 9 }, {
    connect() { connectFired++; throw new Error("boom from a connect handler"); },
});
await new Promise((r) => setTimeout(r, 400));
try { cli.close(); } catch (e) { }
if (connectFired !== 1) throw new Error("test_net_dns_cb_throw: the connect handler fired " + connectFired + " times, want 1");
if (swallowedHandlerThrows() - mid !== 1)
    throw new Error("test_net_dns_cb_throw: a throwing TCP connect handler must be counted, moved by " + (swallowedHandlerThrows() - mid));

const delta = mid - before;
if (fired !== 2) throw new Error("test_net_dns_cb_throw: expected both callbacks to fire, got " + fired);
if (delta !== 2) throw new Error("test_net_dns_cb_throw: 2 callback throws must be counted, swallowedHandlerThrows moved by " + delta);
if (later !== "ok") throw new Error("test_net_dns_cb_throw: a pending exception leaked into later code (" + later + ")");
print("test_net_dns_cb_throw: 2 DNS callback throws and 1 TCP connect handler throw counted and cleared");
