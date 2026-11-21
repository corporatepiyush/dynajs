// timeout: 300
// covers: contract dyna:vec.VectorIndex.save dyna:vec.VectorIndex.load -- save and load form one round trip; neither is testable alone
// VectorIndex persistence: one dyna persist v1 file per index (type 200), sections META (16 u64), DATA (f32, 16 KiB
// aligned), EXT (u64 ids), DEAD (tombstone bytes) and, for hnsw, LVL (u8 levels) and LINKS (each node's link block
// in slot order). The id map is not stored: load rebuilds it from EXT and DEAD.
// Round trip, for every metric x {flat, hnsw} at N = 0, 1, 100 and 2500 with tombstones and 32/64-bit ids, in copy
// and mmap mode: the loaded index answers every query with the same ids and distances, and saving it again writes
// the same bytes (lineage is fixed and the generation is 1, so the file is a pure function of the index state).
// Continuation: adding the same batch to the original and to the loaded copy then saving both gives equal files
// (options and the layer-draw state survive). mmap: the file is never written through (bytes unchanged after
// add/remove on the mapped copy, and after the original path is replaced by a new save).
// Refusals: every truncation point, a flipped byte in every section, and forged records whose section and header
// checksums are re-sealed so the damage reaches the structural checks (EPERSIST_CORRUPT): meta ranges, sizes,
// tombstone flags, live count, reserved and duplicate ids, non-finite data, levels, link counts, out-of-range,
// self and wrong-layer links, the entry point, a flat file with graph data, a missing section and a wrong element
// size. verify: false with a flipped data byte loads or throws a coded Error. Argument and state errors throw.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_vec_persist");
import { VectorIndex } from "dyna:vec";
import { BloomFilter } from "dyna:structures";
import { makeTempDir, removeAll, readBytes, writeFile } from "dyna:file";
import { CRC32C } from "dyna:hash";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function throws(f, Ctor, re, w) {
    try { f(); ok(false, w, "no throw"); }
    catch (e) { ok(e instanceof Ctor && (!re || re.test(String(e.message))), w, e.name + ": " + e.message); }
}
function codeOf(fn) {
    try { const v = fn(); if (v && typeof v.close === "function") v.close(); return "loaded"; }
    catch (e) { return e && e.code ? e.code : "uncoded:" + String(e); }
}

function rng(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}
function data(n, dim, seed) {
    const r = rng(seed), x = new Float32Array(n * dim);
    for (let i = 0; i < x.length; i++) x[i] = r() * 2 - 1;
    return x;
}
function idsFor(n, seed) {
    const r = rng(seed), s = new Set();
    while (s.size < n) {
        const u = BigInt(Math.floor(r() * 4294967296));
        s.add(r() < 0.5 ? u : (u << 32n) | BigInt(Math.floor(r() * 4294967296)));
    }
    return BigUint64Array.from(s);
}
const bytesEq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const sameArr = (a, b) => a.length === b.length && a.every((v, i) => Object.is(v, b[i]));

const dir = makeTempDir("vec-persist-");
let fileNo = 0;
const tmp = (tag) => dir.join(tag + "-" + (fileNo++) + ".vec");

const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const u64 = (b, o) => BigInt(u32(b, o)) + (BigInt(u32(b, o + 4)) << 32n);
function layout(b) {
    const h = 64, ns = u32(b, h + 24), secs = {};
    for (let i = 0; i < ns; i++) {
        const e = h + 160 + 40 * i;
        secs[u32(b, e)] = { entry: e, kind: u32(b, e), elem: u32(b, e + 8), off: Number(u64(b, e + 16)), len: Number(u64(b, e + 24)) };
    }
    return { ns, fileSize: Number(u64(b, h + 40)), secs };
}
const META = 48, DATA = 49, EXT = 50, DEAD = 51, LVL = 52, LINKS = 53;
function reseal(b) {
    const L = layout(b), dv = new DataView(b.buffer, b.byteOffset, b.length);
    for (const s of Object.values(L.secs)) dv.setUint32(s.entry + 32, CRC32C(b.subarray(s.off, s.off + s.len)) >>> 0, true);
    dv.setUint32(64 + 128, 0, true);
    dv.setUint32(64 + 128, CRC32C(b.subarray(64, 64 + 160 + 40 * L.ns)) >>> 0, true);
    return b;
}
function forge(img, edit) {
    const b = Uint8Array.from(img), L = layout(b), dv = new DataView(b.buffer);
    edit(b, L, dv);
    return reseal(b);
}
const metaSet = (dv, L, i, v) => dv.setBigUint64(L.secs[META].off + 8 * i, BigInt(v), true);
const metaGet = (dv, L, i) => dv.getBigUint64(L.secs[META].off + 8 * i, true);

