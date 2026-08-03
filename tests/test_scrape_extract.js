/* Extractor. The point is not that it extracts -- it is that LAYOUT DRIFT is
   loud. A selector that stops matching returns nothing, which looks exactly
   like a page with no such field; `required` is what separates them. */
import { Extractor } from "dyna:scrape";
import { HTMLParse, HTMLText, Selector } from "dyna:html";
let n = 0, bad = 0;
const ok = (c, w) => { n++; if (!c) { bad++; print("FAIL: " + w); } };

const doc = HTMLParse(
  '<div><h1>Widget</h1><span class="price">19.99</span>' +
  '<a href="/a">A</a><a href="/b">B</a></div>');

const ex = new Extractor({
  title: { sel: new Selector("h1"), required: true },
  price: { sel: new Selector(".price"), as: "number", required: true },
  links: { sel: new Selector("a"), attr: "href", all: true },
}, { text: HTMLText });

const r = ex.run(doc);
ok(r.ok === true, "ok when every required field matched");
ok(r.value.title === "Widget", "text extracted: " + JSON.stringify(r.value.title));
ok(r.value.price === 19.99, "as:number coerced: " + JSON.stringify(r.value.price));
ok(Array.isArray(r.value.links) && r.value.links.length === 2, "all:true collects");
ok(r.value.links[0] === "/a", "attr extracted");
ok(r.missing.length === 0, "nothing missing");

/* LAYOUT DRIFT: the selector no longer matches */
const ex2 = new Extractor({
  price: { sel: new Selector(".cost"), as: "number", required: true },
}, { text: HTMLText });
const r2 = ex2.run(doc);
ok(r2.ok === false, "drift makes ok false, not a silent null");
ok(r2.missing.length === 1 && r2.missing[0] === "price",
   "the missing field is NAMED: " + JSON.stringify(r2.missing));

/* a non-required field that misses is absent, not a failure */
const r3 = new Extractor({ x: { sel: new Selector(".nope") } },
                         { text: HTMLText }).run(doc);
ok(r3.ok === true, "optional miss is not a failure");

/* as:number must REFUSE a non-numeric string rather than yield NaN */
const doc2 = HTMLParse('<p class="price">not-a-number</p>');
const r4 = new Extractor({ p: { sel: new Selector(".price"), as: "number",
                                required: true } }, { text: HTMLText }).run(doc2);
ok(r4.ok === false, "non-numeric refused, ok=false");
ok(!Number.isNaN(r4.value.p), "never yields NaN, got " + JSON.stringify(r4.value.p));

/* a text field with no text function must throw, not silently return nothing */
let threw = 0;
try { new Extractor({ t: { sel: new Selector("h1") } }).run(doc); } catch (e) { threw = 1; }
ok(threw === 1, "text without an injected HTMLText throws");
/* a bad spec is refused at construction */
let threw2 = 0;
try { new Extractor({ a: { sel: "h1" } }); } catch (e) { threw2 = 1; }
ok(threw2 === 1, "a string where a Selector belongs is refused");

print("test_scrape_extract: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
