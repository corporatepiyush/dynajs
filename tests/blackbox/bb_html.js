// Parametric black-box contract test, generated from dynajs.d.ts lines 1897-1966. Engine sources not consulted.
// Covers: dyna:html — HTMLParse/HTMLStringify/HTMLText, MarkdownToHTML (in range; doc pins no markdown
// constructs, so structural rows only), Selector, Sanitizer, rewriteLinks, Template.
// Every expectation below is derived from the dynajs.d.ts contract text only. DOC-TENSION rows are marked inline.

import { HTMLParse, HTMLStringify, HTMLText, MarkdownToHTML, Selector, Sanitizer, rewriteLinks, Template } from "dyna:html";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }

// ---- local extras ----
function canon(v) {
  if (Array.isArray(v)) return "[" + v.map(canon).join(",") + "]";
  if (v !== null && typeof v === "object") {
    const ks = Object.keys(v).sort();
    return "{" + ks.map((k) => JSON.stringify(k) + ":" + canon(v[k])).join(",") + "}";
  }
  return JSON.stringify(v);
}
function assertCanonEq(a, b, msg) { n++; if (canon(a) !== canon(b)) throw new Error("assertion failed (canon deep): " + msg + " — got " + canon(a) + " expected " + canon(b)); }
function check(got, expected, label) {
  if (typeof expected === "function") { n++; if (!expected(got)) throw new Error("assertion failed (predicate): " + label + " — got |" + got + "|"); return; }
  if (Array.isArray(expected)) { assertDeepEq(got, expected, label); return; }
  if (expected !== null && typeof expected === "object") { assertCanonEq(got, expected, label); return; }
  assertEq(got, expected, label);
}

/* ================================================================== *
 * H1: HTMLParse tree shape [label, html, expected tree]. The doc pins the
 * shared node shape {name, attrs, children: (string | HTMLElement)[]} and
 * that HTMLParse returns an ARRAY of root nodes.
 * ================================================================== */
const H1 = [
  ["paragraph with text", "<p>hi</p>", [{ name: "p", attrs: {}, children: ["hi"] }]],
  ["attributes parsed into attrs", '<a href="x" data-n="2">l</a>', [{ name: "a", attrs: { href: "x", "data-n": "2" }, children: ["l"] }]],
  ["nested elements", "<div><p>x</p></div>", [{ name: "div", attrs: {}, children: [{ name: "p", attrs: {}, children: ["x"] }] }]],
  ["void element has no children", "<br>", [{ name: "br", attrs: {}, children: [] }]],
  ["self-closing slash tolerated", "<br/>", [{ name: "br", attrs: {}, children: [] }]],
  ["multiple roots returned as a fragment array (doc: parity with XMLParse multiple)", "<p>a</p><p>b</p>", [{ name: "p", attrs: {}, children: ["a"] }, { name: "p", attrs: {}, children: ["b"] }]],
  ["text between elements becomes string children", "<p>a<b>c</b>d</p>", [{ name: "p", attrs: {}, children: ["a", { name: "b", attrs: {}, children: ["c"] }, "d"] }]],
  ["img with src attribute", '<img src="i.png">', [{ name: "img", attrs: { src: "i.png" }, children: [] }]],
];
for (const [label, html, expected] of H1) assertCanonEq(HTMLParse(html), expected, label);

// H1b: tag-soup tolerance rows — the doc says dyna:html parses the shared tree but pins no
// error behavior for HTML, so these are structural (parse must produce something sensible).
const H1b = [
  ["unclosed tag still yields a root", () => HTMLParse("<p>text").length >= 1, true],
  ["unclosed tag keeps its text", () => { const r = HTMLParse("<p>text"); return typeof r[0].name === "string" && r[0].name === "p"; }, true],
  ["mixed content soup yields roots array", () => Array.isArray(HTMLParse("<b>x<i>y</b>z")), true],
];
for (const [label, thunk, expected] of H1b) check(thunk(), expected, label);

