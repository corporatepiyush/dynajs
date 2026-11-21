// covers: dyna:csv.stringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_stringify_parse");
import { parse, stringify, CSVFile } from "dyna:csv";
import { makeTempDir, removeAll, writeFile } from "dyna:file";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function eq(a, b, msg) { assert(JSON.stringify(a) === JSON.stringify(b), msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, msg) { let t = false; try { fn(); } catch { t = true; } assert(t, msg); }
function throwsMsg(fn, re, msg) { let m = ""; try { fn(); } catch (e) { m = String(e.message); } assert(re.test(m), msg + " (got: " + m + ")"); }

__kitS.section("empty rows -> empty string", () => {
    eq(stringify([]), "", "empty rows -> empty string");
    eq(stringify([["a", "b"], ["1", "2"]]), "a,b\n1,2\n", "array rows verbatim");
    eq(stringify([["x,y", 'q"q', "nl\nhere"]]), '"x,y","q""q","nl\nhere"\n', "minimal quoting of delimiter/quote/newline");
    eq(stringify([{ b: "2", a: "1" }, { a: "3" }]), "b,a\n2,1\n,3\n", "object rows: first-row key order, missing key -> empty");
    eq(stringify([{ a: "1" }], { hasHeader: false }), "1\n", "hasHeader:false suppresses the derived header");
    const src = 'name,meta\n"comma, here","line1\nline2"\n"quote ""q""",plain\n';
    const t1 = parse(src);
    const back = stringify([t1.headers, ...t1.rows]);
    eq(back, src, "roundtrip text identical (quotes/commas/newlines-in-fields)");
    eq(parse(back), t1, "roundtrip data identical");
    eq(parse(stringify([{ a: "1", b: "2" }])).rows, [["1", "2"]], "object form roundtrips");
    throwsMsg(() => stringify([["1"], { a: "2" }]), /row 1: rows must all be arrays/, "object after array rows refused");
    throwsMsg(() => stringify([{ a: "1" }, ["2"]]), /row 1: rows must all be objects/, "array after object rows refused");
    throws(() => stringify("nope"), "rows must be an array");
    throws(() => stringify([42]), "row 0 must be an array or an object");
    throws(() => stringify([[1], 5]), "later non-row refused");
    eq(stringify([[1, null, true]]), "1,null,true\n", "cells are ToString'd");
});
__kitS.end();

print("test_csv_parse: all tests passed (" + n + " assertions)");
