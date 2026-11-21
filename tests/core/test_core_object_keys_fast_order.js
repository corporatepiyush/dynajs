// covers: c:src/object/property_get.inc.c
// JS_GetOwnPropertyNamesInternal has a one-pass fast path for plain objects
// whose own keys are all non-index strings. Two facts are pinned here:
// 1. (bug, fixed in the modperf round 2026-11-15) an array index above 2^31-1
//    ("4000000000") is not a tagged-int atom, so the old fast path emitted it
//    in insertion order: Object.keys({b,"4000000000",a}) gave ["b","4000000000","a"]
//    while spread and for-in gave the spec order ["4000000000","b","a"]. The
//    fast path now leaves any key starting with a digit to the full path.
// 2. the fast path also serves object spread / Object.assign (STRING | SYMBOL |
//    ENUM_ONLY) when the object has no symbol keys: spread of a 5-key object
//    100K times 24.6 ms -> 21.9 ms. Symbol, non-enumerable and index keys must
//    still come out exactly as the spec orders them.
// The oracle orders keys by the spec rule written in JS: canonical array
// indices (< 2^32-1) ascending, then the other strings in insertion order.
import { suite } from "../lib/kit.js";

const t = suite("test_core_object_keys_fast_order");

function isIndex(k) {
    if (!/^(0|[1-9][0-9]*)$/.test(k)) return false;
    const v = Number(k);
    return v < 4294967295 && String(v) === k;
}
function specKeys(insertion) {
    const idx = insertion.filter(isIndex).sort((a, b) => Number(a) - Number(b));
    return idx.concat(insertion.filter((k) => !isIndex(k)));
}
function build(keys) {
    const o = {};
    keys.forEach((k, i) => (o[k] = i));
    return o;
}

const SETS = [
    ["b", "4000000000", "a"],
    ["z", "4294967294", "y", "4294967295", "x"],
    ["0x", "01", "1a", "9", "-1", "1.5", "1e3", "a"],
    ["name", "id", "score", "ok", "tag"],
    ["k" + 1, "2147483648", "2147483647", "3000000000", "m", "0"],
    ["١", "１", "q", "123456789012"],
];

t.test("Object.keys follows the spec order for index-like strings", ({ eq }) => {
    for (const s of SETS) eq(JSON.stringify(Object.keys(build(s))), JSON.stringify(specKeys(s)), "keys of " + JSON.stringify(s));
});

t.test("spread, Object.assign, for-in and JSON.stringify agree with Object.keys", ({ eq }) => {
    for (const s of SETS) {
        const o = build(s), want = JSON.stringify(specKeys(s));
        eq(JSON.stringify(Object.keys({ ...o })), want, "spread " + JSON.stringify(s));
        eq(JSON.stringify(Object.keys(Object.assign({}, o))), want, "assign " + JSON.stringify(s));
        const fi = [];
        for (const k in o) fi.push(k);
        eq(JSON.stringify(fi), want, "for-in " + JSON.stringify(s));
        eq(JSON.stringify(Object.keys(JSON.parse(JSON.stringify(o)))), want, "JSON round trip " + JSON.stringify(s));
    }
});

t.test("spread copies symbols and skips non-enumerables", ({ eq }) => {
    const s1 = Symbol("s1"), s2 = Symbol.for("s2");
    const o = { b: 1, [s1]: 2, a: 3, [s2]: 4 };
    Object.defineProperty(o, "hidden", { value: 5, enumerable: false });
    Object.defineProperty(o, s2, { enumerable: false });
    const c = { ...o };
    eq(JSON.stringify(Object.keys(c)), '["b","a"]', "string keys");
    eq(Object.getOwnPropertySymbols(c).length, 1, "only the enumerable symbol");
    eq(c[s1], 2, "symbol value copied");
    eq("hidden" in c, false, "non-enumerable not copied");
    eq(Reflect.ownKeys(o).length, 5, "source keys unchanged");
    const plain = { x: 1, y: 2 };
    const d = Object.assign({}, plain);
    eq(JSON.stringify(d), '{"x":1,"y":2}', "plain assign");
});

t.test("deleted keys leave no hole in the result", ({ eq }) => {
    const o = { a: 1, b: 2, c: 3, d: 4 };
    delete o.b;
    o.e = 5;
    eq(JSON.stringify(Object.keys(o)), '["a","c","d","e"]', "keys after delete");
    eq(JSON.stringify(Object.keys({ ...o })), '["a","c","d","e"]', "spread after delete");
});

await t.run();
