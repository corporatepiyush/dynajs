// covers: dyna:docdb.Collection.drop
// drop removes the collection with its documents and indexes and answers true, or false when it did not exist. Oracle:
// a JS map of collection name -> live document count over 200 seeded inserts and drops across 5 collections: after
// each step listCollections (in creation order, a re-created collection last) and every count, plus stats()
// collections/documents/indexes/indexEntries, equal the map's. Also pinned: the handle stays usable (a later insert
// re-creates the collection with only _id_), another handle on the same name sees the drop, and a watcher gets a
// "drop" event.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { rng, J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.drop");

t.test("200 seeded inserts and drops against a JS map", (a) => {
    const r = rng(0xd20), db = new Database("dr");
    const names = ["c0", "c1", "c2", "c3", "c4"], live = new Map();
    let next = 1;
    for (let step = 0; step < 200; step++) {
        const name = names[Math.floor(r() * names.length)], c = db.collection(name);
        if (r() < 0.7) {
            const k = 1 + Math.floor(r() * 10);
            c.insertMany(Array.from({ length: k }, () => ({ _id: next++, a: step })));
            if (!live.has(name)) { live.set(name, 0); c.createIndex({ a: 1 }); }
            live.set(name, live.get(name) + k);
        } else {
            if (!a.eq(c.drop(), live.has(name), "drop " + name + " step " + step)) return;
            live.delete(name);
        }
        if (!a.eq(J(db.listCollections()), J([...live.keys()]), "listCollections step " + step)) return;
        for (const n of names) a.eq(db.collection(n).estimatedDocumentCount(), live.get(n) || 0, n + " step " + step);
        const docs = [...live.values()].reduce((s, x) => s + x, 0);
        a.deep(db.stats(), { collections: live.size, documents: docs, dataBytes: db.stats().dataBytes, indexes: live.size, indexEntries: docs, retiredBytes: db.stats().retiredBytes, pinnedSnapshots: 0 }, "stats step " + step);
    }
});

t.test("handles stay usable, another handle sees the drop, a watcher gets a drop event", async (a) => {
    const db = new Database("dr2"), c = db.collection("c"), other = db.collection("c");
    c.createIndex({ a: 1 });
    c.insertMany([{ _id: 1, a: 1 }, { _id: 2, a: 2 }]);
    const w = db.watch();
    a.eq(c.drop(), true);
    a.eq(c.drop(), false, "already gone");
    a.deep(other.find(), [], "the other handle sees the drop");
    a.eq(other.estimatedDocumentCount(), 0);
    c.insertOne({ _id: 3, a: 3 });
    a.eq(J(other.find({ a: 3 })), J([{ _id: 3, a: 3 }]));
    a.deep(c.listIndexes().map((s) => s.name), ["_id_"], "the old index went with the collection");
    const ev = await w.next();
    a.eq(ev.value && ev.value.operationType, "drop");
    a.eq(ev.value && ev.value.ns && ev.value.ns.coll, "c");
    await w.return();
});

await t.run();
