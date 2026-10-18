// Commerce backend — catalog, search, accounts, carts, promotions, tax, idempotent checkout, invoices and reporting.
//
// WHAT IT IS
//   The backend of a small online store, built the way a real one is layered:
//
//   catalog      products in SQLite; admins edit them with JSON Patch, every result re-validated by JSON Schema
//   search       the native BM25 InvertedIndex (dyna:structures) keyed by interned SKU ids, prefix autocomplete
//                from the index's own term dictionary (suggest), and "did you mean" correction by Damerau-Levenshtein
//                distance against the vocabulary Trie; a re-indexed product replaces its document in place
//   accounts     bcrypt password hashes, HS256 session tokens checked for issuer, audience and expiry
//   carts        per-customer carts in an LRU with a time-to-live (abandoned carts expire on their own)
//   pricing      exact Money arithmetic: percentage and buy-two-get-one promotions, per-state sales tax
//                rounded half-up per line, weight-banded shipping; discounts spread across lines with
//                Money.allocate so the line totals always add up to the order total
//   checkout     an Idempotency-Key makes retries safe; stock is reserved with a conditional UPDATE inside
//                a transaction, so two customers racing for the last unit cannot both win
//   events       every order is appended to an NDJSON event log, the source the reports are built from
//   invoices     HTML rendered from a template, with a payment QR code
//   operations   Prometheus metrics, and a nightly DataFrame report: revenue by category, best sellers
//
// MODULES  dyna:http · dyna:net (SQLite, Metrics) · dyna:crypto · dyna:oauth2 · dyna:decimal · dyna:schema
//          dyna:json · dyna:structures · dyna:matcher · dyna:html · dyna:encoding · dyna:dataframe · dyna:uuid
//          dyna:file · dyna:cli
//
// RUN      dynajs examples/web/commerce-backend.js --port 8080 --data ./store    serve for real
//          dynajs examples/web/commerce-backend.js                               a scripted day of trading
//                                                                                against a temp store

import { App } from "dyna:http";
import { SQLite, Metrics } from "dyna:net";
import { Bcrypt, JWTSign, RandomBytes } from "dyna:crypto";
import { verifyJWT, parseBearerHeader } from "dyna:oauth2";
import { Decimal, Money } from "dyna:decimal";
import { Schema } from "dyna:schema";
import { Patch } from "dyna:json";
import { LRU, Trie, Interner, InvertedIndex } from "dyna:structures";
import { DamerauLevenshtein } from "dyna:matcher";
import { Template } from "dyna:html";
import { QRToString } from "dyna:encoding";
import { DataFrame } from "dyna:dataframe";
import { ULID } from "dyna:uuid";
import { Path, makeTempDir, makeDir, removeAll, writeFile, readFile } from "dyna:file";
import { Command } from "dyna:cli";

const { options } = new Command("commerce-backend")
    .option("-p, --port <n>", "port to serve on", { type: "number" })
    .option("--data <dir>", "data directory (database, event log, reports)", { type: "string" })
    .parse(scriptArgs.slice(1));
const selfTest = options.port === undefined;
const dataDir = selfTest ? makeTempDir("store") : new Path(options.data || "store");
makeDir(dataDir, { recursive: true });

const CURRENCY = "USD";
const usd = (s) => Money.fromString(String(s), CURRENCY);
const ZERO = new Money(0, CURRENCY);

// =============================================================================
// Catalog
// =============================================================================
const db = new SQLite(dataDir.join("store.db"));
db.exec(`CREATE TABLE IF NOT EXISTS products (
    sku TEXT PRIMARY KEY, name TEXT NOT NULL, description TEXT NOT NULL, category TEXT NOT NULL,
    price TEXT NOT NULL, weight_g INTEGER NOT NULL, stock INTEGER NOT NULL CHECK (stock >= 0))`);
