/* review_dns_ttl.js -- e7 review, DNSResolver {ttl} cache doc-truth (P60-P71).
 *
 * API.md claims under test, one probe per claim:
 *   - absent/0 ttl disables caching (the default)
 *   - a hit serves from the cache with NO wire query
 *   - the cache key is lowercase(name)|type (case variants share; type splits)
 *   - a hit is served for min(the answer's smallest RR TTL, ttl) seconds
 *   - ttl above 86400 clamps to 86400 (time-bounded: not observable in-probe)
 *   - a TTL-0 answer is never cached; an empty answer is never cached
 *   - N concurrent lookups of an uncached name put N queries on the wire
 *   - each hit hands out a FRESH DEEP COPY (mutations cannot poison)
 *   - cache hits are delivered asynchronously (on a job)
 *
 * A purpose-built raw UDP authority answers per queried NAME (encoded in the
 * label), so one server covers every scenario with an exact wire counter.
 */
import { DNSResolver, UDPSocket } from "dyna:net";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const srv = new UDPSocket({ port: 0, host: "127.0.0.1" });
const queries = {};            /* per-name wire counter */
function pushName(a, name) {
    for (const l of name.split(".")) {
        a.push(l.length);
        for (const ch of l) a.push(ch.charCodeAt(0));
    }
    a.push(0);
}
function pushRR(a, name, type, ttl, rdata) {
    pushName(a, name);
    a.push(type >> 8, type & 0xff, 0, 1);
    a.push((ttl >>> 24) & 0xff, (ttl >> 16) & 0xff, (ttl >> 8) & 0xff, ttl & 0xff);
    a.push((rdata.length >> 8) & 0xff, rdata.length & 0xff);
    for (const b of rdata) a.push(b);
}
srv.start({ message: (bytes, from) => {
    const q = new Uint8Array(bytes);
    if (q.length < 12) return;
    const id = (q[0] << 8) | q[1];
    let end = 12;
    while (end < q.length && q[end] !== 0) end += 1 + q[end];
    end += 1;
    const qsec = q.subarray(12, end + 4);
    const qtype = (q[end] << 8) | q[end + 1];
    let at = 12, labels = [];
    while (q[at] !== 0) {
        const l = q[at++];
        let s = "";
        for (let i = 0; i < l; i++) s += String.fromCharCode(q[at + i]);
        labels.push(s);
        at += l;
    }
    const owner = labels.join(".");
    queries[owner] = (queries[owner] || 0) + 1;
    const answers = [];
    const lead = owner.split(".")[0];
    if (qtype === 1) {
        if (lead === "zero")          answers.push([owner, 1, 0,   [10, 0, 0, 1]]);
        else if (lead === "mixed")    { answers.push([owner, 1, 30, [10, 0, 0, 2]]);
                                        answers.push([owner, 1, 0,  [10, 0, 0, 3]]); }
        else if (lead === "multi")    { answers.push([owner, 1, 30, [10, 0, 0, 4]]);
                                        answers.push([owner, 1, 2,  [10, 0, 0, 5]]); }
        else if (lead === "big")      answers.push([owner, 1, 100, [10, 0, 0, 6]]);
        else if (lead === "empty")    { /* 0 answers */ }
        else                          answers.push([owner, 1, 60,  [10, 0, 0, 7]]);
    } else if (qtype === 28) {
        answers.push([owner, 28, 60, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9]]);
    }
    const a = [id >> 8, id & 0xff, 0x81, 0x80, 0, 1, 0, answers.length, 0, 0, 0, 0];
    for (const b of qsec) a.push(b);
    for (const [on, t, tl, rd] of answers) pushRR(a, on, t, tl, rd);
    srv.send(new Uint8Array(a).buffer.slice(0, a.length), from.address, from.port);
}});

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const qof = (name) => queries[name] || 0;

