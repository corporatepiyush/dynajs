// covers: dyna:csv.parseDataFrame
// M1 step 3: parseDataFrame (and CSVFile#toDataFrame) build their columns
// from the shared structural index when the text is regular (every record
// the header's width, quoting regular, unique header names, every requested
// column and dtype a header name): f64 / i32 columns are parsed in C, text
// columns dictionary-encoded straight from the index, several threads for big
// inputs. Anything else (ragged rows, blank lines, bad cells in forced
// columns, unknown names, strict-mode errors) goes through the original
// reader, so its rules and messages are unchanged. Oracle: parse() (itself
// checked against a scalar reference) plus the documented inference rule,
// applied in JS; errors are compared with the original reader's text, which
// the fallback path produces.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_parse_data_frame_index");
import { parse, parseDataFrame } from "dyna:csv";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }
const J = JSON.stringify;
const NUM = /^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/;

function expected(text, opts) {
    opts = opts || {};
    const p = parse(text, { delimiter: opts.delimiter, quote: opts.quote });
    const names = opts.columns || p.headers;
    const off = opts.offset || 0, lim = opts.limit === undefined ? Infinity : opts.limit;
    const rows = p.rows.slice(off, off + lim);
    const out = {};
    for (const name of names) {
        const c = p.headers.indexOf(name);
        const cells = rows.map((r) => r[c]);
        const forced = opts.dtypes && opts.dtypes[name];
        const numeric = cells.every((x) => x === "" || NUM.test(x));
        if (forced === "str" || (!forced && !numeric)) out[name] = cells;
        else if (forced === "i32") out[name] = Array.from(new Int32Array(cells.map(Number)));
        else out[name] = Array.from(new Float64Array(cells.map((x) => (x === "" ? NaN : Number(x)))));
    }
    return out;
}
function actual(text, opts) {
    const cols = parseDataFrame(text, opts).TO_COLUMNS();
    const out = {};
    for (const k of Object.keys(cols)) out[k] = Array.from(cols[k]);
    return out;
}
function same(a, b) { return J(Object.keys(a)) === J(Object.keys(b)) && Object.keys(a).every((k) => a[k].length === b[k].length && a[k].every((x, i) => Object.is(x, b[k][i]))); }

let seed = 4242;
function rnd(k) { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % k; }
const CELLS = ["1", "-2.5", "3e2", ".5", "7.", "", "x", "\"q,1\"", "\"a\"\"b\"", "\"5\"", "\u00e9", "+4", "1e400", "0.1", "12345678901234567890"];
function genText(rows, width, numericBias) {
    const head = Array.from({ length: width }, (_, c) => "c" + c);
    let s = head.join(",") + "\n";
    for (let r = 0; r < rows; r++) {
        const row = [];
        for (let c = 0; c < width; c++) row.push(c < numericBias ? ["1", "-2.5", "3e2", ".5", "", "0.1", "+4"][rnd(7)] : CELLS[rnd(CELLS.length)]);
        s += row.join(",") + (rnd(4) ? "\n" : "\r\n");
    }
    return s;
}

for (let t = 0; t < 200; t++) {
    const text = genText(1 + rnd(40), 1 + rnd(5), rnd(4));
    const opts = [undefined, { offset: 2 }, { limit: 3 }, { offset: 1, limit: 2 }, { columns: ["c0"] }, { dtypes: { c0: "str" } }][t % 6];
    check(same(actual(text, opts), expected(text, opts)), "case " + t + " " + J(opts) + ": " + J(text.slice(0, 80)));
}
__kitS.section("300k rows, 6 columns: typed and dictionary columns equal the oracle (multi-thread path)", () => {
    const text = genText(300000, 6, 3);
    const a = actual(text), e = expected(text);
    check(same(a, e), "300k rows, 6 columns: typed and dictionary columns equal the oracle (multi-thread path)");
    const sub = { columns: ["c4", "c1"], offset: 1000, limit: 50000, dtypes: { c1: "str" } };
    check(same(actual(text, sub), expected(text, sub)), "column selection, offset, limit and a forced str dtype on the big input");
});
__kitS.section("forced i32", () => {
    const i32 = "a,b\n1,x\n-7,y\n";
    check(same(actual(i32, { dtypes: { a: "i32" } }), { a: [1, -7], b: ["x", "y"] }), "forced i32");
    const err = (t, o) => { try { parseDataFrame(t, o); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } };
    check(err("a\n1\n2.5\n", { dtypes: { a: "i32" } }).startsWith("TypeError: csv.parseDataFrame: row 3"), "a non-integer in a forced i32 column keeps the reader's message: " + err("a\n1\n2.5\n", { dtypes: { a: "i32" } }));
    check(err("a\nx\n", { dtypes: { a: "f64" } }).startsWith("TypeError: csv.parseDataFrame: row 2"), "text in a forced f64 column keeps the reader's message");
    check(err("a,a\n1,2\n").startsWith("TypeError"), "duplicate header names keep the reader's error");
    check(err("a\n1\n", { columns: ["zz"] }).startsWith("TypeError"), "an unknown column keeps the reader's error");
    check(err("", {}).startsWith("TypeError"), "empty text is a TypeError");
    check(err("a,b\n1\n", { strict: true }).startsWith("SyntaxError"), "strict width errors keep the reader's SyntaxError");
    check(same(actual("a,b\n1,2\n\n3,4\n"), { a: [1, 3], b: [2, 4] }), "a blank line is still skipped (ragged text takes the reader's path)");
    check(same(actual("a,b\n"), { a: [], b: [] }) && parseDataFrame("a,b\n").DTYPES().a === "f64", "header-only text is a 0-row frame of f64 columns");
    check(same(actual("\ufeffa\n5\n"), { a: [5] }), "a BOM is stripped");
});
__kitS.end();

print("test_csv_parse_data_frame_index: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_csv_parse_data_frame_index: " + failures + " failures");
