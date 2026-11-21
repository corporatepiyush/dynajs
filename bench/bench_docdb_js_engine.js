// The pure-JS document database that examples/systems/document-database.js was before it moved onto dyna:docdb,
// kept verbatim (engine only: paths, compiled filters, the bitmap-posting Index, Collection with planner and
// aggregation, Database with its CBOR journal) so bench/bench_docdb.js can time the same workload on both engines.
import { CBOREncode, CBORDecode } from "dyna:serialize";
import { CRC32C } from "dyna:hash";
import { concat } from "dyna:bytes";
import { Schema } from "dyna:schema";
import { v7 as uuid7 } from "dyna:uuid";
import { makeTempDir, writeFile, readBytes, exists, stat, removeAll } from "dyna:file";
import { SortedMap, OrderedMap, Interner, RoaringBitmap } from "dyna:structures";

const clone = (x) => (x === undefined ? undefined : structuredClone(x));

// =============================================================================
// Paths and comparison
// =============================================================================
// a dotted path is parsed once: its parts, and whether each part is an array position
const PATHS = new Map();
function pathOf(path) {
    let parts = PATHS.get(path);
    if (!parts) PATHS.set(path, (parts = path.split(".").map((part) => ({ part, position: /^\d+$/.test(part) }))));
    return parts;
}
function valuesAt(doc, path) {
    // dotted path with implicit array traversal; a path that meets no array allocates one result array
    const parts = pathOf(path);
    let v = doc;
    for (let i = 0; i < parts.length; i++) {
        if (v === null || typeof v !== "object" || Array.isArray(v)) {
            if (Array.isArray(v)) break;
            return [];
        }
        if (!(parts[i].part in v)) return [];
        v = v[parts[i].part];
        if (i === parts.length - 1) return Array.isArray(v) ? [v, ...v] : [v];
    }
    let cur = [doc];
    for (const { part, position } of pathOf(path)) {
        const next = [];
        for (const c of cur) {
            if (c === null || typeof c !== "object") continue;
            if (Array.isArray(c) && !position) {
                for (const el of c) if (el && typeof el === "object" && part in el) next.push(el[part]);
            } else if (part in c) next.push(c[part]);
        }
        cur = next;
    }
    return cur.flatMap((v) => (Array.isArray(v) ? [v, ...v] : [v]));
}
const getPath = (doc, path) => pathOf(path).reduce((o, { part }) => (o == null ? undefined : o[part]), doc);
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
const eq = (a, b) =>
    a === b || (typeof a === "object" && typeof b === "object" && JSON.stringify(a) === JSON.stringify(b));

// A filter is compiled once into closures (memoised per filter object): one predicate per operator, $regex
// built once, $in as a Set for scalars. Matching a document then allocates only the values found at each path.
const compiled = new WeakMap();
function matches(doc, filter) {
    let f = compiled.get(filter);
    if (!f) compiled.set(filter, (f = compileFilter(filter)));
    return f(doc);
}
const isOps = (cond) =>
    cond !== null &&
    typeof cond === "object" &&
    !Array.isArray(cond) &&
    Object.keys(cond).every((k) => k.startsWith("$"));
