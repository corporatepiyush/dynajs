// covers: dyna:graph.Graph
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_codec_forge");
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

__kitS.section("a Graph record carrying a NaN weight is refused", () => {
    function graph(nanWeight) {
        const p = [1, 1];
        (function w32(v) { p.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); })(EXT);
        B.length = 0; u32(2); uvarint(1n); svarint(1);
        if (nanWeight) {
            const b = new Uint8Array(new Float64Array([NaN]).buffer);
            for (let i = 0; i < 8; i++) u8(b[i]);
        } else {
            f64(2.5);
        }
        uvarint(0n);
        for (let i = 0; i < B.length; i++) p.push(B[i]);
        return p;
    }
    throws(() => Graph.deserialize(forge(TID.GRAPH, graph(true))), TypeError,
           "a Graph record carrying a NaN weight is refused");
    {
        const g = Graph.deserialize(forge(TID.GRAPH, graph(false)));
        check(g.nodeCount === 2 && g.edgeCount === 1 &&
              g.dijkstra(0)[1] === 2.5,
              "a legal weighted Graph record still decodes");
    }
});
__kitS.end();

if (fails === 0) print("test_structures_codec_forge: all " + n + " checks passed");
else {
    print("test_structures_codec_forge: " + fails + " FAILED of " + n);
    throw new Error("test_structures_codec_forge: " + fails + " failures");
}
