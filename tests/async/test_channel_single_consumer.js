// flags: --std
// Channel is single-consumer: a second concurrent for-await fails AT its loop
// with a TypeError that names the cause, instead of as a stray rejection.
import { suite } from "../lib/kit.js";
import { Channel } from "dyna:async";

const t = suite("channel_single_consumer");
const tick = () => new Promise((r) => setTimeout(r, 0));

t.test("a second concurrent for-await throws at the loop", async (a) => {
    const ch = new Channel(4);
    const seen = [];
    const first = (async () => { for await (const v of ch) seen.push(v); })();
    await tick();
    const err = await a.rejects(async () => { for await (const v of ch) seen.push("second:" + v); }, { type: TypeError, message: /single consumer/ });
    a.match(String(err && err.message), /for-await/);
    await ch.send(1);
    await ch.send(2);
    ch.close();
    await first;
    a.deep(seen, [1, 2], "the first consumer was not disturbed");
});

t.test("sequential loops are fine after a break", async (a) => {
    const ch = new Channel(4);
    await ch.send("a");
    await ch.send("b");
    for await (const v of ch) { a.eq(v, "a"); break; }
    const rest = [];
    ch.close();
    for await (const v of ch) rest.push(v);
    a.deep(rest, ["b"]);
});

t.test("sequential loops are fine after a throw in the body", async (a) => {
    const ch = new Channel(4);
    await ch.send(1);
    await ch.send(2);
    await a.rejects(async () => { for await (const v of ch) throw new Error("body " + v); }, "body 1");
    ch.close();
    const rest = [];
    for await (const v of ch) rest.push(v);
    a.deep(rest, [2]);
});

t.test("a drained closed channel can be iterated again", async (a) => {
    const ch = new Channel(2);
    await ch.send(1);
    ch.close();
    const one = [], two = [];
    for await (const v of ch) one.push(v);
    for await (const v of ch) two.push(v);
    a.deep(one, [1]);
    a.deep(two, []);
});

t.test("recv keeps its own single-consumer refusal", async (a) => {
    const ch = new Channel(1);
    const p = ch.recv();
    await a.rejects(ch.recv(), /already pending/);
    await ch.send(7);
    a.deep(await p, { value: 7, done: false });
    ch.close();
});

await t.run();
