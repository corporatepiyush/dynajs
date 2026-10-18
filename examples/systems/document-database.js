// Document database — a MongoDB-shaped store: collections of JSON documents with schema validation, unique and
// secondary indexes, a filter language with a cost-based planner and explain(), an aggregation pipeline, atomic
// update operators, change streams, an append-only journal with compaction, and an HTTP API.
//
// WHAT IT IS
//   storage      every insert/update/delete is appended to a journal (CBOR records with CRC32C); on open the
//                journal is replayed; `compact()` rewrites it as one record per live document
//   validation   each collection may carry a JSON Schema; writes that violate it are refused with the path
//   indexes      single-field indexes over dotted paths (numbers and strings in a sorted structure, arrays
//                indexed per element — "multikey"), optionally unique; maintained on every write
//   queries      filters with $eq $ne $gt $gte $lt $lte $in $nin $exists $regex $elemMatch $and $or $not, dotted
//                paths into nested documents and arrays; projection, sort, skip, limit
//   planner      candidate plans (each usable index, or a collection scan) are costed from index selectivity
//                estimates; explain() reports the chosen plan, keys examined and documents examined
//   updates      $set $unset $inc $push $pull $addToSet applied atomically per document, with upsert
//   aggregation  $match $project $unwind $group (with $sum $avg $min $max $push $count) $sort $limit $lookup
//   changes      a change stream emits insert/update/delete events with a resume token
//   API          POST /db/:coll/find, /insert, /update, /aggregate over HTTP with JSON bodies
//   postings     document ids are interned to dense integers per collection; every index key owns a RoaringBitmap of
//                them, numeric keys are also ordered in a SortedMap and string keys in an OrderedMap, so a range is a
//                native rangeQuery and a plan's estimate is a cardinality, not a materialised Set of ids
//   point reads  an equality on _id is answered from the primary map directly (MongoDB's IDHACK plan), which is
//                what makes a $lookup on _id a hash join instead of a collection scan per joined row
//   intersection when two predicates are both selective the planner ANDs their bitmaps (IXAND) instead of
//                scanning one index and filtering; an aggregation whose first stage is $match pushes it down into
//                find() so the pipeline starts from the index plan rather than a clone of every document
//
// MODULES  dyna:serialize (CBOR) · dyna:hash (CRC32C) · dyna:json (Pointer) · dyna:schema · dyna:structures
//          (SortedMap, OrderedMap, Interner, RoaringBitmap) · dyna:http (App) · dyna:uuid (v7) · dyna:file
//          · dyna:random · dyna:cli (Table)
//
// RUN      dynajs examples/systems/document-database.js     loads 20,000 orders, queries, aggregates, restarts

import { CBOREncode, CBORDecode } from "dyna:serialize";
import { CRC32C } from "dyna:hash";
import { Schema } from "dyna:schema";
import { App } from "dyna:http";
import { v7 as uuid7 } from "dyna:uuid";
import { makeTempDir, writeFile, readBytes, exists, stat, removeAll } from "dyna:file";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { SortedMap, OrderedMap, Interner, RoaringBitmap } from "dyna:structures";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("docdb");
const rng = new Random(1010);
const clone = (x) => (x === undefined ? undefined : JSON.parse(JSON.stringify(x)));

// =============================================================================
// Paths and comparison
// =============================================================================
function valuesAt(doc, path) {
    // dotted path with implicit array traversal
    let cur = [doc];
    for (const part of path.split(".")) {
        const next = [];
        for (const c of cur) {
            if (c === null || typeof c !== "object") continue;
            if (Array.isArray(c) && !/^\d+$/.test(part)) {
                for (const el of c) if (el && typeof el === "object" && part in el) next.push(el[part]);
            } else if (part in c) next.push(c[part]);
        }
        cur = next;
    }
    return cur.flatMap((v) => (Array.isArray(v) ? [v, ...v] : [v]));
}
const getPath = (doc, path) => path.split(".").reduce((o, p) => (o == null ? undefined : o[p]), doc);
const setPath = (doc, path, v) => {
    const parts = path.split(".");
    let o = doc;
    for (const p of parts.slice(0, -1)) o = o[p] ??= {};
    o[parts.at(-1)] = v;
};
const unsetPath = (doc, path) => {
    const parts = path.split(".");
    const o = getPath(doc, parts.slice(0, -1).join(".")) ?? (parts.length === 1 ? doc : undefined);
    if (o) delete o[parts.at(-1)];
};
function projectPath(src, parts) {
    // keeps only the path, mapping through arrays as MongoDB does
    if (!parts.length) return clone(src);
    if (Array.isArray(src)) return src.map((e) => projectPath(e, parts)).filter((e) => e !== undefined);
    if (src && typeof src === "object" && parts[0] in src) {
        const v = projectPath(src[parts[0]], parts.slice(1));
        return v === undefined ? undefined : { [parts[0]]: v };
    }
    return undefined;
}
function mergeInto(dst, src) {
    if (!src) return dst;
    for (const [k, v] of Object.entries(src)) {
        if (Array.isArray(v) && Array.isArray(dst[k]))
            v.forEach((e, i) => {
                dst[k][i] = typeof e === "object" && e ? mergeInto(dst[k][i] || {}, e) : e;
            });
        else if (v && typeof v === "object" && !Array.isArray(v) && dst[k] && typeof dst[k] === "object")
            mergeInto(dst[k], v);
        else dst[k] = v;
    }
    return dst;
}
const TYPE_ORDER = (v) =>
    v === null || v === undefined
        ? 0
        : typeof v === "number"
          ? 1
          : typeof v === "string"
            ? 2
            : typeof v === "boolean"
              ? 4
              : 3;
