// LSM storage engine — a write-ahead log with group commit, a memtable, immutable SSTables with prefix-compressed
// blocks, per-table Bloom filters, leveled compaction, tombstones, MVCC snapshots, range scans through a k-way
// merge, crash recovery, and a YCSB-style benchmark (the LevelDB/RocksDB design).
//
// WHAT IT IS
//   WAL          every write batch is appended as [len][crc32c][seq][ops…] and the file is synced once per
//                batch group (group commit: concurrent writers share one fsync)
//   memtable     a sorted array of (key, seq, value|tombstone) with binary-search inserts; at 256 KiB it is
//                frozen and flushed to a level-0 SSTable while a new one takes writes
//   SSTables     data blocks of ~4 KiB whose keys are prefix-compressed against the previous key, with a restart
//                array (an entry stored whole every 16 keys) at the block's end; each block is LZ4-compressed and
//                followed by the CRC32C of its compressed bytes; then an index of last-key -> offset, a Bloom filter
//                over the keys, and a footer. A point read checks the filter, binary-searches the index, then the
//                block's restarts, and decodes only from the nearest restart; values are decoded only when returned
//   compaction   L0 tables overlap each other; when L0 has 4 tables they merge into L1, and each level is kept under
//                10x the size of the one above by merging a table into the overlapping range of the next level;
//                tombstones are dropped only at the bottom level, and versions older than the oldest snapshot are
//                discarded
//   reads        get() searches memtable -> immutable memtable -> L0 newest first -> L1.. by range; a snapshot
//                pins a sequence number so later writes are invisible to it
//   scans        a lazy heap merge of the memtable (read in pages of 64) and every overlapping table's iterator
//                (each seeks to its block and restart first); newest version wins, tombstones hide keys, and a
//                20-row page is scan(...).take(20) — the merge stops when the page is full
//   compaction   inputs are merged as whole sorted runs, pairwise; write amplification is reported
//   recovery     on open the WAL is replayed (a torn final record is detected by its CRC and ignored) and the
//                manifest lists the live tables
//
// CHECKED AGAINST A MODEL
//   A plain Map (plus a sorted key array) shadows every write. Every read and every scan page in the mixed workload
//   is compared with it; two snapshots copy the model when taken and, after the updates, deletes and compactions
//   that follow, must re-read every key written since plus a sample of the rest, and scan a range, exactly as it
//   was. A byte flipped inside a data block must be refused by that block's CRC, with every other block readable.
//
//   natives      the memtable is an OrderedMap keyed by key + inverted sequence number, so versions sort newest first
//                inside a key and a snapshot read is one rangeQuery with limit 1 (RocksDB's skiplist, minus the
//                O(n) array splice per insert); each SSTable's Bloom filter is a native BloomFilter whose serialized
//                record is stored in the footer; decoded blocks sit in an LRU; blocks are assembled in one growable
//                buffer (TextEncoder.encodeInto straight into it, varints written in place)
//
// MODULES  dyna:file · dyna:hash (CRC32C, XXH3) · dyna:compress (lz4) · dyna:structures
//          (Heap, OrderedMap, BloomFilter, LRU) · dyna:serialize (MsgPack) · dyna:bytes (concat) · dyna:random
//          · dyna:cli (Table)
//
// RUN      dynajs examples/systems/lsm-storage-engine.js     loads 60,000 records, runs a mixed workload, crashes and recovers

import { makeTempDir, writeFile, readBytes, readFile, exists, remove, readDir, stat, removeAll } from "dyna:file";
import { CRC32C, XXH3_64 } from "dyna:hash";
import { lz4Compress, lz4Decompress } from "dyna:compress";
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

