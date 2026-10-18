// Black-box contract test for dyna:csv, generated from dynajs.d.ts lines 1075-1150 (parse/stringify in-memory surface only). Engine sources not consulted.
// PARAMETRIC: case tables driven through one loop per table; rows are [label, args, expected] and every
// failure message names its row. Expected values are derived from the dynajs.d.ts contract text (row shaping,
// strict/tolerant modes, dialect overrides, STRICT option bags) and the documented examples in the module's
// own API contract. CSVFile is excluded: it writes files, and this suite is deterministic and write-free.
import { parse, stringify } from "dyna:csv";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg + " — got |" + JSON.stringify(a) + "| expected |" + JSON.stringify(b) + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true, e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }

/* ==========================================================================
 * TABLE 1 — parse(): the RFC 4180 reader. Rows are [label, text, opts,
 * [headers, rows, totalRows]]. d.ts lines 1082-1093.
 * ========================================================================== */
const PARSE = [
    // Documented example: header + two data rows, totalRows counts DATA rows.
    ["basic parse (contract example)", "name,score\nalice,95\nbob,82\n", {}, ["name", "score"], [["alice", "95"], ["bob", "82"]], 2],
    // "an empty text is the empty table (no throw)"
    ["empty text is the empty table", "", {}, [], [], 0],
    ["empty text, headerless, is the empty table", "", { hasHeader: false }, [], [], 0],
    // "A leading UTF-8 BOM is stripped"
    ["leading UTF-8 BOM stripped", "\uFEFFname,score\n1,2\n", {}, ["name", "score"], [["1", "2"]], 1],
    // Last record may omit the trailing newline (RFC 4180).
    ["no trailing newline still yields the row", "a,b\n1,2", {}, ["a", "b"], [["1", "2"]], 1],
    // CR-LF line breaks must not leak into fields (RFC 4180 record separator).
    ["CRLF line endings", "a,b\r\n1,2\r\n", {}, ["a", "b"], [["1", "2"]], 1],
    // Quoted fields: embedded delimiter.
    ["quoted field holds a comma", "a,b\nx,\"y,z\"\n", {}, ["a", "b"], [["x", "y,z"]], 1],
    // Escaped quotes: "" -> ".
    ["doubled quotes unescape", "a,b\n\"he said \"\"hi\"\"\",x\n", {}, ["a", "b"], [["he said \"hi\"", "x"]], 1],
    // Embedded newline inside a quoted field.
    ["quoted field holds a newline", "a,b\n\"line1\nline2\",y\n", {}, ["a", "b"], [["line1\nline2", "y"]], 1],
    // strict:true accepts valid RFC text with multi-line fields.
    ["strict mode accepts valid quoted newlines", "a,b\n\"l1\nl2\",y\n", { strict: true }, ["a", "b"], [["l1\nl2", "y"]], 1],
    // "Rows are shaped to the first parsed row's width (short rows pad with "", long rows truncate)".
    ["short rows pad, long rows truncate", "a,b,c\n1\n2,3,4,5\n", {}, ["a", "b", "c"], [["1", "", ""], ["2", "3", "4"]], 2],
    // hasHeader:false: "headers is [] and every row is a data row".
    ["headerless: every row is data", "1,2\n3,4\n", { hasHeader: false }, [], [["1", "2"], ["3", "4"]], 2],
    // Dialect overrides (contract example uses tab).
    ["tab delimiter", "id\tname\n7\tann\n", { delimiter: "\t" }, ["id", "name"], [["7", "ann"]], 1],
    ["quote char override", "a,b\nx,'y,z'\n", { quote: "'" }, ["a", "b"], [["x", "y,z"]], 1],
    // Empty fields are real fields (missing columns are the padding case above).
    ["empty fields parse as empty strings", "a,b\n,\n", {}, ["a", "b"], [["", ""]], 1],
    // Quoted content is verbatim (spaces are data). (Tuple had a stray `[]`
    // from generation — totalRows must be the number 0.)
    ["quoted spaces are preserved verbatim", "a,\" b\"\n", {}, ["a", " b"], [], 0],
];
for (const [label, text, opts, headers, rows, total] of PARSE) {
    const t = parse(text, opts);
    assertDeepEq([t.headers, t.rows, t.totalRows], [headers, rows, total], label);
}

/* ==========================================================================
 * TABLE 2 — parse() tolerant mode (default, strict:false): never throws on
 * malformed input, "keeps the readable prefix" (d.ts line 1091). The exact
 * prefix content is implementation-defined, so these rows assert the
 * documented STRUCTURE only.
 * ========================================================================== */