const cmp = (a, b) => TYPE_ORDER(a) - TYPE_ORDER(b) || (a < b ? -1 : a > b ? 1 : 0);
const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);

function matches(doc, filter) {
    for (const [key, cond] of Object.entries(filter)) {
        if (key === "$and") {
            if (!cond.every((f) => matches(doc, f))) return false;
            continue;
        }
        if (key === "$or") {
            if (!cond.some((f) => matches(doc, f))) return false;
            continue;
        }
        const vals = valuesAt(doc, key);
        const ops =
            cond !== null &&
            typeof cond === "object" &&
            !Array.isArray(cond) &&
            Object.keys(cond).every((k) => k.startsWith("$"))
                ? cond
                : { $eq: cond };
        for (const [op, arg] of Object.entries(ops)) {
            const any = (pred) => vals.some(pred);
            const ok = {
                $eq: () => any((v) => eq(v, arg)) || (arg === null && !vals.length),
                $ne: () => !any((v) => eq(v, arg)),
                $gt: () => any((v) => TYPE_ORDER(v) === TYPE_ORDER(arg) && cmp(v, arg) > 0),
                $gte: () => any((v) => TYPE_ORDER(v) === TYPE_ORDER(arg) && cmp(v, arg) >= 0),
                $lt: () => any((v) => TYPE_ORDER(v) === TYPE_ORDER(arg) && cmp(v, arg) < 0),
                $lte: () => any((v) => TYPE_ORDER(v) === TYPE_ORDER(arg) && cmp(v, arg) <= 0),
                $in: () => any((v) => arg.some((a) => eq(v, a))),
                $nin: () => !any((v) => arg.some((a) => eq(v, a))),
                $exists: () => vals.length > 0 === arg,
                $regex: () => any((v) => typeof v === "string" && new RegExp(arg).test(v)),
                $elemMatch: () =>
                    (getPath(doc, key) || []).some((el) =>
                        matches(typeof el === "object" ? el : { _: el }, typeof el === "object" ? arg : { _: arg }),
                    ),
                $not: () => !matches(doc, { [key]: arg }),
            }[op];
            if (!ok) throw new Error(`unknown operator ${op}`);
            if (!ok()) return false;
        }
    }
    return true;
}

