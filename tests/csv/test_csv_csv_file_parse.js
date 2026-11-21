// covers: dyna:csv.CSVFile
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_csv_file_parse");
import { parse, stringify, CSVFile } from "dyna:csv";
import { makeTempDir, removeAll, writeFile } from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) { assert(JSON.stringify(a) === JSON.stringify(b), msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, msg) { let t = false; try { fn(); } catch { t = true; } assert(t, msg); }
function throwsMsg(fn, re, msg) { let m = ""; try { fn(); } catch (e) { m = String(e.message); } assert(re.test(m), msg + " (got: " + m + ")"); }

__kitS.section("read() TSV via delimiter", () => {
    const dir = makeTempDir("dyna-csv-parse-d-");
    try {
        writeFile(dir.join("t.tsv"), "id\tv\n1\ta\n2\tb\n");
        const tsv = new CSVFile(dir.join("t.tsv"));
        eq(tsv.read({ delimiter: "\t" }).headers, ["id", "v"], "read() TSV via delimiter");
        eq(tsv.read({ delimiter: "\t" }).rows, [["1", "a"], ["2", "b"]], "read() TSV rows");
        eq(tsv.read().headers, ["id\tv"], "without the option the comma grammar keeps one field");
        throwsMsg(() => tsv.read({ delimiter: "\t", quote: "," }), /'quote' must not be the delimiter character/, "read() validates delimiter/quote too");
        tsv.close();
        writeFile(dir.join("q.csv"), "id,v\n'x,y',2\n");
        const qf = new CSVFile(dir.join("q.csv"));
        eq(qf.read({ quote: "'" }).rows, [["x,y", "2"]], "read() custom quote");
        qf.close();
        const f = new CSVFile(dir.join("bare.csv"));
        f.create({ headers: ["a", "b"] });
        eq(f.addRow(["x", "1"]).added, 1, "bare row added count");
        eq(f.read().rows[0], ["x", "1"], "bare row written");
        f.addRow({ rows: [["y", "2"]] });
        eq(f.read().totalRows, 2, "the {rows} bag still works");
        throws(() => f.addRow({}), "an empty bag has no 'rows' array");
        throwsMsg(() => f.addRow([["z", "3"]]), /a bare array is one positional row/, "nested bare array refused");
        throws(() => f.addRow(5), "a non-object non-array argument is refused");
        f.close();
    } finally {
        removeAll(dir);
    }
});
__kitS.end();

print("test_csv_parse: all tests passed (" + n + " assertions)");
