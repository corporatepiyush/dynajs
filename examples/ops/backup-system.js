// Backup system — content-defined chunking, deduplication, compression and authenticated encryption into a
// repository, with snapshots, incremental runs, a grandfather-father-son retention policy, garbage collection,
// integrity scrubbing and verified restores (the restic / borg design).
//
// WHAT IT IS
//   chunking     files are cut at content-defined boundaries with a gear rolling hash (FastCDC-style, 2–16 KiB,
//                ~8 KiB average), so an insertion in the middle of a file shifts only the chunks around it
//   dedup        each chunk is identified by a KEYED BLAKE3 hash (the key comes from the repository secret, so the
//                storage provider cannot confirm a guessed file); a chunk already stored is never stored again
//   packing      new chunks are compressed (zstd), encrypted (AEAD, the chunk id as associated data) and appended to
//                pack files; an index maps chunk id -> pack, offset, length
//   keys         the repository master key is wrapped by a key derived from the passphrase with scrypt; a wrong
//                passphrase is detected before anything is decrypted
//   snapshots    a snapshot is an encrypted tree: paths, modes, sizes, mtimes and the chunk list of each file
//   retention    keep the last 3, 7 daily, 4 weekly, 6 monthly; everything else is forgotten, then prune removes
//                chunks no surviving snapshot references and rewrites packs that became mostly garbage
//   check        a scrub decrypts every chunk and verifies its id; a flipped bit in a pack is found and reported
//   restore      any snapshot restores to a directory and every file is verified against its recorded hash
//   liveness     chunk ids are interned to dense integers; the set of chunks reachable from surviving snapshots
//                is a RoaringBitmap, so prune's "is this chunk live" and check's "is this reference present" are
//                bitmap tests, and the dead set is one andNot; byte assembly (nonce || ciphertext, packs, restored
//                files) is dyna:bytes concat, never a spread of every byte into a JS array
//
// MODULES  dyna:file · dyna:hash (BLAKE3 keyed, CDCChunks) · dyna:compress (zstd) · dyna:crypto (Scrypt, AEAD) · dyna:serialize
//          (CBOR) · dyna:time · dyna:random · dyna:cli (Table) · dyna:encoding · dyna:bytes (concat)
//          · dyna:structures (Interner, RoaringBitmap)
//
// RUN      dynajs examples/ops/backup-system.js     backs up a changing project directory over 70 simulated days

import { makeTempDir, makeDir, writeFile, readBytes, readDir, stat, exists, remove, removeAll } from "dyna:file";
import { BLAKE3, BLAKE3Hex, CDCChunks } from "dyna:hash";
import { zstd, unzstd } from "dyna:compress";
import * as crypto from "dyna:crypto";
import { CBOREncode, CBORDecode } from "dyna:serialize";
import { formatRFC3339 } from "dyna:time";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { HexEncode } from "dyna:encoding";
import { concat } from "dyna:bytes";
import { Interner, RoaringBitmap } from "dyna:structures";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("backup");
const AEAD = typeof crypto.ChaCha20Poly1305 === "function" ? crypto.ChaCha20Poly1305 : crypto.AESGCM;
const rng = new Random(70);

// =============================================================================
// Content-defined chunking (gear hash)
// =============================================================================
// hash.CDCChunks runs the gear-hash scan in C and returns the chunk end offsets; slicing stays zero-copy.
const MIN = 2048,
    AVG = 8192,
    MAX = 16384;
function chunk(data) {
    const out = [];
    let start = 0;
    for (const end of CDCChunks(data, { min: MIN, avg: AVG, max: MAX })) {
        out.push(data.subarray(start, end));
        start = end;
    }
    return out;
}

