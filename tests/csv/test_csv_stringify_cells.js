// csv.stringify cell rules, found by the signature-driven input generator
// (tests/lib/sigfuzz.js):
//   - an undefined cell or a hole is an EMPTY field in both row forms (array
//     rows used to print the text "undefined" while record rows printed "");
//   - null prints as null in both forms;
//   - a row with more cells than any string can hold is a RangeError at
//     once (a sparse row of length 2^32-1 used to run for minutes building
//     gigabytes of delimiters before anything refused it).
import * as csv from "dyna:csv";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

eq(csv.stringify([["a", undefined, "b"]]), "a,,b\n", "an undefined cell in an array row is an empty field");
eq(csv.stringify([[1, , 3]]), "1,,3\n", "a hole in an array row is an empty field");
eq(csv.stringify([[undefined]]), "\n", "a row of one undefined cell is an empty line");
eq(csv.stringify([["x", null]]), "x,null\n", "null prints as null in an array row");
eq(csv.stringify([{ a: 1, b: undefined }, { a: null, b: 2 }]), "a,b\n1,\nnull,2\n", "and the same rules hold for record rows");
eq(csv.stringify([[1, "", 3]]), "1,,3\n", "an empty string is still an empty field (control)");

for (const len of [2 ** 30, 2 ** 31, 2 ** 32 - 1]) {
    const row = [];
    row.length = len;
    const t0 = performance.now();
    let msg = "no throw";
    try { csv.stringify([row]); } catch (e) { msg = e.constructor.name + ": " + e.message; }
    const dt = performance.now() - t0;
    ok(/^RangeError: .*more than any string can hold/.test(msg), "a " + len + "-cell row is refused as too long (" + msg + ")");
    ok(dt < 1000, "and refused before doing the work (" + dt.toFixed(1) + " ms)");
}
{
    const row = [];
    row.length = 1000;
    eq(csv.stringify([row]).length, 1000, "a 1000-cell sparse row is 999 delimiters and a newline (control)");
}

print("test_csv_stringify_cells: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
