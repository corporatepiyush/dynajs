// covers: integration dyna:docdb.Collection.find dyna:serialize.BSONDecode -- the BSON iterator under both docdb queries and BSONDecode
// The BSON iterator (src/core/bson/bson-iter.c dyn_bson_iter_next, which every docdb path walk, filter, sort key,
// projection and decode runs on) finds a key's terminator with an inline scan of the first 24 bytes before falling
// back to memchr, and sizes the fixed-width types (double, ObjectId, bool, date, null, undefined, int32, timestamp,
// int64, decimal128) from a table instead of calling dyn_bson_value_size. The oracle builds BSON bytes by hand in JS
// (little-endian lengths, typed payloads) with key lengths 1..40 around the 24-byte switch and every fixed type next
// to strings, documents and arrays: BSONDecode returns the hand-computed values, and docdb finds each document by
// every field (equality, $exists, $gt) with exactly the expected hit sets.
// dyn_bsonjs_ensure (src/modules/vserialize/vserialize-bson-types.c) probes its lazily created BSON classes only once
// JS_IsRegisteredClass says the id is registered: a docdb-only build (module matrix row docdb) allocates those ids
// last, past rt->class_count, so an unguarded JS_GetClassProto asserted on import (matrix mutant M8).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_docdb_bson_iter_key_scan_oracle");
import { Database } from "dyna:docdb";
import { BSONDecode } from "dyna:serialize";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); }
}
const enc = new TextEncoder();
function cat(parts) { const n = parts.reduce((s, p) => s + p.length, 0), o = new Uint8Array(n); let k = 0; for (const p of parts) { o.set(p, k); k += p.length; } return o; }
const u32 = (v) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, v >>> 0, true); return b; };
const i32 = (v) => { const b = new Uint8Array(4); new DataView(b.buffer).setInt32(0, v, true); return b; };
const f64 = (v) => { const b = new Uint8Array(8); new DataView(b.buffer).setFloat64(0, v, true); return b; };
const i64 = (v) => { const b = new Uint8Array(8); new DataView(b.buffer).setBigInt64(0, v, true); return b; };
const cstr = (s) => cat([enc.encode(s), new Uint8Array([0])]);
const str = (s) => { const e = enc.encode(s); return cat([u32(e.length + 1), e, new Uint8Array([0])]); };
function doc(elems) { const body = cat(elems.map(([t, k, v]) => cat([new Uint8Array([t]), cstr(k), v]))); return cat([u32(body.length + 5), body, new Uint8Array([0])]); }
const keyOf = (len, tag) => (tag + "_".repeat(40)).slice(0, len);

__kitS.section("BSONDecode reads every fixed-width type after keys of 1..40 bytes", () => {
    for (let len = 1; len <= 40; len++) {
        const k = (t) => keyOf(len, t);
        const oid = Uint8Array.from({ length: 12 }, (_, i) => i * 7 + len);
        const bytes = doc([
            [0x01, k("d"), f64(len + 0.25)],
            [0x10, k("i"), i32(-len)],
            [0x08, k("b"), new Uint8Array([len & 1])],
            [0x0a, k("n"), new Uint8Array(0)],
            [0x09, k("t"), i64(BigInt(1.7e12 + len))],
            [0x12, k("l"), i64(BigInt(len) * 1000000000000n)],
            [0x07, k("o"), oid],
            [0x02, k("s"), str("v" + len)],
            [0x03, k("e"), doc([[0x10, k("x"), i32(len)]])],
            [0x04, k("a"), doc([[0x10, "0", i32(1)], [0x01, "1", f64(2.5)]])],
            [0x10, k("z"), i32(len * 3)],
        ]);
        const o = BSONDecode(bytes);
        const want = [k("d"), k("i"), k("b"), k("n"), k("t"), k("l"), k("o"), k("s"), k("e"), k("a"), k("z")];
        eq(Object.keys(o).join("|"), want.join("|"), "len " + len + " key order");
        eq(o[k("d")], len + 0.25, "len " + len + " double");
        eq(o[k("i")], -len, "len " + len + " int32");
        eq(o[k("b")], (len & 1) === 1, "len " + len + " bool");
        eq(o[k("n")], null, "len " + len + " null");
        eq(o[k("t")] instanceof Date && o[k("t")].getTime(), 1.7e12 + len, "len " + len + " date");
        eq(o[k("l")], BigInt(len) * 1000000000000n, "len " + len + " int64");
        eq(o[k("o")].toHexString(), Array.from(oid, (x) => x.toString(16).padStart(2, "0")).join(""), "len " + len + " oid");
        eq(o[k("s")], "v" + len, "len " + len + " string");
        eq(o[k("e")][k("x")], len, "len " + len + " nested");
        eq(JSON.stringify(o[k("a")]), "[1,2.5]", "len " + len + " array");
        eq(o[k("z")], len * 3, "len " + len + " field after the fixed run");
    }
});

