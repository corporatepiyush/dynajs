// Autocomplete service — prefix completion over a popularity-ranked phrase dictionary with typo tolerance,
// per-user recency boosts and trending detection, served over HTTP and checked against brute force.
//
// WHAT IT IS
//   The search-box backend of a shop or a docs site:
//
//   dictionary  phrases learned from a query log; popularity per phrase. Phrases live in an OrderedMap so a prefix
//               is one contiguous key range (prefix()), and the hottest short prefixes (1-3 characters) have their
//               top-10 precomputed, because those ranges are the largest and the most frequently typed
//   ranking     candidates' scores go into a Float32Array and topkIndices picks the best k without sorting the range
//   typos       when a prefix finds too little, edit-distance-1 neighbours of the prefix are generated (deletes,
//               transposes, substitutions over the letters actually used) and kept only if they are real prefixes
//               in the Trie; Damerau-Levenshtein (dyna:matcher) orders the fuzzy hits
//   personal    each user's last 8 queries sit in a RingBuffer and boost matching completions
//   trending    hourly counts in CountMinSketches; a phrase whose last-hour count beats its trailing average by 5x is
//               trending and its score is multiplied by that ratio (capped at 10)
//   oracle      every completion list is recomputed by scanning all phrases with startsWith and sorting
//
// MODULES  dyna:structures (OrderedMap, Trie, RingBuffer, CountMinSketch, TDigest) · dyna:matcher (DamerauLevenshtein) ·
//          dyna:simd (topkIndices) · dyna:http (App, fetch) · dyna:random · dyna:cli · String/Array extensions
//
// RUN      dynajs examples/text/autocomplete-service.js

import { OrderedMap, Trie, RingBuffer, CountMinSketch, TDigest } from "dyna:structures";
import { DamerauLevenshtein } from "dyna:matcher";
import { topkIndices } from "dyna:simd";
import { App } from "dyna:http";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";

const results = {};
const rng = new Random(314);
const K = 10;

// =============================================================================
// Query log
// =============================================================================
const brands = [
    "iphone",
    "ipad",
    "galaxy",
    "pixel",
    "macbook",
    "thinkpad",
    "kindle",
    "airpods",
    "playstation",
    "nintendo",
];
const qualifiers = [
    "case",
    "charger",
    "screen protector",
    "15 pro",
    "pro max",
    "mini",
    "refurbished",
    "deals",
    "battery",
    "stand",
    "cable",
    "2026",
];
const phrases = [];
for (const b of brands) {
    phrases.push(b);
    for (const q of qualifiers) phrases.push(`${b} ${q}`);
}
const extra = [
    "ice maker",
    "ikea desk",
    "instant pot",
    "indoor plants",
    "ink cartridges",
    "immersion blender",
    "iron",
    "irobot roomba",
];
phrases.push(...extra);
const popularity = new Map(phrases.map((p, i) => [p, Math.round(5000 / (1 + ((i * 7919) % phrases.length)) ** 0.9)]));
const log = [];
for (let i = 0; i < 60_000; i++) {
    const p = phrases[Math.min(phrases.length - 1, Math.floor(rng.exponential(1 / 25)))];
    log.push({ t: i, q: rng.nextFloat() < 0.04 ? typo(p) : p });
}
function typo(s) {
    const i = 1 + rng.nextBounded(Math.max(1, s.length - 2));
    return s.slice(0, i) + s[i + 1] + s[i] + s.slice(i + 2);
}

// =============================================================================
// Index
// =============================================================================
const dict = new OrderedMap(),
    trie = new Trie();
