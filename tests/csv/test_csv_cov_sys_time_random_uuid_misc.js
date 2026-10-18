// Split from tests/test_cov_sys_time_random_uuid_misc.js: the csv sections.
import { CSVFile } from "dyna:csv";
import { Path, makeTempDir, removeAll } from "dyna:file";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
print("=== csv ===");
{
    const dir = makeTempDir("covcsv-");
    const p = new Path(dir.toString() + "/t.csv");
    const cf = new CSVFile(p);
    const created = cf.create({ headers: ["a", "b"], rows: [[1, "x"], [2, "y"]] });
    eq(created.rows, 2, "create rows count");
    throws(() => cf.create({ headers: ["z"], rows: [] }), /./, "create again w/o overwrite throws");
    let rd = cf.read();
    eq(rd.headers.join(","), "a,b", "read headers");
    eq(rd.rows.length, 2, "read rows");
    eq(rd.totalRows, 2, "read totalRows");
    rd = cf.read({ limit: 1 });
    eq(rd.rows.length, 1, "limit 1");
    rd = cf.read({ offset: 1 });
    eq(rd.rows.length, 1, "offset 1 leaves 1");
    rd = cf.read({ offset: 5 });
    eq(rd.rows.length, 0, "offset past end empty");
    rd = cf.read({ columns: ["b"] });
    eq(rd.headers.join(","), "b", "column projection");
    let ar = cf.addRow({ rows: [{ a: 3, b: "z" }] });
    eq(ar.added, 1, "object addRow added");
    ar = cf.addRow({ rows: [[4, "w"]] });
    eq(ar.added, 1, "positional addRow added");
    eq(cf.read().totalRows, 4, "totalRows after adds");
    let uc = cf.updateCell({ row: 0, column: "b", value: "X" });
    eq(uc.value, "X", "updateCell by name");
    uc = cf.updateCell({ row: 1, columnIndex: 0, value: "9" });
    eq(uc.value, "9", "updateCell by index");
    throws(() => cf.updateCell({ row: 999, column: "a", value: "1" }), /./, "updateCell OOB row throws");
    const colVals = cf.readColumnValuesRange({ column: "a", start: 0, end: 2 });
    eq(colVals.length, 2, "readColumnValuesRange window");
    throws(() => cf.readColumnValuesRange({ column: "a", start: 0, end: 5000 }), /./, "range >1000 refused");
    eq(cf.readRowRange({ start: 0, end: 2 }).rows.length, 2, "readRowRange");
    throws(() => cf.selectColumnRange({ columns: ["a"], start: 0, end: 101 }), /./, "selectColumnRange cap");
    const ac = cf.addColumn({ column: "c", defaultValue: "-" });
    eq(ac.column, "c", "addColumn name");
    eq(ac.totalColumns, 3, "totalColumns after add");
    const rn = cf.renameColumn({ oldName: "c", newName: "cee" });
    eq(rn.newName, "cee", "renameColumn");
    const rc = cf.removeColumn({ column: "cee" });
    eq(rc.removedIndex, 2, "removeColumn index");
    const rr = cf.removeRow({ row: 0 });
    eq(rr.removed, 0, "removeRow echoes removed index");
    eq(rr.totalRows, 3, "removeRow totalRows left");
    throws(() => cf.removeRow({ row: 999 }), /./, "removeRow OOB throws");
    cf.close();
    cf.close(); n++;
    throws(() => cf.read(), /./, "closed CSVFile read throws");
    removeAll(dir);
}
if (fails) {
    print("test_csv_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_csv_cov_sys_time_random_uuid_misc failed");
}
print("test_csv_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
