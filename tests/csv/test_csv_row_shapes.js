// CSV rows are built as whole arrays in one step. They must be real Arrays
// with ordinary index properties at every width, including both sides of the
// row builder's 64-column inline capacity.
import { suite } from "../lib/kit.js";
import { parse } from "dyna:csv";

const t = suite("csv_row_shapes");
const plain = (o) => Reflect.ownKeys(o).map((k) => { const d = Object.getOwnPropertyDescriptor(o, k); return k + ":" + (d.writable && d.enumerable && d.configurable && !d.get && !d.set ? "wec" : "OTHER"); }).join(",");

for (const cols of [1, 63, 64, 65, 200]) {
    t.test("csv rows are real arrays at " + cols + " columns", (a) => {
        const header = Array.from({ length: cols }, (_, i) => "h" + i).join(",");
        const line = (r) => Array.from({ length: cols }, (_, i) => "r" + r + "c" + i).join(",");
        const out = parse(header + "\n" + line(0) + "\n" + line(1) + "\n");
        a.eq(out.rows.length, 2);
        for (const row of out.rows) {
            a.ok(Array.isArray(row));
            a.eq(row.length, cols);
            a.eq(Object.keys(row).length, cols);
        }
        a.eq(out.rows[1][cols - 1], "r1c" + (cols - 1));
        a.eq(out.rows[0][0], "r0c0");
        out.rows[0].push("more");
        a.eq(out.rows[0].length, cols + 1, "the row can grow");
        a.eq(plain(out.rows[1]).split(",")[0], "0:wec");
    });
}

t.test("short csv rows are padded to the header width with empty strings", (a) => {
    const out = parse("a,b,c\n1\n1,2,3,4\n");
    a.deep(out.rows, [["1", "", ""], ["1", "2", "3"]]);
});

await t.run();