db.exec(
    `CREATE TABLE IF NOT EXISTS customers (email TEXT PRIMARY KEY, name TEXT NOT NULL, state TEXT NOT NULL, pw TEXT NOT NULL)`,
);
db.exec(
    `CREATE TABLE IF NOT EXISTS orders (id TEXT PRIMARY KEY, email TEXT NOT NULL, total TEXT NOT NULL, body TEXT NOT NULL)`,
);

const productSchema = Schema.compile({
    type: "object",
    additionalProperties: false,
    required: ["sku", "name", "description", "category", "price", "weight_g", "stock"],
    properties: {
        sku: { type: "string", pattern: "^[A-Z]{2,4}-\\d{3,5}$" },
        name: { type: "string", minLength: 2, maxLength: 120 },
        description: { type: "string", maxLength: 2000 },
        category: { enum: ["audio", "computing", "home", "outdoor", "books"] },
        price: { type: "string", pattern: "^\\d{1,5}\\.\\d{2}$" },
        weight_g: { type: "integer", minimum: 1, maximum: 50000 },
        stock: { type: "integer", minimum: 0 },
    },
});
const getProduct = (sku) => db.query("SELECT * FROM products WHERE sku = ?", [sku])[0];

const SEED = [
    [
        "AUD-101",
        "Wireless headphones",
        "Over-ear wireless headphones with active noise cancelling and a 30 hour battery",
        "audio",
        "149.00",
        260,
        40,
    ],
    [
        "AUD-102",
        "Wireless earbuds",
        "Compact wireless earbuds, water resistant, with a charging case",
        "audio",
        "89.00",
        60,
        120,
    ],
    [
        "AUD-110",
        "Bookshelf speakers",
        "A pair of passive bookshelf speakers with a wooden cabinet",
        "audio",
        "219.00",
        7400,
        12,
    ],
    [
        "CMP-201",
        "Mechanical keyboard",
        "Tenkeyless mechanical keyboard with hot swappable switches and backlight",
        "computing",
        "119.00",
        900,
        30,
    ],
    ["CMP-202", "Wireless mouse", "Ergonomic wireless mouse with silent buttons", "computing", "39.00", 95, 200],
    [
        "CMP-230",
        "USB-C dock",
        "USB-C dock with two displays, ethernet and power delivery",
        "computing",
        "179.00",
        420,
        1,
    ],
    ["HOM-301", "Espresso machine", "Manual espresso machine with a steam wand for milk", "home", "349.00", 8200, 8],
    ["HOM-305", "Coffee grinder", "Burr coffee grinder with 40 grind settings", "home", "129.00", 2300, 25],
    [
        "OUT-401",
        "Trail backpack",
        "A 28 litre trail backpack with a rain cover and hydration sleeve",
        "outdoor",
        "99.00",
        1100,
        50,
    ],
    ["OUT-402", "Camping lantern", "Rechargeable camping lantern, 600 lumens", "outdoor", "34.50", 380, 75],
    [
        "BOK-501",
        "Field guide to birds",
        "An illustrated field guide to the birds of North America",
        "books",
        "24.99",
        650,
        60,
    ],
    ["BOK-502", "Coffee atlas", "A coffee atlas: origins, roasting and brewing", "books", "39.99", 1200, 18],
];
if (!db.query("SELECT COUNT(*) AS n FROM products")[0].n)
    for (const row of SEED) db.exec("INSERT INTO products VALUES (?, ?, ?, ?, ?, ?, ?)", row);

