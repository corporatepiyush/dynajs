// flags: --std
/* review probe: Table deep-dive -- mixed types, degenerate shapes, huge
 * cells, tsv/csv escaping bytes, align validation, mid-build errors. */
import { Table } from "dyna:cli";
let n = 0, fails = 0;
function ok(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    ok(a === b, msg + "\n  got:  " + JSON.stringify(a) + "\n  want: " + JSON.stringify(b));
}
function err(fn, cls, msg) {
    let e = null; try { fn(); } catch (x) { e = x; }
    ok(e && e.constructor.name === cls,
       msg + " (want " + cls + ", got " + (e ? e.constructor.name + ": " + e.message : "no throw") + ")");
    return e ? e.message : "";
}

/* T1 mixed cell types deep in the grid: error must name row AND column */
let m = err(() => Table([["a", "b"], [1, true]], {}), "TypeError", "T1 boolean cell refused");
ok(m.indexOf("(1, 1)") >= 0, "T1 the bad cell is named (1, 1): " + m);
m = err(() => Table([["a"], [null]], {}), "TypeError", "T1b null cell refused");
ok(m.indexOf("(1, 0)") >= 0, "T1b the bad cell is named (1, 0): " + m);
err(() => Table([["a"], [undefined]], {}), "TypeError", "T1c undefined cell refused");
err(() => Table([["a"], [[]]], {}), "TypeError", "T1d array cell refused");
err(() => Table([["a"], [{}]], {}), "TypeError", "T1e object cell refused");

/* T2 degenerate shapes */
eq(Table([]), "", "T2a no rows no head -> empty string");
eq(Table([], { head: [] }), "", "T2b empty head -> empty string");
eq(Table([[]]), "", "T2c one zero-cell row -> empty string");
eq(Table([], { head: ["a"] }), "a\n-\n", "T2d head-only renders the rule");
eq(Table([["x"]], { head: ["h"] }), "h\n-\nx\n", "T2e one cell grid");
/* head [] with non-empty rows: what happens? (pin) */
let t2f; try { t2f = JSON.stringify(Table([["a", "b"]], { head: [] })); }
catch (e) { t2f = "THREW:" + e.constructor.name + ": " + e.message; }
print("PIN T2f head=[] with 2-col rows -> " + t2f);
/* head wider than rows */
m = err(() => Table([["a"]], { head: ["h1", "h2"] }), "TypeError", "T2g ragged row refused");
ok(m.indexOf("row 0") >= 0 && m.indexOf("expected 2") >= 0, "T2g names row and widths: " + m);

/* T3 huge cells (2 MiB) through grid and csv: no truncation, no crash */
const big = "b".repeat(2 * 1024 * 1024);
const t3 = Table([[big, "x"]], { format: "grid" });
ok(t3 === big + "  x\n", "T3a 2MiB cell survives grid verbatim");
const t3b = Table([[big, "y"]], { format: "csv" });
eq(t3b.length, big.length + 3, "T3b csv 2MiB cell exact length");

/* T4 TSV bytes: "TSV quotes nothing" is the documented contract -- cells with
   tabs/newlines are raw injection BY CONTRACT. Pin the bytes. */
eq(Table([["a\tb", "c\nd"]], { format: "tsv" }), "a\tb\tc\nd\n",
   "T4 raw tabs/newlines pass through tsv (documented)");
/* a cell that LOOKS like a formula is not quoted in csv (RFC 4180 does not
   require it) -- pin so nobody assumes spreadsheet-safety */
eq(Table([["=1+1"]], { format: "csv" }), "=1+1\n",
   "T4b formula cell unquoted in csv (RFC 4180 only)");

