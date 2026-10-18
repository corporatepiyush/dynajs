// flags: --std
// A function with eight or more parameters checks duplicate names through a
// hash table built during parsing. resolve_variables freed the variable hash
// but not the two duplicate-check tables, so every compile of such a function
// leaked them (found by the DUMP_LEAKS build: 1 MB lost by one 60000-parameter
// function). The instrument here is the allocator's own block count.
import * as std from "std";
import { memoryUsage } from "dyna:sys";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };

function churn(src) {
    new Function(src);
    std.gc();
    const m0 = memoryUsage().mallocCount;
    for (let i = 0; i < 3000; i++) new Function(src);
    std.gc();
    return memoryUsage().mallocCount - m0;
}
const params = (n, def) => Array.from({ length: n }, (_, i) => def ? "a" + i + "=" + i : "a" + i).join(",");

ok(churn("return function(" + params(24, true) + "){}") < 4, "3000 compiles of a 24-default-parameter function leave no blocks behind");
ok(churn("return function(" + params(24, false) + "){ var " + params(24, false).replace(/a/g, "v") + "; }") < 4,
   "3000 compiles of a function with 24 parameters and 24 vars leave no blocks behind");
ok(churn("return function({" + params(12, false) + "}, [" + params(12, false).replace(/a/g, "b") + "]){}") < 4,
   "3000 compiles of a function with 24 destructured parameters leave no blocks behind");

print("test_parse_dup_hash_leak: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_parse_dup_hash_leak failed");
