// tests/types/docdb: compile-time contract of src/modules/docdb/docdb.d.ts (tsc --strict --noEmit); @ts-expect-error lines must stay errors.
import { Database, Collection, ObjectId, DocDBError, ChangeEvent, Doc, DocStage } from "dyna:docdb";

type Eq<A, B> = (<X>() => X extends A ? 1 : 2) extends (<X>() => X extends B ? 1 : 2) ? true : false;
declare function assert<T extends true>(): void;

interface User { name: string; age: number; tags: string[]; addr?: { city: string; zip: number }; orders: { sku: string; qty: number; price: number }[] }
interface Order { _id: string; user: string; total: number }
type Schemas = { users: User; orders: Order };

async function main() {
    const db = new Database<Schemas>("shop");
    const users = db.collection("users");
    const orders = db.collection("orders");
    const loose = db.collection("anything");
    assert<Eq<typeof loose, Collection<Doc, Schemas>>>();

    users.insertOne({ name: "a", age: 3, tags: [], orders: [] });
    orders.insertOne({ _id: "o1", user: "a", total: 9 });
    // @ts-expect-error orders fix _id as string: it is required (no ObjectId fallback)
    orders.insertOne({ user: "a", total: 9 });
    // @ts-expect-error age is a number
    users.insertOne({ name: "a", age: "3", tags: [], orders: [] });

    users.find({ age: { $gte: 18 }, "addr.city": "Pune", tags: "x", "orders.qty": { $gt: 2 } });
    users.find({ $or: [{ name: /^a/ }, { tags: { $all: ["x", "y"] } }], "orders.0.sku": "s1" });
    users.find({ orders: { $elemMatch: { qty: { $gt: 1 }, sku: "s" } } });
    // @ts-expect-error unknown field path
    users.find({ "addr.street": "x" });
    // @ts-expect-error $regex only on strings
    users.find({ age: { $regex: "1" } });
    // @ts-expect-error $size only on arrays
    users.find({ name: { $size: 2 } });

    const p = users.find({}, { projection: { name: 1, "addr.city": 1 } });
    assert<Eq<keyof (typeof p)[number], "name" | "addr" | "_id">>();
    const p2 = users.findOne({}, { projection: { name: 1, _id: 0 } });
    assert<Eq<keyof NonNullable<typeof p2>, "name">>();
    const p3 = users.findOne({}, { projection: { orders: 0 } });
    assert<Eq<keyof NonNullable<typeof p3>, "_id" | "name" | "age" | "tags" | "addr">>();
    const id = users.insertOne({ name: "b", age: 1, tags: [], orders: [] }).insertedId;
    assert<Eq<typeof id, ObjectId>>();
    assert<Eq<ReturnType<typeof users.distinct<"tags">>, string[]>>();

    users.updateOne({ name: "a" }, { $set: { "addr.city": "X" }, $inc: { age: 1 }, $push: { tags: { $each: ["p"], $slice: -5 } } });
    users.updateMany({}, { $set: { "orders.$[].qty": 0 }, $pull: { orders: { qty: { $lt: 1 } } } });
    // @ts-expect-error $inc on a string path
    users.updateOne({}, { $inc: { name: 1 } });
    // @ts-expect-error $push on a non-array
    users.updateOne({}, { $push: { age: 1 } });
    // @ts-expect-error $set with the wrong value type
    users.updateOne({}, { $set: { "addr.zip": "411" } });

    const g = await users.aggregate([
        { $match: { age: { $gt: 20 } } },
        { $unwind: "$orders" },
        { $group: { _id: "$orders.sku", qty: { $sum: "$orders.qty" }, buyers: { $addToSet: "$name" }, top: { $max: "$orders.price" } } },
        { $sort: { qty: -1 } },
        { $limit: 10 },
    ]);
    assert<Eq<(typeof g)[number], { _id: string; qty: number; buyers: string[]; top: number }>>();

    const pr = await users.aggregate([{ $project: { name: 1, n: { $size: "$tags" }, up: { $toUpper: "$name" }, _id: 0 } }]);
    assert<Eq<(typeof pr)[number], { name: string; n: number; up: string }>>();

    const lk = await users.aggregate([{ $lookup: { from: "orders", localField: "name", foreignField: "user", as: "os" } }]);
    assert<Eq<(typeof lk)[number]["os"], (Order & { _id: string })[]>>();

    const c = await users.aggregate([{ $match: {} }, { $count: "n" }]);
    assert<Eq<(typeof c)[number], { n: number }>>();
    const f = await users.aggregate([{ $facet: { cnt: [{ $count: "k" }], names: [{ $project: { name: 1, _id: 0 } }] } }]);
    assert<Eq<(typeof f)[number]["cnt"], { k: number }[]>>();
    const sbc = await users.aggregate([{ $sortByCount: "$age" }]);
    assert<Eq<(typeof sbc)[number], { _id: number; count: number }>>();
    const rr = await users.aggregate([{ $replaceRoot: { newRoot: "$addr" } }]);
    assert<Eq<(typeof rr)[number], { city: string; zip: number } | undefined>>();

    const dyn: any[] = [];
    const d = await loose.aggregate(dyn);
    assert<Eq<typeof d, Doc[]>>();
    const typed: DocStage<User & { _id: ObjectId }, Schemas>[] = [{ $match: { age: 1 } }];
    const d2 = await users.aggregate(typed);
    assert<Eq<typeof d2, Doc[]>>();
    // @ts-expect-error unknown stage
    users.aggregate([{ $bogus: 1 }]);
    // @ts-expect-error $lookup from an unknown collection
    users.aggregate([{ $lookup: { from: "nope", localField: "name", foreignField: "x", as: "y" } }]);
    // @ts-expect-error $unwind of a non-array path
    users.aggregate([{ $unwind: "$name" }]);

    for await (const ev of users.watch([{ $match: { operationType: "insert" } }])) {
        if (ev.operationType === "insert") { const n: string = ev.fullDocument.name; void n; }
        // @ts-expect-error delete events carry no fullDocument
        if (ev.operationType === "delete") void ev.fullDocument;
    }
    try { users.insertOne({ name: "a", age: 1, tags: [], orders: [] }); }
    catch (e) { if (e instanceof DocDBError && e.codeName === "DuplicateKey") { const k: number = e.code; void k; } }
    void (null as unknown as ChangeEvent<User>);
}
void main;
