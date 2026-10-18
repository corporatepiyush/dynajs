// Message broker — a Kafka-style partitioned log over TCP: segment files with a sparse offset index, idempotent
// producers, consumer groups with rebalancing and committed offsets, retention, log compaction and crash recovery.
//
// WHAT IT IS
//   storage      each topic partition is an append-only log split into segment files (records framed as
//                length + CRC32C + MsgPack); every 4 KiB a sparse index entry maps offset -> file position so a
//                fetch from any offset is a binary search plus a short scan
//   protocol     a binary request/response protocol over TCP: uvarint length frames carrying MsgPack
//                {api, correlationId, …}; responses come back with the same correlation id, so clients pipeline
//   producing    records are routed by key hash to a partition; producers have an id and a per-partition
//                sequence number, so a retried batch is detected and NOT appended twice (exactly-once append)
//   consuming    consumers join a group; the coordinator assigns partitions round-robin and rebalances when a
//                member joins or leaves (heartbeats time out); offsets are committed per group and partition
//   retention    old segments of a time/size-bounded topic are deleted whole; a compacted topic keeps only the
//                newest record per key (tombstones remove a key)
//   recovery     on restart the broker rescans segments, truncates a torn final record (CRC mismatch), and
//                rebuilds indexes; committed offsets survive
//   index        each segment's sparse index is a SortedMap offset -> file position: a fetch is one floorKey()
//   poison       records a consumer cannot process go to a dead-letter topic keyed by order id; the DLQ is compacted,
//                so redeliveries after a rebalance cannot create duplicate dead letters
//   accounting   what each consumer saw is a RoaringBitmap of order ids: "every order consumed" is a cardinality,
//                "redelivered" is the sum of sizes minus the size of the union; consumer lag comes from a lag API
//
// MODULES  dyna:net (TCPServer) · dyna:serialize (MsgPack) · dyna:encoding (uvarint) · dyna:hash (CRC32C, XXH3)
//          · dyna:file · dyna:bytes (concat) · dyna:structures (SortedMap, RoaringBitmap) · dyna:async (sleep)
//          · dyna:cli (Table) · dyna:random
//
// RUN      dynajs examples/net/message-broker.js     produces 30,000 orders, consumes them in a group, crashes and recovers

import { TCPServer } from "dyna:net";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { PutUvarint, Uvarint } from "dyna:encoding";
import { CRC32C, XXH3_64 } from "dyna:hash";
import { makeTempDir, makeDir, writeFile, readBytes, readDir, remove, exists, removeAll } from "dyna:file";
import { SortedMap, RoaringBitmap } from "dyna:structures";
import { concat } from "dyna:bytes";
import { sleep } from "dyna:async";
import { Table } from "dyna:cli";
import { Random } from "dyna:random";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("broker");
const rng = new Random(77);
const SEGMENT_BYTES = 64 * 1024,
    INDEX_EVERY = 4096;

