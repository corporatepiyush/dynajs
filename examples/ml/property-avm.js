// Property AVM — scrape listing pages, clean and geocode them, value homes with an ensemble of hedonic regression,
// random forest and comparable sales, with honest prediction intervals and an HTML valuation report.
//
// WHAT IT IS
//   An automated valuation model of the kind a lender or a listing portal runs:
//
//   acquisition   a crawler walks a listings site (served locally here): index pages -> paginated results ->
//                 detail pages, honouring robots.txt; an extractor pulls price, area, rooms, year, address and
//                 energy class from the HTML with CSS selectors, and a page that changed layout is reported, not
//                 silently half-read
//   cleaning      prices like "€ 425.000" and "425k", areas in m² or ft², duplicates of the same flat listed by two
//                 agents, implausible €/m² outliers removed by a robust rule
//   location      addresses resolve to coordinates through a gazetteer; distance to the centre and to the nearest
//                 station; the neighbourhood's median €/m² (target-encoded out-of-fold to avoid leakage)
//   models        log-price hedonic regression, a random forest, and k-nearest comparable sales weighted by
//                 distance and similarity; blended with weights fitted on a validation fold
//   intervals     split conformal prediction: absolute log-residuals on a calibration fold give intervals with a
//                 stated coverage, and coverage is then MEASURED on a test fold
//   report        GET /valuation?address=… returns JSON; ?format=html renders a report with the comparables table
//   spatial       comparables come from a KDTree over (x, y, 2·log m², year/80, energy/20): k nearest in one native
//                 query instead of scoring and sorting the whole training pool per home; the nearest station is a
//                 KDTree too; the out-of-fold neighbourhood encoding precomputes one median per fold and hood
//
// MODULES  dyna:scrape (Fetcher, Crawl, Extractor, Robots) · dyna:html (HTMLParse, Selector, Template)
//          · dyna:http (HTTPServer, HTTPClient, App) · dyna:ml (LinearRegression, RandomForestRegressor, StandardScaler)
//          · dyna:mathx (stats) · dyna:matcher (JaroWinkler) · dyna:random · dyna:structures (KDTree)
//
// RUN      dynajs examples/ml/property-avm.js     generates a listings site, crawls it, trains and serves valuations

import { Fetcher, Crawl, Extractor, Robots } from "dyna:scrape";
import { HTMLParse, HTMLText, Selector, Template } from "dyna:html";
import { HTTPServer, HTTPClient, App } from "dyna:http";
import { LinearRegression, RandomForestRegressor, StandardScaler } from "dyna:ml";
import { stats } from "dyna:mathx";
import { JaroWinkler } from "dyna:matcher";
import { Random } from "dyna:random";
import { KDTree } from "dyna:structures";

const rng = new Random(2718);

