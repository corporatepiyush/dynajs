// covers: dyna:yaml.Parse
// Owner decision D8: a core-schema int beyond +-(2^53-1) (decimal, 0x, 0o, also beyond int64 up to 4096
// characters) is the nearest Number by default, like JSON.parse (rounded once from the exact value); before P15
// an int beyond int64 was a plain string, P15 made it a BigInt; { bigint: true } restores the exact BigInt.
// ParseAll and ParseStream take the same option. Bad option types are a TypeError.
import { suite } from "../lib/kit.js";
import { Parse, ParseAll, ParseStream, Stringify } from "dyna:yaml";

const t = suite("test_yaml_parse_d8_lossy_default");
const B = { bigint: true };

t.test("default: rounded Numbers", (a) => {
    a.eq(Parse("9007199254740993"), 9007199254740992, "2^53+1 -> 2^53");
    a.eq(Parse("-9007199254740993"), -9007199254740992);
    a.eq(Parse("0x20000000000001"), 9007199254740992, "hex");
    a.eq(Parse("0o400000000000000001"), 9007199254740992, "octal");
    a.eq(Parse("99999999999999999999999"), 1e23, "beyond int64 is a Number, not a string");
    a.eq(Parse("-0x10000000000000001"), -18446744073709551616, "negative hex beyond int64");
    a.eq(Parse("18446744073709551617"), Number(18446744073709551617n), "rounded once");
    a.eq(Parse("9".repeat(400)), Infinity, "past the double range is Infinity, like JSON.parse");
    a.eq(Parse("9".repeat(4097)), "9".repeat(4097), "over 4096 characters stays a string");
    a.deep(ParseAll("9007199254740993\n---\n1"), [9007199254740992, 1], "ParseAll");
    a.deep([...ParseStream("9007199254740993\n---\n2")], [9007199254740992, 2], "ParseStream");
    a.deep(Parse("[9007199254740993, {k: 9007199254740995}]"), [9007199254740992, { k: 9007199254740996 }], "flow");
});

t.test("opt-in { bigint: true }", (a) => {
    a.eq(Parse("9007199254740993", B), 9007199254740993n);
    a.eq(Parse("99999999999999999999999", B), 99999999999999999999999n);
    a.eq(Parse("12", B), 12, "a safe int is a Number");
    a.deep(ParseAll("9007199254740993\n---\n1", B), [9007199254740993n, 1], "ParseAll");
    a.deep([...ParseStream("9007199254740993", B)], [9007199254740993n], "ParseStream");
    a.eq(Parse("9007199254740993", { bigint: false }), 9007199254740992);
});

t.test("round trip", (a) => {
    for (const v of [2n ** 60n + 1n, -(2n ** 70n) - 1n]) {
        a.eq(Parse(Stringify(v), B), v, "BigInt exact with the flag");
        a.eq(Parse(Stringify(v)), Number(v), "BigInt lossy by default");
    }
    for (const v of [2 ** 60, -(2 ** 60), 2 ** 53, -0, 1e300])
        for (const o of [undefined, B])
            a.eq(Parse(Stringify(v), o), v, "Number " + v);
});

t.test("bad options are a TypeError", (a) => {
    a.throws(() => Parse("1", { bigint: "yes" }), TypeError);
    a.throws(() => ParseAll("1", { bigint: 1 }), TypeError);
    a.throws(() => ParseStream("1", { bigint: null }), TypeError);
    a.throws(() => Parse("1", 5), TypeError, "a number bag");
    a.throws(() => Parse("1", { bigInt: true }), TypeError, "unknown key");
});

await t.run();
