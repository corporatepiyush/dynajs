// covers: dyna:html.Selector
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_selector");
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

__kitS.section("<div id=\"root\" class=\"a b\"><p class=\"x\">one<b>bold</b></p>", () => {
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
    {
        const bt = HTMLParse('<a><b><p id="miss"></p></b>' +
                             '<b><b><p id="hit"></p></b></b></a>');
        eq(new Selector("a > b p").all(bt).map(e => e.attrs.id).join(","),
           "miss,hit", "a > b p backtracks past the nearest b");
    }
    eq(all("p.x").length, 2, "a compound of tag and class");
    eq(all("div#root.a").length, 1, "tag, id and class together");
    eq(all("p, b").length, 4, "a selector group is the union");
    eq(all("p:first-child").length, 2, "first-child");
    eq(all("p:last-child").length, 1,
       "last-child -- only the nested p is the last of its parent");
    eq(all("nope").length, 0, "a selector that matches nothing");
    eq(all("*").length, 6, "the universal selector matches every element");
    eq(all("[CLASS=x]").length, 2, "[CLASS=...] matches case-insensitively");
    const first = new Selector("p").first(doc);
    eq(first.attrs.class, "x", "first() returns the first in document order");
    eq(new Selector("nope").first(doc), undefined, "or undefined when no match (dynajs.d.ts)");
    assert(new Selector("p").matches(first), "matches() tests one node");
    assert(!new Selector("div").matches(first), "and says no");
    throws(() => new Selector("div p").matches(first),
           "a combinator in matches() is refused: there are no ancestors to test");
});
__kitS.end();

for (const bad of ["", "  ", "#", ".", "[", "[a", "[a=", "p >", ">", "p,",
                   ":nope", "p:hover", "[lang|=en]"])
    throws(() => new Selector(bad), "refuses the selector " + j(bad));

throws(() => new Selector(42), "a selector must be a string");

if (fails) {
    print("test_html: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_html failed");
}
print("test_html: " + n + " assertions, 0 failures");
