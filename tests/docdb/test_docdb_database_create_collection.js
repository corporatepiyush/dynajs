// covers: dyna:docdb.Database.createCollection
// createCollection makes an empty collection (it shows in listCollections with no documents and only _id_), returns
// its handle, refuses an existing name with NamespaceExists 48 (whether created by createCollection or by a write) and
// applies the collection-name rules (InvalidOptions 72). changeStreamCapacity sizes that collection's change ring: a
// watcher that falls more than that many events behind is HistoryLost 286, a larger capacity keeps them, and over 2^24
// is BadValue 2. The options survive a persist/load round trip (createCollection on the loaded name is still 48 and
// the ring capacity still applies). Oracle: event counts chosen in JS around the capacity.
import { suite } from "../lib/kit.js";
import { Database, DocDBError, Collection } from "dyna:docdb";
import { makeTempDir, removeAll } from "dyna:file";

const t = suite("docdb.Database.createCollection");
const code = (a, fn, want, msg) => { const e = a.throws(fn, DocDBError, msg); a.eq(e && e.code, want, msg); };

t.test("empty collection, handle, NamespaceExists, name rules", (a) => {
    const db = new Database("cc");
    const c = db.createCollection("a");
    a.ok(c instanceof Collection);
    a.eq(c.name, "a");
    a.deep(db.listCollections(), ["a"]);
    a.eq(c.estimatedDocumentCount(), 0);
    a.deep(c.listIndexes().map((s) => s.name), ["_id_"]);
    code(a, () => db.createCollection("a"), 48, "created by createCollection");
    db.collection("b").insertOne({ x: 1 });
    code(a, () => db.createCollection("b"), 48, "created by a write");
    a.eq(db.collection("b").estimatedDocumentCount(), 1, "the refused call left the data");
    code(a, () => db.createCollection("system.x"), 72);
    code(a, () => db.createCollection(""), 72);
    code(a, () => db.createCollection("a$"), 72);
    a.throws(() => db.createCollection(7), TypeError);
    code(a, () => db.createCollection("big", { changeStreamCapacity: 2 ** 24 + 1 }), 2, "capacity over 2^24");
    a.deep(db.listCollections(), ["a", "b"], "refused calls created nothing");
    a.ok(db.dropCollection("a"));
    a.eq(db.createCollection("a").name, "a", "a dropped name can be created again");
});

async function lagged(db, name, writes) {
    const c = db.collection(name), w = c.watch();
    for (let i = 0; i < writes; i++) c.insertOne({ i });
    try { let n = 0; for (let i = 0; i < writes; i++) { const r = await w.next(); if (!r.done) n++; } return n; }
    catch (e) { return e.code; }
    finally { await w.return(); }
}

t.test("changeStreamCapacity bounds the collection's ring and survives persist/load", async (a) => {
    const db = new Database("cc2");
    db.createCollection("small", { changeStreamCapacity: 8 });
    db.createCollection("big", { changeStreamCapacity: 64 });
    a.eq(await lagged(db, "small", 8), 8, "8 events fit a ring of 8");
    a.eq(await lagged(db, "small", 9), 286, "9 do not");
    a.eq(await lagged(db, "big", 40), 40, "a larger ring keeps them");
    const dir = makeTempDir("docdb-cc");
    try {
        await db.persist(dir + "/s.snap");
        const back = await Database.load(dir + "/s.snap");
        code(a, () => back.createCollection("small"), 48, "the loaded collection exists");
        a.eq(await lagged(back, "small", 9), 286, "the loaded ring keeps capacity 8");
        a.eq(await lagged(back, "big", 40), 40);
        back.close();
    } finally { removeAll(dir); }
});

await t.run();
