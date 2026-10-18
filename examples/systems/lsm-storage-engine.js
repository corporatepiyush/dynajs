// LSM storage engine — a write-ahead log with group commit, a memtable, immutable SSTables with prefix-compressed
// blocks, per-table Bloom filters, leveled compaction, tombstones, MVCC snapshots, range scans through a k-way
// merge, crash recovery, and a YCSB-style benchmark (the LevelDB/RocksDB design).
//
// WHAT IT IS
//   WAL          every write batch is appended as [len][crc32c][seq][ops…] and the file is synced once per
//                batch group (group commit: concurrent writers share one fsync)
//   memtable     a sorted array of (key, seq, value|tombstone) with binary-search inserts; at 256 KiB it is
//                frozen and flushed to a level-0 SSTable while a new one takes writes
//   SSTables     data blocks of ~4 KiB whose keys are prefix-compressed against the previous key (restart points
//                every 16 keys), compressed with LZ4, an index block of last-key -> offset, a Bloom filter over
//                the keys, and a footer; a point read checks the filter, binary-searches the index, then one block
//   compaction   L0 tables overlap each other; when L0 has 4 tables they merge into L1, and each level is kept under
//                10x the size of the one above by merging a table into the overlapping range of the next level;
//                tombstones are dropped only at the bottom level, and versions older than the oldest snapshot are
//                discarded
//   reads        get() searches memtable -> immutable memtable -> L0 newest first -> L1.. by range; a snapshot
//                pins a sequence number so later writes are invisible to it
//   scans        a heap merges memtable and every table's iterator, newest version wins, tombstones hide keys
//   recovery     on open the WAL is replayed (a torn final record is detected by its CRC and ignored) and the
//                manifest lists the live tables
//
//   natives      the memtable is an OrderedMap keyed by key + inverted sequence number, so versions sort newest first
//                inside a key and a snapshot read is one rangeQuery with limit 1 (RocksDB's skiplist, minus the
//                O(n) array splice per insert); each SSTable's Bloom filter is a native BloomFilter whose serialized
//                record is stored in the footer; decoded blocks sit in an LRU; byte assembly is dyna:bytes concat
//
// MODULES  dyna:file · dyna:hash (CRC32C, XXH3) · dyna:compress (lz4) · dyna:encoding (varints) · dyna:structures
//          (Heap, OrderedMap, BloomFilter, LRU) · dyna:serialize (MsgPack) · dyna:bytes (concat) · dyna:random
//          · dyna:cli (Table)
//
// RUN      dynajs examples/systems/lsm-storage-engine.js     loads 60,000 records, runs a mixed workload, crashes and recovers

import { makeTempDir, writeFile, readBytes, readFile, exists, remove, readDir, stat, removeAll } from "dyna:file";
import { CRC32C, XXH3_64 } from "dyna:hash";
import { lz4Compress, lz4Decompress } from "dyna:compress";
import { PutUvarint, Uvarint } from "dyna:encoding";
import { Heap, OrderedMap, BloomFilter, LRU } from "dyna:structures";
import { concat } from "dyna:bytes";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("lsm");
const enc = new TextEncoder(),
    dec = new TextDecoder();
const MEMTABLE_BYTES = 256 * 1024,
    BLOCK_BYTES = 4096,
    RESTART = 16,
    L0_TRIGGER = 4,
    LEVEL_BASE = 1 << 20;
const cmpKey = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

// =============================================================================
// Bloom filter (double hashing over one 64-bit hash)
// =============================================================================
const newBloom = (n, bitsPerKey = 10) =>
    new BloomFilter(Math.max(64, n * bitsPerKey), Math.max(1, Math.round(bitsPerKey * 0.69)));
// The memtable's composite key: user key, a separator below every key byte, then the INVERTED sequence number,
// zero-padded so that string order is key ascending, then newest version first.
const MAXSEQ = 2 ** 48;
const memKey = (key, seq) => `${key}\u0001${String(MAXSEQ - seq).padStart(15, "0")}`;

