// covers: blackbox dyna:docdb.Database dyna:docdb.Collection dyna:docdb.ChangeStream dyna:docdb.DocDBError -- the module end to end through its public surface
// Parametric black-box contract test for dyna:docdb, generated from the dyna:docdb block of dynajs.d.ts. Engine sources not consulted.
// Rows are [label, input..., expected]; each failure names its row. Expectations come from the d.ts text and MongoDB's documented semantics it pins.
import { Database, Collection, DocDBError, ObjectId, ChangeStream } from "dyna:docdb";
import { makeTempDir, removeAll, readBytes, writeFile, Path } from "dyna:file";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertDeepEq(a, b, msg) {
    n++;
    if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " -- got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|");
}
function codeOf(fn) {
    try {
        fn();
    } catch (e) {
        return e instanceof DocDBError ? e.code : e instanceof TypeError ? "TypeError" : "other:" + e;
    }
    return "no throw";
}
async function codeOfAsync(p) {
    try {
        await p;
    } catch (e) {
        return e instanceof DocDBError ? e.code : "other:" + e;
    }
    return "no throw";
}
const ids = (docs) => docs.map((d) => d._id);

const db = new Database("bb");
const c = db.collection("people");
assert(c instanceof Collection && c.name === "people", "db.collection returns a Collection");
c.insertMany([
    { _id: 1, name: "ann", age: 31, tags: ["a", "b"], addr: { city: "Rome" }, kids: [{ n: "x", age: 3 }, { n: "y", age: 9 }] },
    { _id: 2, name: "bob", age: 25, tags: ["b"], addr: { city: "Oslo" }, kids: [] },
    { _id: 3, name: "cid", age: "40", tags: [], addr: { city: "Rome" } },
    { _id: 4, name: "dee", age: 52.5, tags: ["c", "a"], kids: [{ n: "z", age: 12 }] },
    { _id: 5, name: "eve", age: null },
]);

{
    const FIND = [
        ["$eq implicit", { name: "bob" }, [2]],
        ["$gt never crosses types", { age: { $gt: 30 } }, [1, 4]],
        ["$gte string compares strings only", { age: { $gte: "3" } }, [3]],
        ["$ne includes missing", { "addr.city": { $ne: "Rome" } }, [2, 4, 5]],
        ["$in over array field", { tags: { $in: ["c"] } }, [4]],
        ["$nin", { tags: { $nin: ["a", "b"] } }, [3, 5]],
        ["array traversal equality", { tags: "a" }, [1, 4]],
        ["numeric path component", { "tags.0": "c" }, [4]],
        ["path through array of documents", { "kids.age": { $gt: 10 } }, [4]],
        ["$elemMatch", { kids: { $elemMatch: { n: "y", age: { $lt: 10 } } } }, [1]],
        ["$size", { tags: { $size: 2 } }, [1, 4]],
        ["$all", { tags: { $all: ["a", "b"] } }, [1]],
        ["$exists false", { addr: { $exists: false } }, [4, 5]],
        ["null matches null and missing", { age: null }, [5]],
        ["$type string", { age: { $type: "string" } }, [3]],
        ["$mod", { age: { $mod: [5, 0] } }, [2]],
        ["$regex", { name: { $regex: "^[ab]" } }, [1, 2]],
        ["$regex $options i", { name: { $regex: "^EVE$", $options: "i" } }, [5]],
        ["$or", { $or: [{ _id: 1 }, { name: "dee" }] }, [1, 4]],
        ["$nor", { $nor: [{ _id: 1 }, { _id: 2 }, { _id: 3 }] }, [4, 5]],
        ["$not", { age: { $not: { $gt: 30 } } }, [2, 3, 5]],
        ["$and", { $and: [{ "addr.city": "Rome" }, { age: { $type: "number" } }] }, [1]],
    ];
    for (const [label, filter, want] of FIND) assertDeepEq(ids(c.find(filter)), want, "find: " + label);

    const OPTS = [
        ["sort desc", {}, { sort: { _id: -1 }, limit: 2 }, [5, 4]],
        ["skip+limit", {}, { skip: 1, limit: 2 }, [2, 3]],
        ["limit 0 means none", {}, { limit: 0 }, [1, 2, 3, 4, 5]],
    ];
    for (const [label, filter, opts, want] of OPTS) assertDeepEq(ids(c.find(filter, opts)), want, "find opts: " + label);
    assertDeepEq(c.findOne({ _id: 2 }, { projection: { name: 1 } }), { _id: 2, name: "bob" }, "projection keeps _id");
    assert(c.findOne({ _id: 99 }) === null, "findOne miss is null");
    assert(c.countDocuments({ "addr.city": "Rome" }) === 2, "countDocuments");
    assert(c.countDocuments({}, { skip: 1, limit: 2 }) === 2, "countDocuments skip/limit");
    assert(c.estimatedDocumentCount() === 5, "estimatedDocumentCount");
    assertDeepEq(c.distinct("tags"), ["a", "b", "c"], "distinct counts elements, first-seen order");
}

