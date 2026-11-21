// covers: dyna:structures.IntervalTree
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_interval_tree_codec_forge");
import { BitSet, RangeSet, RangeMap, IntervalTree, MinMaxHeap, BTree, Multimap } from "dyna:structures";
import { Graph } from "dyna:graph";
import { CRC32C } from "dyna:hash";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind))
            { print("FAIL: " + m + " (wrong error: " + e + ")"); fails++; return; }
        return;
    }
    print("FAIL: " + m + " (no throw)"); fails++;
}

const B = [];
function u8(v)  { B.push(v & 0xff); }
function u16(v) { u8(v); u8(v >>> 8); }
function u32(v) { u8(v); u8(v >>> 8); u8(v >>> 16); u8(v >>> 24); }
function u64(v) { let l = Number(v & 0xFFFFFFFFn), h = Number(v >> 32n);
                  u32(l); u32(h); }
function f64(x) {
    const b = new Uint8Array(new Float64Array([x]).buffer);
    for (let i = 0; i < 8; i++) u8(b[i]);
}
function blob(s) { u32(s.length); for (let i = 0; i < s.length; i++) u8(s.charCodeAt(i)); }
function uvarint(v) {
    do { let b = Number(v & 0x7Fn); v >>= 7n;
         if (v) b |= 0x80; u8(b); } while (v);
}
function svarint(v) { uvarint((v < 0 ? ((~BigInt(v)) << 1n) | 1n : BigInt(v) << 1n)); }

function forge(typeId, payload) {
    B.length = 0;
    u8(0x44); u8(0x59); u8(0x4E); u8(0x53);
    u16(1); u16(typeId); u32(0);
    u64(BigInt(payload.length));
    for (let i = 0; i < payload.length; i++) u8(payload[i]);
    const crc = CRC32C(new Uint8Array(B)) >>> 0;
    u32(crc);
    return new Uint8Array(B);
}

const EXT = 0xFFFFFFFF;
const TID = { RANGESET: 24, RANGEMAP: 25, INTERVALTREE: 26,
              MINMAXHEAP: 27, BTREE: 30, MULTIMAP: 21, BITSET: 1, GRAPH: 14 };
const DS_BS_EMPTY = 0;
const BS_CHUNKW = 1024;

__kitS.section("a NaN interval bound in a forged record is skipped", () => {
    function payloadBytes(cls) {
        const b = cls.serialize();
        return Array.from(b.slice(20, b.length - 4));
    }
    function put32(p, at, v) {
        p[at] = v & 0xff; p[at + 1] = (v >>> 8) & 0xff;
        p[at + 2] = (v >>> 16) & 0xff; p[at + 3] = (v >>> 24) & 0xff;
    }
    function putF64(p, at, x) {
        const b = new Uint8Array(new Float64Array([x]).buffer);
        for (let i = 0; i < 8; i++) p[at + i] = b[i];
    }

    {
        const real = new IntervalTree();
        real.insert(1.5, 2.5, "a");
        const p = payloadBytes(real);
        const bad = p.slice(0, 25);
        putF64(bad, 9, 1.5); putF64(bad, 17, NaN);
        bad.push.apply(bad, p.slice(25));
        const t = IntervalTree.deserialize(forge(TID.INTERVALTREE, bad));
        check(t.size === 0, "a NaN interval bound in a forged record is skipped");
        const inv = p.slice(0, 25);
        putF64(inv, 9, 2.5); putF64(inv, 17, 1.5);
        inv.push.apply(inv, p.slice(25));
        const t2 = IntervalTree.deserialize(forge(TID.INTERVALTREE, inv));
        // The hardened insert refuses lo > hi, and the reader drops any row
        // the live door would refuse (same rule as the NaN row above): a
        // forged inverted interval is skipped, never fabricated as [lo, lo].
        check(t2.size === 0, "an inverted interval in a forged record is skipped like insert's refusal");
    }

    {
        const real = new BTree();
        real.set(1.5, "a");
        const p = payloadBytes(real);
        const forged = p.slice(0, 9);
        put32(forged, 4, 2);
        forged[8] = 0;
        putF64(forged, 9, 1.5); putF64(forged, 17, 2.5);
        forged.push.apply(forged, p.slice(17));
        throws(() => BTree.deserialize(forge(TID.BTREE, forged)), TypeError,
               "a BTree record whose values count disagrees with its keys is refused");
    }

    {
        B.length = 0;
        u8(1); u8(0);
        u32(EXT); u32(2);
        uvarint(2049638230412172402n);
        const p = Array.from(B);
        const t0 = cpuNow();
        throws(() => Graph.deserialize(forge(TID.GRAPH, p)), TypeError,
               "a forged graph degree past 2^32 is refused");
        check(cpuNow() - t0 < 2000, "and refused promptly, not after a giant allocation");
    }
});
__kitS.end();

if (fails === 0) print("test_structures_codec_forge: all " + n + " checks passed");
else {
    print("test_structures_codec_forge: " + fails + " FAILED of " + n);
    throw new Error("test_structures_codec_forge: " + fails + " failures");
}