// =============================================================================
// Repository
// =============================================================================
class Repository {
    static init(dir, passphrase) {
        makeDir(dir.join("packs"), { recursive: true });
        makeDir(dir.join("snapshots"), { recursive: true });
        const master = crypto.RandomBytes(64),
            salt = crypto.RandomBytes(16);
        const kek = crypto.Scrypt(passphrase, salt, { N: 1 << 14, r: 8, p: 1, keyLen: 32 });
        const a = new AEAD(kek),
            nonce = crypto.RandomBytes(12);
        writeFile(
            dir.join("config"),
            CBOREncode({
                version: 2,
                salt,
                nonce,
                wrapped: a.seal(nonce, master, "repo-master-key"),
                chunker: { MIN, AVG, MAX },
            }),
        );
        return new Repository(dir, passphrase);
    }
    constructor(dir, passphrase) {
        this.dir = dir;
        const cfg = CBORDecode(readBytes(dir.join("config")));
        const kek = crypto.Scrypt(passphrase, cfg.salt, { N: 1 << 14, r: 8, p: 1, keyLen: 32 });
        let master;
        try {
            master = new AEAD(kek).open(cfg.nonce, cfg.wrapped, "repo-master-key");
        } catch {
            throw new Error("wrong passphrase");
        }
        this.idKey = master.slice(0, 32);
        this.encKey = master.slice(32);
        this.index = new Map();
        this.pack = null;
        this.packN = 0;
        this.ids = new Interner();
        this.stats = { chunksNew: 0, chunksDup: 0, bytesIn: 0, bytesStored: 0, unchanged: 0 };
        this.parent = new Map();
        if (exists(dir.join("index")))
            for (const [id, loc] of Object.entries(this.decrypt(readBytes(dir.join("index")), "index")))
                this.index.set(id, loc);
        this.packN = readDir(dir.join("packs")).length;
    }
    encrypt(obj, ad) {
        const a = new AEAD(this.encKey),
            n = crypto.RandomBytes(12);
        return concat([n, a.seal(n, zstd(CBOREncode(obj)), ad)]);
    }
    decrypt(bytes, ad) {
        const a = new AEAD(this.encKey);
        return CBORDecode(unzstd(a.open(bytes.subarray(0, 12), bytes.subarray(12), ad)));
    }
    chunkId(data) {
        return BLAKE3Hex(data, { key: this.idKey });
    }
    putChunk(data) {
        const id = this.chunkId(data);
        this.stats.bytesIn += data.length;
        if (this.index.has(id)) {
            this.stats.chunksDup++;
            return id;
        }
        const a = new AEAD(this.encKey),
            nonce = crypto.RandomBytes(12);
        const blob = concat([nonce, a.seal(nonce, zstd(data, { level: 3 }), id)]);
        if (!this.pack || this.pack.size > 512 * 1024)
            (this.flushPack(),
                (this.pack = { name: `pack-${String(this.packN++).padStart(5, "0")}`, parts: [], size: 0 }));
        this.index.set(id, { pack: this.pack.name, offset: this.pack.size, length: blob.length, raw: data.length });
        this.pack.parts.push(blob);
        this.pack.size += blob.length;
        this.stats.chunksNew++;
        this.stats.bytesStored += blob.length;
        return id;
    }
    flushPack() {
        if (!this.pack || !this.pack.parts.length) return;
        writeFile(this.dir.join("packs", this.pack.name), concat(this.pack.parts));
        this.pack = null;
    }
    getChunk(id) {
        const loc = this.index.get(id);
        if (!loc) throw new Error(`missing chunk ${id.slice(0, 12)}`);
        const bytes = readBytes(this.dir.join("packs", loc.pack)).subarray(loc.offset, loc.offset + loc.length);
        let data;
        try {
            data = unzstd(new AEAD(this.encKey).open(bytes.subarray(0, 12), bytes.subarray(12), id));
        } catch {
            throw new Error(`chunk ${id.slice(0, 12)} in ${loc.pack} failed authentication (corrupt or tampered)`);
        }
        if (this.chunkId(data) !== id) throw new Error(`chunk ${id.slice(0, 12)} content does not match its id`);
        return data;
    }
    commit() {
        this.flushPack();
        writeFile(this.dir.join("index"), this.encrypt(Object.fromEntries(this.index), "index"));
    }
    backup(src, time, tags = []) {
        const files = [];
        (function walk(dir, rel, repo) {
            for (const e of readDir(dir).sort((a, b) => a.name.localeCompare(b.name))) {
                const r = rel ? `${rel}/${e.name}` : e.name;
                if (e.isDir) {
                    walk(dir.join(e.name), r, repo);
                    continue;
                }
                const st = stat(dir.join(e.name)),
                    prev = repo.parent.get(r);
                if (prev && prev.size === st.size && prev.mtimeMs === st.mtimeMs) {
                    files.push(prev);
                    repo.stats.unchanged++;
                    continue;
                } // like restic's parent snapshot
                const data = readBytes(dir.join(e.name));
                files.push({
                    path: r,
                    size: data.length,
                    mode: st.mode & 0o777,
                    mtimeMs: st.mtimeMs,
                    hash: BLAKE3Hex(data),
                    chunks: chunk(data).map((c) => repo.putChunk(c)),
                });
            }
        })(src, "", this);
        this.parent = new Map(files.map((f) => [f.path, f]));
        const snap = { time, tags, files, host: "build-01" };
        const id = BLAKE3Hex(CBOREncode(snap)).slice(0, 16);
        writeFile(this.dir.join("snapshots", id), this.encrypt(snap, `snapshot:${id}`));
        this.commit();
        return { id, files: files.length, bytes: files.reduce((s, f) => s + f.size, 0) };
    }
    snapshots() {
        return readDir(this.dir.join("snapshots"))
            .map((e) => ({
                id: e.name,
                ...this.decrypt(readBytes(this.dir.join("snapshots", e.name)), `snapshot:${e.name}`),
            }))
            .sort((a, b) => a.time - b.time);
    }
    restore(id, target) {
        const snap = this.decrypt(readBytes(this.dir.join("snapshots", id)), `snapshot:${id}`);
        let verified = 0;
        for (const f of snap.files) {
            const data = concat(f.chunks.map((c) => this.getChunk(c)));
            if (BLAKE3Hex(data) !== f.hash) throw new Error(`${f.path}: restored content does not match`);
            const p = target.join(f.path);
            makeDir(p.dirname, { recursive: true });
            writeFile(p, data);
            verified++;
        }
        return verified;
    }
    forget(policy, now) {
        const snaps = this.snapshots(),
            keep = new Map();
        const bucket = (t, kind) => {
            const d = new Date(t);
            if (kind === "daily") return d.toISOString().slice(0, 10);
            if (kind === "monthly") return d.toISOString().slice(0, 7);
            const thursday = new Date(
                Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate() + 3 - ((d.getUTCDay() + 6) % 7)),
            );
            return `${thursday.getUTCFullYear()}-W${Math.ceil(((thursday - Date.UTC(thursday.getUTCFullYear(), 0, 1)) / 86400_000 + 1) / 7)}`;
        };
        const newest = [...snaps].reverse();
        newest.slice(0, policy.last).forEach((s) => keep.set(s.id, "last"));
        for (const kind of ["daily", "weekly", "monthly"]) {
            const seen = new Set();
            for (const s of newest) {
                const b = bucket(s.time, kind);
                if (seen.has(b)) continue;
                if (seen.size >= policy[kind]) break;
                seen.add(b);
                if (!keep.has(s.id)) keep.set(s.id, kind);
            }
        }
        const removed = snaps.filter((s) => !keep.has(s.id));
        for (const s of removed) remove(this.dir.join("snapshots", s.id));
        return { kept: [...keep], removed: removed.length };
    }
    liveSet() {
        // chunk ids reachable from any snapshot
        const live = new RoaringBitmap();
        for (const s of this.snapshots()) for (const f of s.files) live.addMany(this.ids.internMany(f.chunks));
        return live;
    }
    prune() {
        const liveBits = this.liveSet(),
            indexed = new RoaringBitmap(this.ids.internMany([...this.index.keys()]));
        const dead = indexed.clone().andNot(liveBits);
        const live = { has: (id) => liveBits.has(this.ids.lookup(id)), size: liveBits.size };
        const byPack = new Map();
        for (const [id, loc] of this.index) {
            if (!byPack.has(loc.pack)) byPack.set(loc.pack, { live: 0, dead: 0, ids: [] });
            const p = byPack.get(loc.pack);
            if (live.has(id)) {
                p.live += loc.length;
                p.ids.push(id);
            } else p.dead += loc.length;
        }
        let rewritten = 0,
            deleted = 0,
            freed = 0;
        for (const [name, p] of byPack) {
            if (p.dead === 0) continue;
            const old = readBytes(this.dir.join("packs", name));
            if (p.live === 0) {
                remove(this.dir.join("packs", name));
                deleted++;
                freed += old.length;
            } else if (p.dead / (p.live + p.dead) > 0.3) {
                // repack: copy live blobs to a new pack
                for (const id of p.ids) {
                    const loc = this.index.get(id),
                        blob = old.slice(loc.offset, loc.offset + loc.length);
                    if (!this.pack || this.pack.size > 512 * 1024)
                        (this.flushPack(),
                            (this.pack = {
                                name: `pack-${String(this.packN++).padStart(5, "0")}`,
                                parts: [],
                                size: 0,
                            }));
                    this.index.set(id, { ...loc, pack: this.pack.name, offset: this.pack.size });
                    this.pack.parts.push(blob);
                    this.pack.size += blob.length;
                }
                this.flushPack();
                remove(this.dir.join("packs", name));
                rewritten++;
                freed += p.dead;
            }
        }
        for (const id of this.ids.getMany(dead.toArray())) this.index.delete(id);
        this.commit();
        return { rewritten, deleted, freed, liveChunks: live.size, deadChunks: dead.size };
    }
    check() {
        const problems = [];
        for (const [id] of this.index) {
            try {
                this.getChunk(id);
            } catch (e) {
                problems.push(e.message);
            }
        }
        const missing = this.liveSet().andNot(new RoaringBitmap(this.ids.internMany([...this.index.keys()])));
        for (const id of this.ids.getMany(missing.toArray()))
            problems.push(`chunk ${id.slice(0, 12)} is referenced but not in the index`);
        return problems;
    }
    size() {
        return readDir(this.dir.join("packs")).reduce((s, e) => s + stat(this.dir.join("packs", e.name)).size, 0);
    }
}

