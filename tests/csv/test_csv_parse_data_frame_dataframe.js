// covers: dyna:csv.parseDataFrame
// csv.parseDataFrame(text, opts) and CSVFile#toDataFrame(opts): CSV straight into a DataFrame in one C pass
// (src/core/dfk/dfk-csv.c), no JS string per cell. Owner ruling B1 (Track H, 2026-11): an untyped column is f64
// when every non-empty cell is a decimal number, else str; an empty cell is NaN in f64 and "" in str; dtypes
// force "f64" | "i32" | "str". Oracles: JS Number() for every numeric cell (the reader must round exactly as
// Number does, on the fast path and the long-digit path alike), and csv.parse for quoting, CRLF, BOM and
// ragged rows. Before B1 neither function existed: every row here is red on the old binary.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_parse_data_frame_dataframe");
import { parseDataFrame, parse, CSVFile } from "dyna:csv";
import { isDataFrame } from "dyna:dataframe";
import { makeTempDir, writeFile, Path } from "dyna:file";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
const err = (f) => { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } };
const cols = (df) => {
    const out = {};
    for (const [k, v] of Object.entries(df.TO_COLUMNS())) out[k] = Array.isArray(v) ? v : Array.from(v, (x) => Object.is(x, -0) ? "-0" : x !== x ? "NaN" : x);
    return out;
};

__kitS.section("parseDataFrame returns a DataFrame, with no dyna:dataframe constructor touched first", () => {
    const df = parseDataFrame("id,region,qty,price,note\n1,r1,2,3.5,x\n2,r2,,4,\n3,r1,5,1e2,\"a,b\"\n");
    check(isDataFrame(df), "parseDataFrame returns a DataFrame, with no dyna:dataframe constructor touched first");
    eq(df.DTYPES(), { id: "f64", region: "str", qty: "f64", price: "f64", note: "str" }, "inference: all-number columns are f64, others str");
    eq(cols(df), { id: [1, 2, 3], region: ["r1", "r2", "r1"], qty: [2, "NaN", 5], price: [3.5, 4, 100], note: ["x", "", "a,b"] }, "empty cell: NaN in f64, \"\" in str");
    eq(df.GROUP_BY_SUM("region", "price").keys, ["r1", "r2"], "a str column groups like a string[] column, keys in first-appearance order");
});

__kitS.section("a column that turns non-numeric late is str", () => {
    const df = parseDataFrame("a\n1\n2\nx\n3\n1\n");
    eq(df.DTYPES(), { a: "str" }, "a column that turns non-numeric late is str");
    eq(cols(df).a, ["1", "2", "x", "3", "1"], "and its earlier cells keep their text, in order");
    eq(df.UNIQUE("a"), ["1", "2", "x", "3"], "dictionary order is first appearance, even after the late switch");
    eq(cols(parseDataFrame("a,b\n,\n,\nq,1\n")), { a: ["", "", "q"], b: ["NaN", "NaN", 1] }, "leading empties then text: \"\" first; leading empties then numbers: NaN");
});

