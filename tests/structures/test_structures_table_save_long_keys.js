// covers: dyna:structures.Table
// P13: Table.serialize/save dedups row and column keys with a dictionary sort whose comparator
// reads an 8-byte big-endian prefix first, then the tail. Keys that share the first 8 bytes,
// differ only past byte 8, or are prefixes of each other must stay distinct through
// serialize/deserialize and save/load, whatever order the cells were put in.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_table_save_long_keys");
import { Table } from "dyna:structures";
import * as file from "dyna:file";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const rows = ["shared_p", "shared_pr", "shared_prefix_a", "shared_prefix_b", "shared_prefix_aa",
    "shared_prefix_é", "shared_prefix_\u{1F600}", "", "s", "shared_q", "shared_prefix_b\u0000"];
const cols = ["column_number_1", "column_number_2", "column_number_10", "column_numbe", "c", "column_number_1x"];
function build(order) {
    const t = new Table();
    for (const i of order) t.put(rows[i % rows.length], cols[(i * 7) % cols.length], i);
    return t;
}
const N = rows.length * cols.length;
const fwd = [...Array(N).keys()];
const orders = [fwd, fwd.slice().reverse(), fwd.map((i) => (i * 37) % N)];

__kitS.section("distinct keys survive a round trip", () => {
    for (const o of orders) {
        const t = build(o);
        const u = Table.deserialize(t.serialize());
        eq(u.size, t.size, "cell count");
        for (const [r, c, v] of t.cells()) eq(u.get(r, c), v, "cell " + JSON.stringify(r) + "/" + JSON.stringify(c));
    }
    const t = build(fwd);
    const d = file.makeTempDir("p13tbl");
    try {
        const p = d.join("t.bin");
        t.save(p);
        const w = Table.load(p);
        eq(w.size, t.size, "load cell count");
        for (const [r, c, v] of t.cells()) eq(w.get(r, c), v, "load cell " + JSON.stringify(r) + "/" + JSON.stringify(c));
    } finally {
        file.removeAll(d);
    }
});

print("test_structures_table_save_long_keys: " + n + " assertions, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
