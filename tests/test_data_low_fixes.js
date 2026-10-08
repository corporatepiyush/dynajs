// timeout: 180
import { JSON5Parse } from "dyna:encoding";
import { ULIDTime } from "dyna:uuid";
import { Parse as YAMLParse, ParseAll as YAMLParseAll } from "dyna:yaml";
import { Schema } from "dyna:schema";
import { DataFrame } from "dyna:dataframe";
import { IntervalTree } from "dyna:structures";
import { Matcher } from "dyna:matcher";
import { Range, parse as parseSemver } from "dyna:semver";
import { RRule, parseDate } from "dyna:time";
import { Bytes } from "dyna:bytes";
import { sniffType } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;

print("== COMPAT-225/226: JSON5 number grammar ==");
{
    for (const bad of [".", "+", "-", "+.", "-.", ".e5", "+e1"])
        throws(() => JSON5Parse(bad), "JSON5 " + JSON.stringify(bad) + " is refused", /bad number/);
    ok(JSON5Parse(".5") === 0.5, "JSON5 .5 parses", String(JSON5Parse(".5")));
    ok(JSON5Parse("5.") === 5, "JSON5 5. parses", String(JSON5Parse("5.")));
    ok(JSON5Parse("5.e2") === 500, "JSON5 5.e2 parses", String(JSON5Parse("5.e2")));
    ok(JSON5Parse("0x20000000000001") === 9007199254740992,
        "a hex literal above 2^53 rounds instead of throwing",
        String(JSON5Parse("0x20000000000001")));
}

print("");
print("== COMPAT-194: Crockford aliases in ULIDTime ==");
{
    const canon = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
    const ms = ULIDTime(canon);
    ok(typeof ms === "number" && ms > 0, "the canonical ULID parses", String(ms));
    ok(ULIDTime("0LARZ3NDEKTSV4RRFFQ69G5FAV") === ms, "I/L alias for 1 decodes");
    ok(ULIDTime("01ARZ3NDEKTSV4RRFFQ69G5FAV".replace("0", "O")) === ms, "O alias for 0 decodes");
}

print("");
print("== COMPAT-228: flow mapping colon separator ==");
{
    const seq = YAMLParse("{a:[1,2]}");
    ok(seq.a && seq.a.length === 2 && seq.a[1] === 2,
        "a nested flow sequence after : parses", JSON.stringify(seq));
    const deep = YAMLParse("{a:{b:1}}");
    ok(deep.a && deep.a["b:1"] === null,
        "a nested flow mapping after : parses; an unseparated colon stays in the key",
        JSON.stringify(deep));
    ok(YAMLParse("{a:[]}").a.length === 0, "an empty nested sequence parses");
    ok(Object.keys(YAMLParse("{a:{}}").a).length === 0, "an empty nested mapping parses");
    const nv = YAMLParse("{a:b}");
    ok(nv["a:b"] === null,
        "an unseparated colon is part of a null-valued key (PyYAML parity)", JSON.stringify(nv));
    const q = YAMLParse('{"a:b": 1}');
    ok(q["a:b"] === 1, "a quoted flow key may contain a colon", JSON.stringify(q));
    const good = YAMLParse("{a: 1, b: [2, 3]}");
    ok(good.a === 1 && good.b.length === 2, "spaced flow mappings still parse", JSON.stringify(good));
    throws(() => YAMLParse("{a}"), "a flow key with no colon at all is still refused", /colon/);
    throws(() => YAMLParseAll("a: 1\n  ---\nb: 2"),
        "an indented document marker does not split the document", /indentation|marker/);
}

print("");
print("== SEC-049: new Matcher() has no stale argv read ==");
{
    throws(() => new Matcher(), "new Matcher() throws instead of reading a stale slot", /requires a pattern/);
    ok(new Matcher("ab").test("xabx") === true, "a normal Matcher still works");
}

print("");
print("== COMPAT-239: schema numeric keywords must be finite ==");
{
    throws(() => Schema.compile({ minimum: NaN }), "minimum: NaN is refused", /finite/);
    throws(() => Schema.compile({ maximum: Infinity }), "maximum: Infinity is refused", /finite/);
    ok(Schema.validate({ minimum: 1 }, 2).valid === true, "a finite bound still works");
}