// =============================================================================
// SSTable writer and reader
// =============================================================================
function writeTable(path, entries) {
    // entries sorted by key, then seq desc
    const blocks = [],
        index = [],
        bloom = newBloom(entries.length);
    let cur = [],
        curBytes = 0,
        prevKey = "";
    const flushBlock = () => {
        if (!cur.length) return;
        const raw = concat(cur.flat()),
            packed = lz4Compress(raw);
        index.push({ last: prevKey, offset: blocks.reduce((s, b) => s + b.length, 0), length: packed.length });
        blocks.push(packed);
        cur = [];
        curBytes = 0;
    };
    entries.forEach((e, i) => {
        const restart = cur.length % RESTART === 0;
        let shared = 0;
        if (!restart)
            while (shared < prevKey.length && shared < e.key.length && prevKey[shared] === e.key[shared]) shared++;
        const suffix = enc.encode(e.key.slice(shared)),
            val = e.value === null ? null : enc.encode(e.value);
        const rec = [
            PutUvarint(shared),
            PutUvarint(suffix.length),
            suffix,
            PutUvarint(e.seq),
            PutUvarint(val === null ? 0 : val.length + 1),
            val || new Uint8Array(0),
        ];
        cur.push(rec);
        curBytes += rec.reduce((s, p) => s + p.length, 0);
        bloom.add(e.key);
        prevKey = e.key;
        if (curBytes >= BLOCK_BYTES || i === entries.length - 1) flushBlock();
    });
    const meta = MsgPackEncode({
        index,
        bloom: bloom.serialize(),
        count: entries.length,
        first: entries[0].key,
        last: entries.at(-1).key,
    });
    const footer = new Uint8Array(8);
    new DataView(footer.buffer).setUint32(0, meta.length);
    new DataView(footer.buffer).setUint32(4, CRC32C(meta));
    const file = concat([...blocks, meta, footer]);
    writeFile(path, file);
    return {
        path: path.toString(),
        size: file.length,
        first: entries[0].key,
        last: entries.at(-1).key,
        count: entries.length,
    };
}
class TableReader {
    constructor(info) {
        Object.assign(this, info);
        const bytes = readBytes(info.path);
        const dv = new DataView(bytes.buffer, bytes.byteOffset + bytes.length - 8),
            len = dv.getUint32(0),
            crc = dv.getUint32(4);
        const meta = bytes.subarray(bytes.length - 8 - len, bytes.length - 8);
        if (CRC32C(meta) !== crc) throw new Error(`${info.path}: footer checksum mismatch`);
        const m = MsgPackDecode(meta);
        this.bytes = bytes;
        this.index = m.index;
        this.bloom = BloomFilter.deserialize(m.bloom);
        this.blockCache = new LRU(64);
        this.stats = { bloomSkips: 0, blockReads: 0 };
    }
    block(i) {
        const hit = this.blockCache.get(String(i));
        if (hit) return hit;
        this.stats.blockReads++;
        const b = this.index[i],
            raw = lz4Decompress(this.bytes.subarray(b.offset, b.offset + b.length));
        const out = [];
        let off = 0,
            prev = "";
        const u = () => {
            const [v, n] = Uvarint(raw.subarray(off));
            off += n;
            return Number(v);
        };
        while (off < raw.length) {
            const shared = u(),
                sl = u(),
                key = prev.slice(0, shared) + dec.decode(raw.subarray(off, off + sl));
            off += sl;
            const seq = u(),
                vl = u(),
                value = vl === 0 ? null : dec.decode(raw.subarray(off, off + vl - 1));
            off += Math.max(0, vl - 1);
            out.push({ key, seq, value });
            prev = key;
        }
        this.blockCache.put(String(i), out);
        return out;
    }
    get(key, maxSeq) {
        if (key < this.first || key > this.last) return undefined;
        if (!this.bloom.mayContain(key)) {
            this.stats.bloomSkips++;
            return undefined;
        }
        let lo = 0,
            hi = this.index.length - 1;
        while (lo < hi) {
            const mid = (lo + hi) >> 1;
            if (this.index[mid].last < key) lo = mid + 1;
            else hi = mid;
        }
        for (let b = lo; b < this.index.length; b++) {
            for (const e of this.block(b)) {
                if (e.key === key && e.seq <= maxSeq) return e;
                if (e.key > key) return undefined;
            }
        }
        return undefined;
    }
    *iterate(from = "") {
        for (let b = 0; b < this.index.length; b++) {
            if (this.index[b].last < from) continue;
            for (const e of this.block(b)) if (e.key >= from) yield e;
        }
    }
}

