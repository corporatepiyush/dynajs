// A plain-JS model of a docdb collection for the per-verb suites: documents in insertion order with integer _id, a
// (0..9), n (number), optional b (string) and optional tags (string array). Each filter is a pair of the docdb filter
// and an independent JS predicate; updates are the docdb operator document and an independent JS mutation; sorts are
// numeric field lists compared here, never by the engine. Order-sensitive checks compare JSON text, so a model
// update keeps MongoDB's field order: an existing field stays in place, a new one is appended.

export function rng(seed) {
    let s = seed >>> 0 || 1;
    return () => {
        s ^= s << 13; s >>>= 0;
        s ^= s >>> 17;
        s ^= s << 5; s >>>= 0;
        return s / 4294967296;
    };
}

export function genDocs(r, count, firstId = 1) {
    const out = [];
    for (let i = 0; i < count; i++) {
        const d = { _id: firstId + i, a: Math.floor(r() * 10), n: Math.floor(r() * 1000) / 4 };
        if (r() < 0.7) d.b = "s" + Math.floor(r() * 5);
        if (r() < 0.4) d.tags = Array.from({ length: 1 + Math.floor(r() * 3) }, () => "t" + Math.floor(r() * 4));
        out.push(d);
    }
    return out;
}

export function filters(r) {
    const k = Math.floor(r() * 10), v = Math.floor(r() * 1000) / 4, s = "s" + Math.floor(r() * 5), t = "t" + Math.floor(r() * 4);
    return [
        [{}, () => true],
        [{ a: k }, (d) => d.a === k],
        [{ a: { $gt: k } }, (d) => d.a > k],
        [{ a: { $in: [k, (k + 3) % 10] } }, (d) => d.a === k || d.a === (k + 3) % 10],
        [{ b: s }, (d) => d.b === s],
        [{ b: { $exists: false } }, (d) => !("b" in d)],
        [{ n: { $lte: v } }, (d) => d.n <= v],
        [{ tags: t }, (d) => Array.isArray(d.tags) && d.tags.includes(t)],
        [{ $or: [{ a: k }, { b: s }] }, (d) => d.a === k || d.b === s],
        [{ a: { $gte: k }, n: { $lt: v } }, (d) => d.a >= k && d.n < v],
        [{ _id: { $gt: 1000000 } }, () => false],
    ];
}

export function updates(r) {
    const x = Math.floor(r() * 100), inc = 1 + Math.floor(r() * 5);
    return [
        [{ $set: { x } }, (d) => { d.x = x; }],
        [{ $inc: { n: inc } }, (d) => { d.n = d.n + inc; }],
        [{ $unset: { b: "" } }, (d) => { delete d.b; }],
        [{ $set: { a: x % 10 }, $inc: { z: 1 } }, (d) => { d.a = x % 10; d.z = (d.z || 0) + 1; }],
    ];
}

export const SORTS = [
    [{ a: 1, _id: 1 }, [["a", 1], ["_id", 1]]],
    [{ a: -1, _id: 1 }, [["a", -1], ["_id", 1]]],
    [{ n: 1, _id: -1 }, [["n", 1], ["_id", -1]]],
    [{ _id: -1 }, [["_id", -1]]],
];

export function sortModel(docs, spec) {
    return docs.slice().sort((p, q) => {
        for (const [f, dir] of spec) if (p[f] !== q[f]) return p[f] < q[f] ? -dir : dir;
        return 0;
    });
}

export function project(d, proj) {
    if (!proj) return d;
    const o = { _id: d._id };
    for (const k of Object.keys(proj)) if (k !== "_id" && k in d) o[k] = d[k];
    return o;
}

export const byId = (docs) => docs.slice().sort((p, q) => p._id - q._id);
export const clone = (d) => JSON.parse(JSON.stringify(d));
export const J = JSON.stringify;
