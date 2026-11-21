// covers: dyna:docdb.Collection.dropIndex
// dropIndex removes an index by name or by its exact key pattern (field order and direction count); the _id_ index is
// InvalidOptions 72, an unknown name or pattern is IndexNotFound 27. Oracle: after each drop, listIndexes holds the
// remaining names (JS list), stats().indexEntries is the documents' remaining entry count computed in JS, and every
// filter answers what an unindexed twin answers (the planner no longer reaches the dropped index; explain names it
// nowhere). dropIndex on a collection that does not exist threw an uninitialised value (typeof "unknown", no name,
// no code) because the binding returned an exception with none pending; it is now NamespaceNotFound 26.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { rng, genDocs, filters, clone, J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.dropIndex");
const names = (c) => c.listIndexes().map((s) => s.name);
const code = (a, fn, want, msg) => { const e = a.throws(fn, DocDBError, msg); a.eq(e && e.code, want, msg); };

t.test("by name and by key pattern, against JS-computed entries and an unindexed twin", (a) => {
    const r = rng(0xd1), db = new Database("di"), ix = db.collection("ix"), plain = db.collection("plain");
    const docs = genDocs(r, 300);
    ix.insertMany(docs.map(clone)); plain.insertMany(docs.map(clone));
    ix.createIndex({ a: 1 }); ix.createIndex({ b: 1, n: -1 }); ix.createIndex({ tags: 1 }); ix.createIndex({ n: 1 }, { name: "nn" });
    const entries = { a_1: docs.length, "b_1_n_-1": docs.length, tags_1: docs.reduce((s, d) => s + (d.tags ? new Set(d.tags).size : 1), 0), nn: docs.length };
    const total = () => Object.values(entries).reduce((s, x) => s + x, 0);
    a.eq(db.stats().indexEntries, total(), "entries before");
    const steps = [["a_1", "a_1"], [{ b: 1, n: -1 }, "b_1_n_-1"], ["nn", "nn"], [{ tags: 1 }, "tags_1"]];
    for (const [arg, name] of steps) {
        a.eq(ix.dropIndex(arg), undefined, "drop " + J(arg));
        delete entries[name];
        a.deep(names(ix), ["_id_", ...Object.keys(entries)], "listIndexes after " + J(arg));
        a.eq(db.stats().indexEntries, total(), "entries after " + J(arg));
        for (const [f] of filters(r)) {
            a.eq(J(ix.find(f, { sort: { _id: 1 } })), J(plain.find(f, { sort: { _id: 1 } })), J(f));
            a.ok(!J(ix.explain(f)).includes('"' + name + '"'), "explain no longer names " + name);
        }
    }
});

t.test("refusals: _id_, unknown names, inexact patterns, missing collection", (a) => {
    const db = new Database("di2"), c = db.collection("c");
    c.insertOne({ _id: 1, a: 1, b: 2 });
    c.createIndex({ a: 1, b: -1 });
    code(a, () => c.dropIndex("_id_"), 72, "_id_");
    code(a, () => c.dropIndex({ _id: 1 }), 27, "the _id pattern is not an index here");
    code(a, () => c.dropIndex("nope"), 27, "unknown name");
    code(a, () => c.dropIndex({ b: -1, a: 1 }), 27, "field order counts");
    code(a, () => c.dropIndex({ a: 1, b: 1 }), 27, "direction counts");
    code(a, () => c.dropIndex({ a: 1 }), 27, "a prefix is not the index");
    a.deep(names(c), ["_id_", "a_1_b_-1"], "nothing was dropped");
    a.throws(() => c.dropIndex(), TypeError);
    let caught;
    try { db.collection("none").dropIndex("a_1"); } catch (e) { caught = e; }
    a.eq(typeof caught, "object", "a real error object");
    a.ok(caught instanceof DocDBError, "DocDBError");
    a.eq(caught && caught.code, 26, "NamespaceNotFound");
    a.deep(db.listCollections(), ["c"], "and nothing was created");
});

await t.run();
