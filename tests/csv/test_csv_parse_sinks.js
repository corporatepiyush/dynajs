// csv.parse builds its rows straight from the input bytes (one JS string per
// cell, no intermediate table) and reuses the previous row's string when a
// cell's bytes repeat in the same column; CSVFile.read still goes through the
// C table. Both are driven by the same scanner, so for any document without a
// NUL byte they must return the same headers, rows and errors -- which makes
// the table path the oracle for the direct path and its repeat bypass.
// (An empty document is the one designed difference: CSVFile.read refuses it.)
import * as csv from "dyna:csv";
import { makeTempDir, removeAll, writeFile, Path } from "dyna:file";

let n = 0, fails = 0;
function ok(c, m, extra) { n++; if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " -- " + extra : "")); } }

const dir = makeTempDir("dyna-csv-sinks-");
const file = new csv.CSVFile(new Path(dir.toString() + "/t.csv"));
const viaTable = (text, opts) => { writeFile(new Path(dir.toString() + "/t.csv"), text); return file.read(opts); };
const run = (f) => { try { return JSON.stringify(f()); } catch (e) { return e.name + ": " + e.message.replace(/^csv(\.parse|File\.read)?:? */, ""); } };

function agree(text, opts, label) {
    if (text === "")
        return;
    const a = run(() => csv.parse(text, opts));
    const b = run(() => viaTable(text, opts));
    ok(a === b, label, a.slice(0, 160) + "  vs  " + b.slice(0, 160));
}

// 1. the repeat bypass: same column, equal length, different bytes; equal
//    bytes across a quoted/unquoted boundary; padding; truncation
agree("h1,h2\nab,cd\nab,ce\nac,cd\nab,cd\n", undefined, "equal-length cells that differ are not reused");
agree('h\nx\n"x"\nx\n"y"\ny\n', undefined, "quoted and unquoted spellings of one value");
agree("a,b,c\n1\n1,2\n1,2,3,4\n,,\n1\n", undefined, "short rows pad, long rows truncate, with repeats");
agree("a,b\n,\n,\nx,\n,x\n", undefined, "empty cells repeating");
{
    const w = 300, hdr = [], r1 = [], r2 = [];
    for (let i = 0; i < w; i++) { hdr.push("c" + i); r1.push("v" + (i % 7)); r2.push(i % 2 ? "v" + (i % 7) : "w" + i); }
    agree([hdr, r1, r2, r1, r2].map((r) => r.join(",")).join("\n") + "\n", undefined, "300 columns: past the 256 columns the bypass tracks");
}
{
    // the reused value is the same string, and still a correct one
    const p = csv.parse("k\nsame\nsame\nother\nsame\n");
    ok(p.rows.map((r) => r[0]).join() === "same,same,other,same", "repeated cells read back correctly");
}

// 2. generated documents, every option, all three error paths
{
    let s = 7;
    const rnd = () => (s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296;
    const alpha = ["a", "b", "1", " ", ",", ";", "\t", '"', "'", "\n", "\r\n", "\r", "é", "日", "😀", "x".repeat(70), "", "true"];
    const OPTS = [undefined, { strict: true }, { delimiter: ";" }, { delimiter: "\t", quote: "'" }];
    for (let c = 0; c < 1200; c++) {
        let t = "";
        const rows = 1 + Math.floor(rnd() * 10);
        for (let r = 0; r < rows; r++) {
            const cols = 1 + Math.floor(rnd() * 6), cells = [];
            for (let k = 0; k < cols; k++) {
                let v = "";
                const len = Math.floor(rnd() * 4);
                for (let j = 0; j < len; j++) v += alpha[Math.floor(rnd() * alpha.length)];
                if (rnd() < 0.3) v = '"' + v.replace(/"/g, rnd() < 0.8 ? '""' : '"') + (rnd() < 0.95 ? '"' : "");
                if (rnd() < 0.25 && r > 0) v = "rep";
                cells.push(v);
            }
            t += cells.join(",") + (r < rows - 1 || rnd() < 0.5 ? (rnd() < 0.2 ? "\r\n" : "\n") : "");
        }
        agree(t, OPTS[c % OPTS.length], "generated #" + c);
    }
}

// 3. a NUL byte is data: csv.parse keeps it (the C table stops at it)
{
    const p = csv.parse("h\na\u0000b\n");
    ok(p.rows[0][0] === "a\u0000b", "a NUL inside a cell is kept", JSON.stringify(p.rows));
}

removeAll(dir);
print("test_csv_parse_sinks: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
