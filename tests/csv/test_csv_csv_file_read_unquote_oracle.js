// covers: dyna:csv.CSVFile.read
// csvx_unquote collapses doubled quotes in a quoted cell: cells under 64 bytes take the byte loop, longer ones
// copy memchr-delimited runs with memcpy (a quoted run of 200 bytes unquotes ~3x faster). Every cell length
// across the 64-byte switch, doubled quotes at the start, end and back to back, and a custom quote byte must
// read back as the JS oracle unquotes them.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_csv_file_read_unquote_oracle");
import { CSVFile } from "dyna:csv";
import { makeTempDir, removeAll, writeFile, Path } from "dyna:file";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; print("FAIL: " + what); } }
let seed = 0xC5F00D;
const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
const dir = makeTempDir("dyna-csv-unquote-");
const enc = (s, q) => q + s.split(q).join(q + q) + q;
function roundTrip(cells, q, what) {
    const text = "h1,h2\n" + cells.map((c, i) => enc(c, q) + "," + enc("r" + i, q)).join("\n") + "\n";
    const p = new Path(dir + "/t.csv");
    writeFile(p, text);
    const r = new CSVFile(p).read(q === '"' ? { limit: 1e9 } : { limit: 1e9, quote: q });
    ok(r.rows.length === cells.length, what + ": " + r.rows.length + " rows, want " + cells.length);
    for (let i = 0; i < cells.length; i++) {
        const got = r.rows[i] && r.rows[i][0];
        if (got !== cells[i]) { ok(false, what + " row " + i + ": got " + JSON.stringify(got) + ", want " + JSON.stringify(cells[i])); return; }
    }
    ok(true, what);
}

__kitS.section("cells of every length 0..300 with random doubled quotes", () => {
    const alpha = "abc ,xyz\"\n01";
    for (const q of ['"', "'"]) {
        const cells = [];
        for (let len = 0; len <= 300; len++) {
            let s = "";
            for (let j = 0; j < len; j++) s += alpha[(rnd() * alpha.length) | 0].replace('"', q);
            cells.push(s);
        }
        roundTrip(cells, q, "quote " + q);
    }
});

__kitS.section("doubled quotes at the start, the end and back to back across the 64-byte switch", () => {
    const cells = [];
    for (const len of [1, 2, 60, 61, 62, 63, 64, 65, 66, 127, 128, 200]) {
        const body = "x".repeat(len);
        cells.push('"' + body, body + '"', '""' + body + '""', body.slice(0, len >> 1) + '"""' + body.slice(len >> 1), '"'.repeat(len));
    }
    roundTrip(cells, '"', "edges");
});

removeAll(dir);
if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_csv_csv_file_read_unquote_oracle: " + checks + " checks passed");
__kitS.end();
