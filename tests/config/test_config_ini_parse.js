// covers: dyna:config.INI.parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_config_ini_parse");
import { TOML, INI, Env, FrontMatter } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

__kitS.section("a = 1 b = two", () => {
    const o = INI.parse("a = 1\nb = two\n");
    eq(o.a, "1", "INI scalar a");
    eq(o.b, "two", "INI scalar b");
});

eq(INI.parse("").a, undefined, "INI empty input");

eq(Object.keys(INI.parse("")).length, 0, "INI empty input has no keys");

__kitS.section("; a comment # another k = v", () => {
    const o = INI.parse("; a comment\n# another\n\nk = v\n");
    eq(o.k, "v", "INI skips comments and blanks");
    eq(Object.keys(o).length, 1, "INI comments produce no keys");
});

__kitS.section("top = 0 [s] x = 1 [s.deep] y = 2", () => {
    const o = INI.parse("top = 0\n[s]\nx = 1\n[s.deep]\ny = 2\n");
    eq(o.top, "0", "INI root key before any section");
    eq(o.s.x, "1", "INI section key");
    eq(o.s.deep.y, "2", "INI dotted section nests");
});

__kitS.section("a = \" padded \" b = bare c = \"tab\\there\" d = 'raw'", () => {
    const o = INI.parse('a = "  padded  "\nb =   bare   \nc = "tab\\there"\nd = \'raw\'\n');
    eq(o.a, "  padded  ", "INI double-quoted value keeps padding");
    eq(o.b, "bare", "INI bare value is trimmed");
    eq(o.c, "tab\there", "INI expands \\t inside double quotes");
    eq(o.d, "raw", "INI single quotes work too");
});

__kitS.section("flag list[] = a list[] = b", () => {
    const o = INI.parse("flag\nlist[] = a\nlist[] = b\n");
    eq(o.flag, true, "INI bare key is boolean true");
    assert(Array.isArray(o.list), "INI key[] builds an array");
    eq(o.list.length, 2, "INI array length");
    eq(o.list[0], "a", "INI array element 0");
    eq(o.list[1], "b", "INI array element 1");
});

eq(INI.parse("url = http://h/?a=1&b=2\n").url, "http://h/?a=1&b=2",
   "INI splits on the first = only");

__kitS.section("[unterminated good = 1 [ok] x = 2", () => {
    const o = INI.parse("[unterminated\ngood = 1\n[ok]\nx = 2\n");
    eq(o.good, "1", "INI survives an unterminated section header");
    eq(o.ok.x, "2", "INI keeps parsing after malformed input");
});

__kitS.section("[s]r k = vr", () => {
    const o = INI.parse("[s]\r\nk = v\r\n");
    eq(o.s.k, "v", "INI handles CRLF");
});

__kitS.section("__proto__ = zzz", () => {
    const o = INI.parse("__proto__ = zzz\n");
    assert(Object.prototype.hasOwnProperty.call(o, "__proto__"),
           "INI writes __proto__ as an OWN property");
    eq(o.__proto__, "zzz", "INI __proto__ reads back as the value");
    eq(({}).polluted, undefined, "INI did not pollute Object.prototype");
});

__kitS.section("[__proto__] polluted = yes", () => {
    const o = INI.parse("[__proto__]\npolluted = yes\n");
    eq(({}).polluted, undefined, "INI section named __proto__ pollutes nothing");
});

__kitS.section("INI caps section nesting depth", () => {
    let threw = false;
    try { INI.parse("[" + "a.".repeat(40) + "b]\nk=1\n"); }
    catch (e) { threw = e instanceof RangeError; }
    assert(threw, "INI caps section nesting depth");
});

__kitS.section("uFEFF[s] k = v", () => {
    const o = INI.parse("\uFEFF[s]\nk = v\n");
    eq(o.s.k, "v", "INI strips a leading BOM");
    const e = Env.parse("\uFEFFA=1\n");
    eq(e.A, "1", "Env strips a leading BOM");
});
__kitS.end();

if (fails) {
    print("test_config: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_config failed");
}
print("test_config: " + n + " assertions, 0 failures");