/* ================================================================== *
 * H2: HTMLText [label, html, expected visible text].
 * ================================================================== */
const H2 = [
  ["flat text", "<p>hello</p>", "hello"],
  ["nested text concatenated", "<p>a<b>c</b>d</p>", "acd"],
  ["array of roots concatenated", "<p>a</p><p>b</p>", "ab"],
  ["attributes are not visible text", '<img src="x">', ""],
  ["deep nesting flattened", "<div><span>s</span></div>", "s"],
  ["void elements contribute no text", "<br>", ""],
];
for (const [label, html, expected] of H2) assertEq(HTMLText(HTMLParse(html)), expected, label);

// H2b: HTMLText accepts a bare node too (doc: node or node array).
assertEq(HTMLText({ name: "p", attrs: {}, children: ["a", { name: "b", attrs: {}, children: ["c"] }] }), "ac", "HTMLText accepts a single node object");

/* ================================================================== *
 * H3: HTMLStringify [label, canonical html] — round-trip rows on inputs the
 * parser normalizes identically, plus one exact writer row.
 * ================================================================== */
const H3 = [
  ["round trip simple", "<p>hi</p>"],
  ["round trip attribute", '<a href="x">l</a>'],
  ["round trip nested", "<div><p>x</p></div>"],
  ["round trip takes the first root node", "<p>only</p>"],
];
for (const [label, html] of H3) assertEq(HTMLStringify(HTMLParse(html)[0]), html, label);

// H3b: exact serialization of a hand-built node [label, node, expected].
const H3b = [
  ["serialize text with attribute", { name: "p", attrs: { id: "1" }, children: ["t"] }, '<p id="1">t</p>'],
  ["serialize plain text node", { name: "span", attrs: {}, children: ["x"] }, "<span>x</span>"],
];
for (const [label, node, expected] of H3b) assertEq(HTMLStringify(node), expected, label);

// H3c: structural serialization rows (empty-children layout the doc does not pin).
const H3c = [
  ["empty node serializes with its name", () => { const s = HTMLStringify({ name: "br", attrs: {}, children: [] }); return typeof s === "string" && s.startsWith("<br") && s.endsWith(">"); }, true],
  ["attrs survive serialization", () => { const s = HTMLStringify({ name: "a", attrs: { href: "h" }, children: [] }); return s.includes('href="h"'); }, true],
];
for (const [label, thunk, expected] of H3c) check(thunk(), expected, label);

/* ================================================================== *
 * H4: Selector [label, thunk, expected]. Tag/.class/#id are core CSS the
 * doc's "compiled CSS selector" contract implies; combinators are only
 * documented for `matches` (NOT allowed), so they are never exercised.
 * ================================================================== */
const H4 = [
  ["all() finds every matching element", () => new Selector("p").all(HTMLParse('<div><p>a</p><p>b</p><span>s</span></div>')).length, 2],
  ["all() empty on no match", () => new Selector("h1").all(HTMLParse("<p>a</p>")).length, 0],
  ["first() returns the first match", () => { const r = new Selector("p").first(HTMLParse("<p>a</p><p>b</p>")); return r.children[0]; }, "a"],
  // DOC-TENSION: prose says "or null when none", the signature says HTMLElement | undefined.
  // Most specific documented reading = the type signature; we pin nil-ness only (== null).
  ["first() on no match is nil (null OR undefined)", () => { const r = new Selector("h1").first(HTMLParse("<p>a</p>")); return r == null; }, true],
  ["class selector", () => new Selector(".x").all(HTMLParse('<p class="x">a</p><p>b</p>')).length, 1],
  ["id selector", () => new Selector("#m").all(HTMLParse('<p id="m">a</p><p>b</p>')).length, 1],
  ["matches true on same tag", () => new Selector("p").matches({ name: "p", attrs: {}, children: [] }), true],
  ["matches false on other tag", () => new Selector("p").matches({ name: "div", attrs: {}, children: [] }), false],
  ["matches attribute-presence selector", () => new Selector("[href]").matches({ name: "a", attrs: { href: "x" }, children: [] }), true],
  ["matches class selector", () => new Selector(".c").matches({ name: "p", attrs: { class: "c" }, children: [] }), true],
  ["matches attribute-value selector", () => new Selector('[href="/h"]').matches({ name: "a", attrs: { href: "/h" }, children: [] }), true],
];
for (const [label, thunk, expected] of H4) check(thunk(), expected, label);