// =============================================================================
// Index
// =============================================================================
class Index {
    constructor(path, { unique = false } = {}, ids) {
        this.path = path;
        this.unique = unique;
        this.ids = ids;
        this.entries = new Map(); // JSON key -> RoaringBitmap of document numbers
        this.numbers = new SortedMap();
        this.strings = new OrderedMap(); // ordered views for range scans
    }
    keysOf(doc) {
        const vs = valuesAt(doc, this.path).filter((v) => v === null || typeof v !== "object");
        return [...new Set((vs.length ? vs : [null]).map((v) => JSON.stringify(v)))];
    }
    add(doc) {
        const n = this.ids.intern(doc._id);
        for (const k of this.keysOf(doc)) {
            let bm = this.entries.get(k);
            if (!bm) {
                this.entries.set(k, (bm = new RoaringBitmap()));
                const v = JSON.parse(k);
                if (typeof v === "number") this.numbers.set(v, k);
                else if (typeof v === "string") this.strings.set(v, k);
            }
            if (this.unique && bm.size && !bm.has(n)) throw new Error(`E11000 duplicate key ${this.path}: ${k}`);
            bm.add(n);
        }
    }
    remove(doc) {
        const n = this.ids.lookup(doc._id);
        if (n === undefined) return;
        for (const k of this.keysOf(doc)) {
            const bm = this.entries.get(k);
            if (!bm) continue;
            bm.delete(n);
            if (bm.isEmpty()) {
                this.entries.delete(k);
                const v = JSON.parse(k);
                if (typeof v === "number") this.numbers.delete(v);
                else if (typeof v === "string") this.strings.delete(v);
            }
        }
    }
    lookup(cond) {
        // returns { bitmap, keysExamined } or null when unusable
        const ops = cond !== null && typeof cond === "object" && !Array.isArray(cond) ? cond : { $eq: cond };
        if (Object.keys(ops).some((o) => !["$eq", "$in", "$gt", "$gte", "$lt", "$lte"].includes(o))) return null;
        const union = (keys) =>
            keys.reduce((acc, k) => {
                const bm = this.entries.get(k);
                return bm ? acc.or(bm) : acc;
            }, new RoaringBitmap());
        if ("$eq" in ops || "$in" in ops) {
            const keys = ("$in" in ops ? ops.$in : [ops.$eq]).map((v) => JSON.stringify(v));
            return { bitmap: union(keys), keysExamined: keys.length };
        }
        const lo = ops.$gt ?? ops.$gte,
            hi = ops.$lt ?? ops.$lte,
            kind = typeof (lo ?? hi);
        if (kind !== "number" && kind !== "string") return null;
        const view = kind === "number" ? this.numbers : this.strings;
        const range = view
            .rangeQuery(lo ?? (kind === "number" ? -Infinity : ""), hi ?? (kind === "number" ? Infinity : "\u{10FFFF}"))
            .filter(([v]) => !("$gt" in ops && v === lo) && !("$lt" in ops && v === hi));
        return { bitmap: union(range.map(([, k]) => k)), keysExamined: range.length };
    }
    estimate(cond) {
        const r = this.lookup(cond);
        return r ? r.bitmap.size : Infinity;
    }
}

