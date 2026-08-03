/* test_html.js -- parse, select and sanitize in dyna:html (design 06).
 *
 * The sanitizer is a SECURITY filter, so most of this file is the payload
 * list: an allow-list that lets one of these through is worse than no
 * sanitizer, because the caller believes it worked. Each vector asserts what
 * comes OUT, not merely that something was removed.
 *
 * The parser's own cases are the ones that decide whether real pages parse at
 * all: void elements, implied end tags, raw-text elements, and a stray close
 * tag that must be ignored rather than unwind the document.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_html.js
 */
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

/* --------------------------------------------------------------- parsing */

eq(j(HTMLParse("<p>hi</p>")), '[{"name":"p","attrs":{},"children":["hi"]}]',
   "the node shape is dyna:xml's");
eq(S("<p>hi</p>"), "<p>hi</p>", "a round trip");
eq(S("<P>hi</P>"), "<p>hi</p>", "tag names lower-case");
eq(S("<p CLASS=x>hi</p>"), '<p class="x">hi</p>', "and so do attribute names");
eq(S("<p class=x>hi</p>"), '<p class="x">hi</p>', "an unquoted value");
eq(S("<p class='x y'>hi</p>"), '<p class="x y">hi</p>', "a single-quoted value");
eq(S("<input disabled>"), "<input disabled=\"\">", "a valueless attribute");

/* VOID ELEMENTS: without this list everything after an <img> nests inside it. */
eq(S("<div><img src=a>text</div>"), '<div><img src="a">text</div>',
   "a void element does not open a scope");
eq(S("<br>"), "<br>", "and needs no close tag");
eq(S("<div><br><br></div>"), "<div><br><br></div>", "twice");

/* IMPLIED END TAGS: a second <p> closes the first, which is why real pages
 * parse at all. */
eq(S("<p>one<p>two"), "<p>one</p><p>two</p>", "a second p closes the first");
eq(S("<ul><li>a<li>b</ul>"), "<ul><li>a</li><li>b</li></ul>", "and li closes li");
eq(S("<p>text<div>block</div>"), "<p>text</p><div>block</div>",
   "a block element closes an open p");

/* A STRAY CLOSE TAG IS IGNORED. Unwinding on it is how one bad tag destroys
 * the rest of a document. */
eq(S("<div>a</span>b</div>"), "<div>ab</div>", "an unmatched close tag is ignored");
eq(S("</p>text"), "text", "even at the top level");
eq(S("<div><b>x</div>"), "<div><b>x</b></div>", "an unclosed element closes at its parent");

/* RAW TEXT: a `<` inside <script> is a character, and treating it as a tag is
 * the classic mXSS opening. */
{
    const t = HTMLParse("<script>if (a < b) { x() }</script>");
    eq(t[0].children[0], "if (a < b) { x() }", "script content is TEXT, not markup");
    eq(t[0].children.length, 1, "and it is one text node");
    eq(HTMLParse("<style>a > b { color: red }</style>")[0].children[0],
       "a > b { color: red }", "and so is style");
    eq(HTMLParse("<title>a < b</title>")[0].children[0], "a < b", "and title");
    eq(S("<script>x<y</script>"), "<script>x<y</script>",
       "raw text is NOT escaped on output -- escaping it would corrupt the script");
}

/* Entities, comments, and text that only looks like markup. */
eq(HTMLParse("<p>&amp;&lt;&gt;&quot;&#65;&#x42;&nbsp;</p>")[0].children[0],
   "&<>\"AB ", "named, decimal and hex references decode");
eq(HTMLParse("<p>&notareal;</p>")[0].children[0], "&notareal;",
   "an unknown reference stays literal, as a browser leaves it");
eq(HTMLParse("<p>a &amp b</p>")[0].children[0], "a &amp b", "and so does a bare &");
eq(HTMLParse("<p>&#xD800;</p>")[0].children[0], "�",
   "a surrogate reference becomes U+FFFD, never a lone half");
