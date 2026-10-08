// 33 · Product page extractor — pulls structured data out of HTML with CSS selectors, then validates it.
//
// WHAT IT SHOWS
//   - dyna:html HTMLParse + Selector: tolerant parsing of real-world markup
//   - dyna:scrape Extractor: a declarative field spec (required fields, numbers, URLs, lists)
//   - reading embedded JSON-LD, which is usually more reliable than visible text
//   - resolving relative links against the page URL, and rejecting half-parsed records
//
// RUN      dynajs examples/apps/33-product-page-extractor.js
//          Pages are inlined so the example runs offline; feed it Fetcher results in production.

import { HTMLParse, HTMLText, Selector } from "dyna:html";
import { Extractor } from "dyna:scrape";

// One spec describes the record. `required` fields make the result "not ok"
// when the page layout changes, instead of producing quietly empty rows.
const product = new Extractor({
    name:        { sel: new Selector("h1.product-title"), required: true },
    price:       { sel: new Selector("[data-price]"), attr: "data-price", as: "number", required: true },
    currency:    { sel: new Selector("meta[itemprop=priceCurrency]"), attr: "content", default: "USD" },
    image:       { sel: new Selector("img.hero"), attr: "src", as: "url" },
    features:    { sel: new Selector("ul.features li"), all: true },
    breadcrumbs: { sel: new Selector("nav.crumbs a"), all: true },
    structured:  { sel: new Selector('script[type="application/ld+json"]'), source: true, as: "json" },
}, { text: HTMLText });

function extract(html, pageUrl) {
    const doc = HTMLParse(html);
    const result = product.run(doc, { base: pageUrl });
    if (!result.ok) return { ok: false, url: pageUrl, missing: result.missing };

    const v = result.value;
    // Prefer the machine-readable block when the page has one.
    const ld = v.structured && v.structured["@type"] === "Product" ? v.structured : null;
    return {
        ok: true, url: pageUrl,
        record: {
            name: v.name.trim(),
            sku: ld?.sku ?? null,
            price: v.price, currency: v.currency,
            inStock: ld ? /InStock$/.test(ld.offers?.availability ?? "") : null,
            image: v.image ?? null,
            category: v.breadcrumbs.slice(1).join(" > "),
            features: v.features.map((f) => f.trim()).filter(Boolean),
        },
    };
}

// ---- sample pages ----------------------------------------------------------
const goodPage = `<!doctype html><html><head>
<meta itemprop="priceCurrency" content="EUR">
<script type="application/ld+json">{"@type":"Product","sku":"KB-75-BLK","offers":{"availability":"https://schema.org/InStock"}}</script>
</head><body>
<nav class="crumbs"><a href="/">Home</a><a href="/c/keyboards">Keyboards</a><a href="/c/keyboards/mech">Mechanical</a></nav>
<h1 class="product-title">  Compact 75% Keyboard </h1>
<img class="hero" src="/img/kb-75.jpg" alt="">
<p class="price"><span data-price="129.90">129,90 €</span></p>
<ul class="features"><li>Hot-swap switches</li><li>USB-C</li><li> PBT keycaps </li></ul>
<p>Unclosed paragraph and stray </div> tags are common in the wild
</body></html>`;

// The same shop after a redesign: the price moved and lost its attribute.
const redesigned = `<html><body><h1 class="product-title">Compact 75% Keyboard</h1>
<div class="price-box">129,90 €</div></body></html>`;

const noStructuredData = `<html><body><h1 class="product-title">USB-C Cable</h1>
<b data-price="9.5">9.50</b><ul class="features"><li>2 m</li></ul></body></html>`;

// ---- run -------------------------------------------------------------------
const pages = [
    ["https://shop.example/p/kb-75", goodPage],
    ["https://shop.example/p/kb-75-new", redesigned],
    ["https://shop.example/p/cable", noStructuredData],
];
const results = pages.map(([url, html]) => extract(html, url));
for (const r of results)
    console.log(r.ok ? `ok      ${r.record.name} — ${r.record.price} ${r.record.currency}` : `FAILED  ${r.url} (missing: ${r.missing.join(", ")})`);

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const [kb, broken, cable] = results;
check(kb.ok && kb.record.name === "Compact 75% Keyboard", "the name is extracted and trimmed");
check(kb.record.price === 129.9 && kb.record.currency === "EUR", "the price comes from the attribute, as a number");
check(kb.record.image === "https://shop.example/img/kb-75.jpg", "relative image URLs are resolved against the page");
check(kb.record.sku === "KB-75-BLK" && kb.record.inStock === true, "JSON-LD supplies sku and stock");
check(kb.record.category === "Keyboards > Mechanical", "breadcrumbs become a category path");
check(kb.record.features.join("|") === "Hot-swap switches|USB-C|PBT keycaps", "list items are collected in order");
check(!broken.ok && broken.missing.includes("price"), "a redesigned page fails loudly, naming the missing field");
check(cable.ok && cable.record.currency === "USD" && cable.record.sku === null, "defaults apply when optional data is absent");
console.log("self-test passed: 2 extracted, 1 rejected with a reason");