const TOLERANT = [
    ["tolerant: garbage after a closing quote does not throw", ["a,b\n\"c\"d,e\n", {}]],
    ["tolerant: unterminated quote does not throw", ["a,\"bc", {}]],
];
for (const [label, args] of TOLERANT) {
    const t = parse(...args); // must not throw
    n++;
    if (!(Array.isArray(t.headers) && Array.isArray(t.rows) && typeof t.totalRows === "number"))
        throw new Error("assertion failed (structure): " + label);
}

/* ==========================================================================
 * TABLE 3 — parse() refusals. Dialect rules (d.ts lines 1085-1088), STRICT
 * option bags (module header, d.ts lines 1077-1080 — the doc's own example
 * message names 'delimeter'), and strict:true syntax errors.
 * ========================================================================== */
const PARSE_REFUSE = [
    // strict mode: "throws a SyntaxError naming the row"; rows are 1-based over the text.
    ["strict: garbage after closing quote throws SyntaxError naming row 2", ["a,b\n\"c\"d,e\n", { strict: true }], SyntaxError, "2"],
    ["strict: unterminated quote at end of text throws", ["a,\"bc", { strict: true }], SyntaxError, null],
    // delimiter: exactly one ASCII char, not `"` / CR / LF, != quote.
    ["delimiter '\"' refused", ["a,b\n", { delimiter: "\"" }], null, null],
    ["delimiter LF refused", ["a,b\n", { delimiter: "\n" }], null, null],
    ["delimiter CR refused", ["a,b\n", { delimiter: "\r" }], null, null],
    ["multi-char delimiter refused", ["a,b\n", { delimiter: ",," }], null, null],
    ["non-ASCII delimiter refused", ["a,b\n", { delimiter: "\u00e9" }], null, null],
    // quote: exactly one ASCII char, not `,` / CR / LF, != delimiter.
    ["quote ',' refused", ["a,b\n", { quote: "," }], null, null],
    ["quote LF refused", ["a,b\n", { quote: "\n" }], null, null],
    ["quote CR refused", ["a,b\n", { quote: "\r" }], null, null],
    ["quote == delimiter refused", ["a;b\n", { delimiter: ";", quote: ";" }], null, null],
    // STRICT bag: the d.ts header's own example key.
    ["unknown option 'delimeter' throws TypeError naming the key", ["a,b\n", { delimeter: ";" }], TypeError, "delimeter"],
    ["unknown option 'strictz' throws TypeError", ["a,b\n", { strictz: 1 }], TypeError, null],
];
for (const [label, args, ErrType, pattern] of PARSE_REFUSE) assertThrows(() => parse(...args), label, ErrType, pattern);

/* ==========================================================================
 * TABLE 4 — stringify(): the RFC 4180 writer; every line ends with \n.
 * Rows are [label, rowsArg, opts, exactOutput]. d.ts lines 1094-1102.
 * ========================================================================== */
const WRITE = [
    // Documented example: quoting kicks in only where needed.
    ["array rows verbatim with minimal quoting (contract example)", [["a", "b"], ["1", "x,y"]], {}, "a,b\n1,\"x,y\"\n"],
    // A quote inside a field: field is quoted and the quote doubled.
    ["embedded quote doubled and field quoted", [["a", "say \"hi\""]], {}, "a,\"say \"\"hi\"\"\"\n"],
    // A newline inside a field: field quoted.
    ["embedded newline quoted", [["l1\nl2"]], {}, "\"l1\nl2\"\n"],
    // With a tab delimiter, the tab is the special char and the comma is not.
    ["tab delimiter quotes tabs, not commas", [["a\tb", "c,d"]], { delimiter: "\t" }, "\"a\tb\"\tc,d\n"],
    // Plain fields are never quoted.
    ["plain field written unquoted", [["plain"]], {}, "plain\n"],
    // Object rows (contract example): header derived from the FIRST row's own keys.
    ["object rows derive header from first row's keys", [{ name: "alice", score: 95 }, { name: "bob", score: 82 }], {}, "name,score\nalice,95\nbob,82\n"],
    // "missing key -> \"\", extra key ignored" (d.ts line 1096).
    ["object rows: missing key writes empty cell, extra key ignored", [{ a: 1, b: 2 }, { a: 3, extra: 9 }], {}, "a,b\n1,2\n3,\n"],
    // "integer-like keys first" (JS own-keys ordering, d.ts line 1096).
    ["object rows: integer-like keys come first", [{ b: 1, 2: "x", 1: "y" }], {}, "1,2,b\ny,x,1\n"],
    // hasHeader:false suppresses the DERIVED header of object rows.
    ["hasHeader:false suppresses the object-row header", [{ a: 1 }], { hasHeader: false }, "1\n"],
    // "no effect on array rows" (d.ts line 1096).
    ["hasHeader:false has no effect on array rows", [["a", "b"]], { hasHeader: false }, "a,b\n"],
    // "Values are coerced to strings" (d.ts line 1096 / module contract).
    ["object values are coerced: true -> 'true'", [{ a: true }], {}, "a\ntrue\n"],
    ["object values are coerced: null -> 'null'", [{ a: null }], {}, "a\nnull\n"],
    // Quote override: the writer quotes and doubles the OVERRIDE char.
    ["quote override doubles the override char", [["it's"]], { quote: "'" }, "'it''s'\n"],
    // Delimiter override applies to object rows too.
    ["delimiter override applies to object rows", [{ a: 1, b: 2 }], { delimiter: ";" }, "a;b\n1;2\n"],
];
for (const [label, rowsArg, opts, expected] of WRITE) assertEq(stringify(rowsArg, opts), expected, label);

