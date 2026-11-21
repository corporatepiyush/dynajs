// covers: dyna:docdb.Database.load
// Load validates every document and rebuilds every index (indexes are not stored). Both run on parallel workers once a
// collection holds 4096+ documents (serially below that), so both paths are checked: every index of every collection
// must come back with the same specs and entry counts, and every query through it must return exactly what the
// original database returned (and what a scan returns), across single-field, multikey, compound descending, unique,
// sparse and partial indexes. A range on an array field ({tags: {$gt, $lt}}) matches documents whose elements fall on
// each side, so the rebuilt index must still know it is multikey or the planner intersects the bounds and loses them.
// A file whose checksums are valid but whose documents break a unique index (crafted here by rewriting one value and
// recomputing every CRC32C) must be refused with InvalidSnapshotFile on either path, and so must a checksum-valid file
// holding one malformed document anywhere among thousands (first, middle or last). The validate workers also build each
// document's _id key and hash, which the serial insert then trusts: every _id must still find its document after load,
// and a forged duplicate _id held by two different workers must still be refused.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { Path, makeTempDir, removeAll, readBytes, writeFile } from "dyna:file";
import { CRC32C } from "dyna:hash";

const t = suite("docdb.Database.load parallel");
const dir = makeTempDir("docdb-load-ix");
const J = (v) => JSON.stringify(v);

function fill(db, name, n) {
    const c = db.collection(name);
    const docs = [];
    for (let i = 0; i < n; i++) {
        const d = { _id: i, k: i % 97, s: `s${i % 13}`, tags: [`t${i % 5}`, `t${i % 11}`], g: { h: i % 3 } };
        if (i % 4 === 0) d.sp = i;
        if (i % 9 === 0) d.k = null;
        docs.push(d);
    }
    c.insertMany(docs);
    c.createIndex({ k: 1 });
    c.createIndex({ tags: 1 });
    c.createIndex({ s: 1, k: -1 });
    c.createIndex({ sp: 1 }, { unique: true, sparse: true });
    c.createIndex({ "g.h": 1 }, { partialFilterExpression: { k: { $gt: 50 } } });
    return c;
}

const QUERIES = [
    { k: 42 },
    { k: null },
    { tags: "t3" },
    { tags: { $in: ["t1", "t10"] } },
    { tags: { $gt: "t3", $lt: "t4" } },
    { s: "s4", k: { $gte: 60 } },
    { sp: { $gt: 100, $lt: 400 } },
    { "g.h": 2, k: { $gt: 50 } },
    { k: { $gte: 90 } },
];

const src = new Database("ld");
fill(src, "big", 6000);
fill(src, "small", 300);

t.test(
    "indexes rebuilt on load (parallel for big collections, serial for small) answer like the original",
    async (a) => {
        const path = `${dir}/s.dds`;
        await src.persist(path);
        const back = await Database.load(path);
        a.eq(back.stats().indexEntries, src.stats().indexEntries);
        a.eq(back.stats().indexes, src.stats().indexes);
        for (const name of ["big", "small"]) {
            const o = src.collection(name),
                b = back.collection(name);
            a.eq(J(b.listIndexes()), J(o.listIndexes()));
            for (const q of QUERIES) {
                const want = J(o.find(q, { sort: { _id: 1 } }).map((d) => d._id));
                a.eq(J(b.find(q, { sort: { _id: 1 } }).map((d) => d._id)), want, `${name} ${J(q)}`);
                a.eq(b.explain(q).queryPlanner.winningPlan.stage !== "COLLSCAN", true, `${name} ${J(q)} uses an index`);
                a.eq(b.explain(q).executionStats.nReturned, o.countDocuments(q));
            }
            a.eq(
                J(b.find({}, { sort: { s: 1, k: -1 }, limit: 20 }).map((d) => d._id)),
                J(o.find({}, { sort: { s: 1, k: -1 }, limit: 20 }).map((d) => d._id)),
            );
        }
        back.close();
    },
);

t.test("a unique index keeps refusing duplicates after load", async (a) => {
    const path = `${dir}/u.dds`;
    await src.persist(path);
    const back = await Database.load(path);
    let code = 0;
    try {
        back.collection("big").insertOne({ _id: "dup", sp: 8 });
    } catch (e) {
        code = e.code;
    }
    a.eq(code, 11000);
    back.close();
});

function find(b, needle) {
    for (let i = 0; i + needle.length <= b.length; i++) if (needle.every((x, j) => b[i + j] === x)) return i;
    throw new Error("value not found");
}

function forgeDuplicate(file, from, to) {
    forge(file, (b, v) => {
        const at = find(b, [0x10, 0x75, 0x00, from & 255, (from >> 8) & 255, 0, 0]);
        v.setInt32(at + 3, to, true);
        return at;
    });
}

