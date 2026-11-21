// covers: c:src/modules/csv/csv-index.c
// csv.parse, CsvIndex.row and CsvIndex.column dedupe short cells (<= 48 bytes) of a column through a
// per-column hash table. The table used to be 4096 slots (128 KiB) per column, calloc'd, scanned and
// freed on every call: a 2-row parse cost ~9 us. It is now sized from the row count (16 slots up to
// 4096, at least twice the rows), so a small parse is ~0.7 us. Dedupe is unobservable; this pins that
// every row count across the size steps (and past the 4096 cap, where the table fills and stops
// caching) reads the same cells as a reference built from the generated rows: repeated values,
// distinct values, quoted cells with commas and doubled quotes, empty cells and cells over 48 bytes.
import { suite } from "../lib/kit.js";
import { parse, index } from "dyna:csv";

const t = suite("csv parse cache sizes");
const LONG = "L".repeat(49);
function cellOf(r, c, mode) {
    switch ((r * 3 + c + mode) % 7) {
        case 0: return "rep" + (c % 3);
        case 1: return "u" + r + "_" + c;
        case 2: return "q,\"" + (r % 5) + "\"";
        case 3: return "";
        case 4: return LONG + (r % 4);
        case 5: return LONG + r;
        default: return "v" + ((r * 7 + c) % 13);
    }
}
const enc = (s) => (/[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s);
function gen(rows, width, mode) {
    const want = [];
    for (let r = 0; r < rows; r++) {
        const row = [];
        for (let c = 0; c < width; c++) row.push(cellOf(r, c, mode));
        want.push(row);
    }
    return { want, text: want.map((row) => row.map(enc).join(",")).join("\n") + "\n" };
}
const SIZES = [1, 2, 7, 8, 9, 15, 16, 17, 31, 33, 255, 1023, 1024, 2047, 2048, 2049, 4096, 6000];
for (const width of [1, 3, 70]) {
    t.test("parse matches the generated rows, width " + width, (a) => {
        for (const rows of SIZES) {
            if (width === 70 && rows > 2049) continue;
            for (const mode of [0, 3]) {
                const { want, text } = gen(rows, width, mode);
                a.deep(parse(text, { hasHeader: false }).rows, want, "rows " + rows + " mode " + mode);
            }
        }
    });
}
t.test("CsvIndex.column and row match the generated rows", (a) => {
    for (const rows of SIZES) {
        const { want, text } = gen(rows, 4, 1);
        const ix = index(text, { hasHeader: false });
        for (let c = 0; c < 4; c++) a.deep(ix.column(c), want.map((row) => row[c]), "column " + c + " rows " + rows);
        a.deep(ix.row(rows - 1), want[rows - 1], "last row, rows " + rows);
    }
});
t.test("a column of one repeated value past the cap stays exact", (a) => {
    const rows = 9000, text = Array.from({ length: rows }, (_, i) => (i % 2 ? "same" : "x" + i)).join("\n") + "\n";
    const got = parse(text, { hasHeader: false }).rows;
    a.eq(got.length, rows);
    let bad = 0;
    for (let i = 0; i < rows; i++) if (got[i][0] !== (i % 2 ? "same" : "x" + i)) bad++;
    a.eq(bad, 0, "mismatched cells");
});
await t.run();
