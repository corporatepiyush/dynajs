// covers: dyna:docdb.Database.dropCollection
// dropCollection(name) removes the collection with its documents and indexes and answers whether it existed. Oracle:
// a JS map name -> documents replaying 200 seeded inserts and drops over 6 names; after each step every collection's
// sorted contents, listCollections and stats().indexes equal the map's. Also pinned: names are checked like
// collection() (InvalidOptions 72, TypeError for a non-string) before anything is dropped, an open handle sees the
// collection empty afterwards and re-creates it on its next write, and a database watcher gets a "drop" event and
// keeps running.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { rng, J } from "../lib/docdb_model.js";

const t = suite("docdb.Database.dropCollection");

t.test("200 seeded inserts and drops against a JS map", (a) => {
    const r = rng(0xdc), db = new Database("dc"), names = ["a", "b", "c", "d", "e", "f"], live = new Map();
    let next = 1;
    for (let step = 0; step < 200; step++) {
        const name = names[Math.floor(r() * names.length)];
        if (r() < 0.65) {
            const docs = Array.from({ length: 1 + Math.floor(r() * 6) }, () => ({ _id: next++, s: step }));
            const c = db.collection(name);
            if (!live.has(name)) { live.set(name, []); c.createIndex({ s: 1 }); }
            c.insertMany(docs);
            live.get(name).push(...docs);
        } else {
            if (!a.eq(db.dropCollection(name), live.has(name), "drop " + name + " step " + step)) return;
            live.delete(name);
        }
        for (const n of names) if (!a.eq(J(db.collection(n).find({}, { sort: { _id: 1 } })), J(live.get(n) || []), n + " step " + step)) return;
        a.eq(J(db.listCollections()), J([...live.keys()]));
        a.eq(db.stats().indexes, live.size, "one secondary index per live collection");
    }
});

t.test("name checks, open handles, watchers", async (a) => {
    const db = new Database("dc2"), c = db.collection("c");
    c.insertMany([{ _id: 1 }, { _id: 2 }]);
    for (const bad of ["", "system.c", "c$"]) { const e = a.throws(() => db.dropCollection(bad), DocDBError, bad); a.eq(e && e.code, 72); }
    a.throws(() => db.dropCollection(), TypeError);
    a.throws(() => db.dropCollection(1), TypeError);
    a.eq(c.estimatedDocumentCount(), 2, "refused calls dropped nothing");
    const w = db.watch();
    a.eq(db.dropCollection("c"), true);
    a.eq(db.dropCollection("c"), false);
    a.deep(c.find(), [], "the open handle sees it empty");
    c.insertOne({ _id: 3 });
    a.deep(db.listCollections(), ["c"], "and re-creates it on a write");
    const e1 = await w.next(), e2 = await w.next();
    a.eq(e1.value && e1.value.operationType, "drop");
    a.eq(e2.value && e2.value.operationType, "insert", "the database watcher keeps running");
    await w.return();
});

await t.run();
