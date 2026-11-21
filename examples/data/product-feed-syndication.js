// Product feed syndication — merge supplier feeds in three formats into one catalog and publish channel feeds.
//
// WHAT IT IS
//   The job a retailer runs every night to keep marketplaces and ad channels in sync:
//
//   ingest      supplier A sends CSV, supplier B a large XML file (read with the streaming SAX parser so it is never
//               held as a tree), supplier C a JSON API page set
//   normalise   one product shape: every GTIN of the night is check-digit validated in ONE gemv call (digits x weights),
//               prices to EUR with Decimal at the day's rates, supplier category paths mapped onto the store taxonomy by
//               longest matching prefix (Trie), image URLs forced to https, promotional noise stripped from titles by a
//               single Aho-Corasick pass (MultiMatcher) then compacted and cut on a word boundary
//   merge       the same GTIN from several suppliers becomes one product in an OrderedMap (so the feed comes out sorted
//               by GTIN with no sort): the cheapest in-stock offer wins, stock is summed, the richest description kept
//   guard       a TDigest per category flags offers priced at more than 4x or under 1/4 of the category median — the
//               classic "decimal point lost in the supplier's export" error — before they reach a channel
//   validate    each product against the channel schema; rejects go to a report with the reason per field (Multiset)
//   delta       GTINs interned to dense ids; added/removed are RoaringBitmap set differences, changed is a content hash
//   delta feed  channels that cannot take a full feed every night get upserts and deletes only; applying night 2's
//               delta to night 1's catalog must reproduce night 2 exactly
//   search      an InvertedIndex (BM25) over titles, brands and categories backs a /search endpoint with suggestions
//   oracles     GS1 check digits catch every single-digit error and every adjacent transposition except digits 5
//               apart (the weights 3 and 1 differ by 2, and 2 x 5 = 10) -- checked exhaustively on 40 codes through
//               the gemv path; EUR prices re-derived in BigInt cents with half-up rounding, including every exact
//               half-cent tie below 2000.00 (where a float multiply-and-round goes wrong); BM25 scores re-derived
//               from the formula (k1 1.2, b 0.75)
//   publish     a Google-Merchant-style RSS/XML feed (gzip), a marketplace CSV, and an HTTP endpoint serving the feed
//               with an ETag so channels can poll with If-None-Match; per-channel price statistics come from dyna:simd
//
// MODULES  dyna:csv · dyna:xml (SAXParser, XmlWriter) · dyna:decimal · dyna:structures (Trie, OrderedMap, TDigest,
//          Interner, RoaringBitmap, InvertedIndex, TopK, Multiset) · dyna:matcher (MultiMatcher) · dyna:simd (gemv,
//          f64Mean/Min/Max) · dyna:url · dyna:schema · dyna:serialize (ValueHash) · dyna:compress (gzip) · dyna:http
//          · dyna:file · dyna:random · String extensions (compact, truncateOnWord) · Number extensions (bytes, format)
//
// RUN      dynajs examples/data/product-feed-syndication.js     builds three supplier feeds, runs two nights, serves

import * as csv from "dyna:csv";
import { SAXParser, XmlWriter } from "dyna:xml";
import { Decimal } from "dyna:decimal";
import { Trie, OrderedMap, TDigest, Interner, RoaringBitmap, InvertedIndex, TopK, Multiset } from "dyna:structures";
import { MultiMatcher } from "dyna:matcher";
import * as simd from "dyna:simd";
import { URL } from "dyna:url";
import { Schema } from "dyna:schema";
import { ValueHash } from "dyna:serialize";
import { gzip, gunzip } from "dyna:compress";
import { App } from "dyna:http";
import { makeTempDir, writeFile, readFile, readBytes, removeAll } from "dyna:file";
import { Random } from "dyna:random";

const work = makeTempDir("feeds");
const RATES = { EUR: "1", USD: "0.9182", GBP: "1.1690" };

