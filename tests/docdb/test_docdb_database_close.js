// covers: dyna:docdb.Database.close
// close() frees the database: closed turns true, every later call on it or on any of its collection handles is a
// TypeError ("closed native resource"), a second close (or dispose) is a no-op, and a change stream on it rejects its
// next read. close is refused with a TypeError while a persist on it is in flight, and the database is untouched (its
// data still answers and the persist completes); after the persist settles close succeeds. Oracle: the call table
// below, every entry of which must throw after close, and JS-held documents before.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { makeTempDir, removeAll } from "dyna:file";
import { J } from "../lib/docdb_model.js";

const t = suite("docdb.Database.close");

t.test("closed turns true and every later call throws", (a) => {
    const db = new Database("cl"), c = db.collection("c");
    c.insertMany([{ _id: 1, a: 1 }, { _id: 2, a: 2 }]);
    c.createIndex({ a: 1 });
    a.eq(db.closed, false);
    a.eq(db.close(), undefined);
    a.eq(db.closed, true);
    const calls = {
        name: () => db.name, collection: () => db.collection("c"), createCollection: () => db.createCollection("n"),
        listCollections: () => db.listCollections(), dropCollection: () => db.dropCollection("c"), stats: () => db.stats(),
        find: () => c.find(), insertOne: () => c.insertOne({}), updateOne: () => c.updateOne({}, { $set: { a: 1 } }),
        deleteMany: () => c.deleteMany({}), count: () => c.estimatedDocumentCount(), listIndexes: () => c.listIndexes(),
        explain: () => c.explain({}), drop: () => c.drop(), watch: () => db.watch(),
    };
    for (const [k, fn] of Object.entries(calls)) a.throws(fn, TypeError, k);
    a.eq(db.close(), undefined, "a second close is a no-op");
    db.dispose();
    db[Symbol.dispose]();
    a.eq(db.closed, true);
});

t.test("dispose closes; a change stream rejects after close", async (a) => {
    const db = new Database("cl2"), c = db.collection("c");
    const w = c.watch();
    c.insertOne({ _id: 1 });
    const first = await w.next();
    a.eq(first.value && first.value.operationType, "insert");
    db[Symbol.dispose]();
    a.eq(db.closed, true);
    await a.rejects(() => w.next(), TypeError, "the stream's database is closed");
});

t.test("close is refused while a persist is in flight", async (a) => {
    const db = new Database("cl3"), c = db.collection("c");
    const docs = Array.from({ length: 40000 }, (_, i) => ({ _id: i, s: "x".repeat(40) }));
    c.insertMany(docs);
    const dir = makeTempDir("docdb-close");
    try {
        const p = db.persist(dir + "/s.snap");
        a.throws(() => db.close(), TypeError, "refused in flight");
        a.eq(db.closed, false);
        a.eq(c.estimatedDocumentCount(), docs.length, "the database still answers");
        const res = await p;
        a.eq(res.documents, docs.length, "the persist completed");
        db.close();
        a.eq(db.closed, true, "close succeeds once it settled");
        const back = await Database.load(dir + "/s.snap");
        a.eq(J(back.collection("c").find({ _id: { $lt: 3 } })), J(docs.slice(0, 3)));
        back.close();
    } finally { removeAll(dir); }
});

await t.run();