{
    const u = db.collection("upd");
    const UPD = [
        ["$set nested", { _id: 1, a: 1 }, { $set: { "b.c": 2 } }, { _id: 1, a: 1, b: { c: 2 } }],
        ["$unset", { _id: 1, a: 1, b: 2 }, { $unset: { a: "" } }, { _id: 1, b: 2 }],
        ["$inc", { _id: 1, a: 1 }, { $inc: { a: 2, z: 1 } }, { _id: 1, a: 3, z: 1 }],
        ["$mul", { _id: 1, a: 3 }, { $mul: { a: 4 } }, { _id: 1, a: 12 }],
        ["$min $max", { _id: 1, a: 5, b: 5 }, { $min: { a: 3 }, $max: { b: 2 } }, { _id: 1, a: 3, b: 5 }],
        ["$rename", { _id: 1, a: 1 }, { $rename: { a: "b" } }, { _id: 1, b: 1 }],
        ["$push $each $sort $slice", { _id: 1, a: [3] }, { $push: { a: { $each: [1, 5, 2], $sort: 1, $slice: 3 } } }, { _id: 1, a: [1, 2, 3] }],
        ["$push $position", { _id: 1, a: [1, 2] }, { $push: { a: { $each: [9], $position: 1 } } }, { _id: 1, a: [1, 9, 2] }],
        ["$addToSet $each", { _id: 1, a: [1] }, { $addToSet: { a: { $each: [1, 2, 2] } } }, { _id: 1, a: [1, 2] }],
        ["$pull condition", { _id: 1, a: [1, 5, 9] }, { $pull: { a: { $gt: 4 } } }, { _id: 1, a: [1] }],
        ["$pullAll", { _id: 1, a: [1, 2, 1, 3] }, { $pullAll: { a: [1, 3] } }, { _id: 1, a: [2] }],
        ["$pop last", { _id: 1, a: [1, 2, 3] }, { $pop: { a: 1 } }, { _id: 1, a: [1, 2] }],
        ["$[] all elements", { _id: 1, a: [1, 2] }, { $inc: { "a.$[]": 10 } }, { _id: 1, a: [11, 12] }],
        ["added fields in path order", { _id: 1 }, { $set: { z: 1, b: 1 } }, { _id: 1, b: 1, z: 1 }],
    ];
    for (const [label, doc, update, want] of UPD) {
        u.deleteMany({});
        u.insertOne(doc);
        const r = u.updateOne({ _id: 1 }, update);
        assert(r.matchedCount === 1 && r.modifiedCount === 1, "update counts: " + label);
        assertDeepEq(u.findOne({ _id: 1 }), want, "update: " + label);
    }
    u.deleteMany({});
    u.insertOne({ _id: 1, a: [1, 2, 3] });
    u.updateOne({ a: 2 }, { $set: { "a.$": 20 } });
    assertDeepEq(u.findOne({}).a, [1, 20, 3], "positional $ is the matched element");

    const up = u.updateOne({ k: "x" }, { $set: { v: 1 }, $setOnInsert: { born: true } }, { upsert: true });
    assert(up.upsertedCount === 1 && up.upsertedId instanceof ObjectId, "upsert creates with ObjectId");
    const got = u.findOne({ k: "x" });
    assert(got.k === "x" && got.v === 1 && got.born === true, "upsert carries filter equality and $setOnInsert");
    u.updateOne({ k: "x" }, { $setOnInsert: { born: false } }, { upsert: true });
    assert(u.findOne({ k: "x" }).born === true, "$setOnInsert ignored on update");

    assertDeepEq(u.findOneAndUpdate({ _id: 1 }, { $set: { b: 1 } }, { returnDocument: "after" }).b, 1, "findOneAndUpdate after");
    assert(u.findOneAndReplace({ _id: 1 }, { q: 1 }).b === 1, "findOneAndReplace returns before");
    assertDeepEq(u.findOne({ _id: 1 }), { _id: 1, q: 1 }, "replace keeps _id");
    assert(u.findOneAndDelete({ _id: 1 })._id === 1 && u.findOne({ _id: 1 }) === null, "findOneAndDelete");

    const ERR = [
        ["immutable _id", () => u.replaceOne({ k: "x" }, { _id: 7 }), 66],
        ["conflicting operators", () => u.updateOne({}, { $set: { a: 1 }, $inc: { a: 1 } }), 40],
        ["$inc non-number", () => u.updateOne({}, { $inc: { k: 1 } }), 14],
        ["arrayFilters refused", () => u.updateOne({}, { $set: { "a.$[e]": 1 } }), 2],
        ["duplicate _id", () => u.insertOne({ _id: got._id }), 11000],
        ["unknown query operator", () => u.find({ a: { $nope: 1 } }), 2],
    ];
    for (const [label, fn, want] of ERR) assert(codeOf(fn) === want, "error code: " + label + " got " + codeOf(fn));
    try {
        u.insertOne({ _id: got._id });
    } catch (e) {
        assert(e.codeName === "DuplicateKey" && /E11000/.test(e.message), "DuplicateKey codeName and E11000 message");
    }

    const w = db.collection("im");
    try {
        w.insertMany([{ _id: 1 }, { _id: 1 }, { _id: 2 }], { ordered: false });
        assert(false, "insertMany duplicate throws");
    } catch (e) {
        assert(e.code === 11000 && e.writeErrors.length === 1 && e.writeErrors[0].index === 1, "unordered writeErrors names the index");
    }
    assert(w.estimatedDocumentCount() === 2, "unordered applies the rest");
    const bw = w.bulkWrite([{ insertOne: { document: { _id: 3 } } }, { updateMany: { filter: {}, update: { $set: { t: 1 } } } }, { deleteOne: { filter: { _id: 1 } } }]);
    assert(bw.insertedCount === 1 && bw.matchedCount === 3 && bw.deletedCount === 1, "bulkWrite counts");
}

