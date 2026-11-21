// covers: dyna:csv.stringify
// F-csv-1: a string cell went through JS_ToCString and was then read as a C string, so everything after its first
// U+0000 was dropped, along with the quoting the rest needed: stringify([["a\0b,c","x"]]) gave "a,x\n". Rows and
// record objects both. A cell is now written with its full length. Oracle: RFC 4180 written in JS (quote a field
// that holds the delimiter, the quote, CR or LF; double the quote inside), which treats NUL as an ordinary byte, as
// Python's csv.writer does; and csv.parse must give back the exact cells.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_stringify_nul_cells");
import * as csv from "dyna:csv";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

function field(s, d = ",", q = "\"") {
    s = String(s);
    return s.includes(d) || s.includes(q) || s.includes("\n") || s.includes("\r") ? q + s.split(q).join(q + q) + q : s;
}
const ref = (rows, d, q) => rows.map((r) => r.map((c) => field(c, d, q)).join(d) + "\n").join("");

const CELLS = ["a\0b,c", "\0", "\0\0", "x\0", "\0y", "a\0\"b", "line\0\nnext", "é\0日", "\0,\0", "plain"];

__kitS.section("array rows keep every byte after a NUL (the ISSUES repro first)", () => {
    eq(csv.stringify([["a\0b,c", "x"]]), "\"a\0b,c\",x\n", "ISSUES repro");
    for (const c of CELLS) eq(csv.stringify([[c, "z"]]), ref([[c, "z"]]), "cell " + JSON.stringify(c));
    eq(csv.stringify([CELLS]), ref([CELLS]), "all cells in one row");
});

__kitS.section("record rows keep every byte after a NUL", () => {
    eq(csv.stringify([{ a: "a\0b,c", b: "x" }]), "a,b\n\"a\0b,c\",x\n", "ISSUES repro, record form");
    const recs = CELLS.map((c, i) => ({ k: c, i: String(i) }));
    eq(csv.stringify(recs), "k,i\n" + ref(recs.map((r) => [r.k, r.i])), "every cell as a record value");
});

__kitS.section("a custom delimiter and quote see the bytes after a NUL", () => {
    const rows = [["a\0;b", "q\0'x"], ["\0", "ok"]];
    eq(csv.stringify(rows, { delimiter: ";", quote: "'" }), ref(rows, ";", "'"), "delimiter ; quote '");
});

__kitS.section("parse gives back the exact cells", () => {
    const rows = [CELLS, CELLS.slice().reverse()];
    const back = csv.parse(csv.stringify(rows), { hasHeader: false }).rows;
    eq(JSON.stringify(back), JSON.stringify(rows), "round trip of NUL-bearing cells");
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_csv_stringify_nul_cells: " + n + " checks passed");
__kitS.end();
