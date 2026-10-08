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

{
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
}

{
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
}

{
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
}

{
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
}

{
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
}

{
  const doc = HTMLParse('<a href="/p?x=1"><img src="/i.png" srcset="/2x.png 2x"></a>');
  const count = rewriteLinks(doc, (url) => "https://ex.test" + url);
  check(count === 3, "parsed tree: 3 URL attrs (href/src/srcset candidate)");
  const html = HTMLStringify(doc[0]);
  check(/https:\/\/ex\.test\/p\?x=1/.test(html), "and the serialization carries it");
}

{
  const doc = HTMLParse('<img srcset="data:image/png;base64,AAAA 1x, /ok.png 2x">');
  const seen = [];
  rewriteLinks(doc, (url) => { seen.push(url); return undefined; });
  eq(seen, ["data:image/png;base64,AAAA", "/ok.png"],
     "srcset: the data: URI is ONE candidate and keeps its comma intact");
  eq(doc[0].attrs.srcset, "data:image/png;base64,AAAA 1x, /ok.png 2x",
     "and an identity rewrite leaves the value byte-intact");

  const doc2 = HTMLParse('<img srcset="/a,b.png 1x">');
  const seen2 = [];
  rewriteLinks(doc2, (url) => { seen2.push(url); return "R:" + url; });
  eq(seen2, ["/a,b.png"], "srcset: a comma path is ONE candidate");
  eq(doc2[0].attrs.srcset, "R:/a,b.png 1x",
     "and the rebuilt candidate keeps its descriptor uncorrupted");

  const doc3 = HTMLParse('<img srcset="/a.png 1x,/b.png 2x, /c.png 3w">');
  const seen3 = [];
  rewriteLinks(doc3, (url) => { seen3.push(url); return url; });
  eq(seen3, ["/a.png", "/b.png", "/c.png"],
     "srcset: candidates split at descriptor-closing commas");

  const doc4 = HTMLParse('<img srcset="one.png, two.png 2x">');
  const seen4 = [];
  rewriteLinks(doc4, (url) => { seen4.push(url); return undefined; });
  eq(seen4, ["one.png", "two.png"],
     "srcset: a trailing comma ends a descriptor-less candidate");

  const doc5 = HTMLParse('<img srcset="/x.png type(a,b) 1x, /y.png 2x">');
  const seen5 = [];
  rewriteLinks(doc5, (url) => { seen5.push(url); return undefined; });
  eq(seen5, ["/x.png", "/y.png"],
     "srcset: a comma inside descriptor parens does not split candidates");
}

{
  const node = { name: "IMG", attrs: { HREF: "/h" }, children: [] };
  const seen = [];
  rewriteLinks(node, (url, tag, attr) => { seen.push(tag + "/" + attr); return url; });
  eq(seen, ["img/href"],
     "tag and attr reach fn lower-cased on hand-built nodes too");
}

{
  const doc = HTMLParse("<img srcset='a.png 1x,b.png 2x'>");
  eq(rewriteLinks(doc, (u) => "R" + u), 2, "comma-only separator: 2 candidates");
  eq(doc[0].attrs.srcset, "Ra.png 1x, Rb.png 2x",
     "comma-only separators normalize to ', ' when >=1 candidate is replaced");

  const docK = HTMLParse("<img srcset='a.png 1x,b.png 2x'>");
  eq(rewriteLinks(docK, () => undefined), 0, "keep-all: nothing replaced");
  eq(docK[0].attrs.srcset, "a.png 1x,b.png 2x",
     "keep-all leaves the attribute byte-identical (no normalization)");

  const doc2 = HTMLParse('<img srcset="a.png 1x,">');
  rewriteLinks(doc2, (u) => "R" + u);
  eq(doc2[0].attrs.srcset, "Ra.png 1x",
     "a trailing comma-ws run drops when >=1 candidate is replaced");

  const doc3 = HTMLParse('<img srcset="   a.png 1x">');
  rewriteLinks(doc3, (u) => "R" + u);
  eq(doc3[0].attrs.srcset, "Ra.png 1x",
     "leading whitespace drops when >=1 candidate is replaced");

  const doc4 = HTMLParse('<img srcset="a.png 1x, , b.png 2x ,">');
  rewriteLinks(doc4, (u) => "R" + u);
  eq(doc4[0].attrs.srcset, "Ra.png 1x, Rb.png 2x ",
     "separator runs collapse to ', '; descriptor spans keep their bytes");

  const doc5 = HTMLParse('<img srcset="a.png 1x,b.png 2x,c.png 3x">');
  eq(rewriteLinks(doc5, (u) => (u === "b.png" ? "RB" : undefined)), 1,
     "partial rewrite: exactly one candidate replaced");
  eq(doc5[0].attrs.srcset, "a.png 1x, RB 2x, c.png 3x",
     "and every separator normalizes to ', ' across the attribute");

  const doc6 = HTMLParse('<img srcset="/a.png 1x ,/b.png 2x\t,  /c.png">');
  eq(rewriteLinks(doc6, () => "X"), 3, "mixed separators: 3 candidates");
  eq(doc6[0].attrs.srcset, "X 1x , X 2x\t, X",
     "descriptor spans keep their exact bytes; joins use ', '");
}

print("test_html_rewritelinks: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