const counts = new Map();
for (const { q } of log) counts.set(q, (counts.get(q) ?? 0) + 1);
for (const [q, n] of counts) {
    if (n < 20) continue;
    dict.set(q, n + (popularity.get(q) ?? 0));
    trie.insert(q);
}
const alphabet = [...new Set([...dict.keys()].join(""))].join("");
const precomputed = new Map();
function rank(cands, scoreOf, k = K) {
    if (cands.length <= k)
        return cands.map((c) => [c, scoreOf(c)]).sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : 1));
    // topkIndices picks k winners among equal scores arbitrarily; widen to every candidate tied with the k-th, then order.
    const s = Float32Array.from(cands, scoreOf);
    let kth = Infinity;
    for (const i of topkIndices(s, k)) kth = Math.min(kth, s[i]);
    const out = [];
    for (let i = 0; i < s.length; i++) if (s[i] >= kth) out.push([cands[i], s[i]]);
    return out.sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : 1)).slice(0, k);
}
for (const key of dict.keys()) for (let n = 1; n <= 3 && n <= key.length; n++) precomputed.set(key.slice(0, n), null);
for (const p of precomputed.keys())
    precomputed.set(
        p,
        rank(
            dict.prefix(p).map(([k]) => k),
            (k) => dict.get(k),
        ),
    );

// =============================================================================
// Personal history and trending
// =============================================================================
const history = new Map();
const hourly = [];
function recordQuery(user, q, hour) {
    (history.get(user) ?? history.set(user, new RingBuffer(8)).get(user)).push(q);
    while (hourly.length <= hour) hourly.push(new CountMinSketch(2048, 4));
    hourly[hour].add(q);
}
function trendBoost(q, hour) {
    if (hour < 1) return 1;
    const last = hourly[hour]?.count(q) ?? 0;
    const window = hourly.slice(Math.max(0, hour - 6), hour);
    const avg = window.map((h) => h.count(q)).average() || 0;
    return last >= 20 && last > 5 * Math.max(1, avg) ? Math.min(10, last / Math.max(1, avg)) : 1;
}

// =============================================================================
// Completion
// =============================================================================
function edits1(prefix) {
    const out = new Set();
    for (let i = 0; i < prefix.length; i++) {
        out.add(prefix.slice(0, i) + prefix.slice(i + 1));
        if (i + 1 < prefix.length) out.add(prefix.slice(0, i) + prefix[i + 1] + prefix[i] + prefix.slice(i + 2));
        for (const c of alphabet) out.add(prefix.slice(0, i) + c + prefix.slice(i + 1));
    }
    return [...out].filter((p) => p && trie.keysWithPrefix(p).length > 0);
}
function complete(raw, { user = null, hour = 0, typos = true } = {}) {
    const prefix = raw.toLowerCase().replace(/\s+/g, " ").trimStart();
    if (!prefix) return { items: [], fuzzy: false };
    const recent = new Set(user && history.get(user) ? history.get(user).toArray() : []);
    const score = (k) => dict.get(k) * (recent.has(k) ? 50 : 1) * trendBoost(k, hour);
    const plain = !recent.size && hour === 0;
    let items =
        plain && precomputed.has(prefix)
            ? precomputed.get(prefix)
            : rank(
                  dict.prefix(prefix).map(([k]) => k),
                  score,
              );
    let fuzzy = false;
    if (typos && items.length < 3 && prefix.length >= 3) {
        const near = new Set();
        const have = new Set(items.map(([k]) => k));
        for (const p of edits1(prefix)) for (const k of trie.keysWithPrefix(p)) if (!have.has(k)) near.add(k);
        const ranked = [...near]
            .map((k) => [k, DamerauLevenshtein(prefix, k.slice(0, prefix.length)), dict.get(k)])
            .sort((a, b) => a[1] - b[1] || b[2] - a[2])
            .slice(0, K - items.length);
        items = [...items, ...ranked.map(([k, , s]) => [k, s])];
        fuzzy = ranked.length > 0;
    }
    return { items, fuzzy };
}
const bruteForce = (prefix) =>
    [...dict.keys()]
        .filter((k) => k.startsWith(prefix))
        .map((k) => [k, dict.get(k)])
        .sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : 1))
        .slice(0, K);

