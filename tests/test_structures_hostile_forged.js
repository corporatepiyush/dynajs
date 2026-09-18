/* test_structures_hostile_forged.js -- hostile DYNS records with a VALID
 * CRC32C, so the envelope passes and the per-reader validation is what must
 * refuse.
 *
 * test_structures_serialize.js's mutation sweep corrupts valid records byte by
 * byte; this file goes one step further and FORGES records: payload fields
 * hand-built to cross each reader's bounds exactly, resealed with a correct
 * checksum so the CRC gate cannot save the readers. Every case must throw
 * (TypeError/RangeError/SyntaxError) -- never crash, never return a container
 * built from hostile bytes.
 *
 * Boundaries proven here (per reader, against the caps in dyna-structures.c):
 *   counts     forged counts 1 over the cap (2^24 for UF/Fenwick/SortedSet...)
 *   lengths    counts that outrun the payload at the reader's own per-element
 *              floor (RAW 8 B/elem, varint 1 B/elem, blob 4 B, ...)
 *   offsets    Trie/Multiset shared-prefix lengths past what was decoded;
 *              BitSet chunk offsets/runs past the chunk; Bloom sparse deltas
 *              past nwords; Table cell indices past the dictionary
 *   shapes     SegTree odd-element rule, Graph undirected parity, Graph edge
 *              targets past n and negative, non-finite weights, HLL register
 *              ranks no add() could produce, zero-length RLE runs
 */
import * as S from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, refused = 0;
const returned = [];

/* ---- little-endian payload builders (the DYNS wire is LE by definition) -- */
function u16(v) { return [v & 0xff, (v >>> 8) & 0xff]; }
function u32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }
function u64(v) { return [...u32(v >>> 0), ...u32(Math.floor(v / 4294967296))]; }
function f64(v) { const b = new Uint8Array(8); new DataView(b.buffer).setFloat64(0, v, true); return [...b]; }
function uvar(v) { const out = []; do { let b = v & 0x7f; v = Math.floor(v / 128); if (v) b |= 0x80; out.push(b); } while (v); return out; }

/* A valid envelope around a hostile payload: magic, version 1, type id,
 * flags, payload length, payload, CRC32C over everything before it. */
function record(tid, payload) {
    const head = [0x44, 0x59, 0x4e, 0x53, ...u16(1), ...u16(tid), ...u32(0), ...u64(payload.length)];
    const body = [...head, ...payload];
    const crc = CRC32C(new Uint8Array(body)) >>> 0;
    return new Uint8Array([...body, ...u32(crc)]);
}

/* Self-test: the CRC we compute must match the writer's on a real record,
   or every "refusal" below would just be a checksum rejection. */
{
    const g = new S.Graph(); g.addNode(); g.addNode(); g.addEdge(0, 1, 2.5);
    const real = g.serialize();
    const mine = CRC32C(real.subarray(0, real.length - 4)) >>> 0;
    const theirs = (real[real.length - 4] | (real[real.length - 3] << 8) |
                    (real[real.length - 2] << 16) | (real[real.length - 1] << 24)) >>> 0;
    if (mine !== theirs) throw new Error("CRC32C self-test failed: " + mine + " vs " + theirs);
}

const TID = { BitSet: 1, UnionFind: 2, Deque: 3, Fenwick: 4, RingBuffer: 5, SegTree: 6, BloomFilter: 7, Trie: 8, LRU: 9, SortedSet: 10, SortedMap: 11, Heap: 12, List: 13, Graph: 14, Multiset: 20, Multimap: 21, BiMap: 22, Table: 23, RangeSet: 24, RangeMap: 25, IntervalTree: 26, MinMaxHeap: 27, CountMinSketch: 28, HyperLogLog: 29, BTree: 30 };

function H(name, clsName, payload) {
    n++;
    let obj;
    try { obj = S[clsName].deserialize(record(TID[clsName], new Uint8Array(payload))); }
    catch (e) {
        if (e instanceof TypeError || e instanceof RangeError || e instanceof SyntaxError) { refused++; return; }
        returned.push(name + " (threw " + (e && e.constructor.name) + ")");
        return;
    }
    returned.push(name + " (DECODED to " + (obj && obj.constructor && obj.constructor.name) + ")");
}

