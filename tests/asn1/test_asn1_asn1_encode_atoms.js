// covers: c:src/modules/asn1/asn1-encode.c
// ASN1.encode reads cls/tag/constructed/value of every node and length of every child array
// through five atoms made once per encode call (each read made and looked up its atom from the
// C string; that was ~35% of encoding a 1K-100K-node tree). The oracle is a JS DER encoder for
// INTEGER, NULL, BOOLEAN, OCTET STRING, SEQUENCE, SET (children sorted by encoding) and context
// tags: random trees must encode byte for byte the same, and on a node of getters and a Proxy
// child array the reads must come in the order cls, tag, constructed, value, then length.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_asn1_asn1_encode_atoms");
import { ASN1 } from "dyna:asn1";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; if (fail < 30) print("FAIL " + m + ": got " + a + ", want " + b); } };
let seed = 99;
const rnd = (n) => { seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; return seed % n; };
const len = (n) => n < 128 ? [n] : n < 256 ? [0x81, n] : n < 65536 ? [0x82, n >> 8, n & 255] : [0x83, n >> 16, (n >> 8) & 255, n & 255];
const tlv = (t, c) => [t, ...len(c.length), ...c];
function intBytes(v) {
    const b = [];
    let x = BigInt(v);
    for (;;) {
        b.unshift(Number(x & 255n));
        x >>= 8n;
        if ((x === 0n && !(b[0] & 0x80)) || (x === -1n && (b[0] & 0x80))) break;
    }
    return b;
}
const cmp = (a, b) => { for (let i = 0; i < Math.min(a.length, b.length); i++) if (a[i] !== b[i]) return a[i] - b[i]; return a.length - b.length; };
function gen(d) {
    const k = d > 3 ? rnd(4) : rnd(7);
    if (k === 0) { const v = rnd(2) ? rnd(1 << 30) - (1 << 29) : rnd(300) - 150; return [{ tag: 2, value: v }, tlv(2, intBytes(v))]; }
    if (k === 1) return [{ tag: 5, value: null }, [5, 0]];
    if (k === 2) { const t = !!rnd(2); return [{ tag: 1, value: t }, [1, 1, t ? 255 : 0]]; }
    if (k === 3) { const b = Array.from({ length: rnd(300) }, () => rnd(256)); return [{ tag: 4, value: new Uint8Array(b) }, tlv(4, b)]; }
    const kids = Array.from({ length: rnd(6) }, () => gen(d + 1));
    if (k === 4) return [{ tag: 16, constructed: true, value: kids.map((x) => x[0]) }, tlv(0x30, kids.flatMap((x) => x[1]))];
    if (k === 5) return [{ tag: 17, constructed: true, value: kids.map((x) => x[0]) }, tlv(0x31, kids.map((x) => x[1]).sort(cmp).flat())];
    const b = Array.from({ length: rnd(20) }, () => rnd(256));
    const t = rnd(30);
    return [{ cls: 2, tag: t, value: new Uint8Array(b) }, tlv(0x80 | t, b)];
}

__kitS.section("random trees against a JS DER encoder", () => {
    for (let i = 0; i < 400; i++) {
        const [node, want] = gen(0);
        eq(Array.from(ASN1.encode(node)).join(","), want.join(","), "tree #" + i);
    }
    const big = Array.from({ length: 5000 }, (_, i) => gen(4));
    eq(Array.from(ASN1.encode({ tag: 16, constructed: true, value: big.map((x) => x[0]) })).join(","), tlv(0x30, big.flatMap((x) => x[1])).join(","), "a 5000-child sequence");
});

__kitS.section("read order through getters and a Proxy array", () => {
    const log = [];
    const leaf = (v) => ({ get cls() { log.push("cls"); return 0; }, get tag() { log.push("tag"); return 2; }, get constructed() { log.push("constructed"); return false; }, get value() { log.push("value"); return v; } });
    const arr = new Proxy([leaf(1), leaf(2)], { get(t, k, r) { if (typeof k === "string") log.push("arr." + k); return Reflect.get(t, k, r); } });
    const root = { get cls() { log.push("cls"); return 0; }, get tag() { log.push("tag"); return 16; }, get constructed() { log.push("constructed"); return true; }, get value() { log.push("value"); return arr; } };
    eq(Array.from(ASN1.encode(root)).join(","), "48,6,2,1,1,2,1,2", "getter tree bytes");
    eq(log.join(" "), "cls tag constructed value arr.length arr.0 cls tag constructed value arr.1 cls tag constructed value", "read order");
    let threw = "";
    try { ASN1.encode({ get tag() { throw new RangeError("boom"); } }); } catch (e) { threw = e.name + ":" + e.message; }
    eq(threw, "RangeError:boom", "a throwing getter propagates");
});

__kitS.end();
print(`test_serialize_asn1_encode_atoms: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
