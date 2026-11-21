// covers: dyna:docdb.Database.load
// P5: load validated every document with the full BSON validator, ~55% of its CPU at 100K documents. Validation now
// tries a one-pass fast check first (common types; UTF-8 checked only where a key or string has a byte >= 0x80) and
// falls back to the full validator for anything else, so it may only accept what the full one accepts. BSONDecode
// alone cannot show that (its decoder re-checks terminators and bounds as it walks), but load trusts what passed: a
// document accepted here is stored and later walked without checks. Oracle: an independent JS validator of the BSON
// structure (lengths bounded by the container, element and string terminators, UTF-8 keys and strings, boolean 0/1,
// binary lengths) over every single-byte mutation (11 replacement values) of every byte of 7 stored documents (one
// with non-ASCII keys and strings and a binary field); each mutated file is re-signed (section CRC32C and the
// footer's CRC of the section CRCs, layout per dynajs.d.ts PERSIST) so the checksums pass and only document
// validation stands between the bytes and the store. Every document the JS validator calls invalid must be refused
// with InvalidSnapshotFile 100001 (the database left as it was); the intact file loads.
// The last case pins the speed as a process-CPU ratio inside one run (load runs on worker threads): 20000 documents
// of common types against the same documents with a regex field first, which forces the full validator; median of
// 5 best-of-7 ratios, single ratios measured 0.54-0.80 with the fast path and 0.83-1.29 on the full path alone, base medians
// 0.90-1.02 (bound 0.8).
import { suite } from "../lib/kit.js";
import { Database, DocDBError } from "dyna:docdb";
import { BSONEncode } from "dyna:serialize";
import { CRC32C } from "dyna:hash";
import { cpuUsage } from "dyna:sys";
import { perfCheck } from "../lib/perf.js";
import { makeTempDir, removeAll, readBytes, writeFile, Path } from "dyna:file";

const t = suite("docdb.Database.load fast validation");
const rd32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const wr32 = (b, o, v) => { b[o] = v & 255; b[o + 1] = (v >>> 8) & 255; b[o + 2] = (v >>> 16) & 255; b[o + 3] = v >>> 24; };
const FIXED = { 1: 8, 9: 8, 17: 8, 18: 8, 16: 4, 19: 16, 7: 12, 6: 0, 10: 0, 255: 0, 127: 0 };

function utf8ok(b, s, e) {
    let i = s;
    while (i < e) {
        const c = b[i];
        if (c < 0x80) { i++; continue; }
        let n, lo = 0x80, hi = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) n = 1;
        else if (c >= 0xe0 && c <= 0xef) { n = 2; if (c === 0xe0) lo = 0xa0; if (c === 0xed) hi = 0x9f; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; if (c === 0xf0) lo = 0x90; if (c === 0xf4) hi = 0x8f; }
        else return false;
        if (i + n >= e) return false;
        if (b[i + 1] < lo || b[i + 1] > hi) return false;
        for (let k = 2; k <= n; k++) if ((b[i + k] & 0xc0) !== 0x80) return false;
        i += n + 1;
    }
    return true;
}

function vdoc(b, s, avail) {
    if (avail < 5) return false;
    const n = rd32(b, s);
    if (n < 5 || n > avail || b[s + n - 1] !== 0) return false;
    const end = s + n - 1;
    let off = s + 4;
    for (;;) {
        const ty = b[off];
        if (ty === 0) return off === end;
        off++;
        let z = off;
        while (z < end && b[z] !== 0) z++;
        if (z >= end || !utf8ok(b, off, z)) return false;
        off = z + 1;
        const av = end - off;
        let l;
        if (ty in FIXED) { l = FIXED[ty]; if (l > av) return false; }
        else if (ty === 8) { if (av < 1 || b[off] > 1) return false; l = 1; }
        else if (ty === 2 || ty === 13 || ty === 14) {
            if (av < 5) return false;
            const sl = rd32(b, off);
            if (sl < 1 || sl > av - 4 || b[off + 4 + sl - 1] !== 0 || !utf8ok(b, off + 4, off + 4 + sl - 1)) return false;
            l = 4 + sl;
        } else if (ty === 5) {
            if (av < 5) return false;
            const bl = rd32(b, off);
            if (bl > av - 5 || (b[off + 4] === 2 && (bl < 4 || rd32(b, off + 5) !== bl - 4))) return false;
            l = 5 + bl;
        } else if (ty === 3 || ty === 4) {
            const r = vdoc(b, off, av);
            if (r !== true) return r;
            l = rd32(b, off);
        } else return null;
        off += l;
    }
}

function sections(f) {
    const out = [], n = rd32(f, 24);
    let pos = 32;
    for (let s = 0; s < n; s++) {
        const len = rd32(f, pos + 4) + rd32(f, pos + 8) * 2 ** 32;
        out.push({ hdr: pos, from: pos + 16, to: pos + 16 + len });
        pos += 16 + len;
    }
    return out;
}