// =============================================================================
// A project directory that changes every day
// =============================================================================
const src = work.join("project"),
    repoDir = work.join("repo");
const words =
    "the quick brown fox jumps over a lazy dog while engineers ship code reviews tests docs and release notes".split(
        " ",
    );
const text = (n) => Array.from({ length: n }, () => words[rng.nextBounded(words.length)]).join(" ");
function seed() {
    makeDir(src.join("src"), { recursive: true });
    makeDir(src.join("assets"), { recursive: true });
    makeDir(src.join("data"), { recursive: true });
    for (let i = 0; i < 25; i++)
        writeFile(src.join("src", `module${i}.js`), `// module ${i}\n` + text(800 + rng.nextBounded(1500)));
    for (let i = 0; i < 4; i++)
        writeFile(src.join("assets", `image${i}.bin`), rng.bytes(60_000 + rng.nextBounded(60_000)));
    writeFile(src.join("data", "events.log"), text(8000));
}
function mutate(day) {
    for (let k = 0; k < 3; k++) {
        // edit a few modules in the middle
        const f = src.join("src", `module${rng.nextBounded(25)}.js`),
            d = new TextDecoder().decode(readBytes(f)),
            at = rng.nextBounded(d.length);
        writeFile(f, d.slice(0, at) + ` /* day ${day} change */ ` + d.slice(at));
    }
    writeFile(src.join("data", "events.log"), text(300), { append: true }); // an append-only log grows
    if (day % 10 === 0) writeFile(src.join("assets", `image${day}.bin`), rng.bytes(50_000));
    if (day === 30) remove(src.join("assets", "image0.bin"));
}

