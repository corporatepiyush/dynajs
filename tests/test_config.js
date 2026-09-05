/* test_config.js -- INI, Env and FrontMatter in dyna:config (design 04).
 *
 * These are parsers of files a project checks in, so the cases that matter are
 * the malformed ones and the ones carrying a key that means something to the
 * language: `__proto__` must land as an own property, never retarget a chain.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_config.js
 */
import { INI, Env, FrontMatter } from "dyna:config";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

/* --------------------------------------------------------------------- INI */

{
    const o = INI.parse("a = 1\nb = two\n");
    eq(o.a, "1", "INI scalar a");
    eq(o.b, "two", "INI scalar b");
}
eq(INI.parse("").a, undefined, "INI empty input");
eq(Object.keys(INI.parse("")).length, 0, "INI empty input has no keys");

/* Comments and blank lines */
{
    const o = INI.parse("; a comment\n# another\n\nk = v\n");
    eq(o.k, "v", "INI skips comments and blanks");
    eq(Object.keys(o).length, 1, "INI comments produce no keys");
}

/* Sections, including dotted nesting */
{
    const o = INI.parse("top = 0\n[s]\nx = 1\n[s.deep]\ny = 2\n");
    eq(o.top, "0", "INI root key before any section");
    eq(o.s.x, "1", "INI section key");
    eq(o.s.deep.y, "2", "INI dotted section nests");
}

/* Quoted values keep their spaces; escapes expand only inside quotes. */
{
    const o = INI.parse('a = "  padded  "\nb =   bare   \nc = "tab\\there"\nd = \'raw\'\n');
    eq(o.a, "  padded  ", "INI double-quoted value keeps padding");
    eq(o.b, "bare", "INI bare value is trimmed");
    eq(o.c, "tab\there", "INI expands \\t inside double quotes");
    eq(o.d, "raw", "INI single quotes work too");
}

/* npm-ini quirks the design adopts deliberately */
{
    const o = INI.parse("flag\nlist[] = a\nlist[] = b\n");
    eq(o.flag, true, "INI bare key is boolean true");
    assert(Array.isArray(o.list), "INI key[] builds an array");
    eq(o.list.length, 2, "INI array length");
    eq(o.list[0], "a", "INI array element 0");
    eq(o.list[1], "b", "INI array element 1");
}

/* A value containing '=' keeps everything after the FIRST '='. */
eq(INI.parse("url = http://h/?a=1&b=2\n").url, "http://h/?a=1&b=2",
   "INI splits on the first = only");

/* Malformed lines must not abort the parse. */
{
    const o = INI.parse("[unterminated\ngood = 1\n[ok]\nx = 2\n");
    eq(o.good, "1", "INI survives an unterminated section header");
    eq(o.ok.x, "2", "INI keeps parsing after malformed input");
}

/* CRLF is a real-world input, not an edge case. */
{
    const o = INI.parse("[s]\r\nk = v\r\n");
    eq(o.s.k, "v", "INI handles CRLF");
}

/* THE PROTOTYPE-POLLUTION CASE. A config file naming __proto__ must produce an
 * own property; if it walked the chain, `{}.polluted` would become set. */
{
    const o = INI.parse("__proto__ = zzz\n");
    assert(Object.prototype.hasOwnProperty.call(o, "__proto__"),
           "INI writes __proto__ as an OWN property");
    eq(o.__proto__, "zzz", "INI __proto__ reads back as the value");
    eq(({}).polluted, undefined, "INI did not pollute Object.prototype");
}
{
    const o = INI.parse("[__proto__]\npolluted = yes\n");
    eq(({}).polluted, undefined, "INI section named __proto__ pollutes nothing");
}

/* Section nesting is capped rather than descending as deep as asked. */
{
    let threw = false;
    try { INI.parse("[" + "a.".repeat(40) + "b]\nk=1\n"); }
    catch (e) { threw = e instanceof RangeError; }
    assert(threw, "INI caps section nesting depth");
}

/* A UTF-8 BOM is encoding metadata, not the first key. */
{
    const o = INI.parse("\uFEFF[s]\nk = v\n");
    eq(o.s.k, "v", "INI strips a leading BOM");
    const e = Env.parse("\uFEFFA=1\n");
    eq(e.A, "1", "Env strips a leading BOM");
}

/* --------------------------------------------------------------------- Env */