print("");
print("== COMPAT-244/245/247: dataframe numeric guards ==");
{
    const f = new DataFrame({ v: new Int32Array([1, 2, 3, 4]) });
    throws(() => f.RESAMPLE("v", Infinity, "sum"), "RESAMPLE(Infinity) is refused", /finite/);
    ok(f.SLICE(-0.5).ROWS === 4, "SLICE(-0.5) truncates to 0 like Array.prototype.slice",
        String(f.SLICE(-0.5).ROWS));
    ok(f.SLICE(-1.5).ROWS === 1, "SLICE(-1.5) drops one row like Array.prototype.slice",
        String(f.SLICE(-1.5).ROWS));
    const wide = new DataFrame({ v: new Float64Array([-1e308, 0, 1e308]) });
    throws(() => wide.HISTOGRAM("v", 4), "HISTOGRAM over an overflowing span is refused", /too wide/);
}

print("");
print("== COMPAT-242: explicit undefined weight column ==");
{
    const f = new DataFrame({ v: new Float64Array([3, 1, 2]), w: new Float64Array([1, 5, 2]) });
    const a = f.TOP_K_WEIGHTED("v", undefined, 2);
    ok(a.keys && a.keys.length === 2, "TOP_K_WEIGHTED(col, undefined, k) resolves k from argv[2]",
        JSON.stringify(a.keys));
    const b = f.TOP_K_WEIGHTED("v", "w", 1);
    ok(b.keys.length === 1 && b.keys[0] === 1, "an explicit weight column still works",
        JSON.stringify(b.keys));
    throws(() => f.TOP_K_WEIGHTED("v", undefined, 65537), "an out-of-range k is still refused", /k must be/);
}

print("");
print("== COMPAT-252: reversed intervals are refused ==");
{
    const t = new IntervalTree();
    throws(() => t.insert(10, 1, "x"), "insert(lo > hi) is refused", /lo must not exceed/);
    t.insert(1, 10, "ok");
    ok(t.size === 1, "a normal interval still inserts", String(t.size));
}

print("");
print("== COMPAT-254/255: semver whitespace and numeric-id overflow ==");
{
    ok(new Range(">=1.0.0\n<2.0.0").test("1.5.0"), "a newline-separated range evaluates");
    throws(() => parseSemver("1.0.0-18446744073709551617"), "an overflowing numeric prerelease id is refused",
        /invalid|range|version/);
}

print("");
print("== COMPAT-256/257: RRule horizon and COUNT+UNTIL ==");
{
    throws(() => RRule.fromString("RRULE:FREQ=DAILY;COUNT=3;UNTIL=20240101T000000Z"),
        "COUNT+UNTIL in an RRULE string is refused", /COUNT and UNTIL/);
    throws(() => new RRule({ freq: "DAILY", count: 3, until: E(2024, 1, 1) }),
        "COUNT+UNTIL in the options object is refused", /COUNT and UNTIL/);
    const yearly = new RRule({ freq: "YEARLY", dtstart: E(2024, 1, 1) });
    ok(yearly.between(E(2024, 1, 1), E(2030, 1, 1)).length === 5,
        "an in-horizon range still returns its occurrences");
    throws(() => yearly.between(E(2024, 1, 1), E(12000, 1, 1)),
        "a range past year 9999 reports exhaustion instead of truncating", /budget exhausted/);
}

print("");
print("== COMPAT-258: Temporal parseDate sign grammar ==");
{
    throws(() => parseDate("+2024-01-01"), "a sign with a 4-digit year is refused", /ISO 8601/);
    throws(() => parseDate("-0001-01-01"), "a negative 4-digit year is refused", /ISO 8601/);
    ok(String(parseDate("+002024-01-01")) === "2024-01-01", "a signed 6-digit year parses",
        String(parseDate("+002024-01-01")));
}

print("");
print("== SEC-175 / OPT-036: byte helpers ==");
{
    const p = new Proxy([], { get(t, k) { if (k === "length") return 9007199254740992; return new Uint8Array(1); } });
    throws(() => Bytes.concat(p), "Bytes.concat with a huge proxy length is refused", /list length/);
    ok(Bytes.concat([new Uint8Array([1, 2]), new Uint8Array([3])]).length === 3,
        "a normal concat still works");
    const big = new Uint8Array(1 << 20).fill(65);
    ok(sniffType(big) === "text/plain", "sniffType caps its NUL scan at the head window", sniffType(big));
}

print("");
print("test_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