const results = {};
seed();
const repo = Repository.init(repoDir, "correct horse battery staple");
const T0 = Date.UTC(2026, 0, 1, 2);
const runs = [];
for (let day = 0; day < 70; day++) {
    if (day) mutate(day);
    const before = { ...repo.stats };
    const r = repo.backup(src, T0 + day * 86400_000, day % 7 === 6 ? ["weekly"] : []);
    runs.push({
        day,
        ...r,
        newBytes: repo.stats.bytesStored - before.bytesStored,
        newChunks: repo.stats.chunksNew - before.chunksNew,
    });
}
const snapsBefore = repo.snapshots().length,
    sizeBefore = repo.size();

// Chunk stability: inserting bytes in the middle of a file changes only nearby chunks.
const sample = rng.bytes(200_000),
    shifted = new Uint8Array([...sample.subarray(0, 100_000), 1, 2, 3, ...sample.subarray(100_000)]);
const ca = new Set(chunk(sample).map((c) => BLAKE3Hex(c))),
    cb = chunk(shifted).map((c) => BLAKE3Hex(c));
const changedChunks = cb.filter((h) => !ca.has(h)).length;
results[`an insertion changed ${changedChunks} of ${cb.length} chunks`] = changedChunks <= 3 && cb.length > 15;
results[
    `incremental runs store a fraction of the first (${(runs[1].newBytes / 1024).toFixed(0)} KiB vs ${(runs[0].newBytes / 1024).toFixed(0)} KiB)`
] = runs.slice(1).every((r) => r.newBytes < runs[0].newBytes * 0.25);
let wrong = null;
try {
    new Repository(repoDir, "Tr0ub4dor&3");
} catch (e) {
    wrong = e.message;
}
results["a wrong passphrase is detected before any data is read"] = wrong === "wrong passphrase";
results["pack files contain no plaintext"] = !readDir(repoDir.join("packs")).some((e) =>
    new TextDecoder().decode(readBytes(repoDir.join("packs", e.name))).includes("module 3"),
);

