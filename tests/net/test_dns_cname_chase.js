// DNSResolver.lookup follows a CNAME chain inside one answer section (RFC 1034 §3.6.2 stub behaviour):
// the records returned are the final owner's records of the asked type. Owners outside the chain stay filtered,
// a loop or a chain longer than 8 hops answers [], an owner that has records of the type is not chased (the
// pinned smuggled-CNAME case in test_net_dns.js), and lookup(name, 5) still returns the first hop only.
import { DNSResolver, UDPSocket } from "dyna:net";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

function pushName(a, name) {
    for (const l of name.split(".")) { a.push(l.length); for (const ch of l) a.push(ch.charCodeAt(0)); }
    a.push(0);
}
function nameBytes(name) { const a = []; pushName(a, name); return a; }
function pushRR(a, owner, type, ttl, rdata) {
    pushName(a, owner);
    a.push(type >> 8, type & 0xff, 0, 1, (ttl >>> 24) & 0xff, (ttl >> 16) & 0xff, (ttl >> 8) & 0xff, ttl & 0xff);
    a.push((rdata.length >> 8) & 0xff, rdata.length & 0xff, ...rdata);
}
const chain = (n, tail) => {
    const rrs = [];
    for (let i = 0; i < n; i++) rrs.push([`c${i}.test`, 5, 60, nameBytes(i + 1 < n ? `c${i + 1}.test` : tail)]);
    return rrs;
};
const ZONE = {
    "alias.test": [["alias.test", 5, 60, nameBytes("mid.test")], ["other.test", 1, 60, [10, 9, 9, 9]], ["mid.test", 5, 60, nameBytes("real.test")], ["real.test", 1, 60, [10, 0, 0, 9]], ["real.test", 1, 30, [10, 0, 0, 10]]],
    "loop.test": [["loop.test", 5, 60, nameBytes("back.test")], ["back.test", 5, 60, nameBytes("loop.test")]],
    "c0.test": [...chain(8, "end.test"), ["end.test", 1, 60, [10, 1, 1, 1]]],
    "spoof.test": [["spoof.test", 5, 60, nameBytes("real.test")], ["elsewhere.test", 1, 60, [6, 6, 6, 6]]],
    "direct.test": [["direct.test", 1, 60, [10, 2, 2, 2]]],
    "both.test": [["both.test", 5, 60, nameBytes("real.test")], ["both.test", 1, 60, [10, 4, 4, 4]], ["real.test", 1, 60, [10, 0, 0, 9]]],
};
ZONE["long.test"] = [["long.test", 5, 60, nameBytes("c0.test")], ...chain(8, "end9.test"), ["end9.test", 1, 60, [10, 3, 3, 3]]];

const srv = new UDPSocket({ port: 0, host: "127.0.0.1" });
srv.start({ message: (bytes, from) => {
    const q = new Uint8Array(bytes);
    let end = 12, labels = [], at = 12;
    while (q[end] !== 0) end += 1 + q[end];
    end += 1;
    while (q[at] !== 0) { const l = q[at++]; labels.push(String.fromCharCode(...q.subarray(at, at + l))); at += l; }
    const qtype = (q[end] << 8) | q[end + 1];
    const owner = labels.join(".");
    let answers = ZONE[owner] ?? [];
    if (qtype === 5) answers = answers.filter((r) => r[0] === owner && r[1] === 5);
    const a = [q[0], q[1], 0x81, 0x80, 0, 1, 0, answers.length, 0, 0, 0, 0, ...q.subarray(12, end + 4)];
    for (const [on, t, tl, rd] of answers) pushRR(a, on, t, tl, rd);
    srv.send(new Uint8Array(a), from.address, from.port);
}});

const r = new DNSResolver({ server: "127.0.0.1", port: srv.port, timeoutMs: 2000 });
const addrs = (recs) => recs.map((x) => x.address).sort().join();

const viaChain = await r.lookup("alias.test", 1);
ok(addrs(viaChain) === "10.0.0.10,10.0.0.9", "two-hop chain returns the final A records", addrs(viaChain));
ok(viaChain.every((x) => x.name === "real.test" && x.type === 1), "records carry the canonical owner name", JSON.stringify(viaChain));
ok(!viaChain.some((x) => x.address === "10.9.9.9"), "an A record outside the chain is not returned");
ok(addrs(await r.lookup("c0.test", 1)) === "10.1.1.1", "an 8-hop chain is followed to the end");
ok((await r.lookup("long.test", 1)).length === 0, "a 9-hop chain is cut at 8 hops and answers []");
ok((await r.lookup("loop.test", 1)).length === 0, "a CNAME loop answers [] instead of spinning");
ok((await r.lookup("spoof.test", 1)).length === 0, "an A record for a name the chain never reaches is refused");
ok(addrs(await r.lookup("direct.test", 1)) === "10.2.2.2", "a plain answer is unchanged");
ok(addrs(await r.lookup("both.test", 1)) === "10.4.4.4", "an owner with records of the asked type is answered directly, its CNAME ignored");
const first = await r.lookup("alias.test", 5);
ok(first.length === 1 && first[0].target === "mid.test", "lookup(name, CNAME) returns only the first hop", JSON.stringify(first));
ok(JSON.stringify(await r.resolveCname("alias.test")) === '["mid.test"]', "resolveCname is the first hop");

r.close(); srv.close();
print("test_dns_cname_chase: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
