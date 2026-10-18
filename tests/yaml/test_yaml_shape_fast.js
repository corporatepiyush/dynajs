// YAML block mappings are built through JS_NewObjectFromProps with a shape
// cache per depth (up to 16 keys per mapping, then key-by-key). The oracle is
// independent of the parser: values are generated as JS objects, written with
// Stringify and read back, and must come back identical -- own-key ORDER,
// property attributes and prototype included, because a wrong shape reuse
// shows up there before it shows up in a value.
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
{
    const base = ["id", "name", "email", "active"];
    const rows = [];
    for (let r = 0; r < 5; r++) rows.push(mapOf(base, (k) => k + r));
    rows.push(mapOf(["name", "id", "email", "active"], (k) => k));
    rows.push(mapOf(["id", "name", "email"], (k) => k));
    rows.push(mapOf(["id", "name", "email", "active", "extra"], (k) => k));
    rows.push(mapOf(base, (k) => k + "again"));
    yamlRound(rows, "row shapes: repeat, reorder, subset, superset, repeat");
}

// 3. awkward keys: integer-like (enumeration order is not insertion order),
//    __proto__ as an own key, non-ASCII, keys that look like scalars
for (const pick of [[0, 9, 10, 11], [9, 10, 11, 12, 13, 14], [7, 8], [15, 16, 17, 18, 19], KEYNAMES.map((_, i) => i)]) {
    const keys = pick.map((i) => KEYNAMES[i]);
    const one = mapOf(keys, (k) => "v" + k);
    const back = yamlRound([one, one, one], "keys " + keys.join("|"));
    if (back && keys.includes("__proto__"))
        ok(Object.getPrototypeOf(back[1]) === Object.prototype && Object.prototype.hasOwnProperty.call(back[1], "__proto__"),
            "__proto__ is an own key, the prototype is untouched");
}

// 4. depth: past the per-depth caches (8), with the same shape at every level
{
    let v = { leaf: 1, other: 2 };
    for (let d = 0; d < 20; d++) v = { left: v, right: { leaf: d, other: d + 1 }, tag: "d" + d };
    yamlRound(v, "nested 20 deep");
    const list = []; for (let i = 0; i < 4; i++) list.push(v);
    yamlRound(list, "nested 20 deep, repeated");
}

// 5. generated documents
{
    const rnd = lcg(12345);
    const gen = (depth) => {
        const r = rnd();
        if (depth > 4 || r < 0.35) return rnd() < 0.5 ? Math.floor(rnd() * 1000) : "s" + Math.floor(rnd() * 50);
        if (r < 0.55) { const a = []; const len = Math.floor(rnd() * 5) + 1; for (let i = 0; i < len; i++) a.push(gen(depth + 1)); return a; }
        const len = Math.floor(rnd() * 40);
        const keys = [];
        for (let i = 0; i < len; i++) {
            const k = rnd() < 0.2 ? KEYNAMES[Math.floor(rnd() * KEYNAMES.length)] : "f" + Math.floor(rnd() * 24);
            if (!keys.includes(k)) keys.push(k);
        }
        return mapOf(keys, () => gen(depth + 1));
    };
    for (let i = 0; i < 300; i++) yamlRound(gen(0), "generated #" + i);
}

// 6. duplicate keys are refused wherever they fall: inside the batch, at its
//    edge, and after the batch spilled into a key-by-key object
function yamlDup(keys, label) {
    const text = keys.map((k, i) => k + ": " + i).join("\n") + "\n";
    let msg = "";
    try { Y.Parse(text); } catch (e) { msg = String(e.message); }
    ok(/duplicate mapping key/.test(msg), "yaml duplicate " + label, msg || "no error");
    msg = "";
    try { Y.Parse("top:\n" + text.split("\n").filter(Boolean).map((l) => "  " + l).join("\n") + "\n"); } catch (e) { msg = String(e.message); }
    ok(/duplicate mapping key/.test(msg), "yaml nested duplicate " + label, msg || "no error");
}
{
    const k = (c) => keysN(c, "k");
    yamlDup(["a", "a"], "second key");
    yamlDup(k(15).concat(["k0"]), "16th key repeats the first (last slot of the batch)");
    yamlDup(k(16).concat(["k3"]), "17th key repeats one in the batch (the spill)");
    yamlDup(k(16).concat(["k15"]), "17th key repeats the 16th");
    yamlDup(k(20).concat(["k18"]), "21st key repeats one added after the spill");
    yamlDup(k(20).concat(["k1"]), "21st key repeats one from the batch");
    yamlDup(["1", "x", "1"], "integer-like key");
    yamlDup(["__proto__", "__proto__"], "__proto__ twice");
    yamlDup(["'q'", "q"], "quoted and plain spellings of one key");
}