/* ---- the battery ---------------------------------------------------- */
H("bitset: nchunks mismatch", "BitSet", [...u32(0xFFFFFFFF), ...u32(1024), ...u32(7)]);
H("bitset: unknown chunk kind", "BitSet", [...u32(0xFFFFFFFF), ...u32(64), ...u32(1), 99]);
H("bitset: array chunk forged offset", "BitSet", [...u32(0xFFFFFFFF), ...u32(64), ...u32(1), 1, ...u16(3), ...u16(5000)]);
H("bitset: array count exceeds bytes", "BitSet", [...u32(0xFFFFFFFF), ...u32(64), ...u32(1), 1, ...u16(100), ...u16(1)]);
H("bitset: run leaves chunk", "BitSet", [...u32(0xFFFFFFFF), ...u32(1), ...u32(1), 3, ...u16(1), ...u16(60), ...u16(63)]);   // n=1 word=64 bits, run 60..124 escapes
H("bitset: raw n outruns payload", "BitSet", [...u32(1000000)]);
H("bitset: chunked n over cap", "BitSet", [...u32(0xFFFFFFFF), ...u32(0x7FFFFFFF), ...u32(32768)]);

H("uf: bad kind", "UnionFind", [...u32(0xFFFFFFFF), ...u32(10), ...u32(10), 7]);
H("uf: identity n over cap", "UnionFind", [...u32(0xFFFFFFFF), ...u32(0x1000001), ...u32(1), 0]);   // 2^24+1 > cap
H("uf: forged parent >= n", "UnionFind", [...u32(0xFFFFFFFF), ...u32(2), ...u32(2), 1, ...uvar(4), ...uvar(0), ...uvar(2), 0, ...uvar(1), 0]);
H("uf: negative parent", "UnionFind", [...u32(0xFFFFFFFF), ...u32(2), ...u32(2), 1, ...uvar(1), ...uvar(0), ...uvar(2), 0, ...uvar(1), 0]);
H("uf: zero rank run", "UnionFind", [...u32(0xFFFFFFFF), ...u32(2), ...u32(2), 1, ...uvar(0), ...uvar(1), ...uvar(0), 0, ...uvar(0), 0]);
H("uf: rank run overruns", "UnionFind", [...u32(0xFFFFFFFF), ...u32(2), ...u32(2), 1, ...uvar(0), ...uvar(1), ...uvar(0), 0, ...uvar(9), 0]);
H("uf: sets > n", "UnionFind", [...u32(0xFFFFFFFF), ...u32(2), ...u32(9), 0]);

H("deque: count lies", "Deque", [...u32(100000)]);
H("deque: blob not array", "Deque", [...u32(1), ...u32(3), 1, 2, 3]);

H("fenwick: kind bad", "Fenwick", [...u32(0xFFFFFFFF), ...u32(4), 9]);
H("fenwick: raw count lies", "Fenwick", [...u32(100000)]);
H("fenwick: int count > left", "Fenwick", [...u32(0xFFFFFFFF), ...u32(9), 2, 1]);   // 9 varints need >= 9 bytes, 1 present
H("fenwick: constant over cap", "Fenwick", [...u32(0xFFFFFFFF), ...u32(0x1000001), 1, ...f64(1)]);   // 2^24+1 > cap

H("ringbuffer: cap 0", "RingBuffer", [...u32(0), ...u32(0)]);
H("ringbuffer: count > cap", "RingBuffer", [...u32(4), ...u32(9)]);
H("ringbuffer: blob not array", "RingBuffer", [...u32(4), ...u32(2), ...u32(5), 1, 2, 3, 4, 5]);

H("segtree: op bad", "SegTree", [...u32(0xFFFFFFFF), ...u32(9)]);
H("segtree: even elems", "SegTree", [...u32(0xFFFFFFFF), ...u32(0), ...u32(4), 0, ...u32(4)]);
H("segtree: raw lies", "SegTree", [...u32(3)]);

H("bloom: nbits 0", "BloomFilter", [...u32(0xFFFFFFFF), ...u32(0), ...u32(3)]);
H("bloom: k 0", "BloomFilter", [...u32(0xFFFFFFFF), ...u32(64), ...u32(0)]);
H("bloom: k 33", "BloomFilter", [...u32(0xFFFFFFFF), ...u32(64), ...u32(33)]);
H("bloom: sparse nz over n", "BloomFilter", [...u32(0xFFFFFFFF), ...u32(64), ...u32(2), 1, ...uvar(999)]);
H("bloom: sparse delta over n", "BloomFilter", [...u32(0xFFFFFFFF), ...u32(64), ...u32(2), 1, ...uvar(1), ...uvar(5), ...uvar(1)]);   // nz=1: delta 5 lands past nwords=2

