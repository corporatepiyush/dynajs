// covers: contract dyna:csv.CSVFile.create dyna:csv.CSVFile.addRow -- both write cells through the same length-aware field writer as stringify (F-csv-1)
// F-csv-1, file side: CSVFile.create and CSVFile.addRow read each cell with JS_ToCString and wrote strlen() bytes,
// so a cell lost everything after its first U+0000 (and the quotes its comma needed). The file bytes now hold the
// whole cell. Oracle: the RFC 4180 rule in JS (quote a field holding , " CR or LF, double the quote inside) for the
// exact file text, and read() must return the exact cells.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_csv_file_nul_cells");
import { CSVFile } from "dyna:csv";
import { makeTempDir, removeAll, readFile } from "dyna:file";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
const field = (s) => (/[,"\r\n]/.test(s) ? "\"" + s.split("\"").join("\"\"") + "\"" : s);
const ref = (rows) => rows.map((r) => r.map(field).join(",") + "\n").join("");

const dir = makeTempDir("dyna-csv-nul-");
const ROWS = [["a\0b,c", "x"], ["\0", "q\0\"z"], ["é\0日", "\0,\0"]];

__kitS.section("create writes every byte after a NUL", () => {
    const p = dir.join("create.csv");
    new CSVFile(p).create({ headers: ["A", "B"], rows: ROWS });
    eq(readFile(p), "A,B\n" + ref(ROWS), "file text");
    eq(JSON.stringify(new CSVFile(p).read().rows), JSON.stringify(ROWS), "read() gives the cells back");
});

__kitS.section("addRow writes every byte after a NUL (positional and record rows)", () => {
    const p = dir.join("add.csv");
    const f = new CSVFile(p);
    f.create({ headers: ["A", "B"] });
    f.addRow(ROWS[0]);
    f.addRow({ rows: [ROWS[1]] });
    f.addRow({ rows: [{ A: ROWS[2][0], B: ROWS[2][1] }] });
    eq(readFile(p), "A,B\n" + ref(ROWS), "file text");
    eq(JSON.stringify(new CSVFile(p).read().rows), JSON.stringify(ROWS), "read() gives the cells back");
});

removeAll(dir);
if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_csv_csv_file_nul_cells: " + n + " checks passed");
__kitS.end();