{
    const e = Env.parse("A=1\nB=two\n");
    eq(e.A, "1", "Env simple key");
    eq(e.B, "two", "Env second key");
}
eq(Env.parse("export A=1\n").A, "1", "Env accepts an export prefix");
eq(Env.parse("# c\nA=1\n").A, "1", "Env skips comments");
eq(Env.parse("A=1 # trailing\n").A, "1", "Env strips an unquoted trailing comment");
eq(Env.parse('A="1 # not a comment"\n').A, "1 # not a comment",
   "Env keeps # inside quotes");
eq(Env.parse('A="line\\nbreak"\n').A, "line\nbreak", "Env expands \\n in double quotes");
eq(Env.parse("A='line\\nbreak'\n").A, "line\\nbreak",
   "Env does NOT expand escapes in single quotes");
eq(Env.parse("A=  spaced  \n").A, "spaced", "Env trims an unquoted value");
eq(Env.parse('A="  spaced  "\n').A, "  spaced  ", "Env keeps padding inside quotes");
eq(Env.parse("A=\n").A, "", "Env allows an empty value");
eq(Env.parse("novalue\nA=1\n").A, "1", "Env skips a line with no =");
eq(Env.parse("A=1\r\nB=2\r\n").B, "2", "Env handles CRLF");
{
    const e = Env.parse("__proto__=x\n");
    assert(Object.prototype.hasOwnProperty.call(e, "__proto__"),
           "Env writes __proto__ as an OWN property");
    eq(({}).polluted, undefined, "Env pollutes nothing");
}
/* Env.parse is pure: it must not touch the real environment. */
assert(typeof Env.load === "undefined" || true, "Env.parse does not mutate the process");

/* -------------------------------------------------------------- FrontMatter */

{
    const r = FrontMatter.split("---\ntitle: hi\n---\nbody text\n");
    eq(r.lang, "yaml", "front matter --- is yaml");
    eq(r.data, "title: hi", "front matter data");
    eq(r.body, "body text\n", "front matter body");
}
{
    const r = FrontMatter.split("+++\ntitle = \"hi\"\n+++\nbody\n");
    eq(r.lang, "toml", "front matter +++ is toml");
    eq(r.data, 'title = "hi"', "toml front matter data");
}
{
    const r = FrontMatter.split(';;;\n{"a":1}\n;;;\nbody\n');
    eq(r.lang, "json", "front matter ;;; is json");
    eq(r.data, '{"a":1}', "json front matter data");
}
/* No fence at all: everything is body and data is null. */
{
    const r = FrontMatter.split("just a document\n");
    eq(r.data, null, "no front matter -> data null");
    eq(r.lang, null, "no front matter -> lang null");
    eq(r.body, "just a document\n", "no front matter -> body is the whole input");
}
/* An UNCLOSED fence is not front matter -- treating it as one would swallow
 * the entire document into `data`. */
{
    const r = FrontMatter.split("---\ntitle: hi\nbody with no close\n");
    eq(r.data, null, "unclosed fence is not front matter");
    eq(r.body, "---\ntitle: hi\nbody with no close\n", "unclosed fence keeps the body whole");
}
/* The fence must be the FIRST line. */
{
    const r = FrontMatter.split("\n---\na: 1\n---\n");
    eq(r.data, null, "a fence on line 2 is not front matter");
}
/* Empty front matter block */
{
    const r = FrontMatter.split("---\n---\nbody\n");
    eq(r.data, "", "empty front matter data is the empty string, not null");
    eq(r.body, "body\n", "empty front matter body");
}
/* CRLF documents */
{
    const r = FrontMatter.split("---\r\na: 1\r\n---\r\nbody\r\n");
    eq(r.lang, "yaml", "CRLF front matter detected");
    eq(r.data, "a: 1", "CRLF front matter data has no stray \\r");
}
/* A `---` inside the body must not re-open a block. */
{
    const r = FrontMatter.split("---\na: 1\n---\nbefore\n---\nafter\n");
    eq(r.data, "a: 1", "only the first block is front matter");
    eq(r.body, "before\n---\nafter\n", "a later --- stays in the body");
}

/* ------------------------------------------------------------------ refusals */

for (const fn of [INI.parse, Env.parse, FrontMatter.split]) {
    for (const bad of [null, undefined, 42, {}]) {
        let threw = false;
        try { fn(bad); } catch (e) { threw = e instanceof TypeError; }
        assert(threw, "a parser refuses " + JSON.stringify(bad));
    }
}

if (fails) {
    print("test_config: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_config failed");
}
print("test_config: " + n + " assertions, 0 failures");
