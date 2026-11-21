// covers: dyna:docdb.Collection.estimatedDocumentCount
// estimatedDocumentCount is the collection's live document count (not its slot count: deleted slots are reused and do
// not count). Oracle: a JS counter over 400 seeded inserts, insertMany batches, deletes, upserts and replaces; after
// each step the count equals the counter and countDocuments({}). Also pinned: a missing or dropped collection answers
// 0 without being created, and a duplicate-key failure in insertMany counts only the documents it applied.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { rng } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.estimatedDocumentCount");

t.test("400 seeded writes against a JS counter", (a) => {
    const r = rng(0xe57), db = new Database("est"), c = db.collection("c");
    const live = new Set();
    let next = 1;
    for (let step = 0; step < 400; step++) {
        const op = Math.floor(r() * 5);
        if (op === 0) { c.insertOne({ _id: next }); live.add(next++); }
        else if (op === 1) {
            const k = 1 + Math.floor(r() * 20), docs = [];
            for (let i = 0; i < k; i++) { docs.push({ _id: next }); live.add(next++); }
            c.insertMany(docs);
        } else if (op === 2) {
            const lo = Math.floor(r() * next), hi = lo + Math.floor(r() * 15);
            c.deleteMany({ _id: { $gte: lo, $lt: hi } });
            for (const id of [...live]) if (id >= lo && id < hi) live.delete(id);
        } else if (op === 3) {
            const id = Math.floor(r() * next * 1.2);
            c.updateOne({ _id: id }, { $set: { u: step } }, { upsert: true });
            live.add(id); next = Math.max(next, id + 1);
        } else {
            const id = Math.floor(r() * next);
            c.replaceOne({ _id: id }, { v: step });
        }
        if (!a.eq(c.estimatedDocumentCount(), live.size, "step " + step + " op " + op)) return;
        if (!a.eq(c.countDocuments({}), live.size, "countDocuments step " + step)) return;
    }
});

t.test("missing, dropped and partially failed", (a) => {
    const db = new Database("est2"), c = db.collection("c");
    a.eq(db.collection("none").estimatedDocumentCount(), 0);
    a.deep(db.listCollections(), []);
    c.insertMany([{ _id: 1 }, { _id: 2 }]);
    const e = a.throws(() => c.insertMany([{ _id: 3 }, { _id: 1 }, { _id: 4 }]), DocDBError);
    a.eq(e && e.code, 11000);
    a.eq(c.estimatedDocumentCount(), 3, "ordered insertMany applied the one before the duplicate");
    a.ok(c.drop());
    a.eq(c.estimatedDocumentCount(), 0, "dropped");
    a.deep(db.listCollections(), []);
});

await t.run();