__kitS.section("docdb Collection.find finds every document by every field across key lengths", () => {
    const db = new Database("bsoniter"), c = db.collection("c"), docs = [];
    for (let i = 0; i < 120; i++) {
        const len = 1 + (i % 40), o = { _id: i };
        o[keyOf(len, "num")] = i % 9;
        o[keyOf(len, "flt")] = (i % 5) + 0.5;
        o[keyOf(len, "big")] = BigInt(i % 4) * 4294967296n;
        o[keyOf(len, "flag")] = i % 3 === 0;
        o[keyOf(len, "when")] = new Date(1.6e12 + (i % 6) * 1000);
        o[keyOf(len, "nil")] = null;
        o[keyOf(len, "txt")] = "t" + (i % 7);
        o[keyOf(len, "sub")] = { [keyOf(41 - len, "inner")]: i % 2 };
        o.tail = i;
        docs.push(o);
    }
    c.insertMany(docs);
    const ids = (r) => r.map((d) => d._id).sort((a, b) => a - b).join(",");
    let bad = 0;
    for (let len = 1; len <= 40; len++) {
        const sameLen = docs.filter((d) => d._id % 40 === len - 1);
        const cases = [
            [{ [keyOf(len, "num")]: 4 }, (d) => d[keyOf(len, "num")] === 4],
            [{ [keyOf(len, "flt")]: { $gt: 2 } }, (d) => d[keyOf(len, "flt")] > 2],
            [{ [keyOf(len, "big")]: 8589934592n }, (d) => d[keyOf(len, "big")] === 8589934592n],
            [{ [keyOf(len, "flag")]: true }, (d) => d[keyOf(len, "flag")] === true],
            [{ [keyOf(len, "when")]: { $gte: new Date(1.6e12 + 3000) } }, (d) => d[keyOf(len, "when")] >= new Date(1.6e12 + 3000)],
            [{ [keyOf(len, "nil")]: { $exists: true } }, (d) => keyOf(len, "nil") in d],
            [{ [keyOf(len, "txt")]: "t3", tail: { $lt: 100 } }, (d) => d[keyOf(len, "txt")] === "t3" && d.tail < 100],
            [{ [keyOf(len, "sub") + "." + keyOf(41 - len, "inner")]: 1 }, (d) => d[keyOf(len, "sub")][keyOf(41 - len, "inner")] === 1],
        ];
        for (const [f, pred] of cases) {
            const got = ids(c.find(f)), want = ids(sameLen.filter(pred));
            if (got !== want) { bad++; if (bad < 5) print("FAIL: len " + len + " " + JSON.stringify(Object.keys(f)) + ": got " + got + ", want " + want); }
        }
        const back = c.find({ _id: len - 1 })[0];
        if (back.tail !== len - 1 || Number(back[keyOf(len, "big")]) !== Number(docs[len - 1][keyOf(len, "big")])) bad++;
    }
    eq(bad, 0, "find hit sets");
    eq(c.countDocuments({ tail: { $gte: 60 } }), 60, "count over the trailing field");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_docdb_bson_iter_key_scan_oracle: " + checks + " checks passed");
__kitS.end();
