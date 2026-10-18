// JSON.parse builds an object whose keys repeat the previous object's (at the
// same nesting depth) straight from that object's shape, comparing raw key
// bytes against the expected key and skipping the key string altogether. The
// reference is JSON.parse with a reviver: that path keeps the parse records and
// never takes the shape route, so it builds every object property by property.
// Every value, key ORDER, own-property set, prototype and error message must agree.
let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; if (fail < 20) print("  FAIL: " + m); } };
const ref = (s) => JSON.parse(s, (k, v) => v);

function same(a, b, path) {
    if (a === b) return Object.is(a, b) ? null : path + ": " + a + " vs " + b;
    if (typeof a !== "object" || typeof b !== "object" || a === null || b === null)
        return Number.isNaN(a) && Number.isNaN(b) ? null : path + ": " + JSON.stringify(a) + " vs " + JSON.stringify(b);
    if (Array.isArray(a) !== Array.isArray(b)) return path + ": array-ness differs";
    if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return path + ": prototype differs";
    const ka = Reflect.ownKeys(a), kb = Reflect.ownKeys(b);
    if (ka.length !== kb.length || ka.some((k, i) => k !== kb[i])) return path + ": keys " + ka.join(",") + " vs " + kb.join(",");
    for (const k of ka) {
        const da = Object.getOwnPropertyDescriptor(a, k), db = Object.getOwnPropertyDescriptor(b, k);
        if (da.writable !== db.writable || da.enumerable !== db.enumerable || da.configurable !== db.configurable)
            return path + "." + String(k) + ": attributes differ";
        const r = same(a[k], b[k], path + "." + String(k));
        if (r) return r;
    }
    return null;
}
function check(s, what) {
    let x, y, ex = null, ey = null;
    try { x = JSON.parse(s); } catch (e) { ex = String(e); }
    try { y = ref(s); } catch (e) { ey = String(e); }
    if (ex || ey) { ok(ex === ey, what + ": errors differ: " + ex + " | " + ey); return; }
    const d = same(x, y, "$");
    ok(d === null, what + ": " + d);
}

// hand-picked shapes, each repeated so the second copy takes the shape route
const cases = [
    '{"a":1,"b":2}', '{"b":2,"a":1}', '{"a":1}', '{"a":1,"b":2,"c":3}', '{}', '{ }', '{"a":{}}',
    '{"a":1,"a":2}', '{"a":1,"b":2,"a":3}', '{"__proto__":1,"x":2}', '{"__proto__":{"y":1}}',
    '{"1":1,"0":0,"a":2}', '{"10":1,"9":2}', '{"\\u0061":1,"b":2}', '{"a\\u0062":1}', '{"ab":1}',
    '{"a\\"b":1}', '{"a\\\\b":1}', '{"a\\b":1}', '{"\\u00e9":1}', '{"é":1}', '{"Ã©":1}', '{"\\u0000":1}',
    '{"a"\n:\t1 ,\r"b" : 2}', '{"a":1,"b":[1,{"a":1,"b":2}]}', '{"length":1}', '{"constructor":1,"toString":2}',
    '{"":1,"a":2}', '{"a":1,"b":2,"c":{"a":1,"b":2,"c":{"a":1}}}',
];
for (const c of cases) {
    check(c, c);
    check("[" + c + "," + c + "," + c + "]", "x3 " + c);
}
// every pair of hand-picked shapes in sequence: a template from one, a candidate from the other
for (const a of cases) for (const b of cases) check("[" + a + "," + b + "," + a + "]", a + " then " + b);

// wide objects (past the 32-slot buffer) and deep ones (past the 8 depth slots)
const wide = (n, off) => "{" + Array.from({ length: n }, (_, i) => '"k' + ((i + off) % n) + '":' + i).join(",") + "}";
for (const n of [31, 32, 33, 64]) { check("[" + wide(n, 0) + "," + wide(n, 0) + "," + wide(n, 1) + "]", "wide " + n); }
let deep = "1";
for (let d = 0; d < 20; d++) deep = '{"d' + (d % 3) + '":' + deep + ',"e":' + d + '}';
check("[" + deep + "," + deep + "]", "deep 20");

// generated: records drawn from a few shapes, so templates hit and miss at every depth
let seed = 9;
const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
const keys = ["id", "name", "a", "b", "__proto__", "0", "7", "x\\ny", "\\u00e9", "é", "q\\\\", "z"];
function gen(depth) {
    const t = rnd(depth > 3 ? 5 : 8);
    if (t === 0) return String(rnd(1000) - 500);
    if (t === 1) return JSON.stringify("s" + rnd(50));
    if (t === 2) return ["true", "false", "null", "1.5e3"][rnd(4)];
    if (t === 3) return "[" + Array.from({ length: rnd(4) }, () => gen(depth + 1)).join(",") + "]";
    const shape = rnd(4), n = 1 + rnd(5), ks = [];
    for (let i = 0; i < n; i++) ks.push(keys[(shape * 3 + i * (shape + 1) + (rnd(6) === 0 ? rnd(keys.length) : 0)) % keys.length]);
    const sp = [" ", "", "\n", "\t", "\r\n"][rnd(5)];
    return "{" + ks.map((k) => sp + '"' + k + '"' + sp + ":" + sp + gen(depth + 1)).join(",") + sp + "}";
}
for (let i = 0; i < 300; i++) check("[" + Array.from({ length: 1 + rnd(12) }, () => gen(0)).join(",") + "]", "generated #" + i);

// malformed input: the same error, whichever path saw it first
const bad = ['[{"a":1},{"a" 1}]', '[{"a":1},{"a":}]', '[{"a":1},{"a":1,}]', '[{"a":1},{"a":1', '[{"a":1},{"b":1 "c":2}]',
    '[{"a":1},{a:1}]', '[{"a":1},{"a":1}}', '[{"a":1},{,"a":1}]', '[{"a":1},{"a":1,"a"}]', '[{"a":1},{"a":tru}]',
    '[{"a":1},{"a\u0001":1}]', '[{"ab":1},{"abX:1}]', '[{"ab":1},{"ab\\":1}]', '[{"ab":1},{"ab', '[{"a":1},{"a":1}' + "\u0000" + ']'];
for (const b of bad) check(b, "malformed " + b);

{
    // JSON.stringify quotes each key once per call through a 64-slot cache
    // indexed by atom; with hundreds of distinct keys in one call the slots
    // collide constantly and every key must still come out as itself. The
    // expected text quotes keys as string VALUES, which never use the cache.
    const keys = [];
    for (let i = 0; i < 300; i++) keys.push(i % 7 === 0 ? "k\"" + i : i % 11 === 0 ? "ü" + i : "key_" + i);
    const rows = [];
    for (let r = 0; r < 40; r++) {
        const o = {};
        for (let j = 0; j < 12; j++) o[keys[(r * 37 + j * 53) % keys.length]] = r * 100 + j;
        rows.push(o);
    }
    const want = "[" + rows.map((o) => "{" + Object.keys(o).map((k) => JSON.stringify(String(k)) + ":" + o[k]).join(",") + "}").join(",") + "]";
    const got = JSON.stringify(rows);
    if (got === want) pass++;
    else { fail++; print("FAIL JSON.stringify: 300 distinct keys in one call each come out as themselves: " + got.slice(0, 160)); }
}

print("test_json_shape_fast: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_json_shape_fast: " + fail + " failures");