// =============================================================================
// Search: BM25 over name + description, Trie autocomplete, typo correction
// =============================================================================
class SearchIndex {
    constructor() {
        this.ix = new InvertedIndex({ k1: 1.2, b: 0.75 });
        this.ids = new Interner();
        this.vocab = new Trie();
    }
    static tokens(text) {
        return text.toLowerCase().match(/[a-z0-9]+/g) || [];
    }
    index(sku, text) {
        const id = this.ids.intern(sku),
            toks = SearchIndex.tokens(text);
        this.ix.remove(id);
        this.ix.add(id, toks.join(" "));
        for (const t of new Set(toks)) this.vocab.insert(t);
    }
    correct(term) {
        // the closest vocabulary word within two edits, else the term itself
        if (this.ix.docFreq(term)) return term;
        let best = term,
            bestD = 3;
        for (const w of this.vocab.keysWithPrefix("")) {
            if (Math.abs(w.length - term.length) > 2 || !this.ix.docFreq(w)) continue;
            const d = DamerauLevenshtein(term, w, { max: 2 });
            if (d < bestD || (d === bestD && this.ix.docFreq(w) > this.ix.docFreq(best))) {
                best = w;
                bestD = d;
            }
        }
        return best;
    }
    search(query, k = 5) {
        const terms = SearchIndex.tokens(query).map((t) => this.correct(t));
        const hits = this.ix
            .search(terms.join(" "), { mode: "or", limit: k })
            .map(([id, score]) => ({ sku: this.ids.get(id), score: +score.toFixed(3) }));
        return { corrected: terms.join(" "), hits };
    }
    suggest(prefix, k = 5) {
        return this.ix.suggest(prefix.toLowerCase(), k).map(([term]) => term);
    }
}
const search = new SearchIndex();
for (const p of db.query("SELECT * FROM products")) search.index(p.sku, `${p.name} ${p.description} ${p.category}`);

// =============================================================================
// Accounts and sessions
// =============================================================================
const SESSION_KEY = RandomBytes(32);
const ISS = "commerce-backend",
    AUD = "storefront";
function issueSession(email) {
    const now = Math.floor(Date.now() / 1000);
    return JWTSign({ iss: ISS, aud: AUD, sub: email, iat: now, exp: now + 3600 }, SESSION_KEY, { alg: "HS256" });
}
function customerOf(req) {
    const token = parseBearerHeader(req.headers.authorization || "");
    if (!token) return null;
    try {
        return verifyJWT(token, SESSION_KEY, { algorithms: ["HS256"], aud: AUD, iss: ISS, requireExp: true }).sub;
    } catch {
        return null;
    }
}

// =============================================================================
// Pricing
// =============================================================================
const TAX = { CA: "0.0725", NY: "0.04", TX: "0.0625", OR: "0" }; // state base rates
const PROMOS = {
    WELCOME10: { kind: "percent", rate: "0.10", minSubtotal: usd("50.00") },
    COFFEE3: { kind: "bogo", category: "books", buy: 2, free: 1 }, // buy two books, the cheapest third is free
};
function shippingFor(grams) {
    // weight bands, free over a threshold handled by caller
    if (grams <= 500) return usd("4.99");
    if (grams <= 2000) return usd("8.99");
    if (grams <= 10000) return usd("19.99");
    return usd("39.99");
}
function price(cart, state, promoCode) {
    const lines = [];
    for (const [sku, qty] of cart) {
        const p = getProduct(sku);
        if (!p) throw Object.assign(new Error(`unknown product ${sku}`), { status: 400 });
        lines.push({
            sku,
            name: p.name,
            category: p.category,
            qty,
            unit: usd(p.price),
            weight: p.weight_g * qty,
            total: usd(p.price).mul(qty),
            discount: ZERO,
        });
    }
    const subtotal = lines.reduce((s, l) => s.add(l.total), ZERO);
    let discount = ZERO,
        promo = null;
    const rule = promoCode ? PROMOS[promoCode] : null;
    if (promoCode && !rule) throw Object.assign(new Error(`unknown promotion ${promoCode}`), { status: 400 });
    if (rule?.kind === "percent" && subtotal.cmp(rule.minSubtotal) >= 0) {
        discount = Money.fromDecimal(subtotal.toDecimal().mul(rule.rate).round(2, "halfUp"), CURRENCY);
        promo = promoCode;
    } else if (rule?.kind === "bogo") {
        const units = lines
            .filter((l) => l.category === rule.category)
            .flatMap((l) => Array(l.qty).fill(l.unit))
            .sort((a, b) => a.cmp(b));
        const freeUnits = Math.floor(units.length / (rule.buy + rule.free)) * rule.free;
        discount = units.slice(0, freeUnits).reduce((s, u) => s.add(u), ZERO);
        if (freeUnits) promo = promoCode;
    }
    // Spread the discount over lines in proportion to their totals; allocate() guarantees an exact sum.
    if (discount.amount() > 0) {
        const parts = discount.allocate(lines.map((l) => Math.max(l.total.amount(), 1)));
        lines.forEach((l, i) => {
            l.discount = parts[i];
        });
    }
    const rate = TAX[state];
    if (rate === undefined) throw Object.assign(new Error(`we do not ship to ${state}`), { status: 400 });
    let tax = ZERO;
    for (const l of lines) {
        l.tax = Money.fromDecimal(l.total.sub(l.discount).toDecimal().mul(rate).round(2, "halfUp"), CURRENCY);
        tax = tax.add(l.tax);
    }
    const goods = subtotal.sub(discount);
    const shipping = goods.cmp(usd("200.00")) >= 0 ? ZERO : shippingFor(lines.reduce((g, l) => g + l.weight, 0));
    return { lines, subtotal, discount, promo, tax, shipping, total: goods.add(tax).add(shipping) };
}
const quoteJSON = (q) => ({
    lines: q.lines.map((l) => ({
        sku: l.sku,
        name: l.name,
        qty: l.qty,
        unit: l.unit.toString(),
        total: l.total.toString(),
        discount: l.discount.toString(),
        tax: l.tax.toString(),
    })),
    subtotal: q.subtotal.toString(),
    discount: q.discount.toString(),
    promo: q.promo,
    tax: q.tax.toString(),
    shipping: q.shipping.toString(),
    total: q.total.toString(),
});

