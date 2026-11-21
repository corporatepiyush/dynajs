// covers: dyna:yaml.Parse
// P15 policy, opt-in since owner decision D8 (every Parse here passes { bigint: true }; the lossy
// default is pinned in test_yaml_parse_d8_lossy_default.js): a YAML int beyond +-(2^53-1) (decimal, 0x, 0o; up to 4096 characters) parses to an exact
// BigInt; Stringify writes a Number beyond 2^53 as a float and -0 as -0.0, so a Number
// round-trips as the same Number and a BigInt as the same BigInt.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse_bigint_policy");
import * as Y from "dyna:yaml";
const yaml = { Parse: (t) => Y.Parse(t, { bigint: true }), Stringify: Y.Stringify };

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
const show = (v) => typeof v === "bigint" ? v + "n" : JSON.stringify(v);
function same(a, b, msg) { assert(Object.is(a, b), msg + " (got " + show(a) + ", want " + show(b) + ")"); }

__kitS.section("parse", () => {
    same(yaml.Parse("a: 9007199254740991").a, 9007199254740991, "2^53-1 stays a Number");
    same(yaml.Parse("a: 9007199254740993").a, 9007199254740993n, "2^53+1 is an exact BigInt");
    same(yaml.Parse("a: -9007199254740993").a, -9007199254740993n, "-(2^53+1) is an exact BigInt");
    same(yaml.Parse("a: 0x7fffffffffffffff").a, 9223372036854775807n, "hex int64 max");
    same(yaml.Parse("a: 0o777777777777777777777").a, 9223372036854775807n, "octal int64 max");
    same(yaml.Parse("a: -9223372036854775808").a, -(2n ** 63n), "int64 min");
    same(yaml.Parse("a: -0x8000000000000000").a, -(2n ** 63n), "hex int64 min");
    same(yaml.Parse("a: -9223372036854775809").a, -9223372036854775809n, "beyond int64 min is a BigInt");
    same(yaml.Parse("a: 9223372036854775808").a, 9223372036854775808n, "beyond int64 max is a BigInt");
    same(yaml.Parse("a: -0x10000000000000000").a, -(2n ** 64n), "negative hex beyond int64");
    same(yaml.Parse("a: +0o2000000000000000000000").a, 2n ** 64n, "signed octal beyond int64");
    same(yaml.Parse("a: 0X1FFFFFFFFFFFFFFFF").a, 2n ** 65n - 1n, "uppercase 0X beyond int64");
    same(yaml.Parse("a: 0O2000000000000000000000").a, 2n ** 64n, "uppercase 0O beyond int64");
    same(yaml.Parse("a: 0o80000000000000000000000").a, "0o80000000000000000000000", "an 8 is not an octal digit, even beyond int64");
    same(yaml.Parse("a: " + "9".repeat(4096)).a, 10n ** 4096n - 1n, "4096 digits is the BigInt cap");
    same(yaml.Parse("a: " + "9".repeat(4097)).a, "9".repeat(4097), "over 4096 characters stays a string");
    same(yaml.Parse(yaml.Stringify({ a: "18446744073709551616" })).a, "18446744073709551616", "an int-looking string is quoted and stays a string");
    same(yaml.Parse("a: 9007199254740993.0").a, 9007199254740992, "a float stays a Number");
});

__kitS.section("stringify round trip", () => {
    for (const v of [9007199254740993n, -(2n ** 63n), 2n ** 63n - 1n])
        same(yaml.Parse(yaml.Stringify({ a: v })).a, v, "BigInt " + v);
    for (const v of [2 ** 60, -(2 ** 60), 2 ** 53, 1e300, 2 ** 53 - 1, -0, 0])
        for (const bigint of [false, true])
            same(Y.Parse(Y.Stringify({ a: v }), { bigint }).a, v, "Number " + v + " bigint:" + bigint);
    same(yaml.Stringify({ a: 2 ** 60 }), "a: 1152921504606847000.0\n", "Number beyond 2^53 is a float");
    same(yaml.Stringify({ a: -0 }), "a: -0.0\n", "-0 is a float");
});

print("test_yaml_parse_bigint_policy: " + n + " assertions, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
