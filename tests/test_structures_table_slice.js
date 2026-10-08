import { Table } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

function model() {
    const cells = [];
    return {
        put(r, c, v) {
            const i = cells.findIndex(x => x.row === r && x.col === c);
            if (i >= 0) cells[i].val = v; else cells.push({ row: r, col: c, val: v });
        },
        del(r, c) {
            const i = cells.findIndex(x => x.row === r && x.col === c);
            if (i < 0) return false;
            cells.splice(i, 1);
            return true;
        },
        row(r) { return cells.filter(x => x.row === r).map(x => [x.col, x.val]); },
        col(c) { return cells.filter(x => x.col === c).map(x => [x.row, x.val]); },
        size() { return cells.length; },
    };
}

function samePairs(got, want) {
    if (got.length !== want.length) return "length " + got.length + " vs " + want.length;
    for (let i = 0; i < got.length; i++)
        if (got[i][0] !== want[i][0] || !Object.is(got[i][1], want[i][1]))
            return "at " + i + ": " + JSON.stringify(got[i]) + " vs " + JSON.stringify(want[i]);
    return null;
}

{
    const t = new Table(), m = model();
    for (let i = 0; i < 500; i++) { t.put("r" + (i % 20), "c" + ((i / 20) | 0), i);
                                    m.put("r" + (i % 20), "c" + ((i / 20) | 0), i); }
    let bad = null;
    for (let r = 0; r < 20 && !bad; r++) bad = samePairs(t.row("r" + r), m.row("r" + r));
    check(bad === null, "row() matches a full scan in order -- " + bad);
    for (let c = 0; c < 25 && !bad; c++) bad = samePairs(t.column("c" + c), m.col("c" + c));
    check(bad === null, "column() matches a full scan in order -- " + bad);
    eq(t.row("absent").length, 0, "an unknown row is empty");
    eq(t.column("absent").length, 0, "an unknown column is empty");
}

{
    const t = new Table();
    for (let i = 0; i < 10; i++) t.put("r", "c" + i, i);
    eq(t.row("r").length, 10, "the build sees the first ten");
    t.put("r", "cX", 99);
    const got = t.row("r");
    eq(got.length, 11, "a put after the build is visible");
    eq(got[10][0], "cX", "and it is LAST, matching insertion order");
    t.put("brand new", "c0", 7);
    eq(t.row("brand new").length, 1, "a wholly new row appears");
    eq(t.column("c0").length, 2, "and it joins the existing column");
    t.put("r", "c0", 1000);
    eq(t.row("r").length, 11, "an overwrite does not add a record");
    eq(t.row("r")[0][1], 1000, "an overwrite changes the value in place");
}

{
    for (const which of ["head", "middle", "tail", "only"]) {
        const t = new Table(), m = model();
        const N = which === "only" ? 1 : 6;
        for (let i = 0; i < N; i++) { t.put("row", "c" + i, i); m.put("row", "c" + i, i); }
        for (let i = 0; i < 4; i++) { t.put("other", "c" + i, 100 + i); m.put("other", "c" + i, 100 + i); }
        t.row("row");
        const target = which === "head" ? 0 : which === "tail" ? N - 1 :
                       which === "only" ? 0 : 2;
        check(t.delete("row", "c" + target), which + ": the delete reported success");
        m.del("row", "c" + target);
        let bad = samePairs(t.row("row"), m.row("row"));
        check(bad === null, which + ": row after delete -- " + bad);
        bad = samePairs(t.row("other"), m.row("other"));
        check(bad === null, which + ": the OTHER row is untouched -- " + bad);
        for (let c = 0; c < N && bad === null; c++)
            bad = samePairs(t.column("c" + c), m.col("c" + c));
        check(bad === null, which + ": columns after delete -- " + bad);
        eq(t.size, m.size(), which + ": size");
    }
}

{
    const t = new Table(), m = model();
    let bad = null;
    for (let round = 0; round < 60 && !bad; round++) {
        for (let i = 0; i < 30; i++) {
            const r = "r" + ((round * 7 + i) % 25), c = "c" + (i % 11);
            t.put(r, c, round * 100 + i); m.put(r, c, round * 100 + i);
        }
        for (let r = 0; r < 25 && !bad; r++)
            bad = samePairs(t.row("r" + r), m.row("r" + r));
        for (let i = 0; i < 20 && !bad; i++) {
            const r = "r" + ((round * 3 + i) % 25), c = "c" + ((i * 5) % 11);
            const a = t.delete(r, c), b = m.del(r, c);
            if (a !== b) bad = "delete disagreed at round " + round;
        }
        for (let c = 0; c < 11 && !bad; c++)
            bad = samePairs(t.column("c" + c), m.col("c" + c));
    }
    check(bad === null, "churn: " + bad);
    eq(t.size, m.size(), "churn: final size");
}

{
    const t = new Table();
    t.put("x", "x", 1);
    t.put("x", "y", 2);
    t.put("y", "x", 3);
    eq(t.row("x").length, 2, "row x");
    eq(t.column("x").length, 2, "column x");
    eq(t.row("x")[0][0], "x", "row x first column");
    t.put("ab", "c", 10);
    t.put("a", "bc", 20);
    eq(t.row("ab").length, 1, "('ab','c') and ('a','bc') are different cells");
    eq(t.row("a").length, 1, "and neither leaks into the other's row");
    eq(t.get("ab", "c"), 10, "value of ('ab','c')");
    eq(t.get("a", "bc"), 20, "value of ('a','bc')");
    t.put("", "", 5);
    eq(t.row("").length, 1, "the empty row key");
    eq(t.column("").length, 1, "the empty column key");
    eq(t.get("", ""), 5, "the empty pair");
}

{
    const t = new Table();
    for (let i = 0; i < 200; i++) t.put("r" + (i % 8), "c" + ((i / 8) | 0), i);
    const back = Table.deserialize(t.serialize());
    eq(back.size, t.size, "decoded size");
    let bad = null;
    for (let r = 0; r < 8 && !bad; r++) bad = samePairs(back.row("r" + r), t.row("r" + r));
    check(bad === null, "a decoded table slices identically -- " + bad);
}

if (fails === 0) print("test_structures_table_slice: all " + n + " checks passed");
else print("test_structures_table_slice: " + fails + " FAILED of " + n);