// =============================================================================
// Carts, checkout, events, invoices
// =============================================================================
const carts = new LRU(10_000, { ttlMs: 2 * 3600_000 }); // email -> Map(sku -> qty)
const idempotency = new LRU(10_000, { ttlMs: 24 * 3600_000 }); // key -> stored response
const eventLog = dataDir.join("orders.ndjson");

const invoiceTemplate = new Template(`<!doctype html>
<html><head><meta charset="utf-8"><title>Invoice {{id}}</title></head>
<body><h1>Invoice {{id}}</h1><p>Billed to {{customer}} ({{state}})</p>
<table><tr><th>Item</th><th>Qty</th><th>Unit</th><th>Total</th></tr>
{{#lines}}<tr><td>{{name}}</td><td>{{qty}}</td><td>{{unit}}</td><td>{{total}}</td></tr>
{{/lines}}</table>
<p>Subtotal {{subtotal}}{{#promo}} · promotion {{promo}} −{{discount}}{{/promo}} · tax {{tax}} · shipping {{shipping}}</p>
<p><strong>Total {{total}}</strong></p>
<pre class="qr">{{qr}}</pre></body></html>`);

function checkout(email, key, promoCode) {
    if (key && idempotency.has(key)) return { ...idempotency.get(key), replayed: true };
    const cart = carts.get(email);
    if (!cart || !cart.size) return { status: 400, body: { error: "cart is empty" } };
    const customer = db.query("SELECT * FROM customers WHERE email = ?", [email])[0];
    let quote;
    try {
        quote = price(cart, customer.state, promoCode);
    } catch (e) {
        return { status: e.status || 500, body: { error: e.message } };
    }

    const id = "ord_" + ULID(undefined, { monotonic: true });
    db.exec("BEGIN IMMEDIATE");
    try {
        for (const l of quote.lines) {
            // The guard and the decrement are one statement: the row only changes when enough stock is left.
            if (
                db.exec("UPDATE products SET stock = stock - ? WHERE sku = ? AND stock >= ?", [l.qty, l.sku, l.qty]) !==
                1
            )
                throw Object.assign(new Error(`only ${getProduct(l.sku).stock} left of ${l.sku}`), { status: 409 });
        }
        const body = { id, email, at: new Date().toISOString(), ...quoteJSON(quote) };
        db.exec("INSERT INTO orders VALUES (?, ?, ?, ?)", [id, email, quote.total.toString(), JSON.stringify(body)]);
        db.exec("COMMIT");
        writeFile(
            eventLog,
            JSON.stringify({
                type: "order.placed",
                ...body,
                lines: body.lines.map((l) => ({ ...l, category: getProduct(l.sku).category })),
            }) + "\n",
            { append: true },
        );
        carts.delete(email);
        Metrics.counter("orders_total", 1, { state: customer.state });
        Metrics.counter("revenue_cents_total", quote.total.amount());
        const result = { status: 201, body };
        if (key) idempotency.put(key, result);
        return result;
    } catch (e) {
        db.exec("ROLLBACK");
        Metrics.counter("checkout_failures_total", 1, { reason: e.status === 409 ? "stock" : "error" });
        return { status: e.status || 500, body: { error: e.message } };
    }
}

