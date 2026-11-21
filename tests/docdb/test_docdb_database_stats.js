// covers: dyna:docdb.Database.stats
// stats() sums over the collections: collections, documents (live count), dataBytes (the live documents' BSON bytes),
// indexes (secondary indexes; _id_ is not counted), indexEntries (one per document per index, one per distinct array
// element for a multikey index, none for a document a sparse or partial index skips), retiredBytes and
// pinnedSnapshots. Oracle: every number is recomputed in JS from the documents held by the test (dataBytes with
// dyna:serialize BSONEncode of the stored document as find returns it) after 250 seeded inserts, updates, deletes and
// index creates/drops over 3 collections. A large aggregate pins a snapshot while it runs (pinnedSnapshots 1, and
// bytes it still reads become retiredBytes when rewritten: arena chunks are 1 MiB and only a fully dead chunk other
// than the current one is retired, so the case writes 40000 documents, about 3 MiB), and both return to 0 once it
// settles.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { BSONEncode } from "dyna:serialize";
import { rng, J } from "../lib/docdb_model.js";

const t = suite("docdb.Database.stats");

function entries(docs, ix) {
    let n = 0;
    for (const d of docs) {
        if (ix.partial && !(typeof d.v === "number" && d.v > 50)) continue;
        if (ix.sparse && !(ix.f in d)) continue;
        n += Array.isArray(d[ix.f]) ? Math.max(1, new Set(d[ix.f]).size) : 1;
    }
    return n;
}

t.test("250 seeded writes and index changes against JS-computed sizes", (a) => {
    const r = rng(0x57a7), db = new Database("st"), names = ["x", "y", "z"], ixs = new Map();
    const IX = [{ f: "v" }, { f: "tags" }, { f: "s", sparse: true }, { f: "w", partial: true }];
    let next = 1;
    for (let step = 0; step < 250; step++) {
        const name = names[Math.floor(r() * 3)], c = db.collection(name), op = Math.floor(r() * 6);
        if (!ixs.has(name)) ixs.set(name, new Map());
        const mine = ixs.get(name);
        if (op <= 1) {
            c.insertMany(Array.from({ length: 1 + Math.floor(r() * 8) }, () => {
                const d = { _id: next++, v: Math.floor(r() * 100) };
                if (r() < 0.5) d.s = "s" + Math.floor(r() * 9);
                if (r() < 0.5) d.tags = Array.from({ length: Math.floor(r() * 4) }, () => Math.floor(r() * 5));
                if (r() < 0.5) d.w = r();
                return d;
            }));
        } else if (op === 2) c.updateMany({ v: { $lt: Math.floor(r() * 100) } }, { $set: { s: "u" + step }, $inc: { v: 7 } });
        else if (op === 3) c.deleteMany({ v: { $gt: Math.floor(r() * 150) } });
        else if (op === 4) {
            const ix = IX[Math.floor(r() * IX.length)], name2 = ix.f + "_1";
            if (!mine.has(name2)) {
                const opts = ix.sparse ? { sparse: true } : ix.partial ? { partialFilterExpression: { v: { $gt: 50 } } } : {};
                c.createIndex({ [ix.f]: 1 }, opts); mine.set(name2, ix);
            }
        } else if (mine.size) {
            const k = [...mine.keys()][Math.floor(r() * mine.size)];
            c.dropIndex(k); mine.delete(k);
        }
        const all = [...db.listCollections()].map((n) => [n, db.collection(n).find()]);
        const want = { collections: all.length, documents: 0, dataBytes: 0, indexes: 0, indexEntries: 0, retiredBytes: 0, pinnedSnapshots: 0 };
        for (const [n, docs] of all) {
            want.documents += docs.length;
            for (const d of docs) want.dataBytes += BSONEncode(d).length;
            for (const ix of (ixs.get(n) || new Map()).values()) { want.indexes++; want.indexEntries += entries(docs, ix); }
        }
        if (!a.eq(J(db.stats()), J(want), "step " + step + " op " + op)) return;
    }
});

t.test("a running aggregate pins a snapshot; both counters return to 0 when it settles", async (a) => {
    const db = new Database("st2"), c = db.collection("c");
    c.insertMany(Array.from({ length: 40000 }, (_, i) => ({ _id: i, s: "x".repeat(40) })));
    const p = c.aggregate([{ $match: { s: { $exists: true } } }, { $count: "n" }]);
    const mid = db.stats();
    a.eq(mid.pinnedSnapshots, 1, "pinned while it runs");
    c.updateMany({}, { $set: { s: "y".repeat(80) } });
    const after = db.stats();
    a.ok(after.retiredBytes > 0, "rewritten bytes the aggregate still reads are retired, not freed");
    a.eq(J(await p), J([{ n: 40000 }]), "the aggregate read its snapshot");
    const end = db.stats();
    a.eq(end.pinnedSnapshots, 0);
    a.eq(end.retiredBytes, 0, "retired bytes are freed when the pin is released");
});

await t.run();
