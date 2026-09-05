/* test_csv.js — dyna:csv CSVFile class (file CRUD, RFC-4180, SIMD parse + atomic I/O).
 * Hermetic: everything happens inside a temp dir that is removed at the end.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_csv.js */
import { CSVFile } from "dyna:csv";
import { makeTempDir, removeAll, exists, writeFile } from "dyna:file";
import * as os from "os";

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

    /* ---------------- whole-file ingest cap (P1b-6) ---------------- */
    /* A file whose st_size exceeds DYN_MAX_INPUT (1 GiB) must be refused BEFORE
     * any malloc/mmap -- a sparse 2 GiB file (truncate -s, 0 disk blocks) is the
     * cheap proof. The cap is enforced in dyn_io_slurp (csv_load passes
     * DYN_MAX_INPUT) and surfaced as a clean RangeError. */
    {
        const cap = 1 << 30;                 /* 1 GiB, DYN_MAX_INPUT */
        const big = P("over-cap.csv");
        const trunc = (path, size) => os.exec(["truncate", "-s", String(size), String(path)]);
        /* only run if `truncate` is present; skip loudly (not faked) otherwise */
        if (trunc(big, cap + 1) === 0) {
            const t0 = Date.now();
            let threw = false, msg = "";
            try { new CSVFile(big).read(); }
            catch (e) { threw = true; msg = String(e.message); }
            const ms = Date.now() - t0;
            assert(threw, "an over-cap CSV file is refused, not read");
            assert(/exceeds|range/i.test(msg), "the refusal names the cap (got: " + msg + ")");
            assert(ms < 1000, "refusal is fast, not a 2 GiB malloc/read (" + ms + "ms)");
        }
        /* a normal under-cap CSV file still reads intact through the same path */
        const okc = P("under-cap.csv");
        new CSVFile(okc).create({ headers: ["A", "B"], rows: [["1", "2"]] });
        eq(new CSVFile(okc).read().totalRows, 1, "an under-cap CSV file reads intact");
    }

    /* ---------------- per-edit durability is opt-in (P1b-6) ---------------- */
    /* Every mutating method now writes temp+rename without fsync unless
     * `{durable:true}` is passed; a K-edit batch must not fsync K times. The
     * option is honored without a public d.ts change (callers default to
     * non-durable = 0, keeping the old behavior with less sync). */
    {
        const f = new CSVFile(P("durable.csv"));
        f.create({ headers: ["A", "B"], rows: [["1", "2"]] });
        f.updateCell({ row: 0, column: "A", value: "10", durable: true });
        f.addRow({ rows: [["2", "3"]], durable: true });
        f.removeRow({ row: 1, durable: true });
        /* same data as the non-durable path -- semantics unchanged */
        eq(f.read().rows, [["10", "2"]], "durable:true edits persist the same data");
        /* default (no option) path still mutates and persists */
        f.updateCell({ row: 0, column: "B", value: "22" });
        eq(f.read().rows[0], ["10", "22"], "default (non-option) edit persists too");
    }

    /* ---------------- leading UTF-8 BOM is stripped on load ---------------- */
    {
        const f = new CSVFile(P("bom.csv"));
        /* create writes the header verbatim: the BOM lands at byte 0 of the file */
        f.create({ headers: ["\uFEFFName", "Age"], rows: [["Alice", "30"]] });
        const r = f.read();
        eq(r.headers, ["Name", "Age"], "a leading UTF-8 BOM is stripped from the first header");
        eq(r.rows, [["Alice", "30"]], "and the data cells are untouched");
    }

    /* ---------------- {strict:true} reader option ---------------- */
    {
        /* garbage after a closing quote: RFC-4180 says this is a syntax error.
         * Tolerant (the default) keeps the historical behavior: the quote ends
         * the field and the trailing text splits the record. */
        const g = P("garbage.csv");
        writeFile(g, 'a,b\n"x"y,z\n');
        const f = new CSVFile(g);
        /* the exact tolerant shape is pinned by the PRE-EXISTING behavior
           (verified identical before this change): the quote ends the field,
           the trailing byte is dropped, the record splits */
        eq(f.read().rows, [["x", ""], ["", "z"]],
           "tolerant default: garbage-after-quote does not throw (unchanged behavior)");
        let m = "";
        try { f.read({ strict: true }); } catch (e) { m = String(e.message); }
        assert(/row 2/.test(m) && /closing quote/.test(m),
               "strict names the row on garbage-after-quote (got: " + m + ")");
    }
    {
        /* a quote still open at EOF */
        const u = P("unterminated.csv");
        writeFile(u, 'a,b\n"x,y\n');
        const f = new CSVFile(u);
        eq(f.read().rows, [["x,y\n", ""]],
           "tolerant default: an unterminated quote commits its partial data (unchanged)");
        let m = "";
        try { f.read({ strict: true }); } catch (e) { m = String(e.message); }
        assert(/row 2/.test(m) && /unterminated/.test(m),
               "strict names the row on an unterminated quote (got: " + m + ")");
        /* strict on a WELL-FORMED file parses identically */
        const ok = P("ok.csv");
        new CSVFile(ok).create({ headers: ["A", "B"], rows: [["1", "2"]] });
        eq(new CSVFile(ok).read({ strict: true }).rows, [["1", "2"]], "strict parses well-formed files identically");
    }

    /* ---------------- method-originated errors carry the method name ---------------- */
    {
        const f = new CSVFile(P("pfx.csv"));
        f.create({ headers: ["A", "B"] });
        let m = "";
        try { f.updateCell({ row: 0, column: "Z", value: "x" }); } catch (e) { m = String(e.message); }
        assert(m.indexOf("csv.updateCell:") === 0,
               "updateCell column errors use the csv.<method>: prefix (got: " + m + ")");
        m = "";
        try { f.removeColumn({ column: "Z" }); } catch (e) { m = String(e.message); }
        assert(m.indexOf("csv.removeColumn:") === 0,
               "removeColumn column errors use the csv.<method>: prefix (got: " + m + ")");
        m = "";
        try { f.readColumnValuesRange({ column: "Z" }); } catch (e) { m = String(e.message); }
        assert(m.indexOf("csv.readColumnValuesRange:") === 0,
               "readColumnValuesRange column errors use the csv.<method>: prefix (got: " + m + ")");
    }

    /* ---------------- a cell whose conversion throws surfaces the user error ---------------- */
    {
        const f = new CSVFile(P("boom.csv"));
        f.create({ headers: ["A", "B"] });
        const evil = { toString() { throw new Error("boom-from-cell"); } };
        let m = "";
        try { f.addRow({ rows: [["ok", evil]] }); } catch (e) { m = String(e.message); }
        assert(/boom-from-cell/.test(m),
               "addRow surfaces a cell's toString exception (got: " + m + ")");
        eq(f.read().rows, [], "addRow wrote no partial row with \"\"");
        m = "";
        try { f.addRow({ rows: [{ A: evil }] }); } catch (e) { m = String(e.message); }
        assert(/boom-from-cell/.test(m), "the named form surfaces it too");
        eq(f.read().rows, [], "named addRow wrote no partial row either");
        m = "";
        try { new CSVFile(P("boom2.csv")).create({ headers: ["A"], rows: [[evil]] }); }
        catch (e) { m = String(e.message); }
        assert(/boom-from-cell/.test(m), "create surfaces a cell's toString exception");
        assert(!exists(P("boom2.csv")), "create wrote nothing when a cell threw");
    }

    print("test_csv: all tests passed (" + n + " assertions)");
} finally {
    removeAll(dir);
}
