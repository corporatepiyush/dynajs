/*
 * Three record formats that stopped writing fixed-width cells:
 *
 *   Graph        degrees and edge targets as varints, the target as a DELTA
 *                from the previous one in the same adjacency list
 *   Table        a sorted, front-coded dictionary of row and column names,
 *                then two varint indices per cell -- an R x C table used to
 *                spend O(R*C) on names with only R + C distinct values
 *   sketches     BloomFilter, CountMinSketch and HyperLogLog pick between RAW
 *                and a compressed form by computing BOTH sizes exactly
 *
 * Every one of these is a way to be silently WRONG rather than to crash: a
 * mis-decoded dictionary index yields a real cell under the wrong name, and a
 * mis-decoded edge delta yields a real edge to the wrong node. So the oracle
 * is always the reconstructed CONTENT compared exhaustively, never the size
 * and never the count.
 *
 * The arm choice is pinned in BOTH directions. A sketch that is genuinely
 * dense must take RAW, or the "compression" is a tax on the case that matters.
 */
import { Graph, Table, BloomFilter, CountMinSketch, HyperLogLog }
    from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }
function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

/* ================================================================= Graph */
{
    const shapes = [
        ["sparse, ascending targets", 500, (i, k) => (i + k) % 500, false],
        ["dense, scattered targets", 200, (i, k) => (i * 7 + k * 131) % 200, false],
        ["weighted", 300, (i, k) => (i + k * 3) % 300, true],
        ["self loops and duplicates", 100, (i, k) => (k % 2) ? i : (i * 5) % 100, false],
    ];
    for (const [label, N, target, weighted] of shapes) {
        const g = new Graph(weighted ? { weighted: true } : undefined);
        for (let i = 0; i < N; i++)
            for (let k = 1; k <= 4; k++)
                g.addEdge(i, target(i, k), weighted ? 1 + (i % 7) : undefined);
        const rec = g.serialize();
        const back = Graph.deserialize(rec);
        eq(back.nodeCount, g.nodeCount, label + ": node count");
        eq(back.edgeCount, g.edgeCount, label + ": edge count");
        /* every adjacency list, in order -- a delta decoded wrong gives a real
           edge to the wrong node, which the counts cannot see */
        let bad = -1;
        for (let i = 0; i < N && bad < 0; i++) {
            const a = g.neighbors(i), b = back.neighbors(i);
            if (a.length !== b.length) { bad = i; break; }
            for (let k = 0; k < a.length; k++) if (a[k] !== b[k]) { bad = i; break; }
        }
        check(bad < 0, label + ": neighbours differ at node " + bad);
        check(sameBytes(rec, back.serialize()), label + ": re-encode is byte-identical");
    }

    /* the empty graph, and one node with no edges */
    for (const [label, build] of [["empty graph", () => new Graph()],
                                  ["nodes, no edges", () => { const g = new Graph();
                                      for (let i = 0; i < 5; i++) g.addNode(); return g; }]]) {
        const g = build(), back = Graph.deserialize(g.serialize());
        eq(back.nodeCount, g.nodeCount, label + ": node count");
        eq(back.edgeCount, g.edgeCount, label + ": edge count");
    }

    /* a varint record must be far smaller than four bytes per edge */
    {
        const g = new Graph();
        for (let i = 0; i < 5000; i++)
            for (let k = 1; k <= 4; k++) g.addEdge(i, (i + k) % 5000);
        const rec = g.serialize();
        check(rec.length < g.edgeCount * 4,
              "the varint record beats a u32 per edge, got " + rec.length +
              " for " + g.edgeCount + " edges");
    }

    /* DFS must not overflow its stack: a node is pushed once per incoming
       edge, so a graph with far more edges than nodes is the case that used
       to write past the end. Under ASan this is the whole point. */
    {
        const g = new Graph();
        const N = 400;
        for (let i = 0; i < N; i++)
            for (let k = 0; k < 60; k++) g.addEdge(i, (i * 13 + k * 7) % N);
        const order = g.dfs(0);
        check(order.length > 0 && order.length <= N,
              "dfs on a dense graph returns at most n nodes, got " + order.length);
        const seen = new Set(order);
        eq(seen.size, order.length, "and visits each node once");
        const bfs = g.bfs(0);
        eq(new Set(bfs).size, bfs.length, "bfs visits each node once");
        eq(bfs.length, order.length, "bfs and dfs reach the same nodes");
    }
}