async function answers(ix, q, dims, k) {
    const out = [];
    for (let i = 0; i < q.length / dims; i++) {
        const r = await ix.search(q.subarray(i * dims, (i + 1) * dims), k);
        out.push(Array.from(r.ids).join() + "|" + Array.from(r.distances).join());
    }
    return out;
}

async function build(metric, index, n, seed, extra) {
    const dims = 6, ix = new VectorIndex(Object.assign({ dims, metric, index }, extra || {}));
    const ids = idsFor(n, seed), x = data(n, dims, seed + 1);
    if (n) await ix.add(ids, x);
    if (n > 3) ix.remove(ids.filter((_, i) => i % 4 === 1));
    if (n > 3) await ix.add(ids.filter((_, i) => i % 8 === 1), x.subarray(0, dims * ids.filter((_, i) => i % 8 === 1).length));
    return { ix, ids, dims };
}

await __kitS.section("round trip in copy and mmap mode", async () => {
    for (const metric of ["l2", "cosine", "ip"]) {
        for (const index of ["flat", "hnsw"]) {
            for (const n of [0, 1, 100, 2500]) {
                const t = metric + " " + index + " n=" + n + ": ";
                const { ix, ids, dims } = await build(metric, index, n, 1000 + n);
                const q = data(12, dims, 77), want = await answers(ix, q, dims, 7);
                const p = tmp("rt"), res = ix.save(p, { fsync: n === 100 });
                const img = readBytes(p);
                ok(res.path === p && res.bytes === img.length && res.written === img.length && res.incremental === false, t + "save result", JSON.stringify(res));
                ok(layout(img).fileSize === img.length, t + "logical size is the file size");
                for (const mmap of [false, true]) {
                    const m = t + (mmap ? "mmap: " : "copy: ");
                    const b = VectorIndex.load(p, { mmap });
                    ok(b instanceof VectorIndex && b.size === ix.size && b.dims === dims && b.metric === metric && b.index === index && !b.closed, m + "getters", b.size + " vs " + ix.size);
                    ok(ids.every((id) => b.has(id) === ix.has(id)), m + "has() agrees on every id, removed ones included");
                    ok(sameArr(await answers(b, q, dims, 7), want), m + "every query answers identically");
                    const p2 = tmp("rt2");
                    b.save(p2, { fsync: false });
                    ok(bytesEq(readBytes(p2), img), m + "save after load reproduces the file byte for byte");
                    b.close();
                }
                ix.close();
            }
        }
    }
});