// Round trip: "stringify([t.headers, ...t.rows]) round-trips parse" (d.ts line 1095).
{
    const t = parse("name,score\nalice,95\nbob,\"82,5\"\n");
    const back = parse(stringify([t.headers, ...t.rows]));
    assertDeepEq([back.headers, back.rows, back.totalRows], [t.headers, t.rows, t.totalRows], "stringify([headers, ...rows]) round-trips parse");
}

/* ==========================================================================
 * TABLE 3b — strict-mode row numbering is 1-based over the text (d.ts line
 * 1083): parametric over the row the garbage sits on.
 * ========================================================================== */
for (const badRow of [1, 2, 3]) {
    const text = ["ok", "ok", "ok"].slice(0, badRow - 1).map((r) => r + ",ok").join("\n") + (badRow > 1 ? "\n" : "") + '"garbage"after\n';
    let msg = "";
    try { parse(text, { strict: true }); } catch (e) { msg = String(e); }
    n++;
    if (!msg.includes(String(badRow))) throw new Error("assertion failed (row number): strict SyntaxError names row " + badRow);
}

/* ==========================================================================
 * TABLE 3c — valid single-character dialects are accepted (d.ts lines
 * 1085-1088 constraints are on the REFUSED set; these are the door openers).
 * ========================================================================== */
for (const d of [";", "|", "\t"]) {
    const t = parse("a" + d + "b\n1" + d + "2\n", { delimiter: d });
    assertDeepEq([t.headers, t.rows, t.totalRows], [["a", "b"], [["1", "2"]], 1], "delimiter '" + (d === "\t" ? "\\t" : d) + "' accepted");
}

/* ==========================================================================
 * TABLE 3d — more RFC 4180 corner rows ([label, text, opts, triple]).
 * ========================================================================== */
const PARSE_EDGE2 = [
    ["quoted empty string is an empty field", "a,b\n\"\",x\n", {}, [["a", "b"], [["", "x"]], 1]],
    ["escaped quote at the end of a field", "a,b\n\"x\"\"\",y\n", {}, [["a", "b"], [["x\"", "y"]], 1]],
    ["embedded newline in a later row keeps earlier rows intact", "a,b\n1,2\n\"p\nq\",z\n", {}, [["a", "b"], [["1", "2"], ["p\nq", "z"]], 2]],
    ["BOM + CRLF + non-default delimiter combine", "\uFEFFa;b\r\n1;2\r\n", { delimiter: ";" }, [["a", "b"], [["1", "2"]], 1]],
];
for (const [label, text, opts, triple] of PARSE_EDGE2) {
    const t = parse(text, opts);
    assertDeepEq([t.headers, t.rows, t.totalRows], triple, label);
}

/* ==========================================================================
 * TABLE 4b — parametric dialect table: one (delimiter, quote) pair per row,
 * exercised through BOTH doors (parse and stringify) — parity rows.
 * ========================================================================== */
const DIALECTS = [
    ["tab / doublequote", "\t", '"'],
    ["semicolon / apostrophe", ";", "'"],
    ["pipe / backtick", "|", "`"],
];
for (const [label, delim, quote] of DIALECTS) {
    const opts = { delimiter: delim, quote };
    // Field value `needs<q>quote`: inside a quoted field the quote is DOUBLED
    // once ("" -> "), so the text carries quote+quote, not four.
    const text = "h1" + delim + "h2\nv1" + delim + quote + "needs" + quote + quote + "quote" + quote + "\n";
    const t = parse(text, opts);
    assertDeepEq([t.headers, t.rows, t.totalRows], [["h1", "h2"], [["v1", "needs" + quote + "quote"]], 1], "dialect parse: " + label);
    const out = stringify([["v1", "needs" + quote + "quote"]], { delimiter: delim, quote, hasHeader: false });
    assertEq(out, "v1" + delim + quote + "needs" + quote + quote + "quote" + quote + "\n", "dialect write: " + label);
}

