// covers: dyna:yaml.Parse
// dyna:yaml integer options (Parse maxDepth, Stringify width) range-check
// BEFORE the integer conversion: NaN reached (int64_t)NaN, undefined behaviour
// found by the dyna:yaml input sweep under UBSan. Every non-integer must still
// be the documented TypeError; the san stage runs this suite.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse_int_options_nan");
import { Parse, Stringify } from "dyna:yaml";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }

__kitS.section("a: 1", () => {
for (const v of [NaN, Infinity, -Infinity, 0.5, 0, -1, 2 ** 32 + 1])
    ok(/^TypeError: .*maxDepth must be an integer 1\.\./.test(err(() => Parse("a: 1", { maxDepth: v }))), "Parse maxDepth " + v);
});

__kitS.section("a: [1, 2]", () => {
eq(JSON.stringify(Parse("a: [1, 2]", { maxDepth: 3 })), '{"a":[1,2]}', "maxDepth 3 (control)");
});
__kitS.end();

print("test_yaml_int_options_nan: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
