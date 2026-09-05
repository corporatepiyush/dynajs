/*
 * Table.row / Table.column: the lazily-built row and column chains.
 *
 * These used to scan every cell, O(n) per slice. The chains are built on first
 * use, EXTENDED by put, and REPAIRED by delete -- three paths, each of which
 * fails differently:
 *
 *   build    wrong order, or a missing record
 *   extend   a record added after the build is invisible to the next slice
 *   repair   a swap-remove renumbers records, and an index slot BORROWS its
 *            key from the record that created the chain -- so deleting that
 *            record frees the string the slot points at. That is a
 *            use-after-free, not a wrong answer, and only churn plus a
 *            sanitizer finds it.
 *
 * The oracle throughout is a full scan built in JS from the same puts, matched
 * element by element IN ORDER: the old implementation returned records in
 * insertion order and callers can see that.
 */
import { Table } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

/* The oracle: a plain JS model of the same table. */
function model() {
    const cells = [];                    /* {row, col, val}, insertion order */
    return {
        put(r, c, v) {
            const i = cells.findIndex(x => x.row === r && x.col === c);
            if (i >= 0) cells[i].val = v; else cells.push({ row: r, col: c, val: v });
        },
        del(r, c) {
            const i = cells.findIndex(x => x.row === r && x.col === c);
            if (i < 0) return false;
            /* INSERTION order, deliberately. The dense array swap-removes, so
               the old full scan returned whatever that left -- an artifact no
               caller could rely on. The chains keep the order the cells were
               put in, which is the stronger contract this pins. */
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

/* --------------------------------------------- 1. build, in order */
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

/* ------------------------------- 2. EXTEND: a put after a slice is visible
   The chains are built on first use; a record added afterwards must appear in
   the next slice, and at the END, since the old scan returned insertion order. */
{
    const t = new Table();
    for (let i = 0; i < 10; i++) t.put("r", "c" + i, i);
    eq(t.row("r").length, 10, "the build sees the first ten");
    t.put("r", "cX", 99);
    const got = t.row("r");
    eq(got.length, 11, "a put after the build is visible");
    eq(got[10][0], "cX", "and it is LAST, matching insertion order");
    /* a put into a row that did not exist at build time */
    t.put("brand new", "c0", 7);
    eq(t.row("brand new").length, 1, "a wholly new row appears");
    eq(t.column("c0").length, 2, "and it joins the existing column");
    /* overwriting a value must not duplicate the record */
    t.put("r", "c0", 1000);
    eq(t.row("r").length, 11, "an overwrite does not add a record");
    eq(t.row("r")[0][1], 1000, "an overwrite changes the value in place");
}

/* ------------------------- 3. REPAIR: delete, including the chain-owning record
   The record that CREATED a chain owns the key string the index borrows.
   Deleting it must re-point the slot, not leave it dangling. Deleting the
   head, the middle, the tail and the only member are four different paths. */
{
    for (const which of ["head", "middle", "tail", "only"]) {
        const t = new Table(), m = model();
        const N = which === "only" ? 1 : 6;
        for (let i = 0; i < N; i++) { t.put("row", "c" + i, i); m.put("row", "c" + i, i); }
        for (let i = 0; i < 4; i++) { t.put("other", "c" + i, 100 + i); m.put("other", "c" + i, 100 + i); }
        t.row("row");                            /* force the build */
        const target = which === "head" ? 0 : which === "tail" ? N - 1 :
                       which === "only" ? 0 : 2;
        check(t.delete("row", "c" + target), which + ": the delete reported success");
        m.del("row", "c" + target);
        let bad = samePairs(t.row("row"), m.row("row"));
        check(bad === null, which + ": row after delete -- " + bad);
        bad = samePairs(t.row("other"), m.row("other"));
        check(bad === null, which + ": the OTHER row is untouched -- " + bad);
        /* every column must still be right too */
        for (let c = 0; c < N && bad === null; c++)
            bad = samePairs(t.column("c" + c), m.col("c" + c));
        check(bad === null, which + ": columns after delete -- " + bad);
        eq(t.size, m.size(), which + ": size");
    }
}

/* --------------- 4. CHURN: the case that finds a dangling borrowed key
   Repeated create/delete of whole rows forces the swap-remove to move records
   between chains and forces chain-owning records to be freed. Under ASan a
   dangling slot key is a heap-use-after-free here; without a sanitizer this
   is still a correctness test, because the freed bytes usually still compare
   equal and the wrong chain is returned. */
{
    const t = new Table(), m = model();
    let bad = null;
    for (let round = 0; round < 60 && !bad; round++) {
        for (let i = 0; i < 30; i++) {
            const r = "r" + ((round * 7 + i) % 25), c = "c" + (i % 11);
            t.put(r, c, round * 100 + i); m.put(r, c, round * 100 + i);
        }
        /* read, so the chains are live and being repaired rather than rebuilt */
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

/* --------------------------------- 5. keys that collide in awkward ways
   A row and a column with the same name share nothing, and the pair index is
   length-prefixed so ("ab","c") and ("a","bc") are different cells. The slice
   chains must keep the same discipline. */
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
    /* the empty string is a legal key */
    t.put("", "", 5);
    eq(t.row("").length, 1, "the empty row key");
    eq(t.column("").length, 1, "the empty column key");
    eq(t.get("", ""), 5, "the empty pair");
}

/* ------------------------------------------- 6. a record round trip keeps it */
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
