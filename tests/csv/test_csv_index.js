// covers: integration dyna:csv.index dyna:csv.parse -- both run on one structural index; fuzzed against a JS port of the scalar scanner
// tests/csv/test_csv_index.js -- M1: CSV runs on one structural index. A
// block pass classifies 64-byte blocks into quote / delimiter / CR / LF masks,
// a prefix-XOR of the quote mask gives the in-quote mask, and the separators
// outside quotes are flattened into one u32 offset array (plus row starts).
// Big inputs are split into chunks whose starting quote parity comes from a
// prefix sum of per-chunk quote counts. Input whose quoting is not regular (a
// quote that does not open a field, text after a closing quote, an unterminated
// quote) falls back to the scalar scanner, so every legacy tolerant/strict rule
// is unchanged. This test checks parse() and index() against a JS port of the
// scalar scanner on fuzzed text, and against generated ground truth on inputs
// big enough to run on several threads, with chunk boundaries inside quoted
// fields, CRLF pairs and multi-byte characters. The tolerant rules work on
// bytes (one byte after a closing quote's stray text ends the row), so a fuzz
// case can split a UTF-8 sequence; a field holding invalid UTF-8 is compared
// only as "invalid", since the engine and TextDecoder replace it differently.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_index");
import { parse, index } from "dyna:csv";
import { writeFile, makeTempFile, Path } from "dyna:file";

let n = 0, failures = 0;
function check(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }
const J = JSON.stringify;

const DEC = new TextDecoder();
function refScan(text, delim, quote, strict) {
    const B = new TextEncoder().encode(text), D = delim.charCodeAt(0), Q = quote.charCodeAt(0), LF = 10, CR = 13;
    const rows = [];
    let row = [];
    let i = 0;
    const len = B.length;
    if (len === 0) return rows;
    for (;;) {
        if (i < len && B[i] === Q) {
            const f = [];
            let closed = false;
            i++;
            while (i < len) {
                if (B[i] !== Q) { f.push(B[i++]); continue; }
                if (i + 1 < len && B[i + 1] === Q) { f.push(Q); i += 2; continue; }
                i++;
                closed = true;
                break;
            }
            if (strict) {
                if (!closed) throw new SyntaxError("row " + (rows.length + 1) + ": unterminated quoted field");
                if (i < len && B[i] !== D && B[i] !== LF && B[i] !== CR)
                    throw new SyntaxError("row " + (rows.length + 1) + ": unexpected text after a closing quote");
            }
            row.push(DEC.decode(new Uint8Array(f)));
        } else {
            let j = i;
            while (j < len && B[j] !== D && B[j] !== LF && B[j] !== CR) j++;
            row.push(DEC.decode(B.subarray(i, j)));
            i = j;
        }
        if (i >= len) { rows.push(row); break; }
        const c = B[i];
        if (c === D) { i++; continue; }
        if (c === CR && i + 1 < len && B[i + 1] === LF) i += 2;
        else i++;
        rows.push(row);
        row = [];
        if (i >= len) break;
    }
    return rows;
}

function refParse(text, opts) {
    opts = opts || {};
    const delim = opts.delimiter || ",", quote = opts.quote || "\"";
    if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);
    const raw = refScan(text, delim, quote, !!opts.strict);
    const width = raw.length ? raw[0].length : 0;
    const shaped = raw.map((r) => { const o = r.slice(0, width); while (o.length < width) o.push(""); return o; });
    const hasHeader = opts.hasHeader !== false;
    const headers = hasHeader && shaped.length ? shaped[0] : [];
    const rows = hasHeader ? shaped.slice(1) : shaped;
    return { headers, rows, totalRows: rows.length };
}

function scrub(v) {
    if (typeof v === "string") return v.includes("\ufffd") ? "<invalid utf-8>" : v;
    if (Array.isArray(v)) return v.map(scrub);
    if (v && typeof v === "object") { const o = {}; for (const k in v) o[k] = scrub(v[k]); return o; }
    return v;
}

function outcome(f) {
    try { return { ok: scrub(f()) }; } catch (e) { return { err: e.constructor.name + ": " + String(e.message).replace(/^csv\.\w+: /, "") }; }
}

let seed = 0x5eed;
function rnd(k) { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % k; }