await __kitS.section("a loaded index continues exactly like the original", async () => {
    for (const metric of ["l2", "cosine", "ip"]) {
        for (const index of ["flat", "hnsw"]) {
            const extra = index === "hnsw" ? { m: 6, efConstruction: 40, dyna: { efSearch: 20, seed: 9, heuristic: metric !== "l2", exactFallback: metric !== "ip" } } : undefined;
            const { ix, dims } = await build(metric, index, 600, 31, extra);
            const p = tmp("cont");
            ix.save(p, { fsync: false });
            const more = idsFor(400, 32).map((v) => v | 1n), mx = data(400, dims, 33);
            for (const mmap of [false, true]) {
                const t = metric + " " + index + (mmap ? " mmap: " : " copy: ");
                const a = VectorIndex.load(p), b = VectorIndex.load(p, { mmap });
                await a.add(more, mx);
                await b.add(more, mx);
                ix.remove(more.subarray(0, 0));
                const pa = tmp("ca"), pb = tmp("cb");
                a.save(pa, { fsync: false }); b.save(pb, { fsync: false });
                ok(bytesEq(readBytes(pa), readBytes(pb)), t + "the same adds after load write the same file");
                const q = data(10, dims, 34);
                ok(sameArr(await answers(a, q, dims, 10), await answers(b, q, dims, 10)), t + "and answer identically");
                a.close(); b.close();
            }
            const o = VectorIndex.load(p);
            await ix.add(more, mx);
            await o.add(more, mx);
            const po = tmp("co"), pi = tmp("ci");
            o.save(po, { fsync: false }); ix.save(pi, { fsync: false });
            ok(bytesEq(readBytes(po), readBytes(pi)), metric + " " + index + ": original and loaded copy stay in lockstep (options and layer draws persisted)");
            ix.close(); o.close();
        }
    }
});

await __kitS.section("mmap never writes through", async () => {
    const { ix, ids, dims } = await build("l2", "hnsw", 3000, 51);
    const p = tmp("mm");
    ix.save(p, { fsync: false });
    const img = readBytes(p);
    const m = VectorIndex.load(p, { mmap: true });
    const q = data(5, dims, 52), before = await answers(m, q, dims, 5);
    ok(m.remove(ids.subarray(0, 50)) > 0, "remove on the mapped copy");
    ok(bytesEq(readBytes(p), img), "the file is unchanged after remove on the mapped copy");
    await m.add(BigUint64Array.of(1n, 2n ** 50n), data(2, dims, 53));
    ok(m.has(1n) && m.has(2n ** 50n), "add on the mapped copy (copies the vectors out)");
    ok(bytesEq(readBytes(p), img), "the file is unchanged after add on the mapped copy");
    const m2 = VectorIndex.load(p, { mmap: true });
    ix.remove(ids.subarray(100, 200));
    ix.save(p, { fsync: false });
    ok(sameArr(await answers(m2, q, dims, 5), before), "a mapping survives a new save to the same path");
    ok(!bytesEq(readBytes(p), img) && VectorIndex.load(p).size === ix.size, "the path now holds the new save");
    m.close(); m2.close(); ix.close();
});

