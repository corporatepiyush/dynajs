// covers: dyna:scrape.Extractor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_scrape_extractor_modern");
import { Robots, Extractor, Fetcher, Crawl } from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const eq = (a, b, w) => ok(a === b, w, "got [" + String(a) + "] want [" + String(b) + "]");
function throws(fn, w) {
    let t = false, m = "";
    try { fn(); } catch (e) { t = true; m = String(e.message || e); }
    ok(t, w, t ? "" : "did NOT throw");
    return m;
}
const pathOf = (u) => {
    const s = u.indexOf("://");
    if (s < 0) return u;
    const slash = u.indexOf("/", s + 3);
    return slash < 0 ? "/" : u.slice(slash);
};

await __kitS.section("<div class=\"p\"> hi </div>", async () => {
    const doc = HTMLParse(
        '<div class="p">  hi </div>' +
        '<span class="n"> 41.5 </span>' +
        '<script type="application/ld+json">{"@type":"Thing","v":7}</script>' +
        '<div data-cfg=\'{"a":[1,2]}\'></div>' +
        '<style>.x{color:red}</style>' +
        '<span class="bad">{nope</span>');

    const ex = new Extractor({
        padded: { sel: new Selector(".p"), trim: true },
        missD:  { sel: new Selector(".nothing"), default: "fallback" },
        num:    { sel: new Selector(".n"), as: "number", trim: true },
        ld:     { sel: new Selector("script[type='application/ld+json']"),
                  source: true, as: "json" },
        cssrc:  { sel: new Selector("style"), source: true, trim: true },
        attrj:  { sel: new Selector("[data-cfg]"), attr: "data-cfg",
                  as: "json" },
        bad:    { sel: new Selector(".bad"), as: "json" }
    }, { text: HTMLText });
    const r = ex.run(doc, { base: "http://b.test/x/y" });
    eq(r.value.padded, "hi", "trim strips surrounding whitespace");
    eq(r.value.missD, "fallback", "default substitutes for an empty field");
    ok(!r.missing.includes("missD"), "a defaulted field is not missing");
    eq(r.value.num, 41.5, "trim-then-coerce yields the number");
    ok(r.value.ld && r.value.ld.v === 7 && r.value.ld["@type"] === "Thing",
       "source:true + as:\"json\" reads ld+json script SOURCE");
    eq(r.value.cssrc, ".x{color:red}",
       "source reads style bodies (HTMLText would refuse)");
    ok(r.value.attrj && r.value.attrj.a[1] === 2,
       "as:\"json\" parses JSON carried in an attribute");
    eq(r.value.bad, undefined, "invalid JSON is a refusal, not an exception");
    const txt = new Extractor({ t: { sel: new Selector("body") } },
                              { text: HTMLText }).run(doc);
    ok(!/color|@type/.test(String(txt.value.t)),
       "CONTROL: HTMLText still excludes script/style from text");

    const d2 = HTMLParse('<a class="r" href="z/w">1</a>' +
                         '<a class="a" href="https://ok.test/x">2</a>');
    const u = new Extractor({
        rel: { sel: new Selector("a.r"), attr: "href", as: "url" },
        abs: { sel: new Selector("a.a"), attr: "href", as: "url" }
    }, { text: HTMLText });
    const ru = u.run(d2, { base: "http://b.test/x/y" });
    eq(ru.value.rel, "http://b.test/x/z/w", "as:url resolves against base");
    eq(ru.value.abs, "https://ok.test/x", "absolute urls pass untouched");
    const nob = u.run(d2);
    eq(nob.value.rel, undefined, "relative url without base is refused");

    const rd = new Extractor({
        opt: { sel: new Selector(".gone"), default: 0, required: true }
    }, { text: HTMLText });
    ok(rd.run(doc).ok, "required is satisfied by its default");

    const m = (() => { try { new Extractor({ x: { sel: new Selector("i"),
                                                  as: "date" } }); return ""; }
                       catch (e) { return String(e.message); } })();
    ok(/date/.test(m) && /number.*url.*json/.test(m),
       "unsupported coercion refused AT CONSTRUCTION, naming alternatives");
    const m2 = (() => { try { new Extractor({ y: { sel: new Selector("i"),
                                                   as: "numbr" } }); return ""; }
                        catch (e) { return String(e.message); } })();
    ok(/y/.test(m2) && /numbr/.test(m2) && /number.*url.*json/.test(m2),
       "a TYPO'd coercion refuses just as loudly (silent drift is the bug)");
    ok((() => { try { new Extractor({ z: { sel: new Selector("i"),
                                           as: "number" } }); return true; }
                catch (e) { return false; } })(),
       "CONTROL: the supported spellings still construct");
});
__kitS.end();

print("test_scrape_modern: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_scrape_modern: " + fail + " failures");
