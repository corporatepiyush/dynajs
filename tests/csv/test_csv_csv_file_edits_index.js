// covers: integration dyna:csv.CSVFile.updateCell dyna:csv.CSVFile.addRow dyna:csv.CSVFile.removeRow dyna:csv.CSVFile.addColumn dyna:csv.CSVFile.removeColumn dyna:csv.CSVFile.renameColumn -- the six edits share one index-plus-overlay writer; checked against a JS model of the edit over parse()
// M1 step 5: CSVFile's edits used to parse the whole file into a malloc'd
// string-per-cell Table, change it and re-serialise every cell (34 MB:
// ~185 ms for a one-cell update). They now index the mapped file and stream
// it back through a small overlay (drop a row, set a cell, add or drop a
// column): a row with no quote byte that ends in LF is already in canonical
// form, so runs of such rows are copied as they are, and only touched or
// quoted rows are re-emitted cell by cell (same file: ~19 ms; add/remove
// column ~34 ms). The output must stay the canonical text the Table wrote:
// LF endings, a field quoted only when it holds a comma, quote, CR or LF,
// ragged rows kept at their own width. The oracle applies each edit to the
// parse() rows in JS and serialises that canonically. Two behaviours differ
// from the Table version (both red there): a NUL byte inside an existing
// cell used to cut the cell short on ANY edit of the file (data loss in rows
// the edit never named), and a defaultValue that cannot become a string
// (a Symbol) was ignored with the error left pending; it now throws and the
// file is not written.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_csv_file_edits_index");
import { CSVFile, parse } from "dyna:csv";
import { makeTempDir, removeAll, writeFile, readFile, Path } from "dyna:file";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }
const J = JSON.stringify;
const dir = makeTempDir("dyna-csv-edits-");
const p = new Path(dir + "/t.csv");

const field = (s) => /[,"\r\n]/.test(s) ? "\"" + s.replace(/"/g, "\"\"") + "\"" : s;
const serialise = (rows) => rows.map((r) => r.map(field).join(",") + "\n").join("");

// refScan: the scalar scanner's record rules (copied from test_csv_index.js,
// where it is fuzzed against the C scanner); unlike parse() it keeps each
// record's own width, which the edits must preserve.
const DEC = new TextDecoder();
function refScan(text, delim, quote, strict) {
    const B = new TextEncoder().encode(text), D = delim.charCodeAt(0), Q = quote.charCodeAt(0), LF = 10, CR = 13;
    const rows = [];
    let row = [];
    let i = 0;
    const len = B.length;
    if (len === 0) return rows;
    for (;;) {
        if (i < len && B[i] === Q) {
            const f = [];
            let closed = false;
            i++;
            while (i < len) {
                if (B[i] !== Q) { f.push(B[i++]); continue; }
                if (i + 1 < len && B[i + 1] === Q) { f.push(Q); i += 2; continue; }
                i++;
                closed = true;
                break;
            }
            if (strict) {
                if (!closed) throw new SyntaxError("row " + (rows.length + 1) + ": unterminated quoted field");
                if (i < len && B[i] !== D && B[i] !== LF && B[i] !== CR)
                    throw new SyntaxError("row " + (rows.length + 1) + ": unexpected text after a closing quote");
            }
            row.push(DEC.decode(new Uint8Array(f)));
        } else {
            let j = i;
            while (j < len && B[j] !== D && B[j] !== LF && B[j] !== CR) j++;
            row.push(DEC.decode(B.subarray(i, j)));
            i = j;
        }
        if (i >= len) { rows.push(row); break; }
        const c = B[i];
        if (c === D) { i++; continue; }
        if (c === CR && i + 1 < len && B[i + 1] === LF) i += 2;
        else i++;
        rows.push(row);
        row = [];
        if (i >= len) break;
    }
    return rows;
}

function model(text) {
    parse(text);
    if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);
    return refScan(text, ",", "\"", false);
}