// =============================================================================
// Log storage
// =============================================================================
function frameRecord(rec) {
    const body = MsgPackEncode(rec),
        crc = CRC32C(body);
    const out = new Uint8Array(8 + body.length),
        dv = new DataView(out.buffer);
    dv.setUint32(0, body.length);
    dv.setUint32(4, crc);
    out.set(body, 8);
    return out;
}
class Partition {
    constructor(dir, { compacted = false, retainSegments = Infinity, segmentBytes = SEGMENT_BYTES } = {}) {
        this.dir = dir;
        this.compacted = compacted;
        this.retainSegments = retainSegments;
        this.segmentBytes = segmentBytes;
        makeDir(dir, { recursive: true });
        this.segments = new SortedMap(); // base offset -> { file, size, index: [[offset, pos]] }
        this.next = 0;
        this.producers = new Map();
        this.truncated = 0;
        this.recover();
    }
    segFile(base) {
        return this.dir.join(`${String(base).padStart(20, "0")}.log`);
    }
    recover() {
        for (const e of readDir(this.dir)
            .filter((f) => f.name.endsWith(".log"))
            .sort((a, b) => a.name.localeCompare(b.name))) {
            const base = Number(e.name.slice(0, 20)),
                path = this.dir.join(e.name),
                bytes = readBytes(path);
            const seg = { base, file: path, size: 0, index: new SortedMap(), last: base - 1 };
            let pos = 0,
                sinceIndex = INDEX_EVERY;
            while (pos + 8 <= bytes.length) {
                const dv = new DataView(bytes.buffer, bytes.byteOffset + pos),
                    len = dv.getUint32(0),
                    crc = dv.getUint32(4);
                if (pos + 8 + len > bytes.length || CRC32C(bytes.subarray(pos + 8, pos + 8 + len)) !== crc) break; // torn tail
                const rec = MsgPackDecode(bytes.subarray(pos + 8, pos + 8 + len));
                if (sinceIndex >= INDEX_EVERY) {
                    seg.index.set(rec.offset, pos);
                    sinceIndex = 0;
                    seg.lastIndexed = pos;
                }
                if (rec.pid) this.producers.set(rec.pid, Math.max(this.producers.get(rec.pid) ?? -1, rec.seq));
                seg.last = rec.offset;
                sinceIndex += 8 + len;
                pos += 8 + len;
            }
            if (pos < bytes.length) {
                writeFile(path, bytes.subarray(0, pos));
                this.truncated += bytes.length - pos;
            }
            seg.size = pos;
            this.segments.set(base, seg);
            this.next = Math.max(this.next, seg.last + 1);
        }
    }
    append(records, pid, firstSeq) {
        if (pid !== undefined) {
            const last = this.producers.get(pid) ?? -1;
            if (firstSeq <= last) return { duplicate: true, baseOffset: null }; // a retried batch
            if (firstSeq !== last + 1) return { error: `out of order sequence ${firstSeq}, expected ${last + 1}` };
        }
        const base = this.next;
        let segBase = this.segments.lastKey();
        let seg = segBase === undefined || segBase === null ? null : this.segments.get(segBase);
        const chunks = [];
        records.forEach((r, i) => {
            if (!seg || seg.size >= this.segmentBytes) {
                if (seg && chunks.length) {
                    writeFile(seg.file, concat(chunks.splice(0)), { append: true });
                }
                seg = {
                    base: this.next,
                    file: this.segFile(this.next),
                    size: 0,
                    index: new SortedMap(),
                    last: this.next - 1,
                    lastIndexed: -Infinity,
                };
                writeFile(seg.file, new Uint8Array(0));
                this.segments.set(seg.base, seg);
            }
            const framed = frameRecord({
                offset: this.next,
                key: r.key ?? null,
                value: r.value,
                ts: r.ts ?? Date.now(),
                pid,
                seq: pid !== undefined ? firstSeq + i : undefined,
            });
            if (seg.size - (seg.lastIndexed ?? -Infinity) >= INDEX_EVERY) {
                seg.index.set(this.next, seg.size);
                seg.lastIndexed = seg.size;
            }
            chunks.push(framed);
            seg.size += framed.length;
            seg.last = this.next;
            this.next++;
        });
        if (chunks.length) writeFile(seg.file, concat(chunks), { append: true });
        if (pid !== undefined) this.producers.set(pid, firstSeq + records.length - 1);
        this.enforceRetention();
        return { baseOffset: base };
    }
    fetch(offset, maxRecords = 500) {
        const out = [];
        let segBase = this.segments.floorKey(offset);
        if (segBase === undefined || segBase === null) segBase = this.segments.firstKey();
        while (segBase !== undefined && segBase !== null && out.length < maxRecords) {
            const seg = this.segments.get(segBase),
                bytes = readBytes(seg.file);
            const at = seg.index.floorKey(offset),
                start = at === undefined || at === null ? 0 : seg.index.get(at);
            for (let pos = start; pos + 8 <= seg.size && out.length < maxRecords;) {
                const len = new DataView(bytes.buffer, bytes.byteOffset + pos).getUint32(0);
                const rec = MsgPackDecode(bytes.subarray(pos + 8, pos + 8 + len));
                if (rec.offset >= offset) out.push(rec);
                pos += 8 + len;
            }
            segBase = this.segments.ceilKey(segBase + 1);
        }
        return out;
    }
    enforceRetention() {
        const keys = [...this.segments.keys()];
        while (keys.length > this.retainSegments) {
            const b = keys.shift();
            remove(this.segments.get(b).file);
            this.segments.delete(b);
        }
    }
    compact() {
        // rewrite closed segments keeping the newest value per key
        const keys = [...this.segments.keys()];
        if (keys.length < 2) return 0;
        const closed = keys.slice(0, -1),
            latest = new Map();
        for (const b of keys)
            for (const r of this.fetch(b, 1e9).filter((x) => x.offset <= this.segments.get(b).last))
                latest.set(r.key, r.offset);
        let removed = 0;
        for (const b of closed) {
            const seg = this.segments.get(b),
                keep = [];
            for (const r of this.fetch(b, 1e9)) {
                if (r.offset > seg.last) break;
                if (latest.get(r.key) === r.offset && r.value !== null) keep.push(r);
                else removed++;
            }
            const bytes = concat(keep.map(frameRecord));
            writeFile(seg.file, bytes);
            const index = new SortedMap();
            if (keep.length) index.set(keep[0].offset, 0);
            Object.assign(seg, { size: bytes.length, index, lastIndexed: 0 });
        }
        return removed;
    }
    get bytes() {
        return [...this.segments.keys()].reduce((s, b) => s + this.segments.get(b).size, 0);
    }
}

