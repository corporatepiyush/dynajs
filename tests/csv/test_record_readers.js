// flags: --std
// csv.stringify(records) and DataFrame.FROM_RECORDS read every record through
// a per-column property cache (JS_GetOwnDataCached / JS_GetPropertyCached:
// same shape as the last row -> read the cached slot), and FROM_RECORDS skips
// re-enumerating a row's keys when it has the previous row's shape
// (JS_SameShape). Both shortcuts are only valid for plain objects whose
// shape fixes their own keys; everything else must take the slow path. The
// oracles are computed here in JS from the records themselves and compared
// with what comes back out: csv.parse of the CSV text, TO_RECORDS of the
// frame.
import * as csv from "dyna:csv";
import * as df from "dyna:dataframe";

let n = 0, fails = 0;
function ok(c, m, extra) { n++; if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " -- " + extra : "")); } }

// --- the hostile row kinds -------------------------------------------------
function kinds(i) {
    const plain = { id: i, name: "n" + (i % 3), score: i / 4 };
    const reordered = { score: i / 4, id: i, name: "r" + i };
    const withGetter = { id: i, score: 1 };
    Object.defineProperty(withGetter, "name", { get() { return "g" + i; }, enumerable: true, configurable: true });
    const deleted = { id: i, tmp: 1, name: "d" + i, score: 2 };
    delete deleted.tmp;
    const readded = { id: i, name: "x", score: 3 };
    delete readded.name;
    readded.name = "a" + i;
    const nullProto = Object.assign(Object.create(null), { id: i, name: "z" + i, score: 4 });
    class Rec { constructor() { this.id = i; this.name = "c" + i; this.score = 5; } }
    const frozen = Object.freeze({ id: i, name: "f" + i, score: 6 });
    const proxied = new Proxy({ id: i, name: "p" + i, score: 7 }, {});
    const intKeys = { id: i, name: "i" + i, score: 8, 1: "one", 0: "zero" };
    const protoKey = JSON.parse('{"id":' + i + ',"name":"q","score":9,"__proto__":"own"}');
    return [plain, reordered, withGetter, deleted, readded, nullProto, new Rec(), frozen, proxied, intKeys, protoKey];
}
const KINDS = ["plain", "reordered", "getter", "deleted", "re-added", "null-proto", "class", "frozen", "proxy", "int-keys", "__proto__-key"];

// own data value as csv.stringify sees it: accessors and missing keys are empty
function ownCell(o, k) {
    const d = Object.getOwnPropertyDescriptor(o, k);
    if (!d || !("value" in d) || d.value === undefined) return "";
    return String(d.value);
}

// --- csv.stringify ---------------------------------------------------------
function csvCheck(rows, label) {
    const header = Object.keys(rows[0]);
    let text, back;
    try { text = csv.stringify(rows); back = csv.parse(text); } catch (e) { ok(false, "csv " + label, e.message); return; }
    ok(back.headers.join("|") === header.join("|"), "csv " + label + ": header", back.headers.join("|"));
    for (let r = 0; r < rows.length; r++) {
        const want = header.map((k) => ownCell(rows[r], k)).join("|");
        const got = back.rows[r].join("|");
        if (want !== got) { ok(false, "csv " + label + ": row " + r, got + " vs " + want); return; }
    }
    ok(true, "csv " + label);
}
for (let a = 0; a < KINDS.length; a++) {
    // a kind first, then every other kind interleaved with plain rows, so
    // each one meets a warm cache that belongs to a different shape
    const rows = [];
    for (let i = 0; i < 40; i++) {
        const ks = kinds(i);
        rows.push(i % 2 ? ks[a] : ks[(a + i) % KINDS.length]);
    }
    rows[0] = kinds(99)[a];
    csvCheck(rows, "led by " + KINDS[a]);
}
{
    // a long run of one shape (the cache is warm), then the slot moves
    const rows = [];
    for (let i = 0; i < 500; i++) rows.push({ id: i, name: "s" + i, score: i });
    for (let i = 0; i < 50; i++) rows.push({ name: "t" + i, id: -i, score: 0 });
    for (let i = 0; i < 50; i++) rows.push({ id: i, name: "u" + i, score: 1, extra: 1 });
    csvCheck(rows, "warm cache, then moved and extra keys");
}