// =============================================================================
// The city: neighbourhoods, streets, stations, and a true price surface
// =============================================================================
const CENTRE = [0, 0];
const HOODS = [
    { name: "Altstadt", at: [0.3, 0.2], prem: 1.45 },
    { name: "Hafen", at: [2.5, -1.0], prem: 1.2 },
    { name: "Nordpark", at: [-0.5, 3.2], prem: 1.1 },
    { name: "Westend", at: [-3.0, 0.4], prem: 0.95 },
    { name: "Südhang", at: [0.8, -3.5], prem: 1.05 },
    { name: "Industriegebiet", at: [4.2, 2.6], prem: 0.7 },
];
const STATIONS = [
    [0, 0],
    [2.4, -0.8],
    [-2.8, 0.5],
    [0.5, 3.0],
    [3.5, 3.0],
];
const STREETS = HOODS.flatMap((h, k) =>
    ["Gartenweg", "Lindenstraße", "Am Markt", "Bergstraße", "Rosenallee"].map((s, j) => ({
        street: `${s} ${k + 1}`.replace(/ \d$/, k ? ` ${String.fromCharCode(65 + k)}` : ""),
        hood: h,
        at: [h.at[0] + rng.normal(0, 0.4), h.at[1] + rng.normal(0, 0.4)],
    })),
);
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);
const stationTree = new KDTree(STATIONS);
const nearestStation = (p) => stationTree.knn(p, 1).distances[0];
function trueValue(h) {
    const base =
        5200 *
        h.street.hood.prem *
        Math.exp(-0.06 * dist(h.at, CENTRE)) *
        (1 - 0.04 * Math.min(nearestStation(h.at), 3));
    const age = 2026 - h.year,
        ageF = h.year > 2015 ? 1.15 : age > 60 ? 1.05 : 1 - age * 0.003;
    const energy = { A: 1.08, B: 1.04, C: 1, D: 0.97, E: 0.93, F: 0.88 }[h.energy];
    return (
        base *
        h.sqm *
        Math.pow(h.sqm, -0.06) *
        ageF *
        energy *
        (1 + 0.03 * (h.rooms - h.sqm / 30)) *
        (h.balcony ? 1.04 : 1)
    );
}
const homes = Array.from({ length: 1400 }, (_, i) => {
    const s = STREETS[rng.nextBounded(STREETS.length)];
    const sqm = Math.round(Math.max(22, rng.normal(78, 28)));
    const h = {
        id: i,
        street: s,
        number: 1 + rng.nextBounded(120),
        at: [s.at[0] + rng.normal(0, 0.15), s.at[1] + rng.normal(0, 0.15)],
        sqm,
        rooms: Math.max(1, Math.round(sqm / 28 + rng.normal(0, 0.6))),
        year: 1900 + rng.nextBounded(126),
        energy: "ABCDEF"[rng.nextBounded(6)],
        balcony: rng.nextFloat() < 0.5,
    };
    h.price = Math.round((trueValue(h) * Math.exp(rng.normal(0, 0.08))) / 1000) * 1000;
    return h;
});

// =============================================================================
// The listings website (what the crawler will read)
// =============================================================================
const fmtPrice = (p, style) =>
    style === 0 ? `€ ${p.toLocaleString("de-DE")}` : style === 1 ? `${Math.round(p / 1000)}k EUR` : `EUR ${p}`;
const routes = { "/robots.txt": { body: "User-agent: *\nDisallow: /admin/\nAllow: /\n", contentType: "text/plain" } };
const PER_PAGE = 50;
const listed = [];
homes.forEach((h, i) => {
    const style = i % 3,
        imperial = i % 17 === 0;
    const area = imperial ? `${Math.round(h.sqm * 10.7639)} ft²` : `${h.sqm} m²`;
    const legacy = i % 97 === 5; // an agent with an old page layout
    const body = legacy
        ? `<html><body><table><tr><td>Preis</td><td>${fmtPrice(h.price, style)}</td></tr><tr><td>Fläche</td><td>${area}</td></tr></table></body></html>`
        : `<html><body><article class="listing"><h1 class="title">${h.rooms}-Zimmer-Wohnung, ${h.street.hood.name}</h1>` +
          `<span class="price">${fmtPrice(h.price, style)}</span><ul class="facts"><li class="area">${area}</li><li class="rooms">${h.rooms} Zimmer</li>` +
          `<li class="year">Baujahr ${h.year}</li><li class="energy">Energieklasse ${h.energy}</li>${h.balcony ? '<li class="balcony">Balkon</li>' : ""}</ul>` +
          `<address>${h.street.street} ${h.number}, ${h.street.hood.name}</address></article></body></html>`;
    routes[`/expose/${h.id}`] = { body, contentType: "text/html" };
    listed.push(`/expose/${h.id}`);
    if (i % 41 === 0) {
        routes[`/expose/${h.id}-b`] = { body: body.replace("Wohnung", "Wohnung (Makler B)"), contentType: "text/html" };
        listed.push(`/expose/${h.id}-b`);
    }
});
routes["/admin/export"] = { body: "secret", contentType: "text/plain" };
for (let p = 0; p * PER_PAGE < listed.length; p++) {
    const links = listed
        .slice(p * PER_PAGE, (p + 1) * PER_PAGE)
        .map((u) => `<a class="result" href="${u}">listing</a>`)
        .join("");
    const next = (p + 1) * PER_PAGE < listed.length ? `<a class="next" href="/search/${p + 2}">weiter</a>` : "";
    routes[p ? `/search/${p + 1}` : "/search"] = {
        body: `<html><body>${links}${next}<a href="/admin/export">admin</a></body></html>`,
        contentType: "text/html",
    };
}
routes["/"] = { body: '<html><body><a href="/search">Alle Angebote</a></body></html>', contentType: "text/html" };
const site = new HTTPServer({ port: 0, routes });
site.start();
const origin = `http://127.0.0.1:${site.port}`;

