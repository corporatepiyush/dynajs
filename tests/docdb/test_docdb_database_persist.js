// covers: dyna:docdb.Database.persist
// Split from test_docdb_database_load_corruption.js (its persist cases; the prelude moved verbatim).
// Snapshot robustness. The file layout (dynajs.d.ts, dyna:docdb PERSIST) is parsed here independently: header 32 B,
// sections of {kind u32, len u64 as two u32, CRC32C u32} + payload, footer 24 B. Every truncation at and around every
// section boundary, every single-byte bit flip, an appended byte and foreign files must be refused with
// InvalidSnapshotFile (100001) and must leave the target database untouched; the intact file must load. Persist is
// atomic: no temp file survives, and a failed persist leaves the previous snapshot in place.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { Path, makeTempDir, removeAll, readBytes, writeFile, readDir } from "dyna:file";

const t = suite("docdb.Database.persist");
const dir = makeTempDir("docdb-corrupt");
const P = (n) => new Path(dir.toString() + "/" + n);
const ps = (n) => dir.toString() + "/" + n;

const src = new Database("snap");
const ca = src.collection("a"), cb = src.collection("b");
ca.insertMany(Array.from({ length: 40 }, (_, i) => ({ _id: i, k: i % 7, s: "v" + i, arr: [i, i + 1] })));
cb.insertMany([{ x: 1 }, { x: 2, nested: { y: [1, 2, 3] } }]);
ca.createIndex({ k: 1, s: -1 }, { unique: true });
ca.createIndex({ arr: 1 }, { sparse: true });
src.createCollection("empty");
await src.persist(ps("good.dds"));
const GOOD = readBytes(P("good.dds"));

function boundaries(bytes) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const n = dv.getUint32(24, true), out = [0, 8, 28, 32];
    let pos = 32;
    for (let s = 0; s < n; s++) {
        const len = dv.getUint32(pos + 4, true) + dv.getUint32(pos + 8, true) * 2 ** 32;
        out.push(pos, pos + 4, pos + 16, pos + 16 + len);
        pos += 16 + len;
    }
    out.push(pos, bytes.length - 8, bytes.length);
    return { sections: n, footer: pos, cuts: [...new Set(out.flatMap((c) => [c - 1, c, c + 1]))].filter((c) => c >= 0 && c < bytes.length) };
}

async function refused(a, bytes, what) {
    writeFile(P("bad.dds"), bytes);
    const target = new Database("target");
    target.collection("keep").insertOne({ _id: "sentinel" });
    let err = null;
    try {
        await target.load(ps("bad.dds"));
    } catch (e) {
        err = e;
    }
    const ok = err instanceof DocDBError && err.code === 100001 && err.codeName === "InvalidSnapshotFile";
    if (!ok) a.ok(false, `${what}: ${err ? err.code + " " + err.message : "accepted"}`);
    const intact = target.collection("keep").countDocuments() === 1 && target.listCollections().join() === "keep";
    if (!intact) a.ok(false, `${what}: the target changed`);
    target.close();
    return ok && intact;
}

t.test("persist is atomic: no temp file remains, and a failed persist keeps the old file", async (a) => {
    a.eq(readDir(dir).filter((e) => e.name.includes(".tmp.")).length, 0);
    a.ok(await src.persist(ps("good.dds")).then(() => true));
    let err = null;
    try {
        await src.persist(ps("no/such/dir/x.dds"));
    } catch (e) {
        err = e;
    }
    a.ok(err instanceof DocDBError && err.code === 100002, String(err && err.message));
    a.eq(readBytes(P("good.dds")).length, GOOD.length);
    a.eq(readDir(dir).filter((e) => e.name.includes(".tmp.")).length, 0);
});

t.test("a persist sees the state at the call, not later writes", async (a) => {
    const db = new Database("snapiso");
    const c = db.collection("c");
    c.insertMany(Array.from({ length: 2000 }, (_, i) => ({ _id: i, v: "x".repeat(200) })));
    const p = db.persist(ps("iso.dds"), { fsync: false });
    c.deleteMany({});
    c.insertOne({ _id: "late" });
    const r = await p;
    a.eq(r.documents, 2000);
    const back = await Database.load(ps("iso.dds"));
    a.eq(back.collection("c").countDocuments(), 2000);
    a.eq(back.collection("c").findOne({ _id: "late" }), null);
    a.eq(db.stats().pinnedSnapshots, 0);
    back.close();
    db.close();
});

await t.run();
src.close();
removeAll(dir);
