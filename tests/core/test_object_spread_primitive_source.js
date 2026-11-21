// covers: c:src/object/property_set_convert.inc.c
// F29: {..."hey"} gave {} because JS_CopyDataProperties returned at once for any non-object source. ECMA-262
// CopyDataProperties (used by object spread, PropertyDefinitionEvaluation) skips only undefined and null and
// otherwise copies the own enumerable keys of ToObject(source): a string spreads its indices, while a number,
// boolean, bigint or symbol wraps to an object with no own enumerable keys and adds nothing. Rest destructuring
// already did ToObject first. Oracle: that spec text written in JS (Reflect.ownKeys of Object(v), enumerable only,
// defined in order), compared key by key with descriptors.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_object_spread_primitive_source");

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

function specSpread(v) {
    const out = {};
    if (v === undefined || v === null) return out;
    const o = Object(v);
    for (const k of Reflect.ownKeys(o)) {
        const d = Object.getOwnPropertyDescriptor(o, k);
        if (d && d.enumerable) Object.defineProperty(out, k, { value: o[k], writable: true, enumerable: true, configurable: true });
    }
    return out;
}
function same(got, want, what) {
    const gk = Reflect.ownKeys(got), wk = Reflect.ownKeys(want);
    eq(JSON.stringify(gk.map(String)), JSON.stringify(wk.map(String)), what + ": keys in order");
    for (const k of wk) {
        const g = Object.getOwnPropertyDescriptor(got, k), w = Object.getOwnPropertyDescriptor(want, k);
        ok(g && g.value === w.value && g.writable && g.enumerable && g.configurable, what + ": key " + String(k));
    }
}

let rope = "";
for (let i = 0; i < 40; i++) rope += String.fromCharCode(97 + (i % 26));

__kitS.section("a string spreads its indices (the ISSUES repro first)", () => {
    same({ ..."hey" }, { 0: "h", 1: "e", 2: "y" }, "ISSUES repro");
    for (const s of ["", "a", "hey", "a\u{1F600}b", "\0x", rope, rope + rope])
        same({ ...s }, specSpread(s), "spread " + JSON.stringify(s.slice(0, 12)));
});

__kitS.section("other primitives add nothing; undefined and null are skipped", () => {
    for (const v of [0, 5, -0, NaN, 1.5, true, false, 10n, Symbol("s"), undefined, null])
        same({ ...v }, specSpread(v), "spread " + String(v));
    same({ ...new String("ab") }, { 0: "a", 1: "b" }, "a String object (control)");
});

__kitS.section("order, overrides and mixing with other properties", () => {
    same({ x: 1, ..."ab", y: 2 }, { x: 1, 0: "a", 1: "b", y: 2 }, "around other keys");
    same({ ..."abc", 1: "Z" }, { 0: "a", 1: "Z", 2: "c" }, "a later key overrides an index");
    same({ 0: "q", ..."ab" }, { 0: "a", 1: "b" }, "the spread overrides an earlier index");
    same({ ..."ab", ..."xyz" }, { 0: "x", 1: "y", 2: "z" }, "two strings");
    let calls = 0;
    const proto = { set 0(v) { calls++; } };
    const o = Object.assign(Object.create(proto), { ..."ab" });
    eq(calls, 1, "spread defines (no setter) while Object.assign afterwards sets (control)");
    eq(Object.getPrototypeOf({ ..."ab" }), Object.prototype, "result is a plain object");
});

__kitS.section("rest destructuring from a string (already ToObject, control)", () => {
    const { ...r } = "ab";
    same(r, { 0: "a", 1: "b" }, "const {...r} = \"ab\"");
    const { 0: first, ...rest } = "xyz";
    eq(first, "x", "picked index");
    same(rest, { 1: "y", 2: "z" }, "rest excludes the picked key");
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_object_spread_primitive_source: " + n + " checks passed");
__kitS.end();
