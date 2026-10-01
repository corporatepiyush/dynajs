import * as S from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + a + ", want " + b); }
function throwsOf(fn, kind, m) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    check(e !== null, m + " (expected a throw)");
    if (e && kind) check(e instanceof kind,
        m + " (wanted " + kind.name + ", got " + (e && e.constructor.name) + ")");
    return e;
}

const BUILD = {
    BiMap: () => { const x = new S.BiMap(); x.set("a", 1); return x; },
    BitSet: () => { const x = new S.BitSet(64); x.set(3); return x; },
    BloomFilter: () => { const x = new S.BloomFilter(100, 0.01); x.add("a"); return x; },
    CountMinSketch: () => { const x = new S.CountMinSketch(64, 4); x.add("a"); return x; },
    BTree: () => { const x = new S.BTree(); x.set(1, "a"); x.set(2, "b"); return x; },
    Deque: () => { const x = new S.Deque(); x.pushBack(1); return x; },
    Fenwick: () => new S.Fenwick(8),
    Graph: () => { const x = new S.Graph(); x.addNode("a"); return x; },
    Heap: () => { const x = new S.Heap((a, b) => a - b); x.push(1); return x; },
    HyperLogLog: () => { const x = new S.HyperLogLog(); x.add("a"); return x; },
    IntervalTree: () => { const x = new S.IntervalTree(); x.insert(1, 2, "v"); return x; },
    LRU: () => { const x = new S.LRU(4); x.set("a", 1); return x; },
    List: () => { const x = new S.List(); x.pushBack(1); return x; },
    MinMaxHeap: () => { const x = new S.MinMaxHeap(); x.push(1); return x; },
    Multimap: () => { const x = new S.Multimap(); x.put("a", 1); return x; },
    Multiset: () => { const x = new S.Multiset(); x.add("a"); return x; },
    RangeMap: () => { const x = new S.RangeMap(); x.put(1, 2, "v"); return x; },
    RangeSet: () => { const x = new S.RangeSet(); x.add(1, 2); return x; },
    RingBuffer: () => { const x = new S.RingBuffer(4); x.push(1); return x; },
    SegTree: () => new S.SegTree(8),
    SortedMap: () => { const x = new S.SortedMap(); x.set(1, "a"); return x; },
    SortedSet: () => { const x = new S.SortedSet(); x.add(1); return x; },
    Table: () => { const x = new S.Table(); x.put("r", "c", 1); return x; },
    Trie: () => { const x = new S.Trie(); x.insert("ab"); return x; },
    UnionFind: () => new S.UnionFind(8),
};
const ARG = { Heap: (a, b) => a - b };

{
    const exported = Object.keys(S).filter((k) => typeof S[k] === "function");
    const unbuilt = exported.filter((k) => !BUILD[k]);
    check(unbuilt.length === 0,
          "every exported class is covered by this file: missing " +
          JSON.stringify(unbuilt));

    for (const gone of ["Serializer", "typeOf", "deserialize", "encode", "decode"])
        check(!(gone in S), "dyna:structures must not export '" + gone + "'");

    let withSer = 0, withDe = 0;
    for (const name of exported) {
        const C = S[name];
        if (typeof C.prototype.serialize === "function") withSer++;
        if (typeof C.deserialize === "function") withDe++;
    }
    eq(withSer, exported.length, "every class has an instance serialize()");
    eq(withDe, exported.length, "every class has a static deserialize()");
}

{
    for (const [name, build] of Object.entries(BUILD)) {
        const o = build();
        const C = S[name];
        check(Object.prototype.hasOwnProperty.call(C.prototype, "serialize"),
              name + ".prototype owns serialize");
        check(Object.prototype.hasOwnProperty.call(C, "deserialize"),
              name + " owns the static deserialize");
        eq(typeof o.serialize, "function", name + " instance sees serialize");
        eq(typeof o.deserialize, "undefined",
           name + " instances must not expose deserialize");
        eq(o.serialize.length, 0, name + ".serialize() takes no argument");
        check(C.deserialize.length >= 1, name + ".deserialize takes bytes");
    }
}

{
    for (const [name, build] of Object.entries(BUILD)) {
        const C = S[name];
        const o = build();
        const a = o.serialize();
        const b = o.serialize();
        check(a instanceof Uint8Array, name + " serialize returns a Uint8Array");
        eq(a.length, b.length, name + " two serializes agree in length");
        let same = true;
        for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) { same = false; break; }
        check(same, name + " two serializes are byte-identical");

        const back = C.deserialize(a, ARG[name]);
        check(back instanceof C, name + " deserialize returns a " + name);
        check(back !== o, name + " deserialize returns a NEW object");
        const c = back.serialize();
        eq(c.length, a.length, name + " re-serialize length matches");
    }
}

{
    const recs = [];
    for (const [name, build] of Object.entries(BUILD))
        recs.push([name, S[name], build().serialize()]);

    let pairs = 0, refused = 0, named = 0;
    for (const [nameA, C] of recs) {
        for (const [nameB, CB, bytesB] of recs) {
            if (C === CB) continue;
            pairs++;
            const e = (() => { try { C.deserialize(bytesB, ARG[nameA]); return null; }
                               catch (x) { return x; } })();
            if (e) {
                refused++;
                const msg = String(e.message || e);
                if (msg.indexOf(nameA) >= 0 && msg.indexOf(nameB) >= 0) named++;
            }
        }
    }
    check(pairs > 400, "the sweep covers real pairs, got " + pairs);
    eq(refused, pairs, "EVERY cross-type deserialize is refused");
    eq(named, pairs, "and every refusal names BOTH types");
    print("  cross-type: " + pairs + " pairs, " + refused + " refused, " +
          named + " naming both");
}

