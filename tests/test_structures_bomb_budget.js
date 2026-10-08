// timeout: 300
import * as S from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }

function u16(v) { return [v & 0xff, (v >>> 8) & 0xff]; }
function u32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }
function u64(v) { return [...u32(v >>> 0), ...u32(Math.floor(v / 4294967296))]; }
function f64(v) { const b = new Uint8Array(8); new DataView(b.buffer).setFloat64(0, v, true); return [...b]; }
function uvar(v) { const out = []; do { let b = v & 0x7f; v = Math.floor(v / 128); if (v) b |= 0x80; out.push(b); } while (v); return out; }

function record(tid, payload) {
    const head = [0x44, 0x59, 0x4e, 0x53, ...u16(1), ...u16(tid), ...u32(0), ...u64(payload.length)];
    const body = [...head, ...payload];
    const crc = CRC32C(new Uint8Array(body)) >>> 0;
    return new Uint8Array([...body, ...u32(crc)]);
}

const TID = { BitSet: 1, UnionFind: 2, Deque: 3, Fenwick: 4, RingBuffer: 5, SegTree: 6, BloomFilter: 7, Trie: 8, LRU: 9, SortedSet: 10, SortedMap: 11, Heap: 12, List: 13, Graph: 14, Multiset: 20, Multimap: 21, BiMap: 22, Table: 23, RangeSet: 24, RangeMap: 25, IntervalTree: 26, MinMaxHeap: 27, CountMinSketch: 28, HyperLogLog: 29, BTree: 30 };

function refused(clsName, payload, label) {
    n++;
    try {
        const obj = S[clsName].deserialize(record(TID[clsName], new Uint8Array(payload)));
        fails++;
        print("FAIL: " + label + " decoded to " + (obj && obj.constructor && obj.constructor.name));
        return false;
    } catch (e) {
        if (e instanceof TypeError || e instanceof RangeError || e instanceof SyntaxError || e instanceof Error)
            return true;
        fails++;
        print("FAIL: " + label + " threw a non-Error " + e);
        return false;
    }
}

function roundTrip(build, clsName, label) {
    n++;
    try {
        const src = build();
        const back = S[clsName].deserialize(src.serialize());
        if (!back) { fails++; print("FAIL: " + label + " did not survive a round trip"); }
        return back;
    } catch (e) {
        fails++;
        print("FAIL: " + label + " threw on a legitimate record: " + e);
        return null;
    }
}

const HEADER = 20, TRAILER = 4;

{
    const fen = record(TID.Fenwick, new Uint8Array([...u32(0xFFFFFFFF), ...u32(1 << 24), 1, ...f64(1)]));
    check(fen.length < 64, "the Fenwick bomb record is tiny, got " + fen.length);
    refused("Fenwick", [...u32(0xFFFFFFFF), ...u32(1 << 24), 1, ...f64(1)],
        "SEC-047 fenwick constant 2^24");
}

{
    const seg = record(TID.SegTree, new Uint8Array([...u32(0xFFFFFFFF), ...u32(0), ...u32(0xFFFFFF), 1, ...f64(1)]));
    check(seg.length < 64, "the SegTree bomb record is tiny, got " + seg.length);
    refused("SegTree", [...u32(0xFFFFFFFF), ...u32(0), ...u32(0xFFFFFF), 1, ...f64(1)],
        "SEC-047 segtree constant 0xFFFFFF elems");
}

{
    const uf = record(TID.UnionFind, new Uint8Array([...u32(0xFFFFFFFF), ...u32(1 << 24), ...u32(1), 0]));
    check(uf.length < 64, "the UnionFind bomb record is tiny, got " + uf.length);
    refused("UnionFind", [...u32(0xFFFFFFFF), ...u32(1 << 24), ...u32(1), 0],
        "SEC-047 union-find identity 2^24");
}

{
    const bloom = record(TID.BloomFilter, new Uint8Array([...u32(0xFFFFFFFF), ...u32(1 << 30), ...u32(1), 1, ...uvar(0)]));
    check(bloom.length < 64, "the Bloom bomb record is tiny, got " + bloom.length);
    refused("BloomFilter", [...u32(0xFFFFFFFF), ...u32(1 << 30), ...u32(1), 1, ...uvar(0)],
        "SEC-048 bloom ext 2^30 bits sparse");
}

