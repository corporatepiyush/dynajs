// covers: dyna:yaml.Stringify
// tests/yaml/test_yaml_stringify_fn_bigint.js -- Stringify treats functions
// and BigInts as JSON.stringify does where JSON has an answer (B15): a
// function-valued property is omitted, a function in an array (or at the top)
// is null, and an object left with no properties is {}. A BigInt is a plain
// integer scalar, never the quoted string it used to be ("-5" for -5n), and
// function source text is never written out (it used to be, as a plain scalar).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_stringify_fn_bigint");
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

__kitS.section("(block)", () => {
for (const [label, value, block, flow] of CASES) {
    eq(Stringify(value), block, label + " (block)");
    eq(Stringify(value, { flow: true }), flow, label + " (flow)");
}
});

__kitS.section("a: 2 b: 1", () => {
eq(Stringify({ z: fn, b: 1, a: 2 }, { sortKeys: true }), "a: 2\nb: 1\n", "sortKeys skips functions too");
});

__kitS.section("function source never reaches the output", () => {
eq(/do not print me|secret/.test(Stringify({ f: fn, xs: [fn] }) + Stringify(fn, { flow: true })), false, "function source never reaches the output");
});

let reads = 0;

const obj = { get g() { reads++; return fn; }, k: 1 };

__kitS.section("a getter returning a function is read once and its key omitted", () => {
Stringify(obj);
eq(reads, 1, "a getter returning a function is read once and its key omitted");
});
__kitS.end();

print("test_yaml_stringify_fn_bigint: " + n + " checks, " + failures + " failures");
if (failures) throw new Error("test_yaml_stringify_fn_bigint: " + failures + " failures");
