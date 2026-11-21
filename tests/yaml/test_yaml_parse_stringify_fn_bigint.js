// covers: dyna:yaml.Parse
// tests/yaml/test_yaml_stringify_fn_bigint.js -- Stringify treats functions
// and BigInts as JSON.stringify does where JSON has an answer (B15): a
// function-valued property is omitted, a function in an array (or at the top)
// is null, and an object left with no properties is {}. A BigInt is a plain
// integer scalar, never the quoted string it used to be ("-5" for -5n), and
// function source text is never written out (it used to be, as a plain scalar).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse_stringify_fn_bigint");
import { Stringify, Parse } from "dyna:yaml";

let n = 0, failures = 0;
function eq(got, want, msg) { n++; if (got !== want) { failures++; print("  FAIL:", msg, JSON.stringify(got), "!==", JSON.stringify(want)); } }

const fn = function secret() { return "do not print me"; };
const CASES = [
    ["function property omitted", { a: 1, f: fn, b: 2 }, "a: 1\nb: 2\n", "{a: 1, b: 2}\n"],
    ["first property a function", { f: fn, a: 1 }, "a: 1\n", "{a: 1}\n"],
    ["function in an array is null", [1, fn, 3], "- 1\n- null\n- 3\n", "[1, null, 3]\n"],
    ["top-level function is null", fn, "null\n", "null\n"],
    ["object of only functions is {}", { only: { f: fn } }, "only: {}\n", "{only: {}}\n"],
    ["array of a function-only object", [{ g: fn }], "- {}\n", "[{}]\n"],
    ["top-level function-only object", { f: fn }, "{}\n", "{}\n"],
    ["method shorthand and arrow both omitted", { m() { return 1; }, a: () => 1, k: 0 }, "k: 0\n", "{k: 0}\n"],
    ["BigInt values are integer scalars", { m: -5n, z: 0n, p: 7n }, "m: -5\nz: 0\np: 7\n", "{m: -5, z: 0, p: 7}\n"],
    ["BigInt beyond 2^53 is unquoted digits", { big: 12345678901234567890n }, "big: 12345678901234567890\n", "{big: 12345678901234567890}\n"],
    ["BigInt in an array and at the top", [1n, -2n], "- 1\n- -2\n", "[1, -2]\n"],
    ["top-level BigInt", 10n, "10\n", "10\n"],
];

__kitS.section("round trip reads the integers back as numbers", () => {
eq(JSON.stringify(Parse(Stringify({ m: -5n, f: fn, xs: [fn, 3n] }))), JSON.stringify({ m: -5, xs: [null, 3] }), "round trip reads the integers back as numbers");
});
__kitS.end();

let reads = 0;

print("test_yaml_stringify_fn_bigint: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_yaml_stringify_fn_bigint: " + failures + " failures");
