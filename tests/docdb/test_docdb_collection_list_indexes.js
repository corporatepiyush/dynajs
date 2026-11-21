// covers: dyna:docdb.Collection.listIndexes
// listIndexes answers the specs in creation order with the implicit _id_ first: { v: 2, key, name } plus unique,
// sparse and partialFilterExpression only when set. Oracle: the expected spec list is built in JS from the
// createIndex calls (default names "f_1_g_-1", explicit names kept, key order kept) and compared as JSON text (field
// order counts) after every create and drop; a repeated spec adds nothing. A missing collection answers [] without
// being created; a dropped collection loses its indexes.
import { suite } from "../lib/kit.js";
import { Database } from "dyna:docdb";
import { J } from "../lib/docdb_model.js";

const t = suite("docdb.Collection.listIndexes");
const ID = { v: 2, key: { _id: 1 }, name: "_id_" };

function spec(keys, opts = {}) {
    const s = { v: 2, key: keys, name: opts.name || Object.entries(keys).map(([k, d]) => k + "_" + d).join("_") };
    if (opts.unique) s.unique = true;
    if (opts.sparse) s.sparse = true;
    if (opts.partialFilterExpression) s.partialFilterExpression = opts.partialFilterExpression;
    return s;
}

t.test("creation order, _id_ first, options only when set", (a) => {
    const db = new Database("li"), c = db.collection("c");
    c.insertMany([{ _id: 1, a: 1, b: "x", d: { e: 1 } }, { _id: 2, a: 2 }]);
    const want = [ID];
    a.eq(J(c.listIndexes()), J(want), "only _id_");
    const calls = [
        [{ a: 1 }],
        [{ b: -1, a: 1 }, { unique: true, sparse: true, name: "bx" }],
        [{ "d.e": 1 }, { partialFilterExpression: { a: { $gt: 1 } } }],
        [{ z: -1, y: 1 }],
        [{ a: 1 }],
    ];
    for (const [keys, opts] of calls) {
        const name = c.createIndex(keys, opts);
        const s = spec(keys, opts);
        a.eq(name, s.name, "createIndex name");
        if (!want.some((w) => w.name === s.name)) want.push(s);
        a.eq(J(c.listIndexes()), J(want), "after " + J([keys, opts]));
    }
    c.dropIndex("bx");
    want.splice(want.findIndex((w) => w.name === "bx"), 1);
    a.eq(J(c.listIndexes()), J(want), "after a drop");
    const got = c.listIndexes();
    got[0].name = "mutated";
    a.eq(c.listIndexes()[0].name, "_id_", "each call answers fresh objects");
});

t.test("missing and dropped collections answer [] and are not created", (a) => {
    const db = new Database("li2"), c = db.collection("c");
    a.deep(db.collection("none").listIndexes(), []);
    a.deep(db.listCollections(), []);
    c.createIndex({ a: 1 });
    a.eq(c.listIndexes().length, 2);
    c.drop();
    a.deep(c.listIndexes(), []);
    c.insertOne({ a: 1 });
    a.eq(J(c.listIndexes()), J([ID]), "a recreated collection starts with _id_ only");
});

await t.run();