// A growable byte buffer: varints and UTF-8 are written in place, with no per-field arrays to concatenate.
class ByteBuf {
    constructor(cap = 8192) {
        this.a = new Uint8Array(cap);
        this.n = 0;
    }
    room(k) {
        if (this.n + k <= this.a.length) return;
        const b = new Uint8Array(Math.max(this.a.length * 2, this.n + k));
        b.set(this.a.subarray(0, this.n));
        this.a = b;
    }
    varint(x) {
        this.room(10);
        while (x >= 128) {
            this.a[this.n++] = (x % 128) | 128;
            x = Math.floor(x / 128);
        }
        this.a[this.n++] = x;
    }
    u32(x) {
        this.room(4);
        this.a[this.n++] = x & 255;
        this.a[this.n++] = (x >>> 8) & 255;
        this.a[this.n++] = (x >>> 16) & 255;
        this.a[this.n++] = x >>> 24;
    }
    // varint(byteLength + bias) then the UTF-8 bytes: encoded straight after a one-byte length slot, and shifted
    // right only in the rare case the length needs a longer varint
    lenText(str, bias = 0) {
        this.room(str.length * 3 + 10);
        const at = this.n,
            w = enc.encodeInto(str, this.a.subarray(at + 1)).written,
            len = w + bias;
        if (len < 128) {
            this.a[at] = len;
            this.n = at + 1 + w;
            return;
        }
        let k = 1;
        for (let v = len; v >= 128; v = Math.floor(v / 128)) k++;
        this.a.copyWithin(at + k, at + 1, at + 1 + w);
        this.varint(len);
        this.n += w;
    }
    put(bytes) {
        this.room(bytes.length);
        this.a.set(bytes, this.n);
        this.n += bytes.length;
    }
    bytes() {
        return this.a.subarray(0, this.n);
    }
}

