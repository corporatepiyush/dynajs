// covers: dyna:config.TOML.parse
// P15 policy, opt-in since owner decision D8: with { bigint: true } a TOML integer beyond +-(2^53-1)
// parses to an exact BigInt (the default is a rounded Number, pinned in test_config_toml_parse_d8_lossy_default.js); TOML.stringify writes
// a BigInt in the int64 range as an integer and a Number beyond 2^53 as a float, so both
// round-trip with their type.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_toml_parse_bigint_policy");
import { TOML } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
const show = (v) => typeof v === "bigint" ? v + "n" : JSON.stringify(v);
function same(a, b, msg) { assert(Object.is(a, b), msg + " (got " + show(a) + ", want " + show(b) + ")"); }
function throwsT(fn, msg) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    assert(e instanceof TypeError, msg + " (got " + e + ")");
}

__kitS.section("parse", () => {
    same(TOML.parse("a = 9007199254740991").a, 9007199254740991, "2^53-1 stays a Number");
    same(TOML.parse("a = -9007199254740991").a, -9007199254740991, "-(2^53-1) stays a Number");
    same(TOML.parse("a = 9007199254740993", { bigint: true }).a, 9007199254740993n, "2^53+1 is an exact BigInt");
    same(TOML.parse("a = -9007199254740993", { bigint: true }).a, -9007199254740993n, "-(2^53+1) is an exact BigInt");
    same(TOML.parse("a = 0x7fff_ffff_ffff_ffff", { bigint: true }).a, 9223372036854775807n, "hex int64 max");
    same(TOML.parse("a = 9007199254740993.0").a, 9007199254740992, "a float stays a Number");
});

__kitS.section("stringify round trip", () => {
    for (const v of [9007199254740993n, -(2n ** 63n), 2n ** 63n - 1n])
        same(TOML.parse(TOML.stringify({ a: v }), { bigint: true }).a, v, "BigInt " + v);
    same(TOML.stringify({ a: 2n ** 60n }), "\na = 1152921504606846976", "BigInt is a TOML integer");
    for (const v of [2 ** 60, -(2 ** 60), 2 ** 53, 1e300, 2 ** 53 - 1, -0])
        for (const bigint of [false, true])
            same(TOML.parse(TOML.stringify({ a: v }), { bigint }).a, v, "Number " + v + " bigint:" + bigint);
    same(TOML.stringify({ a: 2 ** 60 }), "\na = 1152921504606847000.0", "Number beyond 2^53 is a float");
    throwsT(() => TOML.stringify({ a: 2n ** 63n }), "BigInt beyond int64 is unsupported");
});

print("test_config_toml_parse_bigint_policy: " + n + " assertions, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
