// covers: dyna:html.rewriteLinks
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_rewrite_links_rewritelinks");
import { rewriteLinks, HTMLParse, HTMLStringify } from "dyna:html";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b));
const throws = (fn, m) => {
  let t = false, msg = "";
  try { fn(); } catch (e) { t = true; msg = String(e.message || e); }
  check(t, m + (t ? "" : " -- did NOT throw"));
  return msg;
};

{
  let m = throws(() => rewriteLinks(), "no arguments refused");
  throws(() => rewriteLinks({}), "missing fn refused");
  m = throws(() => rewriteLinks({}, 42), "non-function fn refused");
  check(/function/.test(m), "error names the function requirement");
  m = throws(() => rewriteLinks(42, () => undefined), "non-object doc refused");
  throws(() => rewriteLinks({ name: "a", attrs: {}, children: [] }, 42),
         "bad fn refused even with a good doc");
}

__kitS.section("four URL attributes replaced (no srcset here)", () => {
  const doc = [{
    name: "a", attrs: { href: "/one", CLASS: "keep" },
    children: [{
      name: "img", attrs: { src: "/pic.png", alt: "/not-a-url-attr" }, children: []
    }, {
      name: "form", attrs: { action: "/go", formaction: "/fgo" }, children: []
    }]
  }];
  const calls = [];
  const count = rewriteLinks(doc, (url, tag, attr) => {
    calls.push([url, tag, attr]);
    return "https://ex.test" + url;
  });
  eq(count, 4, "four URL attributes replaced (no srcset here)");
  eq(doc[0].attrs.href, "https://ex.test/one", "href rewritten IN PLACE");
  eq(doc[0].attrs.CLASS, "keep", "non-URL attributes untouched");
  eq(doc[0].children[0].attrs.src, "https://ex.test/pic.png", "src rewritten");
  eq(doc[0].children[0].attrs.alt, "/not-a-url-attr", "alt untouched");
  eq(doc[0].children[1].attrs.action, "https://ex.test/go", "action rewritten");
  eq(doc[0].children[1].attrs.formaction, "https://ex.test/fgo", "formaction rewritten");
  eq(calls, [["/one", "a", "href"], ["/pic.png", "img", "src"],
             ["/go", "form", "action"], ["/fgo", "form", "formaction"]],
     "fn sees (url, tag, attr) with tag/attr lower-cased, in document order");
});

__kitS.section("only the replaced attribute counts", () => {
  const doc = [{ name: "a", attrs: { href: "/h", ping: "/p", cite: "/c" },
                 children: [] }];
  const count = rewriteLinks(doc, (url) => {
    if (url === "/h") return undefined;
    if (url === "/p") return null;
    return 42;
  });
  eq(count, 1, "only the replaced attribute counts");
  eq(doc[0].attrs.href, "/h", "undefined keeps the original");
  eq(doc[0].attrs.ping, "/p", "null keeps the original");
  eq(doc[0].attrs.cite, "42", "a non-string return coerces with ToString");
  eq(rewriteLinks(doc, () => ""), 3, "an empty string IS a replacement (counts)");
  eq(doc[0].attrs.href, "", "and it lands");
});

__kitS.section("/a.png 1x, /b.png 2x, /c.png", () => {
  const el = { name: "img", attrs: { srcset: "/a.png 1x, /b.png 2x, /c.png" },
               children: [] };
  const count = rewriteLinks(el, (url) => "https://cdn.test" + url);
  eq(count, 3, "each srcset candidate counts separately");
  eq(el.attrs.srcset,
     "https://cdn.test/a.png 1x, https://cdn.test/b.png 2x, https://cdn.test/c.png",
     "URLs rewritten, descriptors preserved, canonical comma spacing");
  el.attrs.srcset = "/a.png 1x, /b.png 2x";
  rewriteLinks(el, (url) => (url === "/b.png" ? "/B" : undefined));
  eq(el.attrs.srcset, "/a.png 1x, /B 2x", "kept candidate survives a partial rewrite");
  el.attrs.srcset = "/only.png";
  eq(rewriteLinks(el, () => "/x"), 1, "one candidate, one count");
  eq(el.attrs.srcset, "/x", "rewritten");
  el.attrs.srcset = 42;
  eq(rewriteLinks(el, () => "/x"), 0, "a non-string attr value is skipped");
});

__kitS.section("stop here", () => {
  const doc = [{ name: "a", attrs: { href: "/first" }, children: [
    { name: "img", attrs: { src: "/boom", data: "/never" }, children: [] }
  ]}];
  const msg = throws(() => rewriteLinks(doc, (url) => {
    if (url === "/boom") throw new Error("stop here");
    return "R:" + url;
  }), "fn throw propagates out of rewriteLinks");
  check(/stop here/.test(msg), "the original error travels");
  eq(doc[0].attrs.href, "R:/first", "the rewrite BEFORE the throw is applied");
  eq(doc[0].children[0].attrs.src, "/boom", "the throwing attribute is unchanged");
  eq(doc[0].children[0].attrs.data, "/never", "the walk STOPPED at the throw");
});

__kitS.section("a bare node is a valid doc", () => {
  const el = { name: "a", attrs: { href: "/x" }, children: [] };
  eq(rewriteLinks(el, () => "/y"), 1, "a bare node is a valid doc");
  const mixed = { name: "p", attrs: {}, children: ["text", 42, null,
    { name: "a", attrs: { href: "/z" }, children: [] }] };
  eq(rewriteLinks(mixed, () => "/w"), 1, "only element children are walked");
  let deep = { name: "a", attrs: { href: "/d" }, children: [] };
  for (let i = 0; i < 300; i++) deep = { name: "d", attrs: {}, children: [deep] };
  let deepErr = "none";
  try { rewriteLinks(deep, () => "/r"); } catch (e) { deepErr = e.name; }
  eq(deepErr, "RangeError", "a tree deeper than the parser's 256-level bound is refused, not partly rewritten");
  let shallow = { name: "a", attrs: { href: "/d" }, children: [] };
  for (let i = 0; i < 200; i++) shallow = { name: "d", attrs: {}, children: [shallow] };
  eq(rewriteLinks(shallow, () => "/r"), 1, "a 200-level tree is rewritten in full");
  eq(rewriteLinks({ name: "a", attrs: {}, children: [] }, () => "/r"), 0,
     "nothing to rewrite is a clean 0");
});

__kitS.section("tag and attr reach fn lower-cased on hand-built nodes too", () => {
  const node = { name: "IMG", attrs: { HREF: "/h" }, children: [] };
  const seen = [];
  rewriteLinks(node, (url, tag, attr) => { seen.push(tag + "/" + attr); return url; });
  eq(seen, ["img/href"],
     "tag and attr reach fn lower-cased on hand-built nodes too");
});
__kitS.end();

print("test_html_rewritelinks: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
