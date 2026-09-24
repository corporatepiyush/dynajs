// flags: --std
/* test_csv_parse.js — dyna:csv in-memory parse/stringify with the
 * delimiter/quote options and the strict-options pilot (one
 * unknown-key probe per options bag in the module). window aliases,
 * bare addRow and read-dialect probes live here too (they need a
 * file, so temp dirs are created and removed inline).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_csv_parse.js */
import { parse, stringify, CSVFile } from "dyna:csv";
import { makeTempDir, removeAll, writeFile } from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) { assert(JSON.stringify(a) === JSON.stringify(b), msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, msg) { let t = false; try { fn(); } catch { t = true; } assert(t, msg); }
function throwsMsg(fn, re, msg) { let m = ""; try { fn(); } catch (e) { m = String(e.message); } assert(re.test(m), msg + " (got: " + m + ")"); }

/* ----------------: parse basics ---------------- */
{
    const t = parse("a,b\n1,2\n3,4\n");
    eq(t.headers, ["a", "b"], "parse headers");
    eq(t.rows, [["1", "2"], ["3", "4"]], "parse rows");
    eq(t.totalRows, 2, "parse totalRows");
    /* header-only */
    eq(parse("a,b\n").rows, [], "header-only has no rows");
    eq(parse("a,b").totalRows, 0, "header-only without trailing newline");
    /* empty text is the empty table (documented divergence from read()) */
    eq(parse("", {}).headers, [], "empty text has no headers");
    eq(parse("", {}).totalRows, 0, "empty text has no rows");
    /* quoted fields: embedded delimiter, newline, escaped quotes
     * (3-column header: a wider row truncates to the header width) */
    const q = parse('a,b,c\n"x,y","1\n2","he said ""hi"""\n');
    eq(q.rows, [["x,y", "1\n2", 'he said "hi"']], "quoted delimiter/newline/escaped quotes");
    /* CRLF records */
    eq(parse("a,b\r\n1,2\r\n").rows, [["1", "2"]], "CRLF records");
    /* CR-only records */
    eq(parse("a,b\r1,2\r").rows, [["1", "2"]], "CR-only records");
    /* BOM stripped */
    eq(parse("\uFEFFa,b\n1,2\n").headers, ["a", "b"], "BOM stripped");
    /* ragged rows shaped to the header width (read()'s convention) */
    eq(parse("a,b,c\n1\n2,3,4,5\n").rows, [["1", "", ""], ["2", "3", "4"]], "ragged rows pad/truncate to header width");
    /* hasHeader:false: headers is [] and all rows are data */
    const bare = parse("1,2\n3,4\n", { hasHeader: false });
    eq(bare.headers, [], "hasHeader:false -> headers []");
    eq(bare.rows, [["1", "2"], ["3", "4"]], "hasHeader:false -> all rows are data");
    eq(bare.totalRows, 2, "hasHeader:false -> totalRows counts every row");
    /* strict mode names the text row; tolerant stays the default */
    throwsMsg(() => parse('a,b\n"x"y,1\n', { strict: true }), /csv\.parse: row 2: unexpected text after a closing quote/, "strict garbage names the text row");
    throwsMsg(() => parse('a,b\n"x\n', { strict: true }), /csv\.parse: row 2: unterminated quoted field/, "strict unterminated names the row");
    eq(parse('a,b\n"x"y,1\n').rows, [["x", ""], ["", "1"]], "tolerant default: garbage after a closing quote ends the record");
}

/* ----------------: stringify + roundtrips ---------------- */
{
    eq(stringify([]), "", "empty rows -> empty string");
    eq(stringify([["a", "b"], ["1", "2"]]), "a,b\n1,2\n", "array rows verbatim");
    eq(stringify([["x,y", 'q"q', "nl\nhere"]]), '"x,y","q""q","nl\nhere"\n', "minimal quoting of delimiter/quote/newline");
    /* object form: first-row key insertion order is the column order */
    eq(stringify([{ b: "2", a: "1" }, { a: "3" }]), "b,a\n2,1\n,3\n", "object rows: first-row key order, missing key -> empty");
    eq(stringify([{ a: "1" }], { hasHeader: false }), "1\n", "hasHeader:false suppresses the derived header");
    /* roundtrip identity: string -> parse -> stringify -> parse */
    const src = 'name,meta\n"comma, here","line1\nline2"\n"quote ""q""",plain\n';
    const t1 = parse(src);
    const back = stringify([t1.headers, ...t1.rows]);
    eq(back, src, "roundtrip text identical (quotes/commas/newlines-in-fields)");
    eq(parse(back), t1, "roundtrip data identical");
    /* object form roundtrips through parse */
    eq(parse(stringify([{ a: "1", b: "2" }])).rows, [["1", "2"]], "object form roundtrips");
    /* mixed forms refused */
    throwsMsg(() => stringify([["1"], { a: "2" }]), /row 1: rows must all be arrays/, "object after array rows refused");
    throwsMsg(() => stringify([{ a: "1" }, ["2"]]), /row 1: rows must all be objects/, "array after object rows refused");
    throws(() => stringify("nope"), "rows must be an array");
    throws(() => stringify([42]), "row 0 must be an array or an object");
    throws(() => stringify([[1], 5]), "later non-row refused");
    /* ToString parity (positional null -> "null", matching addRow's array form) */
    eq(stringify([[1, null, true]]), "1,null,true\n", "cells are ToString'd");
}

/* ----------------: delimiter/quote on the free functions ---------------- */
{
    const tsv = parse("a\tb\n1\t2\n", { delimiter: "\t" });
    eq(tsv.headers, ["a", "b"], "TSV parse");
    eq(stringify([tsv.headers, ...tsv.rows], { delimiter: "\t" }), "a\tb\n1\t2\n", "TSV stringify roundtrip");
    eq(parse("a;b\n1;2\n", { delimiter: ";" }).rows, [["1", "2"]], "semicolon parse");
    /* custom quote char */
    eq(parse("a,b\n'x,y',2\n", { quote: "'" }).rows, [["x,y", "2"]], "single-quote dialect parse");
    eq(stringify([["x,y"]], { quote: "'" }), "'x,y'\n", "single-quote dialect stringify");
    /* quoted fields containing the custom delimiter / newlines */
    eq(parse('a;b\n"1;2";"3\n4"\n', { delimiter: ";" }).rows, [["1;2", "3\n4"]], "quoted custom delimiter and newline");
    /* rejections: the canonical roles are reserved */
    throwsMsg(() => parse("x", { delimiter: '"' }), /'delimiter' must not be the quote character/, "delimiter=quote-char refused");
    throwsMsg(() => parse("x", { quote: "," }), /'quote' must not be the delimiter character/, "quote=delimiter-char refused");
    throwsMsg(() => parse("x", { delimiter: '"', quote: "," }), /'delimiter' must not be the quote character/, "swapped pair refused");
    throwsMsg(() => parse("x", { delimiter: "::" }), /'delimiter' must be exactly one ASCII character/, "multi-char delimiter refused");
    throwsMsg(() => parse("x", { quote: "§" }), /'quote' must be exactly one ASCII character/, "non-ASCII quote refused");
    throwsMsg(() => parse("x", { delimiter: "\n" }), /'delimiter' must not be CR or LF/, "newline delimiter refused");
    throwsMsg(() => parse("x", { quote: "\r" }), /'quote' must not be CR or LF/, "CR quote refused");
    throwsMsg(() => parse("x", { delimiter: ";", quote: ";" }), /'delimiter' and 'quote' must differ/, "identical pair refused");
    throwsMsg(() => parse("x", { delimiter: 44 }), /'delimiter' must be exactly one ASCII character/, "numeric delimiter refused");
}

/* ----------------: one unknown-key probe per options bag ---------------- */
{
    throwsMsg(() => parse("a,b\n", { delim: "," }), /csv\.parse: unknown option "delim" \(valid: delimiter, quote, hasHeader, strict\)/, "parse bag is strict");
    throwsMsg(() => stringify([["1"]], { sep: "," }), /csv\.stringify: unknown option "sep" \(valid: delimiter, quote, hasHeader\)/, "stringify bag is strict");
    throwsMsg(() => stringify([["1"]], {}), /^$/, "empty bag accepted");
    const dir = makeTempDir("dyna-csv-parse-test-");
    try {
        const P = (nm) => dir.join(nm);
        const f = new CSVFile(P("s.csv"));
        f.create({ headers: ["a", "b"], rows: [["1", "2"]] });
        throwsMsg(() => f.read({ col: ["a"] }), /csv\.read: unknown option "col" \(valid: offset, limit, columns, strict, delimiter, quote\)/, "read bag is strict");
        throwsMsg(() => f.addRow({ rowz: [["1"]] }), /csv\.addRow: unknown option "rowz" \(valid: rows, strict, durable\)/, "addRow bag is strict");
        throwsMsg(() => f.updateCell({ row: 0, columnIndex: 0, value: "x", val: "y" }), /csv\.updateCell: unknown option "val"/, "updateCell bag is strict");
        throwsMsg(() => f.removeRow({ row: 0, hard: true }), /csv\.removeRow: unknown option "hard"/, "removeRow bag is strict");
        throwsMsg(() => f.addColumn({ column: "c", def: "" }), /csv\.addColumn: unknown option "def"/, "addColumn bag is strict");
        throwsMsg(() => f.removeColumn({ columnIndex: 0, col: "c" }), /csv\.removeColumn: unknown option "col"/, "removeColumn bag is strict");
        throwsMsg(() => f.renameColumn({ oldName: "a", newName: "z", quiet: true }), /csv\.renameColumn: unknown option "quiet"/, "renameColumn bag is strict");
        throwsMsg(() => f.readColumnValuesRange({ column: "a", starte: 0 }), /csv\.readColumnValuesRange: unknown option "starte"/, "readColumnValuesRange bag is strict");
        throwsMsg(() => f.readRowRange({ start: 0, finish: 1 }), /csv\.readRowRange: unknown option "finish"/, "readRowRange bag is strict");
        throwsMsg(() => f.selectColumnRange({ columns: ["a"], strt: 0 }), /csv\.selectColumnRange: unknown option "strt"/, "selectColumnRange bag is strict");
        throwsMsg(() => f.create({ headers: ["z"], head: [] }), /csv\.create: unknown option "head" \(valid: headers, rows, overwrite\)/, "create bag is strict");
        /* no false positives: every documented key still accepted */
        f.create({ headers: ["a", "b"], rows: [["1", "2"]], overwrite: true });
        eq(f.read({ offset: 0, limit: 1, columns: ["a"], strict: false }).totalRows, 1, "read valid keys accepted");
        eq(f.addRow({ rows: [["3", "4"]], strict: false, durable: false }).added, 1, "addRow valid keys accepted");
        f.updateCell({ row: 0, columnIndex: 0, value: "x", strict: false, durable: false });
        f.removeRow({ row: 0, strict: false, durable: false });
        f.addColumn({ column: "c", defaultValue: "-", strict: false, durable: false });
        f.removeColumn({ column: "c", strict: false, durable: false });
        f.renameColumn({ oldName: "b", newName: "B", strict: false, durable: false });
        f.readColumnValuesRange({ column: "a", start: 0, end: 1, maxRows: 10, strict: false });
        f.readRowRange({ offset: 0, limit: 1, maxRows: 10, strict: false });
        f.selectColumnRange({ columns: ["a"], offset: 0, limit: 1, maxRows: 10, strict: false });
        f.close();
    } finally {
        removeAll(dir);
    }
}

/* ----------------: window aliases + maxRows ---------------- */
{
    const dir = makeTempDir("dyna-csv-parse-w-");
    try {
        const rows = [];
        for (let i = 0; i < 30; i++) rows.push([String(i), "v" + i]);
        const f = new CSVFile(dir.join("w.csv"));
        f.create({ headers: ["id", "v"], rows });
        /* the alias names the same window */
        eq(f.readRowRange({ start: 5, end: 8 }).rows, f.readRowRange({ offset: 5, limit: 3 }).rows, "readRowRange {offset,limit} == {start,end}");
        eq(f.readColumnValuesRange({ column: "id", start: 2, end: 6 }), f.readColumnValuesRange({ column: "id", offset: 2, limit: 4 }), "readColumnValuesRange alias window");
        eq(f.selectColumnRange({ columns: ["v"], start: 1, end: 4 }).rows, f.selectColumnRange({ columns: ["v"], offset: 1, limit: 3 }).rows, "selectColumnRange alias window");
        /* defaults unchanged */
        eq(f.readRowRange().rows, [["0", "v0"]], "no-arg readRowRange still means row 0");
        eq(f.readRowRange({ offset: 7 }).rows, [["7", "v7"]], "offset-only readRowRange still means one row");
        eq(f.readColumnValuesRange({ column: "id", offset: 28 }).length, 2, "limit omitted = to the end");
        /* mixing the two forms is refused */
        throwsMsg(() => f.readRowRange({ start: 0, limit: 2 }), /use either \{start, end\} or \{offset, limit\} -- not both/, "mixed forms refused (readRowRange)");
        throwsMsg(() => f.readColumnValuesRange({ column: "id", offset: 0, end: 2 }), /not both/, "mixed forms refused (readColumnValuesRange)");
        throwsMsg(() => f.selectColumnRange({ columns: ["id"], start: 0, limit: 2 }), /not both/, "mixed forms refused (selectColumnRange)");
        /* the hidden caps: documented, still enforced, now overridable */
        throwsMsg(() => f.readColumnValuesRange({ column: "id", start: 0, end: 1001 }), /exceeds the maximum of 1000 rows/, "default 1000 cap enforced");
        eq(f.readColumnValuesRange({ column: "id", start: 0, end: 1001, maxRows: 1001 }).length, 30, "maxRows raises the 1000 cap");
        throwsMsg(() => f.readRowRange({ start: 0, end: 101 }), /exceeds the maximum of 100 rows/, "default 100 cap (readRowRange)");
        throwsMsg(() => f.selectColumnRange({ columns: ["id"], start: 0, end: 101 }), /exceeds the maximum of 100 rows/, "default 100 cap (selectColumnRange)");
        eq(f.readRowRange({ offset: 0, limit: 101, maxRows: 101 }).rows.length, 30, "maxRows raises the 100 cap");
        throwsMsg(() => f.readColumnValuesRange({ column: "id", start: 0, end: 4, maxRows: -1 }), /maxRows must not be negative/, "negative maxRows refused");
        throwsMsg(() => f.readColumnValuesRange({ column: "id", start: 0, end: 4, maxRows: 2 }), /exceeds the maximum of 2 rows/, "maxRows can tighten the cap (refuses the wider window)");
        eq(f.readColumnValuesRange({ column: "id", start: 0, end: 4, maxRows: 5 }).length, 4, "maxRows above the window keeps it allowed");
        /* limit semantics */
        eq(f.readRowRange({ offset: 29, limit: 100 }).rows.length, 1, "limit clamps at EOF");
        eq(f.readRowRange({ offset: 1e10 }).rows, [], "huge offset clamps to an empty window, no crash");
        throwsMsg(() => f.readRowRange({ offset: 0, limit: 4611686018427387904 }), /exceeds the maximum of 100 rows/, "limit 2^62 is an explicit window: refused, no overflow");
        eq(f.readRowRange({ offset: -100 }).rows.length, 1, "negative offset clamps to 0");
        eq(f.readRowRange({ offset: 1, limit: 0 }).rows.length, 0, "limit 0 is an empty window");
        eq(f.selectColumnRange({ columns: ["id"], offset: 1, limit: -1 }).rows.length, 29, "limit -1 on selectColumnRange = to the end");
        f.close();
    } finally {
        removeAll(dir);
    }
}

/* ---------------- adversarial edges (LOOP 2 probes, kept) ---------------- */
{
    /* a 1 MiB quoted field survives parse and serializes back unquoted */
    const big = "x".repeat(1024 * 1024);
    const t = parse('a,b\n"' + big + '",1\n');
    eq(t.rows[0][0].length, 1048576, "1 MiB quoted field survives parse");
    eq(stringify(t.rows, { hasHeader: false }), big + ",1\n", "1 MiB field roundtrips");
    /* a huge limit is an explicit window: refused by the cap, no overflow */
    throwsMsg(() => stringify([], { delimiter: "\t".repeat(2) }), /exactly one ASCII character/, "multi-char stringify delimiter refused");
    throws(() => parse("x", new Proxy({}, { ownKeys() { throw new Error("trap"); } })), "a throwing ownKeys trap on the bag propagates");
    const weird = {};
    Object.defineProperty(weird, "__proto__", { value: 1, enumerable: true });
    throwsMsg(() => parse("x", weird), /unknown option "__proto__"/, "own __proto__ key rejected");
    throwsMsg(() => parse("x", ["delimiter"]), /unknown option "0"/, "an array in the bag position is checked by key");
    /* symbol-keyed options are ignored (string-key grammar) */
    eq(parse("a\n1", { [Symbol("s")]: 1 }).totalRows, 1, "symbol keys do not trip opts_check");
}

/* ---------------- on read + bare addRow ---------------- */
{
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
        /*the bare positional row */
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
}

print("test_csv_parse: all tests passed (" + n + " assertions)");