__kitS.section("a inf nan 1", () => {
    eq(parseDataFrame("a\ninf\nnan\n 1\n").DTYPES(), { a: "str" }, "inf, nan and a padded number are text, not numbers");
    eq(cols(parseDataFrame("a\n-0\n.5\n5.\n1e5\n1E-3\n+7\n")).a, ["-0", 0.5, 5, 100000, 0.001, 7], "decimal grammar: sign, bare point, exponent");
    eq(parseDataFrame("a\n0x10\n").DTYPES(), { a: "str" }, "hex is not a CSV number");
    eq(parseDataFrame("a\n1_000\n").DTYPES(), { a: "str" }, "underscores are not a CSV number");
    let text = "v\n";
    const want = [];
    let seed = 7;
    const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
    for (let i = 0; i < 4000; i++) {
        const digits = 1 + Math.floor(rnd() * 22);
        let s = (rnd() < 0.3 ? "-" : "") + String(Math.floor(rnd() * 10));
        for (let d = 1; d < digits; d++) s += String(Math.floor(rnd() * 10));
        const dot = Math.floor(rnd() * (s.length + 1));
        if (rnd() < 0.7 && dot > (s[0] === "-" ? 1 : 0)) s = s.slice(0, dot) + "." + s.slice(dot);
        if (rnd() < 0.3) s += "e" + (Math.floor(rnd() * 80) - 40);
        if (i % 500 === 0) s = ["4.9e-324", "1.7976931348623157e308", "2.2250738585072014e-308", "9007199254740993", "0.1", "1e23"][i / 500 % 6];
        text += s + "\n";
        want.push(Number(s));
    }
    const got = parseDataFrame(text).TO_COLUMNS().v;
    let bad = 0, first = "";
    for (let i = 0; i < want.length; i++)
        if (!Object.is(got[i], want[i])) { if (!bad) first = i + ": " + text.split("\n")[i + 1] + " -> " + got[i] + " want " + want[i]; bad++; }
    check(bad === 0, "4000 random decimals parse exactly as Number() does, fast and long-digit paths (" + bad + " differ; first " + first + ")");
});

__kitS.section("columns selects and orders", () => {
    const df = parseDataFrame("a,b,c\n1,2,3\n4,5,6\n7,8,9\n", { columns: ["c", "a"], offset: 1, limit: 1, dtypes: { a: "i32" } });
    eq(df.COLUMNS, ["c", "a"], "columns selects and orders");
    eq(df.DTYPES(), { c: "f64", a: "i32" }, "dtypes forces i32");
    eq(cols(df), { c: [6], a: [4] }, "offset skips data rows, limit caps them");
    eq(parseDataFrame("a\n1\n2\n", { limit: 0 }).ROWS, 0, "limit 0 is an empty frame");
    eq(parseDataFrame("a\n1\n2\n", { offset: 5 }).ROWS, 0, "offset past the end is an empty frame");
    eq(parseDataFrame("a,b\n1,2\n", { columns: [] }).COLS, 0, "columns [] is a frame with no columns");
    eq(parseDataFrame("a,b\n1,x\n2,3\n", { dtypes: { b: "str" } }).DTYPES(), { a: "f64", b: "str" }, "dtypes forces str");
    eq(cols(parseDataFrame("a\n1\n\n2\n", { dtypes: { a: "f64" } })).a, [1, "NaN", 2], "a forced f64 keeps NaN for an empty cell (one column: a blank line IS an empty cell)");
    eq(cols(parseDataFrame("a\n2147483647\n-2147483648\n", { dtypes: { a: "i32" } })).a, [2147483647, -2147483648], "i32 range ends");
});

__kitS.section("a blank line is skipped when the header has 2+ columns", () => {
    eq(cols(parseDataFrame("a,b\n1,x\n\n2,y\n")), { a: [1, 2], b: ["x", "y"] }, "a blank line is skipped when the header has 2+ columns");
    eq(cols(parseDataFrame("a,b\r\n1,x\r\n2,y")), { a: [1, 2], b: ["x", "y"] }, "CRLF and a missing final newline");
    eq(parseDataFrame("﻿a,b\n1,2\n").COLUMNS, ["a", "b"], "a UTF-8 BOM is not part of the first name");
    eq(cols(parseDataFrame("a,b\n1\n2,3,4\n")), { a: [1, 2], b: ["NaN", 3] }, "ragged rows: a short row pads, a long row drops the extra cells");
    const q = 'a,b\n"x""y",2\n"multi\nline",3\n';
    eq(cols(parseDataFrame(q)).a, parse(q).rows.map((r) => r[0]), "quoted cells match csv.parse (doubled quote, embedded newline)");
    eq(cols(parseDataFrame("a;b\n1;'x;y'\n", { delimiter: ";", quote: "'" })), { a: [1], b: ["x;y"] }, "delimiter and quote options");
    eq(parseDataFrame("a,b\n").ROWS, 0, "a header-only text is a 0-row frame");
    eq(parseDataFrame("a,b\n").DTYPES(), { a: "f64", b: "f64" }, "and its untyped columns are f64");
});