{
    const ix = db.collection("ix");
    const docs = [];
    for (let i = 0; i < 2000; i++) docs.push({ _id: i, a: i % 100, b: i % 7, t: [i % 3, 10 + (i % 5)] });
    ix.insertMany(docs);
    assert(ix.createIndex({ a: 1 }) === "a_1", "default index name");
    assert(ix.createIndex({ a: 1, b: -1 }) === "a_1_b_-1", "compound default name");
    assert(ix.createIndex({ a: 1 }) === "a_1", "same spec is a no-op");
    assert([85, 86].includes(codeOf(() => ix.createIndex({ b: 1 }, { name: "a_1" }))), "same name other spec is 85/86");
    ix.createIndex({ t: 1 });
    const names = ix.listIndexes().map((s) => s.name);
    assertDeepEq(names[0], "_id_", "listIndexes _id_ first");
    const stage = (p) => {
        let s = p;
        while (s.inputStage && s.stage !== "IXSCAN") s = s.inputStage;
        return s.stage;
    };
    const PLANS = [
        ["IDHACK", { _id: 5 }, "IDHACK"],
        ["single-field index", { a: 42 }, "IXSCAN"],
        ["multikey index", { t: 11 }, "IXSCAN"],
        ["no index", { b: 3 }, "COLLSCAN"],
    ];
    for (const [label, filter, want] of PLANS) {
        const ex = ix.explain(filter);
        assert(stage(ex.queryPlanner.winningPlan) === want, "plan: " + label + " got " + JSON.stringify(ex.queryPlanner.winningPlan));
        assert(ex.executionStats.nReturned === ix.countDocuments(filter), "explain nReturned: " + label);
    }
    assert(ix.find({ a: 42 }).length === 20 && ix.find({ t: 11 }).length === 400, "indexed answers");
    ix.createIndex({ u: 1 }, { unique: true, sparse: true });
    ix.updateOne({ _id: 1 }, { $set: { u: "k" } });
    assert(codeOf(() => ix.updateOne({ _id: 2 }, { $set: { u: "k" } })) === 11000, "unique index refuses duplicate");
    ix.createIndex({ t: 1, q: 1 }, { name: "tq" });
    assert(codeOf(() => ix.insertOne({ _id: "p", t: [1, 2], q: 1 })) === "no throw", "one array per compound key is fine");
    assert(codeOf(() => ix.insertOne({ _id: "pp", t: [1, 2], q: [1, 2] })) === 171, "parallel arrays refused");
    ix.dropIndex("tq");
    assert(codeOf(() => ix.dropIndex("nope")) === 27, "IndexNotFound 27");
}