const ATOMS = ["a", "b", "1", "2.5", ",", ";", "\"", "'", "\n", "\r", "\r\n", " ", "\u00e9", "\u4e2d", "\u{1F600}", "\"\"", "x\"y", ""];
function fuzzText() {
    let s = rnd(10) === 0 ? "\ufeff" : "";
    const k = rnd(60);
    for (let i = 0; i < k; i++) s += ATOMS[rnd(ATOMS.length)];
    return s;
}

const DIALECTS = [{}, { delimiter: ";" }, { quote: "'" }, { delimiter: ";", quote: "'", hasHeader: false }, { hasHeader: false }];
let mism = 0;
for (let t = 0; t < 4000 && mism < 5; t++) {
    const text = fuzzText();
    const opts = Object.assign({}, DIALECTS[t % DIALECTS.length], t % 3 === 0 ? { strict: true } : {});
    const want = outcome(() => refParse(text, opts));
    const got = outcome(() => parse(text, opts));
    if (J(want) !== J(got)) { mism++; check(false, "parse fuzz " + t + " " + J(text) + " " + J(opts) + ": want " + J(want) + " got " + J(got)); }
    const gotIx = outcome(() => {
        const ix = index(text, opts);
        const rows = [];
        for (let r = 0; r < ix.rowCount; r++) rows.push(ix.row(r));
        return { headers: ix.headers, rows, totalRows: ix.rowCount };
    });
    if (J(want) !== J(gotIx)) { mism++; check(false, "index fuzz " + t + " " + J(text) + " " + J(opts) + ": want " + J(want) + " got " + J(gotIx)); }
}
check(mism === 0, "4000 fuzzed inputs: parse() and index() match the scalar reference");

const PIECES = ["", "plain", "with,comma", "with \"quotes\"", "line\nbreak", "crlf\r\ninside", "\u00e9t\u00e9", "\u4e2d\u6587", "\u{1F600}", "12", "-3.5e2", "\"", ",", "\r"];
function needsQuote(f, d, q) { return f.includes(d) || f.includes(q) || f.includes("\n") || f.includes("\r") || f === "" && rnd(3) === 0; }
function enc(f, d, q) {
    if (!needsQuote(f, d, q) && rnd(4)) return f;
    return q + f.split(q).join(q + q) + q;
}
function genTable(rows, width, ragged) {
    const t = [];
    for (let r = 0; r < rows; r++) {
        const w = ragged && r > 0 && rnd(7) === 0 ? 1 + rnd(width + 2) : width;
        const row = [];
        for (let c = 0; c < w; c++) row.push(PIECES[rnd(PIECES.length)] + (rnd(3) ? "" : String(r)));
        if (w === 1 && row[0] === "") row[0] = "e";
        t.push(row);
    }
    return t;
}
function toText(t, d, q) {
    let s = "";
    for (const row of t) s += row.map((f) => enc(f, d, q)).join(d) + (rnd(2) ? "\r\n" : "\n");
    return s;
}
function shape(t, width) { return t.map((r) => { const o = r.slice(0, width); while (o.length < width) o.push(""); return o; }); }

for (const [label, rows, d, q, ragged] of [["small", 300, ",", "\"", false], ["ragged", 2000, ",", "\"", true], ["big multi-chunk", 140000, ",", "\"", false], ["big ; and '", 140000, ";", "'", true]]) {
    const width = 6;
    const t = genTable(rows, width, ragged);
    const text = toText(t, d, q);
    const want = shape(t, width);
    const opts = { delimiter: d, quote: q };
    const p = parse(text, opts);
    check(J(p.headers) === J(want[0]) && J(p.rows) === J(want.slice(1)), label + ": parse() matches the generated table (" + text.length + " chars)");
    const ix = index(text, opts);
    let ok = ix.rowCount === rows - 1 && ix.columnCount === width && J(ix.headers) === J(want[0]);
    for (let r = 0; ok && r < ix.rowCount; r += 1 + rnd(5)) ok = J(ix.row(r)) === J(want[r + 1]) && ix.get(r, r % width) === want[r + 1][r % width];
    check(ok, label + ": index() rows and cells match");
    const col = ix.column(2), dict = ix.column(3, "dict");
    let cok = col.length === rows - 1 && dict.codes.length === rows - 1;
    for (let r = 0; cok && r < rows - 1; r++) cok = col[r] === want[r + 1][2] && dict.values[dict.codes[r]] === want[r + 1][3];
    check(cok, label + ": column() strings and dict codes/values match");
    check(new Set(dict.values).size === dict.values.length, label + ": dict values are distinct");
}

