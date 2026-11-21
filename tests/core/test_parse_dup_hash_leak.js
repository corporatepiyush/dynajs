// flags: --std
// covers: c:src/parser/parser.inc.c
// A function with eight or more parameters checks duplicate names through a
// hash table built during parsing. resolve_variables freed the variable hash
// but not the two duplicate-check tables, so every compile of such a function
// leaked them (found by the DUMP_LEAKS build: 1 MB lost by one 60000-parameter
// function). The instrument here is the allocator's own block count.
// Two frees cover the tables: resolve_variables on success, and
// js_free_function_def when a compile fails after the parameters were read;
// the failing-compile cases below are the only ones that reach the second.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_parse_dup_hash_leak");
import * as std from "std";
import { memoryUsage } from "dyna:sys";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };

function compile(src) {
    try { new Function(src); } catch (e) { if (!(e instanceof SyntaxError)) throw e; }
}
function churn(src) {
    compile(src);
    std.gc();
    const m0 = memoryUsage().mallocCount;
    for (let i = 0; i < 3000; i++) compile(src);
    std.gc();
    return memoryUsage().mallocCount - m0;
}
const params = (n, def) => Array.from({ length: n }, (_, i) => def ? "a" + i + "=" + i : "a" + i).join(",");

__kitS.section("return function", () => {
ok(churn("return function(" + params(24, true) + "){}") < 4, "3000 compiles of a 24-default-parameter function leave no blocks behind");
ok(churn("return function(" + params(24, false) + "){ var " + params(24, false).replace(/a/g, "v") + "; }") < 4,
   "3000 compiles of a function with 24 parameters and 24 vars leave no blocks behind");
ok(churn("return function({" + params(12, false) + "}, [" + params(12, false).replace(/a/g, "b") + "]){}") < 4,
   "3000 compiles of a function with 24 destructured parameters leave no blocks behind");
});

__kitS.section("return function (2)", () => {
let synErr = 0;
try { new Function("return function(" + params(24, true) + "){ var " + params(24, false).replace(/a/g, "v") + "; ) }"); } catch (e) { synErr = e instanceof SyntaxError; }
ok(synErr, "the failing-compile source really is a SyntaxError");
ok(churn("return function(" + params(24, true) + "){ var " + params(24, false).replace(/a/g, "v") + "; ) }") < 4,
   "3000 failing compiles after 24 parameters and 24 vars leave no blocks behind");
ok(churn("return function({" + params(12, false) + "}, [" + params(12, false).replace(/a/g, "b") + "]){ ) }") < 4,
   "3000 failing compiles after 24 destructured parameters leave no blocks behind");
});
__kitS.end();

print("test_parse_dup_hash_leak: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_parse_dup_hash_leak failed");