// =============================================================================
// The engine
// =============================================================================
class DB {
    constructor(dir) {
        this.dir = dir;
        this.walPath = dir.join("wal.log");
        this.mem = new OrderedMap();
        this.memBytes = 0;
        this.seq = 0;
        this.levels = [[], [], [], []];
        this.snapshots = new Set();
        this.stats = {
            walSyncs: 0,
            walRecords: 0,
            flushes: 0,
            compactions: 0,
            bytesCompacted: 0,
            tombstonesDropped: 0,
            recoveredOps: 0,
            tornBytes: 0,
        };
        this.pendingBatches = [];
        this.tableSeq = 0;
        if (exists(dir.join("MANIFEST"))) {
            const m = JSON.parse(readFile(dir.join("MANIFEST")));
            this.seq = m.seq;
            this.tableSeq = m.tableSeq;
            this.levels = m.levels.map((lvl) => lvl.map((t) => new TableReader({ ...t, path: dir.join(t.file) })));
        }
        if (exists(this.walPath)) this.replayWal();
    }
    replayWal() {
        const bytes = readBytes(this.walPath);
        let off = 0;
        while (off + 8 <= bytes.length) {
            const dv = new DataView(bytes.buffer, bytes.byteOffset + off),
                len = dv.getUint32(0),
                crc = dv.getUint32(4);
            if (off + 8 + len > bytes.length || CRC32C(bytes.subarray(off + 8, off + 8 + len)) !== crc) {
                this.stats.tornBytes = bytes.length - off;
                break;
            }
            const rec = MsgPackDecode(bytes.subarray(off + 8, off + 8 + len));
            rec.ops.forEach((op, i) => this.memPut(op[0], op[1], rec.seq + i));
            this.seq = Math.max(this.seq, rec.seq + rec.ops.length - 1);
            this.stats.recoveredOps += rec.ops.length;
            off += 8 + len;
        }
        if (off < bytes.length) writeFile(this.walPath, bytes.subarray(0, off));
    }
    memPut(key, value, seq) {
        this.mem.set(memKey(key, seq), { key, seq, value });
        this.memBytes += key.length + (value?.length || 0) + 16;
    }
    async write(ops) {
        // group commit: batches queued in one turn share one sync
        return new Promise((resolve) => {
            this.pendingBatches.push({ ops, resolve });
            if (this.pendingBatches.length === 1) queueMicrotask(() => this.commitGroup());
        });
    }
    commitGroup() {
        const group = this.pendingBatches.splice(0),
            records = [];
        for (const b of group) {
            const seq = this.seq + 1;
            this.seq += b.ops.length;
            const body = MsgPackEncode({ seq, ops: b.ops }),
                hdr = new Uint8Array(8);
            new DataView(hdr.buffer).setUint32(0, body.length);
            new DataView(hdr.buffer).setUint32(4, CRC32C(body));
            records.push(hdr, body);
            b.ops.forEach((op, i) => this.memPut(op[0], op[1], seq + i));
            this.stats.walRecords++;
        }
        writeFile(this.walPath, concat(records), { append: true });
        this.stats.walSyncs++;
        if (this.memBytes >= MEMTABLE_BYTES) this.flush();
        for (const b of group) b.resolve(this.seq);
    }
    put(key, value) {
        return this.write([[key, value]]);
    }
    del(key) {
        return this.write([[key, null]]);
    }
    flush() {
        if (!this.mem.size) return;
        const entries = this.mem.entries().map(([, e]) => e);
        this.mem = new OrderedMap();
        this.memBytes = 0;
        const file = `t${String(++this.tableSeq).padStart(6, "0")}.sst`;
        const info = writeTable(this.dir.join(file), entries);
        this.levels[0].unshift(new TableReader({ ...info, path: this.dir.join(file), file }));
        this.stats.flushes++;
        writeFile(this.walPath, new Uint8Array(0));
        this.saveManifest();
        this.compact();
    }
    saveManifest() {
        writeFile(
            this.dir.join("MANIFEST"),
            JSON.stringify({
                seq: this.seq,
                tableSeq: this.tableSeq,
                levels: this.levels.map((l) =>
                    l.map((t) => ({ file: t.file, size: t.size, first: t.first, last: t.last, count: t.count })),
                ),
            }),
        );
    }
    compact() {
        for (;;) {
            let level = -1;
            if (this.levels[0].length >= L0_TRIGGER) level = 0;
            else
                for (let l = 1; l < this.levels.length - 1; l++)
                    if (this.levels[l].reduce((s, t) => s + t.size, 0) > LEVEL_BASE * 10 ** (l - 1)) {
                        level = l;
                        break;
                    }
            if (level < 0) return;
            const inputs = level === 0 ? [...this.levels[0]] : [this.levels[level][0]];
            const lo = inputs.reduce((m, t) => (t.first < m ? t.first : m), inputs[0].first),
                hi = inputs.reduce((m, t) => (t.last > m ? t.last : m), inputs[0].last);
            const overlap = this.levels[level + 1].filter((t) => !(t.last < lo || t.first > hi));
            const bottom =
                level + 1 === this.levels.length - 1 || this.levels.slice(level + 2).every((l) => l.length === 0);
            const oldestSnap = this.snapshots.size ? Math.min(...this.snapshots) : Infinity;
            const merged = [];
            for (const e of mergeIter([...inputs, ...overlap].map((t) => t.iterate()))) {
                const prev = merged.at(-1);
                if (prev && prev.key === e.key && prev.seq <= oldestSnap) continue; // an older version nobody can see
                merged.push(e);
            }
            const out = [];
            for (let i = 0; i < merged.length; i++) {
                const e = merged[i],
                    newest = merged[i - 1]?.key !== e.key;
                if (bottom && newest && e.value === null && e.seq <= oldestSnap) {
                    this.stats.tombstonesDropped++;
                    while (merged[i + 1]?.key === e.key) i++;
                    continue;
                }
                out.push(e);
            }
            const outTables = [];
            for (let i = 0; i < out.length;) {
                let j = Math.min(out.length, i + 4000);
                while (j < out.length && out[j].key === out[j - 1].key) j++; // keep a key's versions in one table
                const file = `t${String(++this.tableSeq).padStart(6, "0")}.sst`;
                outTables.push(
                    new TableReader({
                        ...writeTable(this.dir.join(file), out.slice(i, j)),
                        path: this.dir.join(file),
                        file,
                    }),
                );
                i = j;
            }
            this.stats.compactions++;
            this.stats.bytesCompacted += [...inputs, ...overlap].reduce((s, t) => s + t.size, 0);
            this.levels[level] = this.levels[level].filter((t) => !inputs.includes(t));
            this.levels[level + 1] = [...this.levels[level + 1].filter((t) => !overlap.includes(t)), ...outTables].sort(
                (a, b) => cmpKey(a.first, b.first),
            );
            this.saveManifest();
            for (const t of [...inputs, ...overlap]) remove(this.dir.join(t.file));
        }
    }
    get(key, snapshot = this.seq) {
        const [hit] = this.mem.rangeQuery(memKey(key, snapshot), `${key}\u0001\u{10FFFF}`, { limit: 1 }); // newest version <= snapshot
        if (hit) return hit[1].value ?? undefined;
        for (const t of this.levels[0]) {
            const e = t.get(key, snapshot);
            if (e) return e.value ?? undefined;
        }
        for (let l = 1; l < this.levels.length; l++)
            for (const t of this.levels[l])
                if (key >= t.first && key <= t.last) {
                    const e = t.get(key, snapshot);
                    if (e) return e.value ?? undefined;
                }
        return undefined;
    }
    snapshot() {
        const s = this.seq;
        this.snapshots.add(s);
        return { seq: s, release: () => this.snapshots.delete(s) };
    }
    *scan(from, to, snapshot = this.seq) {
        const sources = [
            this.mem
                .rangeQuery(from, "\u{10FFFF}")
                .map(([, e]) => e)
                [Symbol.iterator](),
            ...this.levels.flat().map((t) => t.iterate(from)),
        ];
        let last = null;
        for (const e of mergeIter(sources)) {
            if (e.key >= to) return;
            if (e.seq > snapshot || e.key === last) continue;
            last = e.key;
            if (e.value !== null) yield [e.key, e.value];
        }
    }
}
function* mergeIter(iters) {
    // k-way merge: key asc, seq desc
    const heap = new Heap((a, b) => cmpKey(a.e.key, b.e.key) || b.e.seq - a.e.seq);
    iters.forEach((it, i) => {
        const n = it.next();
        if (!n.done) heap.push({ e: n.value, i });
    });
    for (let top = heap.pop(); top !== undefined && top !== null; top = heap.pop()) {
        yield top.e;
        const n = iters[top.i].next();
        if (!n.done) heap.push({ e: n.value, i: top.i });
    }
}