{
    // numbers are formatted straight into the output, not through a string:
    // they must read exactly as String(v) does, under every delimiter
    const edge = [0, -0, 1, -1, NaN, Infinity, -Infinity, 1e21, 1e-7, 0.1 + 0.2, 2 ** 53, -(2 ** 31), 2 ** 31, 5e-324,
        1.7976931348623157e308, 123456789.125, -1e-300, 4.35, 1e20, 0.000001];
    for (const delim of [",", ".", "e", "-"]) {
        const recs = edge.map((v, i) => ({ v, i }));
        const back = csv.parse(csv.stringify(recs, { delimiter: delim }), { delimiter: delim });
        const bad = edge.findIndex((v, i) => back.rows[i][0] !== String(v) || back.rows[i][1] !== String(i));
        ok(bad < 0, "number cells read as String(v) with delimiter " + JSON.stringify(delim), bad >= 0 ? back.rows[bad].join("|") + " vs " + String(edge[bad]) : undefined);
        const arrs = csv.parse(csv.stringify(edge.map((v) => [v]), { delimiter: delim }), { delimiter: delim, hasHeader: false });
        ok(arrs.rows.every((r, i) => r[0] === String(edge[i])), "and in array rows, delimiter " + JSON.stringify(delim));
    }
}

// --- FROM_RECORDS ----------------------------------------------------------
const D0 = new df.DataFrame({ a: new Float64Array(1) });
function frameCheck(rows, label) {
    // columns: union of own enumerable string keys in first-seen order;
    // a column is numeric when every present value is a number
    const cols = [];
    for (const r of rows) for (const k of Reflect.ownKeys(r)) if (typeof k === "string" && Object.getOwnPropertyDescriptor(r, k).enumerable && !cols.includes(k)) cols.push(k);
    const isNum = cols.map((k) => rows.every((r) => r[k] === undefined || typeof r[k] === "number"));
    let back;
    try { back = D0.FROM_RECORDS(rows).TO_RECORDS(); } catch (e) { ok(false, "frame " + label, e.message); return; }
    // TO_RECORDS hands back objects, which list integer-like keys first
    const keyOrder = Object.keys(Object.fromEntries(cols.map((k) => [k, 0]))).join("|");
    ok(back.length === rows.length && Object.keys(back[0]).join("|") === keyOrder, "frame " + label + ": columns", Object.keys(back[0] || {}).join("|") + " vs " + keyOrder);
    for (let r = 0; r < rows.length; r++) {
        for (let c = 0; c < cols.length; c++) {
            const v = rows[r][cols[c]];
            const want = isNum[c] ? (v === undefined ? NaN : v) : (v === undefined ? "" : String(v));
            if (!Object.is(back[r][cols[c]], want)) { ok(false, "frame " + label + ": row " + r + " " + cols[c], String(back[r][cols[c]]) + " vs " + String(want)); return; }
        }
    }
    ok(true, "frame " + label);
}
// FROM_RECORDS refuses neither kind, but its oracle here walks keys with
// Reflect and a proxy's traps would make that the thing under test
const FRAME_KINDS = KINDS.map((_, i) => i).filter((i) => KINDS[i] !== "null-proto" && KINDS[i] !== "proxy");
for (const a of FRAME_KINDS) {
    const rows = [];
    for (let i = 0; i < 40; i++) {
        const ks = kinds(i);
        rows.push(i % 2 ? ks[a] : ks[FRAME_KINDS[(a + i) % FRAME_KINDS.length]]);
    }
    rows[0] = kinds(99)[a];
    frameCheck(rows, "led by " + KINDS[a]);
}
{
    // the shape skip: 1000 rows of one shape, then a row with a new key --
    // the new column must still be discovered
    const rows = [];
    for (let i = 0; i < 1000; i++) rows.push({ x: i, y: "y" + (i % 5) });
    rows.push({ x: -1, y: "late", z: 7 });
    for (let i = 0; i < 10; i++) rows.push({ x: i, y: "again" });
    frameCheck(rows, "a key that first appears in row 1001");
    const same = [];
    for (let i = 0; i < 300; i++) same.push({ a: i, b: i % 2 ? "odd" : "even" });
    frameCheck(same, "one shape throughout");
}
{
    // proxies can share one shape while exposing different keys: the shape
    // skip must not apply to them
    const rows = [new Proxy({ x: 1 }, {}), new Proxy({ y: "b" }, {}), new Proxy({ x: 3, z: 4 }, {})];
    const back = D0.FROM_RECORDS(rows).TO_RECORDS();
    ok(Object.keys(back[0]).join() === "x,y,z", "every proxy row's keys are enumerated", Object.keys(back[0]).join());
    ok(back[1].y === "b" && back[2].z === 4 && Number.isNaN(back[1].x), "and read through the proxy");
}
{
    // a getter that is called: FROM_RECORDS uses get semantics, and each call
    // sees its own row
    let calls = 0;
    const rows = [];
    for (let i = 0; i < 20; i++) {
        const o = { id: i };
        Object.defineProperty(o, "v", { get() { calls++; return i * 10; }, enumerable: true });
        rows.push(o);
    }
    const back = D0.FROM_RECORDS(rows).TO_RECORDS();
    ok(back.every((r, i) => r.v === i * 10), "getters run per row and return their own row's value");
    ok(calls === 40, "each getter runs once per pass (type pass + fill pass)", calls);
}

print("test_record_readers: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