function forge(file, edit) {
    const b = readBytes(new Path(file));
    const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const at = edit(b, v);
    const nsec = v.getUint32(24, true),
        crcs = [];
    let off = 32;
    for (let s = 0; s < nsec; s++) {
        const len = v.getUint32(off + 4, true);
        const start = off + 16;
        if (at >= start && at < start + len) v.setUint32(off + 12, CRC32C(b.subarray(start, start + len)), true);
        crcs.push(v.getUint32(off + 12, true));
        off = start + len;
    }
    const cb = new Uint8Array(crcs.length * 4);
    crcs.forEach((c, k) => new DataView(cb.buffer).setUint32(k * 4, c, true));
    v.setUint32(off + 20, CRC32C(cb), true);
    writeFile(new Path(file), b);
}

t.test(
    "a checksum-valid file whose documents violate a unique index is refused (serial and parallel rebuild)",
    async (a) => {
        for (const n of [5, 5000]) {
            const db = new Database("forge");
            const c = db.collection("dupe");
            c.insertMany(Array.from({ length: n }, (_, i) => ({ _id: i, u: 1000 + i })));
            c.createIndex({ k: 1 });
            c.createIndex({ u: 1 }, { unique: true });
            const path = `${dir}/forge${n}.dds`;
            await db.persist(path);
            db.close();
            const ok = await Database.load(path);
            a.eq(ok.collection("dupe").countDocuments({ u: 1001 }), 1);
            ok.close();
            forgeDuplicate(path, 1001, 1000);
            let err = null;
            try {
                await Database.load(path);
            } catch (e) {
                err = e;
            }
            a.eq(err instanceof DocDBError && err.code, 100001, `n=${n}`);
            a.eq(/index rebuild failed/.test(String(err && err.message)), true, String(err && err.message));
        }
    },
);

t.test("ids keyed by the validate workers find every document, and a duplicate _id across workers is refused", async (a) => {
    for (const n of [6, 5000]) {
        const db = new Database("ids");
        const ids = Array.from({ length: n }, (_, i) => (i % 3 === 0 ? i : i % 3 === 1 ? `s${i}` : { o: i }));
        db.collection("c").insertMany(ids.map((id, i) => ({ _id: id, w: i })));
        const path = `${dir}/ids${n}.dds`;
        await db.persist(path);
        db.close();
        const back = await Database.load(path);
        const c = back.collection("c");
        a.eq(ids.filter((id, i) => c.findOne({ _id: id })?.w !== i).length, 0, `n=${n}: every _id finds its document`);
        a.eq(c.findOne({ _id: { o: 2 } })?.w, 2, `n=${n}: a document _id is found`);
        let code = 0;
        try {
            c.insertOne({ _id: "s1" });
        } catch (e) {
            code = e.code;
        }
        a.eq(code, 11000, `n=${n}: a loaded _id refuses a duplicate`);
        back.close();
        const last = n - 1 - ((n - 1) % 3);
        forge(path, (b, v) => {
            const at = find(b, [0x10, 0x5f, 0x69, 0x64, 0x00, last & 255, (last >> 8) & 255, 0, 0]);
            v.setInt32(at + 5, 0, true);
            return at;
        });
        let err = null;
        try {
            await Database.load(path);
        } catch (e) {
            err = e;
        }
        a.eq(err instanceof DocDBError && err.code, 100001, `n=${n}`);
        a.eq(/duplicate key/.test(String(err && err.message)), true, String(err && err.message));
    }
});

t.test("a checksum-valid file with one malformed document among thousands is refused wherever it sits", async (a) => {
    for (const [n, victim] of [
        [5000, 0],
        [5000, 2500],
        [5000, 4999],
        [6, 3],
    ]) {
        const db = new Database("forge");
        db.collection("v").insertMany(Array.from({ length: n }, (_, i) => ({ _id: i, f: true, w: 70000 + i })));
        const path = `${dir}/bad${n}-${victim}.dds`;
        await db.persist(path);
        db.close();
        forge(path, (b) => {
            const w = 70000 + victim;
            const at = find(b, [0x10, 0x77, 0x00, w & 255, (w >> 8) & 255, (w >> 16) & 255, 0]);
            if (b[at - 4] !== 0x08 || b[at - 3] !== 0x66) throw new Error("layout");
            b[at - 1] = 2;
            return at;
        });
        let err = null;
        try {
            await Database.load(path);
        } catch (e) {
            err = e;
        }
        a.eq(err instanceof DocDBError && err.code, 100001, `n=${n} victim=${victim}`);
        a.eq(/not valid BSON/.test(String(err && err.message)), true, String(err && err.message));
    }
});

await t.run();
src.close();
removeAll(dir);
