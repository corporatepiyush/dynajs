// covers: dyna:structures.PatriciaTrie -- the v2 record (raw keys, one value per entry, sorted cursor load) and the v1 reader kept for older records
// v2 (bytes byte has 0x80 set): [u8 bytes|0x80][u8 space][u32 count] then per entry [u32 bits][ceil(bits/8) key bytes] in walk
// order, then a flat value array of the same count. v1 ([u8 bytes][u8 space] then a flat array of
// key/bits/value triples) is what builds before version 2 wrote; V1 below holds such records,
// written by that build, so they must keep loading. Load inserts keys with a path cursor, so a
// walk-ordered record builds in about linear time; any order still loads correctly.
import { suite } from "../lib/kit.js";
import { PatriciaTrie } from "dyna:structures";
import { CRC32C } from "dyna:hash";

const t = suite("patricia_trie_codec_v2");
const V1 = {
    "string": "44594e5301002f0000000000d1000000000000000001cb0000000d01027809210e0200000f00ffffffff0f050005000e0201000f01ffffffff0f61051005020e0202000f02ffffffff0f6162052005040e0203000f03ffffffff0f616263053005060e0201000f01ffffffff0f62051005020e0202000f02ffffffff0f6261052005040e0206000f06ffffffff0f68c3a96c6c6f0560050a0e0202000f02ffffffff0f6b37052005040e0203000f03ffffffff0f6b3730053005060e0203000f03ffffffff0f6f626a05300801020902050205040e0202000f02ffffffff0f7a7a05200504c04dc1de",
    "bytes": "44594e5301002f00000000004c000000000000000101460000000d00090c0e0200000f00ffffffff0f050005000e0201000f01ffffffff0f01051005020e0202000f02ffffffff0f0102052005040e0203000f03ffffffff0fff00070530050656cd3119",
    "cidr": "44594e5301002f00000000008e000000000000000002880000000d0009120e0201000f01ffffffff0f0405100706616e790e0202000f02ffffffff0f040a0520070674656e0e0203000f03ffffffff0f040a010530070e74656e2d6f6e650e0205000f05ffffffff0f04c0a8010705500708686f73740e0201000f01ffffffff0f0605100708616e79360e0205000f05ffffffff0f0620010db805500708646f6336664337ac",
    "empty": "44594e5301002f00000000000a000000000000000000040000000d000900c545ebad"
};
const HEADER = 20;
const unhex = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));
function editPayload(rec, fn) {
    const body = [...rec.subarray(HEADER, rec.length - 4)];
    fn(body);
    const out = new Uint8Array(HEADER + body.length + 4);
    out.set(rec.subarray(0, HEADER));
    out.set(body, HEADER);
    const dv = new DataView(out.buffer);
    dv.setUint32(12, body.length, true);
    dv.setUint32(16, 0, true);
    dv.setUint32(out.length - 4, CRC32C(out.subarray(0, out.length - 4)) >>> 0, true);
    return out;
}
const dump = (p) => JSON.stringify(p.withPrefix().map(([k, v]) => [k instanceof Uint8Array ? [...k] : k, v]));
const IPS = ["10.0.0.1", "10.1.2.3", "192.168.1.7", "192.168.1.8", "8.8.8.8", "2001:db8::1", "2001:db9::1"];
const cidrProbe = (p) => JSON.stringify(IPS.map((ip) => String(p.matchIP(ip))));

t.test("v1 records written by the older build load with the same entries", (a) => {
    const s = PatriciaTrie.deserialize(unhex(V1.string));
    a.eq(s.size, 11);
    a.eq(s.get("héllo"), 5);
    a.eq(JSON.stringify(s.get("obj")), '{"x":[1,2]}');
    a.eq(JSON.stringify(s.longestPrefix("abcd")), '["abc",3]');
    a.eq(s.withPrefix("k7").length, 2);
    const b = PatriciaTrie.deserialize(unhex(V1.bytes));
    a.eq(dump(b), "[[[],0],[[1],1],[[1,2],2],[[255,0,7],3]]");
    const c = PatriciaTrie.deserialize(unhex(V1.cidr));
    a.eq(cidrProbe(c), '["ten","ten-one","host","any","any","doc6","any6"]');
    a.eq(PatriciaTrie.deserialize(unhex(V1.empty)).size, 0);
    for (const k of ["string", "bytes", "cidr", "empty"]) {
        const old = PatriciaTrie.deserialize(unhex(V1[k]));
        const rec = old.serialize();
        a.eq(rec[HEADER] & 0x80, 0x80, k + ": a new save writes v2");
        const back = PatriciaTrie.deserialize(rec);
        a.eq(k === "cidr" ? cidrProbe(back) : dump(back), k === "cidr" ? cidrProbe(old) : dump(old), k + ": v1 -> v2 keeps every entry");
        a.ok(rec.length < unhex(V1[k]).length || k === "empty", k + ": v2 is smaller");
    }
});