// =============================================================================
// SSTable writer and reader
// =============================================================================
// Block layout (LevelDB's): entries [shared][suffixLen][suffix][seq][valueLen+1 | 0 = tombstone][value], then the
// u32 offset of every restart entry (shared = 0, every RESTART keys) and the restart count; the block is LZ4
// compressed and followed by the CRC32C of the compressed bytes.
function writeTable(path, entries) {
    // entries sorted by key, then seq desc
    const file = new ByteBuf(1 << 16),
        blk = new ByteBuf(BLOCK_BYTES * 2),
        index = [],
        bloom = newBloom(entries.length);
    let restarts = [],
        count = 0,
        prevKey = "";
    const flushBlock = () => {
        if (!count) return;
        for (const r of restarts) blk.u32(r);
        blk.u32(restarts.length);
        const packed = lz4Compress(blk.bytes());
        index.push({ last: prevKey, offset: file.n, length: packed.length + 4 });
        file.put(packed);
        file.u32(CRC32C(packed));
        blk.n = 0;
        restarts = [];
        count = 0;
    };
    entries.forEach((e, i) => {
        let shared = 0;
        if (count % RESTART === 0) restarts.push(blk.n);
        else while (shared < prevKey.length && shared < e.key.length && prevKey[shared] === e.key[shared]) shared++;
        blk.varint(shared);
        blk.lenText(shared ? e.key.slice(shared) : e.key);
        blk.varint(e.seq);
        if (e.value === null) blk.varint(0);
        else blk.lenText(e.value, 1);
        count++;
        bloom.add(e.key);
        prevKey = e.key;
        if (blk.n >= BLOCK_BYTES || i === entries.length - 1) flushBlock();
    });
    const meta = MsgPackEncode({
        index,
        bloom: bloom.serialize(),
        count: entries.length,
        first: entries[0].key,
        last: entries.at(-1).key,
    });
    file.put(meta);
    file.room(8);
    new DataView(file.a.buffer).setUint32(file.n, meta.length);
    new DataView(file.a.buffer).setUint32(file.n + 4, CRC32C(meta));
    file.n += 8;
    writeFile(path, file.bytes());
    return {
        path: path.toString(),
        size: file.n,
        first: entries[0].key,
        last: entries.at(-1).key,
        count: entries.length,
    };
}
// Walks a decoded block from one offset. Values are decoded only when asked for: a point read compares keys and
// sequence numbers, and turns bytes into a string for the one entry it returns.
class BlockCursor {
    constructor(blk, at) {
        this.raw = blk.raw;
        this.end = blk.end;
        this.off = at;
        this.key = "";
    }
    varint() {
        const raw = this.raw;
        let b = raw[this.off++];
        if (b < 128) return b;
        let x = b & 127,
            s = 128;
        do {
            b = raw[this.off++];
            x += (b & 127) * s;
            s *= 128;
        } while (b & 128);
        return x;
    }
    next() {
        if (this.off >= this.end) return false;
        const shared = this.varint(),
            sl = this.varint(),
            suffix = dec.decode(this.raw.subarray(this.off, this.off + sl));
        this.key = shared ? this.key.slice(0, shared) + suffix : suffix;
        this.off += sl;
        this.seq = this.varint();
        const vl = this.varint();
        this.vo = this.off;
        this.vl = vl;
        if (vl) this.off += vl - 1;
        return true;
    }
    value() {
        return this.vl === 0 ? null : dec.decode(this.raw.subarray(this.vo, this.vo + this.vl - 1));
    }
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
        this.stats = { bloomSkips: 0, blockReads: 0, pointReads: 0, decoded: 0 };
    }
    block(i) {
        const hit = this.blockCache.get(i);
        if (hit) return hit;
        this.stats.blockReads++;
        const b = this.index[i],
            stored = this.bytes.subarray(b.offset, b.offset + b.length),
            packed = stored.subarray(0, stored.length - 4),
            want = new DataView(stored.buffer, stored.byteOffset + stored.length - 4, 4).getUint32(0, true);
        if (CRC32C(packed) !== want) throw new Error(`${this.file ?? this.path}: block ${i} checksum mismatch`);
        const raw = lz4Decompress(packed),
            dv = new DataView(raw.buffer, raw.byteOffset, raw.length),
            nr = dv.getUint32(raw.length - 4, true),
            end = raw.length - 4 - 4 * nr,
            restarts = new Int32Array(nr);
        for (let r = 0; r < nr; r++) restarts[r] = dv.getUint32(end + 4 * r, true);
        const blk = { raw, restarts, end };
        this.blockCache.put(i, blk);
        return blk;
    }
    restartKey(blk, r) {
        const c = new BlockCursor(blk, blk.restarts[r]);
        c.next();
        return c.key;
    }
    get(key, maxSeq) {
        if (key < this.first || key > this.last) return undefined;
        if (!this.bloom.mayContain(key)) {
            this.stats.bloomSkips++;
            return undefined;
        }
        this.stats.pointReads++;
        // every version of the key comes after the last restart that sorts below it
        let [b, at] = this.seek(key);
        for (; b < this.index.length; b++, at = 0) {
            const c = new BlockCursor(this.block(b), at);
            while (c.next()) {
                this.stats.decoded++;
                if (c.key === key && c.seq <= maxSeq) return { key, seq: c.seq, value: c.value() };
                if (c.key > key) return undefined;
            }
        }
        return undefined;
    }
    entries() {
        const out = [];
        for (let b = 0; b < this.index.length; b++)
            for (const c = new BlockCursor(this.block(b), 0); c.next();)
                out.push({ key: c.key, seq: c.seq, value: c.value() });
        return out;
    }
    // the first block that can hold `from`, and inside it the last restart below `from`
    seek(from) {
        let lo = 0,
            hi = this.index.length;
        while (lo < hi) {
            const mid = (lo + hi) >> 1;
            if (this.index[mid].last < from) lo = mid + 1;
            else hi = mid;
        }
        if (lo === this.index.length) return [lo, 0];
        const blk = this.block(lo);
        let r = 0,
            h = blk.restarts.length - 1;
        while (r < h) {
            const m = (r + h + 1) >> 1;
            if (this.restartKey(blk, m) < from) r = m;
            else h = m - 1;
        }
        return [lo, blk.restarts[r]];
    }
    *iterate(from = "") {
        let [b, at] = this.seek(from);
        for (; b < this.index.length; b++, at = 0)
            for (const c = new BlockCursor(this.block(b), at); c.next();)
                if (c.key >= from) yield { key: c.key, seq: c.seq, value: c.value() };
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
            bytesFlushed: 0,
            bytesRewritten: 0,
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
        this.stats.bytesFlushed += info.size;
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
            for (const e of mergeRuns([...inputs, ...overlap].map((t) => t.entries()))) {
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
            this.stats.bytesRewritten += outTables.reduce((s, t) => s + t.size, 0);
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
            memRange(this.mem, from),
            ...this.levels
                .flat()
                .filter((t) => t.last >= from && t.first < to)
                .map((t) => t.iterate(from)),
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
// The memtable read lazily in pages: a scan that stops after 20 rows must not copy the whole memtable.
function* memRange(mem, from) {
    for (let lo = from; lo !== undefined;) {
        const page = mem.rangeQuery(lo, "\u{10FFFF}", { limit: 64 });
        for (const [, e] of page) yield e;
        if (page.length < 64) return;
        lo = mem.higherKey(page.at(-1)[0]);
    }
}
// Compaction merges whole tables: sorted runs merged pairwise in a balanced tree, one comparison per output entry
// per round. The lazy heap merge below serves scans, which stop after a page.
const before = (a, b) => a.key < b.key || (a.key === b.key && a.seq > b.seq);
function mergeRuns(runs) {
    while (runs.length > 1) {
        const next = [];
        for (let r = 0; r + 1 < runs.length; r += 2) {
            const a = runs[r],
                b = runs[r + 1],
                out = new Array(a.length + b.length);
            let i = 0,
                j = 0,
                k = 0;
            while (i < a.length && j < b.length) out[k++] = before(b[j], a[i]) ? b[j++] : a[i++];
            while (i < a.length) out[k++] = a[i++];
            while (j < b.length) out[k++] = b[j++];
            next.push(out);
        }
        if (runs.length % 2) next.push(runs.at(-1));
        runs = next;
    }
    return runs[0] ?? [];
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
// The model: a plain Map plus a sorted key array kept by binary insertion, so a scan page's expected rows are a
// slice. Every write also lands in the "touched" set of each live snapshot, which is what the MVCC check re-reads.
const truth = new Map(),
    sortedKeys = [],
    liveSnaps = [];
const lowerBound = (k) => {
    let lo = 0,
        hi = sortedKeys.length;
    while (lo < hi) {
        const m = (lo + hi) >> 1;
        if (sortedKeys[m] < k) lo = m + 1;
        else hi = m;
    }
    return lo;
};
const tset = (k, v) => {
    if (!truth.has(k)) sortedKeys.splice(lowerBound(k), 0, k);
    truth.set(k, v);
    for (const sn of liveSnaps) sn.touched.add(k);
};
const tdel = (k) => {
    if (truth.delete(k)) sortedKeys.splice(lowerBound(k), 1);
    for (const sn of liveSnaps) sn.touched.add(k);
};
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
sortedKeys.push(...[...truth.keys()].sort()); // one sort after the bulk load, binary insertion from here on
results[`group commit: 60000 writes in ${db.stats.walSyncs} WAL syncs`] =
    db.stats.walSyncs === 1200 && db.stats.walRecords === 60000;

// Snapshot, then updates and deletes.
const snap = db.snapshot();
const snapValue = db.get(keyOf(42));
const ops = { read: 0, update: 0, scan: 0, delete: 0, readMissBloom: 0 };
let scanMismatches = 0;
t = performance.now();
for (let n = 0; n < 20000; n++) {
    if (n === 4000 || n === 12000)
        liveSnaps.push({
            snap: db.snapshot(),
            model: new Map(truth),
            touched: new Set(),
            compactions: db.stats.compactions,
        });
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
        tset(keyOf(i), v);
        await db.put(keyOf(i), v);
        ops.update++;
    } else if (r < 0.97) {
        // a 20-row page: the merge is lazy, so take() stops it after the rows it needs
        const rows = db.scan(keyOf(i), "user~").take(20).toArray(),
            at = lowerBound(keyOf(i)),
            want = sortedKeys.slice(at, at + 20);
        if (rows.length !== want.length || rows.some(([k, v], j) => k !== want[j] || v !== truth.get(k)))
            scanMismatches++;
        ops.scan++;
    } else {
        tdel(keyOf(i));
        await db.del(keyOf(i));
        ops.delete++;
    }
}
const mixedMs = performance.now() - t;
for (let i = 0; i < 2000; i++)
    if (db.get(`user${String(i * 4999).padStart(8, "0")}-absent`) === undefined) ops.readMissBloom++;
await db.put(keyOf(42), "changed after the snapshot");
tset(keyOf(42), "changed after the snapshot");
results[`all ${ops.scan} scan pages match the model, row for row`] = scanMismatches === 0;
// MVCC: each snapshot must still read the database as it was, after the updates, deletes, flushes and compactions
// that followed it. Every key written since is re-read, plus a sample of the untouched ones, plus a range scan.
for (const [n, sn] of liveSnaps.entries()) {
    const keys = [...sn.touched, ...[...sn.model.keys()].filter((_, j) => j % 11 === 0)];
    const bad = keys.filter((k) => db.get(k, sn.snap.seq) !== sn.model.get(k)).length;
    const page = [...db.scan("user0", "user02", sn.snap.seq)],
        want = [...sn.model.keys()].filter((k) => k >= "user0" && k < "user02").sort();
    const scanOk = page.length === want.length && page.every(([k, v], j) => k === want[j] && v === sn.model.get(k));
    results[
        `snapshot ${n + 1} reads its own past through ${db.stats.compactions - sn.compactions} later compactions (${keys.length} keys, ${sn.touched.size} rewritten since, ${page.length}-row scan)`
    ] = bad === 0 && scanOk && db.stats.compactions > sn.compactions && sn.touched.size > 500;
}
for (const sn of liveSnaps.splice(0)) sn.snap.release();
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
const firstSession = { ...db.stats };
const wal = readBytes(work.join("wal.log"));
writeFile(work.join("wal.log"), wal.subarray(0, wal.length - 3));
tset("crash-test-a", "1");
db = new DB(work);
results[`recovery replayed ${db.stats.recoveredOps} WAL ops and ignored the torn record`] =
    db.stats.tornBytes > 0 && db.get("crash-test-a") === "1" && db.get("crash-test-b") === undefined;
let mismatches = 0;
for (const [k, v] of truth) if (db.get(k) !== v) mismatches++;
for (let i = 0; i < 60000; i += 97) if (!truth.has(keyOf(i)) && db.get(keyOf(i)) !== undefined) mismatches++;
results[`after reopening, all ${truth.size} live keys and the deletes read back correctly`] = mismatches === 0;

// Bit rot: flip one byte inside a data block. Its CRC must refuse the block by name rather than hand back a wrong
// value, and the damage must stay inside that block: the table's other blocks and every other table still read.
const victim = db.levels.flat().find((tb) => tb.index.length > 1),
    rotten = readBytes(victim.path);
rotten[victim.index[0].offset + 7] ^= 0x20;
writeFile(victim.path, rotten);
const reopened = new DB(work),
    rv = reopened.levels.flat().find((tb) => tb.file === victim.file);
let rotError = "";
try {
    rv.get(victim.first, Infinity);
} catch (e) {
    rotError = e.message;
}
let scanError = "";
try {
    reopened.scan(victim.first, "user~").take(5).toArray();
} catch (e) {
    scanError = e.message;
}
const secondBlockKey = new BlockCursor(rv.block(1), 0);
secondBlockKey.next();
const others = reopened.levels.flat().filter((tb) => tb !== rv);
results[`a flipped bit in ${victim.file} block 0 is refused by its CRC, and only that block is lost`] =
    /block 0 checksum mismatch/.test(rotError) &&
    /block 0 checksum mismatch/.test(scanError) &&
    rv.get(secondBlockKey.key, Infinity)?.key === secondBlockKey.key &&
    others.every((tb) => tb.get(tb.last, Infinity)?.key === tb.last);

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
    `  write amp  ${((firstSession.bytesFlushed + firstSession.bytesRewritten) / firstSession.bytesFlushed).toFixed(2)}x (${(firstSession.bytesFlushed / 1024).toFixed(0)} KiB flushed, ${(firstSession.bytesRewritten / 1024).toFixed(0)} KiB rewritten by compaction)`,
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