async function main() {
    const noCache = new DNSResolver({ server: "127.0.0.1", port: srv.port, timeoutMs: 3000 });
    const cache   = new DNSResolver({ server: "127.0.0.1", port: srv.port, timeoutMs: 3000, ttl: 100 });
    const cache0  = new DNSResolver({ server: "127.0.0.1", port: srv.port, timeoutMs: 3000, ttl: 0 });

    /* P60: absent ttl disables caching */
    await noCache.lookup("plain.test");
    await noCache.lookup("plain.test");
    ok(qof("plain.test") === 2,
       "P60 absent {ttl}: every lookup is a wire query (got " + qof("plain.test") + ", want 2)");

    /* P61: ttl 0 disables caching */
    await cache0.lookup("zero0.test");
    await cache0.lookup("zero0.test");
    ok(qof("zero0.test") === 2,
       "P61 {ttl: 0}: caching disabled (got " + qof("zero0.test") + ", want 2)");

    /* P62: a hit serves from the cache */
    await cache.lookup("hit.test");
    await cache.lookup("hit.test");
    await cache.lookup("hit.test");
    ok(qof("hit.test") === 1,
       "P62 fresh answer cached: 3 lookups = 1 wire query (got " + qof("hit.test") + ")");

    /* P63: key = lowercase(name)|type */
    await cache.lookup("CaSeD.TeSt");
    await cache.lookup("cased.test");
    await cache.lookup("CASED.TEST");
    ok(qof("cased.test") + qof("CaSeD.TeSt") + qof("CASED.TEST") === 1,
       "P63 case variants share one cache entry (wires=" + qof("cased.test") + ")");
    await cache.lookup("typed.test", 1);
    await cache.lookup("typed.test", 28);
    await cache.lookup("typed.test", 1);
    await cache.lookup("typed.test", 28);
    ok(qof("typed.test") === 2,
       "P63b the TYPE is part of the key: A and AAAA cache apart (wires=" +
       qof("typed.test") + ", want 2)");

    /* P68: fresh deep copy per hit */
    await cache.lookup("copy.test");
    const r1 = await cache.lookup("copy.test");
    r1[0].address = "6.6.6.6";
    r1[0].ttl = -1;
    r1.push({ name: "junk", type: 0, ttl: 0, address: "9.9.9.9" });
    const r2 = await cache.lookup("copy.test");
    ok(r2.length === 1 && r2[0].address === "10.0.0.7" && r2[0].ttl === 60 &&
       r2[0] !== r1[0],
       "P68 every hit is a fresh deep copy (mutation-safe)",
       JSON.stringify(r2));

    /* P69: hits settle asynchronously, on a job */
    {
        const p = cache.lookup("copy.test");       /* a HIT */
        let hit = false;
        const order = [];
        p.then(() => { hit = true; order.push("hit"); });
        Promise.resolve().then(() => order.push("micro"));
        await sleep(20);
        ok(order.indexOf("micro") < order.indexOf("hit"),
           "P69 a cache hit settles asynchronously (on a job): " + JSON.stringify(order));
    }

    /* P64: expiry = min(smallest RR TTL, ttl): RRs {30,2} with ttl=100 -> 2s */
    await cache.lookup("multi.test");
    await sleep(300);
    const hitEarly = await cache.lookup("multi.test");
    await sleep(2400);
    await cache.lookup("multi.test");
    ok(qof("multi.test") === 2 && hitEarly.length === 2,
       "P64 expiry is min(smallest RR TTL, ttl): {30,2}+cap100 expires ~2s (wires=" +
       qof("multi.test") + ", want 2)");

    /* P65: the cap direction: RR {100} with ttl=2 -> 2s */
    const cap2 = new DNSResolver({ server: "127.0.0.1", port: srv.port, timeoutMs: 3000, ttl: 2 });
    await cap2.lookup("big.test");
    await sleep(300);
    await cap2.lookup("big.test");
    await sleep(2400);
    await cap2.lookup("big.test");
    ok(qof("big.test") === 2,
       "P65 expiry is capped by {ttl}: RR 100 + cap 2 expires ~2s (wires=" +
       qof("big.test") + ", want 2)");

    /* P66: a TTL-0 answer is never cached (single and mixed) */
    await cache.lookup("zero.test");
    await cache.lookup("zero.test");
    await cache.lookup("mixed.test");
    await cache.lookup("mixed.test");
    ok(qof("zero.test") === 2 && qof("mixed.test") === 2,
       "P66 TTL-0 answers are never cached (zero=" + qof("zero.test") +
       " mixed{30,0}=" + qof("mixed.test") + ", want 2/2)");

    /* P67: an empty answer is never cached */
    await cache.lookup("empty.test");
    const e2 = await cache.lookup("empty.test");
    ok(qof("empty.test") === 2 && e2.length === 0,
       "P67 empty answers are never cached (wires=" + qof("empty.test") + ", want 2)");

    /* P70: N concurrent lookups of an uncached name = N wire queries */
    await Promise.all([cache.lookup("burst.test"), cache.lookup("burst.test"),
                       cache.lookup("burst.test")]);
    await sleep(50);
    ok(qof("burst.test") === 3,
       "P70 no single-flight: 3 concurrent lookups = 3 wire queries (got " +
       qof("burst.test") + ")");

    /* P71: ttl > 86400 clamps -- expiry not observable in a bounded probe.
     * Construction must accept it; the clamp value is code-inspected. */
    {
        const e = (() => { try {
            const r = new DNSResolver({ server: "127.0.0.1", port: srv.port,
                                        timeoutMs: 3000, ttl: 200000 });
            return { r };
        } catch (x) { return { e: x }; } })();
        if (e.e) {
            ok(false, "P71 {ttl: 200000} accepted (clamped to 86400 per doc)",
               e.e.constructor.name + ": " + e.e.message);
        } else {
            await e.r.lookup("clamp.test");
            await e.r.lookup("clamp.test");
            ok(qof("clamp.test") === 1,
               "P71 {ttl: 200000} accepted and caches (86400 clamp itself is " +
               "wall-clock 24h: code-inspected, not probe-observable)");
            e.r.close();
        }
        /* negative ttl: not covered by the doc -- record behavior */
        const neg = (() => { try {
            return { r: new DNSResolver({ server: "127.0.0.1", port: srv.port,
                                          timeoutMs: 3000, ttl: -5 }) };
        } catch (x) { return { e: x }; } })();
        if (neg.e) print("  INFO  P71b {ttl: -5} throws " + neg.e.constructor.name + ": " + neg.e.message);
        else {
            await neg.r.lookup("neg.test");
            await neg.r.lookup("neg.test");
            print("  INFO  P71b {ttl: -5} accepted; cache " +
                  (qof("neg.test") === 1 ? "ACTIVE (wires=1)" : "DISABLED (wires=" + qof("neg.test") + ")"));
            neg.r.close();
        }
    }

    noCache.close(); cache.close(); cache0.close(); srv.close();
    print("review_dns_ttl: " + pass + " passed, " + fail + " failed");
    if (fail) throw new Error("review_dns_ttl: " + fail + " failures");
}

await main();