const restored = work.join("restore-latest");
const latest = repo.snapshots().at(-1);
results["the latest snapshot restores byte-identical"] =
    repo.restore(latest.id, restored) === latest.files.length &&
    HexEncode(BLAKE3(readBytes(restored.join("data/events.log")))) ===
        HexEncode(BLAKE3(readBytes(src.join("data/events.log"))));

const forget = repo.forget({ last: 3, daily: 7, weekly: 4, monthly: 6 }, T0 + 70 * 86400_000);
const pruned = repo.prune();
const sizeAfter = repo.size();
const kept = repo.snapshots();
results[`retention kept ${kept.length} of ${snapsBefore} snapshots`] =
    kept.length === forget.kept.length && kept.length >= 10 && kept.length <= 17;
results["the monthly tier keeps a January snapshot"] =
    kept.some((s) => s.time === T0) || kept.some((s) => new Date(s.time).getUTCMonth() === 0);
results[
    `prune freed ${(pruned.freed / 1024).toFixed(0)} KiB (${pruned.deleted} packs deleted, ${pruned.rewritten} rewritten)`
] = sizeAfter < sizeBefore && pruned.freed > 0;
results["a scrub after prune finds no problems"] = repo.check().length === 0;
results[`prune dropped exactly the ${pruned.deadChunks} unreachable chunks from the index`] =
    pruned.deadChunks > 0 && repo.index.size === pruned.liveChunks;
const oldest = kept[0],
    oldRestore = work.join("restore-oldest");
results[`the oldest kept snapshot (${formatRFC3339(oldest.time / 1000).slice(0, 10)}) still restores fully`] =
    repo.restore(oldest.id, oldRestore) === oldest.files.length;
results["forgotten snapshots are gone from the repository"] =
    forget.removed === snapsBefore - kept.length && readDir(repoDir.join("snapshots")).length === kept.length;

// Bit rot: flip one byte in a pack; the scrub names the chunk and the restore refuses the damaged file.
const packs = readDir(repoDir.join("packs"))
    .map((e) => e.name)
    .sort();
const victim = repoDir.join("packs", packs[0]),
    bytes = readBytes(victim);
bytes[Math.floor(bytes.length / 2)] ^= 0x40;
writeFile(victim, bytes);
const problems = repo.check();
results[`scrub detected corruption: ${problems[0]}`] = problems.length >= 1 && /chunk/.test(problems[0]);

print("backup-system");
print(`  repository ${AEAD.name} + zstd, keyed BLAKE3 ids, gear CDC ${MIN / 1024}-${MAX / 1024} KiB`);
print(
    table([
        ["day", "files", "source", "new chunks", "stored"],
        ...runs
            .filter((r) => [0, 1, 2, 10, 30, 69].includes(r.day))
            .map((r) => [
                String(r.day),
                String(r.files),
                `${(r.bytes / 1024).toFixed(0)} KiB`,
                String(r.newChunks),
                `${(r.newBytes / 1024).toFixed(1)} KiB`,
            ]),
    ]),
);
print(
    `  dedup      ${repo.stats.unchanged} unchanged files skipped by size+mtime, ${repo.stats.chunksDup} duplicate chunks avoided; ${(repo.stats.bytesIn / 1024 / 1024).toFixed(1)} MiB backed up into ${(sizeBefore / 1024 / 1024).toFixed(2)} MiB`,
);
print(
    `  retention  ${snapsBefore} -> ${kept.length} snapshots (${[...new Set(forget.kept.map(([, why]) => why))].join(", ")}); repo ${(sizeBefore / 1024).toFixed(0)} -> ${(sizeAfter / 1024).toFixed(0)} KiB`,
);
print(`  scrub      ${problems.length} problem(s) after injected bit rot: ${problems[0]}`);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
