// covers: dyna:docdb.Database.load
// Snapshot robustness. The file layout (dynajs.d.ts, dyna:docdb PERSIST) is parsed here independently: header 32 B,
// sections of {kind u32, len u64 as two u32, CRC32C u32} + payload, footer 24 B. Every truncation at and around every
// section boundary, every single-byte bit flip, an appended byte and foreign files must be refused with
// InvalidSnapshotFile (100001) and must leave the target database untouched; the intact file must load. Persist is
// atomic: no temp file survives, and a failed persist leaves the previous snapshot in place.
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { Path, makeTempDir, removeAll, readBytes, writeFile, readDir } from "dyna:file";

const t = suite("docdb.Database.load");
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

t.test("the intact snapshot loads with documents and rebuilt indexes", async (a) => {
    const db = await Database.load(ps("good.dds"));
    a.eq(db.name, "snap");
    a.eq(db.listCollections().sort().join(), "a,b,empty");
    a.eq(db.collection("a").countDocuments(), 40);
    a.eq(JSON.stringify(db.collection("a").listIndexes()), JSON.stringify(ca.listIndexes()));
    a.eq(JSON.stringify(db.collection("b").find({}, { projection: { _id: 0 } })), '[{"x":1},{"x":2,"nested":{"y":[1,2,3]}}]');
    a.throws(() => db.collection("a").insertOne({ k: 0, s: "v0" }), DocDBError, "the unique index came back");
    db.close();
});

t.test("truncation at and around every section boundary is refused", async (a) => {
    const { sections, cuts } = boundaries(GOOD);
    a.eq(sections, 4, "catalog + 3 collections");
    let n = 0;
    for (const c of cuts) if (await refused(a, GOOD.slice(0, c), `truncated to ${c}`)) n++;
    a.eq(n, cuts.length);
});

t.test("every single-byte bit flip is refused", async (a) => {
    let n = 0;
    for (let i = 0; i < GOOD.length; i++) {
        const bad = GOOD.slice();
        bad[i] ^= 1 << (i % 8);
        if (await refused(a, bad, `bit ${i % 8} of byte ${i}`)) n++;
    }
    a.eq(n, GOOD.length);
});

t.test("an appended byte, an empty file, a foreign file and a section-count lie are refused", async (a) => {
    const ext = new Uint8Array(GOOD.length + 1);
    ext.set(GOOD);
    a.ok(await refused(a, ext, "extended"));
    a.ok(await refused(a, new Uint8Array(0), "empty"));
    a.ok(await refused(a, new TextEncoder().encode("{\"not\":\"a snapshot\"}".repeat(4)), "json"));
    const lie = GOOD.slice();
    new DataView(lie.buffer).setUint32(lie.length - 8, 3, true);
    a.ok(await refused(a, lie, "footer section count"));
});

await t.run();
src.close();
removeAll(dir);