function outcome(fn) {
    try { return fn(); } catch (e) { return { err: e.name + ": " + e.message.replace(/'[^']*'/g, "P") }; }
}

const OPS = [
    ["removeRow 0", (f) => f.removeRow({ row: 0 }), (m) => { if (m.length < 2) return { err: "RangeError: csv.removeRow: row 0 out of range" }; m.splice(1, 1); return { removed: 0, totalRows: m.length - 1 }; }],
    ["removeRow last", (f, m0) => f.removeRow({ row: Math.max(0, m0.length - 2) }), (m) => { const r = Math.max(0, m.length - 2); if (m.length < 2) return { err: "RangeError: csv.removeRow: row 0 out of range" }; m.splice(r + 1, 1); return { removed: r, totalRows: m.length - 1 }; }],
    ["updateCell b", (f) => f.updateCell({ row: 0, columnIndex: 1, value: "v,\"x" }), (m) => {
        if (m.length < 2) return { err: "RangeError: csv.updateCell: row 0 out of range (the file has no data rows)" };
        if (m[0].length < 2) return { err: "RangeError: csv.updateCell: columnIndex 1 out of range" };
        while (m[1].length < 2) m[1].push("");
        m[1][1] = "v,\"x";
        return { row: 0, column: m[0][1], value: "v,\"x" };
    }],
    ["updateCell first", (f, m0) => f.updateCell({ row: m0.length - 2, column: m0[0][0], value: "" }), (m) => {
        const r = m.length - 2;
        if (m.length < 2) return { err: "RangeError: csv.updateCell: row -1 out of range (the file has no data rows)" };
        while (m[r + 1].length < 1) m[r + 1].push("");
        m[r + 1][m[0].indexOf(m[0][0])] = "";
        return { row: r, column: m[0][0], value: "" };
    }],
    ["addColumn", (f) => f.addColumn({ column: "n\"", defaultValue: "d\n" }), (m) => {
        if (m[0].includes("n\"")) return { err: "TypeError: csv.addColumn: column P already exists" };
        const w = m[0].length;
        m.forEach((row, i) => { while (row.length < w) row.push(""); row.push(i ? "d\n" : "n\""); });
        return { column: "n\"", totalColumns: w + 1 };
    }],
    ["removeColumn 0", (f) => f.removeColumn({ columnIndex: 0 }), (m) => {
        for (const row of m) if (row.length > 0) row.splice(0, 1);
        return { removedIndex: 0, totalColumns: m[0].length };
    }],
    ["removeColumn last", (f, m0) => f.removeColumn({ columnIndex: m0[0].length - 1 }), (m) => {
        const c = m[0].length - 1;
        for (const row of m) if (c < row.length) row.splice(c, 1);
        return { removedIndex: c, totalColumns: m[0].length };
    }],
    ["renameColumn", (f, m0) => f.renameColumn({ oldName: m0[0][0], newName: "z,z" }), (m) => {
        const old = m[0][0];
        if (old !== "z,z" && m[0].includes("z,z")) return { err: "TypeError: csv.renameColumn: column P already exists" };
        m[0][m[0].indexOf(old)] = "z,z";
        return { oldName: old, newName: "z,z" };
    }],
    ["addRow", (f) => f.addRow({ rows: [[1, "2,", 3, 4], { [f.read().headers[0]]: "x\"" }, []] }), (m) => {
        const w = m[0].length;
        const a = [1, "2,", 3, 4].slice(0, w).map(String);
        while (a.length < w) a.push("");
        const o = m[0].map((h) => h === m[0][0] ? "x\"" : "");
        m.push(a, o, m[0].map(() => ""));
        return { added: 3, totalRows: m.length - 1 };
    }],
    ["addRow bare", (f) => f.addRow(["p", "q\r"]), (m) => {
        const a = ["p", "q\r"].slice(0, m[0].length);
        while (a.length < m[0].length) a.push("");
        m.push(a);
        return { added: 1, totalRows: m.length - 1 };
    }],
];

function compare(text, tag) {
    let m0;
    try { m0 = model(text); } catch (e) { return; }
    if (m0[0].length === 0) return;
    for (const [name, op, apply] of OPS) {
        writeFile(p, text);
        const m = model(text);
        const want = apply(m);
        const got = outcome(() => op(new CSVFile(p), m0));
        const after = readFile(p);
        if (want.err) {
            check(got.err === want.err.replace(/'[^']*'/g, "P"), tag + " " + name + " error " + J(got) + " vs " + J(want));
            check(after === text, tag + " " + name + " left the file alone after an error");
        } else {
            check(J(got) === J(want), tag + " " + name + " result " + J(got) + " vs " + J(want));
            check(after === serialise(m), tag + " " + name + " text " + J(after) + " vs " + J(serialise(m)));
        }
    }
}

const fixed = [
    "a\n", "a,b\n1,2\n", "﻿a,b\n1,2\n", "a,b\n1\n1,2,3\n", "a,b\n\n1,2\n", "\"a\nb\",c\n\"x\"\"y\",z\n",
    "a,b\n\"x\"y,2\n", "\"\",b\n1,2\n", "a,b\r\n1,2\r\n", "a,b\r1,2\r", ",\n,\n", "a\n\n\n", "a,b\n1,2",
    "\"a\",\"b\"\n\"1\",\"2\"\n", "a,b\n1,2\n3,4\n5,6\n", "a,b,c\n1,2,3\n\"q\",5,6\n7,8,9\n", "a,b\n1\u00002,3\n\u0000,x\n",
];
for (let i = 0; i < fixed.length; i++) compare(fixed[i], "fixed[" + i + "]");

let seed = 4242;
const rnd = (k) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % k; };
const atoms = ["a", "b", "", ",", "\n", "\r\n", "\r", "\"", "\"\"", "x y", "é", "\"q,\"", "1.5", " ", "\"a\"b", "h", "\u0000"];
for (let i = 0; i < 200; i++) {
    let t = ["a,b", "a,b,c", "\"a\",b", "a"][rnd(4)] + ["\n", "\r\n", "\r"][rnd(3)];
    const k = rnd(25);
    for (let j = 0; j < k; j++) t += atoms[rnd(atoms.length)];
    compare(t, "fuzz[" + i + "]");
}

writeFile(p, "a,b\n1,2\n");
const sym = outcome(() => new CSVFile(p).addColumn({ column: "n", defaultValue: Symbol("s") }));
check(sym.err && sym.err.startsWith("TypeError"), "a Symbol defaultValue throws " + J(sym));
check(readFile(p) === "a,b\n1,2\n", "and the file is not written");
writeFile(p, "a,b\n\"x\nunterminated\n");
check(outcome(() => new CSVFile(p).removeRow({ row: 0, strict: true })).err === "SyntaxError: csv: row 2: unterminated quoted field", "strict edits report the scalar scanner's error");
writeFile(p, "");
check(outcome(() => new CSVFile(p).updateCell({ row: 0, column: "a", value: "x" })).err === "TypeError: csv: P is empty (no header)", "an edit of an empty file");

__kitS.section("four edits in a row over a 120,000-row file (copied runs + quoted rows) match the model", () => {
    let text = "id,q,w\n";
    for (let i = 0; i < 120000; i++) text += i + "," + (i % 1000 === 7 ? "\"v,\"\"" + i + "\"" : "v" + i) + "," + (i % 3 ? "k" : "") + "\n";
    writeFile(p, text);
    const m = model(text);
    const f = new CSVFile(p);
    f.updateCell({ row: 60000, column: "w", value: "new" });
    m[60001][2] = "new";
    f.removeRow({ row: 7 });
    m.splice(8, 1);
    f.addColumn({ column: "z", defaultValue: "0" });
    m.forEach((row, i) => row.push(i ? "0" : "z"));
    f.removeColumn({ column: "q" });
    m.forEach((row) => row.splice(1, 1));
    check(readFile(p) === serialise(m), "four edits in a row over a 120,000-row file (copied runs + quoted rows) match the model");
});
__kitS.end();

removeAll(dir);
print(`test_csv_csv_file_edits_index: ${n - failures}/${n} passed`);
if (failures) throw new Error(failures + " failures");
