// covers: c:src/builtins/builtin-json.c
// JSON.stringify with a gap no longer builds three strings per nested object
// or array (indent + gap, "\n" + indent, " "): the indent is a depth number,
// a Latin-1 gap of 1-10 chars is pre-expanded into "\n" + gap x 32 and written
// with one copy for depth <= 32 (deeper, or a two-byte gap, appends gap per
// level). The key passed to toJSON / the replacer is only materialised when
// one of them is called (it used to be built for every object element of an
// array). Modperf round 2026-11-15, stringify(records, null, 2) + parse 1K:
// 1.50 ms -> 1.28 ms (-14%); toJSON-free stringify -3.5%. Oracle: a JS
// stringifier written from ECMA-262 SerializeJSONProperty / SerializeJSONObject
// / SerializeJSONArray (indent = stepback + gap), JSON.stringify of each
// string for quoting.
import { suite } from "../lib/kit.js";

const t = suite("test_core_json_stringify_gap_depth");

function ref(holder, key, value, gap, indent, replacer, calls) {
    if (value !== null && (typeof value === "object" || typeof value === "bigint") && typeof value.toJSON === "function") {
        calls.push("toJSON:" + typeof key + ":" + key);
        value = value.toJSON(key);
    }
    if (replacer) {
        calls.push("rep:" + typeof key + ":" + key);
        value = replacer.call(holder, key, value);
    }
    if (value === null) return "null";
    if (value === true || value === false) return String(value);
    if (typeof value === "string") return JSON.stringify(value);
    if (typeof value === "number") return isFinite(value) ? String(value) : "null";
    if (typeof value !== "object" || typeof value === "function") return undefined;
    const step = indent + gap;
    if (Array.isArray(value)) {
        if (!value.length) return "[]";
        const parts = [];
        for (let i = 0; i < value.length; i++) {
            const s = ref(value, String(i), value[i], gap, step, replacer, calls);
            parts.push(s === undefined ? "null" : s);
        }
        return gap ? "[\n" + step + parts.join(",\n" + step) + "\n" + indent + "]" : "[" + parts.join(",") + "]";
    }
    const parts = [];
    for (const k of Object.keys(value)) {
        const s = ref(value, k, value[k], gap, step, replacer, calls);
        if (s !== undefined) parts.push(JSON.stringify(k) + (gap ? ": " : ":") + s);
    }
    if (!parts.length) return "{}";
    return gap ? "{\n" + step + parts.join(",\n" + step) + "\n" + indent + "}" : "{" + parts.join(",") + "}";
}
function gapOf(space) {
    if (typeof space === "number") return " ".repeat(Math.max(0, Math.min(10, Math.trunc(space))));
    if (typeof space === "string") return space.slice(0, 10);
    return "";
}

let seed = 2024;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff), seed);
function gen(d) {
    const r = rnd() % 10;
    if (d > 6 || r < 3) return [null, true, 1.5, -0, "s\n\"x", 42, "é中", NaN][rnd() % 8];
    if (r < 6) return Array.from({ length: rnd() % 4 }, () => gen(d + 1));
    const o = {};
    for (let i = 0; i < rnd() % 4; i++) o[["a", "b c", "10", "é", "x"][rnd() % 5]] = gen(d + 1);
    if (rnd() % 7 === 0) o.u = undefined;
    return o;
}
const SPACES = [undefined, 0, 1, 2, 4, 10, 11, -3, "", "\t", "--", "abcdefghijkl", "é", "中", "中文"];

t.test("random trees at every gap match the spec oracle", ({ eq }) => {
    for (let i = 0; i < 400; i++) {
        const v = gen(0);
        for (const sp of SPACES) eq(JSON.stringify(v, null, sp), ref({ "": v }, "", v, gapOf(sp), "", null, []), "tree " + i + " space " + JSON.stringify(sp));
    }
});

t.test("depth beyond the 32-level indent cache", ({ eq }) => {
    let v = [1];
    for (let i = 0; i < 70; i++) v = i & 1 ? [v, i] : { k: v, i };
    for (const sp of [1, 2, 10, "\t", "中", "ab"]) eq(JSON.stringify(v, null, sp), ref({ "": v }, "", v, gapOf(sp), "", null, []), "deep space " + JSON.stringify(sp));
});

t.test("toJSON and replacer receive string keys, only when they exist", ({ eq }) => {
    class P {
        constructor(i) { this.i = i; }
        toJSON(k) { return { k, kt: typeof k, i: this.i }; }
    }
    const v = { a: [new P(0), { b: new P(1) }, [new P(2)]], c: new P(3), [7]: new P(4) };
    const calls = [], got = [];
    eq(JSON.stringify(v, null, 2), ref({ "": v }, "", v, "  ", "", null, calls), "toJSON keys");
    const rep = function (k, x) {
        got.push("rep:" + typeof k + ":" + k);
        return typeof x === "number" ? x * 10 : x;
    };
    const out = JSON.stringify(v, rep, "\t");
    const seen = got.slice();
    const calls2 = [];
    const quiet = (k, x) => (typeof x === "number" ? x * 10 : x);
    eq(out, ref({ "": v }, "", v, "\t", "", quiet, calls2), "replacer output");
    eq(seen.join("|"), calls2.filter((c) => c.startsWith("rep:")).join("|"), "replacer key order and types");
    const proto = { toJSON(k) { return "P:" + typeof k + ":" + k; } };
    const arr = [Object.create(proto), [2], { y: Object.create(proto) }];
    eq(JSON.stringify(arr, null, 1), '[\n "P:string:0",\n [\n  2\n ],\n {\n  "y": "P:string:y"\n }\n]', "inherited toJSON sees string index keys");
});

await t.run();