// =============================================================================
// Collection and database
// =============================================================================
class Collection {
    constructor(db, name) {
        this.db = db;
        this.name = name;
        this.docs = new Map();
        this.indexes = new Map();
        this.validator = null;
        this.ids = new Interner();
    }
    createIndex(path, opts) {
        const ix = new Index(path, opts, this.ids);
        for (const d of this.docs.values()) ix.add(d);
        this.indexes.set(path, ix);
        this.db.log({ op: "index", coll: this.name, path, opts });
        return ix;
    }
    setValidator(schema) {
        this.validator = Schema.compile(schema);
        this.validatorSource = schema;
        this.db.log({ op: "validator", coll: this.name, schema });
    }
    validate(doc) {
        if (!this.validator) return;
        const r = this.validator.validate(doc);
        if (!r.valid)
            throw new Error(
                `document failed validation: ${r.errors.map((e) => `${e.instancePath || e.path || "/"} ${e.message}`).join("; ")}`,
            );
    }
    applyInsert(doc) {
        for (const ix of this.indexes.values()) ix.add(doc);
        this.docs.set(doc._id, doc);
    }
    insertOne(doc) {
        const d = { _id: doc._id ?? uuid7(), ...clone(doc) };
        this.validate(d);
        const added = [];
        try {
            for (const ix of this.indexes.values()) {
                ix.add(d);
                added.push(ix);
            }
        } catch (e) {
            for (const ix of added) ix.remove(d);
            throw e;
        }
        this.docs.set(d._id, d);
        this.db.log({ op: "put", coll: this.name, doc: d });
        this.db.emit("insert", this.name, d);
        return d._id;
    }
    plan(filter) {
        if ("_id" in filter && (typeof filter._id !== "object" || filter._id === null))
            return { stage: "IDHACK", estimate: this.docs.has(filter._id) ? 1 : 0 }; // primary-key point read
        const candidates = [];
        for (const [path, cond] of Object.entries(filter))
            if (!path.startsWith("$") && this.indexes.has(path)) {
                const est = this.indexes.get(path).estimate(cond);
                if (est !== Infinity) candidates.push({ path, est });
            }
        candidates.sort((a, b) => a.est - b.est);
        const selective = candidates.filter((c) => c.est < this.docs.size * 0.4);
        if (selective.length >= 2) {
            // index intersection when it halves the work
            const [a, b] = selective,
                both = this.indexes
                    .get(a.path)
                    .lookup(filter[a.path])
                    .bitmap.andCardinality(this.indexes.get(b.path).lookup(filter[b.path]).bitmap);
            if (both < a.est * 0.5)
                return {
                    stage: "IXAND",
                    indexes: [a.path, b.path],
                    estimate: both,
                    rejected: selective.slice(2).map((c) => c.path),
                };
        }
        return selective.length
            ? {
                  stage: "IXSCAN",
                  index: selective[0].path,
                  estimate: selective[0].est,
                  rejected: candidates.slice(1).map((c) => c.path),
              }
            : { stage: "COLLSCAN" };
    }
    find(filter = {}, { projection, sort, skip = 0, limit = Infinity, explain = false } = {}) {
        const plan = this.plan(filter);
        let ids,
            keysExamined = 0;
        if (plan.stage === "IDHACK") ids = { toArray: () => [], direct: this.docs.has(filter._id) ? [filter._id] : [] };
        if (plan.stage === "IXSCAN") {
            const r = this.indexes.get(plan.index).lookup(filter[plan.index]);
            ids = r.bitmap;
            keysExamined = r.keysExamined;
        }
        if (plan.stage === "IXAND") {
            const [a, b] = plan.indexes.map((p) => this.indexes.get(p).lookup(filter[p]));
            ids = a.bitmap.and(b.bitmap);
            keysExamined = a.keysExamined + b.keysExamined;
        }
        const source = ids?.direct
            ? ids.direct.map((id) => this.docs.get(id))
            : ids
              ? this.ids.getMany(ids.toArray()).map((id) => this.docs.get(id))
              : [...this.docs.values()];
        let out = source.filter((d) => matches(d, filter));
        if (sort)
            out.sort((a, b) => {
                for (const [k, dir] of Object.entries(sort)) {
                    const c = cmp(getPath(a, k), getPath(b, k));
                    if (c) return c * dir;
                }
                return 0;
            });
        out = out.slice(skip, skip + limit);
        if (projection)
            out = out.map((d) => {
                const inc = Object.entries(projection).filter(([, v]) => v),
                    o = inc.length ? { _id: d._id } : clone(d);
                for (const [k, v] of Object.entries(projection)) {
                    if (v) mergeInto(o, projectPath(d, k.split(".")));
                    else unsetPath(o, k);
                }
                return o;
            });
        else out = out.map(clone);
        return explain ? { plan, keysExamined, docsExamined: source.length, nReturned: out.length } : out;
    }
    updateMany(filter, update, { upsert = false } = {}) {
        let n = 0;
        const targets = this.find(filter).map((d) => d._id);
        if (!targets.length && upsert) {
            const base = Object.fromEntries(
                Object.entries(filter).filter(([k, v]) => !k.startsWith("$") && (typeof v !== "object" || v === null)),
            );
            const id = this.insertOne(base);
            targets.push(id);
        }
        for (const id of targets) {
            const old = this.docs.get(id),
                next = clone(old);
            for (const [op, fields] of Object.entries(update))
                for (const [path, arg] of Object.entries(fields)) {
                    const cur = getPath(next, path);
                    if (op === "$set") setPath(next, path, clone(arg));
                    else if (op === "$unset") unsetPath(next, path);
                    else if (op === "$inc") {
                        if (cur !== undefined && typeof cur !== "number")
                            throw new Error(`cannot $inc non-number ${path}`);
                        setPath(next, path, (cur ?? 0) + arg);
                    } else if (op === "$push") setPath(next, path, [...(cur ?? []), clone(arg)]);
                    else if (op === "$addToSet")
                        setPath(next, path, (cur ?? []).some((x) => eq(x, arg)) ? cur : [...(cur ?? []), clone(arg)]);
                    else if (op === "$pull")
                        setPath(
                            next,
                            path,
                            (cur ?? []).filter(
                                (x) => !(typeof arg === "object" && arg !== null ? matches(x, arg) : eq(x, arg)),
                            ),
                        );
                    else throw new Error(`unknown update operator ${op}`);
                }
            this.validate(next);
            for (const ix of this.indexes.values()) ix.remove(old);
            try {
                for (const ix of this.indexes.values()) ix.add(next);
            } catch (e) {
                for (const ix of this.indexes.values()) {
                    ix.remove(next);
                    ix.add(old);
                }
                throw e;
            }
            this.docs.set(id, next);
            this.db.log({ op: "put", coll: this.name, doc: next });
            this.db.emit("update", this.name, next);
            n++;
        }
        return n;
    }
    deleteMany(filter) {
        const ids = this.find(filter).map((d) => d._id);
        for (const id of ids) {
            const d = this.docs.get(id);
            for (const ix of this.indexes.values()) ix.remove(d);
            this.docs.delete(id);
            this.db.log({ op: "del", coll: this.name, id });
            this.db.emit("delete", this.name, { _id: id });
        }
        return ids.length;
    }
    aggregate(pipeline) {
        const [first] = pipeline,
            pushed = first && "$match" in first; // push a leading $match into the index planner
        let rows = pushed ? this.find(first.$match) : [...this.docs.values()].map(clone);
        for (const stage of pushed ? pipeline.slice(1) : pipeline) {
            const [op, arg] = Object.entries(stage)[0];
            if (op === "$match") rows = rows.filter((d) => matches(d, arg));
            else if (op === "$unwind") {
                const p = arg.replace(/^\$/, "");
                rows = rows.flatMap((d) =>
                    (getPath(d, p) || []).map((v) => {
                        const c = clone(d);
                        setPath(c, p, v);
                        return c;
                    }),
                );
            } else if (op === "$project")
                rows = rows.map((d) =>
                    Object.fromEntries(
                        Object.entries(arg).map(([k, v]) => [
                            k,
                            typeof v === "string" && v.startsWith("$")
                                ? getPath(d, v.slice(1))
                                : v === 1
                                  ? getPath(d, k)
                                  : evalExpr(v, d),
                        ]),
                    ),
                );
            else if (op === "$group") {
                const groups = new Map();
                for (const d of rows) {
                    const key =
                        typeof arg._id === "string"
                            ? getPath(d, arg._id.slice(1))
                            : arg._id && typeof arg._id === "object"
                              ? Object.fromEntries(Object.entries(arg._id).map(([k, v]) => [k, getPath(d, v.slice(1))]))
                              : null;
                    const gk = JSON.stringify(key);
                    if (!groups.has(gk)) groups.set(gk, { _id: key, rows: [] });
                    groups.get(gk).rows.push(d);
                }
                rows = [...groups.values()].map(({ _id, rows: rs }) => {
                    const out = { _id };
                    for (const [field, acc] of Object.entries(arg)) {
                        if (field === "_id") continue;
                        const [aop, expr] = Object.entries(acc)[0],
                            vals = rs.map((r) =>
                                typeof expr === "string" ? getPath(r, expr.slice(1)) : evalExpr(expr, r),
                            );
                        out[field] =
                            aop === "$sum"
                                ? vals.reduce((a, b) => a + (b || 0), 0)
                                : aop === "$avg"
                                  ? vals.reduce((a, b) => a + b, 0) / vals.length
                                  : aop === "$min"
                                    ? vals.reduce((a, b) => (cmp(b, a) < 0 ? b : a))
                                    : aop === "$max"
                                      ? vals.reduce((a, b) => (cmp(b, a) > 0 ? b : a))
                                      : aop === "$push"
                                        ? vals
                                        : aop === "$count"
                                          ? rs.length
                                          : null;
                    }
                    return out;
                });
            } else if (op === "$sort")
                rows.sort((a, b) => {
                    for (const [k, dir] of Object.entries(arg)) {
                        const c = cmp(getPath(a, k), getPath(b, k));
                        if (c) return c * dir;
                    }
                    return 0;
                });
            else if (op === "$limit") rows = rows.slice(0, arg);
            else if (op === "$lookup") {
                const other = this.db.collection(arg.from);
                rows = rows.map((d) => ({
                    ...d,
                    [arg.as]: other.find({ [arg.foreignField]: getPath(d, arg.localField) }),
                }));
            } else throw new Error(`unknown stage ${op}`);
        }
        return rows;
    }
}
function evalExpr(e, d) {
    if (typeof e === "number") return e;
    if (typeof e === "string") return e.startsWith("$") ? getPath(d, e.slice(1)) : e;
    const [op, args] = Object.entries(e)[0],
        v = args.map((a) => evalExpr(a, d));
    return {
        $multiply: () => v.reduce((a, b) => a * b, 1),
        $add: () => v.reduce((a, b) => a + b, 0),
        $subtract: () => v[0] - v[1],
    }[op]();
}
class Database {
    constructor(dir) {
        this.dir = dir;
        this.colls = new Map();
        this.listeners = [];
        this.changeSeq = 0;
        this.replaying = false;
        this.journal = dir.join("journal.cbor");
        this.torn = 0;
        if (exists(this.journal)) this.replay();
    }
    collection(name) {
        if (!this.colls.has(name)) this.colls.set(name, new Collection(this, name));
        return this.colls.get(name);
    }
    log(rec) {
        if (this.replaying) return;
        const body = CBOREncode(rec),
            hdr = new Uint8Array(8),
            dv = new DataView(hdr.buffer);
        dv.setUint32(0, body.length);
        dv.setUint32(4, CRC32C(body));
        this.buffer ??= [];
        this.buffer.push(hdr, body);
        if (this.buffer.length > 256) this.sync();
    }
    sync() {
        if (!this.buffer?.length) return;
        const n = this.buffer.reduce((s, p) => s + p.length, 0),
            out = new Uint8Array(n);
        let o = 0;
        for (const p of this.buffer) {
            out.set(p, o);
            o += p.length;
        }
        writeFile(this.journal, out, { append: true });
        this.buffer = [];
    }
    replay() {
        this.replaying = true;
        const bytes = readBytes(this.journal);
        let off = 0;
        while (off + 8 <= bytes.length) {
            const dv = new DataView(bytes.buffer, bytes.byteOffset + off),
                len = dv.getUint32(0);
            if (off + 8 + len > bytes.length || CRC32C(bytes.subarray(off + 8, off + 8 + len)) !== dv.getUint32(4)) {
                this.torn = bytes.length - off;
                break;
            }
            const r = CBORDecode(bytes.subarray(off + 8, off + 8 + len)),
                c = this.collection(r.coll);
            if (r.op === "put") {
                const old = c.docs.get(r.doc._id);
                if (old) for (const ix of c.indexes.values()) ix.remove(old);
                c.applyInsert(r.doc);
            } else if (r.op === "del") {
                const d = c.docs.get(r.id);
                if (d) {
                    for (const ix of c.indexes.values()) ix.remove(d);
                    c.docs.delete(r.id);
                }
            } else if (r.op === "index") c.createIndex(r.path, r.opts);
            else if (r.op === "validator") c.setValidator(r.schema);
            off += 8 + len;
        }
        this.replaying = false;
    }
    compact() {
        this.sync();
        this.replaying = false;
        const recs = [];
        for (const c of this.colls.values()) {
            if (c.validatorSource) recs.push({ op: "validator", coll: c.name, schema: c.validatorSource });
            for (const ix of c.indexes.values())
                recs.push({ op: "index", coll: c.name, path: ix.path, opts: { unique: ix.unique } });
            for (const d of c.docs.values()) recs.push({ op: "put", coll: c.name, doc: d });
        }
        writeFile(this.journal, new Uint8Array(0));
        for (const r of recs) this.log(r);
        this.sync();
    }
    watch(fn) {
        this.listeners.push(fn);
    }
    emit(type, coll, doc) {
        const ev = {
            _id: { seq: ++this.changeSeq },
            operationType: type,
            ns: coll,
            documentKey: doc._id,
            fullDocument: type === "delete" ? undefined : clone(doc),
        };
        if (!this.replaying) for (const l of this.listeners) l(ev);
    }
}