/* ================================================================== *
 * H5: Sanitizer [label, thunk, expected] — allow-list semantics.
 * ================================================================== */
const H5 = [
  ["clean keeps allowed tags", () => { const s = new Sanitizer({ allow: { b: true, p: true } }); return s.clean("<b>x</b>").includes("<b"); }, true],
  ["clean strips raw disallowed script tag", () => { const s = new Sanitizer({ allow: { b: true } }); return !s.clean("<script>x</script>").includes("<script"); }, true],
  ["clean returns a string on text-only input", () => typeof new Sanitizer({ allow: {} }).clean("hello"), "string"],
  ["protocols option accepted", () => { const s = new Sanitizer({ allow: { a: true }, protocols: { href: ["http", "https"] } }); return typeof s.clean('<a href="https://x">y</a>'); }, "string"],
];
for (const [label, thunk, expected] of H5) check(thunk(), expected, label);

// H5b: refusal rows — the doc says "An allow-list is required; there is no default policy".
const H5b = [
  ["Sanitizer without opts is refused", () => new Sanitizer()],
  ["Sanitizer without allow key is refused", () => new Sanitizer({})],
];
for (const [label, thunk] of H5b) assertThrows(thunk, label);

/* ================================================================== *
 * H6: rewriteLinks [label, thunk, expected] — the doc pins this API in
 * detail: in-place rewrite, count of replaced attributes, lower-cased
 * tag/attr names, undefined/null keeps original, srcset splits with
 * descriptors preserved rejoined ", ", ToString coercion, throw mid-walk
 * leaves earlier rewrites applied and propagates.
 * ================================================================== */