__kitS.section("SyntaxError: csv.parseDataFrame: row 2: 1 fields where the header has 2", () => {
    eq(err(() => parseDataFrame("a,b\n1\n", { strict: true })), "SyntaxError: csv.parseDataFrame: row 2: 1 fields where the header has 2", "strict: a ragged row is refused with its row");
    eq(err(() => parseDataFrame('a\n"open\n', { strict: true })), "SyntaxError: csv.parseDataFrame: row 2: unterminated quoted field", "strict: unterminated quote");
    eq(err(() => parseDataFrame('a\n"x"y\n', { strict: true })), "SyntaxError: csv.parseDataFrame: row 2: unexpected text after a closing quote", "strict: text after a closing quote");
    eq(err(() => parseDataFrame("a\n1\nx\n", { dtypes: { a: "f64" } })), "TypeError: csv.parseDataFrame: row 3: column 'a': 'x' is not a number", "a forced f64 names the row, column and cell");
    eq(err(() => parseDataFrame("a\n2147483648\n", { dtypes: { a: "i32" } })), "TypeError: csv.parseDataFrame: row 2: column 'a': '2147483648' is not a 32-bit integer", "i32 overflow is refused");
    eq(err(() => parseDataFrame("a\n\n", { dtypes: { a: "i32" } })), "TypeError: csv.parseDataFrame: row 2: column 'a': '' is not a 32-bit integer", "i32 has no NaN: an empty cell is refused");
    eq(err(() => parseDataFrame("a,a\n1,2\n")), "TypeError: csv.parseDataFrame: duplicate column 'a' in the header", "duplicate header names");
    eq(err(() => parseDataFrame("a\n1\n", { columns: ["b"] })), "TypeError: csv.parseDataFrame: no such column 'b'", "an unknown column");
    eq(err(() => parseDataFrame("a,b\n1,2\n", { columns: ["a", "a"] })), "TypeError: csv.parseDataFrame: column 'a' is listed twice", "a column listed twice");
    eq(err(() => parseDataFrame("a,b\n1,2\n", { columns: ["a"], dtypes: { b: "str" } })), "TypeError: csv.parseDataFrame: dtypes names 'b', which is not a selected column", "dtypes for an unselected column");
    eq(err(() => parseDataFrame("a\n1\n", { dtypes: { a: "int" } })), 'TypeError: csv.parseDataFrame: dtypes[\'a\'] must be "f64", "i32" or "str"', "an unknown dtype");
    eq(err(() => parseDataFrame("a\n1\n", { bogus: 1 })), 'TypeError: csv.parseDataFrame: unknown option "bogus" (valid: columns, dtypes, offset, limit, delimiter, quote, strict)', "an unknown option");
    eq(err(() => parseDataFrame("a\n1\n", { offset: -1 })), "RangeError: csv.parseDataFrame: 'offset' must be a non-negative integer", "a negative offset");
    eq(err(() => parseDataFrame("")), "TypeError: csv.parseDataFrame: the input is empty (no header)", "empty text");
    eq(err(() => parseDataFrame(42)), "TypeError: csv.parseDataFrame: expected CSV text (a string)", "a non-string");
    const sparse = []; sparse.length = 2 ** 32 - 1;
    eq(err(() => parseDataFrame("a\n1\n", { columns: sparse })), "RangeError: csv: 'columns' has more entries than a header can hold", "a 2^32-1 slot columns list is refused before it is walked");
});
__kitS.end();
print("test_csv_dataframe: " + n + " checks, " + fails + " failed");
if (fails) throw new Error("test_csv_dataframe: " + fails + " failures");