// =============================================================================
// HTTP surface
// =============================================================================
const app = new App({ port: options.port || 0 });
const reply = (status, body, headers) => ({
    status,
    body: JSON.stringify(body),
    contentType: "application/json",
    headers,
});
const authed = (req) => {
    const email = customerOf(req);
    return email ? { email } : { denied: reply(401, { error: "sign in first" }) };
};
const parse = (req) => {
    try {
        return JSON.parse(req.body || "{}");
    } catch {
        return null;
    }
};

app.post("/accounts", (req) => {
    const b = parse(req);
    if (
        !b ||
        !/^[^@\s]+@[^@\s]+\.[a-z]{2,}$/.test(b.email || "") ||
        (b.password || "").length < 10 ||
        !(b.state in TAX)
    )
        return reply(400, { error: "email, a password of 10+ characters and a state we ship to are required" });
    if (db.query("SELECT 1 AS x FROM customers WHERE email = ?", [b.email]).length)
        return reply(409, { error: "account exists" });
    db.exec("INSERT INTO customers VALUES (?, ?, ?, ?)", [
        b.email,
        b.name || b.email,
        b.state,
        Bcrypt.hash(b.password, selfTest ? 4 : 11),
    ]);
    return reply(201, { email: b.email });
});
app.post("/sessions", (req) => {
    const b = parse(req) || {};
    const row = db.query("SELECT pw FROM customers WHERE email = ?", [b.email || ""])[0];
    if (!row || !Bcrypt.verify(b.password || "", row.pw)) return reply(401, { error: "wrong email or password" });
    return reply(201, { token: issueSession(b.email) });
});

app.get("/search", (req) => {
    const r = search.search(req.query.q || "", Number(req.query.k) || 5);
    return {
        corrected: r.corrected,
        results: r.hits.map((h) => ({ ...h, name: getProduct(h.sku).name, price: getProduct(h.sku).price })),
    };
});
app.get("/suggest", (req) => search.suggest(req.query.prefix || ""));
app.get("/products/:sku", (req) => getProduct(req.params.sku) || reply(404, { error: "no such product" }));

app.patch("/admin/products/:sku", (req) => {
    if (req.headers["x-admin-token"] !== ADMIN_TOKEN) return reply(403, { error: "admin only" });
    const current = getProduct(req.params.sku);
    if (!current) return reply(404, { error: "no such product" });
    let next;
    try {
        next = Patch.apply(current, parse(req) || []);
    } catch (e) {
        return reply(400, { error: `patch failed: ${e.message}` });
    }
    const v = productSchema.validate(next);
    if (!v.valid || next.sku !== current.sku)
        return reply(422, {
            errors: v.errors
                .map((e) => `${e.path}: ${e.message}`)
                .concat(next.sku !== current.sku ? ["/sku: immutable"] : []),
        });
    db.exec(
        "UPDATE products SET name = ?, description = ?, category = ?, price = ?, weight_g = ?, stock = ? WHERE sku = ?",
        [next.name, next.description, next.category, next.price, next.weight_g, next.stock, next.sku],
    );
    search.index(next.sku, `${next.name} ${next.description} ${next.category}`);
    return next;
});

