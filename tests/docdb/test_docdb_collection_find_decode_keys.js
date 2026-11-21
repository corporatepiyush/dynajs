// covers: dyna:docdb.Collection.find
// A result batch (find, aggregate) decodes through one decoder that remembers, per nesting depth and key position, the
// last key bytes it saw and their atom (keys up to 23 bytes, the first 16 keys, depths 0-7), so a run of same-shape
// documents skips the atom lookup. The cache may only ever answer what a fresh lookup would: keys that differ only in
// their last byte, keys of the same length, a key that moves position, keys past the 16th or longer than 23 bytes,
// non-ASCII keys, keys below depth 8, and integer-like keys must all come back exactly, in insertion order.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";

const t = suite("docdb.Collection.find decode key cache");
const db = new Database("dk");

function shape(v) {
    if (Array.isArray(v)) return v.map(shape);
    if (v && typeof v === "object") return Object.keys(v).map((k) => [k, shape(v[k])]);
    return v;
}

function deepNest(d, leaf) {
    let o = { leaf };
    for (let i = 0; i < d; i++) o = { [`n${i % 3}`]: o, [`m${d - i}`]: i };
    return o;
}

const LONG = "k".repeat(23);
const docs = [];
for (let i = 0; i < 400; i++) {
    const d = { _id: i };
    switch (i % 8) {
        case 0: Object.assign(d, { ab: 1, ac: 2, ad: 3 }); break;
        case 1: Object.assign(d, { ab: 1, ad: 2, ac: 3 }); break;
        case 2: Object.assign(d, { aa: 1, ab: 2, ac: 3 }); break;
        case 3: Object.assign(d, { [LONG]: 1, [`${LONG}x`]: 2, [`${LONG}y`]: 3 }); break;
        case 4: for (let k = 0; k < 20; k++) d[`f${(k + i) % 20}`] = k; break;
        case 5: Object.assign(d, { "é": 1, "e": 2, "ü": 3, "日本": 4 }); break;
        case 6: Object.assign(d, { nest: deepNest(10, i), x: { y: { z: i } } }); break;
        case 7: Object.assign(d, { items: [{ sku: "a", q: 1 }, { sku: "b", qty: 2 }, { q: 3, sku: "c" }, { "0": 1, "1": 2 }] }); break;
    }
    docs.push(d);
}
const c = db.collection("c");
c.insertMany(docs);

t.test("find returns every key, value and key order of a mixed-shape batch", (a) => {
    const got = c.find({}, { sort: { _id: 1 } });
    a.eq(got.length, docs.length, "count");
    let bad = 0;
    for (let i = 0; i < docs.length; i++)
        if (JSON.stringify(shape(got[i])) !== JSON.stringify(shape(docs[i])) && bad++ < 3)
            a.deep(shape(got[i]), shape(docs[i]), `doc ${i}`);
    a.eq(bad, 0, "documents decoded differently from what was inserted");
});

t.test("a projection batch alternating two key sets of equal length keeps each", (a) => {
    const p = db.collection("p");
    p.insertMany(Array.from({ length: 100 }, (_, i) => (i % 2 ? { _id: i, xa: i, xb: i } : { _id: i, xb: i, xc: i })));
    const got = p.find({}, { sort: { _id: 1 }, projection: { _id: 0 } });
    a.deep(got.slice(0, 4).map((d) => Object.keys(d)), [["xb", "xc"], ["xa", "xb"], ["xb", "xc"], ["xa", "xb"]]);
    a.eq(got.filter((d, i) => Object.keys(d).join() !== (i % 2 ? "xa,xb" : "xb,xc")).length, 0, "every document keeps its own keys");
});

await t.run();
db.close();
