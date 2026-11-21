// covers: dyna:config.TOML.parse
// Owner decision D8: TOML.parse is lossy by default like JSON.parse -- an integer beyond +-(2^53-1) is the nearest
// Number; { bigint: true } returns the exact BigInt. TOML integers stay int64 (wider is a SyntaxError in both modes).
// A non-object options value, an unknown key or a non-boolean bigint is a TypeError.
import { suite } from "../lib/kit.js";
import { TOML } from "dyna:config";

const t = suite("test_config_toml_parse_d8_lossy_default");
const B = { bigint: true };

t.test("default: rounded Numbers", (a) => {
    a.eq(TOML.parse("a = 9007199254740993").a, 9007199254740992, "2^53+1 -> 2^53");
    a.eq(TOML.parse("a = -9007199254740993").a, -9007199254740992);
    a.eq(TOML.parse("a = 9007199254740991").a, 9007199254740991, "2^53-1 exact");
    a.eq(TOML.parse("a = 0x7fff_ffff_ffff_ffff").a, 9223372036854775807);
    a.eq(TOML.parse("a = -9223372036854775808").a, -9223372036854775808);
    a.eq(TOML.parse("[t]\nx = [9007199254740993]").t.x[0], 9007199254740992, "inside an array");
    a.eq(TOML.parse("a = { b = 9007199254740995 }").a.b, 9007199254740996, "inline table, ties to even");
});

t.test("opt-in { bigint: true }", (a) => {
    a.eq(TOML.parse("a = 9007199254740993", B).a, 9007199254740993n);
    a.eq(TOML.parse("a = -9223372036854775808", B).a, -9223372036854775808n);
    a.eq(TOML.parse("a = 0o777", B).a, 511, "a safe int is a Number");
    a.eq(TOML.parse("a = 9007199254740993", { bigint: false }).a, 9007199254740992);
});

t.test("round trip", (a) => {
    const v = 2n ** 62n + 1n;
    a.eq(TOML.parse(TOML.stringify({ a: v }), B).a, v, "BigInt exact with the flag");
    a.eq(TOML.parse(TOML.stringify({ a: v })).a, Number(v), "BigInt lossy by default");
    for (const n of [2 ** 60, -(2 ** 60), 1e300, -0])
        a.eq(TOML.parse(TOML.stringify({ a: n })).a, n, "Number " + n);
    a.throws(() => TOML.parse("a = 9223372036854775808"), SyntaxError, "beyond int64 still refused");
    a.throws(() => TOML.parse("a = 9223372036854775808", B), SyntaxError, "beyond int64 still refused (bigint)");
});

t.test("bad options are a TypeError", (a) => {
    a.throws(() => TOML.parse("a = 1", { bigint: "yes" }), TypeError);
    a.throws(() => TOML.parse("a = 1", { bigint: 1 }), TypeError);
    a.throws(() => TOML.parse("a = 1", 1), TypeError, "a number bag");
    a.throws(() => TOML.parse("a = 1", { big: true }), TypeError, "unknown key");
    a.eq(TOML.parse("a = 1", null).a, 1, "null bag");
});

await t.run();