// =============================================================================
// Store taxonomy and supplier category mapping (longest prefix wins)
// =============================================================================
const CATEGORY_MAP = {
    electronics: "Electronics",
    "electronics/audio": "Electronics > Audio",
    "electronics/audio/headphones": "Electronics > Audio > Headphones",
    "electronics/computers": "Electronics > Computers",
    "electronics/computers/laptops": "Electronics > Computers > Laptops",
    home: "Home & Garden",
    "home/kitchen": "Home & Garden > Kitchen",
    "home/kitchen/coffee": "Home & Garden > Kitchen > Coffee Makers",
    sport: "Sporting Goods",
    "sport/cycling": "Sporting Goods > Cycling",
};
const catTrie = new Trie();
for (const k of Object.keys(CATEGORY_MAP)) catTrie.insert(k + "/");
const mapCategory = (path) => {
    const norm =
        path
            .toLowerCase()
            .replace(/\s*[>|]\s*/g, "/")
            .replace(/\s+/g, "-")
            .replace(/&/g, "and") + "/";
    const hit = catTrie.longestPrefix(norm);
    return hit ? CATEGORY_MAP[hit.slice(0, -1)] : null;
};

// =============================================================================
// GTIN validation in bulk: every code of the night is one row of a digit matrix,
// and one gemv against the GS1 weight vector (3,1,3,1,... from the right) yields
// every weighted sum at once. The sums are small integers, exact in float32.
// =============================================================================
const GTIN_BODY = 13;
const GTIN_WEIGHTS = Float32Array.from({ length: GTIN_BODY }, (_, i) => ((GTIN_BODY - 1 - i) % 2 === 0 ? 3 : 1));
function validateGtins(codes) {
    const m = codes.length;
    const digits = new Float32Array(m * GTIN_BODY);
    const checks = new Int32Array(m);
    const shapeOk = new Uint8Array(m);
    for (let r = 0; r < m; r++) {
        const g = codes[r];
        if (g.length !== 14 || !/^\d{14}$/.test(g)) continue;
        shapeOk[r] = 1;
        for (let i = 0; i < GTIN_BODY; i++) digits[r * GTIN_BODY + i] = g.charCodeAt(i) - 48;
        checks[r] = g.charCodeAt(GTIN_BODY) - 48;
    }
    const sums = simd.gemv(new Float32Array(m), digits, GTIN_WEIGHTS, m, GTIN_BODY, 0);
    return codes.map((_, r) => shapeOk[r] === 1 && (10 - (sums[r] % 10)) % 10 === checks[r]);
}

