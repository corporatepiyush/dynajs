// flags: --std
// covers: dyna:dns.DNSResolver
// F-DNS-TTL-HANG: an in-flight query must keep its resolver alive. Before the fix the resolver, its pending
// resolve/reject and the awaiting async frame formed a cycle the GC collected, so the promise never settled and
// the event loop waited forever (review_dns_ttl.js hung). Each case runs under the kit's timeoutMs, a timer that
// references no frame, so a lost query fails here instead of hanging. Idle resolvers must stay collectable.
import * as std from "std";
import { UDPSocket } from "dyna:net";
import { DNSResolver } from "dyna:dns";
import { suite } from "../lib/kit.js";

const t = suite("test_dns_dns_resolver_inflight_gc");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let silent = null, answer = null;

t.before(() => {
    silent = new UDPSocket({ port: 0, host: "127.0.0.1" });
    silent.start({ message: () => {} });
    answer = new UDPSocket({ port: 0, host: "127.0.0.1" });
    answer.start({ message: (bytes, from) => {
        const q = new Uint8Array(bytes);
        let end = 12;
        while (end < q.length && q[end] !== 0) end += 1 + q[end];
        const a = [q[0], q[1], 0x81, 0x80, 0, 1, 0, 1, 0, 0, 0, 0];
        for (const b of q.subarray(12, end + 5)) a.push(b);
        a.push(0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 0, 0, 7);
        setTimeout(() => answer.send(new Uint8Array(a).buffer, from.address, from.port), 80);
    }});
});
t.after(() => { if (silent) silent.close(); if (answer) answer.close(); });

t.test("a dropped resolver with a silent server still times out after a GC", async (a) => {
    const got = await (async () => {
        const r = new DNSResolver({ server: "127.0.0.1", port: silent.port, timeoutMs: 300 });
        setTimeout(() => std.gc(), 40);
        try { await r.lookup("silent.test"); return "resolved"; } catch (e) { return String(e.message || e); }
    })();
    a.eq(got, "query timed out");
}, { timeoutMs: 3000 });

t.test("a dropped resolver still delivers a late answer after a GC", async (a) => {
    const got = await (async () => {
        const r = new DNSResolver({ server: "127.0.0.1", port: answer.port, timeoutMs: 2000 });
        setTimeout(() => std.gc(), 30);
        const recs = await r.lookup("answer.test");
        return recs.map((x) => x.address).join(",");
    })();
    a.eq(got, "10.0.0.7");
}, { timeoutMs: 3000 });

t.test("a callback query on a dropped resolver survives a GC", async (a) => {
    const cbp = new Promise((resolve) => {
        const r = new DNSResolver({ server: "127.0.0.1", port: answer.port, timeoutMs: 2000 });
        r.query("cb.test", 1, (err, recs) => resolve([err, recs && recs[0] && recs[0].address]));
    });
    setTimeout(() => std.gc(), 30);
    const cb = await cbp;
    a.eq(cb[0], null, "no error");
    a.eq(cb[1], "10.0.0.7", "the answer arrived");
}, { timeoutMs: 3000 });

t.test("an idle resolver (every query settled, never closed) is collected: the in-flight pin is released", async (a) => {
    let fired = 0;
    const fr = new FinalizationRegistry(() => { fired++; });
    await (async () => {
        const r = new DNSResolver({ server: "127.0.0.1", port: answer.port, timeoutMs: 2000 });
        fr.register(r, 1);
        await r.lookup("idle.test");
    })();
    for (let i = 0; i < 40 && !fired; i++) { await sleep(10); std.gc(); }
    a.eq(fired, 1);
}, { timeoutMs: 3000 });

await t.run();