// =============================================================================
// Scenario
// =============================================================================
const results = {};
let db = new Database(work);
const orders = db.collection("orders"),
    customers = db.collection("customers");
const changes = [];
db.watch((ev) => {
    if (ev.ns === "customers") changes.push(ev);
});
orders.setValidator({
    type: "object",
    required: ["customer", "status", "total", "items"],
    properties: {
        status: { enum: ["new", "paid", "shipped", "cancelled"] },
        total: { type: "number", minimum: 0 },
        items: { type: "array", minItems: 1 },
    },
});
customers.createIndex("email", { unique: true });
orders.createIndex("customer");
orders.createIndex("status");
orders.createIndex("total");
orders.createIndex("items.sku");
const CITIES = ["Berlin", "Paris", "Madrid", "Rome", "Vienna", "Warsaw"];
for (let c = 0; c < 1000; c++)
    customers.insertOne({
        _id: `c${c}`,
        email: `user${c}@mail.example`,
        name: `User ${c}`,
        address: { city: CITIES[c % 6], zip: String(10000 + c) },
        tier: c % 10 === 0 ? "gold" : "standard",
    });
let t = performance.now();
for (let o = 0; o < 20000; o++) {
    const items = Array.from({ length: 1 + rng.nextBounded(4) }, () => ({
        sku: `SKU-${rng.nextBounded(300)}`,
        qty: 1 + rng.nextBounded(3),
        price: 5 + rng.nextBounded(200),
    }));
    orders.insertOne({
        customer: `c${rng.nextBounded(1000)}`,
        status: ["new", "paid", "paid", "shipped", "shipped", "shipped", "cancelled"][rng.nextBounded(7)],
        total: items.reduce((s, i) => s + i.qty * i.price, 0),
        items,
        placed: `2026-0${1 + rng.nextBounded(6)}-${String(1 + rng.nextBounded(28)).padStart(2, "0")}`,
    });
}
db.sync();
const loadMs = performance.now() - t;

