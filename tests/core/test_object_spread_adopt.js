// covers: c:src/object/property_set_convert.inc.c
// Object spread shape adoption (js_copy_data_properties_adopt). When {...src} spreads into a fresh,
// empty, extensible plain object with the same prototype, and src is a plain object on a hashed shape
// whose every property is an enumerable/writable/configurable data property with a non-index string
// key, the target adopts src's shape and copies the values; no user code can run, so this is
// unobservable. Integer-like keys, symbols, private names, accessors, non-enumerable or read-only
// properties, deleted slots, a different prototype, Object.assign and rest patterns take the generic
// path. Why: per-key enumerate + define made {...o, x} 2x the cost of an explicit literal
// (ENGINE-PERF.md item 3, r09). The oracle is a key-by-key copy through Reflect.ownKeys and
// Object.defineProperty.
import { suite } from "../lib/kit.js";
import { cpuNow } from "../lib/cpuclock.js";
import { perfCheck } from "../lib/perf.js";
const t = suite("test_object_spread_adopt");

function dump(o) {
    const parts = [];
    for (const k of Reflect.ownKeys(o)) {
        const d = Object.getOwnPropertyDescriptor(o, k);
        const kn = typeof k === "symbol" ? "@@" + k.description : k;
        if ("value" in d) parts.push(kn + ":" + (d.value && typeof d.value === "object" ? "obj" : String(d.value)) + (d.writable ? "w" : "-") + (d.enumerable ? "e" : "-") + (d.configurable ? "c" : "-"));
        else parts.push(kn + ":<acc>");
    }
    const p = Object.getPrototypeOf(o);
    return parts.join(" ") + " proto=" + (p === null ? "null" : p === Object.prototype ? "Object" : "other");
}

function oracle(src) {
    const out = {};
    if (src === null || src === undefined) return out;
    const o = Object(src);
    for (const k of Reflect.ownKeys(o)) {
        const d = Object.getOwnPropertyDescriptor(o, k);
        if (d && d.enumerable) Object.defineProperty(out, k, { value: o[k], writable: true, enumerable: true, configurable: true });
    }
    return out;
}

