/* test_csv.js — dyna:csv CSVFile class (file CRUD, RFC-4180, SIMD parse + atomic I/O).
 * Hermetic: everything happens inside a temp dir that is removed at the end.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_csv.js */
import { CSVFile } from "dyna:csv";
import { makeTempDir, removeAll, exists } from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) { assert(JSON.stringify(a) === JSON.stringify(b), msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, msg) { let t = false; try { fn(); } catch { t = true; } assert(t, msg); }

const dir = makeTempDir("dyna-csv-test-");
/* makeTempDir returns a Path, so a child is one .join away -- no string
 * concatenation and no re-parse per call. */
const P = (name) => dir.join(name);

try {
    /* ---------------- create ---------------- */
    {
        const p = P("a.csv");
        const f = new CSVFile(p);
        const res = f.create({ headers: ["Name", "Age", "City"], rows: [["Alice", "30", "NYC"], ["Bob", "25", "LA"]] });
        eq(res.rows, 2, "create returns row count");
        assert(exists(p), "file created");
        const r = f.read();
        eq(r.headers, ["Name", "Age", "City"], "create headers");
        eq(r.rows, [["Alice", "30", "NYC"], ["Bob", "25", "LA"]], "create rows");
        eq(r.totalRows, 2, "create totalRows");
    }
    /* headers-only + fail-if-exists + overwrite */
    {
        const p = P("empty.csv");
        const f = new CSVFile(p);
        f.create({ headers: ["X", "Y"] });
        eq(f.read().totalRows, 0, "headers-only file has 0 rows");
        throws(() => f.create({ headers: ["X"] }), "create fails when file exists");
        f.create({ headers: ["Z"], overwrite: true });
        eq(f.read().headers, ["Z"], "overwrite replaces the file");
        throws(() => new CSVFile(P("bad.csv")).create({ headers: ["A", "B"], rows: [["only-one"]] }), "create rejects a wrong-width row");
        throws(() => new CSVFile(P("noheaders.csv")).create({ headers: [] }), "create requires >= 1 header");
        /* parent dirs auto-created */
        new CSVFile(P("nested/deep/x.csv")).create({ headers: ["a"] });
        assert(exists(P("nested/deep/x.csv")), "parent dirs auto-created");
    }

    /* ---------------- read: pagination + column filter ---------------- */
    {
        const f = new CSVFile(P("page.csv"));
        const rows = [];
        for (let i = 0; i < 100; i++) rows.push(["id" + i, "v" + i, String(i)]);
        f.create({ headers: ["ID", "Val", "N"], rows });
        eq(f.read({ offset: 0, limit: 10 }).rows.length, 10, "limit=10 → 10 rows");
        eq(f.read({ offset: 95, limit: 10 }).rows.length, 5, "offset near end clamps");
        eq(f.read({ offset: 10, limit: 1 }).rows[0], ["id10", "v10", "10"], "offset picks the right row");
        eq(f.read().totalRows, 100, "totalRows ignores pagination");
        const sel = f.read({ columns: ["N", "ID"], offset: 0, limit: 2 });
        eq(sel.headers, ["N", "ID"], "column filter reorders headers");
        eq(sel.rows[0], ["0", "id0"], "column filter reorders + selects cells");
        throws(() => f.read({ columns: ["Nope"] }), "read rejects an unknown column");
    }

    /* ---------------- addRow: positional + named ---------------- */
    {
        const f = new CSVFile(P("add.csv"));
        f.create({ headers: ["Name", "Age"] });
        let res = f.addRow({ rows: [["Alice", "30"], ["Bob", "25"]] });
        eq(res.added, 2, "addRow positional added count");
        eq(res.totalRows, 2, "addRow totalRows");
        res = f.addRow({ rows: [{ Name: "Carol", Age: "40" }, { Age: "22", Name: "Dave" }] });
        eq(res.totalRows, 4, "addRow named grows total");
        /* named form: missing key → empty, extra key ignored */
        f.addRow({ rows: [{ Name: "Eve", Bogus: "x" }] });
        const r = f.read();
        eq(r.rows[2], ["Carol", "40"], "named row maps by header");
        eq(r.rows[3], ["Dave", "22"], "named row order-independent");
        eq(r.rows[4], ["Eve", ""], "named row: missing key → empty, extra key ignored");
    }

    /* ---------------- updateCell ---------------- */
    {
        const f = new CSVFile(P("upd.csv"));
        f.create({ headers: ["A", "B", "C"], rows: [["1", "2", "3"], ["4", "5", "6"]] });
        f.updateCell({ row: 0, column: "B", value: "99" });
        f.updateCell({ row: 1, columnIndex: 2, value: "60" });
        f.updateCell({ row: 0, column: "A", value: "" });   /* clear a cell */
        const r = f.read();
        eq(r.rows[0], ["", "99", "3"], "updateCell by name + clear");
        eq(r.rows[1], ["4", "5", "60"], "updateCell by columnIndex");
        throws(() => f.updateCell({ row: 9, column: "A", value: "x" }), "updateCell rejects OOB row");
        throws(() => f.updateCell({ row: 0, column: "Z", value: "x" }), "updateCell rejects bad column");
    }

    /* ---------------- removeRow ---------------- */
    {
        const f = new CSVFile(P("rm.csv"));
        f.create({ headers: ["V"], rows: [["a"], ["b"], ["c"], ["d"]] });
        const res = f.removeRow({ row: 1 });   /* remove "b" */
        eq(res.totalRows, 3, "removeRow new total");
        eq(f.read().rows, [["a"], ["c"], ["d"]], "removeRow shifts up");
        throws(() => f.removeRow({ row: 99 }), "removeRow rejects OOB");
    }

    /* ---------------- addColumn / removeColumn / renameColumn ---------------- */
    {
        const f = new CSVFile(P("cols.csv"));
        f.create({ headers: ["A", "B"], rows: [["1", "2"], ["3", "4"]] });
        f.addColumn({ column: "C", defaultValue: "z" });
        eq(f.read().rows[0], ["1", "2", "z"], "addColumn with default");
        f.addColumn({ column: "D" });   /* empty default */
        eq(f.read().rows[1], ["3", "4", "z", ""], "addColumn empty default");
        throws(() => f.addColumn({ column: "A" }), "addColumn rejects a duplicate");

        f.renameColumn({ oldName: "B", newName: "Beta" });
        eq(f.read().headers, ["A", "Beta", "C", "D"], "renameColumn");
        f.renameColumn({ oldName: "A", newName: "A" });   /* no-op */
        throws(() => f.renameColumn({ oldName: "A", newName: "C" }), "renameColumn rejects an existing name");
        throws(() => f.renameColumn({ oldName: "Nope", newName: "X" }), "renameColumn rejects a missing column");

        f.removeColumn({ column: "C" });
        eq(f.read().headers, ["A", "Beta", "D"], "removeColumn by name compacts");
        f.removeColumn({ columnIndex: 0 });
        eq(f.read().headers, ["Beta", "D"], "removeColumn by index");
        eq(f.read().rows[0], ["2", ""], "removeColumn drops the right cells");
    }

    /* ---------------- range readers ---------------- */
    {
        const f = new CSVFile(P("range.csv"));
        const rows = [];
        for (let i = 0; i < 50; i++) rows.push([String(i), "name" + i, "e" + i + "@x.com"]);
        f.create({ headers: ["ID", "Name", "Email"], rows });

        eq(f.readColumnValuesRange({ column: "Email", start: 0, end: 3 }), ["e0@x.com", "e1@x.com", "e2@x.com"], "readColumnValuesRange");
        eq(f.readColumnValuesRange({ column: "ID" }).length, 50, "readColumnValuesRange all");
        throws(() => f.readColumnValuesRange({ column: "ID", start: 0, end: 2000 }), "readColumnValuesRange caps at 1000");

        const rr = f.readRowRange();   /* default: single row 0 */
        eq(rr.rows.length, 1, "readRowRange default is one row");
        eq(rr.rows[0], ["0", "name0", "e0@x.com"], "readRowRange row 0");
        eq(f.readRowRange({ start: 5, end: 8 }).rows.length, 3, "readRowRange window");
        throws(() => f.readRowRange({ start: 0, end: 200 }), "readRowRange caps at 100");

        const sc = f.selectColumnRange({ columns: ["Email", "ID"], start: 0, end: 2 });
        eq(sc.columns, ["Email", "ID"], "selectColumnRange columns");
        eq(sc.rows[0], ["e0@x.com", "0"], "selectColumnRange projects + reorders");
        throws(() => f.selectColumnRange({ columns: ["Nope"] }), "selectColumnRange rejects bad column");
        throws(() => f.selectColumnRange({ columns: [] }), "selectColumnRange requires columns");
    }

    /* ---------------- RFC 4180 quoting round-trips ---------------- */
    {
        const f = new CSVFile(P("quote.csv"));
        const tricky = [
            ["a,b", "plain", "x"],                       /* embedded comma */
            ['he said "hi"', "y", "z"],                  /* embedded quotes */
            ["line1\nline2", "multi", "w"],              /* embedded newline */
            ['"leading quote', "trailing\r", "  spaced  "],
            ["", "empty-first", ""],                     /* empty fields */
        ];
        f.create({ headers: ["Weird", "B", "C"], rows: tricky });
        const r = f.read();
        eq(r.rows, tricky, "RFC-4180 quoting round-trips (comma/quote/newline/empty)");
        /* mutate a quoted table + reread — quoting must survive edits */
        f.updateCell({ row: 0, column: "B", value: 'now, with "quotes"' });
        eq(f.read().rows[0], ["a,b", 'now, with "quotes"', "x"], "quoting survives updateCell");
    }

    /* ---------------- reentrant-close safety (CLAUDE.md §5 rule) ---------------- */
    {
        /* A method must copy its path BEFORE coercing arguments; a reentrant
         * valueOf that close()s the instance mid-coercion must not use-after-free
         * (ASan/UBSan is the real check — this exercises the path). */
        const f = new CSVFile(P("reenter.csv"));
        f.create({ headers: ["A", "B"], rows: [["1", "2"]] });
        try { f.updateCell({ row: { valueOf() { f.close(); return 0; } }, column: "A", value: "x" }); } catch {}
        assert(true, "reentrant-close updateCell did not crash");
        /* the instance is now closed: further use throws, not crashes */
        throws(() => f.read(), "a closed CSVFile throws on reuse");
    }

    /* ---------------- errors ---------------- */
    {
        throws(() => new CSVFile(P("does-not-exist.csv")).read(), "read of a missing file throws");
        throws(() => new CSVFile(), "constructing without a path throws");
        throws(() => new CSVFile(P("x.csv")).create({}), "create without headers throws");
    }

    print("test_csv: all tests passed (" + n + " assertions)");
} finally {
    removeAll(dir);
}