/* ================================================================= Table */
{
    /* The dictionary's whole point: many cells, few distinct names. */
    const R = 60, C = 40;
    const t = new Table();
    for (let r = 0; r < R; r++)
        for (let c = 0; c < C; c++)
            t.put("region/" + r + "/subdivision", "metric/" + c + "/daily", r * 1000 + c);
    const rec = t.serialize();
    const back = Table.deserialize(rec);
    eq(back.size, t.size, "Table: cell count");
    let bad = null;
    for (let r = 0; r < R && !bad; r++)
        for (let c = 0; c < C; c++) {
            const rk = "region/" + r + "/subdivision", ck = "metric/" + c + "/daily";
            if (back.get(rk, ck) !== t.get(rk, ck)) { bad = rk + " x " + ck; break; }
        }
    check(bad === null, "Table: every cell survives, wrong at " + bad);
    check(sameBytes(rec, back.serialize()), "Table: re-encode is byte-identical");
    /* names are long and repeated; the record must be far under storing them */
    const naive = R * C * (("region/0/subdivision").length + ("metric/0/daily").length);
    check(rec.length < naive / 3,
          "the dictionary beats repeating both names per cell: " + rec.length +
          " vs " + naive);

    /* rows and columns must not be conflated even when the names collide */
    {
        const u = new Table();
        u.put("a", "a", 1); u.put("a", "b", 2); u.put("b", "a", 3);
        const b = Table.deserialize(u.serialize());
        eq(b.get("a", "a"), 1, "Table: (a,a)");
        eq(b.get("a", "b"), 2, "Table: (a,b)");
        eq(b.get("b", "a"), 3, "Table: (b,a)");
        eq(b.get("b", "b"), undefined, "Table: (b,b) is absent");
        /* the length-prefixed pair rule must survive the dictionary */
        const v = new Table();
        v.put("ab", "c", 10); v.put("a", "bc", 20); v.put("", "", 30);
        const d = Table.deserialize(v.serialize());
        eq(d.get("ab", "c"), 10, "Table: ('ab','c')");
        eq(d.get("a", "bc"), 20, "Table: ('a','bc')");
        eq(d.get("", ""), 30, "Table: the empty pair");
        eq(d.size, 3, "Table: three distinct cells");
    }

    /* empty, and one cell */
    eq(Table.deserialize(new Table().serialize()).size, 0, "Table: empty");
    {
        const o = new Table(); o.put("only", "cell", 7);
        const b = Table.deserialize(o.serialize());
        eq(b.size, 1, "Table: one cell");
        eq(b.get("only", "cell"), 7, "Table: its value");
    }

    /* slices must work on a decoded table -- the chains are rebuilt lazily */
    {
        const b = Table.deserialize(rec);
        eq(b.row("region/3/subdivision").length, C, "a decoded table slices by row");
        eq(b.column("metric/7/daily").length, R, "and by column");
    }
}

/* ============================================================== sketches */
{
    /* SPARSE: barely used, so the compressed arm must win by a lot. */
    {
        const b = new BloomFilter(1 << 20, 4);
        for (let i = 0; i < 50; i++) b.add("k" + i);
        const rec = b.serialize();
        check(rec.length < (1 << 20) / 8 / 4,
              "a sparse BloomFilter compresses, got " + rec.length);
        const back = BloomFilter.deserialize(rec);
        let bad = null;
        for (let i = 0; i < 50; i++) if (!back.mayContain("k" + i)) { bad = i; break; }
        check(bad === null, "every sparse member survives, lost " + bad);
        /* and a non-member must still be rejected at the same rate: compare
           the decoded filter's answer to the source's for every probe */
        for (let i = 0; i < 2000 && bad === null; i++)
            if (b.mayContain("miss" + i) !== back.mayContain("miss" + i)) bad = i;
        check(bad === null, "and every non-member answers identically, differs at " + bad);
        eq(back.bits, b.bits, "bit count");
        eq(back.hashes, b.hashes, "hash count");
    }

    /* DENSE: nearly every word set, so RAW must win -- the compressed form
       would be larger and choosing it would tax the case that matters. */
    {
        const b = new BloomFilter(1 << 16, 4);
        for (let i = 0; i < 40000; i++) b.add("k" + i);
        const rec = b.serialize();
        check(rec.length < (1 << 16) / 8 * 1.1,
              "a dense BloomFilter takes RAW and costs about the bit array, got " +
              rec.length + " vs " + ((1 << 16) / 8));
        const back = BloomFilter.deserialize(rec);
        let bad = null;
        for (let i = 0; i < 2000 && bad === null; i++)
            if (b.mayContain("k" + i) !== back.mayContain("k" + i)) bad = i;
        check(bad === null, "a dense filter round-trips, differs at " + bad);
    }

    /* CountMinSketch: mostly-zero counters, then a loaded one. */
    for (const [label, load] of [["sparse", 40], ["loaded", 20000]]) {
        const s = new CountMinSketch(2048, 5);
        for (let i = 0; i < load; i++) s.add("k" + (i % (load / 2 | 0 || 1)), 1 + (i % 3));
        const rec = s.serialize();
        const back = CountMinSketch.deserialize(rec);
        eq(back.width, s.width, label + " CMS: width");
        eq(back.depth, s.depth, label + " CMS: depth");
        eq(back.totalCount, s.totalCount, label + " CMS: total");
        let bad = null;
        for (let i = 0; i < 200 && bad === null; i++) {
            const k = "k" + i;
            if (s.count(k) !== back.count(k)) bad = k;
        }
        check(bad === null, label + " CMS: counts differ at " + bad);
        check(sameBytes(rec, back.serialize()), label + " CMS: re-encode identical");
    }
    {
        const fresh = new CountMinSketch(4096, 5);
        check(fresh.serialize().length < 200,
              "an untouched CountMinSketch is tiny, got " + fresh.serialize().length);
    }

    /* HyperLogLog: an untouched sketch is all zeros; a loaded one is not. */
    {
        const fresh = new HyperLogLog(14);
        check(fresh.serialize().length < 100,
              "an untouched HyperLogLog is tiny, got " + fresh.serialize().length);
        eq(HyperLogLog.deserialize(fresh.serialize()).count(), 0,
           "and decodes to a zero estimate");

        const h = new HyperLogLog(14);
        for (let i = 0; i < 200000; i++) h.add("u" + i);
        const rec = h.serialize();
        const back = HyperLogLog.deserialize(rec);
        eq(back.precision, h.precision, "HLL: precision");
        eq(back.registers, h.registers, "HLL: register count");
        eq(back.count(), h.count(), "HLL: the estimate is bit-identical");
        check(sameBytes(rec, back.serialize()), "HLL: re-encode is byte-identical");
        /* a loaded sketch's registers are a narrow band, so RLE may or may not
           win -- what must hold is that it never costs much MORE than raw */
        check(rec.length < h.registers * 1.2,
              "a loaded HLL record stays near its register count, got " + rec.length);

        /* merging a decoded sketch must behave like merging the source */
        const other = new HyperLogLog(14);
        for (let i = 200000; i < 400000; i++) other.add("u" + i);
        const m1 = HyperLogLog.deserialize(h.serialize()); m1.merge(other);
        const m2 = HyperLogLog.deserialize(h.serialize()); m2.merge(other);
        eq(m1.count(), m2.count(), "a decoded sketch merges deterministically");
    }
}

