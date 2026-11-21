// covers: dyna:docdb.Database.watch
// A database-wide change stream (the MongoDB manual's db.watch()): events of every collection, including ones created
// after watch(), in write order; a collection drop is a "drop" event but does not invalidate the stream; ns.coll
// filters through the pipeline; a resume token replays across collections; Database.load replacing collections
// reports them as drops.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { makeTempDir, removeAll } from "dyna:file";

const t = suite("docdb.Database.watch");
const tag = (e) => e && `${e.operationType}:${e.ns ? e.ns.coll : "-"}:${e.documentKey ? e.documentKey._id : "-"}`;
const drain = (cs) => {
    const out = [];
    for (let e; (e = cs.tryNext()) !== null; ) out.push(tag(e));
    return out.join(" ");
};

t.test("every collection, in write order, collections created later included; drop does not invalidate", async (a) => {
    const db = new Database("dw1");
    db.collection("a").insertOne({ _id: 0 });
    const cs = db.watch();
    db.collection("a").insertOne({ _id: 1 });
    db.collection("b").insertOne({ _id: 2 });
    db.collection("a").updateOne({ _id: 1 }, { $set: { x: 1 } });
    db.dropCollection("b");
    db.collection("c").insertOne({ _id: 3 });
    a.eq(drain(cs), "insert:a:1 insert:b:2 update:a:1 drop:b:- insert:c:3");
    a.eq(cs.closed, false);
    cs.close();
    db.close();
});

t.test("a $match on ns.coll narrows it; a resume token replays across collections", async (a) => {
    const db = new Database("dw2");
    const only = db.watch([{ $match: { "ns.coll": "keep" } }]);
    const all = db.watch();
    db.collection("keep").insertOne({ _id: 1 });
    db.collection("skip").insertOne({ _id: 2 });
    db.collection("keep").deleteOne({ _id: 1 });
    a.eq(drain(only), "insert:keep:1 delete:keep:1");
    all.tryNext();
    const tok = all.resumeToken;
    const again = db.watch([], { resumeAfter: tok });
    a.eq(drain(again), "insert:skip:2 delete:keep:1");
    only.close();
    all.close();
    again.close();
    db.close();
});

t.test("Database.load replacing a collection reports it dropped", async (a) => {
    const db = new Database("dw3");
    db.collection("x").insertOne({ _id: 1 });
    const dir = makeTempDir("docdb-dw3");
    const path = dir.toString() + "/x.dds";
    await db.persist(path, { fsync: false });
    const cs = db.watch();
    db.collection("x").insertOne({ _id: 2 });
    await db.load(path, { merge: true });
    a.eq(drain(cs), "insert:x:2 drop:x:-");
    a.eq(db.collection("x").countDocuments(), 1);
    cs.close();
    db.close();
    removeAll(dir);
});

await t.run();