const H6 = [
  ["rewrites href in place, count 1", () => { const doc = HTMLParse('<a href="/a">x</a>'); const cnt = rewriteLinks(doc, (url) => "http://s" + url); return cnt === 1 && doc[0].attrs.href === "http://s/a"; }, true],
  ["tag and attr names reach fn lower-cased", () => { let t, a; rewriteLinks(HTMLParse('<A HREF="/a">x</A>'), (u, tag, attr) => { t = tag; a = attr; return u; }); return t === "a" && a === "href"; }, true],
  ["fn returning undefined keeps original (count 0)", () => { const doc = HTMLParse('<a href="/a">x</a>'); const cnt = rewriteLinks(doc, () => undefined); return cnt === 0 && doc[0].attrs.href === "/a"; }, true],
  ["fn returning null keeps original (count 0)", () => { const doc = HTMLParse('<a href="/a">x</a>'); return rewriteLinks(doc, () => null); }, 0],
  ["non-URL attributes are never rewritten", () => rewriteLinks(HTMLParse('<p title="t">x</p>'), () => "z"), 0],
  ["srcset splits candidates, each counts", () => { const doc = HTMLParse('<img srcset="/a.png 1x, /b.png 2x">'); return rewriteLinks(doc, (u) => "P" + u); }, 2],
  ["srcset descriptors preserved, rejoined with ', '", () => { const doc = HTMLParse('<img srcset="/a.png 1x, /b.png 2x">'); rewriteLinks(doc, (u) => "P" + u); return doc[0].attrs.srcset; }, "P/a.png 1x, P/b.png 2x"],
  ["every documented single-URL sink is visited (href)", () => rewriteLinks(HTMLParse('<a href="/a"></a>'), (u) => u), 1],
  ["sink src", () => rewriteLinks(HTMLParse('<img src="/b">'), (u) => u), 1],
  ["sink action", () => rewriteLinks(HTMLParse('<form action="/s"></form>'), (u) => u), 1],
  ["sink formaction", () => rewriteLinks(HTMLParse('<button formaction="/f"></button>'), (u) => u), 1],
  ["sink cite", () => rewriteLinks(HTMLParse('<blockquote cite="/c"></blockquote>'), (u) => u), 1],
  ["sink poster", () => rewriteLinks(HTMLParse('<video poster="/p"></video>'), (u) => u), 1],
  ["sink background", () => rewriteLinks(HTMLParse('<body background="/bg"></body>'), (u) => u), 1],
  ["sink longdesc", () => rewriteLinks(HTMLParse('<img longdesc="/d">'), (u) => u), 1],
  ["count sums across several attributes", () => rewriteLinks(HTMLParse('<a href="/a"></a><img src="/b">'), (u) => u), 2],
  ["non-string return is coerced with ToString", () => { const doc = HTMLParse('<a href="/a">x</a>'); rewriteLinks(doc, () => 42); return doc[0].attrs.href; }, "42"],
  ["empty-string return replaces (only undefined/null keep)", () => { const doc = HTMLParse('<a href="/a">x</a>'); rewriteLinks(doc, () => ""); return doc[0].attrs.href; }, ""],
  ["accepts a single node (not only arrays)", () => { const node = HTMLParse('<a href="/a">x</a>')[0]; return rewriteLinks(node, (u) => "P" + u); }, 1],
];
for (const [label, thunk, expected] of H6) check(thunk(), expected, label);

// H6b: mid-walk throw semantics (documented): exception propagates, earlier rewrites stay applied.
assertThrows(() => {
  const doc = HTMLParse('<a href="/a">x</a><a href="/b">y</a>');
  rewriteLinks(doc, (u) => { if (u === "/b") throw new Error("boom"); return "P" + u; });
}, "rewriteLinks fn throwing mid-walk propagates");
{
  const doc = HTMLParse('<a href="/a">x</a><a href="/b">y</a>');
  try { rewriteLinks(doc, (u) => { if (u === "/b") throw new Error("boom"); return "P" + u; }); } catch (e) { /* consumed below */ }
  assert(doc[0].attrs.href === "P/a", "rewriteLinks earlier rewrites stay applied after a mid-walk throw");
}

/* ================================================================== *
 * H7: Template — the doc pins only "compiled template with escaping" and
 * render(data); no template syntax is documented, so rows are structural.
 * ================================================================== */
const H7 = [
  ["literal text survives render", () => new Template("hello").render().includes("hello"), true],
  ["render with data returns a string", () => typeof new Template("x").render({ a: 1 }), "string"],
  ["render with no data returns a string", () => typeof new Template("x").render(), "string"],
  ["escape option accepted", () => typeof new Template("x", { escape: true }).render(), "string"],
];
for (const [label, thunk, expected] of H7) check(thunk(), expected, label);

/* ================================================================== *
 * H8: MarkdownToHTML — IN the assigned range (line 1915), but the doc pins
 * only "Renders Markdown to HTML (through the module's escaper)" and the
 * allowRawHTML option; no markdown constructs are documented, so these
 * rows are structural only.
 * ================================================================== */
const H8 = [
  ["returns a string for a heading", () => typeof MarkdownToHTML("# t"), "string"],
  ["returns a string for plain text", () => typeof MarkdownToHTML("plain"), "string"],
  ["allowRawHTML option accepted", () => typeof MarkdownToHTML("<b>x</b>", { allowRawHTML: true }), "string"],
];
for (const [label, thunk, expected] of H8) check(thunk(), expected, label);

print("bb_html: all tests passed (" + n + " assertions)");