// 6b. flow mappings take the same path: {k: v, ...}
function flowText(v) {
    if (v === null) return "null";
    if (typeof v !== "object") return String(v);
    if (Array.isArray(v)) return "[" + v.map(flowText).join(", ") + "]";
    return "{" + Object.keys(v).map((k) => k + ": " + flowText(v[k])).join(", ") + "}";
}
function flowRound(v, label) {
    let back, err = null;
    try { back = Y.Parse(flowText(v) + "\n"); } catch (e) { err = e; }
    const r = err ? "threw " + err.message : same(v, back, "$");
    ok(r === null, "yaml flow " + label, r);
}
for (const count of [0, 1, 2, 15, 16, 17, 18, 33]) {
    const keys = keysN(count, "f");
    flowRound(mapOf(keys, (k) => k.length), "flat {} of " + count);
    const rows = []; for (let r = 0; r < 5; r++) rows.push(mapOf(keys, () => r));
    flowRound(rows, "5 rows of " + count);
}
{
    const rows = [];
    for (let r = 0; r < 8; r++) rows.push(r % 3 ? mapOf(["id", "name"], () => r) : mapOf(["name", "id", "x"], () => r));
    flowRound(rows, "alternating flow shapes");
    let v = { leaf: 1 };
    for (let d = 0; d < 12; d++) v = { inner: v, n: d, list: [d, { k: d }] };
    flowRound(v, "nested flow 12 deep");
    const got = Y.Parse("[a: 1, b: 2, {c: 3}, d: {e: 4}]\n");
    ok(same([{ a: 1 }, { b: 2 }, { c: 3 }, { d: { e: 4 } }], got, "$") === null, "single-pair entries in a flow sequence", JSON.stringify(got));
    const bare = Y.Parse("{a:, b: 2, c:}\n");
    ok(same({ a: null, b: 2, c: null }, bare, "$") === null, "flow keys without values are null", JSON.stringify(bare));
}
function flowDup(keys, label) {
    let msg = "";
    try { Y.Parse("{" + keys.map((k, i) => k + ": " + i).join(", ") + "}\n"); } catch (e) { msg = String(e.message); }
    ok(/duplicate mapping key/.test(msg), "yaml flow duplicate " + label, msg || "no error");
}
{
    const k = (c) => keysN(c, "k");
    flowDup(["a", "a"], "second key");
    flowDup(k(16).concat(["k0"]), "17th key repeats one in the batch (the spill)");
    flowDup(k(20).concat(["k18"]), "21st key repeats one added after the spill");
    let msg = "";
    try { Y.Parse("{a: 1, b: [1, 2\n"); } catch (e) { msg = String(e.message); }
    ok(/unterminated|flow/.test(msg), "an unterminated flow mapping with a pending batch is refused", msg);
    msg = "";
    try { Y.Parse("{a: 1, # c\n}\n"); } catch (e) { msg = String(e.message); }
    ok(msg !== "", "a comment inside a flow mapping with a pending batch is refused", msg);
}

// 7. the stream form keeps its caches across documents
{
    const docs = []; for (let i = 0; i < 6; i++) docs.push(mapOf(i % 2 ? ["p", "q"] : ["id", "name"], (kk) => kk + i));
    const text = docs.map((d) => "---\n" + Y.Stringify(d)).join("");
    const all = Y.ParseAll(text);
    ok(same(docs, all, "$") === null, "ParseAll over alternating shapes", same(docs, all, "$"));
    const got = [];
    for (const d of Y.ParseStream(text)) got.push(d);
    ok(same(docs, got, "$") === null, "ParseStream over alternating shapes", same(docs, got, "$"));
}

// 8. results are independent objects: mutating one row leaves the next alone
{
    const rows = Y.Parse("- a: 1\n  b: 2\n- a: 3\n  b: 4\n");
    rows[0].c = 9; delete rows[0].a;
    ok(Reflect.ownKeys(rows[1]).join() === "a,b" && rows[1].a === 3, "rows do not share storage");
}

print("test_yaml_shape_fast: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
