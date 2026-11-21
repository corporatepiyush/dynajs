// covers: dyna:html.Sanitizer
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_sanitizer");
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

const san = new Sanitizer({
    allow: { p: [], b: [], i: [], a: ["href", "title"], img: ["src", "alt"] },
    protocols: { "a.href": ["https", "http", "mailto"], "img.src": ["https"] },
});

eq(san.clean("<p>plain <b>bold</b></p>"), "<p>plain <b>bold</b></p>",
   "allowed markup passes through");

eq(san.clean('<a href="https://e.com" title="t">x</a>'),
   '<a href="https://e.com" title="t">x</a>', "and allowed attributes");

__kitS.section("a script element AND its content", () => {
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
});

__kitS.section("a disallowed tag keeps its text", () => {
    eq(san.clean("<div>kept</div>"), "kept", "a disallowed tag keeps its text");
    eq(san.clean("<div><p>kept</p></div>"), "<p>kept</p>",
       "and its allowed descendants");
    eq(san.clean("<script>gone</script>"), "",
       "but a raw-text element's content goes with it, because it IS script");
});

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

__kitS.section("<p onclick=x>a<script>b</script><a href=\"javascript:1\">c</a>", () => {
    const dirty = '<p onclick=x>a<script>b</script><a href="javascript:1">c</a>' +
                  '<b>d</b><div>e</div></p>';
    const once = san.clean(dirty);
    eq(san.clean(once), once, "clean() is idempotent");
});

__kitS.section("<img srcset=\"javascript:alert(1) 1x, //e.com/a.png 2x\">", () => {
    const s2 = new Sanitizer({
        allow: { img: ["src", "srcset"], td: ["background"], a: ["href", "ping"] },
    });
    eq(s2.clean('<img srcset="javascript:alert(1) 1x, //e.com/a.png 2x">'),
       "<img>", "a srcset carrying javascript: is dropped whole");
    eq(s2.clean('<img srcset="a.png 1x, b.png 2x">'),
       '<img srcset="a.png 1x, b.png 2x">', "and a relative srcset stays");
    eq(s2.clean('<td background="javascript:alert(1)">x</td>'),
       "<td>x</td>", "a background carrying javascript: is dropped");
    eq(s2.clean('<a ping="javascript:alert(1)" href="/r">y</a>'),
       '<a href="/r">y</a>', "a ping carrying javascript: goes, the href stays");
});
__kitS.end();

if (fails) {
    print("test_html: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_html failed");
}
print("test_html: " + n + " assertions, 0 failures");