// =============================================================================
// Workload
// =============================================================================
const results = {};
const rng = new Random(7);
const keyOf = (i) => `user${String(XXH3_64(String(i), 3, { as: "bigint" }) % 10_000_000n).padStart(8, "0")}`;
const value = (i, v) => `{"id":${i},"v":${v},"name":"user-${i}","bio":"${"lorem ipsum ".repeat(4 + (i % 6))}"}`;
const truth = new Map();
let db = new DB(work);
let t = performance.now();
for (let i = 0; i < 60000; i += 50) {
    // 50 writers per group
    await Promise.all(
        Array.from({ length: 50 }, (_, k) => {
            truth.set(keyOf(i + k), value(i + k, 0));
            return db.put(keyOf(i + k), value(i + k, 0));
        }),
    );
}
const loadMs = performance.now() - t;
results[`group commit: 60000 writes in ${db.stats.walSyncs} WAL syncs`] =
    db.stats.walSyncs === 1200 && db.stats.walRecords === 60000;

// Snapshot, then updates and deletes.
const snap = db.snapshot();
const snapValue = db.get(keyOf(42));
const ops = { read: 0, update: 0, scan: 0, delete: 0, readMissBloom: 0 };
t = performance.now();
for (let n = 0; n < 20000; n++) {
    const r = rng.nextFloat(),
        i = Math.floor(60000 * rng.nextFloat() ** 2); // skewed towards low ids
    if (r < 0.7) {
        const got = db.get(keyOf(i));
        ops.read++;
        if (got !== truth.get(keyOf(i))) {
            results[`read ${keyOf(i)} mismatched`] = false;
            break;
        }
    } else if (r < 0.9) {
        const v = value(i, n);
        truth.set(keyOf(i), v);
        await db.put(keyOf(i), v);
        ops.update++;
    } else if (r < 0.97) {
        const rows = [];
        for (const row of db.scan(keyOf(i), "user~")) {
            rows.push(row);
            if (rows.length === 20) break;
        }
        ops.scan++;
    } // a 20-row page
    else {
        truth.delete(keyOf(i));
        await db.del(keyOf(i));
        ops.delete++;
    }
}
const mixedMs = performance.now() - t;
for (let i = 0; i < 2000; i++)
    if (db.get(`user${String(i * 4999).padStart(8, "0")}-absent`) === undefined) ops.readMissBloom++;