app.put("/cart/:sku", (req) => {
    const who = authed(req);
    if (who.denied) return who.denied;
    const qty = Number((parse(req) || {}).qty);
    if (!getProduct(req.params.sku)) return reply(404, { error: "no such product" });
    if (!Number.isInteger(qty) || qty < 0 || qty > 20) return reply(400, { error: "qty must be an integer 0..20" });
    const cart = carts.get(who.email) || new Map();
    if (qty === 0) cart.delete(req.params.sku);
    else cart.set(req.params.sku, qty);
    carts.put(who.email, cart);
    return { items: Object.fromEntries(cart) };
});
app.get("/cart/quote", (req) => {
    const who = authed(req);
    if (who.denied) return who.denied;
    const cart = carts.get(who.email);
    if (!cart) return reply(404, { error: "no cart" });
    try {
        return quoteJSON(
            price(cart, db.query("SELECT state FROM customers WHERE email = ?", [who.email])[0].state, req.query.promo),
        );
    } catch (e) {
        return reply(e.status || 500, { error: e.message });
    }
});
app.post("/checkout", (req) => {
    const who = authed(req);
    if (who.denied) return who.denied;
    const r = checkout(who.email, req.headers["idempotency-key"], (parse(req) || {}).promo);
    return reply(r.status, r.body, r.replayed ? { "idempotent-replay": "true" } : undefined);
});
app.get("/orders/:id/invoice", (req) => {
    const who = authed(req);
    if (who.denied) return who.denied;
    const row = db.query("SELECT * FROM orders WHERE id = ? AND email = ?", [req.params.id, who.email])[0];
    if (!row) return reply(404, { error: "no such order" });
    const o = JSON.parse(row.body);
    const customer = db.query("SELECT * FROM customers WHERE email = ?", [who.email])[0];
    const qr = QRToString(`PAY:${o.id}:${o.total}:${CURRENCY}`, { ecc: "M" });
    return {
        body: invoiceTemplate.render({ ...o, customer: customer.name, state: customer.state, qr }),
        contentType: "text/html; charset=utf-8",
    };
});
app.get("/metrics", () => ({ body: Metrics.scrape(), contentType: "text/plain" }));

// The nightly report: read the event log into a frame and aggregate.
function nightlyReport() {
    const events = readFile(eventLog)
        .split("\n")
        .filter(Boolean)
        .map((l) => JSON.parse(l));
    const rows = events.flatMap((e) =>
        e.lines.map((l) => ({
            order: e.id,
            category: l.category,
            sku: l.sku,
            qty: l.qty,
            cents: Money.fromString(l.total, CURRENCY).sub(Money.fromString(l.discount, CURRENCY)).amount(),
        })),
    );
    const df = new DataFrame({
        category: rows.map((r) => r.category),
        sku: rows.map((r) => r.sku),
        qty: Int32Array.from(rows, (r) => r.qty),
        cents: Float64Array.from(rows, (r) => r.cents),
    });
    const byCat = df.GROUP_BY_SUM("category", "cents");
    const units = df.GROUP_BY_SUM("sku", "qty");
    const best = units.keys
        .map((k, i) => [k, units.values[i]])
        .sort((a, b) => b[1] - a[1])
        .slice(0, 3);
    const csv =
        "category,revenue\n" +
        byCat.keys.map((k, i) => `${k},${new Money(byCat.values[i], CURRENCY)}`).join("\n") +
        "\n";
    writeFile(dataDir.join("revenue-by-category.csv"), csv);
    return {
        orders: events.length,
        revenueByCategory: Object.fromEntries(
            byCat.keys.map((k, i) => [k, new Money(byCat.values[i], CURRENCY).toString()]),
        ),
        bestSellers: best,
        csv,
    };
}

const ADMIN_TOKEN = selfTest
    ? "admin-demo-token"
    : RandomBytes(16).reduce((s, b) => s + b.toString(16).padStart(2, "0"), "");