t.test("spread matches a key-by-key copy for many source kinds", (a) => {
    const sym = Symbol("s");
    class C { #priv = 1; constructor() { this.x = 1; this.y = 2; } get priv() { return this.#priv; } }
    const frozen = Object.freeze({ a: 1, b: 2 });
    const withDeleted = { a: 1, b: 2, c: 3 }; delete withDeleted.b;
    const nonEnum = { a: 1 }; Object.defineProperty(nonEnum, "hidden", { value: 2, enumerable: false, writable: true, configurable: true });
    const getter = { a: 1, get g() { return "got"; } };
    const nullProto = Object.assign(Object.create(null), { a: 1, b: 2 });
    const sources = [
        ["plain", { k: 1, t: "x", off: 1, end: 2, line: 1, col: 1, sl: 0, ws: false, p: "" }],
        ["int keys", { b: 1, 2: "two", a: 3, 1: "one" }],
        ["big index", { a: 1, "4294967294": 2, "4294967295": 3, b: 4 }],
        ["symbol", { a: 1, [sym]: 2, b: 3 }],
        ["class instance", new C()],
        ["frozen", frozen],
        ["deleted slot", withDeleted],
        ["non-enumerable", nonEnum],
        ["getter", getter],
        ["null proto", nullProto],
        ["array", [1, 2, 3]],
        ["empty", {}],
        ["nested values", { o: { z: 1 }, arr: [1], f() {} }],
    ];
    for (const [name, src] of sources) {
        for (let i = 0; i < 5; i++) {
            const got = { ...src };
            a.eq(dump(got), dump(oracle(src)), name + " #" + i);
            a.eq(Object.getPrototypeOf(got), Object.prototype, name + " keeps Object.prototype");
        }
    }
});

t.test("copies and sources stay independent after sharing a shape", (a) => {
    const base = { a: 1, b: { n: 1 }, c: "s" };
    const copies = [];
    for (let i = 0; i < 20; i++) copies.push({ ...base });
    copies[0].a = 99;
    copies[1].d = 4;
    delete copies[2].a;
    Object.defineProperty(copies[3], "c", { enumerable: false });
    Object.freeze(copies[4]);
    copies[5].b.n = 7;
    a.eq(dump(base), "a:1wec b:objwec c:swec proto=Object", "source shape and values untouched");
    a.eq(base.b.n, 7, "values are shared references, as in the spec");
    a.eq(dump(copies[0]), "a:99wec b:objwec c:swec proto=Object", "write to a copy");
    a.eq(dump(copies[1]), "a:1wec b:objwec c:swec d:4wec proto=Object", "add to a copy");
    a.eq(dump(copies[2]), "b:objwec c:swec proto=Object", "delete from a copy");
    a.eq(dump(copies[3]), "a:1wec b:objwec c:sw-c proto=Object", "redefine on a copy");
    a.eq(Object.isFrozen(copies[4]) && !Object.isFrozen(base) && !Object.isFrozen(copies[6]), true, "freeze one copy");
    a.eq(dump(copies[6]), "a:1wec b:objwec c:swec proto=Object", "other copies unaffected");
    base.e = 5;
    a.eq(dump(copies[7]), "a:1wec b:objwec c:swec proto=Object", "adding to the source does not reach copies");
    a.eq(dump({ ...base }), "a:1wec b:objwec c:swec e:5wec proto=Object", "a new copy sees the new key");
});

t.test("spread then explicit fields, and generic paths still apply", (a) => {
    const base = { k: 1, t: "x", off: 1 };
    let sum = 0;
    for (let i = 0; i < 1000; i++) { const o = { ...base, off: i, extra: i }; sum += o.off + o.extra + o.k; }
    a.eq(sum, 2 * 499500 + 1000, "override and extra field");
    a.eq(dump({ x: 0, ...base }), "x:0wec k:1wec t:xwec off:1wec proto=Object", "a non-empty target takes the generic path");
    a.eq(dump({ __proto__: null, ...base }), "k:1wec t:xwec off:1wec proto=null", "a different prototype takes the generic path");
    const tgt = {}; Object.assign(tgt, base);
    a.eq(dump(tgt), "k:1wec t:xwec off:1wec proto=Object", "Object.assign");
    const { k, ...rest } = base;
    a.eq(dump(rest) + k, "t:xwec off:1wec proto=Object1", "rest pattern excludes keys");
    const P = { inherited: 1 };
    const child = Object.create(P); child.own = 2;
    a.eq(dump({ ...child }), "own:2wec proto=Object", "inherited keys are not copied");
    let hits = 0;
    Object.defineProperty(Object.prototype, "k", { set() { hits++; }, configurable: true });
    try { a.eq(dump({ ...base }), "k:1wec t:xwec off:1wec proto=Object", "spread defines, never sets"); a.eq(hits, 0, "no setter call"); }
    finally { delete Object.prototype.k; }
});

t.test("spread costs about what an explicit literal costs", (a) => {
    const b = { k: 1, t: "x", off: 1, end: 2, line: 1, col: 1, sl: 0, ws: false, p: "" };
    const best = (fn) => { let m = 1e9; for (let r = 0; r < 5; r++) { const t0 = cpuNow(); fn(); m = Math.min(m, cpuNow() - t0); } return m; };
    let s = 0;
    const spread = best(() => { for (let i = 0; i < 100000; i++) s += ({ ...b, off: i }).off; });
    const lit = best(() => { for (let i = 0; i < 100000; i++) s += ({ k: b.k, t: b.t, off: i, end: b.end, line: b.line, col: b.col, sl: b.sl, ws: b.ws, p: b.p }).off; });
    perfCheck(a.ok.bind(a), spread < lit * 1.6 + 1, "100k {...o, off} " + spread.toFixed(2) + " ms vs explicit 9-field literal " + lit.toFixed(2) + " ms (was about 2x before shape adoption)", a.skip.bind(a));
});

await t.run();