{
    const o = db.collection("orders");
    o.insertMany([
        { _id: 1, cust: "a", items: [{ sku: "x", q: 2, p: 5 }, { sku: "y", q: 1, p: 10 }] },
        { _id: 2, cust: "b", items: [{ sku: "x", q: 1, p: 5 }] },
        { _id: 3, cust: "a", items: [] },
    ]);
    db.collection("cust").insertMany([
        { _id: "a", city: "Rome" },
        { _id: "b", city: "Oslo" },
    ]);
    const AGG = [
        ["$unwind $group $sort", [{ $unwind: "$items" }, { $group: { _id: "$items.sku", units: { $sum: "$items.q" } } }, { $sort: { _id: 1 } }], [{ _id: "x", units: 3 }, { _id: "y", units: 1 }]],
        ["$group first-seen order", [{ $group: { _id: "$cust", n: { $sum: 1 } } }], [{ _id: "a", n: 2 }, { _id: "b", n: 1 }]],
        ["$count", [{ $match: { cust: "a" } }, { $count: "n" }], [{ n: 2 }]],
        ["$project expression", [{ $match: { _id: 1 } }, { $project: { _id: 0, total: { $sum: { $map: { input: "$items", in: { $multiply: ["$$this.q", "$$this.p"] } } } } } }], [{ total: 20 }]],
        ["$lookup equality", [{ $match: { _id: 2 } }, { $lookup: { from: "cust", localField: "cust", foreignField: "_id", as: "c" } }, { $project: { _id: 0, city: { $first: "$c.city" } } }], [{ city: "Oslo" }]],
        ["$sortByCount", [{ $sortByCount: "$cust" }], [{ _id: "a", count: 2 }, { _id: "b", count: 1 }]],
        ["$bucket default last", [{ $bucket: { groupBy: "$_id", boundaries: [1, 2], default: "rest" } }], [{ _id: 1, count: 1 }, { _id: "rest", count: 2 }]],
        ["$facet", [{ $facet: { n: [{ $count: "c" }], ids: [{ $project: { _id: 1 } }, { $limit: 1 }] } }], [{ n: [{ c: 3 }], ids: [{ _id: 1 }] }]],
        ["$divide is double", [{ $match: { _id: 1 } }, { $project: { _id: 0, d: { $divide: [4, 2] }, s: { $add: [1, 2] } } }], [{ d: 2, s: 3 }]],
        ["$cond $ifNull $size", [{ $project: { _id: 0, k: { $cond: [{ $gt: [{ $size: "$items" }, 1] }, "many", { $ifNull: ["$none", "few"] }] } } }], [{ k: "many" }, { k: "few" }, { k: "few" }]],
    ];
    for (const [label, pipe, want] of AGG) assertDeepEq(await o.aggregate(pipe), want, "aggregate: " + label);

    const AGGERR = [
        ["unknown stage", [{ $nope: 1 }], 40324],
        ["unknown operator", [{ $project: { a: { $nope: 1 } } }], 168],
        ["divide by zero", [{ $project: { a: { $divide: [1, 0] } } }], 16608],
        ["$lookup pipeline form", [{ $lookup: { from: "cust", pipeline: [], as: "c" } }], 9],
    ];
    for (const [label, pipe, want] of AGGERR) {
        let threw = false;
        let p;
        try {
            p = o.aggregate(pipe);
        } catch (e) {
            threw = true;
        }
        assert(!threw && p instanceof Promise, "aggregate never throws synchronously: " + label);
        const got = await codeOfAsync(p);
        assert(got === want, "aggregate rejects: " + label + " got " + got);
    }
}