t.test("v2 round trips: keys in any insertion order, prefixes, duplicates, every key space", (a) => {
    let x = 12345;
    const rnd = () => (x = (x * 1103515245 + 12345) >>> 0) / 2 ** 32;
    const s = new PatriciaTrie();
    for (let i = 0; i < 4000; i++) {
        const len = Math.floor(rnd() * 6);
        let k = "";
        for (let j = 0; j < len; j++) k += "abéz"[Math.floor(rnd() * 4)];
        s.set(k, i);
    }
    a.eq(dump(PatriciaTrie.deserialize(s.serialize())), dump(s), "string keys");
    const b = new PatriciaTrie({ keys: "bytes" });
    for (let i = 0; i < 3000; i++) b.set(new Uint8Array(Array.from({ length: Math.floor(rnd() * 4) }, () => Math.floor(rnd() * 3) * 127)), i);
    const b2 = PatriciaTrie.deserialize(b.serialize());
    a.eq(dump(b2), dump(b), "byte keys");
    b2.set(new Uint8Array([0, 0, 0, 0, 0]), "new");
    a.eq(b2.get(new Uint8Array([0, 0, 0, 0, 0])), "new", "a loaded trie accepts writes");
    const c = new PatriciaTrie();
    for (let i = 0; i < 2000; i++) c.addCIDR(`${10 + (i % 3)}.${(i * 37) & 255}.${i & 255}.0/${8 + (i % 25)}`, i);
    c.addCIDR("2001:db8::/32", "doc6");
    a.eq(cidrProbe(PatriciaTrie.deserialize(c.serialize())), cidrProbe(c), "CIDR keys");
    a.eq(PatriciaTrie.deserialize(c.serialize()).size, c.size);
});

t.test("forged v2 records: each guard refuses its own forgery", (a) => {
    const s = new PatriciaTrie().set("ab", 1).serialize();
    a.eq(PatriciaTrie.deserialize(editPayload(s, () => {})).get("ab"), 1, "the re-signed record decodes");
    a.throws(() => PatriciaTrie.deserialize(editPayload(s, (b) => { b[6] = 15; })), /does not fit its key space/, "a string key of 15 bits");
    a.throws(() => PatriciaTrie.deserialize(editPayload(s, (b) => { b[1] = 2; })), /does not fit its key space/, "a string key under the CIDR space");
    a.throws(() => PatriciaTrie.deserialize(editPayload(s, (b) => { b[1] = 0; })), /key space does not match/, "entries under the empty space");
    a.throws(() => PatriciaTrie.deserialize(editPayload(s, (b) => { b[0] = 0x82; })), /truncated DYNS record/, "bytes flag 2");
    a.throws(() => PatriciaTrie.deserialize(editPayload(s, (b) => { b[2] = 2; })), /truncated DYNS record|does not fit its key space/, "a count above the entries");
    a.throws(() => PatriciaTrie.deserialize(editPayload(s, (b) => { b[2] = 0xff; b[3] = 0xff; b[4] = 0xff; b[5] = 0xff; })), /truncated DYNS record/, "a count of 2^32-1");
    a.throws(() => PatriciaTrie.deserialize(editPayload(s, (b) => { b[6] = 0xff; b[7] = 0xff; b[8] = 0xff; b[9] = 0xff; })), /truncated DYNS record|does not fit/, "a key of 2^32-1 bits");
    const two = new PatriciaTrie().set("a", 1).set("b", 2).serialize();
    a.throws(() => PatriciaTrie.deserialize(editPayload(two, (b) => { b[2] = 1; })), Error, "fewer keys than values");
    a.throws(() => PatriciaTrie.deserialize(editPayload(two, (b) => { b[2] = 1; b.splice(11, 5); })), /truncated DYNS record/, "one key and two values");
    a.eq(PatriciaTrie.deserialize(editPayload(two, (b) => { b.splice(11, 0); })).get("b"), 2, "the two-key record decodes");
    const e = new PatriciaTrie().serialize();
    a.throws(() => PatriciaTrie.deserialize(editPayload(e, (b) => { b[1] = 1; })), /key space does not match/, "no entries under the string space");
    const c = new PatriciaTrie().addCIDR("10.0.0.0/8", 1).serialize();
    a.eq(PatriciaTrie.deserialize(editPayload(c, () => {})).matchIP("10.9.9.9"), 1);
    a.throws(() => PatriciaTrie.deserialize(editPayload(c, (b) => { b[10] = 5; })), /does not fit its key space/, "family 5");
    a.throws(() => PatriciaTrie.deserialize(editPayload(c, (b) => { b[6] = 41; })), /does not fit its key space|truncated/, "an IPv4 prefix of 33");
    const host = new PatriciaTrie().addCIDR("10.0.0.1/32", 1).serialize();
    a.eq(PatriciaTrie.deserialize(editPayload(host, () => {})).matchIP("10.0.0.1"), 1);
    a.throws(() => PatriciaTrie.deserialize(editPayload(host, (b) => { b[6] = 41; b.splice(15, 0, 0x80); })), /does not fit its key space/, "an IPv4 prefix of 33 with its sixth byte");
    a.throws(() => PatriciaTrie.deserialize(editPayload(c, (b) => { b[6] = 7; })), /does not fit its key space/, "a CIDR key without its family byte");
});

await t.run();