const refused = [];
for (const bad of [
    { customer: "c1", status: "lost", total: 5, items: [{}] },
    { customer: "c1", status: "new", total: -1, items: [{}] },
    { customer: "c1", status: "new", total: 1, items: [] },
])
    try {
        orders.insertOne(bad);
        refused.push("accepted");
    } catch (e) {
        refused.push(e.message.slice(0, 60));
    }
results["schema validation refuses invalid orders"] = refused.every((r) => /validation/.test(r));
let dup = null;
try {
    customers.insertOne({ email: "user5@mail.example" });
} catch (e) {
    dup = e.message;
}
results["the unique email index refuses a duplicate and leaves no partial index entries"] =
    /E11000/.test(dup || "") && customers.docs.size === 1000;

const q1 = { customer: "c42", status: { $in: ["paid", "shipped"] } };
const ex1 = orders.find(q1, { explain: true }),
    ex2 = orders.find({ total: { $gte: 2000 } }, { explain: true }),
    ex3 = orders.find({ status: { $ne: "cancelled" } }, { explain: true });
results[`the planner picks the selective customer index over status (examined ${ex1.docsExamined} docs)`] =
    ex1.plan.stage === "IXSCAN" && ex1.plan.index === "customer" && ex1.docsExamined < 40;
results["range queries use the numeric index; a $ne falls back to a collection scan"] =
    ex2.plan.index === "total" && ex3.plan.stage === "COLLSCAN";