/* ==========================================================================
 * TABLE 4c — more parse boundary rows ([label, text, opts, triple]).
 * ========================================================================== */
const PARSE_EDGE = [
    ["header-only table has zero data rows", "a,b\n", {}, [["a", "b"], [], 0]],
    ["single-cell table", "solo\n", {}, [["solo"], [], 0]],
    ["header field with embedded comma", "\"a,b\",c\n1,2\n", {}, [["a,b", "c"], [["1", "2"]], 1]],
    ["BOM stripped in headerless mode too", "\uFEFF1,2\n", { hasHeader: false }, [[], [["1", "2"]], 1]],
];
for (const [label, text, opts, triple] of PARSE_EDGE) {
    const t = parse(text, opts);
    assertDeepEq([t.headers, t.rows, t.totalRows], triple, label);
}

/* ==========================================================================
 * TABLE 4c — parametric round-trip matrix: for each dialect, a table carrying
 * every quoting hazard (delimiter, quote char, newline) must survive
 * stringify -> parse byte-faithfully (the writer's contract, d.ts line 1094).
 * ========================================================================== */
const RT_DIALECTS = [[",", '"'], ["\t", '"'], [";", "'"], ["|", "`"]];
for (const [delim, quote] of RT_DIALECTS) {
    const opts = { delimiter: delim, quote };
    const table = [["h1", "h2"], ["v" + delim + "1", "needs " + quote + "quote"], ["line1\nline2", "plain"]];
    const back = parse(stringify(table, opts), opts);
    assertDeepEq([back.headers, back.rows, back.totalRows], [["h1", "h2"], [["v" + delim + "1", "needs " + quote + "quote"], ["line1\nline2", "plain"]], 2],
        "round trip through dialect " + JSON.stringify(delim) + "/" + JSON.stringify(quote));
}

/* ==========================================================================
 * TABLE 4d — strict-mode parity: strict:true must accept every VALID table
 * the tolerant default accepts (strict only adds refusals, d.ts line 1091).
 * ========================================================================== */
const STRICT_OK = [
    ["strict accepts the basic table", "name,score\nalice,95\nbob,82\n", {}],
    ["strict accepts CRLF", "a,b\r\n1,2\r\n", {}],
    ["strict accepts a BOM", "\uFEFFa,b\n1,2\n", {}],
    ["strict accepts headerless text", "1,\"x,y\"\n", { hasHeader: false }],
    ["strict accepts doubled quotes", "a,b\n\"\"\"q\"\"\",y\n", {}],
];
for (const [label, text, base] of STRICT_OK) {
    const a = parse(text, base);
    const b = parse(text, { ...base, strict: true });
    assertDeepEq([b.headers, b.rows, b.totalRows], [a.headers, a.rows, a.totalRows], label);
}

/* ==========================================================================
 * TABLE 4e — remaining writer/parser boundary rows.
 * ========================================================================== */
const EDGE3 = [
    ["writer quotes a CR-bearing field (CR is structural)", [["a\rb"]], {}, "\"a\rb\"\n"],
    ["writer emits an empty trailing cell", [["a", ""]], {}, "a,\n"],
    ["parse reads consecutive delimiters as empty middle fields", "a,b,c\n1,,3\n", {}, [["a", "b", "c"], [["1", "", "3"]], 1]],
    ["parse rows.length == totalRows (data rows, header excluded)", "a,b\n1,2\n3,4\n5,6\n", {}, (t) => t.rows.length === t.totalRows && t.totalRows === 3],
];
for (const [label, a, b, c] of EDGE3) {
    if (typeof c === "function") { n++; if (!c(parse(a, b))) throw new Error("assertion failed (predicate): " + label); }
    else if (typeof a === "string") { const t = parse(a, b); assertDeepEq([t.headers, t.rows, t.totalRows], c, label); }
    else assertEq(stringify(a, b), c, label);
}

/* ==========================================================================
 * TABLE 5 — stringify() refusals: mixed forms and the STRICT option bag.
 * ========================================================================== */
const WRITE_REFUSE = [
    ["mixing array and object rows is refused", [["a"], { a: 1 }]],
    ["mixing object first and array second is also refused", [{ a: 1 }, ["a"]]],
    ["unknown option 'delimeter' throws TypeError naming the key", [[["a"]], { delimeter: ";" }], TypeError, "delimeter"],
    ["unknown option 'strict' throws (not a stringify option per d.ts)", [[["a"]], { strict: true }], TypeError, "strict"],
];
for (const [label, args, ErrType, pattern] of WRITE_REFUSE) assertThrows(() => stringify(...args), label, ErrType, pattern);

print("bb_csv: all tests passed (" + n + " assertions)");
