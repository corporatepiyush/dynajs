// covers: dyna:docdb.Database.collection
// collection(name) answers a handle on a collection that need not exist: it creates nothing until the first write,
// handles on one name share the data, the name is checked (empty, over 200 bytes, '$', NUL or a "system." prefix is
// InvalidOptions 72; a non-string is a TypeError), and the handle's name is the name given. Oracle: listCollections
// before and after each step, JS-held documents through two handles, and the documented name rules applied to a
// table of names in JS (200 bytes is the limit in UTF-8 bytes, so 100 two-byte characters pass and 101 fail).
import { suite } from "../lib/kit.js";
import { Database, DocDBError, Collection } from "dyna:docdb";
import { J } from "../lib/docdb_model.js";

const t = suite("docdb.Database.collection");

t.test("a handle creates nothing until a write; handles on one name share data", (a) => {
    const db = new Database("dc");
    const h1 = db.collection("orders"), h2 = db.collection("orders");
    a.ok(h1 instanceof Collection);
    a.eq(h1.name, "orders");
    a.deep(db.listCollections(), [], "a handle alone creates nothing");
    a.deep(h1.find(), []);
    a.eq(h1.countDocuments({}), 0);
    a.deep(h1.distinct("a"), []);
    a.deep(db.listCollections(), [], "reads create nothing");
    h1.insertOne({ _id: 1, a: 1 });
    a.deep(db.listCollections(), ["orders"]);
    a.eq(J(h2.find()), J([{ _id: 1, a: 1 }]), "the second handle sees the first one's write");
    h2.updateOne({ _id: 1 }, { $set: { a: 2 } });
    a.eq(J(h1.findOne({ _id: 1 })), J({ _id: 1, a: 2 }));
    a.throws(() => new Collection(), TypeError, "collections come from db.collection");
});

t.test("name rules", (a) => {
    const db = new Database("dc2");
    const rows = [
        ["a", true], ["a.b", true], ["x".repeat(200), true], ["é".repeat(100), true], ["é".repeat(101), false],
        ["x".repeat(201), false], ["", false], ["a$b", false], ["$x", false], ["a\0b", false], ["system.users", false],
        ["system", true], ["systemx", true], ["my.system.x", true],
    ];
    for (const [name, ok] of rows) {
        if (ok) a.eq(db.collection(name).name, name, J(name));
        else { const e = a.throws(() => db.collection(name), DocDBError, J(name)); a.eq(e && e.code, 72, J(name)); }
    }
    for (const bad of [undefined, 5, null, {}, Symbol("s")]) a.throws(() => db.collection(bad), TypeError, String(bad));
    a.deep(db.listCollections(), []);
});

await t.run();