H("trie: count lies (old form)", "Trie", [...u32(100000)]);
H("trie: frontcoded shared > decoded", "Trie", [...u32(0xFFFFFFFF), ...u32(1), ...uvar(5), ...uvar(1), 0x61]);
H("trie: suffix > left", "Trie", [...u32(0xFFFFFFFF), ...u32(1), ...uvar(0), ...uvar(99)]);

H("lru: cap 0", "LRU", [...u32(0), ...u32(0)]);
H("lru: count > cap", "LRU", [...u32(2), ...u32(9)]);
H("lru: count vs blob mismatch", "LRU", [...u32(4), ...u32(2), ...u32(1), 0x61, ...u32(1), 0x62]);

H("sortedset: kind bad", "SortedSet", [...u32(0xFFFFFFFF), 9]);
H("sortedset: arithmetic over cap", "SortedSet", [...u32(0xFFFFFFFF), 1, ...u32(0x1000001), ...f64(0), ...f64(1)]);   // 2^24+1 > cap
H("sortedset: raw count lies", "SortedSet", [...u32(0xFFFFFFFF), 0, ...u32(100000)]);
H("sortedset: NaN key in record", "SortedSet", [...u32(0xFFFFFFFF), 0, ...u32(1), ...f64(NaN)]);
H("sortedmap: NaN key in record", "SortedMap", [...u32(0xFFFFFFFF), 0, ...u32(1), ...f64(NaN)]);

H("list: count lies", "List", [...u32(500000)]);
H("list: blob not array", "List", [...u32(1), ...u32(2), 9, 9]);

H("graph: varint degree outruns", "Graph", [1, 1, ...u32(0xFFFFFFFF), ...u32(1), ...uvar(1000000)]);
H("graph: edge target >= n", "Graph", [1, 1, ...u32(0xFFFFFFFF), ...u32(2), ...uvar(1), ...uvar(5), ...f64(1), ...uvar(0)]);
H("graph: negative target", "Graph", [1, 1, ...u32(0xFFFFFFFF), ...u32(2), ...uvar(1), ...uvar(1), ...uvar(2), ...f64(1)]);
H("graph: nonfinite weight", "Graph", [1, 1, ...u32(0xFFFFFFFF), ...u32(2), ...uvar(1), ...uvar(1), ...f64(Infinity)]);
H("graph: undirected odd parity", "Graph", [0, 0, ...u32(0xFFFFFFFF), ...u32(3), ...uvar(1), ...uvar(1), ...uvar(0)]);
H("graph: old form n lies", "Graph", [0, 0, ...u32(1000000)]);

H("multiset: count lies", "Multiset", [...u32(0xFFFFFFFF), ...u32(100000)]);
H("multiset: shared > decoded", "Multiset", [...u32(0xFFFFFFFF), ...u32(1), ...uvar(9), ...uvar(1), 0x61, ...uvar(1)]);
H("multiset: suffix > left", "Multiset", [...u32(0xFFFFFFFF), ...u32(1), ...uvar(0), ...uvar(77)]);

H("multimap: per-key count > left", "Multimap", [...u32(1), ...u32(1), 0x61, ...u32(0xFFFFFFF)]);
H("multimap: total over cap", "Multimap", [...u32(2), ...u32(1), 0x61, ...u32(0xFFFFFF), ...u32(1), 0x62, ...u32(0xFFFFFF)]);
H("multimap: vals mismatch", "Multimap", [...u32(1), ...u32(1), 0x61, ...u32(2)]);

H("bimap: count lies", "BiMap", [...u32(100000)]);
H("bimap: truncated entry", "BiMap", [...u32(1), ...u32(2), 0x61, 0x62]);

H("table: dict first shared != 0", "Table", [...u32(0xFFFFFFFF), ...u32(1), ...uvar(1), ...uvar(0), 0x61]);
H("table: dict shared > prev", "Table", [...u32(0xFFFFFFFF), ...u32(2), ...uvar(2), ...uvar(1), 0x61, ...uvar(2), 0x62, ...uvar(1), 0x63]);
H("table: cell index >= dict", "Table", [...u32(0xFFFFFFFF), ...u32(1), ...uvar(1), ...uvar(1), 0x61, ...uvar(1), ...uvar(1), 0x63, ...uvar(5), ...uvar(0)]);

