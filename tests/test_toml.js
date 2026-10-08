import { TOML } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function eqd(a, b, msg) {
    assert(JSON.stringify(a) === JSON.stringify(b),
        msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) { let t = false; try { fn(); } catch (e) { t = true; } assert(t, msg); }
function throwsMsg(fn, substr, msg) {
    let t = false;
    try { fn(); } catch (e) { t = String(e).indexOf(substr) >= 0; }
    assert(t, msg + " (want a throw containing: " + substr + ")");
}

eqd(TOML.parse("a = 1"), { a: 1 }, "valid/key/simple");
eqd(TOML.parse("a.b = 1"), { a: { b: 1 } }, "valid/key/dotted");
{
    const d = TOML.parse('str = "hello"');
    eq(d.str, "hello", "valid/string/simple");
}
{
    const d = TOML.parse('lit = \'C:\\Users\\nodejs\'');
    eq(d.lit, "C:\\Users\\nodejs", "valid/string/literal");
}
{
    const d = TOML.parse('esc = "a\\nb\\t\\"\\u00e9"');
    eq(d.esc, "a\nb\t\"\u00e9", "valid/string/escapes");
}
{
    const d = TOML.parse('ml = """\nline1\nline2\n"""');
    eq(d.ml, "line1\nline2\n", "valid/string/multiline");
}
{
    const d = TOML.parse("ml2 = '''\nraw \\n stays\n'''");
    eq(d.ml2, "raw \\n stays\n", "valid/string/multiline-literal");
}
{
    const d = TOML.parse("hex = 0xdeadBEEF\noct = 0o755\nbin = 0b1101");
    eq(d.hex, 0xdeadbeef, "valid/integer/hex");
    eq(d.oct, 0o755, "valid/integer/octal");
    eq(d.bin, 13, "valid/integer/binary");
}
{
    const d = TOML.parse("big = 9_223_372_036_854_775_807\nneg = -17\npi = 3.14\nexp = 5e22");
    eq(d.big, 9223372036854775807, "valid/integer/underscores (INT64_MAX)");
    eq(d.neg, -17, "valid/integer/negative");
    eq(d.pi, 3.14, "valid/float/simple");
    eq(d.exp, 5e22, "valid/float/exponent");
}
{
    const d = TOML.parse("t = true\nf = false\ninf = inf\nnin = nan");
    eq(d.t, true, "valid/boolean/simple");
    eq(d.f, false, "valid/boolean/simple");
    eq(d.inf, Infinity, "valid/float/inf");
    assert(Number.isNaN(d.nin), "valid/float/nan");
}
{
    const d = TOML.parse("dt1 = 1979-05-27T07:32:00Z\ndt2 = 1979-05-27T00:32:00-07:00\ndt3 = 1979-05-27 07:32:00Z\nld = 1979-05-27\nlt = 07:32:00\nltf = 00:32:00.999999\nldt = 1979-05-27T07:32:00");
    eq(d.dt1, "1979-05-27T07:32:00Z", "valid/datetime/utc");
    eq(d.dt2, "1979-05-27T00:32:00-07:00", "valid/datetime/offset");
    eq(d.dt3, "1979-05-27 07:32:00Z", "valid/datetime/space separator");
    eq(d.ld, "1979-05-27", "valid/datetime/local-date");
    eq(d.lt, "07:32:00", "valid/datetime/local-time");
    eq(d.ltf, "00:32:00.999999", "valid/datetime/local-time fraction");
    eq(d.ldt, "1979-05-27T07:32:00", "valid/datetime/local-date-time");
}
{
    const d = TOML.parse("[server]\nhost = \"x\"\n[server.ssl]\nenabled = true\n[[list]]\na = 1\n[[list]]\na = 2");
    eq(d.server.host, "x", "valid/table/simple");
    eq(d.server.ssl.enabled, true, "valid/table/dotted header");
    eq(d.list.length, 2, "valid/spec/array-of-tables");
    eq(d.list[1].a, 2, "valid/spec/array-of-tables (2)");
}
{
    const d = TOML.parse("a = [1, 2, 3]\nb = [\"x\", \"y\"]\nc = [{ p = 1 }, { p = 2 }]\nempty = []\nmulti = [\n  1, # one\n  2,\n]");
    eqd(d.a, [1, 2, 3], "valid/array/simple");
    eqd(d.c, [{ p: 1 }, { p: 2 }], "valid/array/inline-table");
    eq(d.empty.length, 0, "valid/array/empty");
    eq(d.multi.length, 2, "valid/array/multiline + trailing comma");
}
{
    const d = TOML.parse("inline = { x = 1, y = \"z\" }");
    eqd(d.inline, { x: 1, y: "z" }, "valid/inline-table/simple");
}
{
    const d = TOML.parse("a = 1\r\nb = 2\r\n");
    eq(d.b, 2, "valid/spec/crlf");
}

throwsMsg(() => TOML.parse("a = 1\na = 2"), "duplicate key", "invalid/key/duplicate");
throwsMsg(() => TOML.parse("a = 2\na.b = 1"), "dotted key crosses a value", "invalid/key/dotted-key-conflict");
throwsMsg(() => TOML.parse("a = 1\n[a]"), "conflicts with a value", "invalid/table/redefine");
throwsMsg(() => TOML.parse("a = \"unterminated"), "unterminated string", "invalid/string/unterminated");
throwsMsg(() => TOML.parse("a = 'x\n'"), "unterminated literal", "invalid/string/literal-newline");
throwsMsg(() => TOML.parse("a = \"x\\q\""), "invalid escape", "invalid/string/bad-escape");
throwsMsg(() => TOML.parse("a = \"x\u0001\""), "control character", "invalid/string/control-char");
throwsMsg(() => TOML.parse("a = \"\\ud800\""), "scalar value", "invalid/string/surrogate-escape");
throwsMsg(() => TOML.parse("a = 1_"), "underscore placement", "invalid/integer/trailing-underscore");
throwsMsg(() => TOML.parse("a = 08"), "leading zero", "invalid/integer/leading-zero");
throwsMsg(() => TOML.parse("a = 0x2g"), "malformed number", "invalid/integer/bad-hex");
throwsMsg(() => TOML.parse("a = 1..2"), "malformed float", "invalid/float/double-dot");
throwsMsg(() => TOML.parse("a = 1e"), "no digit after the exponent", "invalid/float/bare-exponent");
throwsMsg(() => TOML.parse("a = 1."), "no digit after the dot", "invalid/float/trailing-dot");
throwsMsg(() => TOML.parse("a = 1.0e"), "no digit after the exponent", "invalid/float/trailing-exponent");
throwsMsg(() => TOML.parse("a = 01.5"), "leading zero", "invalid/float/leading-zero");
throwsMsg(() => TOML.parse("a = .5"), "malformed float", "invalid/float/leading-dot");
throwsMsg(() => TOML.parse("a = 2024-13-01T00:00:00Z"), "invalid date-time", "invalid/datetime/month-13");
throwsMsg(() => TOML.parse("a = 2024-01-01T24:00:00Z"), "invalid date-time", "invalid/datetime/hour-24");
throwsMsg(() => TOML.parse("a = 2024-01-01T00:00:00+"), "invalid date-time", "invalid/datetime/dangling-zone");
throwsMsg(() => TOML.parse("a = 2024-01-01T00:00:00."), "invalid date-time", "invalid/datetime/empty-fraction");
throwsMsg(() => TOML.parse("a = "), "expected a value", "invalid/key/empty-value");
throwsMsg(() => TOML.parse("a = [1, 2"), "expected ',' or ']'", "invalid/array/unterminated");
throwsMsg(() => TOML.parse("[a"), "malformed table header", "invalid/table/unterminated");
throwsMsg(() => TOML.parse("= 1"), "expected key = value", "invalid/key/empty");
throwsMsg(() => TOML.parse("[a]\n[a]"), "already defined", "invalid/table/duplicate-header");
throws(() => TOML.parse(42), "a non-string input is a TypeError");

{
    const deep63 = "a = " + "[".repeat(63) + "1" + "]".repeat(63);
    eqd(TOML.parse(deep63).a, (() => { let v = 1; for (let i = 0; i < 63; i++) v = [v]; return v; })(),
        "63 nested arrays parse (the cap is 64)");
    const deep64 = "a = " + "[".repeat(64) + "1" + "]".repeat(64);
    eqd(TOML.parse(deep64).a, (() => { let v = 1; for (let i = 0; i < 64; i++) v = [v]; return v; })(),
        "64 nested arrays parse (the cap is 64)");
    const deep65 = "a = " + "[".repeat(65) + "1" + "]".repeat(65);
    throwsMsg(() => TOML.parse(deep65), "nesting exceeds", "65 nested arrays refuse (N)");
}

{
    eq(TOML.stringify({ a: 1, b: "x", c: [1, 2], d: true }),
       '\na = 1\nb = "x"\nc = [1, 2]\nd = true', "stringify: root object is a document");
    eq(TOML.stringify({ s: "a\nb\"c\\" }),
       '\ns = "a\\nb\\"c\\\\"', "stringify: escapes");
    eq(TOML.stringify({ n: { a: 1 } }), '\nn = {a = 1}', "stringify: nested objects stay inline");
    throws(() => TOML.stringify(3.5), "stringify refuses a scalar root");
    throws(() => TOML.stringify([1, 2]), "stringify refuses an array root");
    throws(() => TOML.stringify(), "stringify requires a value");
    const v = { x: 1, y: [true, "s"], z: { n: 1 } };
    eqd(TOML.parse(TOML.stringify(v)), v, "stringify/parse round trip");

    throwsMsg(() => TOML.parse("x = " + "1".repeat(81)), "integer out of 64-bit",
        "81-digit integer refuses by name");
    throwsMsg(() => TOML.parse("x = " + "1".repeat(127)), "integer out of 64-bit",
        "127-digit integer (max token) refuses by name");
    throwsMsg(() => TOML.parse(("a.").repeat(64) + "b = 1"), "key nesting exceeds 64",
        "65-segment dotted key refuses at the depth cap");
}

{
    const d = TOML.parse("a = +inf\nb = -inf\nc = +nan\nd = -nan");
    eq(d.a, Infinity, "valid/float/signed-inf (+inf)");
    eq(d.b, -Infinity, "valid/float/signed-inf (-inf)");
    assert(Number.isNaN(d.c), "valid/float/signed-nan (+nan)");
    assert(Number.isNaN(d.d), "valid/float/signed-nan (-nan)");
}

{
    eq(TOML.parse("x = 9223372036854775807").x, 9223372036854775807,
       "INT64_MAX parses exactly");
    eq(TOML.parse("x = -9223372036854775808").x, -9223372036854775808,
       "INT64_MIN parses exactly");
    throwsMsg(() => TOML.parse("x = 9223372036854775808"), "integer out of 64-bit",
        "INT64_MAX + 1 refuses");
    throwsMsg(() => TOML.parse("x = -9223372036854775809"), "integer out of 64-bit",
        "INT64_MIN - 1 refuses");
}

{
    const d = TOML.parse('a = """\r\nx\r\ny\r\n"""');
    eq(d.a, "x\ny\n", "CRLF in a multi-line basic string normalises to LF");
    const l = TOML.parse("b = '''\r\np\r\nq'''");
    eq(l.b, "p\nq", "and in a multi-line literal string too");
}

{
    const d = TOML.parse("\uFEFFa = 1");
    eq(d.a, 1, "a BOM-led document parses");
    eqd(Object.keys(d), ["a"], "and the BOM is not glued onto a key");
}

{
    eqd(Object.keys(TOML.parse("a = 1")), ["a"], "parse('a = 1') own keys are exactly [a]");
    const d = TOML.parse("[t]\nx = 1");
    eqd(Object.keys(d), ["t"], "the document shows only user keys");
    eqd(Object.keys(d.t), ["x"], "a header-created table shows only user keys");
    eqd(Object.getOwnPropertyNames(d.t), ["x"], "even getOwnPropertyNames sees no marker");
    eq(TOML.stringify(d).indexOf("__toml_header") < 0, true,
       "stringify never emits the marker");
    eqd(TOML.parse(TOML.stringify(d)), { t: { x: 1 } }, "and the round trip is marker-free");
    eqd(TOML.parse("[a]\n__toml_header = 1"), { a: { __toml_header: 1 } },
        "a user key named like the old marker parses like any key");
}

{
    throwsMsg(() => TOML.parse("a = {x = 1}\n[a]\nb = 2"), "already defined",
        "invalid/table/inline-table-then-header");
    throwsMsg(() => TOML.parse('a.b = 1\n[a]\nc = 2'), "already defined",
        "invalid/table/dotted-key-then-header");
    throwsMsg(() => TOML.parse('[fruit]\napple.color = "red"\n[fruit.apple]'),
              "already defined", "invalid/table/dotted-subtable-then-header");
    throwsMsg(() => TOML.parse("a = {x = 1}\na.y = 2"), "closed inline table",
        "invalid/inline-table/extend-dotted-key");
    throwsMsg(() => TOML.parse("a = {x = 1}\n[a.b]\nz = 1"), "closed inline table",
        "invalid/inline-table/extend-header");
    throwsMsg(() => TOML.parse("t = {i = {x = 1}}\nt.i.y = 2"), "closed inline table",
        "invalid/inline-table/extend-nested");
    throwsMsg(() => TOML.parse("a = {i = {x = 1}, i.z = 2}"), "closed inline table",
        "invalid/inline-table/extend-in-same-literal");
    eqd(TOML.parse("a = {i.b = 1, i.c = 2}"), { a: { i: { b: 1, c: 2 } } },
        "valid/inline-table: dotted pairs in ONE literal share the sub-table");
    eqd(TOML.parse("[x.y]\nz = 1\n[x]\nw = 2"), { x: { y: { z: 1 }, w: 2 } },
        "a super-table defined AFTER its sub-table stays legal");
    eqd(TOML.parse("a.b = 1\n[a.c]\nd = 2"), { a: { b: 1, c: { d: 2 } } },
        "adding a sub-table beside a dotted key stays legal");
    eqd(TOML.parse('[fruit]\napple.color = "red"\n[fruit.apple.texture]\nsmooth = true'),
        { fruit: { apple: { color: "red", texture: { smooth: true } } } },
        "and a sub-table UNDER a dotted-key table stays legal");
}

{
    eq(TOML.stringify(TOML.parse("f = 5.0")), "\nf = 5.0",
       "a parsed 5.0 stringifies as 5.0");
    eq(TOML.parse(TOML.stringify(TOML.parse("f = 5.0"))).f, 5,
       "and it round-trips to the number 5");
    eq(TOML.stringify(TOML.parse("i = 5")), "\ni = 5",
       "an int never grows a .0");
    eq(TOML.stringify(TOML.parse("n = -0.0")), "\nn = -0.0",
       "negative zero keeps its sign");
    throws(() => TOML.stringify({ a: null }),
           "null is refused (TOML 1.0 has no null)");
    throws(() => TOML.stringify({ a: undefined }),
           "undefined is refused (TOML 1.0 has no null)");
    throws(() => TOML.stringify(null), "and a null root");
    eq(JSON.stringify(TOML.parse(TOML.stringify({ a: 1, b: [2, 3] }))),
       JSON.stringify(TOML.parse("a = 1\nb = [2, 3]")),
       "a null-free document still round-trips");
}

{
    const d = TOML.parse("a = 5e+22\nb = 5e-22\nc = 6.626e-34");
    eq(d.a, 5e22, "valid/float/exponent: e+ is a valid float");
    eq(d.b, 5e-22, "valid/float/exponent: e- is a valid float");
    eq(d.c, 6.626e-34, "valid/float/exponent: frac + signed exponent");
}
{
    const d = TOML.parse("a = 0xdead_beef\nb = 0o7_5\nc = 0b11_01");
    eq(d.a, 0xdeadbeef, "valid/integer/hex: underscores between hex digits");
    eq(d.b, 0o75, "valid/integer/octal: underscores");
    eq(d.c, 0b1101, "valid/integer/binary: underscores");
}
eq(TOML.stringify({ a: -Infinity }), "\na = -inf", "stringify: -inf renders whole");
eq(TOML.parse(TOML.stringify({ a: -Infinity })).a, -Infinity, "-inf round trips");
{
    const big = 9.223372036854776e18;
    eq(TOML.parse(TOML.stringify({ a: big })).a, big, "%g float round trips");
}

{
    const d = TOML.parse('"a b" = 1');
    eqd(Object.keys(d), ["a b"], "valid/key/quoted: quoted basic key is unquoted");
    const l = TOML.parse("'lit key' = 1");
    eqd(Object.keys(l), ["lit key"], "valid/key/quoted: literal key");
    const e = TOML.parse('"a\\tb" = 1');
    eqd(Object.keys(e), ["a\tb"], "valid/key/quoted: escapes expand in keys");
    const q = TOML.parse('"" = 1');
    eqd(Object.keys(q), [""], "valid/key/quoted: an empty quoted key is legal");
    eq(TOML.parse('"a.b" = 1')["a.b"], 1, "a quoted dot does not split the key");
    eqd(TOML.parse('a."b c".d = 1'), { a: { "b c": { d: 1 } } },
        "valid/key/dotted: quoted middle segment");
    eqd(TOML.parse('["a b"]\nx = 1'), { "a b": { x: 1 } },
        "valid/table/names: quoted header key");
    eq(TOML.parse('["]"]\nx = 1')["]"].x, 1, "a quote inside a quoted key");
    eqd(TOML.parse('a = {"q k" = 1}'), { a: { "q k": 1 } },
        "valid/inline-table: quoted key");
    eqd(TOML.parse('a = {x.y = 1}'), { a: { x: { y: 1 } } },
        "valid/inline-table: dotted key");
    eqd(TOML.parse(TOML.stringify(TOML.parse('a."b c".d = 1'))),
        { a: { "b c": { d: 1 } } }, "quoted keys round trip through stringify");
}
throwsMsg(() => TOML.parse("a..b = 1"), "expected a key", "invalid/key/empty dotted part");
throwsMsg(() => TOML.parse(".a = 1"), "expected key = value", "invalid/key/leading dot");
throwsMsg(() => TOML.parse("a. = 1"), "expected a key", "invalid/key/trailing dot");
throwsMsg(() => TOML.parse("a b = 1"), "expected '='", "invalid/key/space in bare key");
throwsMsg(() => TOML.parse("[a..b]\nx = 1"), "expected a key", "invalid/table/empty header part");
eqd(TOML.parse("[ a ]\nx = 1"), { a: { x: 1 } },
    "whitespace around a header key is allowed and trimmed");
eqd(TOML.parse("a . b = 1"), { a: { b: 1 } },
    "whitespace around dotted-key separators is allowed");

throwsMsg(() => TOML.parse("a = 1 b = 2"), "expected a newline after a key-value pair",
    "invalid/key/after-value: two pairs on one line");
throwsMsg(() => TOML.parse("a = 1 junk"), "expected a newline after a key-value pair",
    "invalid/key/after-value: trailing junk");
throwsMsg(() => TOML.parse("[a] junk"), "junk after a table header",
    "invalid/table/after-header: trailing junk");

throwsMsg(() => TOML.parse("a = {x = 1,}"), "expected a key in an inline table",
    "invalid/inline-table/trailing-comma");

{
    eq(TOML.parse('a = """a\\ \n   b"""').a, "ab",
       "line-ending backslash trims the next line's indent");
    eq(TOML.parse('a = """a\\\r\n\tb"""').a, "ab",
       "and the CRLF form too");
}
throwsMsg(() => TOML.parse('a = "x\u007Fy"'), "control character",
    "invalid/string/del-char");

{
    const d = TOML.parse('[[fruits]]\nname = "apple"\n[fruits.physical]\ncolor = "red"\nshape = "round"\n[[fruits]]\nname = "banana"\n[fruits.physical]\ncolor = "yellow"');
    eq(d.fruits.length, 2, "valid/table/array-of-tables: two fruits");
    eq(d.fruits[0].physical.color, "red", "sub-table lands in the FIRST fruit");
    eq(d.fruits[1].physical.color, "yellow", "sub-table lands in the LAST fruit");
    throwsMsg(() => TOML.parse('[[f]]\n[f.physical]\n[f.physical]'), "already defined",
        "invalid/table: redefining a sub-table of the same element");
}
{
    let doc = "", i;
    for (i = 0; i < 5000; i++) doc += "[t" + i + "]\nx = 1\n";
    const d = TOML.parse(doc);
    eq(d.t4999.x, 1, "5000 distinct headers parse (hash set)");
    throwsMsg(() => TOML.parse(doc + "[t0]"), "already defined",
        "duplicate detection intact after 5000 marks");
}

if (fails) {
    print("test_toml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_toml failed");
}
print("test_toml: " + n + " assertions, 0 failures");