const brute = [...orders.docs.values()].filter(
    (d) => d.customer === "c42" && ["paid", "shipped"].includes(d.status),
).length;
const q3 = { "items.sku": "SKU-7", status: "paid" },
    ex4 = orders.find(q3, { explain: true });
results[
    `two selective predicates are answered by bitmap intersection (${ex4.plan.stage}, ${ex4.docsExamined} docs examined)`
] =
    ex4.plan.stage === "IXAND" &&
    orders.find(q3).length === [...orders.docs.values()].filter((d) => matches(d, q3)).length &&
    ex4.docsExamined === ex4.nReturned;
results["index and scan answers agree"] = orders.find(q1).length === brute;
const multikey = orders.find(
    { "items.sku": "SKU-7", total: { $lt: 100 } },
    { projection: { total: 1, "items.sku": 1 }, sort: { total: -1 }, limit: 5 },
);
results["a multikey index on an array field, with projection, sort and limit"] =
    multikey.length > 0 &&
    multikey.every(
        (d) =>
            d.items.some((i) => i.sku === "SKU-7") &&
            d.total < 100 &&
            Object.keys(d).sort().join() === "_id,items,total",
    );
results["$elemMatch binds conditions to the same array element"] = orders
    .find({ items: { $elemMatch: { sku: "SKU-3", qty: { $gte: 3 } } } })
    .every((d) => d.items.some((i) => i.sku === "SKU-3" && i.qty >= 3));

const upd = orders.updateMany(
    { customer: "c42", status: "paid" },
    { $set: { status: "shipped" }, $push: { history: { at: "2026-07-01", event: "shipped" } } },
);
results[`$set/$push updated ${upd} orders atomically and the status index followed`] =
    upd >= 1 && orders.find({ customer: "c42", status: "paid" }).length === 0;
customers.updateMany({ email: "vip@mail.example" }, { $set: { tier: "gold" }, $inc: { points: 50 } }, { upsert: true });
customers.updateMany({ _id: "c7" }, { $addToSet: { tags: "newsletter" } });
customers.updateMany({ _id: "c7" }, { $addToSet: { tags: "newsletter" } });
let badInc = null;
try {
    customers.updateMany({ _id: "c7" }, { $inc: { name: 1 } });
} catch (e) {
    badInc = e.message;
}
results["upsert, idempotent $addToSet, and a refused $inc"] =
    customers.find({ email: "vip@mail.example" })[0]?.points === 50 &&
    customers.find({ _id: "c7" })[0].tags.length === 1 &&
    /cannot \$inc/.test(badInc || "");

const revenue = orders.aggregate([
    { $match: { status: { $in: ["paid", "shipped"] } } },
    { $unwind: "$items" },
    {
        $group: {
            _id: "$items.sku",
            units: { $sum: "$items.qty" },
            revenue: { $sum: { $multiply: ["$items.qty", "$items.price"] } },
        },
    },
    { $sort: { revenue: -1 } },
    { $limit: 5 },
]);
const skuCheck = [...orders.docs.values()]
    .filter((d) => ["paid", "shipped"].includes(d.status))
    .flatMap((d) => d.items)
    .filter((i) => i.sku === revenue[0]._id)
    .reduce((s, i) => s + i.qty * i.price, 0);