function resign(f) {
    const secs = sections(f), crcs = new Uint8Array(secs.length * 4);
    secs.forEach((s, i) => { const c = CRC32C(f.subarray(s.from, s.to)) >>> 0; wr32(f, s.hdr + 12, c); wr32(crcs, i * 4, c); });
    wr32(f, f.length - 4, CRC32C(crcs) >>> 0);
    return f;
}

function find(hay, needle) {
    outer: for (let i = 0; i + needle.length <= hay.length; i++) {
        for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
        return i;
    }
    return -1;
}

const DOCS = [
    { _id: 1, s: "abc", n: 7, b: true, d: { x: 1, y: "zz" }, arr: [1, "two", { t: false }] },
    { _id: 2, k: "", z: null, f: 2.5, big: 2 ** 40 },
    { _id: 3, a: [[1], [2, [3]]], e: {} },
    { _id: 4, name: "orders", items: [{ sku: "SKU-1", qty: 2 }, { sku: "SKU-22", qty: 1 }] },
    { _id: 5, when: new Date(1700000000000), t: true, u: false },
    { _id: 6, s: "x".repeat(40), n: -1 },
    { _id: 7, caf\u00e9: "cr\u00e8me br\u00fbl\u00e9e", bin: new Uint8Array([1, 2, 3]), t: "\u65e5\u672c" },
];
const VALUES = [0, 1, 2, 3, 4, 8, 0x10, 0x7f, 0x80, 0xc3, 0xff];

t.test("every mutation the JS validator rejects is refused by load", async (a) => {
    const dir = makeTempDir("docdb-loadv"), P = (n) => dir.toString() + "/" + n;
    try {
        const src = new Database("v"), c = src.collection("c");
        c.insertMany(DOCS);
        await src.persist(P("good.dds"));
        const good = readBytes(new Path(P("good.dds")));
        const back = await Database.load(P("good.dds"));
        a.eq(JSON.stringify(back.collection("c").find()), JSON.stringify(DOCS), "the intact file loads");
        back.close();
        resign(good);
        let invalid = 0, refusedN = 0;
        for (const d of DOCS) {
            const bytes = BSONEncode(d), at = find(good, bytes);
            if (!a.ok(at > 0, "document " + d._id + " found in the file")) return;
            for (let i = 0; i < bytes.length; i++) {
                for (const v of VALUES) {
                    if (bytes[i] === v) continue;
                    const m = bytes.slice();
                    m[i] = v;
                    if (vdoc(m, 0, m.length) !== false || rd32(m, 0) !== m.length) continue;
                    invalid++;
                    const f = good.slice();
                    f.set(m, at);
                    writeFile(new Path(P("bad.dds")), resign(f));
                    const target = new Database("target");
                    target.collection("keep").insertOne({ _id: "sentinel" });
                    let err = null;
                    try { await target.load(P("bad.dds")); } catch (e) { err = e; }
                    const ok = err instanceof DocDBError && err.code === 100001;
                    if (ok) refusedN++;
                    else if (!a.ok(false, "doc " + d._id + " byte " + i + " = " + v + " was " + (err ? err.message : "accepted"))) { target.close(); return; }
                    a.eq(target.collection("keep").countDocuments(), 1, "target untouched");
                    target.close();
                }
            }
        }
        a.ok(invalid > 300, "invalid mutations exercised (" + invalid + ")");
        a.eq(refusedN, invalid, "all refused");
    } finally { removeAll(dir); }
});

t.test("the fast path is taken: an all-common-types snapshot loads well under the cost of one forced onto the full path", async (a) => {
    const { skip } = a;
    const dir = makeTempDir("docdb-loadpin"), N = 20000;
    const mk = (i, r) => ({ _id: i, r, customer: "c" + (i % 500), status: "paid", items: [{ sku: "SKU-" + (i % 300), qty: 2 }, { sku: "S" + i, qty: 1 }], note: "n" + i });
    try {
        for (const [k, rv] of [["fast", "x"], ["full", /x/]]) {
            const db = new Database("p");
            db.collection("c").insertMany(Array.from({ length: N }, (_, i) => mk(i, rv)));
            await db.persist(dir.toString() + "/" + k + ".dds");
            db.close();
        }
        const cpu = () => { const u = cpuUsage(); return u.user + u.system; };
        const best = async (k) => {
            let b = Infinity;
            for (let r = 0; r < 7; r++) {
                const c0 = cpu(), d = await Database.load(dir.toString() + "/" + k + ".dds");
                b = Math.min(b, cpu() - c0);
                d.close();
            }
            return b;
        };
        const rs = [];
        for (let i = 0; i < 5; i++) rs.push((await best("fast")) / (await best("full")));
        const med = rs.sort((x, y) => x - y)[2];
        perfCheck(a.ok, med < 0.8, `fast / full load CPU median ${med.toFixed(2)} (full path for both: about 1.0)`, skip);
    } finally { removeAll(dir); }
});

await t.run();
