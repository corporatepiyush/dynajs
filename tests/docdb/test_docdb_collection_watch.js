// covers: dyna:docdb.Collection.watch
// Change streams against the MongoDB manual's "Change Events" page: event shapes per operationType (insert and
// replace carry fullDocument, update carries updateDescription {updatedFields, removedFields}, delete only the
// documentKey), resume tokens ({_data}) that replay everything after them, a pipeline of $match/$project-like stages
// (anything else is BadValue 2), fullDocument: "updateLookup" (the document as it is when the event is read, null once
// deleted), collection drop = "drop" then "invalidate" then the stream ends, and ChangeStreamHistoryLost 286 when the
// resume point fell out of the bounded change ring (changeStreamCapacity), whether at watch() or while reading. Also
// the async-iterator protocol, tryNext, and that a pending next() holds the Database open (close() is refused).
import { suite } from "../lib/kit.js";
import { Database, DocDBError, ChangeStream } from "dyna:docdb";

const t = suite("docdb.Collection.watch");
const J = (x) => JSON.stringify(x, (k, v) => (k === "wallTime" || k === "_id" && v && typeof v === "object" && "_data" in v ? undefined : v));
const op = (e) => e && `${e.operationType}:${e.documentKey ? e.documentKey._id : "-"}`;
const codeOf = (fn) => {
    try {
        fn();
        return "no throw";
    } catch (e) {
        return e instanceof DocDBError ? e.code : String(e);
    }
};

t.test("event shapes for insert, update, replace and delete", async (a) => {
    const db = new Database("w1");
    const c = db.collection("c");
    const cs = c.watch();
    a.ok(cs instanceof ChangeStream);
    c.insertOne({ _id: 1, a: 1, b: { c: 1 } });
    c.updateOne({ _id: 1 }, { $set: { "b.c": 2, z: [1] }, $unset: { a: "" } });
    c.replaceOne({ _id: 1 }, { x: 1 });
    c.deleteOne({ _id: 1 });
    const ins = (await cs.next()).value;
    a.eq(J(ins), '{"operationType":"insert","ns":{"db":"w1","coll":"c"},"documentKey":{"_id":1},"fullDocument":{"_id":1,"a":1,"b":{"c":1}}}');
    a.ok(ins.wallTime instanceof Date);
    a.eq(typeof ins._id._data, "string");
    a.eq(J(cs.tryNext()), '{"operationType":"update","ns":{"db":"w1","coll":"c"},"documentKey":{"_id":1},"updateDescription":{"updatedFields":{"b.c":2,"z":[1]},"removedFields":["a"]}}');
    a.eq(J(cs.tryNext()), '{"operationType":"replace","ns":{"db":"w1","coll":"c"},"documentKey":{"_id":1},"fullDocument":{"_id":1,"x":1}}');
    a.eq(J(cs.tryNext()), '{"operationType":"delete","ns":{"db":"w1","coll":"c"},"documentKey":{"_id":1}}');
    a.eq(cs.tryNext(), null);
    cs.close();
    db.close();
});

t.test("next() waits for the next write; queued next() calls resolve in order; tryNext refuses while one waits", async (a) => {
    const db = new Database("w2");
    const c = db.collection("c");
    const cs = c.watch();
    const p1 = cs.next(), p2 = cs.next();
    a.eq(codeOf(() => cs.tryNext()).includes("already waiting"), true);
    let settled = false;
    p1.then(() => (settled = true));
    await null;
    a.eq(settled, false, "nothing written yet");
    c.insertMany([{ _id: 1 }, { _id: 2 }]);
    a.eq(op((await p1).value), "insert:1");
    a.eq(op((await p2).value), "insert:2");
    const p3 = cs.next();
    c.updateOne({ _id: 1 }, { $set: { u: 1 } });
    a.eq(op((await p3).value), "update:1", "an update wakes a waiting next()");
    const p4 = cs.next();
    c.deleteOne({ _id: 2 });
    a.eq(op((await p4).value), "delete:2", "a delete alone wakes a waiting next()");
    cs.close();
    a.eq(J(await cs.next()), '{"done":true}');
    db.close();
});

