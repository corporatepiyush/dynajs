// covers: dyna:html.HTMLParse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_html_parse_rewritelinks");
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

__kitS.section("<a href=\"/p?x=1\"><img src=\"/i.png\" srcset=\"/2x.png 2x\"></a>", () => {
  const doc = HTMLParse('<a href="/p?x=1"><img src="/i.png" srcset="/2x.png 2x"></a>');
  const count = rewriteLinks(doc, (url) => "https://ex.test" + url);
  check(count === 3, "parsed tree: 3 URL attrs (href/src/srcset candidate)");
  const html = HTMLStringify(doc[0]);
  check(/https:\/\/ex\.test\/p\?x=1/.test(html), "and the serialization carries it");
});

__kitS.section("<img srcset=\"data:image/png;base64,AAAA 1x, /ok.png 2x\">", () => {
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
});

__kitS.section("<img srcset='a.png 1x,b.png 2x'>", () => {
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
});
__kitS.end();

print("test_html_rewritelinks: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