app.start();
if (!selfTest) {
    print(`commerce-backend on http://127.0.0.1:${app.port}   data=${dataDir}   admin token: ${ADMIN_TOKEN}`);
} else {
    await scriptedDay();
}

// =============================================================================
// A scripted day of trading against the real HTTP surface
// =============================================================================
async function scriptedDay() {
    const results = {};
    const base = `http://127.0.0.1:${app.port}`;
    const call = async (method, path, body, headers = {}) => {
        const r = await fetch(base + path, {
            method,
            headers: { "content-type": "application/json", ...headers },
            body: body === undefined ? undefined : JSON.stringify(body),
        });
        const text = await r.text();
        let json = null;
        try {
            json = JSON.parse(text);
        } catch {
            json = text;
        }
        return { status: r.status, body: json, headers: r.headers };
    };
    const as = (token) => ({ authorization: `Bearer ${token}` });

    // Customers sign up and sign in.
    const people = [
        ["ana@example.com", "CA"],
        ["ben@example.com", "NY"],
        ["cho@example.com", "OR"],
    ];
    const tokens = {};
    for (const [email, state] of people) {
        results[`signup ${email}`] =
            (
                await call("POST", "/accounts", {
                    email,
                    name: email.split("@")[0],
                    password: "correct horse battery",
                    state,
                })
            ).status === 201;
        tokens[email] = (await call("POST", "/sessions", { email, password: "correct horse battery" })).body.token;
    }
    results["a wrong password is refused"] =
        (await call("POST", "/sessions", { email: "ana@example.com", password: "wrong password!" })).status === 401;

    // Search with a typo, then autocomplete.
    const s = (await call("GET", "/search?q=wireles%20headphnes")).body;
    results[`typo-tolerant search -> ${JSON.stringify(s)}`] =
        s.corrected === "wireless headphones" && s.results[0].sku === "AUD-101";
    const sug = (await call("GET", "/suggest?prefix=co")).body;
    results[`autocomplete "co" -> ${sug}`] = sug.includes("coffee");

    // Ana buys headphones and a grinder with WELCOME10, retrying the checkout with the same key.
    const ana = as(tokens["ana@example.com"]);
    await call("PUT", "/cart/AUD-101", { qty: 1 }, ana);
    await call("PUT", "/cart/HOM-305", { qty: 2 }, ana);
    const quote = (await call("GET", "/cart/quote?promo=WELCOME10", undefined, ana)).body;
    // 149.00 + 2*129.00 = 407.00; 10% = 40.70; CA tax 7.25% per line, half-up: 9.72 + 16.83 = 26.55; free shipping over 200.
    results[`quote ${JSON.stringify(quote)}`] =
        quote.subtotal === "407.00" &&
        quote.discount === "40.70" &&
        quote.tax === "26.55" &&
        quote.shipping === "0.00" &&
        quote.total === "392.85";
    results["line discounts sum exactly to the order discount"] =
        quote.lines.reduce((s2, l) => s2.add(Money.fromString(l.discount, CURRENCY)), ZERO).toString() === "40.70";
    const key = "chk-" + ULID();
    const o1 = await call("POST", "/checkout", { promo: "WELCOME10" }, { ...ana, "idempotency-key": key });
    const o1again = await call("POST", "/checkout", { promo: "WELCOME10" }, { ...ana, "idempotency-key": key });
    results["a retried checkout returns the same order"] =
        o1.status === 201 &&
        o1again.status === 201 &&
        o1again.body.id === o1.body.id &&
        o1again.headers.get("idempotent-replay") === "true";
    results["stock decremented once despite the retry"] = getProduct("HOM-305").stock === 23;

    // Ben buys three books with COFFEE3: the cheapest one is free; NY tax; weight-banded shipping.
    const ben = as(tokens["ben@example.com"]);
    await call("PUT", "/cart/BOK-501", { qty: 2 }, ben);
    await call("PUT", "/cart/BOK-502", { qty: 1 }, ben);
    const o2 = await call("POST", "/checkout", { promo: "COFFEE3" }, ben);
    // 2*24.99 + 39.99 = 89.97; free unit 24.99 -> 64.98; NY 4% per line; 2500 g -> 19.99 shipping.
    results[`bogo order ${JSON.stringify(o2.body)}`] =
        o2.status === 201 && o2.body.discount === "24.99" && o2.body.shipping === "19.99";

    // Ben and Cho race for the last USB-C dock: exactly one wins.
    const cho = as(tokens["cho@example.com"]);
    await call("PUT", "/cart/CMP-230", { qty: 1 }, ben);
    await call("PUT", "/cart/CMP-230", { qty: 1 }, cho);
    const race = await Promise.all([call("POST", "/checkout", {}, ben), call("POST", "/checkout", {}, cho)]);
    results[`race for the last unit -> ${race.map((r) => r.status)}`] =
        race
            .map((r) => r.status)
            .sort()
            .join() === "201,409" && getProduct("CMP-230").stock === 0;

    // An admin re-prices the earbuds with a JSON Patch; an invalid patch is refused by the schema.
    const admin = { "x-admin-token": ADMIN_TOKEN };
    const repriced = await call(
        "PATCH",
        "/admin/products/AUD-102",
        [
            { op: "replace", path: "/price", value: "79.00" },
            { op: "replace", path: "/name", value: "Wireless earbuds Pro" },
        ],
        admin,
    );
    results["admin patch applied"] = repriced.status === 200 && getProduct("AUD-102").price === "79.00";
    const invalid = await call(
        "PATCH",
        "/admin/products/AUD-102",
        [{ op: "replace", path: "/category", value: "toys" }],
        admin,
    );
    results["schema rejects a patch to an unknown category"] = invalid.status === 422;
    results["the search index follows catalog edits"] =
        (await call("GET", "/search?q=earbuds%20pro")).body.results[0].sku === "AUD-102";

    // The invoice renders the order, escaped, with a payment QR code.
    const invoice = await call("GET", `/orders/${o1.body.id}/invoice`, undefined, ana);
    results["invoice renders totals and the promotion"] =
        invoice.status === 200 && invoice.body.includes("Total 392.85") && invoice.body.includes("WELCOME10");
    results["another customer cannot read the invoice"] =
        (await call("GET", `/orders/${o1.body.id}/invoice`, undefined, ben)).status === 404;

    // Nightly report from the event log; its revenue equals the sum of the orders, to the cent.
    const report = nightlyReport();
    const placed = db.query("SELECT total FROM orders");
    const orderSum = placed.reduce((s2, r) => s2.add(Money.fromString(r.total, CURRENCY)), ZERO);
    const winner = race.find((r) => r.status === 201).body;
    results[`three orders in the log (${report.orders})`] = report.orders === 3 && placed.length === 3;
    const goods = [o1.body, o2.body, winner].reduce(
        (s2, o) => s2.add(Money.fromString(o.subtotal, CURRENCY)).sub(Money.fromString(o.discount, CURRENCY)),
        ZERO,
    );
    const reportGoods = Object.values(report.revenueByCategory).reduce(
        (s2, v) => s2.add(Money.fromString(v, CURRENCY)),
        ZERO,
    );
    results[`report goods revenue ${reportGoods} equals orders ${goods}`] = reportGoods.equals(goods);
    const metrics = (await call("GET", "/metrics")).body;
    results["metrics count orders and stock failures"] =
        /orders_total\{state="CA"\} 1/.test(metrics) && /checkout_failures_total\{reason="stock"\} 1/.test(metrics);

    print("commerce-backend: a day of trading");
    print(`  orders     ${placed.length}, takings ${orderSum} (goods ${goods})`);
    print(`  revenue    ${JSON.stringify(report.revenueByCategory)}`);
    print(`  best       ${report.bestSellers.map(([sku, n]) => `${sku}×${n}`).join(", ")}`);
    print(`  search     "wireles headphnes" -> "${s.corrected}" -> ${s.results[0].name}`);
    app.close();
    db.close();
    removeAll(dataDir);
    check(results);
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