// =============================================================================
// Normalisation
// =============================================================================
const PROMO = ["BEST PRICE", "Best Price", "best price", "FREE SHIPPING", "Free Shipping", "free shipping", "!!"];
const promo = new MultiMatcher(PROMO);
const cleanTitle = (t) => promo.replaceAllIn(t, "").compact().trimChars(" -!").truncateOnWord(150, "right", "");
const toEur = (amount, currency) => {
    if (!(currency in RATES)) throw new Error(`no rate for ${currency}`);
    return new Decimal(String(amount).replace(",", ".")).mul(RATES[currency]).toFixed(2);
};
const httpsImage = (u) => {
    const fixed = u.replace(/^http:\/\//i, "https://");
    try {
        return new URL(fixed).protocol === "https:" ? fixed : null;
    } catch {
        return null;
    }
};
const rejects = [];
const reject = (supplier, sku, reason, kind) => rejects.push({ supplier, sku, reason, kind });
function normaliseAll(offers) {
    const gtins = offers.map(([, raw]) =>
        String(raw.gtin || "")
            .replace(/\D/g, "")
            .padStart(14, "0"),
    );
    const valid = validateGtins(gtins);
    const out = [];
    offers.forEach(([supplier, raw], i) => {
        if (!valid[i]) return reject(supplier, raw.sku, `GTIN ${raw.gtin} fails its check digit`, "gtin");
        let price;
        try {
            price = toEur(raw.price, raw.currency);
        } catch (e) {
            return reject(supplier, raw.sku, e.message, "currency");
        }
        const gtin = gtins[i].slice(1);
        out.push({
            gtin,
            supplier,
            sku: raw.sku,
            title: cleanTitle(raw.title || ""),
            description: (raw.description || "").compact(),
            brand: (raw.brand || "").trim(),
            price,
            stock: Number(raw.stock) || 0,
            category: mapCategory(raw.category || ""),
            image: httpsImage(raw.image || ""),
            link: `https://shop.example/p/${gtin}`,
        });
    });
    return out;
}

// =============================================================================
// Readers: CSV, streaming XML, JSON pages
// =============================================================================
function readCsv(path) {
    const { headers, rows } = csv.parse(readFile(path), { hasHeader: true });
    return rows.map((r) => headers.zipObj(r));
}
function readXmlStream(path) {
    const items = [];
    let cur = null;
    let field = null;
    let text = "";
    const sax = new SAXParser({
        onOpen(name, attrs) {
            if (name === "item") cur = { currency: attrs.currency || "EUR" };
            else if (cur) {
                field = name;
                text = "";
                if (name === "price" && attrs.currency) cur.currency = attrs.currency;
            }
        },
        onText(t) {
            if (field) text += t;
        },
        onCData(t) {
            if (field) text += t;
        },
        onClose(name) {
            if (name === "item") {
                items.push(cur);
                cur = null;
            } else if (cur && name === field) {
                cur[field] = text.trim();
                field = null;
            }
        },
    });
    const bytes = new TextEncoder().encode(readFile(path));
    for (let off = 0; off < bytes.length; off += 4096) sax.write(bytes.subarray(off, off + 4096)); // as if from a socket
    sax.end();
    return items;
}
const readJsonPages = (paths) =>
    paths.flatMap((p) =>
        JSON.parse(readFile(p)).data.map((d) => ({
            sku: d.id,
            gtin: d.attributes.ean,
            title: d.attributes.name,
            brand: d.attributes.manufacturer,
            price: d.attributes.pricing.amount,
            currency: d.attributes.pricing.currency,
            stock: d.attributes.inventory,
            category: d.attributes.taxonomy.join(" > "),
            image: d.attributes.media[0],
            description: d.attributes.body,
        })),
    );

// =============================================================================
// Merge, price guard, validate, delta, search, publish
// =============================================================================
const channelSchema = Schema.compile({
    type: "object",
    required: ["gtin", "title", "price", "category", "image", "brand"],
    properties: {
        gtin: { type: "string", pattern: "^\\d{13,14}$" },
        title: { type: "string", minLength: 5, maxLength: 150 },
        price: { type: "string", pattern: "^\\d+\\.\\d{2}$" },
        category: { type: "string" },
        image: { type: "string", pattern: "^https://" },
        brand: { type: "string", minLength: 1 },
    },
});
function merge(products) {
    const byGtin = new OrderedMap();
    for (const p of products) {
        const cur = byGtin.get(p.gtin);
        if (!cur) {
            byGtin.set(p.gtin, { ...p, offers: 1, sources: [p.supplier] });
            continue;
        }
        const cheaper = p.stock > 0 && (cur.stock === 0 || new Decimal(p.price).cmp(cur.price) < 0);
        const merged = cheaper ? { ...cur, price: p.price, supplier: p.supplier, sku: p.sku } : cur;
        merged.stock = cur.stock + p.stock;
        merged.offers = cur.offers + 1;
        merged.sources = cur.sources.append(p.supplier);
        if (p.description.length > merged.description.length) merged.description = p.description;
        merged.category ||= p.category;
        merged.image ||= p.image;
        merged.brand ||= p.brand;
        byGtin.set(p.gtin, merged);
    }
    return byGtin;
}
function priceGuard(products) {
    const digests = new Map();
    for (const p of products) {
        const d = digests.getOrInsertComputed(p.category || "?", () => new TDigest(50));
        d.add(Number(p.price));
    }
    const medians = new Map([...digests].map(([c, d]) => [c, d.quantile(0.5)]));
    const [ok, flagged] = products.partition((p) => {
        const ratio = Number(p.price) / medians.get(p.category || "?");
        return ratio <= 4 && ratio >= 0.25;
    });
    for (const p of flagged)
        reject(
            p.sources.join("+"),
            p.sku,
            `price ${p.price} is ${(Number(p.price) / medians.get(p.category)).round(1)}x the category median`,
            "price",
        );
    return ok;
}
const interner = new Interner();
function buildSearch(products) {
    const index = new InvertedIndex();
    for (const p of products) index.add(interner.intern(p.gtin), `${p.title} ${p.brand} ${p.category}`);
    return index;
}
function publish(products, night) {
    const w = new XmlWriter({ indent: 1 });
    w.open("rss", { version: "2.0", "xmlns:g": "http://base.google.com/ns/1.0" });
    w.open("channel");
    for (const [k, v] of [
        ["title", "Example Shop"],
        ["link", "https://shop.example"],
        ["description", `Nightly feed ${night}`],
    ]) {
        w.open(k);
        w.text(v);
        w.close();
    }
    for (const p of products) {
        w.open("item");
        for (const [k, v] of [
            ["g:id", p.gtin],
            ["g:title", p.title],
            ["g:description", p.description.truncateOnWord(500)],
            ["g:link", p.link],
            ["g:image_link", p.image],
            ["g:brand", p.brand],
            ["g:gtin", p.gtin],
            ["g:price", `${p.price} EUR`],
            ["g:availability", p.stock > 0 ? "in_stock" : "out_of_stock"],
            ["g:google_product_category", p.category],
        ]) {
            w.open(k);
            w.text(String(v));
            w.close();
        }
        w.close();
    }
    w.close();
    w.close();
    const xml = w.finish();
    const market = csv.stringify([
        ["ean", "title", "price_eur", "qty", "category"],
        ...products.map((p) => [p.gtin, p.title, p.price, String(p.stock), p.category]),
    ]);
    writeFile(work.join(`feed-${night}.xml.gz`), gzip(xml));
    writeFile(work.join(`marketplace-${night}.csv`), market);
    const prices = Float64Array.from(products, (p) => Number(p.price));
    const stats = { mean: simd.f64Mean(prices), min: simd.f64Min(prices), max: simd.f64Max(prices) };
    return { xml, market, stats };
}
function runNight(night, inputs, previous) {
    rejects.length = 0;
    const offers = [
        ...readCsv(inputs.a).map((r) => ["alpha", r]),
        ...readXmlStream(inputs.b).map((r) => ["bravo", r]),
        ...readJsonPages(inputs.c).map((r) => ["charlie", r]),
    ];
    const raw = normaliseAll(offers);
    const merged = merge(raw);
    const guarded = priceGuard(merged.entries().map(([, p]) => p));
    const valid = [];
    for (const p of guarded) {
        const r = channelSchema.validate(p);
        if (r.valid) valid.push(p);
        else
            reject(
                p.sources.join("+"),
                p.sku,
                r.errors.map((e) => `${e.path || e.instancePath || ""} ${e.message}`).join("; "),
                "schema",
            );
    }
    const hashes = new Map(valid.map((p) => [p.gtin, ValueHash({ ...p, sources: undefined })]));
    const ids = new RoaringBitmap(interner.internMany(valid.map((p) => p.gtin)));
    const prevIds = previous.ids || new RoaringBitmap();
    const delta = {
        added: interner.getMany(ids.clone().andNot(prevIds).toArray()),
        removed: interner.getMany(prevIds.clone().andNot(ids).toArray()),
        changed: valid
            .filter((p) => previous.hashes?.has(p.gtin) && previous.hashes.get(p.gtin) !== hashes.get(p.gtin))
            .pluck("gtin"),
    };
    const brands = new TopK(3);
    for (const p of raw) brands.add(p.brand);
    const reasons = new Multiset();
    for (const r of rejects) reasons.add(r.kind);
    const out = publish(valid, night);
    return {
        raw: raw.length,
        merged: merged.size,
        valid,
        rejects: [...rejects],
        reasons,
        brands,
        delta,
        hashes,
        ids,
        search: buildSearch(valid),
        ...out,
    };
}

// The delta feed: every added or changed product in full, every removed GTIN by id
const PROJECT = ["gtin", "title", "price", "stock", "category", "image", "brand", "description", "link"];
function deltaFeed(night, previous) {
    const w = new XmlWriter({ indent: 0 });
    w.open("delta", { night: String(night.n) });
    const byGtin = new Map(night.valid.map((p) => [p.gtin, p]));
    for (const g of [...night.delta.added, ...night.delta.changed]) {
        w.open("upsert");
        for (const k of PROJECT) {
            w.open(k);
            w.text(String(byGtin.get(g)[k] ?? ""));
            w.close();
        }
        w.close();
    }
    for (const g of night.delta.removed) {
        w.open("delete", { gtin: g });
        w.close();
    }
    w.close();
    return w.finish();
}
function applyDelta(catalogByGtin, xml) {
    const out = new Map(catalogByGtin);
    let cur = null,
        field = null,
        text = "";
    const sax = new SAXParser({
        onOpen(name, attrs) {
            if (name === "upsert") cur = {};
            else if (name === "delete") out.delete(attrs.gtin);
            else if (cur) {
                field = name;
                text = "";
            }
        },
        onText(t) {
            if (field) text += t;
        },
        onClose(name) {
            if (name === "upsert") {
                cur.stock = Number(cur.stock);
                out.set(cur.gtin, cur);
                cur = null;
            } else if (cur && name === field) {
                cur[field] = text;
                field = null;
            }
        },
    });
    sax.write(new TextEncoder().encode(xml));
    sax.end();
    return out;
}
const projection = (p) => JSON.stringify(PROJECT.map((k) => (k === "stock" ? Number(p[k]) : String(p[k] ?? ""))));

// =============================================================================
// Two nights of supplier data
// =============================================================================
const rng = new Random(11);
const withCheck = (body12) => {
    const d = body12.split("").map(Number);
    const s = d.reverse().reduce((a, x, i) => a + x * (i % 2 === 0 ? 3 : 1), 0);
    return body12 + ((10 - (s % 10)) % 10);
};
const KINDS = [
    ["Wireless Headphones", "Sonora", ["Electronics", "Audio", "Headphones"]],
    ['Laptop Sleeve 14"', "Packwell", ["Electronics", "Computers", "Laptops", "Sleeves"]],
    ["Espresso Machine", "Brewmaster", ["Home", "Kitchen", "Coffee"]],
    ["Bike Light Set", "Lumen", ["Sport", "Cycling", "Lights"]],
    ["USB-C Hub", "Portix", ["Electronics", "Computers"]],
    ["Pour-over Kettle", "Kettlo", ["Home", "Kitchen"]],
];
const catalog = Array.from({ length: 160 }, (_, i) => ({
    gtin: withCheck(String(400638133000 + i * 7)),
    title: KINDS[i % 6][0] + ` Model ${100 + i}`,
    brand: KINDS[i % 6][1],
    path: KINDS[i % 6][2],
    base: 40 + rng.nextBounded(60) + rng.nextBounded(100) / 100,
}));
function makeInputs(night) {
    const a = [["sku", "gtin", "title", "brand", "price", "currency", "stock", "category", "image", "description"]];
    const bItems = [];
    const cPages = [[], []];
    for (const [i, p] of catalog.entries()) {
        if (night === 2 && i % 40 === 0) continue; // discontinued on night 2
        const drift = night === 2 && i % 25 === 3 ? 1.1 : 1; // a price change
        if (i % 3 !== 2)
            a.push([
                `A-${i}`,
                p.gtin,
                p.title + (i % 10 === 0 ? "  BEST PRICE!!!" : i % 10 === 5 ? " - Free Shipping" : ""),
                p.brand,
                (p.base * drift).toFixed(2).replace(".", ","),
                "EUR",
                String(rng.nextBounded(30)),
                p.path.join(" > "),
                `http://img.alpha.example/${i}.jpg`,
                `${p.title}   by ${p.brand}.`,
            ]);
        if (i % 2 === 0)
            bItems.push(
                `<item><sku>B-${i}</sku><gtin>${p.gtin}</gtin><title><![CDATA[${p.title} & accessories]]></title><brand>${p.brand}</brand>` +
                    `<price currency="USD">${(((p.base * drift) / 0.9182) * 0.97).toFixed(2)}</price><stock>${i % 8 === 0 ? 0 : 5}</stock><category>${p.path.join(" | ")}</category>` +
                    `<image>https://cdn.bravo.example/${i}.png</image><description>${p.title} from ${p.brand}. Includes carry case, cable and a two-year warranty.</description></item>`,
            );
        if (i % 5 === 1)
            cPages[i % 2].push({
                id: `C-${i}`,
                attributes: {
                    ean: p.gtin,
                    name: p.title,
                    manufacturer: p.brand,
                    pricing: { amount: (((p.base * drift) / 1.169) * 1.02).toFixed(2), currency: "GBP" },
                    inventory: 3,
                    taxonomy: p.path,
                    media: [`https://charlie.example/m/${i}.webp`],
                    body: p.title,
                },
            });
    }
    a.push(["A-bad", "4006381330019", "Broken Barcode", "X", "9,99", "EUR", "1", "Home", "https://x/1.jpg", ""]);
    a.push(["A-yen", catalog[0].gtin, "Imported", "Y", "1200", "JPY", "1", "Home", "https://x/2.jpg", ""]);
    a.push([
        "A-ftp",
        withCheck("400638139999"),
        "Odd Image Product",
        "Z",
        "5,00",
        "EUR",
        "2",
        "Toys > Puzzles",
        "ftp://img/3.jpg",
        "",
    ]);
    a.push([
        "A-dec",
        withCheck("400638139990"),
        "Espresso Machine Deluxe",
        "Brewmaster",
        "7990,00",
        "EUR",
        "4",
        "Home > Kitchen > Coffee",
        "https://x/4.jpg",
        "decimal point lost",
    ]);
    const paths = {
        a: work.join(`a-${night}.csv`),
        b: work.join(`b-${night}.xml`),
        c: [work.join(`c-${night}-1.json`), work.join(`c-${night}-2.json`)],
    };
    writeFile(paths.a, csv.stringify(a));
    writeFile(paths.b, `<?xml version="1.0"?>\n<catalog supplier="bravo">${bItems.join("\n")}</catalog>`);
    cPages.forEach((data, k) => writeFile(paths.c[k], JSON.stringify({ data, meta: { page: k + 1 } })));
    return paths;
}

const inputs1 = makeInputs(1),
    inputs2 = makeInputs(2);
const n1 = { n: 1, ...runNight(1, inputs1, {}) };
const n2 = { n: 2, ...runNight(2, inputs2, n1) };
const delta2 = deltaFeed(n2, n1);

const app = new App({ port: 0 });
const current = n2.xml;
const etag = `"${ValueHash(current).slice(0, 16)}"`;
app.get("/feed.xml", (req) =>
    req.headers["if-none-match"] === etag
        ? { status: 304, body: "" }
        : { body: current, contentType: "application/rss+xml", headers: { etag } },
);
app.get("/search", (req) => ({
    hits: n2.search
        .search(req.query.q || "", { mode: "and", limit: 5 })
        .map(([id, score]) => ({ gtin: interner.get(id), score: score.round(3) })),
    suggest: n2.search.suggest((req.query.q || "").split(" ").last(), 3).pluck(0),
}));
app.start();
const base = `http://127.0.0.1:${app.port}`;
const r1 = await fetch(base + "/feed.xml");
const body = await r1.text();
const r2 = await fetch(base + "/feed.xml", { headers: { "if-none-match": r1.headers.get("etag") } });
await r2.text();
const found = await (await fetch(base + "/search?q=espresso+brewmaster")).json();
const typed = await (await fetch(base + "/search?q=kett")).json();
app.close();

// =============================================================================
// Self-test: every outcome lands in one object, checked once
// =============================================================================
function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
const results = {};
const sample = n2.valid.find((p) => p.gtin === catalog[6].gtin);
const espressoGtins = new Set(catalog.filter((_, i) => i % 6 === 2).pluck("gtin"));
results["bad GTIN and unknown currency rejected with reasons"] =
    n1.rejects.some((r) => /check digit/.test(r.reason)) && n1.rejects.some((r) => /JPY/.test(r.reason));
results["bulk gemv GTIN check agrees with the scalar GS1 rule on every catalog code"] =
    validateGtins(catalog.map((p) => "0" + p.gtin)).all(Boolean) && !validateGtins(["04006381330019"])[0];
results["schema rejects the ftp image / unmapped category product"] = n1.rejects.some(
    (r) => r.kind === "schema" && /image|category/.test(r.reason),
);
results["price guard catches the lost decimal point (TDigest median)"] = n1.rejects.some(
    (r) => r.kind === "price" && r.sku === "A-dec",
);
results["merged GTIN takes bravo's cheaper in-stock offer"] =
    sample && sample.offers >= 2 && sample.sources.includes("bravo") && sample.supplier === "bravo";
results["category mapped by longest prefix, image forced to https"] =
    sample.category === "Electronics > Audio > Headphones" && sample.image.startsWith("https://");
results["an unknown deeper supplier level falls back to the longest known prefix"] =
    n2.valid.find((p) => p.gtin === catalog[1].gtin).category === "Electronics > Computers > Laptops";
results["promotional noise stripped from titles in one MultiMatcher pass"] = !/BEST PRICE|Free Shipping|!!|\s{2}/.test(
    n1.valid.pluck("title").join("|"),
);
results["OrderedMap emits the feed sorted by GTIN without a sort"] = n1.valid
    .pluck("gtin")
    .every((g, i, a) => i === 0 || a[i - 1] < g);
results[
    `night 2 delta from RoaringBitmap differences: ${n2.delta.changed.length} changed, ${n2.delta.removed.length} removed`
] =
    n1.delta.added.length === n1.valid.length &&
    n2.delta.removed.length === 4 &&
    n2.delta.changed.length > 0 &&
    n2.delta.added.length === 0;
results["gzip feed on disk round-trips"] = gunzip(readBytes(work.join("feed-2.xml.gz")), { asString: true }) === n2.xml;
results["feed endpoint honours If-None-Match"] = r1.status === 200 && r2.status === 304 && body.includes("<g:gtin>");
results["marketplace CSV has one row per valid product"] =
    n2.market.split("\n").filter(Boolean).length === n2.valid.length + 1;
results["BM25 search returns only espresso machines for 'espresso brewmaster'"] =
    found.hits.length === 5 && found.hits.all((h) => espressoGtins.has(h.gtin));
results["prefix suggestion completes 'kett' to 'kettle'/'kettlo'"] = typed.suggest.some((s) => s.startsWith("kettl"));
results["TopK names a real brand as the most offered"] = KINDS.pluck(1).includes(n1.brands.list()[0][0]);
results["simd price stats bracket the mean"] = n2.stats.min <= n2.stats.mean && n2.stats.mean <= n2.stats.max;
{
    const rebuilt = applyDelta(new Map(n1.valid.map((p) => [p.gtin, p])), delta2);
    const want = new Map(n2.valid.map((p) => [p.gtin, projection(p)]));
    results[
        `night 1 + the ${delta2.length.bytes()} delta feed reproduces night 2's ${n2.valid.length} products exactly`
    ] = rebuilt.size === want.size && [...rebuilt].every(([g, p]) => want.get(g) === projection(p));
}
{
    // GS1 error detection, exhaustively: every single-digit substitution and every adjacent transposition of 40 codes
    const variants = [],
        expect = [];
    for (const p of catalog.slice(0, 40)) {
        const g = "0" + p.gtin;
        for (let pos = 1; pos < 14; pos++)
            for (let d = 0; d < 10; d++)
                if (String(d) !== g[pos]) {
                    variants.push(g.slice(0, pos) + d + g.slice(pos + 1));
                    expect.push(false); // weights 1 and 3 are both units mod 10: any single change moves the sum
                }
        for (let pos = 1; pos < 13; pos++)
            if (g[pos] !== g[pos + 1]) {
                variants.push(g.slice(0, pos) + g[pos + 1] + g[pos] + g.slice(pos + 2));
                expect.push(Math.abs(g[pos] - g[pos + 1]) === 5); // undetected exactly when 2(a - b) = 0 mod 10
            }
    }
    const got = validateGtins(variants);
    results[`GS1 theory holds on ${variants.length} corrupted codes through one gemv`] =
        got.every((v, k) => v === expect[k]) && expect.some(Boolean);
}
{
    // EUR conversion in BigInt: cents x rate (4 decimals) in units of 1e-6, rounded half-up to cents
    const toCents = (s) => {
        const [i, f = ""] = String(s).replace(",", ".").split(".");
        return BigInt(i) * 100n + BigInt((f + "00").slice(0, 2));
    };
    const rate4 = (r) => BigInt(Math.round(Number(r) * 10000));
    let n = 0,
        bad = 0;
    for (const [src, rows] of [
        ["alpha", readCsv(inputs1.a)],
        ["bravo", readXmlStream(inputs1.b)],
        ["charlie", readJsonPages(inputs1.c)],
    ])
        for (const r of rows) {
            const cur = r.currency || "EUR";
            if (!(cur in RATES)) continue;
            const micro = toCents(r.price) * rate4(RATES[cur]);
            const cents = (micro + 5000n) / 10000n;
            const want = `${cents / 100n}.${String(cents % 100n).padStart(2, "0")}`;
            n++;
            if (toEur(r.price, cur) !== want) bad++;
        }
    // generated prices never land on an exact half cent, so plant every one below 2000.00: cents x rate4 ending in
    // 5000. A float multiply-and-round gets some of them wrong (the planted set must contain such a case, or it proves
    // nothing about the rounding)
    let ties = 0,
        floatWrong = 0;
    for (const cur of ["USD", "GBP"]) {
        const r4 = rate4(RATES[cur]);
        for (let c = 1n; c < 200000n; c++)
            if ((c * r4) % 10000n === 5000n) {
                const price = `${c / 100n}.${String(c % 100n).padStart(2, "0")}`;
                const cents = (c * r4 + 5000n) / 10000n;
                const want = `${cents / 100n}.${String(cents % 100n).padStart(2, "0")}`;
                ties++;
                if ((Math.round(Number(price) * Number(RATES[cur]) * 100) / 100).toFixed(2) !== want) floatWrong++;
                if (toEur(price, cur) !== want) bad++;
            }
    }
    results[
        `Decimal EUR conversion equals BigInt half-up arithmetic on all ${n} offers and ${ties} exact half-cent ties (${floatWrong} of which a float round gets wrong)`
    ] = bad === 0 && n > 200 && ties > 100 && floatWrong > 0;
}
{
    // BM25 from the formula: idf = ln(1 + (N - df + 0.5) / (df + 0.5)), tf saturation k1 = 1.2, length norm b = 0.75
    const docs = n2.valid.map((p) => InvertedIndex.tokenize(`${p.title} ${p.brand} ${p.category}`));
    const N = docs.length,
        avg = docs.reduce((a, d) => a + d.length, 0) / N;
    const q = ["espresso", "deluxe", "kitchen"];
    const df = Object.fromEntries(q.map((t) => [t, docs.filter((d) => d.includes(t)).length]));
    const score = (d) =>
        q.reduce((s, t) => {
            const tf = d.filter((x) => x === t).length;
            if (!tf) return s;
            const idf = Math.log(1 + (N - df[t] + 0.5) / (df[t] + 0.5));
            return s + (idf * tf * 2.2) / (tf + 1.2 * (1 - 0.75 + (0.75 * d.length) / avg));
        }, 0);
    const want = docs
        .map((d, i) => [n2.valid[i].gtin, score(d)])
        .filter(([, sc]) => sc > 0)
        .sortBy(([, sc]) => -sc);
    const got = n2.search.search(q.join(" "), { mode: "or", limit: want.length });
    results[`BM25 scores equal the formula for all ${want.length} matching products`] =
        got.length === want.length &&
        got.every(
            ([id, sc]) => Math.abs(sc - score(docs[n2.valid.findIndex((p) => p.gtin === interner.get(id))])) < 1e-9,
        );
}
check(results);

print("product-feed-syndication");
for (const [name, n] of [
    ["night 1", n1],
    ["night 2", n2],
])
    print(
        `  ${name}   ${n.raw} offers -> ${n.merged} products -> ${n.valid.length} valid, ${n.rejects.length} rejected ` +
            `(${n.reasons
                .entrySet()
                .map(([k, c]) => `${k} ${c}`)
                .join(", ")}) | +${n.delta.added.length} ~${n.delta.changed.length} -${n.delta.removed.length}`,
    );
print(`  delta     ${delta2.length.bytes()} (night 2 upserts and deletes) vs ${n2.xml.length.bytes()} full feed`);
print(`  feed      ${n2.xml.length.bytes()} XML, ${gzip(n2.xml).length.bytes()} gzipped, ETag ${etag}`);
print(`  prices    mean ${n2.stats.mean.format(2)} EUR, range ${n2.stats.min.format(2)}..${n2.stats.max.format(2)}`);
print(
    `  search    "espresso brewmaster" -> ${found.hits.map((h) => h.gtin.last(4)).join(" ")}; "kett" -> ${typed.suggest.join(", ")}`,
);
for (const r of n1.rejects.take(5)) print(`  reject    ${r.supplier}/${r.sku}: ${r.reason}`);
removeAll(work);