t.test("events written before watch() are not delivered; a collection that does not exist yet can be watched", async (a) => {
    const db = new Database("w3");
    const c = db.collection("c");
    c.insertOne({ _id: 0 });
    const cs = c.watch();
    const later = db.collection("later").watch();
    c.insertOne({ _id: 1 });
    db.collection("later").insertOne({ _id: 7 });
    a.eq(op(cs.tryNext()), "insert:1");
    a.eq(cs.tryNext(), null);
    a.eq(op(later.tryNext()), "insert:7");
    cs.close();
    later.close();
    db.close();
});

t.test("a pipeline filters and reshapes; other stages are BadValue 2", async (a) => {
    const db = new Database("w4");
    const c = db.collection("c");
    const cs = c.watch([{ $match: { operationType: { $in: ["insert", "replace"] }, "fullDocument.v": { $gt: 1 } } }, { $project: { "fullDocument.v": 1 } }]);
    c.insertMany([{ _id: 1, v: 1 }, { _id: 2, v: 2, w: 0 }]);
    c.deleteOne({ _id: 2 });
    c.replaceOne({ _id: 1 }, { v: 9 });
    a.eq(J(cs.tryNext()), '{"fullDocument":{"v":2}}');
    a.eq(J(cs.tryNext()), '{"fullDocument":{"v":9}}');
    a.eq(cs.tryNext(), null);
    a.eq(codeOf(() => c.watch([{ $group: { _id: 1 } }])), 2);
    a.eq(codeOf(() => c.watch([{ $lookup: { from: "c", localField: "a", foreignField: "a", as: "x" } }])), 2);
    a.eq(codeOf(() => c.watch("x")), 14);
    a.eq(codeOf(() => c.watch([], { fullDocument: "whenAvailable" })), 2);
    cs.close();
    db.close();
});

t.test("fullDocument: updateLookup reads the document as it is now; null once it is gone", async (a) => {
    const db = new Database("w5");
    const c = db.collection("c");
    const cs = c.watch([], { fullDocument: "updateLookup" });
    c.insertOne({ _id: 1, n: 0 });
    c.updateOne({ _id: 1 }, { $inc: { n: 1 } });
    c.updateOne({ _id: 1 }, { $inc: { n: 1 } });
    cs.tryNext();
    a.eq(J(cs.tryNext().fullDocument), '{"_id":1,"n":2}', "the current version, not the one at update time");
    c.deleteOne({ _id: 1 });
    a.eq(cs.tryNext().fullDocument, null);
    cs.close();
    db.close();
});

t.test("resumeAfter replays everything after the token; startAfter is accepted the same way", async (a) => {
    const db = new Database("w6");
    const c = db.collection("c");
    const cs = c.watch();
    c.insertMany([{ _id: 1 }, { _id: 2 }, { _id: 3 }]);
    cs.tryNext();
    const tok = cs.resumeToken;
    cs.close();
    const r1 = c.watch([], { resumeAfter: tok });
    a.eq([op(r1.tryNext()), op(r1.tryNext()), r1.tryNext()].join(), "insert:2,insert:3,");
    const r2 = c.watch([], { startAfter: tok });
    a.eq(op(r2.tryNext()), "insert:2");
    a.eq(codeOf(() => c.watch([], { resumeAfter: { _data: "zz" } })), 2);
    a.eq(codeOf(() => c.watch([], { resumeAfter: { _data: "ffffffffffffffff" } })), 286, "a token from the future");
    r1.close();
    r2.close();
    db.close();
});