await db.put(keyOf(42), "changed after the snapshot");
truth.set(keyOf(42), "changed after the snapshot");
results["a snapshot keeps seeing the value as of its sequence number"] =
    db.get(keyOf(42), snap.seq) === snapValue && db.get(keyOf(42)) === "changed after the snapshot";
const scanned = [...db.scan("user0", "user1")];
const expected = [...truth.keys()].filter((k) => k >= "user0" && k < "user1").sort();
results[`a range scan merges ${db.levels.flat().length} tables and the memtable correctly (${scanned.length} keys)`] =
    scanned.length === expected.length && scanned.every(([k, v], i) => k === expected[i] && v === truth.get(k));
const bloomSkips = db.levels.flat().reduce((s, tb) => s + tb.stats.bloomSkips, 0);
results[`Bloom filters skipped ${bloomSkips} table probes for absent keys`] = bloomSkips > 1500;
results[`${db.stats.compactions} compactions kept L0 at ${db.levels[0].length} tables`] =
    db.stats.compactions > 0 && db.levels[0].length < L0_TRIGGER && db.levels[1].length > 0;
snap.release();

// Crash: unsynced memtable lives only in the WAL; tear the WAL's last record, reopen, compare.
await db.put("crash-test-a", "1");
await db.put("crash-test-b", "2");
const wal = readBytes(work.join("wal.log"));
writeFile(work.join("wal.log"), wal.subarray(0, wal.length - 3));
truth.set("crash-test-a", "1");
db = new DB(work);
results[`recovery replayed ${db.stats.recoveredOps} WAL ops and ignored the torn record`] =
    db.stats.tornBytes > 0 && db.get("crash-test-a") === "1" && db.get("crash-test-b") === undefined;
let mismatches = 0;
for (const [k, v] of truth) if (db.get(k) !== v) mismatches++;
for (let i = 0; i < 60000; i += 97) if (!truth.has(keyOf(i)) && db.get(keyOf(i)) !== undefined) mismatches++;
results[`after reopening, all ${truth.size} live keys and the deletes read back correctly`] = mismatches === 0;

const files = readDir(work).filter((f) => f.name.endsWith(".sst"));
const diskBytes = files.reduce((s, f) => s + stat(work.join(f.name)).size, 0),
    logical = [...truth].reduce((s, [k, v]) => s + k.length + v.length, 0);
print("lsm-storage-engine");
print(
    `  load       60000 puts in ${loadMs.toFixed(0)} ms (${((60000 / loadMs) * 1000).toFixed(0)}/s), ${db.stats.flushes || "-"} flushes this session`,
);
print(
    `  mixed      ${Object.entries(ops)
        .map(([k, v]) => `${k} ${v}`)
        .join(", ")} in ${mixedMs.toFixed(0)} ms`,
);
print(
    table([
        ["level", "tables", "bytes", "keys"],
        ...db.levels.map((l, i) => [
            `L${i}`,
            String(l.length),
            String(l.reduce((s, x) => s + x.size, 0)),
            String(l.reduce((s, x) => s + x.count, 0)),
        ]),
    ]),
);
print(
    `  space      ${(diskBytes / 1024).toFixed(0)} KiB on disk for ${(logical / 1024).toFixed(0)} KiB of live data (prefix compression + LZ4)`,
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
