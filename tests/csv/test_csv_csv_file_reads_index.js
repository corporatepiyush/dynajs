// covers: integration dyna:csv.CSVFile.read dyna:csv.CSVFile.readColumnValuesRange dyna:csv.CSVFile.readRowRange dyna:csv.CSVFile.selectColumnRange dyna:csv.CSVFile.rows -- the five readers share one structural-index load; checked against parse()
// M1 step 4: CSVFile's readers (read, readColumnValuesRange, readRowRange,
// selectColumnRange, rows) used to parse the whole file into a malloc'd
// string-per-cell Table on every call (34 MB: ~104 ms for a 10-row window,
// ~384 ms for read()). They now build the structural index over the file
// bytes and cut strings only for the cells they return (same file: ~9 ms and
// ~205 ms). Irregular quoting is canonicalised through the scalar scanner,
// so strict errors keep their "csv: row N: ..." text. The oracle is parse()
// over the same text: a reader row is the parse() row padded with "" or cut
// to the header's width. A cell holding a NUL byte used to be truncated at
// the NUL (the Table kept C strings); it now matches parse() (red on the
// Table version).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_csv_file_reads_index");
import { CSVFile, parse } from "dyna:csv";
import { makeTempDir, removeAll, writeFile, Path } from "dyna:file";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }
const J = JSON.stringify;
const dir = makeTempDir("dyna-csv-reads-");
const p = new Path(dir + "/t.csv");

function oracle(text, opts) {
    try {
        const r = parse(text, opts);
        return { headers: r.headers, rows: r.rows.map((row) => r.headers.map((_, c) => row[c] === undefined ? "" : row[c])) };
    } catch (e) {
        return { err: e.name + ": " + e.message.replace(/^csv\.parse:/, "csv:") };
    }
}