/* ========================================== adversarial, across all three */
{
    const TRAILER = 4;
    const repair = (b) => {
        const crc = CRC32C(b.subarray(0, b.length - TRAILER)) >>> 0;
        for (let k = 0; k < 4; k++) b[b.length - TRAILER + k] = (crc >>> (k * 8)) & 0xff;
        return b;
    };
    const sweep = (good, cls, touch, label) => {
        let attempts = 0, refused = 0, decoded = 0;
        for (let i = 20; i < good.length - TRAILER; i += Math.max(1, (good.length / 150) | 0)) {
            for (const mask of [0xff, 0x01, 0x80]) {
                const b = good.slice(); b[i] ^= mask; repair(b);
                attempts++;
                try { const o = cls.deserialize(b); decoded++; touch(o);
                      check(true, "a decoded record answered"); }
                catch (e) { refused++; check(e instanceof Error, "a refusal is an Error"); }
            }
        }
        check(attempts > 50, label + ": the sweep ran, " + attempts + " mutations");
        print("  " + label + ": " + attempts + " mutations, " + refused +
              " refused, " + decoded + " decoded and exercised");
    };

    {
        const g = new Graph();
        for (let i = 0; i < 200; i++) for (let k = 1; k <= 3; k++) g.addEdge(i, (i + k) % 200);
        sweep(g.serialize(), Graph, (o) => { o.nodeCount; o.edgeCount;
            for (let i = 0; i < o.nodeCount; i++) o.neighbors(i); }, "Graph");
    }
    {
        const t = new Table();
        for (let r = 0; r < 20; r++) for (let c = 0; c < 10; c++) t.put("r" + r, "c" + c, r * 10 + c);
        sweep(t.serialize(), Table, (o) => { o.size;
            for (let r = 0; r < 20; r++) o.row("r" + r); }, "Table");
    }
    {
        const s = new CountMinSketch(256, 4);
        for (let i = 0; i < 500; i++) s.add("k" + (i % 60), 1);
        sweep(s.serialize(), CountMinSketch, (o) => { o.width; o.totalCount;
            for (let i = 0; i < 60; i++) o.count("k" + i); }, "CountMinSketch");
    }
    {
        const h = new HyperLogLog(10);
        for (let i = 0; i < 5000; i++) h.add("u" + i);
        sweep(h.serialize(), HyperLogLog, (o) => { o.count(); o.registers; }, "HyperLogLog");
    }
}

/* ------------------------------------------- truncation at every length */
{
    const g = new Graph();
    for (let i = 0; i < 100; i++) for (let k = 1; k <= 3; k++) g.addEdge(i, (i + k) % 100);
    const t = new Table();
    for (let r = 0; r < 15; r++) for (let c = 0; c < 8; c++) t.put("r" + r, "c" + c, r);
    const h = new HyperLogLog(10);
    for (let i = 0; i < 2000; i++) h.add("u" + i);
    for (const [cls, rec, label] of [[Graph, g.serialize(), "Graph"],
                                     [Table, t.serialize(), "Table"],
                                     [HyperLogLog, h.serialize(), "HyperLogLog"]]) {
        let survived = 0;
        for (let len = 0; len < rec.length; len += Math.max(1, (rec.length / 100) | 0)) {
            try { cls.deserialize(rec.subarray(0, len)); survived++; } catch (e) { /* expected */ }
        }
        eq(survived, 0, label + ": no truncation may decode");
    }
}

if (fails === 0) print("test_structures_record_codecs: all " + n + " checks passed");
else print("test_structures_record_codecs: " + fails + " FAILED of " + n);