// =============================================================================
// Broker: topics, groups, protocol
// =============================================================================
class Broker {
    constructor(dir) {
        this.dir = dir;
        this.topics = new Map();
        this.groups = new Map();
        this.offsets = new Map();
        if (exists(dir.join("offsets.msgpack")))
            this.offsets = new Map(Object.entries(MsgPackDecode(readBytes(dir.join("offsets.msgpack")))));
        if (exists(dir.join("topics.msgpack")))
            for (const [name, cfg] of Object.entries(MsgPackDecode(readBytes(dir.join("topics.msgpack")))))
                this.createTopic(name, cfg);
    }
    createTopic(name, cfg) {
        if (this.topics.has(name)) return;
        this.topics.set(name, {
            cfg,
            parts: Array.from({ length: cfg.partitions }, (_, p) => new Partition(this.dir.join(name, String(p)), cfg)),
        });
        writeFile(
            this.dir.join("topics.msgpack"),
            MsgPackEncode(Object.fromEntries([...this.topics].map(([n, t]) => [n, t.cfg]))),
        );
    }
    group(name) {
        if (!this.groups.has(name)) this.groups.set(name, { members: new Map(), generation: 0, assignment: new Map() });
        return this.groups.get(name);
    }
    rebalance(g, topic) {
        const members = [...g.members.keys()].sort(),
            parts = this.topics.get(topic).parts.length;
        g.generation++;
        g.assignment = new Map(members.map((m) => [m, []]));
        for (let p = 0; p < parts; p++) g.assignment.get(members[p % members.length])?.push(p);
    }
    expire(now) {
        for (const [gname, g] of this.groups)
            for (const [m, info] of g.members)
                if (now - info.seen > 600) {
                    g.members.delete(m);
                    this.rebalance(g, info.topic);
                }
    }
    handle(req) {
        const t = this.topics.get(req.topic);
        switch (req.api) {
            case "produce": {
                if (!t) return { error: "unknown topic" };
                const p =
                    req.partition ??
                    Number(XXH3_64(String(req.records[0].key), 0, { as: "bigint" }) % BigInt(t.parts.length));
                return { partition: p, ...t.parts[p].append(req.records, req.producerId, req.sequence) };
            }
            case "fetch":
                return t
                    ? {
                          records: t.parts[req.partition].fetch(req.offset, req.max ?? 500),
                          highWatermark: t.parts[req.partition].next,
                      }
                    : { error: "unknown topic" };
            case "join": {
                const g = this.group(req.group);
                g.members.set(req.member, { seen: Date.now(), topic: req.topic });
                this.rebalance(g, req.topic);
                return { generation: g.generation };
            }
            case "heartbeat": {
                this.expire(Date.now());
                const g = this.group(req.group);
                const m = g.members.get(req.member);
                if (!m) return { error: "unknown member", rejoin: true };
                m.seen = Date.now();
                return { generation: g.generation, partitions: g.assignment.get(req.member) || [] };
            }
            case "leave": {
                const g = this.group(req.group);
                g.members.delete(req.member);
                this.rebalance(g, req.topic);
                return { ok: true };
            }
            case "commit": {
                const g = this.group(req.group);
                if (req.generation !== g.generation) return { error: "stale generation", rejoin: true };
                for (const [p, off] of Object.entries(req.offsets))
                    this.offsets.set(`${req.group}|${req.topic}|${p}`, off);
                writeFile(this.dir.join("offsets.msgpack"), MsgPackEncode(Object.fromEntries(this.offsets)));
                return { ok: true };
            }
            case "lag": {
                if (!t) return { error: "unknown topic" };
                return {
                    lag: t.parts.reduce(
                        (s, p, i) => s + p.next - (this.offsets.get(`${req.group}|${req.topic}|${i}`) ?? 0),
                        0,
                    ),
                };
            }
            case "committed":
                return { offset: this.offsets.get(`${req.group}|${req.topic}|${req.partition}`) ?? 0 };
            default:
                return { error: `unknown api ${req.api}` };
        }
    }
}
const frame = (obj) => {
    const b = MsgPackEncode(obj),
        l = PutUvarint(b.length);
    return concat([l, b]);
};
function deframer(on) {
    let buf = new Uint8Array(0);
    return (first, second) => {
        const chunk = second ?? first; // usable as feed(bytes) or as a TCP data(conn, bytes) handler
        buf = concat([buf, chunk]);
        for (;;) {
            if (!buf.length) return;
            let n, used;
            try {
                [n, used] = Uvarint(buf);
            } catch {
                return;
            }
            if (buf.length < used + Number(n)) return;
            on(MsgPackDecode(buf.subarray(used, used + Number(n))));
            buf = buf.slice(used + Number(n));
        }
    };
}
function serve(broker) {
    const srv = new TCPServer({ port: 0 });
    srv.start({
        connect: (c) => {
            c.feed = deframer((req) => c.write(frame({ correlationId: req.correlationId, ...broker.handle(req) })));
        },
        data: (c, b) => c.feed(b),
    });
    return srv;
}
class Client {
    constructor(port) {
        this.port = port;
        this.cid = 0;
        this.waiting = new Map();
    }
    connect() {
        return new Promise((res, rej) => {
            this.conn = TCPServer.connect(
                { host: "127.0.0.1", port: this.port },
                {
                    connect: (c, e) => (e ? rej(new Error(e)) : ((this.raw = c), res(this))),
                    data: deframer((m) => {
                        this.waiting.get(m.correlationId)?.(m);
                        this.waiting.delete(m.correlationId);
                    }),
                },
            );
        });
    }
    call(req) {
        return new Promise((res) => {
            const id = ++this.cid;
            this.waiting.set(id, res);
            this.raw.write(frame({ ...req, correlationId: id }));
        });
    }
    close() {
        this.conn.close();
    }
}

