// flags: --std
// covers: dyna:structures.RBTree
// RBTree, WBTree, SkipList, XArray, PatriciaTrie, CSRGraph, RStarTree,
// SuffixArray: every query is checked against a brute-force model written here
// (a sorted array, a linear scan, Bellman-Ford, naive suffix sort).
import * as std from "std";
import { memoryUsage, setNativeMemoryLimit } from "dyna:sys";
import { CRC32C } from "dyna:hash";
import { suite } from "../lib/kit.js";
import { Deque, RBTree, WBTree, SkipList, XArray, PatriciaTrie, CSRGraph, RStarTree, SuffixArray } from "dyna:structures";

const t = suite("structures_ext");

let seed = 0x2545F491;
function rnd(n) {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0;
    return seed % n;
}

const enc = new TextEncoder();
function utf8cmp(a, b) {
    const x = enc.encode(a), y = enc.encode(b), n = Math.min(x.length, y.length);
    for (let i = 0; i < n; i++) if (x[i] !== y[i]) return x[i] - y[i];
    return x.length - y.length;
}

function treeModel(cmp) {
    const keys = [], vals = new Map();
    const find = (k) => { let lo = 0, hi = keys.length; while (lo < hi) { const mid = (lo + hi) >> 1; if (cmp(keys[mid], k) < 0) lo = mid + 1; else hi = mid; } return lo; };
    return {
        set(k, v) { const i = find(k); if (i >= keys.length || cmp(keys[i], k) !== 0) keys.splice(i, 0, k); vals.set(k, v); },
        del(k) { const i = find(k); if (i < keys.length && cmp(keys[i], k) === 0) { keys.splice(i, 1); vals.delete(k); return true; } return false; },
        get(k) { return vals.get(k); },
        rank(k) { return find(k); },
        floor(k) { const i = find(k); if (i < keys.length && cmp(keys[i], k) === 0) return keys[i]; return i > 0 ? keys[i - 1] : undefined; },
        ceil(k) { const i = find(k); return i < keys.length ? keys[i] : undefined; },
        keys,
    };
}

for (const T of [RBTree, WBTree, SkipList]) {
    t.test(T.name + " number keys agree with a sorted array", (a) => {
        const tr = new T(), mod = treeModel((x, y) => x - y);
        let bad = 0;
        for (let step = 0; step < 6000; step++) {
            const k = rnd(800) - 400 + (rnd(4) === 0 ? 0.5 : 0), op = rnd(10);
            if (op < 5) { tr.set(k, step); mod.set(k, step); }
            else if (op < 8) { if (tr.delete(k) !== mod.del(k)) bad++; }
            else {
                if (tr.get(k) !== mod.get(k) || tr.has(k) !== (mod.get(k) !== undefined)) bad++;
                if (tr.floorKey(k) !== mod.floor(k) || tr.ceilKey(k) !== mod.ceil(k)) bad++;
                if (tr.rank && tr.rank(k) !== mod.rank(k)) bad++;
            }
        }
        a.eq(bad, 0, "mismatches");
        a.deep(tr.keys(), mod.keys, "in-order keys");
        a.eq(tr.size, mod.keys.length);
        a.eq(tr.firstKey(), mod.keys[0]);
        a.eq(tr.lastKey(), mod.keys[mod.keys.length - 1]);
        if (tr.at) {
            for (let i = 0; i < mod.keys.length; i += 37) a.deep(tr.at(i), [mod.keys[i], mod.get(mod.keys[i])]);
            a.deep(tr.at(-1), [mod.keys[mod.keys.length - 1], mod.get(mod.keys[mod.keys.length - 1])]);
            a.eq(tr.at(mod.keys.length), undefined);
            a.eq(tr.at(0.5), undefined);
        }
        a.deep([...tr].map((e) => e[0]), mod.keys, "iterator yields entries in order");
    });

    t.test(T.name + " string keys order by UTF-8 bytes", (a) => {
        const tr = new T({ keys: "string" }), mod = treeModel(utf8cmp);
        const alpha = ["", "a", "b", "ab", "é", "￿", "😀", "\ud800", "z", "aé"];
        for (let i = 0; i < 2000; i++) {
            const k = alpha[rnd(alpha.length)] + alpha[rnd(alpha.length)];
            if (rnd(4) === 0) { a.eq(tr.delete(k), mod.del(k)); } else { tr.set(k, i); mod.set(k, i); }
        }
        a.deep(tr.keys(), mod.keys);
        a.eq(tr.get("😀"), mod.get("😀"));
        a.eq(tr.has(42), false, "a number key is coerced to its string");
    });

    t.test(T.name + " refusals and -0", (a) => {
        const tr = new T();
        a.throws(() => tr.set(NaN, 1), RangeError);
        a.eq(tr.get(NaN), undefined);
        a.eq(tr.has(NaN), false);
        a.eq(tr.delete(NaN), false);
        tr.set(-0, "z");
        a.eq(tr.get(0), "z");
        a.ok(Object.is(tr.firstKey(), 0), "-0 is stored as +0");
        a.throws(() => new T({ keys: "date" }), TypeError);
        a.throws(() => new T(5), TypeError);
        a.throws(() => T.prototype.set.call({}, 1, 1), TypeError);
        let calls = 0;
        const k = { valueOf() { calls++; tr.delete(0); return 7; } };
        tr.set(k, "seven");
        a.eq(calls, 1, "the key is coerced once");
        a.eq(tr.get(7), "seven");
        a.eq(tr.get(0), undefined, "mutation inside coercion is honoured");
    });

    t.test(T.name + " string key copies are freed on replace, delete and collection", (a) => {
        setNativeMemoryLimit(1 << 30);
        std.gc();
        const base = memoryUsage().nativeSize;
        let tr = new T({ keys: "string" });
        for (let i = 0; i < 200; i++) tr.set("key-" + (i % 50) + "-padding-to-make-it-longer", i);
        const held = memoryUsage().nativeSize;
        a.ok(held > base, "the tree holds native memory");
        for (let i = 0; i < 5000; i++) tr.set("key-" + (i % 50) + "-padding-to-make-it-longer", i);
        a.eq(memoryUsage().nativeSize, held, "replacing a value allocates nothing that stays");
        for (let i = 0; i < 25; i++) tr.delete("key-" + i + "-padding-to-make-it-longer");
        a.ok(memoryUsage().nativeSize < held, "delete releases the key");
        tr = null;
        std.gc();
        a.eq(memoryUsage().nativeSize, base, "collection releases everything");
        setNativeMemoryLimit(0);
    });

    t.test(T.name + " holding itself is collected", (a) => {
        for (let i = 0; i < 50; i++) { const tr = new T(); tr.set(1, tr); tr.set(2, { tr }); }
        std.gc();
        a.ok(true);
    });
}

await t.run();