eq(S("<p>a<!-- comment -->b</p>"), "<p>ab</p>", "a comment is not in the tree");
eq(S("<!DOCTYPE html><p>x</p>"), "<p>x</p>", "nor a doctype");
eq(HTMLParse("<p>2 < 3</p>")[0].children[0], "2 < 3", "`< ` is text, not a tag");
eq(HTMLParse("<p>a<3</p>")[0].children[0], "a<3", "and so is `<3`");
throws(() => HTMLParse(42), "the input must be a string");
{
    const deep = "<div>".repeat(300);
    throws(() => HTMLParse(deep), "nesting past the cap is refused");
}
{
    const o = HTMLParse('<p __proto__="x">t</p>');
    eq(Object.keys(o[0].attrs).length, 1, "a __proto__ attribute is an own property");
    eq(o[0].attrs.__proto__, "x", "with its value");
}

/* -------------------------------------------------------------- selectors */

{
    const doc = HTMLParse(
        '<div id="root" class="a b"><p class="x">one<b>bold</b></p>' +
        '<p class="y">two</p><span><p class="x">three</p></span></div>');
    const all = (s) => new Selector(s).all(doc);
    eq(all("p").length, 3, "a tag selector finds them all");
    eq(all("#root").length, 1, "an id");
    eq(all(".x").length, 2, "a class, matched as a WORD in the attribute");
    eq(all(".a").length, 1, "one class out of several");
    eq(all("[id]").length, 1, "attribute presence");
    eq(all('[id="root"]').length, 1, "attribute equality");
    eq(all('[class^="a"]').length, 1, "a prefix match");
    eq(all('[class$="y"]').length, 1, "a suffix match");
    eq(all('[class*=" "]').length, 1, "a substring match");
    eq(all("div p").length, 3, "a descendant combinator");
    eq(all("div > p").length, 2, "a child combinator excludes the nested one");
    eq(all("span > p").length, 1, "and finds the nested one from its parent");
    eq(all("p b").length, 1, "two levels");
    eq(all("p.x").length, 2, "a compound of tag and class");
    eq(all("div#root.a").length, 1, "tag, id and class together");
    eq(all("p, b").length, 4, "a selector group is the union");
    eq(all("p:first-child").length, 2, "first-child");
    eq(all("p:last-child").length, 1,
       "last-child -- only the nested p is the last of its parent");
    eq(all("nope").length, 0, "a selector that matches nothing");
    eq(all("*").length, 6, "the universal selector matches every element");
    const first = new Selector("p").first(doc);
    eq(first.attrs.class, "x", "first() returns the first in document order");
    eq(new Selector("nope").first(doc), undefined, "or undefined");
    assert(new Selector("p").matches(first), "matches() tests one node");
    assert(!new Selector("div").matches(first), "and says no");
    throws(() => new Selector("div p").matches(first),
           "a combinator in matches() is refused: there are no ancestors to test");
}
for (const bad of ["", "  ", "#", ".", "[", "[a", "[a=", "p >", ">", "p,",
                   ":nope", "p:hover"])
    throws(() => new Selector(bad), "refuses the selector " + j(bad));
throws(() => new Selector(42), "a selector must be a string");
{
    /* A compiled selector is reusable, which is why it is a class. */
    const s = new Selector("p");
    const d1 = HTMLParse("<p>a</p>"), d2 = HTMLParse("<div><p>b</p><p>c</p></div>");
    eq(s.all(d1).length, 1, "first document");
    eq(s.all(d2).length, 2, "second document, same compiled selector");
}

/* ---------------------------------------------- THE SANITIZER PAYLOADS */

const san = new Sanitizer({
    allow: { p: [], b: [], i: [], a: ["href", "title"], img: ["src", "alt"] },
    protocols: { "a.href": ["https", "http", "mailto"], "img.src": ["https"] },
});
eq(san.clean("<p>plain <b>bold</b></p>"), "<p>plain <b>bold</b></p>",
   "allowed markup passes through");
eq(san.clean('<a href="https://e.com" title="t">x</a>'),
   '<a href="https://e.com" title="t">x</a>', "and allowed attributes");

