// timeout: 180
// Split from tests/test_data_low_fixes.js: the yaml sections.
import { Parse as YAMLParse, ParseAll as YAMLParseAll } from "dyna:yaml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const throws = (fn, w, re) => {
    try { fn(); ok(false, w, "no throw"); }
    catch (e) { ok(!re || re.test(String(e.message)), w, String(e.message).slice(0, 80)); }
};
const E = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
print("== COMPAT-225/226: JSON5 number grammar ==");
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
print("test_yaml_data_low_fixes: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_yaml_data_low_fixes: " + fail + " of " + (pass + fail) + " assertions FAILED");