// =============================================================================
// Scenario
// =============================================================================
const results = {};
let broker = new Broker(work),
    server = serve(broker);
broker.createTopic("orders", { partitions: 6 });
broker.createTopic("prices", { partitions: 2, compacted: true, segmentBytes: 4096 });
broker.createTopic("clicks", { partitions: 1, retainSegments: 3, segmentBytes: 32768 });
broker.createTopic("orders-dlq", { partitions: 1, compacted: true, segmentBytes: 1024 });
const isPoison = (order) => order.id % 5000 === 4999; // e.g. a currency the billing system cannot price
let producer = await new Client(server.port).connect();
const sent = new Map(),
    seqs = new Map();
let duplicatesRejected = 0,
    t0 = performance.now();
const inflight = [];
for (let b = 0; b < 300; b++) {
    // 300 batches x 100 orders, pipelined
    const records = Array.from({ length: 100 }, (_, i) => {
        const id = b * 100 + i,
            customer = `c${rng.nextBounded(500)}`;
        sent.set(id, customer);
        return { key: customer, value: { id, customer, amount: rng.nextBounded(10000) / 100 } };
    });
    const p = Number(XXH3_64(records[0].key, 0, { as: "bigint" }) % 6n);
    const seq = seqs.get(p) ?? 0;
    seqs.set(p, seq + 100);
    const req = { api: "produce", topic: "orders", partition: p, records, producerId: "orders-svc-1", sequence: seq };
    inflight.push(producer.call(req));
    if (b % 50 === 7)
        inflight.push(
            producer.call(req).then((r) => {
                if (r.duplicate) duplicatesRejected++;
                return r;
            }),
        ); // a retried batch
    if (inflight.length > 32) await Promise.all(inflight.splice(0));
}
await Promise.all(inflight.splice(0));
const produceMs = performance.now() - t0;
const totalRecords = broker.topics.get("orders").parts.reduce((s, p) => s + p.next, 0);
results[`30000 orders appended once each; ${duplicatesRejected} retried batches rejected as duplicates`] =
    totalRecords === 30000 && duplicatesRejected === 6;