await __kitS.section("damaged and forged files are refused", async () => {
    const { ix, dims } = await build("cosine", "hnsw", 300, 61);
    const p = tmp("good"), bad = tmp("bad");
    ix.save(p, { fsync: false });
    const img = readBytes(p), L = layout(img);
    ok([META, DATA, EXT, DEAD, LVL, LINKS].every((k) => L.secs[k]), "hnsw file has the six sections");
    ok(L.secs[DATA].off % 16384 === 0, "vector data is 16 KiB aligned");
    ok(codeOf(() => VectorIndex.load(p)) === "loaded", "the untouched file loads");
    const both = (bytes, want, what, opts) => {
        writeFile(bad, bytes);
        for (const mmap of [false, true]) {
            const got = codeOf(() => VectorIndex.load(bad, Object.assign({ mmap }, opts || {})));
            ok(Array.isArray(want) ? want.includes(got) : got === want, what + (mmap ? " (mmap)" : ""), got);
        }
    };
    const cuts = new Set([0, 1, 63, 64, 100, 300, img.length - 1]);
    for (const s of Object.values(L.secs)) { cuts.add(s.off); cuts.add(s.off + (s.len >> 1)); }
    for (const c of [...cuts].filter((c) => c < img.length)) both(img.subarray(0, c), ["EPERSIST_TRUNCATED", "EPERSIST_MAGIC"], "truncated at " + c);
    for (const s of Object.values(L.secs)) {
        if (!s.len) continue;
        const b = Uint8Array.from(img);
        b[s.off + (s.len >> 1)] ^= 0x40;
        both(b, "EPERSIST_CHECKSUM", "flipped byte in section " + s.kind);
    }
    const flipped = Uint8Array.from(img);
    flipped[L.secs[DATA].off + 3] ^= 0x7f;
    both(flipped, ["loaded", "EPERSIST_CORRUPT"], "verify: false with a flipped data byte", { verify: false });
    const corrupt = (what, edit) => both(forge(img, edit), "EPERSIST_CORRUPT", "forged: " + what);
    corrupt("layout version 2", (b, L, dv) => metaSet(dv, L, 0, 2));
    corrupt("dims 0", (b, L, dv) => metaSet(dv, L, 1, 0));
    corrupt("dims 65537", (b, L, dv) => metaSet(dv, L, 1, 65537));
    corrupt("dims changed", (b, L, dv) => metaSet(dv, L, 1, dims + 1));
    corrupt("metric 3", (b, L, dv) => metaSet(dv, L, 2, 3));
    corrupt("kind 2", (b, L, dv) => metaSet(dv, L, 3, 2));
    corrupt("kind flat with graph data", (b, L, dv) => metaSet(dv, L, 3, 0));
    corrupt("m 1", (b, L, dv) => metaSet(dv, L, 4, 1));
    corrupt("m 257", (b, L, dv) => metaSet(dv, L, 4, 257));
    corrupt("m changed (link sizes no longer match)", (b, L, dv) => metaSet(dv, L, 4, 15));
    corrupt("efConstruction 0", (b, L, dv) => metaSet(dv, L, 5, 0));
    corrupt("efSearch 65537", (b, L, dv) => metaSet(dv, L, 6, 65537));
    corrupt("flags 4", (b, L, dv) => metaSet(dv, L, 7, 4));
    corrupt("quantization 1", (b, L, dv) => metaSet(dv, L, 8, 1));
    corrupt("n + 1", (b, L, dv) => metaSet(dv, L, 10, metaGet(dv, L, 10) + 1n));
    corrupt("n huge", (b, L, dv) => metaSet(dv, L, 10, 2n ** 40n));
    corrupt("live > n", (b, L, dv) => metaSet(dv, L, 11, metaGet(dv, L, 10) + 1n));
    corrupt("live - 1", (b, L, dv) => metaSet(dv, L, 11, metaGet(dv, L, 11) - 1n));
    corrupt("entry out of range", (b, L, dv) => metaSet(dv, L, 12, metaGet(dv, L, 10)));
    corrupt("entry above 2^32", (b, L, dv) => metaSet(dv, L, 12, 2n ** 33n));
    corrupt("top 31", (b, L, dv) => metaSet(dv, L, 13, 31));
    corrupt("top - 1", (b, L, dv) => metaSet(dv, L, 13, metaGet(dv, L, 13) - 1n));
    corrupt("link word total", (b, L, dv) => metaSet(dv, L, 14, metaGet(dv, L, 14) + 1n));
    corrupt("reserved word set", (b, L, dv) => metaSet(dv, L, 15, 1));
    const n = Number(metaGet(new DataView(img.buffer, img.byteOffset), L, 10));
    const deadAt = (b, L, want) => { for (let i = 0; i < n; i++) if (b[L.secs[DEAD].off + i] === want) return i; return -1; };
    corrupt("tombstone flag 2 on a live slot", (b, L) => { b[L.secs[DEAD].off + deadAt(b, L, 0)] = 2; });
    corrupt("tombstone flag 2 on a removed slot", (b, L) => { b[L.secs[DEAD].off + deadAt(b, L, 1)] = 2; });
    corrupt("tombstone set without live update", (b, L) => { b[L.secs[DEAD].off + deadAt(b, L, 0)] = 1; });
    corrupt("tombstone cleared without live update", (b, L) => { b[L.secs[DEAD].off + deadAt(b, L, 1)] = 0; });
    corrupt("live slot holds 2^64-1", (b, L, dv) => dv.setBigUint64(L.secs[EXT].off + 8 * deadAt(b, L, 0), 2n ** 64n - 1n, true));
    corrupt("duplicate live id", (b, L, dv) => {
        const i = deadAt(b, L, 0);
        let j = i + 1;
        while (b[L.secs[DEAD].off + j]) j++;
        dv.setBigUint64(L.secs[EXT].off + 8 * j, dv.getBigUint64(L.secs[EXT].off + 8 * i, true), true);
    });
    corrupt("NaN component", (b, L, dv) => dv.setFloat32(L.secs[DATA].off + 4 * 7, NaN, true));
    corrupt("Infinity component", (b, L, dv) => dv.setFloat32(L.secs[DATA].off + 4 * 11, -Infinity, true));
    const m = 16, block = (l) => 1 + 2 * m + l * (1 + m);
    const lv = (b, L, i) => b[L.secs[LVL].off + i];
    const blockOff = (b, L, i) => { let w = 0; for (let s = 0; s < i; s++) w += block(lv(b, L, s)); return L.secs[LINKS].off + 4 * w; };
    const firstAt = (b, L, f) => { for (let i = 0; i < n; i++) if (f(i)) return i; return -1; };
    corrupt("level 31", (b, L) => { b[L.secs[LVL].off] = 31; });
    corrupt("level raised (link sizes no longer match)", (b, L) => { b[L.secs[LVL].off + firstAt(b, L, (i) => lv(b, L, i) === 0)] = 1; });
    corrupt("level-0 link count 2m + 1", (b, L, dv) => dv.setUint32(blockOff(b, L, 0), 2 * m + 1, true));
    corrupt("link to slot n", (b, L, dv) => { const o = blockOff(b, L, 0); dv.setUint32(o, Math.max(1, dv.getUint32(o, true)), true); dv.setUint32(o + 4, n, true); });
    corrupt("self link", (b, L, dv) => { const o = blockOff(b, L, 5); dv.setUint32(o, Math.max(1, dv.getUint32(o, true)), true); dv.setUint32(o + 4, 5, true); });
    corrupt("link to a node below that layer", (b, L, dv) => {
        const hi = firstAt(b, L, (i) => lv(b, L, i) >= 1), lo = firstAt(b, L, (i) => lv(b, L, i) === 0 && i !== hi);
        const o = blockOff(b, L, hi) + 4 * (1 + 2 * m);
        dv.setUint32(o, Math.max(1, dv.getUint32(o, true)), true);
        dv.setUint32(o + 4, lo, true);
    });
    corrupt("entry is not a top-level node", (b, L, dv) => metaSet(dv, L, 12, firstAt(b, L, (i) => lv(b, L, i) < Number(metaGet(dv, L, 13)))));
    corrupt("entry and top below the highest level", (b, L, dv) => {
        const top = Number(metaGet(dv, L, 13)), e = firstAt(b, L, (i) => lv(b, L, i) === top - 1);
        metaSet(dv, L, 12, e); metaSet(dv, L, 13, top - 1);
    });
    corrupt("a section is missing", (b, L, dv) => dv.setUint32(L.secs[EXT].entry, 60, true));
    corrupt("graph section missing", (b, L, dv) => dv.setUint32(L.secs[LINKS].entry, 61, true));
    corrupt("wrong element size", (b, L, dv) => dv.setUint32(L.secs[DATA].entry + 8, 8, true));
    corrupt("meta wrong size", (b, L, dv) => dv.setBigUint64(L.secs[META].entry + 24, 120n, true));
    const fl = (await build("l2", "flat", 50, 62)).ix, pf = tmp("flat");
    fl.save(pf, { fsync: false });
    const fimg = readBytes(pf), FL = layout(fimg);
    ok(!FL.secs[LVL] && !FL.secs[LINKS], "a flat file has no graph sections");
    both(forge(fimg, (b, L, dv) => metaSet(dv, L, 12, 0)), "EPERSIST_CORRUPT", "forged: flat with an entry point");
    both(forge(fimg, (b, L, dv) => metaSet(dv, L, 14, 1)), "EPERSIST_CORRUPT", "forged: flat with link words");
    both(forge(fimg, (b, L, dv) => metaSet(dv, L, 3, 1)), "EPERSIST_CORRUPT", "forged: hnsw without graph sections");
    fl.close();
    const bl = new BloomFilter(1000, 0.01), pb = tmp("bloom");
    bl.save(pb, { fsync: false });
    let e = null;
    try { VectorIndex.load(pb); } catch (x) { e = x; }
    ok(e && e.code === "EPERSIST_TYPE" && /not a VectorIndex/.test(e.message), "a BloomFilter file is EPERSIST_TYPE", e && e.message);
    e = null;
    try { BloomFilter.load(p); } catch (x) { e = x; }
    ok(e && e.code === "EPERSIST_TYPE", "BloomFilter.load of a VectorIndex file is EPERSIST_TYPE", e && e.message);
    e = null;
    try { VectorIndex.load(dir.join("missing.vec")); } catch (x) { e = x; }
    ok(e && e.code === "EPERSIST_IO" && typeof e.errno === "number" && /VectorIndex\.load\(/.test(e.message), "a missing file is EPERSIST_IO with errno", e && e.message);
    ix.close();
});

await __kitS.section("argument and state errors", async () => {
    const ix = new VectorIndex({ dims: 2 });
    await ix.add(Uint32Array.of(1), Float32Array.of(1, 2));
    const p = tmp("args");
    throws(() => ix.save("plain-string.vec"), TypeError, /Path/, "save with a string path");
    throws(() => ix.save(), TypeError, /Path/, "save with no path");
    throws(() => ix.save(p, { fsync: 1 }), TypeError, /fsync must be a boolean/, "save fsync not boolean");
    throws(() => ix.save(p, { incremental: "yes" }), TypeError, /incremental must be a boolean/, "save incremental not boolean");
    throws(() => ix.save(p, 5), TypeError, /options must be an object/, "save options not an object");
    ok(ix.save(p, { incremental: true, fsync: false }).incremental === false, "incremental: true writes a full save");
    throws(() => VectorIndex.load("x.vec"), TypeError, /Path/, "load with a string path");
    throws(() => VectorIndex.load(p, { mmap: "true" }), TypeError, /mmap must be a boolean/, "load mmap not boolean");
    throws(() => VectorIndex.load(p, { verify: 0 }), TypeError, /verify must be a boolean/, "load verify not boolean");
    ok(VectorIndex.load(p, null).size === 1 && VectorIndex.load(p, {}).size === 1, "load with null or empty options");
    const big = new VectorIndex({ dims: 32 }), bigIds = Uint32Array.from({ length: 3000 }, (_, i) => i);
    const pending = big.add(bigIds, data(3000, 32, 91));
    ok(big.size === 0, "a large add is pending");
    throws(() => big.save(p), TypeError, /has not settled/, "save while an add is unsettled");
    await pending;
    const pb = tmp("big");
    ok(big.save(pb, { fsync: false }).bytes > 3000 * 32 * 4 && VectorIndex.load(pb).size === 3000, "save after the add settles holds it");
    big.close();
    await ix.add(Uint32Array.of(2, 3), Float32Array.of(3, 4, 5, 6));
    ok(ix.save(p, { fsync: false }).bytes > 0, "save after the adds settle");
    ix.close();
    throws(() => ix.save(p), TypeError, null, "save after close");
    throws(() => VectorIndex.prototype.save.call({}, p), TypeError, null, "save on a foreign receiver");
    ok(typeof VectorIndex.load === "function" && VectorIndex.load.length === 1 && !Object.keys(VectorIndex).includes("load"), "static load is a non-enumerable function");
    const l = VectorIndex.load(p);
    ok(l.size === 3 && l.has(3), "the last save holds every add");
    l.close();
    ok(l.closed && (() => { try { l.has(1); return false; } catch (x) { return x instanceof TypeError; } })(), "a loaded index closes like any other");
});

removeAll(dir);
__kitS.end();
print(`test_vec_persist: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_vec_persist: " + fail + " failing checks");