{
    const cms = record(TID.CountMinSketch, new Uint8Array([...u32(0xFFFFFFFF), ...u32(1 << 18), ...u32(64), ...u64(0), 1, ...uvar(0)]));
    check(cms.length < 64, "the CMS bomb record is tiny, got " + cms.length);
    refused("CountMinSketch", [...u32(0xFFFFFFFF), ...u32(1 << 18), ...u32(64), ...u64(0), 1, ...uvar(0)],
        "SEC-048 cms ext 2^24 counters sparse");
}

{
    const chunks = [];
    for (let i = 0; i < 16384; i++) chunks.push(0);
    const bs = record(TID.BitSet, new Uint8Array([...u32(0xFFFFFFFF), ...u32(1 << 24), ...u32(16384), ...chunks]));
    check(bs.length < 20000, "the BitSet bomb record is small, got " + bs.length);
    refused("BitSet", [...u32(0xFFFFFFFF), ...u32(1 << 24), ...u32(16384), ...chunks],
        "SEC-131 chunked bitset 2^24 words from empty chunks");
}

{
    const f = roundTrip(() => { const x = new S.Fenwick(4096); x.update(0, 1); return x; }, "Fenwick", "legit Fenwick");
    if (f) check(f.prefixSum(0) === 1, "legit Fenwick value");
    const b = roundTrip(() => new S.BitSet(1 << 20), "BitSet", "legit empty 1M-bit BitSet");
    if (b) check(b.count === 0 && b.toArray().length === 0, "legit BitSet shape");
    const s = roundTrip(() => {
        const x = new S.BitSet(1 << 20);
        for (let i = 200000; i < 800000; i++) x.set(i);
        return x;
    }, "BitSet", "legit run-encoded 1M-bit BitSet");
    if (s) check(s.count === 600000, "legit BitSet run count");
    const bl = roundTrip(() => { const x = new S.BloomFilter(1 << 16, 3); x.add("a"); return x; }, "BloomFilter", "legit Bloom");
    if (bl) check(bl.mayContain("a") === true, "legit Bloom membership");
    const c = roundTrip(() => { const x = new S.CountMinSketch(256, 4); x.add("a", 3); return x; }, "CountMinSketch", "legit CMS");
    if (c) check(c.count("a") === 3, "legit CMS count");
    const sg = roundTrip(() => { const x = new S.SegTree(1024, "sum"); x.update(0, 7); return x; }, "SegTree", "legit SegTree");
    if (sg) check(sg.rangeQuery(0, 0) === 7, "legit SegTree query");
    const uf = roundTrip(() => { const x = new S.UnionFind(1024); x.union(0, 1); return x; }, "UnionFind", "legit UnionFind");
    if (uf) check(uf.find(0) === uf.find(1), "legit UnionFind join");
}

{
    n++;
    const big = new S.Fenwick(1 << 20);
    big.update(0, 0.5);
    const rec = big.serialize();
    const back = S.Fenwick.deserialize(rec);
    if (!(back instanceof S.Fenwick) || back.prefixSum(0) !== 0.5)
        { fails++; print("FAIL: a 1M-entry raw Fenwick must still round-trip, got " + rec.length + " bytes"); }
}

{
    // R3-1: an arithmetic SortedSet/SortedMap record declares its key count
    // with no payload backing, so the decode budget must charge it.
    const n = 1 << 24;
    const payload = [...u32(0xFFFFFFFF), 1, ...u32(n), ...f64(0), ...f64(1)];
    const rec = record(TID.SortedSet, payload);
    check(rec.length < 64, "the skip-arith SortedSet bomb is tiny, got " + rec.length);
    refused("SortedSet", payload, "R3-1 sortedset arithmetic 2^24 keys");
    refused("SortedMap", payload, "R3-1 sortedmap arithmetic 2^24 keys");
    const set = roundTrip(() => {
        const x = new S.SortedSet();
        for (let i = 0; i < 100000; i++) x.add(i);
        return x;
    }, "SortedSet", "R3-1 legit 100k arithmetic SortedSet");
    if (set) check(set.size === 100000 && set.has(99999), "R3-1 legit arithmetic SortedSet shape");
}

{
    // R3-2: an all-constant Fenwick(2^20) is a legitimate 41-byte record
    // (8 MiB native) and must round-trip under the bounded per-record ceiling.
    n++;
    const rec = new S.Fenwick(1 << 20).serialize();
    try {
        const back = S.Fenwick.deserialize(rec);
        if (!(back instanceof S.Fenwick) || back.prefixSum(1 << 19) !== 0)
            { fails++; print("FAIL: all-constant Fenwick(2^20) round trip, got " + rec.length + " bytes"); }
    } catch (e) {
        fails++;
        print("FAIL: all-constant Fenwick(2^20) must round-trip (" + rec.length + " B): " + e);
    }
}