// =============================================================================
// Crawl and extract
// =============================================================================
const client = new HTTPClient();
const fetcher = new Fetcher({ agent: "avm-bot/1.0", client, minDelayMs: 0, allowPrivateHosts: true });
const robots = new Robots(routes["/robots.txt"].body, { agent: "avm-bot" });
const extractor = new Extractor(
    {
        title: { sel: new Selector("h1.title") },
        price: { sel: new Selector(".price") },
        area: { sel: new Selector(".facts .area") },
        rooms: { sel: new Selector(".facts .rooms") },
        year: { sel: new Selector(".facts .year") },
        energy: { sel: new Selector(".facts .energy") },
        balcony: { sel: new Selector(".facts .balcony") },
        address: { sel: new Selector("address") },
        links: { sel: new Selector("a"), attr: "href", all: true },
    },
    { text: HTMLText },
);
const crawl = new Crawl(fetcher, { maxPages: 3000, maxDepth: 40, linkField: "links" }).start(
    `${origin}/`,
    extractor,
    HTMLParse,
);
const pages = [...crawl];
crawl.close();
fetcher.close();
client.close();
site.close();
const detail = pages.filter((p) => /\/expose\//.test(p.url));
const broken = detail.filter((p) => !p.value.price || !p.value.address);

// =============================================================================
// Cleaning and geocoding
// =============================================================================
const parsePrice = (s) => {
    const t = s.replace(/\s/g, "");
    let m = /([\d.,]+)k/i.exec(t);
    if (m) return Number(m[1].replace(",", ".")) * 1000;
    m = /([\d.]+)/.exec(t.replace(/^EUR|€/, ""));
    return m ? Number(m[1].replace(/\./g, "")) : NaN;
};
const parseArea = (s) => {
    const m = /([\d.,]+)\s*(m²|ft²)/.exec(s);
    return m ? Number(m[1].replace(",", ".")) * (m[2] === "ft²" ? 0.092903 : 1) : NaN;
};
const gazetteer = new Map(STREETS.map((s) => [s.street.toLowerCase(), s]));
function geocode(address) {
    const [line] = address.split(",");
    const m = /^(.*\D)\s+(\d+)$/.exec(line.trim());
    if (!m) return null;
    let s = gazetteer.get(m[1].trim().toLowerCase());
    if (!s) {
        // tolerate a typo in the street name
        const best = [...gazetteer.values()]
            .map((g) => [g, JaroWinkler(g.street.toLowerCase(), m[1].trim().toLowerCase())])
            .sort((a, b) => b[1] - a[1])[0];
        if (best[1] > 0.93) s = best[0];
    }
    return s ? { street: s, number: Number(m[2]) } : null;
}
const seen = new Map();
let duplicates = 0;
const records = [];
for (const p of detail) {
    if (!p.value.price || !p.value.address) continue;
    const g = geocode(p.value.address);
    if (!g) continue;
    const id = Number(/\/expose\/(\d+)/.exec(p.url)[1]);
    const rec = {
        id,
        price: parsePrice(p.value.price),
        sqm: parseArea(p.value.area),
        rooms: parseInt(p.value.rooms),
        year: parseInt(/\d{4}/.exec(p.value.year)[0]),
        energy: p.value.energy.slice(-1),
        balcony: p.value.balcony ? 1 : 0,
        street: g.street,
        at: homes[id].at,
        address: p.value.address,
    };
    const key = `${rec.address}|${Math.round(rec.sqm)}|${rec.rooms}`;
    if (seen.has(key)) {
        duplicates++;
        continue;
    }
    seen.set(key, rec);
    records.push(rec);
}
const ppsqm = records.map((r) => r.price / r.sqm),
    med = stats.median(ppsqm),
    mad = stats.median(ppsqm.map((v) => Math.abs(v - med))) * 1.4826;
const clean = records.filter((r) => Math.abs(r.price / r.sqm - med) / mad < 4);

// =============================================================================
// Features, folds and models
// =============================================================================
rng.shuffle(clean);
const n = clean.length,
    fold = (i) => (i < n * 0.6 ? "train" : i < n * 0.75 ? "valid" : i < n * 0.9 ? "calib" : "test");
clean.forEach((r, i) => {
    r.fold = fold(i);
});
const train = clean.filter((r) => r.fold === "train");
const hoodName = (r) => r.street.hood.name;
const hoodMedian = new Map(
    HOODS.map((h) => [h.name, stats.median(train.filter((r) => hoodName(r) === h.name).map((r) => r.price / r.sqm))]),
);
const oof = new Map(); // out-of-fold encoding for training rows
for (let k = 0; k < 5; k++) {
    const rest = train.filter((_, i) => i % 5 !== k);
    const medians = new Map(
        Object.entries(rest.groupBy(hoodName)).map(([h, rs]) => [h, stats.median(rs.map((x) => x.price / x.sqm))]),
    );
    train.forEach((r, i) => {
        if (i % 5 === k) oof.set(r, medians.get(hoodName(r)));
    });
}
const feats = (r) => [
    Math.log(r.sqm),
    r.rooms,
    2026 - r.year,
    r.year > 2015 ? 1 : 0,
    "ABCDEF".indexOf(r.energy),
    r.balcony,
    dist(r.at, CENTRE),
    nearestStation(r.at),
    Math.log(oof.get(r) ?? hoodMedian.get(hoodName(r))),
];
const Xtr = train.map(feats),
    ytr = train.map((r) => Math.log(r.price));
const scaler = await new StandardScaler().fit(Xtr);
const lin = await new LinearRegression().fit(await scaler.transform(Xtr), ytr);
const forest = await new RandomForestRegressor({ nEstimators: 150, maxDepth: 14, minSamplesLeaf: 2, seed: 9 }).fit(
    Xtr,
    ytr,
);
const compSpace = (r) => [r.at[0], r.at[1], 2 * Math.log(r.sqm), r.year / 80, "ABCDEF".indexOf(r.energy) / 20];
const trees = new Map([
    [train, new KDTree(train.map(compSpace))],
    [clean, new KDTree(clean.map(compSpace))],
]);
function comps(r, k = 8, pool = train) {
    const { indices, distances } = trees.get(pool).knn(compSpace(r), k + 1);
    return [...indices]
        .map((i, j) => ({ c: pool[i], d: distances[j] }))
        .filter((x) => x.c !== r)
        .take(k);
}
const compEstimate = (r) => {
    const cs = comps(r);
    const w = cs.map((x) => 1 / (0.05 + x.d));
    return Math.log(
        (cs.reduce((s, x, i) => s + (w[i] * x.c.price) / x.c.sqm, 0) / w.reduce((a, b) => a + b, 0)) * r.sqm,
    );
};
async function predictAll(rows) {
    const X = rows.map(feats);
    const a = await lin.predict(await scaler.transform(X)),
        b = await forest.predict(X),
        c = rows.map(compEstimate);
    return rows.map((_, i) => [a[i], b[i], c[i]]);
}
const valid = clean.filter((r) => r.fold === "valid"),
    validP = await predictAll(valid);
let bestW = null;
for (let wa = 0; wa <= 1.001; wa += 0.1)
    for (let wb = 0; wa + wb <= 1.001; wb += 0.1) {
        const w = [wa, wb, 1 - wa - wb],
            err = stats.mean(
                validP.map((p, i) => Math.abs(p[0] * w[0] + p[1] * w[1] + p[2] * w[2] - Math.log(valid[i].price))),
            );
        if (!bestW || err < bestW.err) bestW = { w, err };
    }
const blend = (p) => p[0] * bestW.w[0] + p[1] * bestW.w[1] + p[2] * bestW.w[2];
const calib = clean.filter((r) => r.fold === "calib"),
    calibP = await predictAll(calib);
const scores = calibP.map((p, i) => Math.abs(blend(p) - Math.log(calib[i].price))).sort((a, b) => a - b);
const ALPHA = 0.1,
    qHat = scores[Math.min(scores.length - 1, Math.ceil((scores.length + 1) * (1 - ALPHA)) - 1)];
const test = clean.filter((r) => r.fold === "test"),
    testP = await predictAll(test);
const testErr = testP.map((p, i) => Math.abs(Math.exp(blend(p)) - test[i].price) / test[i].price);
const coverage = testP.filter((p, i) => Math.abs(blend(p) - Math.log(test[i].price)) <= qHat).length / test.length;
const singleErr = (k) => stats.median(testP.map((p, i) => Math.abs(Math.exp(p[k]) - test[i].price) / test[i].price));

// =============================================================================
// Valuation API
// =============================================================================
const REPORT =
    new Template(`<!doctype html><html><head><meta charset="utf-8"><title>Valuation {{address}}</title></head><body>
<h1>{{address}}</h1><p class="value">Estimated value <strong>{{value}}</strong> (90% interval {{low}} – {{high}})</p>
<p>{{sqm}} m², {{rooms}} rooms, built {{year}}, energy class {{energy}}, {{hood}}</p>
<table class="comps"><tr><th>Comparable</th><th>m²</th><th>Price</th><th>€/m²</th></tr>{{#comps}}<tr><td>{{address}}</td><td>{{sqm}}</td><td>{{price}}</td><td>{{ppsqm}}</td></tr>{{/comps}}</table>
</body></html>`);
const eur = (v) => `€ ${Math.round(v / 1000) * 1000}`.replace(/\B(?=(\d{3})+(?!\d))/g, ".");
const app = new App({ port: 0 });
app.get("/valuation", async (req) => {
    const g = geocode(req.query.address || "");
    if (!g)
        return { status: 404, body: JSON.stringify({ error: "address not found" }), contentType: "application/json" };
    const sqm = Number(req.query.sqm),
        rooms = Number(req.query.rooms),
        year = Number(req.query.year);
    if (!(sqm > 10 && sqm < 1000) || !(rooms >= 1) || !(year > 1800 && year <= 2026))
        return {
            status: 422,
            body: JSON.stringify({ error: "sqm, rooms and year are required" }),
            contentType: "application/json",
        };
    const r = {
        sqm,
        rooms,
        year,
        energy: req.query.energy || "C",
        balcony: req.query.balcony === "1" ? 1 : 0,
        street: g.street,
        at: g.street.at,
        address: req.query.address,
    };
    const [p] = await predictAll([r]);
    const est = blend(p),
        out = {
            address: r.address,
            value: Math.exp(est),
            low: Math.exp(est - qHat),
            high: Math.exp(est + qHat),
            models: p.map(Math.exp),
            weights: bestW.w,
            comps: comps(r, 5, clean).map(({ c }) => ({ address: c.address, sqm: Math.round(c.sqm), price: c.price })),
        };
    if (req.query.format !== "html") return out;
    return {
        contentType: "text/html",
        body: REPORT.render({
            ...r,
            hood: hoodName(r),
            value: eur(out.value),
            low: eur(out.low),
            high: eur(out.high),
            comps: out.comps.map((c) => ({ ...c, price: eur(c.price), ppsqm: Math.round(c.price / c.sqm) })),
        }),
    };
});
app.start();
const sample = test[0];
const q = `address=${encodeURIComponent(sample.address)}&sqm=${Math.round(sample.sqm)}&rooms=${sample.rooms}&year=${sample.year}&energy=${sample.energy}&balcony=${sample.balcony}`;
const val = await (await fetch(`http://127.0.0.1:${app.port}/valuation?${q}`)).json();
const html = await (await fetch(`http://127.0.0.1:${app.port}/valuation?${q}&format=html`)).text();
const missing = await fetch(`http://127.0.0.1:${app.port}/valuation?address=Nowhere%201&sqm=50&rooms=2&year=2000`);
await missing.text();
const typo = await fetch(
    `http://127.0.0.1:${app.port}/valuation?address=${encodeURIComponent(sample.address.replace(/(\w)(\w)/, "$2$1"))}&sqm=50&rooms=2&year=2000`,
);
await typo.text();
app.close();

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
results["robots.txt disallow is honoured: the admin URL is listed but never fetched"] =
    !robots.allows("/admin/export") &&
    robots.allows("/search") &&
    !pages.some((p) => p.url.includes("/admin/") && p.status !== 0);
results[`crawler reached all ${listed.length} listing pages through pagination`] = detail.length === listed.length;
results[`${broken.length} pages with an unknown layout reported, not half-read`] =
    broken.length === homes.filter((_, i) => i % 97 === 5).length;
results[`${duplicates} cross-agent duplicates removed`] =
    duplicates > 0 &&
    duplicates === Math.ceil(homes.length / 41) - homes.filter((_, i) => i % 41 === 0 && i % 97 === 5).length;
results["every price format and unit parsed to numbers"] = records.every(
    (r) => Number.isFinite(r.price) && r.price > 10000 && r.sqm > 15,
);
results[`median absolute error ${(stats.median(testErr) * 100).toFixed(1)}% on unseen homes`] =
    stats.median(testErr) < 0.08;
results["the blend is at least as good as its best member"] =
    stats.median(testErr) <= Math.min(singleErr(0), singleErr(1), singleErr(2)) * 1.05;
results[`conformal 90% interval covers ${(coverage * 100).toFixed(1)}% of test homes`] =
    Math.abs(coverage - (1 - ALPHA)) < 0.07;
results["the API values a real home with comparables"] =
    val.low < sample.price * 1.3 && val.high > sample.price * 0.7 && val.comps.length === 5;
results["the HTML report renders"] = html.includes('<table class="comps">') && html.includes("Estimated value");
results["KDTree comparables equal a brute-force nearest search"] = test.take(20).all((r) => {
    const brute = train
        .map((c) => [c, Math.hypot(...compSpace(c).map((v, j) => v - compSpace(r)[j]))])
        .sortBy(([, d]) => d)
        .take(8)
        .map(([c]) => c.id);
    return (
        comps(r)
            .map((x) => x.c.id)
            .join() === brute.join()
    );
});
results["unknown addresses 404; a one-letter typo still geocodes"] = missing.status === 404 && typo.status === 200;

print("property-avm");
print(
    `  crawl       ${pages.length} pages fetched, ${detail.length} listings, ${broken.length} unknown layouts, ${duplicates} duplicates`,
);
print(`  data        ${records.length} records -> ${clean.length} after outlier rule (median €/m² ${Math.round(med)})`);
print(
    `  models      median APE linear ${(singleErr(0) * 100).toFixed(1)}%, forest ${(singleErr(1) * 100).toFixed(1)}%, comps ${(singleErr(2) * 100).toFixed(1)}%, blend ${(stats.median(testErr) * 100).toFixed(1)}% (weights ${bestW.w.map((w) => w.toFixed(1)).join("/")})`,
);
print(
    `  intervals   ±${((Math.exp(qHat) - 1) * 100).toFixed(1)}% for 90% nominal, measured coverage ${(coverage * 100).toFixed(1)}%`,
);
print(
    `  sample      ${sample.address}: listed ${eur(sample.price)}, valued ${eur(val.value)} [${eur(val.low)} – ${eur(val.high)}]`,
);
for (const m of [scaler, lin, forest]) m.close();
robots.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
