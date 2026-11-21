// covers: dyna:yaml.Stringify
// YAML block mappings are built through JS_NewObjectFromProps with a shape
// cache per depth (up to 16 keys per mapping, then key-by-key). The oracle is
// independent of the parser: values are generated as JS objects, written with
// Stringify and read back, and must come back identical -- own-key ORDER,
// property attributes and prototype included, because a wrong shape reuse
// shows up there before it shows up in a value.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_stringify_shape_fast");
import * as Y from "dyna:yaml";

let n = 0, fails = 0;
function ok(c, m, extra) { n++; if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " -- " + extra : "")); } }

function same(a, b, path) {
    if (a === null || typeof a !== "object") return Object.is(a, b) ? null : path + ": " + JSON.stringify(a) + " vs " + JSON.stringify(b);
    if (b === null || typeof b !== "object") return path + ": object vs " + JSON.stringify(b);
    if (Array.isArray(a) !== Array.isArray(b)) return path + ": array-ness differs";
    if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return path + ": prototype differs";
    const ka = Reflect.ownKeys(a), kb = Reflect.ownKeys(b);
    if (ka.length !== kb.length) return path + ": key count " + ka.length + " vs " + kb.length + " [" + kb.join(",") + "]";
    for (let i = 0; i < ka.length; i++) {
        if (ka[i] !== kb[i]) return path + ": key " + i + " is " + String(kb[i]) + ", want " + String(ka[i]);
        const da = Object.getOwnPropertyDescriptor(a, ka[i]), db = Object.getOwnPropertyDescriptor(b, kb[i]);
        if (!Array.isArray(a) && (!db.writable || !db.enumerable || !db.configurable || db.get || db.set))
            return path + "." + String(ka[i]) + ": not a plain writable/enumerable/configurable data property";
        const r = same(da.value, db.value, path + "." + String(ka[i]));
        if (r) return r;
    }
    if (!Object.isExtensible(b)) return path + ": result is not extensible";
    return null;
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>>  0) / 4294967296; }

// ---------------------------------------------------------------- YAML
function yamlRound(v, label) {
    const text = Y.Stringify(v);
    let back, err = null;
    try { back = Y.Parse(text); } catch (e) { err = e; }
    const r = err ? "threw " + err.message : same(v, back, "$");
    ok(r === null, "yaml " + label, r);
    return back;
}

const KEYNAMES = ["id", "name", "email", "a", "b", "c", "x1", "__proto__", "constructor", "0", "1", "10", "2", "-1", "00", "é", "日本", "k k", "true", "null"];
function mapOf(keys, mk) { const o = {}; for (const k of keys) Object.defineProperty(o, k, { value: mk(k), writable: true, enumerable: true, configurable: true }); return o; }
function keysN(count, prefix) { const a = []; for (let i = 0; i < count; i++) a.push(prefix + i); return a; }

// 1. sizes across the batch threshold, alone and as rows of a list
for (const count of [0, 1, 2, 15, 16, 17, 18, 31, 32, 33, 40]) {
    const keys = keysN(count, "k");
    yamlRound(mapOf(keys, (k) => k.length), "flat map of " + count);
    const rows = []; for (let r = 0; r < 6; r++) rows.push(mapOf(keys, (k) => r + ":" + k));
    yamlRound(rows, "6 rows of " + count + " keys");
}

// 2. the same shape repeated, then a different order, a subset, a superset

__kitS.section("ParseAll over alternating shapes", () => {
    const docs = []; for (let i = 0; i < 6; i++) docs.push(mapOf(i % 2 ? ["p", "q"] : ["id", "name"], (kk) => kk + i));
    const text = docs.map((d) => "---\n" + Y.Stringify(d)).join("");
    const all = Y.ParseAll(text);
    ok(same(docs, all, "$") === null, "ParseAll over alternating shapes", same(docs, all, "$"));
    const got = [];
    for (const d of Y.ParseStream(text)) got.push(d);
    ok(same(docs, got, "$") === null, "ParseStream over alternating shapes", same(docs, got, "$"));
});
__kitS.end();

print("test_yaml_shape_fast: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
