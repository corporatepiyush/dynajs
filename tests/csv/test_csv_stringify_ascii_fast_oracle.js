// covers: dyna:csv.stringify -- string cells are written from the engine's one-byte string data when it is plain ASCII
// A string cell whose bytes are all ASCII and non-NUL is now scanned once against a 256-entry class table (delimiter,
// quote, CR, LF need quoting; NUL and bytes >= 0x80 fall back) and copied straight from the string's own bytes, quoted
// runs with doubled quotes copied in slices, instead of first converting it to a C string. Every other cell (numbers,
// booleans, null, Latin-1 or wide text, a NUL byte, ropes) still takes the length-aware conversion; a NUL is an
// ordinary byte (F-csv-1 fixed: the cell used to end at its first NUL). Oracle: a JS stringify with the same rules must produce identical text for random
// array and record rows over an alphabet of delimiters, quotes, CR/LF, Latin-1, emoji and NUL, with the default,
// ";", tab/"'" and "|" dialects, plus long cells and wide rows.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_stringify_ascii_fast_oracle");
import * as csv from "dyna:csv";

let checks = 0, fails = 0;
function eq(got, want, what) {
    checks++;
    if (got !== want) {
        fails++;
        let k = 0;
        while (k < got.length && got[k] === want[k]) k++;
        if (fails < 20) print("FAIL: " + what + " at " + k + ": got " + JSON.stringify(got.slice(k, k + 30)) + ", want " + JSON.stringify(want.slice(k, k + 30)));
    }
}
let seed = 4242;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const alpha = ["a", "b", "Z", "0", " ", ",", '"', "\n", "\r", ";", "\t", "'", "|", "é", "\u0000", "😀", "~", "\u007f"];
function cellText(v) {
    if (v === undefined) return null;
    const s = typeof v === "number" ? String(v) : String(v);
    return s;
}
function field(v, d, q) {
    const s = cellText(v);
    if (s === null) return "";
    if (!(s.includes(d) || s.includes(q) || s.includes("\n") || s.includes("\r"))) return s;
    return q + s.split(q).join(q + q) + q;
}
function refArrays(rows, d, q) {
    return rows.map((r) => r.map((v) => field(v, d, q)).join(d) + "\n").join("");
}
function refRecords(recs, d, q, hdr) {
    const keys = Object.keys(recs[0]);
    return (hdr ? keys.map((k) => field(k, d, q)).join(d) + "\n" : "") + recs.map((r) => keys.map((k) => field(r[k], d, q)).join(d) + "\n").join("");
}
function randCell() {
    const r = rnd();
    if (r < 0.05) return Math.floor(rnd() * 2000) - 1000;
    if (r < 0.08) return rnd() * 1e6;
    if (r < 0.1) return null;
    if (r < 0.12) return undefined;
    if (r < 0.14) return rnd() < 0.5;
    const plain = r < 0.6, k = Math.floor(rnd() * (rnd() < 0.05 ? 200 : 9));
    let s = "";
    for (let i = 0; i < k; i++) s += plain ? alpha[Math.floor(rnd() * 5)] : alpha[Math.floor(rnd() * alpha.length)];
    return s;
}
const dialects = [[",", '"', undefined], [";", '"', { delimiter: ";" }], ["\t", "'", { delimiter: "\t", quote: "'" }], [",", "|", { quote: "|" }]];

__kitS.section("array rows equal the JS stringify", () => {
    for (const [n, w] of [[1, 1], [5, 3], [400, 6], [3000, 9], [50, 70]]) {
        const rows = Array.from({ length: n }, () => Array.from({ length: w }, randCell));
        for (const [d, q, o] of dialects) eq(csv.stringify(rows, o), refArrays(rows, d, q), "arrays n=" + n + " w=" + w + " " + JSON.stringify(o));
    }
});

__kitS.section("record rows equal the JS stringify", () => {
    const keys = ["id", "a,b", 'q"k', "é", "plain", "line\nbreak"];
    for (const n of [1, 7, 2500]) {
        const recs = Array.from({ length: n }, () => Object.fromEntries(keys.map((k) => [k, randCell()])));
        for (const [d, q, o] of dialects) {
            eq(csv.stringify(recs, o), refRecords(recs, d, q, true), "records n=" + n + " " + JSON.stringify(o));
            eq(csv.stringify(recs, { ...(o || {}), hasHeader: false }), refRecords(recs, d, q, false), "records n=" + n + " no header " + JSON.stringify(o));
        }
    }
});

__kitS.section("concatenated and sliced strings take the same path", () => {
    const base = "abc,def" + "x".repeat(5000), rows = [];
    for (let i = 0; i < 300; i++) rows.push([base.slice(i, i + 40), "k" + i + '"' + i, ["a", i, "b"].join(""), base.slice(0, i % 7) + "\n"]);
    eq(csv.stringify(rows), refArrays(rows, ",", '"'), "sliced and joined cells");
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_csv_stringify_ascii_fast_oracle: " + checks + " checks passed");
__kitS.end();
