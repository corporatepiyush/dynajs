// dyna:yaml integer options (Parse maxDepth, Stringify width) range-check
// BEFORE the integer conversion: NaN reached (int64_t)NaN, undefined behaviour
// found by the dyna:yaml input sweep under UBSan. Every non-integer must still
// be the documented TypeError; the san stage runs this suite.
import { Parse, Stringify } from "dyna:yaml";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
for (const v of [NaN, Infinity, -Infinity, 0.5, 0, -1, 2 ** 32 + 1])
    ok(/^TypeError: .*maxDepth must be an integer 1\.\./.test(err(() => Parse("a: 1", { maxDepth: v }))), "Parse maxDepth " + v);
for (const v of [NaN, Infinity, -1, 0.5, 65537])
    ok(/^TypeError: .*width/.test(err(() => Stringify({ a: 1 }, { width: v }))), "Stringify width " + v);
eq(JSON.stringify(Parse("a: [1, 2]", { maxDepth: 3 })), '{"a":[1,2]}', "maxDepth 3 (control)");
eq(Stringify({ a: 1 }, { width: 0 }), "a: 1\n", "width 0 (control)");

print("test_yaml_int_options_nan: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