H("rangeset: raw count lies", "RangeSet", [...u32(100000)]);
H("rangeset: int count > left", "RangeSet", [...u32(0xFFFFFFFF), ...u32(9), 2, 1]);   // 9 deltas need >= 9 bytes
H("rmap: vals mismatch", "RangeMap", [...u32(0xFFFFFFFF), 2, 0, ...u32(1), ...f64(0), ...f64(1)]);

H("mmheap: pri count lies", "MinMaxHeap", [...u32(999999)]);
H("mmheap: vals mismatch", "MinMaxHeap", [...u32(0xFFFFFFFF), 2, 0, ...u32(2), ...f64(1), ...f64(2)]);

H("cms: width 0", "CountMinSketch", [...u32(0xFFFFFFFF), ...u32(0), ...u32(3), ...u64(0)]);
H("cms: depth 0", "CountMinSketch", [...u32(0xFFFFFFFF), ...u32(8), ...u32(0), ...u64(0)]);
H("cms: depth 65", "CountMinSketch", [...u32(0xFFFFFFFF), ...u32(8), ...u32(65), ...u64(0)]);
H("cms: dims over 2^26", "CountMinSketch", [...u32(0xFFFFFFFF), ...u32(0xFFFFFF), ...u32(64), ...u64(0)]);
H("cms: old form lies", "CountMinSketch", [...u32(1000000), ...u32(3), ...u64(0)]);
H("cms: sparse kind bad", "CountMinSketch", [...u32(0xFFFFFFFF), ...u32(8), ...u32(2), ...u64(0), 7]);

H("hll: precision out of range", "HyperLogLog", [...u32(0xFFFFFFFF), ...u32(3)]);
H("hll: rle zero run", "HyperLogLog", [...u32(0xFFFFFFFF), ...u32(4), 1, ...uvar(0), 0]);
H("hll: rle run overruns", "HyperLogLog", [...u32(0xFFFFFFFF), ...u32(4), 1, ...uvar(9), 0]);
H("hll: register rank impossible", "HyperLogLog", [...u32(0xFFFFFFFF), ...u32(4), 1, ...uvar(1), 200]);
H("hll: old blob size wrong", "HyperLogLog", [...u32(4), ...u32(3), 1, 2, 3]);

H("btree: NaN key", "BTree", [...u32(0xFFFFFFFF), 0, ...u32(1), ...f64(NaN)]);
H("btree: vals mismatch", "BTree", [...u32(0xFFFFFFFF), 0, ...u32(2), ...f64(1), ...f64(2)]);
H("btree: raw count lies", "BTree", [...u32(1000000)]);


/* envelope-level cases that must also refuse */
n++;
try { const l = new S.LRU(4); l.put("a", 1); S.Trie.deserialize(l.serialize()); returned.push("cross-type trie<-lru (DECODED)"); }
catch (e) { refused++; }

for (const cut of [0, 3, 10, 19, 20, 23]) {
    n++;
    try { const g = new S.Graph(); g.addNode(); S.Graph.deserialize(g.serialize().subarray(0, cut)); returned.push("truncated@" + cut + " (DECODED)"); }
    catch (e) { refused++; }
}
n++;
try { const d = new S.Deque(); d.pushBack(1); const b = new Uint8Array(d.serialize()); b[25] ^= 0xff; S.Deque.deserialize(b); returned.push("bitflip (DECODED)"); }
catch (e) { refused++; }
n++;
{
    /* length-field lie with a recomputed CRC: the payload length must
       describe THIS buffer exactly */
    const d = new S.Deque(); d.pushBack(1); const b = new Uint8Array(d.serialize());
    b[12] = 0xff; b[13] = 0xff; b[14] = 0xff; b[15] = 0xff; b[16] = 0xff;
    const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
    b[b.length - 4] = crc & 0xff; b[b.length - 3] = (crc >>> 8) & 0xff;
    b[b.length - 2] = (crc >>> 16) & 0xff; b[b.length - 1] = (crc >>> 24) & 0xff;
    try { S.Deque.deserialize(b); returned.push("length-field lie (DECODED)"); }
    catch (e) { refused++; }
}
n++;
try { const h = new S.Heap(); h.push(1); S.Heap.deserialize(h.serialize(), 42); returned.push("heap opts non-fn (DECODED)"); }
catch (e) { refused++; }

if (returned.length === 0)
    print("test_structures_hostile_forged: all " + n + " hostile records refused cleanly");
else {
    print("test_structures_hostile_forged: " + (n - refused) + " of " + n + " were NOT refused:");
    for (const r of returned) print("  " + r);
    throw new Error("hostile records decoded");
}