{
    for (const [name] of Object.entries(BUILD)) {
        const C = S[name];
        throwsOf(() => C.deserialize(), TypeError, name + ".deserialize()");
        throwsOf(() => C.deserialize(null), TypeError, name + ".deserialize(null)");
        throwsOf(() => C.deserialize("bytes"), TypeError, name + ".deserialize(string)");
        throwsOf(() => C.deserialize(42), TypeError, name + ".deserialize(number)");
        throwsOf(() => C.deserialize({}), TypeError, name + ".deserialize({})");
        throwsOf(() => C.deserialize([]), TypeError, name + ".deserialize([])");
        throwsOf(() => C.deserialize(new Uint8Array(0)), TypeError,
                 name + ".deserialize(empty)");
        throwsOf(() => C.deserialize(new Uint8Array(8)), TypeError,
                 name + ".deserialize(too short)");
    }
}

{
    const o = BUILD.SortedSet();
    const rec = o.serialize();

    check(S.SortedSet.deserialize(rec) instanceof S.SortedSet, "plain Uint8Array");

    const padded = new Uint8Array(rec.length + 16);
    padded.set(rec, 7);
    const view = padded.subarray(7, 7 + rec.length);
    check(S.SortedSet.deserialize(view) instanceof S.SortedSet,
          "a subarray VIEW at a non-zero byteOffset must read ITS bytes");

    const ab = rec.slice().buffer;
    check(S.SortedSet.deserialize(ab) instanceof S.SortedSet, "a bare ArrayBuffer");

    throwsOf(() => S.SortedSet.deserialize(rec.subarray(0, rec.length - 1)),
             TypeError, "a record one byte short");
}

{
    const h = new S.Heap((a, b) => b - a);
    [1, 5, 3].forEach((v) => h.push(v));
    const rec = h.serialize();

    const nat = S.Heap.deserialize(rec);
    eq(nat.pop(), 1, "omitting the comparator decodes in natural order");

    const e = throwsOf(() => S.Heap.deserialize(rec, 42), TypeError,
                       "a Heap with a non-function comparator");
    check(e && String(e.message).indexOf("Heap.deserialize") >= 0,
          "and the message names the CURRENT API, not a removed one: " +
          (e && e.message));

    const back = S.Heap.deserialize(rec, (a, b) => b - a);
    eq(back.pop(), 5, "max-heap order restored from the supplied comparator");

    const flipped = S.Heap.deserialize(rec, (a, b) => a - b);
    eq(flipped.pop(), 1, "the caller's comparator decides the order, not the record");

    throwsOf(() => S.Heap.deserialize(rec, 5), TypeError, "Heap comparator: number");
    throwsOf(() => S.Heap.deserialize(rec, "asc"), TypeError, "Heap comparator: string");
}

{
    const o = BUILD.Deque();
    const loose = o.serialize;
    throwsOf(() => loose(), TypeError,
             "serialize() torn off its receiver must throw, not crash");
    throwsOf(() => loose.call({}), TypeError, "serialize() on a plain object");
    const tr = new S.Trie(); tr.insert("ab");
    const borrowed = loose.call(tr);
    check(S.Trie.deserialize(borrowed) instanceof S.Trie,
          "serialize() borrowed by another class writes the RECEIVER's type");
    throwsOf(() => S.Deque.deserialize(borrowed), TypeError,
             "and that record is a Trie, so Deque.deserialize refuses it");
    check(loose.call(o) instanceof Uint8Array, "serialize.call(itsOwner) works");
}

{
    let inner = null;
    const l = new S.List();
    l.pushBack(1); l.pushBack(2);
    l.toArray = function () { inner = BUILD.BitSet().serialize(); return [1, 2]; };
    const out = l.serialize();
    check(inner instanceof Uint8Array && S.BitSet.deserialize(inner) instanceof S.BitSet,
          "the inner serialize completed rather than being refused");
    eq(S.List.deserialize(out).toArray().join(","), "1,2",
       "and the outer record is exact");

    let depth = 0;
    const nest = (level) => {
        const q = new S.List();
        q.pushBack(level);
        q.toArray = function () {
            depth = Math.max(depth, level);
            if (level < 3) nest(level + 1).serialize();
            return [level];
        };
        return q;
    };
    eq(S.List.deserialize(nest(1).serialize()).toArray().join(","), "1",
       "three levels of reentrancy stay correct");
    eq(depth, 3, "and all three levels really ran");
}

{
    for (const [name, build] of Object.entries(BUILD)) {
        const C = S[name];
        const a = build().serialize();
        const b = build().serialize();
        eq(a.length, b.length, name + ": equal containers give equal-length records");
        let same = true;
        for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) { same = false; break; }
        check(same, name + ": equal containers give IDENTICAL bytes");

        const once = C.deserialize(a, ARG[name]).serialize();
        const twice = C.deserialize(once, ARG[name]).serialize();
        let stable = once.length === twice.length;
        for (let i = 0; stable && i < once.length; i++)
            if (once[i] !== twice[i]) stable = false;
        check(stable, name + ": decode/encode reaches a fixed point");
    }
}

if (fails === 0) print("test_structures_serde: all " + n + " checks passed");
else print("test_structures_serde: " + fails + " FAILED of " + n);