{
    const w = new Database("w");
    const c = w.collection("e");
    assert(codeOf(() => new ChangeStream()) !== "no throw", "ChangeStream constructor throws");
    const s = c.watch([{ $match: { operationType: { $in: ["insert", "update", "delete"] } } }]);
    assert(s.tryNext() === null && s.closed === false, "empty stream");
    c.insertOne({ _id: 1, v: 1 });
    c.updateOne({ _id: 1 }, { $set: { v: 2 } });
    c.deleteOne({ _id: 1 });
    const KINDS = [
        ["insert", (e) => e.fullDocument.v === 1 && e.documentKey._id === 1 && e.ns.coll === "e"],
        ["update", (e) => e.updateDescription.updatedFields.v === 2 && Array.isArray(e.updateDescription.removedFields)],
        ["delete", (e) => e.documentKey._id === 1 && e.fullDocument === undefined],
    ];
    let first = null;
    for (const [kind, ok] of KINDS) {
        const r = await s.next();
        assert(!r.done && r.value.operationType === kind && ok(r.value), "event " + kind);
        assert(r.value.wallTime instanceof Date && typeof r.value._id._data === "string", "event token and wallTime: " + kind);
        if (!first) first = r.value._id;
    }
    const again = c.watch([], { resumeAfter: first });
    assertDeepEq([(await again.next()).value.operationType, (await again.next()).value.operationType], ["update", "delete"], "resumeAfter delivers what follows");
    again.close();
    assert(again.closed && (await again.next()).done, "closed stream is done");

    const look = c.watch([], { fullDocument: "updateLookup" });
    c.insertOne({ _id: 2, v: 1 });
    c.updateOne({ _id: 2 }, { $inc: { v: 1 } });
    await look.next();
    assert((await look.next()).value.fullDocument.v === 2, "updateLookup carries the document");
    look.close();

    const held = c.watch();
    const pending = held.next();
    assert(codeOf(() => w.close()) !== "no throw", "close refused while next() waits");
    c.drop();
    const seq = [];
    let r = await pending;
    while (!r.done) {
        seq.push(r.value.operationType);
        r = await held.next();
    }
    assertDeepEq(seq, ["drop", "invalidate"], "drop then invalidate ends a collection stream");

    const small = new Database("s", { changeStreamCapacity: 4 });
    const sc = small.collection("q");
    const lag = sc.watch();
    for (let i = 0; i < 10; i++) sc.insertOne({ _id: i });
    assert((await codeOfAsync(lag.next())) === 286, "evicted position is ChangeStreamHistoryLost 286");
    assert(codeOf(() => sc.watch([], { resumeAfter: { _data: "zz" } }).tryNext()) !== "no throw", "foreign token refused");
    assert(codeOf(() => sc.watch([{ $group: { _id: 1 } }])) === 2, "watch refuses $group with BadValue 2");
    lag.close();
    small.close();
    s.close();
    w.close();
}

{
    const dir = makeTempDir("bb-docdb");
    try {
        const p = db.persist(dir + "/a.snap");
        const res = await p;
        assert(res.collections === db.listCollections().length && res.documents === db.stats().documents && res.bytes > 0, "persist counts");
        const back = await Database.load(dir + "/a.snap");
        assert(back.name === "bb", "load restores the name");
        assertDeepEq(back.collection("people").find({ "addr.city": "Rome" }), c.find({ "addr.city": "Rome" }), "documents survive");
        assertDeepEq(back.collection("ix").listIndexes(), db.collection("ix").listIndexes(), "index specs survive");
        assert(back.collection("ix").explain({ a: 1 }).queryPlanner.winningPlan.stage !== "COLLSCAN", "indexes rebuilt");
        back.close();

        const bytes = readBytes(new Path(dir + "/a.snap"));
        const DAMAGE = [
            ["truncated", bytes.slice(0, bytes.length - 9)],
            ["extended", (() => { const b = new Uint8Array(bytes.length + 1); b.set(bytes); return b; })()],
            ["bit flip", (() => { const b = bytes.slice(); b[b.length >> 1] ^= 4; return b; })()],
            ["empty", new Uint8Array(0)],
        ];
        for (const [label, b] of DAMAGE) {
            writeFile(new Path(dir + "/bad.snap"), b);
            assert((await codeOfAsync(Database.load(dir + "/bad.snap"))) === 100001, "InvalidSnapshotFile: " + label);
        }
        assert((await codeOfAsync(Database.load(dir + "/missing.snap"))) === 100002, "SnapshotIOError on a missing file");

        const m = new Database("m");
        m.collection("keep").insertOne({ _id: 1 });
        await db.collection("people").persist(dir + "/p.snap");
        await m.load(dir + "/p.snap", { merge: true });
        assertDeepEq(m.listCollections().sort(), ["keep", "people"], "merge keeps other collections");
        await m.load(dir + "/p.snap");
        assertDeepEq(m.listCollections(), ["people"], "load replaces the database");
        m.close();
    } finally {
        removeAll(dir);
    }
}

assert(db.dropCollection("upd") === true && db.dropCollection("upd") === false, "dropCollection reports existence");
assert(codeOf(() => db.createCollection("people")) === 48, "NamespaceExists 48");
db.close();
assert(db.closed, "closed");
print("bb_docdb: all tests passed (" + n + " assertions)");