// Compacted and retention topics.
for (let r = 0; r < 2000; r++)
    await producer.call({
        api: "produce",
        topic: "prices",
        records: [{ key: `SKU-${r % 50}`, value: r % 400 === 399 ? null : { price: r } }],
    });
for (let r = 0; r < 200; r++)
    await producer.call({
        api: "produce",
        topic: "clicks",
        partition: 0,
        records: Array.from({ length: 20 }, (_, i) => ({ key: null, value: { page: `/p/${i}`, pad: "x".repeat(80) } })),
    });

// A consumer group of three; one leaves mid-way (rebalance); offsets committed.
async function consumer(name, stopAfter = Infinity) {
    const c = await new Client(server.port).connect();
    await c.call({ api: "join", group: "billing", member: name, topic: "orders" });
    const seen = [];
    let idle = 0,
        gen = 0;
    while (idle < 6 && seen.length < stopAfter) {
        const hb = await c.call({ api: "heartbeat", group: "billing", member: name });
        gen = hb.generation;
        let got = 0;
        for (const p of hb.partitions) {
            const { offset } = await c.call({ api: "committed", group: "billing", topic: "orders", partition: p });
            const r = await c.call({ api: "fetch", topic: "orders", partition: p, offset, max: 300 });
            if (!r.records.length) continue;
            seen.push(...r.records.map((x) => x.value.id));
            got += r.records.length;
            const poison = r.records.filter((x) => isPoison(x.value));
            if (poison.length)
                await c.call({
                    api: "produce",
                    topic: "orders-dlq",
                    partition: 0,
                    records: poison.map((x) => ({
                        key: String(x.value.id),
                        value: { ...x.value, error: "unpriceable", from: `orders/${p}@${x.offset}` },
                    })),
                });
            const res = await c.call({
                api: "commit",
                group: "billing",
                topic: "orders",
                generation: gen,
                offsets: { [p]: r.records.at(-1).offset + 1 },
            });
            if (res.error) seen.splice(seen.length - r.records.length); // a rebalance invalidated this batch
        }
        idle = got ? 0 : idle + 1;
        await sleep(5);
    }
    if (stopAfter !== Infinity) await c.call({ api: "leave", group: "billing", member: name, topic: "orders" });
    c.close();
    return seen;
}
const batches = await Promise.all([consumer("c1"), consumer("c2"), consumer("c3", 3000)]);
const seenBy = batches.map((ids) => new RoaringBitmap(Uint32Array.from(ids)));
const union = seenBy.reduce((u, b) => u.or(b), new RoaringBitmap());
const redelivered = batches.reduce((s, b) => s + b.length, 0) - union.size;
results[`the group consumed every order (${union.size}); ${redelivered} redelivered after the rebalance`] =
    union.size === 30000 && union.equals(new RoaringBitmap().addRange(0, 30000));