t.test("HistoryLost 286 once the resume point was evicted from the ring, at watch() and while reading", async (a) => {
    const db = new Database("w7", { changeStreamCapacity: 4 });
    const c = db.collection("c");
    const cs = c.watch();
    c.insertOne({ _id: 0 });
    const tok = cs.resumeToken;
    for (let i = 1; i <= 6; i++) c.insertOne({ _id: i });
    a.eq(codeOf(() => cs.tryNext()), 286, "the reader fell behind the ring");
    await a.rejects(cs.next(), DocDBError);
    a.eq(codeOf(() => c.watch([], { resumeAfter: tok })), 286);
    const fresh = c.watch();
    c.insertOne({ _id: 9 });
    a.eq(op(fresh.tryNext()), "insert:9", "a new stream starts at the present");
    cs.close();
    fresh.close();
    db.close();
});

t.test("a resume point from before change recording started, or before a drop, is HistoryLost 286", async (a) => {
    const db = new Database("w8");
    const c = db.collection("c");
    const first = c.watch();
    const tok0 = first.resumeToken;
    first.close();
    c.insertOne({ _id: 1 });
    const ok = c.watch([], { resumeAfter: tok0 });
    a.eq(op(ok.tryNext()), "insert:1", "once a stream has existed every collection keeps recording");
    ok.close();
    const tok1 = c.watch().resumeToken;
    db.collection("gone").insertOne({ _id: 1 });
    db.dropCollection("gone");
    a.eq(codeOf(() => c.watch([], { resumeAfter: tok1 })), 286, "a drop with no stream open");
    const held = c.watch();
    const tok2 = held.resumeToken;
    db.collection("gone2").insertOne({ _id: 1 });
    db.dropCollection("gone2");
    a.eq(codeOf(() => c.watch([], { resumeAfter: tok2 })), 286, "a drop while another stream is open");
    held.close();
    const db2 = new Database("w8b");
    db2.collection("c").insertOne({ _id: 1 });
    a.eq(codeOf(() => db2.collection("c").watch([], { resumeAfter: { _data: "0000000000000000" } })), 286, "writes before any watch() were not recorded");
    db2.close();
    db.close();
});

t.test("dropping the collection delivers drop, then invalidate, then the stream is done", async (a) => {
    const db = new Database("w9");
    const c = db.collection("c");
    c.insertOne({ _id: 1 });
    const cs = c.watch();
    const pending = cs.next();
    c.insertOne({ _id: 2 });
    c.drop();
    a.eq(op((await pending).value), "insert:2", "undelivered events survive the drop");
    a.eq(J((await cs.next()).value), '{"operationType":"drop","ns":{"db":"w9","coll":"c"}}');
    a.eq(J((await cs.next()).value), '{"operationType":"invalidate"}');
    a.eq(J(await cs.next()), '{"done":true}');
    a.eq(cs.closed, true);
    db.close();
});

t.test("for await ... of, and break closes the stream", async (a) => {
    const db = new Database("w10");
    const c = db.collection("c");
    const cs = c.watch();
    setTimeout(() => c.insertMany([{ _id: 1 }, { _id: 2 }, { _id: 3 }]), 1);
    const seen = [];
    for await (const ev of cs) {
        seen.push(ev.documentKey._id);
        if (seen.length === 2) break;
    }
    a.eq(seen.join(), "1,2");
    a.eq(cs.closed, true);
    db.close();
});

t.test("a pending next() holds the Database open; a closed Database fails the stream", async (a) => {
    const db = new Database("w11");
    const c = db.collection("c");
    const cs = c.watch();
    const p = cs.next();
    a.eq(codeOf(() => db.close()).includes("in flight"), true);
    cs.close();
    a.eq(J(await p), '{"done":true}', "close() ends the waiting next()");
    const cs2 = c.watch();
    db.close();
    await a.rejects(cs2.next(), TypeError);
    a.eq(codeOf(() => new ChangeStream()).includes("watch()"), true);
});

await t.run();
