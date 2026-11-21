// covers: dyna:structures.Table
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_table_record_codecs");
import { Table, BloomFilter, CountMinSketch, HyperLogLog } from "dyna:structures";
import { Graph } from "dyna:graph";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }
function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

__kitS.section("Table: cell count", () => {
    const R = 60, C = 40;
    const t = new Table();
    for (let r = 0; r < R; r++)
        for (let c = 0; c < C; c++)
            t.put("region/" + r + "/subdivision", "metric/" + c + "/daily", r * 1000 + c);
    const rec = t.serialize();
    const back = Table.deserialize(rec);
    eq(back.size, t.size, "Table: cell count");
    let bad = null;
    for (let r = 0; r < R && !bad; r++)
        for (let c = 0; c < C; c++) {
            const rk = "region/" + r + "/subdivision", ck = "metric/" + c + "/daily";
            if (back.get(rk, ck) !== t.get(rk, ck)) { bad = rk + " x " + ck; break; }
        }
    check(bad === null, "Table: every cell survives, wrong at " + bad);
    check(sameBytes(rec, back.serialize()), "Table: re-encode is byte-identical");
    const naive = R * C * (("region/0/subdivision").length + ("metric/0/daily").length);
    check(rec.length < naive / 3,
          "the dictionary beats repeating both names per cell: " + rec.length +
          " vs " + naive);

    {
        const u = new Table();
        u.put("a", "a", 1); u.put("a", "b", 2); u.put("b", "a", 3);
        const b = Table.deserialize(u.serialize());
        eq(b.get("a", "a"), 1, "Table: (a,a)");
        eq(b.get("a", "b"), 2, "Table: (a,b)");
        eq(b.get("b", "a"), 3, "Table: (b,a)");
        eq(b.get("b", "b"), undefined, "Table: (b,b) is absent");
        const v = new Table();
        v.put("ab", "c", 10); v.put("a", "bc", 20); v.put("", "", 30);
        const d = Table.deserialize(v.serialize());
        eq(d.get("ab", "c"), 10, "Table: ('ab','c')");
        eq(d.get("a", "bc"), 20, "Table: ('a','bc')");
        eq(d.get("", ""), 30, "Table: the empty pair");
        eq(d.size, 3, "Table: three distinct cells");
    }

    eq(Table.deserialize(new Table().serialize()).size, 0, "Table: empty");
    {
        const o = new Table(); o.put("only", "cell", 7);
        const b = Table.deserialize(o.serialize());
        eq(b.size, 1, "Table: one cell");
        eq(b.get("only", "cell"), 7, "Table: its value");
    }

    {
        const b = Table.deserialize(rec);
        eq(b.row("region/3/subdivision").length, C, "a decoded table slices by row");
        eq(b.column("metric/7/daily").length, R, "and by column");
    }
});
__kitS.end();

if (fails === 0) print("test_structures_record_codecs: all " + n + " checks passed");
else print("test_structures_record_codecs: " + fails + " FAILED of " + n);
