// covers: dyna:docdb.Database.listCollections
// listCollections answers the existing collections' names in creation order (a dropped and re-created one moves to
// the end) as a fresh array. Oracle: a JS list replaying 300 seeded steps of createCollection, first writes, writes
// that create nothing (no-match updates, deletes, reads, explain) and drops across 8 names; after a persist/load round
// trip the order is the same.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { makeTempDir, removeAll } from "dyna:file";
import { rng, J } from "../lib/docdb_model.js";

const t = suite("docdb.Database.listCollections");

t.test("300 seeded steps against a JS list, and the order survives persist/load", async (a) => {
    const r = rng(0x1c), db = new Database("lc"), names = ["n0", "n1", "n2", "n3", "n4", "n5", "n6", "n7"];
    let want = [];
    for (let step = 0; step < 300; step++) {
        const name = names[Math.floor(r() * names.length)], c = db.collection(name), op = Math.floor(r() * 6);
        if (op === 0) { if (!want.includes(name)) { db.createCollection(name); want.push(name); } }
        else if (op === 1) { c.insertOne({ step }); if (!want.includes(name)) want.push(name); }
        else if (op === 2) { c.updateOne({ none: 1 }, { $set: { x: 1 } }); c.replaceOne({ none: 1 }, { x: 1 }); c.findOneAndUpdate({ none: 1 }, { $set: { x: 1 } }); }
        else if (op === 3) { c.deleteMany({ none: 1 }); c.find({}); c.countDocuments({}); c.explain({ a: 1 }); c.findOneAndDelete({ none: 1 }); }
        else { a.eq(db.dropCollection(name), want.includes(name), "drop " + name); want = want.filter((n) => n !== name); }
        if (!a.eq(J(db.listCollections()), J(want), "step " + step + " op " + op + " " + name)) return;
    }
    const got = db.listCollections();
    got.push("zz");
    a.eq(J(db.listCollections()), J(want), "a fresh array each call");
    const dir = makeTempDir("docdb-lc");
    try {
        await db.persist(dir + "/s.snap");
        const back = await Database.load(dir + "/s.snap");
        a.eq(J(back.listCollections()), J(want), "order after load");
        back.close();
    } finally { removeAll(dir); }
});

await t.run();