// =============================================================================
// Checks against brute force
// =============================================================================
{
    let mismatches = 0,
        checked = 0;
    for (const key of dict.keys())
        for (let n = 1; n <= key.length; n += 2) {
            const p = key.slice(0, n);
            checked++;
            if (JSON.stringify(complete(p, { typos: false }).items) !== JSON.stringify(bruteForce(p))) mismatches++;
        }
    results["prefix completions equal a brute-force scan"] = mismatches === 0 && checked > 500;
    results["short prefixes come from the precomputed table"] =
        precomputed.get("i").length === K && precomputed.get("i") === complete("i").items;
    results["typo'd log entries below the support threshold are not in the dictionary"] = !dict.has(
        typo("iphone case"),
    );
}
{
    const r = complete("iphnoe ca");
    results["transposed prefix still completes (fuzzy)"] = r.fuzzy && r.items.some(([k]) => k === "iphone case");
    const r2 = complete("macbok");
    results["dropped letter still completes"] = r2.fuzzy && r2.items.some(([k]) => k.startsWith("macbook"));
    results["nonsense finds nothing"] = complete("qqqqzz").items.length === 0;
    results["whitespace and case are normalised"] =
        JSON.stringify(complete("  IPHONE  c").items) === JSON.stringify(complete("iphone c").items);
}
{
    const base = complete("ki").items.map(([k]) => k);
    const target = base.at(-1);
    for (const q of ["galaxy case", target, "pixel mini"]) recordQuery("alice", q, 0);
    const mine = complete("ki", { user: "alice" }).items.map(([k]) => k);
    results["a user's recent query is boosted to the top"] = base[0] !== target && mine[0] === target;
    results["other users are unaffected"] =
        JSON.stringify(complete("ki", { user: "bob" }).items.map(([k]) => k)) === JSON.stringify(base);
}
{
    for (let h = 0; h < 7; h++)
        for (let i = 0; i < 400; i++)
            recordQuery(`u${i % 50}`, phrases[Math.floor(rng.exponential(1 / 25)) % phrases.length], h);
    const sleeper = rank(
        dict.prefix("i").map(([k]) => k),
        (k) => dict.get(k),
        1000,
    )[14][0];
    for (let i = 0; i < 120; i++) recordQuery(`u${i % 50}`, sleeper, 7);
    const before = complete("i").items.map(([k]) => k);
    const after = complete("i", { hour: 7 }).items.map(([k]) => k);
    results["a spiking phrase is detected as trending"] = trendBoost(sleeper, 7) > 1 && trendBoost(before[0], 7) === 1;
    results["trending phrase moves into the top 10"] = !before.includes(sleeper) && after.includes(sleeper);
}

// =============================================================================
// Serving
// =============================================================================
const latency = new TDigest(100);
const app = new App({ port: 0 });
app.get("/complete", (req) => {
    const t0 = performance.now();
    const r = complete(req.query.q ?? "", { user: req.query.user ?? null });
    latency.add(performance.now() - t0);
    return { q: req.query.q, fuzzy: r.fuzzy, items: r.items.map(([k]) => k) };
});
app.start();
const base = `http://127.0.0.1:${app.port}`;
const typed = [];
for (let i = 0; i < 300; i++) {
    const p = phrases[rng.nextBounded(phrases.length)];
    for (let n = 1; n <= Math.min(p.length, 8); n++) typed.push(p.slice(0, n));
}
let served = 0;
for (let i = 0; i < typed.length; i += 64) {
    const rs = await Promise.all(
        typed
            .slice(i, i + 64)
            .map((q) => fetch(`${base}/complete?q=${encodeURIComponent(q)}&user=alice`).then((r) => r.json())),
    );
    served += rs.filter((r) => Array.isArray(r.items)).length;
}
const empty = await (await fetch(`${base}/complete?q=`)).json();
results["every keystroke is served"] = served === typed.length;
results["empty query returns nothing"] = empty.items.length === 0;
results["p99 completion time under 2 ms"] = latency.quantile(0.99) < 2;
print(
    Table(
        [
            ["phrases", dict.keys().length.format()],
            ["precomputed prefixes", precomputed.size.format()],
            ["keystrokes served", served.format()],
            ["p50 / p99 ms", `${latency.quantile(0.5).toFixed(3)} / ${latency.quantile(0.99).toFixed(3)}`],
        ],
        { head: ["autocomplete", "value"] },
    ),
);

app.close();
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
