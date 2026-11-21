// covers: dyna:async.Channel
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_async_channel");
import { pmap, Pool, Channel, Queue, Semaphore, sleep, withTimeout, retry, debounce, throttle } from "dyna:async";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function sleepMs(ms) { return new Promise(res => setTimeout(res, ms)); }
function withTestTimeout(p, ms, what) {
    let timer;
    const deadline = new Promise((_, rej) => { timer = setTimeout(() => rej(new Error("TIMEOUT: " + what)), ms); });
    return Promise.race([p, deadline]).finally(() => clearTimeout(timer));
}
async function t(fn, ms) { await withTestTimeout(fn(), ms, fn.name); }

await __kitS.section("direct handoff to a parked recv", async () => {
await t(async function channelMpsc() {
    const ch = new Channel();
    const send = ch.send("v1");
    eq(JSON.stringify(await ch.recv()), '{"value":"v1","done":false}', "direct handoff to a parked recv");
    await send;
}, 5000);
});

await __kitS.section("send blocks at capacity (backpressure, no silent drop)", async () => {
await t(async function channelCapacity() {
    const ch = new Channel(1);
    await ch.send("a");
    let blocked = true;
    const second = ch.send("b").then(() => { blocked = false; });
    await sleepMs(40);
    assert(blocked, "send blocks at capacity (backpressure, no silent drop)");
    eq(ch.length, 1, "the accepted value is buffered");
    eq(JSON.stringify(await ch.recv()), '{"value":"a","done":false}', "recv drains");
    await second;
    eq(ch.length, 1, "the blocked send was admitted when room appeared");
    eq(JSON.stringify(await ch.recv()), '{"value":"b","done":false}', "and is delivered in order");
}, 5000);
});

await __kitS.section("close resolves a parked recv with done", async () => {
await t(async function channelCloseWhilePending() {
    const ch = new Channel();
    const parked = ch.recv();
    await sleepMs(20);
    ch.close();
    eq(JSON.stringify(await parked), '{"done":true}', "close resolves a parked recv with done");
    ch.close();
    eq(JSON.stringify(await ch.recv()), '{"done":true}', "recv after close is done");
    let threw = false;
    try { await ch.send("x"); } catch (e) { threw = true; }
    assert(threw, "send after close rejects");
}, 5000);
});

await __kitS.section("close rejects a send that was blocked on capacity (its value was never accepted)", async () => {
await t(async function channelCloseRejectsBlockedSend() {
    const ch = new Channel(1);
    await ch.send("a");
    const blocked = ch.send("b");
    blocked.catch(() => {});
    await sleepMs(20);
    ch.close();
    let threw = false;
    try { await blocked; } catch (e) { threw = true; }
    assert(threw, "close rejects a send that was blocked on capacity (its value was never accepted)");
    eq(ch.length, 1, "and the previously accepted value stays buffered");
    eq(JSON.stringify(await ch.recv()), '{"value":"a","done":false}', "the drain still works after close");
    eq(JSON.stringify(await ch.recv()), '{"done":true}', "then done");
}, 5000);
});

await __kitS.section("a second concurrent recv is refused (MPSC: one consumer)", async () => {
await t(async function channelSingleConsumer() {
    const ch = new Channel();
    ch.recv().catch(() => {});
    let threw = false;
    try { await ch.recv(); } catch (e) { threw = true; }
    assert(threw, "a second concurrent recv is refused (MPSC: one consumer)");
}, 5000);
});

await __kitS.section("for-await drains and stops at done", async () => {
await t(async function channelAsyncIterator() {
    const ch = new Channel(4);
    await ch.send(1); await ch.send(2); await ch.send(3);
    ch.close();
    const seen = [];
    for await (const msg of ch) seen.push(msg);
    eq(JSON.stringify(seen), "[1,2,3]", "for-await drains and stops at done");
}, 5000);
});

await __kitS.section("Channel(2.5) throws RangeError", async () => {
await t(async function channelQueueIntegerGuards() {
    let threw = false;
    try { new Channel(2.5); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "Channel(2.5) throws RangeError");
    threw = false;
    try { new Queue(2.5); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "Queue(2.5) throws RangeError");
    threw = false;
    try { new Channel(0); } catch (e) { threw = true; }
    assert(threw, "Channel(0) still refuses");
}, 5000);
});

await __kitS.section("200 sends delivered through a cap-4 channel", async () => {
await t(async function channelChurn() {
    const ch = new Channel(4);
    (async () => { for (let i = 0; i < 200; i++) await ch.send(i); ch.close(); })();
    let total = 0, count = 0;
    for await (const v of ch) { total += v; count++; await sleepMs(1); }
    eq(count, 200, "200 sends delivered through a cap-4 channel");
    eq(total, 19900, "and in order");
}, 10000);
});
__kitS.end();

print("test_async: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_async failed");
