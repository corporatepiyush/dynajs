// covers: dyna:scrape.Robots
import { Extractor, Robots } from "dyna:scrape";
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

let threw = 0;

function throwsMsgSC(fn, re, msg) {
    let threw = null;
    try { fn(); } catch (e) { threw = e; }
    ok(threw !== null && threw instanceof TypeError && re.test(threw.message),
       msg + " (got " + (threw ? threw.name + ": " + threw.message : "no throw") + ")");
}

throwsMsgSC(() => new Robots("User-agent: *\nDisallow: /", { agnt: "b" }),
            /unknown option "agnt" \(valid: agent\)/, "Robots bag");

print("test_scrape_extract: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
