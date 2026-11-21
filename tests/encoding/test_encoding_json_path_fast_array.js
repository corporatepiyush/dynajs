// covers: c:src/modules/encoding/encoding-jsonpath.c
// JSONPath child/wildcard/descendant steps over a plain dense Array read the
// elements straight from the fast-array storage (JS_GetFastArray) instead of
// listing own property names and fetching each index through
// JS_GetOwnProperty; sparse arrays, arrays with extra keys or accessor
// elements, Proxies and typed arrays keep the generic path. Modperf round
// 2026-11-15: 4 queries over 1K records 0.88 ms -> 0.79 ms. Oracle: a small
// JS evaluator for the selectors used here ($, .name, [*], [n], ..*), whose
// array walk uses Object.keys (own enumerable index keys, ascending) and whose
// descendant walk emits a node's children before recursing into each child.
import { suite } from "../lib/kit.js";
import * as e from "dyna:encoding";

const t = suite("test_encoding_json_path_fast_array");

function kids(v) {
    if (v === null || typeof v !== "object") return [];
    if (Array.isArray(v)) return Object.keys(v).filter((k) => /^(0|[1-9]\d*)$/.test(k)).map((k) => [+k, v[k]]);
    return Object.keys(v).map((k) => [k, v[k]]);
}
function seg(k) {
    return typeof k === "number" ? "[" + k + "]" : "['" + k + "']";
}
function desc(v, out) {
    const ks = kids(v);
    for (const [k, c] of ks) out.push([seg(k), c]);
    for (const [k, c] of ks) for (const [p, x] of desc(c, [])) out.push([seg(k) + p, x]);
    return out;
}
function evalRef(steps, root) {
    let cur = [["$", root]];
    for (const s of steps) {
        const next = [];
        for (const [p, v] of cur) {
            if (s === "*") for (const [k, c] of kids(v)) next.push([p + (typeof k === "number" ? "[" + k + "]" : "['" + k + "']"), c]);
            else if (s === "..*") for (const [q, c] of desc(v, [])) next.push([p + q, c]);
            else if (typeof s === "number") {
                if (Array.isArray(v) && Object.prototype.hasOwnProperty.call(v, s)) next.push([p + "[" + s + "]", v[s]]);
            } else if (v !== null && typeof v === "object" && !Array.isArray(v) && Object.prototype.hasOwnProperty.call(v, s)) next.push([p + "['" + s + "']", v[s]]);
        }
        cur = next;
    }
    return cur;
}

const sparse = [1, , 3];
sparse[6] = 7;
const extra = [4, 5];
extra.tag = "x";
const doc = {
    list: Array.from({ length: 50 }, (_, i) => ({ id: i, v: [i, [i * 2, { w: i }]] })),
    sparse,
    extra,
    nested: [[1, [2, [3, [4]]]], [], [{ a: [9] }]],
};

t.test("wildcards and indexes over dense, sparse and keyed arrays match the oracle", ({ eq }) => {
    const queries = [["$[*]", ["*"]], ["$.list[*].id", ["list", "*", "id"]], ["$.list[*].v[*]", ["list", "*", "v", "*"]], ["$.sparse[*]", ["sparse", "*"]], ["$.extra[*]", ["extra", "*"]], ["$.nested[*][*]", ["nested", "*", "*"]], ["$.list[3].v[1][1].w", ["list", 3, "v", 1, 1, "w"]], ["$.nested..*", ["nested", "..*"]]];
    for (const [q, steps] of queries) {
        const p = new e.JSONPath(q);
        const want = evalRef(steps, doc);
        eq(JSON.stringify(p.all(doc)), JSON.stringify(want.map((x) => x[1])), q + " values");
        eq(JSON.stringify(p.paths(doc).length), JSON.stringify(want.length), q + " path count");
    }
});

t.test("paths of dense array children carry their indexes in order", ({ eq }) => {
    const p = new e.JSONPath("$.list[*].v[0]");
    eq(p.paths(doc).join("|"), doc.list.map((_, i) => "$['list'][" + i + "]['v'][0]").join("|"), "paths");
    const q = new e.JSONPath("$[*]");
    const arr = Array.from({ length: 3000 }, (_, i) => i * 3);
    eq(q.all(arr).join(","), arr.join(","), "3000 elements in order");
});

await t.run();