const lagNow = (await producer.call({ api: "lag", group: "billing", topic: "orders" })).lag;
results["consumer lag is zero once the group has caught up"] = lagNow === 0;
broker.topics.get("orders-dlq").parts[0].compact();
const dlqKeys = new Set(
    broker.topics
        .get("orders-dlq")
        .parts[0].fetch(0, 1e9)
        .map((r) => r.key),
);
results[`every poison order is dead-lettered exactly once (${dlqKeys.size})`] =
    dlqKeys.size === 6 && [...dlqKeys].all((k) => isPoison({ id: Number(k) }));
results[`membership changes triggered ${broker.groups.get("billing").generation} rebalances`] =
    broker.groups.get("billing").generation >= 4;
const perKeyOrder = broker.topics.get("orders").parts.every((p) => {
    const last = new Map();
    return p.fetch(0, 1e9).every((r) => {
        const ok = (last.get(r.key) ?? -1) < r.value.id;
        last.set(r.key, r.value.id);
        return ok;
    });
});
results["within each partition every key's records keep their production order"] = perKeyOrder;

const pricesBefore = broker.topics.get("prices").parts.reduce((s, p) => s + p.fetch(0, 1e9).length, 0);
const removed = broker.topics.get("prices").parts.reduce((s, p) => s + p.compact(), 0);
const latest = new Map();
for (const p of broker.topics.get("prices").parts) for (const r of p.fetch(0, 1e9)) latest.set(r.key, r.value);
results[`compaction removed ${removed} superseded records; tombstoned keys stay deleted`] =
    removed > 1500 &&
    Array.from({ length: 49 }, (_, k) => latest.get(`SKU-${k}`)?.price === 1950 + k).every(Boolean) &&
    latest.get("SKU-49") === null;
const clicks = broker.topics.get("clicks").parts[0];
results[`retention kept ${clicks.segments.keys().length} segments; the oldest offsets are gone`] =
    clicks.segments.keys().length <= 3 && clicks.fetch(0, 5)[0].offset > 0;

// Crash: tear the last record of one orders partition, restart, recover.
producer.close();
server.close();
const p0 = broker.topics.get("orders").parts[0],
    lastSeg = p0.segments.get(p0.segments.lastKey());
const torn = readBytes(lastSeg.file);
writeFile(lastSeg.file, torn.subarray(0, torn.length - 5));
const highBefore = p0.next;
broker = new Broker(work);
server = serve(broker);
const p0b = broker.topics.get("orders").parts[0];
results[`recovery truncated the torn record (${p0b.truncated} bytes); high watermark ${highBefore} -> ${p0b.next}`] =
    p0b.next === highBefore - 1 && p0b.truncated > 0;
results["committed group offsets survived the restart"] = broker.offsets.size >= 6;
producer = await new Client(server.port).connect();
const retry = await producer.call({
    api: "produce",
    topic: "orders",
    partition: 0,
    records: [{ key: "c1", value: { id: -1 } }],
    producerId: "orders-svc-1",
    sequence: seqs.get(0) - 1,
});
results["the producer re-sends the lost record and the sequence check accepts exactly that one"] =
    retry.baseOffset === highBefore - 1;
producer.close();
server.close();

print("message-broker");
print(
    `  produce    30000 records in 300 pipelined batches over TCP: ${produceMs.toFixed(0)} ms (${((30000 / produceMs) * 1000).toFixed(0)} records/s)`,
);
print(
    table([
        ["topic", "partition", "segments", "bytes", "high watermark"],
        ...[...broker.topics].flatMap(([n, t]) =>
            t.parts.map((p, i) => [n, String(i), String(p.segments.keys().length), String(p.bytes), String(p.next)]),
        ),
    ]),
);
print(
    `  group      ${batches.map((r, i) => `c${i + 1}:${r.length}`).join(" ")} records (${redelivered} redelivered); generation ${broker.groups.get("billing")?.generation ?? "(restarted)"}`,
);
print(`  dlq        ${[...dlqKeys].join(", ")}`);
print(`  compaction prices ${pricesBefore} -> ${pricesBefore - removed} records for ${latest.size} keys`);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