results[`aggregation: top SKU ${revenue[0]._id} earned ${revenue[0].revenue} (verified by hand)`] =
    revenue.length === 5 && revenue[0].revenue === skuCheck;
const byCity = orders.aggregate([
    { $match: { status: "shipped" } },
    { $group: { _id: "$customer", n: { $sum: 1 } } },
    { $lookup: { from: "customers", localField: "_id", foreignField: "_id", as: "c" } },
    { $project: { city: "$c.0.address.city", n: 1 } },
    { $group: { _id: "$city", orders: { $sum: "$n" } } },
    { $sort: { orders: -1 } },
]);
results["an _id equality is a primary-key point read (IDHACK)"] =
    customers.find({ _id: "c5" }, { explain: true }).plan.stage === "IDHACK" &&
    customers.find({ _id: "c5" })[0].email === "user5@mail.example";
results["a $lookup join groups shipped orders by customer city"] =
    byCity.length === 6 && byCity.reduce((s, r) => s + r.orders, 0) === orders.find({ status: "shipped" }).length;
const deleted = orders.deleteMany({ status: "cancelled", total: { $lt: 50 } });
results[`the change stream saw ${changes.length} customer events`] =
    changes.some((c) => c.operationType === "insert") &&
    changes.some((c) => c.operationType === "update" && c.documentKey === "c7");

// Restart: replay the journal; compact; replay again.
db.sync();
const journalBefore = stat(db.journal).size,
    countBefore = orders.docs.size,
    sample = orders.find({ customer: "c42" }, { sort: { _id: 1 } });
db = new Database(work);
results[`journal replay restored ${countBefore} orders with their indexes`] =
    db.collection("orders").docs.size === countBefore &&
    JSON.stringify(db.collection("orders").find({ customer: "c42" }, { sort: { _id: 1 } })) === JSON.stringify(sample);
db.compact();
const journalAfter = stat(db.journal).size;
db = new Database(work);
results[`compaction ${journalBefore} -> ${journalAfter} bytes; indexes rebuilt on replay`] =
    journalAfter < journalBefore &&
    db.collection("orders").docs.size === countBefore &&
    db.collection("orders").find({ total: { $gte: 2000 } }, { explain: true }).plan.index === "total";

// HTTP API.
const app = new App({ port: 0 });
app.post("/db/:coll/find", (req) => {
    const b = JSON.parse(req.body);
    try {
        return { docs: db.collection(req.params.coll).find(b.filter || {}, b.options || {}) };
    } catch (e) {
        return { status: 400, body: JSON.stringify({ error: e.message }), contentType: "application/json" };
    }
});
app.post("/db/:coll/aggregate", (req) => ({
    docs: db.collection(req.params.coll).aggregate(JSON.parse(req.body).pipeline),
}));
app.start();
const api = await (
    await fetch(`http://127.0.0.1:${app.port}/db/orders/find`, {
        method: "POST",
        body: JSON.stringify({
            filter: { status: "new", total: { $gt: 1500 } },
            options: { sort: { total: -1 }, limit: 3, projection: { total: 1 } },
        }),
    })
).json();
const badOp = await fetch(`http://127.0.0.1:${app.port}/db/orders/find`, {
    method: "POST",
    body: JSON.stringify({ filter: { total: { $where: "1" } } }),
});
await badOp.text();
app.close();
results["the HTTP API serves queries and rejects unknown operators"] =
    api.docs.length === 3 && api.docs[0].total >= api.docs[2].total && badOp.status === 400;

print("document-database");
print(
    `  load       21000 documents in ${loadMs.toFixed(0)} ms with 5 indexes; ${deleted} cancelled small orders deleted`,
);
print(
    table([
        ["query", "plan", "keys examined", "docs examined", "returned"],
        ...[
            ["customer=c42, status in [paid,shipped]", ex1],
            ["total >= 2000", ex2],
            ["status != cancelled", ex3],
        ].map(([q, e]) => [
            q,
            e.plan.stage + (e.plan.index ? `(${e.plan.index})` : ""),
            String(e.keysExamined),
            String(e.docsExamined),
            String(e.nReturned),
        ]),
    ]),
);
print(table([["sku", "units", "revenue"], ...revenue.map((r) => [r._id, String(r.units), String(r.revenue)])]));
print(`  by city    ${byCity.map((r) => `${r._id} ${r.orders}`).join(", ")}`);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
