// covers: dyna:html.HTMLParse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_html_parse");
import { HTMLParse, HTMLStringify, Selector, Sanitizer } from "dyna:html";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const S = (h) => HTMLStringify(HTMLParse(h));
const j = (v) => JSON.stringify(v);

eq(j(HTMLParse("<p>hi</p>")), '[{"name":"p","attrs":{},"children":["hi"]}]',
   "the node shape is dyna:xml's");

eq(S("<p>hi</p>"), "<p>hi</p>", "a round trip");

eq(S("<P>hi</P>"), "<p>hi</p>", "tag names lower-case");

eq(S("<p CLASS=x>hi</p>"), '<p class="x">hi</p>', "and so do attribute names");

eq(S("<p class=x>hi</p>"), '<p class="x">hi</p>', "an unquoted value");

eq(S("<p class='x y'>hi</p>"), '<p class="x y">hi</p>', "a single-quoted value");

eq(S("<input disabled>"), "<input disabled=\"\">", "a valueless attribute");

eq(S("<div><img src=a>text</div>"), '<div><img src="a">text</div>',
   "a void element does not open a scope");

eq(S("<br>"), "<br>", "and needs no close tag");

eq(S("<div><br><br></div>"), "<div><br><br></div>", "twice");

eq(S("<p>one<p>two"), "<p>one</p><p>two</p>", "a second p closes the first");

eq(S("<ul><li>a<li>b</ul>"), "<ul><li>a</li><li>b</li></ul>", "and li closes li");

eq(S("<p>text<div>block</div>"), "<p>text</p><div>block</div>",
   "a block element closes an open p");

eq(S("<div>a</span>b</div>"), "<div>ab</div>", "an unmatched close tag is ignored");

eq(S("</p>text"), "text", "even at the top level");

eq(S("<div><b>x</div>"), "<div><b>x</b></div>", "an unclosed element closes at its parent");

__kitS.section("<script>if (a < b) { x() }</script>", () => {
    const t = HTMLParse("<script>if (a < b) { x() }</script>");
    eq(t[0].children[0], "if (a < b) { x() }", "script content is TEXT, not markup");
    eq(t[0].children.length, 1, "and it is one text node");
    eq(HTMLParse("<style>a > b { color: red }</style>")[0].children[0],
       "a > b { color: red }", "and so is style");
    eq(HTMLParse("<title>a < b</title>")[0].children[0], "a < b", "and title");
    eq(S("<script>x<y</script>"), "<script>x<y</script>",
       "raw text is NOT escaped on output -- escaping it would corrupt the script");
    eq(HTMLParse("<script>x</scripty>alert(1)</script>")[0].children[0],
       "x</scripty>alert(1)", "a longer tag name does not end raw text");
    eq(HTMLParse("<script>x</script >y</script>").map(v =>
           typeof v === "string" ? v : v.children[0]).join("|"), "x|y",
       "a space before > still closes raw text");
});

eq(HTMLParse("<p>&amp;&lt;&gt;&quot;&#65;&#x42;&nbsp;</p>")[0].children[0],
   "&<>\"AB ", "named, decimal and hex references decode");

eq(HTMLParse("<p>&notareal;</p>")[0].children[0], "&notareal;",
   "an unknown reference stays literal, as a browser leaves it");

eq(HTMLParse("<p>a &amp b</p>")[0].children[0], "a &amp b", "and so does a bare &");

eq(HTMLParse("<p>&#xD800;</p>")[0].children[0], "\uFFFD",
   "a surrogate reference becomes U+FFFD, never a lone half");

__kitS.section("a 100-byte text run survives intact", () => {
    eq(HTMLParse("<p>" + "a".repeat(100) + "</p>")[0].children[0],
       "a".repeat(100), "a 100-byte text run survives intact");
    eq(HTMLParse("<p>" + "a".repeat(64) + "#65;b</p>")[0].children[0],
       "a".repeat(64) + "#65;b", "byte 64 starting a fake entity is NOT decoded");
    eq(HTMLParse("<p>" + "a".repeat(63) + "Xb</p>")[0].children[0],
       "a".repeat(63) + "Xb", "byte 64 of a 65-byte run is not eaten");
    eq(HTMLParse("<p>" + "a".repeat(100) + "&amp;" + "b".repeat(100) + "</p>")[0].children[0],
       "a".repeat(100) + "&" + "b".repeat(100),
       "an entity beyond the window still decodes");
    eq(HTMLParse('<p title="' + "x".repeat(70) + "&amp;y" + '">v</p>')[0].attrs.title,
       "x".repeat(70) + "&y", "and attribute values take the same path");
});

eq(S("<p>a<!-- comment -->b</p>"), "<p>ab</p>", "a comment is not in the tree");

eq(S("<!DOCTYPE html><p>x</p>"), "<p>x</p>", "nor a doctype");

eq(HTMLParse("<p>2 < 3</p>")[0].children[0], "2 < 3", "`< ` is text, not a tag");

eq(HTMLParse("<p>a<3</p>")[0].children[0], "a<3", "and so is `<3`");

throws(() => HTMLParse(42), "the input must be a string");

__kitS.section("nesting past the cap is refused", () => {
    const deep = "<div>".repeat(300);
    throws(() => HTMLParse(deep), "nesting past the cap is refused");
});

__kitS.section("<p __proto__=\"x\">t</p>", () => {
    const o = HTMLParse('<p __proto__="x">t</p>');
    eq(Object.keys(o[0].attrs).length, 1, "a __proto__ attribute is an own property");
    eq(o[0].attrs.__proto__, "x", "with its value");
});

__kitS.section("<a href=\"1\" href=\"2\">t</a>", () => {
    const o = HTMLParse('<a href="1" href="2">t</a>');
    eq(o[0].attrs.href, "1", "the first duplicate attribute wins");
    eq(HTMLParse('<a HREF="1" href="2">t</a>')[0].attrs.href, "1",
       "and the comparison is case-insensitive");
});

__kitS.section("first document", () => {
    const s = new Selector("p");
    const d1 = HTMLParse("<p>a</p>"), d2 = HTMLParse("<div><p>b</p><p>c</p></div>");
    eq(s.all(d1).length, 1, "first document");
    eq(s.all(d2).length, 2, "second document, same compiled selector");
});
__kitS.end();

if (fails) {
    print("test_html: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_html failed");
}
print("test_html: " + n + " assertions, 0 failures");
