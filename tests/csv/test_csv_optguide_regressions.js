// Split from tests/test_optguide_regressions.js: the csv sections.
import { CSVFile } from "dyna:csv";
import { Path, makeTempDir, removeAll } from "dyna:file";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}
const TMPDIR = makeTempDir("optguide_csv_");
const TMP = String(TMPDIR) + "/t";
{
    const mk = (n, rows) => {
        const f = new CSVFile(new Path(TMP + "_" + n + ".csv"));
        f.create({ headers: ["A", "B", "C"], rows: rows || [["1", "2", "3"], ["4", "5", "6"]],
                   overwrite: true });
        return f;
    };
    let threw = null;
    try { mk("rn").renameColumn({ oldName: "B", newName: "" }); } catch (e) { threw = e; }
    ok(!threw, "renameColumn to an empty name is accepted" + (threw ? ": " + threw.message : ""));
    threw = null;
    try { mk("ac").addColumn({ column: "" }); } catch (e) { threw = e; }
    ok(!threw, "addColumn with an empty name is accepted" + (threw ? ": " + threw.message : ""));
    threw = null;
    try { mk("ad").addColumn({ column: "D", defaultValue: "" }); } catch (e) { threw = e; }
    ok(!threw, "addColumn with an empty defaultValue is accepted");

    for (const [name, fn] of [
        ["updateCell",   f => f.updateCell({ row: 0, column: "B", value: "x" })],
        ["updateCell->empty", f => f.updateCell({ row: 0, column: "B", value: "" })],
        ["removeRow",    f => f.removeRow({ row: 0 })],
        ["removeColumn", f => f.removeColumn({ column: "B" })],
        ["renameColumn", f => f.renameColumn({ oldName: "B", newName: "Z" })],
        ["addColumn",    f => f.addColumn({ column: "D", defaultValue: "d" })],
        ["addRow",       f => f.addRow({ rows: [["7", "8", "9"]] })],
        ["addRow named", f => f.addRow({ rows: [{ A: "7", B: "8", C: "9" }] })],
    ]) {
        let err = null;
        try { fn(mk("m")); } catch (e) { err = e; }
        ok(!err, "csv " + name + " on a parsed table" + (err ? ": " + err.message : ""));
    }

    {
        const big = "x".repeat(70000);
        const f = new CSVFile(new Path(TMP + "_big.csv"));
        const rows = [];
        for (let i = 0; i < 20; i++) rows.push([big, "t" + i, "u" + i]);
        f.create({ headers: ["A", "B", "C"], rows: rows, overwrite: true });
        const got = f.readRowRange({ start: 0, end: 3 });
        ok(got.rows.length === 3, "oversize cells: rows survive the arena");
        eq(got.rows[0][0].length, 70000, "oversize cell round-trips at full length");
        eq(got.rows[2][1], "t2", "a short cell after an oversize one is intact");
    }
    for (const n of [65534, 65535, 65536, 65537]) {
        const f = new CSVFile(new Path(TMP + "_b" + n + ".csv"));
        f.create({ headers: ["A", "B"], rows: [["y".repeat(n), "tail"]], overwrite: true });
        const got = f.readRowRange({ start: 0, end: 1 });
        eq(got.rows[0][0].length, n, "cell of exactly " + n + " bytes round-trips");
        eq(got.rows[0][1], "tail", "the cell after a " + n + "-byte cell is intact");
    }
}
removeAll(TMPDIR);
print("test_csv_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