function outcome(fn) {
    try { return fn(); } catch (e) { return { err: e.name + ": " + e.message.replace(/'[^']*'/, "P") }; }
}

async function batches(f, opts) {
    try {
        const out = [];
        for await (const b of f.rows(opts)) out.push(b);
        return out;
    } catch (e) { return { err: e.name + ": " + e.message }; }
}

async function compare(text, tag) {
    writeFile(p, text);
    const f = new CSVFile(p);
    const o = oracle(text, {}), os = oracle(text, { strict: true });
    const empty = !o.err && o.headers.length === 0;
    const sameErr = (got, want) => got && got.err === want.err;
    if (empty) {
        const r = outcome(() => f.read());
        check(r.err === "TypeError: csv: P is empty (no header)", tag + " empty: " + J(r));
        return;
    }
    const total = o.err ? 0 : o.rows.length;
    const r = outcome(() => f.read());
    if (o.err) check(sameErr(r, o), tag + " read error " + J(r) + " vs " + J(o));
    else check(J(r) === J({ headers: o.headers, rows: o.rows, totalRows: total }), tag + " read " + J(r) + " vs " + J(o));
    const rs = outcome(() => f.read({ strict: true }));
    if (os.err) check(sameErr(rs, os), tag + " strict read error " + J(rs) + " vs " + J(os));
    else check(J(rs) === J({ headers: os.headers, rows: os.rows, totalRows: os.rows.length }), tag + " strict read");
    if (o.err) return;
    const ol = outcome(() => f.read({ offset: 1, limit: 2 }));
    check(J(ol.rows) === J(o.rows.slice(1, 3)), tag + " read offset/limit " + J(ol));
    const h = o.headers;
    const pick = [h[h.length - 1], h[0]];
    const rc = outcome(() => f.read({ columns: pick }));
    const ix = pick.map((name) => h.indexOf(name));
    check(J(rc) === J({ headers: pick, rows: o.rows.map((row) => ix.map((c) => row[c])), totalRows: total }), tag + " read columns " + J(rc));
    const cv = outcome(() => f.readColumnValuesRange({ column: h[0], start: 0, end: 5 }));
    check(J(cv) === J(o.rows.slice(0, 5).map((row) => row[0])), tag + " readColumnValuesRange " + J(cv));
    const rr = outcome(() => f.readRowRange({ start: 1, end: 6 }));
    check(J(rr) === J({ headers: h, rows: o.rows.slice(1, 6) }), tag + " readRowRange " + J(rr));
    const sc = outcome(() => f.selectColumnRange({ columns: pick, start: 0, end: 4 }));
    check(J(sc) === J({ columns: pick, rows: o.rows.slice(0, 4).map((row) => ix.map((c) => row[c])) }), tag + " selectColumnRange " + J(sc));
    const bs = await batches(f, { batch: 2 });
    const want = [];
    for (let i = 0; i < total; i += 2) want.push({ headers: h, rows: o.rows.slice(i, i + 2), offset: i, totalRows: total });
    check(J(bs) === J(want), tag + " rows() " + J(bs) + " vs " + J(want));
}

const fixed = [
    "a\n", "a,b\n1,2\n", "﻿a,b\n1,2\n", "a,b\n1\n1,2,3\n", "a,b\n\n1,2\n", "\"a\nb\",c\n\"x\"\"y\",z\n",
    "a,b\n\"unterminated\n", "a,b\n\"x\"y,2\n", "a,a,b\n1,2,3\n", "\"\",b\n1,2\n", "a,b\r\n1,2\r\n", "a,b\r1,2\r",
    ",\n,\n", "a\n\n\n", "a,b\n1\u00002,3\n\u0000,x\n", "\"h,1\",\"h\"\"2\"\n\"v,1\",\"v\"\"2\"\n", "a,b\n1,2",
];
for (let i = 0; i < fixed.length; i++) await compare(fixed[i], "fixed[" + i + "]");

let seed = 20261114;
const rnd = (k) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % k; };
const atoms = ["a", "b", "", ",", "\n", "\r\n", "\r", "\"", "\"\"", "x y", "é", "\"q,\"", "1.5", " ", "\"a\"b", "h", "\u0000"];
for (let i = 0; i < 300; i++) {
    let t = ["a,b", "a,b,c", "\"a\",b", "a"][rnd(4)] + ["\n", "\r\n", "\r"][rnd(3)];
    const k = rnd(30);
    for (let j = 0; j < k; j++) t += atoms[rnd(atoms.length)];
    await compare(t, "fuzz[" + i + "]");
}

writeFile(p, "");
check(outcome(() => new CSVFile(p).read()).err === "TypeError: csv: P is empty (no header)", "an empty file has no header");
check(outcome(() => new CSVFile(p).readRowRange()).err === "TypeError: csv: P is empty (no header)", "readRowRange on an empty file");
check(outcome(() => new CSVFile(new Path(dir + "/missing.csv")).read()).err.startsWith("TypeError: csv: cannot read P:"), "a missing file names the path");

writeFile(p, "a;b\n'x;y';'it''s'\n");
check(J(new CSVFile(p).read({ delimiter: ";", quote: "'" }).rows) === J([["x;y", "it's"]]), "read() honours delimiter and quote");
writeFile(p, "a,b\n1,2\n");
check(outcome(() => new CSVFile(p).selectColumnRange({ columns: ["zz"] })).err === "TypeError: csv.selectColumnRange: no such column P", "selectColumnRange unknown column");
check(outcome(() => new CSVFile(p).readColumnValuesRange({ column: "zz" })).err === "TypeError: csv.readColumnValuesRange: no such column P", "readColumnValuesRange unknown column");

await __kitS.section("a 150,000-row file (repeated cells cached) reads back exactly", async () => {
    let text = "id,q,w\n";
    for (let i = 0; i < 150000; i++) text += i + ",\"v" + (i % 5) + "\"\"x\"," + (i % 3 ? "k" : "") + "\n";
    writeFile(p, text);
    const f = new CSVFile(p);
    const r = f.read();
    let ok = r.totalRows === 150000;
    for (let i = 0; ok && i < 150000; i += 997) ok = J(r.rows[i]) === J([String(i), "v" + (i % 5) + "\"x", i % 3 ? "k" : ""]);
    check(ok && J(r.rows[149999]) === J(["149999", "v4\"x", "k"]), "a 150,000-row file (repeated cells cached) reads back exactly");
    const rr = f.readRowRange({ start: 70000, end: 72000, maxRows: 5000 });
    check(rr.rows.length === 2000 && J(rr.rows[1999]) === J(r.rows[71999]), "a wide readRowRange window matches read()");
    let seen = 0, same = true;
    for await (const b of f.rows({ batch: 40000 })) { if (J(b.rows[b.rows.length - 1]) !== J(r.rows[b.offset + b.rows.length - 1])) same = false; seen += b.rows.length; }
    check(seen === 150000 && same, "rows() over the big file matches read()");
});
__kitS.end();

removeAll(dir);
print(`test_csv_csv_file_reads_index: ${n - failures}/${n} passed`);
if (failures) throw new Error(failures + " failures");
