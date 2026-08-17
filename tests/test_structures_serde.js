/*
 * serialize() / deserialize() as ORDINARY METHODS -- the surface contract.
 *
 * test_structures_serialize.js is the wire-format oracle: round trips, byte
 * stability, and the corruption sweeps. This file is the other half, and it is
 * the half that did not exist while serialization was a separate class: what
 * the METHODS themselves promise, on every class, including the ways a caller
 * can hold them wrong.
 *
 * Everything here enumerates from the BINARY -- Object.getOwnPropertyNames on
 * the live module -- never from a hand-written list, because a hand-written
 * list is exactly what stops matching the code.
 */
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

/* One live instance of every class that has a codec, built from the binary's
 * own export list. A class added later joins this sweep for free. */
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
const ARG = { Heap: (a, b) => a - b };     /* the one class needing a rebuild arg */

/* ------------------------------------------------------------------ 1. shape */
{
    /* The class list comes from the module, so a NEW class that forgets a
       codec shows up here as an unbuilt name rather than silently escaping. */
    const exported = Object.keys(S).filter((k) => typeof S[k] === "function");
    const unbuilt = exported.filter((k) => !BUILD[k]);
    check(unbuilt.length === 0,
          "every exported class is covered by this file: missing " +
          JSON.stringify(unbuilt));

    /* There is no separate serialization export any more -- the whole point of
       the change. If one comes back, this says so. */
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

/* ------------------------------------------------- 2. serialize() is a METHOD */
{
    for (const [name, build] of Object.entries(BUILD)) {
        const o = build();
        const C = S[name];
        /* an OWN method of the prototype, not something on Object.prototype */
        check(Object.prototype.hasOwnProperty.call(C.prototype, "serialize"),
              name + ".prototype owns serialize");
        check(Object.prototype.hasOwnProperty.call(C, "deserialize"),
              name + " owns the static deserialize");
        eq(typeof o.serialize, "function", name + " instance sees serialize");
        /* the static is NOT visible on an instance: it takes bytes, not this */
        eq(typeof o.deserialize, "undefined",
           name + " instances must not expose deserialize");
        /* arity: serialize takes nothing, deserialize takes bytes */
        eq(o.serialize.length, 0, name + ".serialize() takes no argument");
        check(C.deserialize.length >= 1, name + ".deserialize takes bytes");
    }
}

/* ------------------------------------------- 3. round trip, every class, twice
   Serializing twice must give identical bytes: the method must hold no state
   between calls, which is what replaced the shared output buffer. */
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

/* ------------------------------- 4. deserialize REFUSES another type's record
   The property the removed generic static could not have. N^2 over every
   pair, because a check that holds only for the pair you thought of is not a
   check. */
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
                /* naming BOTH is what lets a caller act on it */
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

/* ---------------------------------------------- 5. bad arguments, every class */
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

/* ------------------------------------- 6. byte SOURCES a caller may reasonably
   hold: a Uint8Array, a view with an offset, and a bare ArrayBuffer. An
   offset view is the one that silently reads the wrong bytes if the binding
   ignores byteOffset. */
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

    /* A view that is one byte SHORT must be refused, not read past. */
    throwsOf(() => S.SortedSet.deserialize(rec.subarray(0, rec.length - 1)),
             TypeError, "a record one byte short");
}

/* ------------------------------------------- 7. the Heap rebuild argument
   A heap's order IS its comparator and a function is not data, so the record
   cannot carry it. This is the case that justifies a per-class static: the old
   generic decode(bytes, arg) had a second parameter meaningless for 22 of 23
   types. */
{
    const h = new S.Heap((a, b) => b - a);        /* MAX heap */
    [1, 5, 3].forEach((v) => h.push(v));
    const rec = h.serialize();

    /* Omitting the comparator selects natural order rather than failing --
       the same choice `new Heap()` makes. The record carries values, so the
       MAX ordering is not in it and these bytes decode as a min-heap. */
    const nat = S.Heap.deserialize(rec);
    eq(nat.pop(), 1, "omitting the comparator decodes in natural order");

    /* Optional is not permissive: a non-function still names the API. */
    const e = throwsOf(() => S.Heap.deserialize(rec, 42), TypeError,
                       "a Heap with a non-function comparator");
    check(e && String(e.message).indexOf("Heap.deserialize") >= 0,
          "and the message names the CURRENT API, not a removed one: " +
          (e && e.message));

    const back = S.Heap.deserialize(rec, (a, b) => b - a);
    eq(back.pop(), 5, "max-heap order restored from the supplied comparator");

    /* Supplying the OPPOSITE comparator is legal and must be honoured: the
       record carries the elements, the caller carries the order. */
    const flipped = S.Heap.deserialize(rec, (a, b) => a - b);
    eq(flipped.pop(), 1, "the caller's comparator decides the order, not the record");

    /* A non-function where the comparator goes must be refused. */
    throwsOf(() => S.Heap.deserialize(rec, 5), TypeError, "Heap comparator: number");
    throwsOf(() => S.Heap.deserialize(rec, "asc"), TypeError, "Heap comparator: string");
}

/* --------------------------------- 8. serialize() on a detached / closed thing
   These classes are plain GC objects, so there is no close(); the case that
   remains is calling the method with the wrong `this`, which a caller reaches
   by destructuring. */
{
    const o = BUILD.Deque();
    const loose = o.serialize;
    throwsOf(() => loose(), TypeError,
             "serialize() torn off its receiver must throw, not crash");
    throwsOf(() => loose.call({}), TypeError, "serialize() on a plain object");
    /* Borrowing is SAFE, not an error: serialize() dispatches on the real
       class of `this`, so Deque.prototype.serialize.call(trie) writes a TRIE
       record. Worth pinning -- the alternative implementation, keying off the
       method's owner, would silently write the wrong type_id. */
    const tr = new S.Trie(); tr.insert("ab");
    const borrowed = loose.call(tr);
    check(S.Trie.deserialize(borrowed) instanceof S.Trie,
          "serialize() borrowed by another class writes the RECEIVER's type");
    throwsOf(() => S.Deque.deserialize(borrowed), TypeError,
             "and that record is a Trie, so Deque.deserialize refuses it");
    check(loose.call(o) instanceof Uint8Array, "serialize.call(itsOwner) works");
}

/* ------------------------------------------ 9. reentrancy is SAFE, not refused
   List's codec calls the object's own toArray(), which a caller can shadow.
   With a shared output buffer that had to be refused mid-encode; with a buffer
   per call it must simply work, to any depth. */
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

    /* Genuine NESTING needs each inner List to shadow toArray too -- a fresh
       List uses the native path and does not recurse, which is how the first
       version of this case silently tested one level while claiming three. */
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

/* ------------------------------------ 10. a record survives being round-tripped
   through a copy, and equal containers give equal bytes */
{
    for (const [name, build] of Object.entries(BUILD)) {
        const C = S[name];
        const a = build().serialize();
        const b = build().serialize();          /* an INDEPENDENT equal container */
        eq(a.length, b.length, name + ": equal containers give equal-length records");
        let same = true;
        for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) { same = false; break; }
        check(same, name + ": equal containers give IDENTICAL bytes");

        /* decode -> encode -> decode is stable */
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
