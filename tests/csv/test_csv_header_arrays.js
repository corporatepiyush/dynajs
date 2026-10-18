// flags: --std
// CSVFile header/column lists (create's headers, read's columns), found by
// the signature-driven input generator (tests/lib/sigfuzz.js): a sparse list
// of length 2^32-1 used to calloc 32 GiB and stringify four billion holes into
// "undefined" headers (10 GB resident before it was killed). Every entry must
// now be a string, so a hole fails at its index at once.
import { CSVFile } from "dyna:csv";
import { Path } from "dyna:file";
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }

const dir = `${(std.getenv("TMPDIR") || "/tmp").replace(/\/+$/, "")}/csv-header-arrays-${os.getpid()}`;
os.mkdir(dir, 0o700);
let k = 0;
const fresh = () => new CSVFile(new Path(`${dir}/t${++k}.csv`));

for (const len of [2 ** 20, 2 ** 31 - 1, 2 ** 32 - 1]) {
    const h = [];
    h.length = len;
    const t0 = performance.now();
    const m = err(() => fresh().create({ headers: h }));
    const dt = performance.now() - t0;
    ok(/^(TypeError: csv: 'headers'\[0\] must be a string|RangeError: csv: 'headers' has more entries)/.test(m), `create: a ${len}-slot sparse header list is refused (${m})`);
    ok(dt < 500, `create: and refused before any work (${dt.toFixed(1)} ms)`);
}
eq(err(() => fresh().create({ headers: ["a", 2] })), "TypeError: csv: 'headers'[1] must be a string", "create: a number header is refused by index");
eq(err(() => fresh().create({ headers: ["a", , "c"] })), "TypeError: csv: 'headers'[1] must be a string", "create: a hole is refused, not written as \"undefined\"");
{
    const h = ["a"];
    Object.defineProperty(h, 1, { get() { throw new RangeError("getter threw"); } });
    eq(err(() => fresh().create({ headers: h })), "RangeError: getter threw", "create: a throwing getter's own error propagates");
}

const f = fresh();
eq(f.create({ headers: ["a", "b"], rows: [[1, 2], [3, 4]] }).rows, 2, "create with string headers (control)");
{
    const c = [];
    c.length = 2 ** 32 - 1;
    const t0 = performance.now();
    const m = err(() => f.read({ columns: c }));
    ok(/^(TypeError: csv: 'columns'\[0\] must be a string|RangeError)/.test(m), `read: a sparse column list is refused (${m})`);
    ok(performance.now() - t0 < 500, "read: and refused before any work");
}
eq(JSON.stringify(f.read({ columns: ["b"] }).rows), '[["2"],["4"]]', "read with a string column list (control)");

// addRow's bare-array form is one row truncated to the header width, so only
// those cells are read: a sparse row of 2^32-1 slots used to be scanned in
// full for nested arrays first (236 s), and that scan swallowed getter errors.
{
    const g = fresh();
    g.create({ headers: ["a", "b"] });
    const row = ["1", "2"];
    row.length = 2 ** 32 - 1;
    const t0 = performance.now();
    eq(g.addRow(row).added, 1, "addRow: a 2^32-1 slot bare row adds one row");
    ok(performance.now() - t0 < 500, "addRow: reading only the header's width");
    eq(JSON.stringify(g.read().rows), '[["1","2"]]', "addRow: and the row holds the first two cells");
    eq(err(() => g.addRow([["x", "y"]])), "TypeError: csv.addRow: a bare array is one positional row -- pass {rows: [...]} to add several", "addRow: a nested bare array is still refused (control)");
    const boom = ["1"];
    Object.defineProperty(boom, 1, { get() { throw new RangeError("cell getter threw"); } });
    eq(err(() => g.addRow(boom)), "RangeError: cell getter threw", "addRow: a throwing cell getter's error propagates");
    eq(JSON.stringify(g.read().rows), '[["1","2"]]', "addRow: and nothing was added by the refused calls");
}
for (let i = 1; i <= k; i++) os.remove(`${dir}/t${i}.csv`);
os.remove(dir);
print("test_csv_header_arrays: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