/* T5 CSV quoting is RFC 4180 */
eq(Table([['a"b']], { format: "csv" }), '"a""b"' + "\n", "T5a doubled quotes");
eq(Table([["a,b"]], { format: "csv" }), '"a,b"' + "\n", "T5b comma quotes");
eq(Table([["a\nb"]], { format: "csv" }), '"a\nb"' + "\n", "T5c LF quotes");
eq(Table([["a\r\nb"]], { format: "csv" }), '"a\r\nb"' + "\n", "T5d CRLF quotes");
eq(Table([["plain", 'q"x']], { head: ["h1", "h2"], format: "csv" }),
   'h1,h2\nplain,"q""x"\n', "T5e head+row csv bytes");
eq(Table([['']], { format: "csv" }), "\n", "T5f empty cell unquoted");

/* T6 grid alignment pixel-exactness (last column: no trailing padding;
   head is aligned like its column) */
eq(Table([["a", "longer"], ["x", "y"]], { head: ["h1", "h2"], align: ["left", "right"] }),
   "h1      h2\n--  ------\na   longer\nx        y\n", "T6a left/right grid");
eq(Table([["a"]], { align: ["center"] }), "a\n", "T6b single col center");
const t6c = Table([["a", "b"], ["xx", "yy"]], { align: ["center", "right"] });
eq(t6c, "a    b\nxx  yy\n", "T6c center/right grid padding");

/* T6 align validation */
m = err(() => Table([["a"]], { align: ["diagonal"] }), "RangeError", "T6d bad alignment refused");
ok(m.indexOf("valid: left, right, center") >= 0, "T6d names the valid set: " + m);
m = err(() => Table([["a"]], { align: ["left", "left"] }), "RangeError", "T6e too many aligns refused");
ok(m.indexOf("2 entries for 1 columns") >= 0, "T6e " + m);
let t6f; try { t6f = JSON.stringify(Table([["a"]], { align: [5] })); }
catch (e) { t6f = "THREW:" + e.constructor.name + ": " + e.message; }
print("PIN T6f align [5] (non-string entry) -> " + t6f);

/* T7 format validation */
m = err(() => Table([["a"]], { format: "xml" }), "RangeError", "T7 unknown format refused");
ok(m.indexOf("valid: grid, tsv, csv") >= 0, "T7 names the valid set: " + m);
err(() => Table([["a"]], { format: 5 }), "TypeError", "T7b non-string format refused");

/* T8 number cells render via ToString */
eq(Table([[1.5, -0, NaN, Infinity, 1e21]]), "1.5  0  NaN  Infinity  1e+21\n",
   "T8 number cells: 1.5 / 0 / NaN / Infinity / 1e+21");

/* T9 head element validation */
m = err(() => Table([["a"]], { head: [5] }), "TypeError", "T9 non-string head refused");
ok(m.indexOf("head[0]") >= 0, "T9 names head[0]: " + m);
m = err(() => Table([[1, 2]], { head: ["a", null] }), "TypeError", "T9b null head element refused");
ok(m.indexOf("head[1]") >= 0, "T9b names head[1]: " + m);

/* T10 mid-build error after allocations (leak hunt: every cell dup'd so far
   must be freed) -- deep grid, error on the LAST row */
const rows10 = [];
for (let i = 0; i < 40; i++) rows10.push(["cell" + i, "x" + i, "z" + i]);
rows10.push(["boom", "x", {}]);
m = err(() => Table(rows10, { head: ["a", "b", "c"], format: "grid" }), "TypeError", "T10 late cell error");
ok(m.indexOf("(40, 2)") >= 0, "T10 names (40, 2): " + m);

/* T11 Proxy rows with a lying length */
const p11 = new Proxy([["a"]], { get(t, k, r) { return k === "length" ? 5 : Reflect.get(t, k, r); } });
m = err(() => Table(p11), "TypeError", "T11 lying length refused cleanly");

/* T12 strict opts bag */
m = err(() => Table([["a"]], { head: ["h"], aligns: [] }), "TypeError", "T12 unknown key refused");
ok(m.indexOf('unknown option "aligns"') >= 0, "T12 " + m);

print("probe_table: " + (n - fails) + "/" + n + " ok");
if (fails) throw new Error("probe_table failures: " + fails);