{
    /* Each payload states what must come OUT. "It threw something away" is not
     * the assertion -- the assertion is that nothing executable survived. */
    const vectors = [
        ['<script>alert(1)</script>', "", "a script element AND its content"],
        ['<p onclick="alert(1)">x</p>', "<p>x</p>", "an event handler attribute"],
        ['<p ONCLICK="alert(1)">x</p>', "<p>x</p>", "however it is cased"],
        ['<a href="javascript:alert(1)">x</a>', "<a>x</a>", "a javascript: URL"],
        ['<a href="JaVaScRiPt:alert(1)">x</a>', "<a>x</a>", "cased differently"],
        ['<a href="java\tscript:alert(1)">x</a>', "<a>x</a>",
         "with an embedded tab, which a browser ignores"],
        ['<a href="java\nscript:alert(1)">x</a>', "<a>x</a>", "or a newline"],
        ['<a href=" javascript:alert(1)">x</a>', "<a>x</a>", "or leading space"],
        ['<a href="data:text/html,<script>alert(1)</script>">x</a>', "<a>x</a>",
         "a data: URL"],
        ['<a href="vbscript:msgbox">x</a>', "<a>x</a>", "vbscript:"],
        ['<img src="javascript:alert(1)">', "<img>", "on an image too"],
        ['<img src="http://e.com/x.png">', "<img>",
         "and http where only https is allowed"],
        ['<iframe src="https://evil"></iframe>', "", "a disallowed element"],
        ['<object data="x"></object>', "", "an object"],
        ['<embed src="x">', "", "an embed"],
        ['<style>body{background:url(javascript:1)}</style>', "",
         "a style element and its content"],
        ['<p><svg onload="alert(1)"></svg></p>', "<p></p>", "an svg with a handler"],
        ['<p>a<b onmouseover=alert(1)>b</b></p>', "<p>a<b>b</b></p>",
         "a handler on an ALLOWED element"],
        ['<form action="x"><input name="y"></form>', "", "a form"],
        ['<a href="#" style="x">y</a>', '<a href="#">y</a>',
         "a style attribute goes; the allowed relative href stays"],
        ['<p><!--<script>alert(1)</script>--></p>', "<p></p>", "a commented script"],
    ];
    let bad = 0;
    for (const [input, want, what] of vectors) {
        const got = san.clean(input);
        if (got !== want) {
            bad++;
            print("  " + what + ": " + j(input) + " -> " + j(got) + ", want " + j(want));
        }
    }
    assert(bad === 0, "every XSS payload produces exactly the safe output ("
                      + (vectors.length - bad) + "/" + vectors.length + ")");
    assert(vectors.length === 21, "the payload list is the one above");
}
{
    /* A disallowed element loses its TAG but keeps its text: dropping the
     * subtree silently deletes content the author wrote. */
    eq(san.clean("<div>kept</div>"), "kept", "a disallowed tag keeps its text");
    eq(san.clean("<div><p>kept</p></div>"), "<p>kept</p>",
       "and its allowed descendants");
    eq(san.clean("<script>gone</script>"), "",
       "but a raw-text element's content goes with it, because it IS script");
}
eq(san.clean("<p>a &lt; b &amp; c</p>"), "<p>a &lt; b &amp; c</p>",
   "text is re-escaped, so the output re-parses to the same thing");
eq(san.clean('<a href="https://e.com/?a=1&b=2">x</a>'),
   '<a href="https://e.com/?a=1&amp;b=2">x</a>', "and so is an attribute value");
eq(san.clean('<a href="/relative">x</a>'), '<a href="/relative">x</a>',
   "a relative URL has no scheme to check");
eq(san.clean('<a href="mailto:a@b.c">x</a>'), '<a href="mailto:a@b.c">x</a>',
   "and a listed scheme passes");
eq(san.clean(""), "", "empty input");
throws(() => new Sanitizer(), "an allow-list is required: there is no default policy");
throws(() => new Sanitizer({}), "and `allow` is the part that matters");
throws(() => san.clean(42), "clean takes a string");
throws(() => Sanitizer.prototype.clean.call({}, "x"), "and a real receiver");
{
    /* The output of clean() must be a fixed point: cleaning it again changes
     * nothing. A sanitizer that is not idempotent has a second-pass hole. */
    const dirty = '<p onclick=x>a<script>b</script><a href="javascript:1">c</a>' +
                  '<b>d</b><div>e</div></p>';
    const once = san.clean(dirty);
    eq(san.clean(once), once, "clean() is idempotent");
}

if (fails) {
    print("test_html: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_html failed");
}
print("test_html: " + n + " assertions, 0 failures");