__kitS.section("f64 / i32 / i64 columns parse exactly like Number() and BigInt(), quoted cells", () => {
    const rows = ["id,price,qty,big"];
    const want = [];
    for (let r = 0; r < 200000; r++) {
        const price = (r % 977) / 7 + (r % 3 ? 0 : 1e-7);
        const big = BigInt(r) * 46116860184273n - 4611686018427387904n;
        rows.push(r + "," + (r % 11 === 0 ? "" : String(price)) + "," + (r % 5 === 0 ? "\"" + (r - 100000) + "\"" : String(r - 100000)) + "," + big);
        want.push([r % 11 === 0 ? NaN : price, r - 100000, big]);
    }
    const text = rows.join("\n");
    const ix = index(text);
    const f = ix.column("price", "f64"), i32 = ix.column("qty", "i32"), i64 = ix.column("big", "i64");
    let ok = f instanceof Float64Array && i32 instanceof Int32Array && i64 instanceof BigInt64Array;
    for (let r = 0; ok && r < want.length; r++)
        ok = (Number.isNaN(want[r][0]) ? Number.isNaN(f[r]) : f[r] === want[r][0]) && i32[r] === want[r][1] && i64[r] === want[r][2];
    check(ok, "f64 / i32 / i64 columns parse exactly like Number() and BigInt(), quoted cells included, empty f64 is NaN");
    const t2 = "a\n1\n2.5\n";
    check(outcome(() => index(t2).column("a", "i32")).err === "TypeError: CsvIndex.column: row 1, column 0: '2.5' is not a 32-bit integer", "a non-integer in an i32 column names its row: " + J(outcome(() => index(t2).column("a", "i32"))));
    check(outcome(() => index("a\n2147483648\n").column(0, "i32")).err !== undefined, "i32 overflow is refused");
    check(outcome(() => index("a\nx\n").column(0, "f64")).err !== undefined, "text in an f64 column is refused");
    check(outcome(() => index("a\n9223372036854775808\n").column(0, "i64")).err !== undefined, "i64 overflow is refused");
    check(index("a\n-9223372036854775808\n").column(0, "i64")[0] === -9223372036854775808n, "i64 minimum parses");
});

__kitS.section("a Uint8Array source is copied: mutating it later does not change the index", () => {
    const bytes = new TextEncoder().encode("h1,h2\n\u00e9,\"q\"\"x\"\n");
    const ix = index(bytes);
    bytes.fill(0);
    check(ix.get(0, 0) === "\u00e9" && ix.get(0, "h2") === "q\"x", "a Uint8Array source is copied: mutating it later does not change the index");
    const ab = new TextEncoder().encode("\ufeffa,b\n1,2").buffer;
    check(J(index(ab).row(0)) === J(["1", "2"]) && J(index(ab).headers) === J(["a", "b"]), "an ArrayBuffer source with a BOM");
    const p = makeTempFile("csvix");
    writeFile(p, new TextEncoder().encode("k,v\nx,1\ny,2\n"));
    const fx = index(p);
    check(fx.rowCount === 2 && J(fx.column("v", "f64")) === J(new Float64Array([1, 2])), "a Path source is mapped and indexed");
    check(outcome(() => index(new Path(p.toString() + ".missing"))).err?.startsWith("TypeError"), "a missing file is a TypeError");
    check(outcome(() => index(42)).err?.startsWith("TypeError"), "a number source is a TypeError");
    check(outcome(() => index("a\n1").get(1, 0)).err?.startsWith("RangeError"), "row out of range is a RangeError");
    check(outcome(() => index("a\n1").get(0, "zz")).err?.startsWith("TypeError"), "unknown column name is a TypeError");
    check(outcome(() => index("a\n1", { hasHeader: false }).column("a")).err?.startsWith("TypeError"), "column by name without a header is a TypeError");
    check(outcome(() => index("a\n1").column(0, "u8")).err?.startsWith("TypeError"), "unknown dtype is a TypeError");
    check(outcome(() => index("a\n\"x", { strict: true })).err === "SyntaxError: row 2: unterminated quoted field", "strict index reports an unterminated quote by row");
    const e = index("");
    check(e.rowCount === 0 && e.columnCount === 0 && J(e.headers) === "[]", "empty text is the empty index");
});
__kitS.end();

print("test_csv_index: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_csv_index: " + failures + " failures");