{
    // R5-1: legitimate compressed records at the documented 2^20..2^22 scale
    // must round-trip. Each writer picks its amplified encoding (constant
    // numeric arrays, arithmetic skip keys, BTree key runs), so these pin the
    // top of the legit-amplification envelope the decoder budget must admit.
    for (const p of [20, 21, 22]) {
        const n = 1 << p;
        const fen = roundTrip(() => new S.Fenwick(n), "Fenwick",
            "R5-1 all-constant Fenwick(2^" + p + ")");
        if (fen) check(fen.prefixSum(n - 1) === 0, "R5-1 Fenwick(2^" + p + ") value");
        const set = roundTrip(() => {
            const x = new S.SortedSet();
            for (let i = 0; i < n; i++) x.add(i);
            return x;
        }, "SortedSet", "R5-1 arithmetic SortedSet(2^" + p + ")");
        if (set) check(set.size === n && set.has(n - 1), "R5-1 SortedSet(2^" + p + ") shape");
        const map = roundTrip(() => {
            const x = new S.SortedMap();
            for (let i = 0; i < n; i++) x.set(i, i * 2);
            return x;
        }, "SortedMap", "R5-1 arithmetic SortedMap(2^" + p + ")");
        if (map) check(map.size === n && map.get(n - 1) === (n - 1) * 2,
            "R5-1 SortedMap(2^" + p + ") shape");
    }
    const bt = roundTrip(() => {
        const x = new S.BTree();
        for (let i = 0; i < (1 << 22); i++) x.set(i, i);
        return x;
    }, "BTree", "R5-1 BTree(2^22)");
    if (bt) check(bt.size === (1 << 22) && bt.get((1 << 22) - 1) === (1 << 22) - 1,
        "R5-1 BTree(2^22) shape");
    // Payload-backed numeric arrays are not amplified from declared counts, so
    // the raw path keeps its 2^24-element reach (a 134 MB record, 128 MiB native).
    const raw = roundTrip(() => {
        const x = new S.Fenwick(1 << 24);
        x.update(0, 0.5);
        return x;
    }, "Fenwick", "R5-1 raw Fenwick(2^24)");
    if (raw) check(raw.prefixSum(0) === 0.5, "R5-1 raw Fenwick(2^24) value");
}

{
    // R5-2: exact decode boundaries under the widened envelope. The amplified
    // encodings cap at their legitimate scale (numeric 2^23 elements,
    // skip-arith 2^22 keys, chunked-bitmap/sketch 2^22 words); one step past
    // each cap is refused, and the SEC-047/048/131 bomb records stay refused.
    refused("Fenwick", [...u32(0xFFFFFFFF), ...u32((1 << 23) + 1), 1, ...f64(1)],
        "R5-2 fenwick constant 2^23+1 (numeric element cap)");
    refused("SortedSet", [...u32(0xFFFFFFFF), 1, ...u32((1 << 22) + 1), ...f64(0), ...f64(1)],
        "R5-2 sortedset arithmetic 2^22+1 (skip-key cap)");
    refused("SortedMap", [...u32(0xFFFFFFFF), 1, ...u32((1 << 22) + 1), ...f64(0), ...f64(1)],
        "R5-2 sortedmap arithmetic 2^22+1 (skip-key cap)");
    {
        const nw = (1 << 22) + 1024, chunks = new Array(nw / 1024).fill(0);
        refused("BitSet", [...u32(0xFFFFFFFF), ...u32(nw), ...u32(nw / 1024), ...chunks],
            "R5-2 chunked bitset 2^22+1024 words (chunk-word cap)");
    }
    refused("BloomFilter", [...u32(0xFFFFFFFF), ...u32((1 << 28) + 4096), ...u32(1), 1, ...uvar(0)],
        "R5-2 bloom ext nwords 2^22+64 (sketch-word cap)");
    refused("CountMinSketch", [...u32(0xFFFFFFFF), ...u32((1 << 16) + 1), ...u32(64), ...u64(0), 1, ...uvar(0)],
        "R5-2 cms ext counters 2^22+1 (sketch-word cap)");
}

if (fails === 0) print("test_structures_bomb_budget: all " + n + " checks passed");
else {
    print("test_structures_bomb_budget: " + fails + " FAILED of " + n);
    throw new Error("test_structures_bomb_budget: " + fails + " failures");
}
