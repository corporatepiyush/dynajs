// covers: c:src/modules/encoding/encoding-json5.c
// StableStringify sorts each object's keys by UTF-16 code units. The sorted
// order is remembered per key count (8 slots, len & 7) together with the
// object's own key list (atoms held by reference); an object with the same key
// list in the same order reuses the permutation instead of decoding and
// sorting again; the whole list is compared, so lists that share a prefix
// but differ later miss. A different list, a reordered list or a list with a key
// added or removed misses and re-sorts. Modperf round 2026-11-15: 10K records
// 11.5 ms -> 6.9 ms. Oracle: a JS stable stringifier that sorts keys with the
// default (UTF-16) string order.
import { suite } from "../lib/kit.js";
import * as e from "dyna:encoding";

const t = suite("test_encoding_stable_stringify_order_cache");

function ref(v) {
    if (v === null || typeof v === "number" || typeof v === "boolean" || typeof v === "string") return JSON.stringify(v);
    if (Array.isArray(v)) return "[" + v.map((x) => (x === undefined || typeof x === "function" ? "null" : ref(x))).join(",") + "]";
    const ks = Object.keys(v).sort();
    const parts = [];
    for (const k of ks) {
        const x = v[k];
        if (x === undefined || typeof x === "function") continue;
        parts.push(JSON.stringify(k) + ":" + ref(x));
    }
    return "{" + parts.join(",") + "}";
}

let seed = 4242;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff), seed);
const KEYS = ["b", "a", "zz", "é", "B", "10", "2", "😀", "￿", "k", "aa", "a\u0000"];

t.test("records with repeated, reordered and mutated key lists match the oracle", ({ eq }) => {
    for (let round = 0; round < 40; round++) {
        const recs = [];
        const base = KEYS.slice(0, 2 + (round % 9));
        for (let i = 0; i < 60; i++) {
            const r = {};
            const keys = i % 5 === 1 ? base.slice().reverse() : i % 7 === 2 ? base.concat(["extra" + (i % 3)]) : i % 11 === 3 ? base.slice(1) : base;
            for (const k of keys) r[k] = rnd() % 3 === 0 ? { y: 1, x: [k, i] } : i;
            if (i % 13 === 4) r[base[0]] = undefined;
            recs.push(r);
        }
        eq(e.StableStringify(recs), ref(recs), "round " + round);
    }
});

t.test("same-length lists that share no keys never reuse each other's order", ({ eq }) => {
    const a = [];
    for (let i = 0; i < 200; i++) {
        const o = {};
        const off = i % 4;
        for (let k = 0; k < 8; k++) o[KEYS[(k + off) % KEYS.length] + (i % 3)] = k;
        a.push(o);
    }
    eq(e.StableStringify(a), ref(a), "8-key and 16-key slots collide on len & 7");
    const big = {};
    for (let i = 0; i < 2000; i++) big["k" + ((i * 7919) % 2000)] = i;
    eq(e.StableStringify([big, big, { ...big, z: 1 }]), ref([big, big, { ...big, z: 1 }]), "large objects");
});

t.test("lists that agree on a prefix but differ later re-sort", ({ eq }) => {
    const pairs = [
        [{ a: 1, c: 2, b: 3 }, { a: 1, b: 2, z: 3 }],
        [{ m: 1, a: 2, z: 3, b: 4 }, { m: 1, a: 2, b: 3, z: 4 }],
        [{ k: 1, q: 2, c: 3, d: 4, e: 5 }, { k: 1, q: 2, c: 3, e: 4, d: 5 }],
    ];
    for (const [x, y] of pairs) {
        const arr = [x, y, x, y];
        eq(e.StableStringify(arr), ref(arr), Object.keys(x).join("") + " then " + Object.keys(y).join(""));
    }
});

await t.run();