function compileFilter(filter) {
    const preds = Object.entries(filter).map(([key, cond]) => {
        if (key === "$and") {
            const fs = cond.map(compileFilter);
            return (d) => fs.every((f) => f(d));
        }
        if (key === "$or") {
            const fs = cond.map(compileFilter);
            return (d) => fs.some((f) => f(d));
        }
        const tests = Object.entries(isOps(cond) ? cond : { $eq: cond }).map(([op, arg]) => compileOp(key, op, arg));
        return (d) => {
            const vals = valuesAt(d, key);
            return tests.every((t) => t(d, vals));
        };
    });
    return (d) => preds.every((p) => p(d));
}
function compileOp(key, op, arg) {
    const ordered = (test) => (d, vals) => vals.some((v) => TYPE_ORDER(v) === TYPE_ORDER(arg) && test(cmp(v, arg)));
    const inSet = (list) => {
        const scalars = new Set(list.filter((a) => a === null || typeof a !== "object")),
            objects = list.filter((a) => a !== null && typeof a === "object");
        return (v) => scalars.has(v) || objects.some((a) => eq(v, a));
    };
    switch (op) {
        case "$eq":
            return (d, vals) => vals.some((v) => eq(v, arg)) || (arg === null && !vals.length);
        case "$ne":
            return (d, vals) => !vals.some((v) => eq(v, arg));
        case "$gt":
            return ordered((c) => c > 0);
        case "$gte":
            return ordered((c) => c >= 0);
        case "$lt":
            return ordered((c) => c < 0);
        case "$lte":
            return ordered((c) => c <= 0);
        case "$in": {
            const has = inSet(arg);
            return (d, vals) => vals.some(has);
        }
        case "$nin": {
            const has = inSet(arg);
            return (d, vals) => !vals.some(has);
        }
        case "$exists":
            return (d, vals) => vals.length > 0 === arg;
        case "$regex": {
            const re = new RegExp(arg);
            return (d, vals) => vals.some((v) => typeof v === "string" && re.test(v));
        }
        case "$elemMatch": {
            const onObject = compileFilter(arg),
                onScalar = compileFilter({ _: arg });
            return (d) =>
                (getPath(d, key) || []).some((el) => (typeof el === "object" ? onObject(el) : onScalar({ _: el })));
        }
        case "$not": {
            const inner = compileFilter({ [key]: arg });
            return (d) => !inner(d);
        }
    }
    throw new Error(`unknown operator ${op}`);
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
        const n = this.ids.intern(doc._id),
            keys = this.keysOf(doc);
        // once one document holds several keys, a two-sided range may be met by two different elements
        if (keys.length > 1) this.multikey = true;
        for (const k of keys) {
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
        const view = kind === "number" ? this.numbers : this.strings,
            bottom = kind === "number" ? -Infinity : "",
            top = kind === "number" ? Infinity : "\u{10FFFF}";
        const scan = (from, to) => {
            const range = view
                .rangeQuery(from ?? bottom, to ?? top)
                .filter(
                    ([v]) =>
                        !(from !== undefined && "$gt" in ops && v === lo) &&
                        !(to !== undefined && "$lt" in ops && v === hi),
                );
            return { bitmap: union(range.map(([, k]) => k)), keysExamined: range.length };
        };
        if (!this.multikey || lo === undefined || hi === undefined) return scan(lo, hi);
        // multikey: { $gte: a, $lte: b } holds when SOME element is >= a and SOME element is <= b, so the
        // candidates are the intersection of the two one-sided ranges, not the keys between a and b
        const above = scan(lo, undefined),
            below = scan(undefined, hi);
        return { bitmap: above.bitmap.and(below.bitmap), keysExamined: above.keysExamined + below.keysExamined };
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
        const candidates = Object.entries(filter)
            .filter(([path]) => !path.startsWith("$") && this.indexes.has(path))
            .map(([path, cond]) => ({ path, est: this.indexes.get(path).estimate(cond) }))
            .filter((c) => c.est !== Infinity)
            .sort((a, b) => a.est - b.est);
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
        else if (!explain) out = out.map(clone);
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
                // each output row shares the input row's untouched fields and copies only the objects on the path
                const p = arg.replace(/^\$/, ""),
                    parts = pathOf(p).map(({ part }) => part);
                const withPath = (o, i, v) =>
                    i === parts.length - 1
                        ? { ...o, [parts[i]]: v }
                        : { ...o, [parts[i]]: withPath(o?.[parts[i]] ?? {}, i + 1, v) };
                rows = rows.flatMap((d) => (getPath(d, p) || []).map((v) => withPath(d, 0, v)));
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
        writeFile(this.journal, concat(this.buffer), { append: true });
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
                // a torn tail (crash mid-append) is cut off, or every later append would sit behind it, unreadable
                this.torn = bytes.length - off;
                writeFile(this.journal, bytes.subarray(0, off));
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
        const recs = this.colls
            .values()
            .flatMap((c) =>
                Iterator.concat(
                    c.validatorSource ? [{ op: "validator", coll: c.name, schema: c.validatorSource }] : [],
                    c.indexes
                        .values()
                        .map((ix) => ({ op: "index", coll: c.name, path: ix.path, opts: { unique: ix.unique } })),
                    c.docs.values().map((d) => ({ op: "put", coll: c.name, doc: d })),
                ),
            )
            .toArray();
        writeFile(this.journal, new Uint8Array(0));
        for (const r of recs) this.log(r);
        this.sync();
    }
    watch(fn, { ns } = {}) {
        // a stream on one collection costs nothing for writes to the others: no event object, no document copy
        this.listeners.push({ fn, ns });
    }
    emit(type, coll, doc) {
        const seq = ++this.changeSeq,
            to = this.replaying ? [] : this.listeners.filter((l) => l.ns === undefined || l.ns === coll);
        if (!to.length) return;
        const ev = {
            _id: { seq },
            operationType: type,
            ns: coll,
            documentKey: doc._id,
            fullDocument: type === "delete" ? undefined : clone(doc),
        };
        for (const l of to) l.fn(ev);
    }
}

export { Database, matches };
