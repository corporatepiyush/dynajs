// covers: dyna:yaml.Stringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_stringify");
import { Parse, ParseAll, Stringify } from "dyna:yaml";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
const j = (v) => JSON.stringify(v);
function same(text, want, msg) { eq(j(Parse(text)), j(want), msg); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
function refuses(text, needle, msg) {
    let m = "";
    try { Parse(text); } catch (e) { m = String(e.message); }
    assert(m.indexOf(needle) >= 0, msg + " (message: " + m + ")");
}

__kitS.section("a: b", () => {
    const cases = [
        { a: 1, b: "two", c: true, d: null },
        { list: [1, 2, 3], nested: { x: { y: "z" } } },
        [{ name: "a" }, { name: "b" }],
        { empty: [], blank: {}, s: "" },
        { tricky: "yes", also: "no", num: "123", colon: "a: b", hash: "a # b" },
        { spaces: "  padded  ", dash: "-x", multi: "line\nbreak" },
        [[1, 2], [3, 4]],
        { deep: { a: [{ b: [1, { c: 2 }] }] } },
    ];
    let bad = 0;
    for (const c of cases) {
        const text = Stringify(c);
        let back;
        try { back = Parse(text); } catch (e) { back = "THREW: " + e.message; }
        if (j(back) !== j(c)) {
            bad++;
            print("  round trip differs for " + j(c));
            print("    emitted: " + JSON.stringify(text));
            print("    back:    " + j(back));
        }
    }
    assert(bad === 0, "every value round-trips through Stringify (" +
                      (cases.length - bad) + "/" + cases.length + ")");
});

eq(Stringify({ a: 1 }), "a: 1\n", "the simplest document");

eq(Stringify({ a: [1, 2] }), "a:\n  - 1\n  - 2\n", "a sequence under a key");

eq(Stringify({ a: { b: 1 } }), "a:\n  b: 1\n", "a nested mapping");

eq(Stringify([1, 2]), "- 1\n- 2\n", "a top-level sequence");

eq(Stringify({ a: "yes" }), 'a: "yes"\n',
   "a 1.1 boolean word is QUOTED on output, for readers that are not 1.2");

eq(Stringify({ a: "123" }), 'a: "123"\n', "and so is a numeric-looking string");

eq(Stringify({ a: 1 }, { indent: 4 }), "a: 1\n", "indent does not affect a flat map");

eq(Stringify({ a: { b: 1 } }, { indent: 4 }), "a:\n    b: 1\n", "but it does a nested one");

eq(Stringify("scalar"), "scalar\n", "a bare scalar document");

eq(Stringify(null), "null\n", "and null");

throws(() => Stringify(), "a value is required");

throws(() => Stringify({}, { indent: 0 }), "indent must be at least 1");

__kitS.section("a: .nan b: .inf c: -.inf", () => {
    const y = Stringify({ a: NaN, b: Infinity, c: -Infinity });
    eq(y, "a: .nan\nb: .inf\nc: -.inf\n", "non-finite numbers emit as .nan/.inf/-.inf");
    const o = Parse(y);
    assert(Number.isNaN(o.a), ".nan re-parses as NaN (a number, not a string)");
    eq(o.b, Infinity, ".inf re-parses as Infinity");
    eq(o.c, -Infinity, "-.inf re-parses as -Infinity");
});

eq(Stringify({ a: "..." }), 'a: "..."\n', "a document-marker string is quoted");

eq(Stringify({ a: "---" }), 'a: "---"\n', "and so is the other marker");

function throwsMsg(fn, re, msg) {
    let m = "";
    try { fn(); } catch (e) { m = String(e.message); }
    assert(re.test(m), msg + " (got: " + (m || "no throw") + ")");
}

__kitS.section("Stringify bag names the key AND the valid set", () => {
    throwsMsg(() => Stringify({ a: 1 }, { indnt: 4 }),
              /^unknown option "indnt" \(valid: indent, width, sortKeys, flow\)$/,
              "Stringify bag names the key AND the valid set");
    eq(Stringify({ a: { b: 1 } }, { indent: 4 }), "a:\n    b: 1\n",
       "a valid Stringify bag is unchanged");
    throwsMsg(() => Parse("a: 1", { dep: 4 }),
              /^unknown option "dep" \(valid: maxDepth, schema, bigint\)$/,
              "Parse bag is strict too (YA-1, kept green here)");
    eq(j(Stringify({ a: 1 }, 42)), '"a: 1\\n"', "a primitive bag reads as absent");
});
__kitS.end();

if (fails) {
    print("test_yaml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_yaml failed");
}
print("test_yaml: " + n + " assertions, 0 failures");
