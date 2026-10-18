// Similarity detection — source-code plagiarism with MOSS-style winnowing, near-duplicate web pages with SimHash,
// topical similarity with TF-IDF vectors, passage alignment with Smith–Waterman, and clustering of the results.
//
// WHAT IT IS
//   Three detectors a university, a search engine and a content team each run, sharing one report:
//
//   code         48 student submissions of the same assignment. Each is lexed, identifiers and literals are
//                replaced by placeholders (so renaming variables does not hide copying), comments and whitespace
//                dropped; k-gram hashes are WINNOWED (the minimum hash in every window of w, Schleimer et al.
//                2003) to get position-aware fingerprints. Pairs sharing many fingerprints are aligned
//                token-by-token with Smith–Waterman to report the matching line ranges, and a template file
//                handed out by the instructor is subtracted so starter code never counts as copying
//   pages        2,000 crawled pages: 64-bit SimHash over TF-IDF-weighted words; near-duplicates are pages
//                within Hamming distance 7, found with 8-band LSH (any such pair shares a band) rather than all pairs
//   topics       TF-IDF vectors with cosine similarity computed by SIMD dot products, to separate "same topic"
//                from "same text"
//   clusters     pairs above threshold become graph edges; connected components are the copy rings / duplicate
//                groups; each cluster gets a representative and a word-level diff against it
//
//   bits         SimHash votes and fingerprints work on two 32-bit halves (no BigInt shift per bit), and the
//                Hamming distance is two native popcounts (mathx.bits.onesCount32)
//
// MODULES  dyna:hash (XXH3, XXHash32) · dyna:simd (dot, normL2, scale) · dyna:structures (Graph, Multimap, Interner)
//          · dyna:matcher (SmithWaterman, DiffWords) · dyna:html (Template) · dyna:cli (Table) · dyna:random · dyna:mathx (bits)
//
// RUN      dynajs examples/text/similarity-detection.js     generates submissions and pages with planted copies

import { XXH3_64, XXHash32 } from "dyna:hash";
import { dot, normL2, scale } from "dyna:simd";
import { Graph, Multimap, Interner } from "dyna:structures";
import { DiffWords, SmithWaterman } from "dyna:matcher";
import { bits } from "dyna:mathx";
import { Template } from "dyna:html";
import { Table } from "dyna:cli";
import { Random } from "dyna:random";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const rng = new Random(2003);

// =============================================================================
// Part 1: code submissions
// =============================================================================
const KEYWORDS = new Set(
    "function return const let var if else for while of in new class this true false null undefined break continue throw try catch".split(
        " ",
    ),
);
function lex(src) {
    const toks = [];
    src.split("\n").forEach((line, ln) => {
        const code = line.replace(/\/\/.*$/, "");
        for (const m of code.matchAll(/"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|\d+(?:\.\d+)?|[A-Za-z_$][\w$]*|[^\s\w]/g)) {
            const t = m[0];
            toks.push({
                norm: /^["'\d]/.test(t) ? "LIT" : /^[A-Za-z_$]/.test(t) && !KEYWORDS.has(t) ? "ID" : t,
                line: ln + 1,
            });
        }
    });
    return toks;
}
const K = 8,
    W = 6;
function winnow(toks) {
    const hashes = [];
    for (let i = 0; i + K <= toks.length; i++)
        hashes.push({
            h: XXHash32(
                toks
                    .slice(i, i + K)
                    .map((t) => t.norm)
                    .join(" "),
                7,
            ),
            pos: i,
        });
    const fp = new Map();
    let last = -1;
    for (let i = 0; i + W <= hashes.length; i++) {
        let min = i;
        for (let j = i; j < i + W; j++) if (hashes[j].h <= hashes[min].h) min = j; // rightmost minimum
        if (min !== last) {
            fp.set(hashes[min].h, hashes[min].pos);
            last = min;
        }
    }
    return fp;
}
// Local alignment over normalised token ids in C (matcher.SmithWaterman on Int32Arrays): match +2, mismatch -1,
// gap -1, ties preferring the diagonal, so the reported line ranges are those of the textbook recurrence.
const tokenIds = new Interner();
function smithWaterman(a, b) {
    const r = SmithWaterman(a.ids, b.ids, { match: 2, mismatch: -1, gap: -1 });
    return {
        score: r.score,
        a: [a[r.aStart]?.line, a[r.aEnd - 1]?.line],
        b: [b[r.bStart]?.line, b[r.bEnd - 1]?.line],
        tokens: r.aEnd - r.aStart,
    };
}
const TEMPLATE = `// Assignment 3: inventory report. Do not change the signature.
function report(items) {
  // TODO: implement
  return "";
}
export { report };
`;
const SEGMENTS = [
    // every student composes one variant of each step
    [
        (n) => `  const ${n.totals} = new Map();`,
        (n) => `  const ${n.sums} = {};\n  const ${n.keys} = [];`,
        (n) => `  let ${n.acc} = Object.create(null);`,
    ],
    [
        (n) => `  for (const ${n.item} of ${n.items}) {\n    if (!${n.item}.sku || ${n.item}.qty < 0) continue;`,
        (n) =>
            `  for (let ${n.i} = 0; ${n.i} < ${n.items}.length; ${n.i}++) {\n    const ${n.item} = ${n.items}[${n.i}];\n    if (${n.item}.qty < 0) { continue; }`,
        (n) => `  ${n.items}.filter((${n.item}) => ${n.item}.sku && ${n.item}.qty >= 0).forEach((${n.item}) => {`,
    ],
    [
        (n) =>
            `    const ${n.prev} = ${n.totals}.get(${n.item}.sku) || 0;\n    ${n.totals}.set(${n.item}.sku, ${n.prev} + ${n.item}.qty * ${n.item}.price);\n  }`,
        (n) =>
            `    if (!(${n.item}.sku in ${n.sums})) { ${n.sums}[${n.item}.sku] = 0; ${n.keys}.push(${n.item}.sku); }\n    ${n.sums}[${n.item}.sku] += ${n.item}.price * ${n.item}.qty;\n  }`,
        (n) => `    ${n.acc}[${n.item}.sku] = (${n.acc}[${n.item}.sku] ?? 0) + ${n.item}.qty * ${n.item}.price;\n  });`,
    ],
    [
        (n) =>
            `  const ${n.lines} = [];\n  for (const [${n.sku}, ${n.value}] of ${n.totals}) ${n.lines}.push(${n.sku} + ": " + ${n.value}.toFixed(2));\n  ${n.lines}.sort();\n  return ${n.lines}.join("\\n");`,
        (n) =>
            `  ${n.keys}.sort();\n  let ${n.out} = "";\n  for (const ${n.k} of ${n.keys}) { ${n.out} += ${n.k} + ": " + ${n.sums}[${n.k}].toFixed(2) + "\\n"; }\n  return ${n.out}.trim();`,
        (n) =>
            `  return Object.keys(${n.acc}).sort().map((${n.sku}) => \`\${${n.sku}}: \${${n.acc}[${n.sku}].toFixed(2)}\`).join("\\n");`,
    ],
];
const NAMES = [
    "items",
    "totals",
    "item",
    "prev",
    "lines",
    "sku",
    "value",
    "acc",
    "a",
    "b",
    "out",
    "keys",
    "sums",
    "i",
    "k",
];
const ALIASES = [
    "list",
    "inventory",
    "rows",
    "entries",
    "data",
    "map",
    "x",
    "it",
    "row",
    "rec",
    "res",
    "result",
    "agg",
    "memo",
    "s",
    "v",
    "key",
    "val",
    "idx",
    "j",
    "buf",
    "txt",
    "names",
    "sumBy",
];
const renamed = () =>
    Object.fromEntries(
        NAMES.map((nm, i) => [
            nm,
            `${ALIASES[(i * 7 + rng.nextBounded(ALIASES.length)) % ALIASES.length]}${rng.nextBounded(9)}`,
        ]),
    );
const NOISE = [
    "  // validate input",
    '  if (!Array.isArray(ITEMS)) throw new TypeError("items must be an array");',
    '  console.log("report", ITEMS.length);',
    "  const started = Date.now();",
    "  let count = 0;",
    "  const seen = new Set();",
];
const subs = [];
for (let s = 0; s < 48; s++) {
    const names = rng.nextFloat() < 0.5 ? renamed() : Object.fromEntries(NAMES.map((n) => [n, n]));
    const body = SEGMENTS.map((variants) => variants[rng.nextBounded(variants.length)](names));
    for (let k = 0, extra = 1 + rng.nextBounded(3); k < extra; k++)
        body.splice(
            rng.nextBounded(body.length),
            0,
            NOISE[rng.nextBounded(NOISE.length)].replace(/ITEMS/g, names.items),
        );
    const helpers = Array.from(
        { length: 2 + rng.nextBounded(6) },
        (_, k) => `const helper${s}_${k} = (x${k}) => x${k} * ${k + s} + ${rng.nextBounded(100)};`,
    ).join("\n");
    subs.push({
        id: `S${String(s).padStart(2, "0")}`,
        code: `${helpers}\n// Assignment 3: inventory report. Do not change the signature.\nfunction report(${names.items}) {\n${body.join("\n")}\n}\nexport { report };\n// student ${s}`,
        copiedFrom: null,
    });
}
// Planted copy rings: S05 <- S02 renamed + comments + reordered helpers; S11, S17 <- S08 with edits; S40 copies half of S31.
const copy = (from, to, f) => {
    subs[to].code = f(subs[from].code);
    subs[to].copiedFrom = subs[from].id;
};
copy(2, 5, (c) =>
    c
        .replace(
            /\b(items|totals|item|lines)\b/g,
            (w) => ({ items: "stock", totals: "acc", item: "entry", lines: "rows" })[w],
        )
        .replace(/\n/g, "\n  // my own work\n"),
);
copy(8, 11, (c) => c.replace(/helper8/g, "util11").replace("continue;", "{ continue; }"));
copy(8, 17, (c) => c.replace(/;\n/g, ";\n\n").replace(/sums/g, "total"));
copy(
    31,
    40,
    (c) =>
        c
            .split("\n")
            .slice(0, Math.floor(c.split("\n").length * 0.6))
            .join("\n") + "\n// rest written myself\nfunction tail() { return 1; }",
);
const templateFp = winnow(lex(TEMPLATE));
for (const s of subs) {
    s.toks = lex(s.code);
    s.toks.ids = Int32Array.from(tokenIds.internMany(s.toks.map((t) => t.norm)));
    s.fp = winnow(s.toks);
    for (const h of templateFp.keys()) s.fp.delete(h);
}
// Inverted index over fingerprints avoids comparing all pairs; very common fingerprints (in > 50%) are boilerplate.
const postings = new Multimap();
for (const s of subs) for (const h of s.fp.keys()) postings.put(String(h), s.id);
const pairShared = new Map();
for (const k of postings.keys()) {
    const ids = postings.get(k);
    if (ids.length > subs.length / 2) continue;
    for (let a = 0; a < ids.length; a++)
        for (let b = a + 1; b < ids.length; b++) {
            const key = [ids[a], ids[b]].sort().join("|");
            pairShared.set(key, (pairShared.get(key) || 0) + 1);
        }
}
const byId = new Map(subs.map((s) => [s.id, s]));
const codePairs = [...pairShared]
    .map(([key, shared]) => {
        const [a, b] = key.split("|").map((id) => byId.get(id));
        return { a: a.id, b: b.id, shared, sim: shared / Math.min(a.fp.size, b.fp.size) };
    })
    .filter((p) => p.sim >= 0.5)
    .sort((x, y) => y.sim - x.sim)
    .map((p) => ({ ...p, align: smithWaterman(byId.get(p.a).toks, byId.get(p.b).toks) }));

// =============================================================================
// Part 2: web pages — SimHash + LSH, and TF-IDF cosine
// =============================================================================
const TOPICS = {
    cooking: "recipe oven bake flour sugar butter minutes dough tray golden crust".split(" "),
    travel: "flight hotel beach museum city train passport itinerary tour sunset".split(" "),
    finance: "stock bond yield market interest inflation portfolio dividend risk return".split(" "),
    tech: "server cache latency deploy cluster database query index memory thread".split(" "),
};
const FILL = "the a of and to in is for on with as by at from this that it be are was".split(" ");
const pages = [];
for (let p = 0; p < 2000; p++) {
    const topic = Object.keys(TOPICS)[p % 4],
        words = [];
    for (let k = 0; k < 120; k++)
        words.push(
            rng.nextFloat() < 0.45
                ? TOPICS[topic][rng.nextBounded(10)]
                : rng.nextFloat() < 0.5
                  ? FILL[rng.nextBounded(FILL.length)]
                  : `w${rng.nextBounded(3000)}`,
        );
    pages.push({ id: p, topic, text: words.join(" "), dupOf: null });
}
for (let d = 0; d < 60; d++) {
    // near-duplicates: boilerplate swaps and a few edits
    const src = pages[rng.nextBounded(1000)],
        words = src.text.split(" ");
    for (let e = 0; e < 2; e++) words[rng.nextBounded(words.length)] = `edit${e}`;
    pages.push({ id: pages.length, topic: src.topic, text: `${words.join(" ")} share`, dupOf: src.id });
}
function simhash(text, idf) {
    // TF-IDF weighted: common words barely vote
    const v = new Float64Array(64),
        tf = new Map();
    for (const w of text.split(" ")) tf.set(w, (tf.get(w) || 0) + 1);
    for (const [w, n] of tf) {
        const hex = XXH3_64(w, 0, { as: "hex" }).padStart(16, "0"),
            lo = parseInt(hex.slice(8), 16),
            hi = parseInt(hex.slice(0, 8), 16),
            weight = n * (idf.get(w) ?? 1);
        for (let b = 0; b < 32; b++) {
            v[b] += (lo >>> b) & 1 ? weight : -weight;
            v[32 + b] += (hi >>> b) & 1 ? weight : -weight;
        }
    }
    let lo = 0,
        hi = 0;
    for (let b = 0; b < 32; b++) {
        if (v[b] > 0) lo |= 1 << b;
        if (v[32 + b] > 0) hi |= 1 << b;
    }
    return [lo >>> 0, hi >>> 0];
}
const hamming = (a, b) => bits.onesCount32((a[0] ^ b[0]) >>> 0) + bits.onesCount32((a[1] ^ b[1]) >>> 0);
const docFreq = new Map();
for (const p of pages) for (const w of new Set(p.text.split(" "))) docFreq.set(w, (docFreq.get(w) || 0) + 1);
const idfOf = new Map([...docFreq].map(([w, n]) => [w, Math.log(pages.length / n)]));
for (const p of pages) p.sim = simhash(p.text, idfOf);
const bands = new Multimap();
for (const p of pages) for (let b = 0; b < 8; b++) bands.put(`${b}:${(p.sim[b >> 2] >>> (8 * (b & 3))) & 0xff}`, p.id);
const dupPairs = new Set(),
    compared = { n: 0 };
for (const key of bands.keys()) {
    const ids = bands.get(key);
    for (let a = 0; a < ids.length; a++)
        for (let b = a + 1; b < ids.length; b++) {
            compared.n++;
            if (hamming(pages[ids[a]].sim, pages[ids[b]].sim) <= 7)
                dupPairs.add([ids[a], ids[b]].sort((x, y) => x - y).join("|"));
        }
}
const vocab = new Map(),
    df = new Map();
for (const p of pages) for (const w of new Set(p.text.split(" "))) df.set(w, (df.get(w) || 0) + 1);
for (const [w, n] of df) if (n >= 5 && n < pages.length * 0.5) vocab.set(w, vocab.size);
for (const p of pages) {
    const v = new Float32Array(vocab.size);
    for (const w of p.text.split(" ")) if (vocab.has(w)) v[vocab.get(w)] += Math.log(pages.length / df.get(w));
    p.vec = scale(v, 1 / (normL2(v) || 1));
}
const sameTopic = [],
    crossTopic = [];
for (let k = 0; k < 400; k++) {
    const a = pages[rng.nextBounded(2000)],
        b = pages[rng.nextBounded(2000)];
    if (a === b) continue;
    (a.topic === b.topic ? sameTopic : crossTopic).push(dot(a.vec, b.vec));
}
const mean = (xs) => xs.reduce((s, x) => s + x, 0) / xs.length;

// =============================================================================
// Clusters and report
// =============================================================================
function clusters(ids, pairs) {
    const g = new Graph(),
        idx = new Map(ids.map((id) => [id, g.addNode()]));
    for (const [a, b] of pairs) g.addEdge(idx.get(a), idx.get(b));
    const comp = g.connectedComponents(),
        rev = new Map([...idx].map(([id, n]) => [n, id])),
        out = new Map();
    (Array.isArray(comp) ? comp : comp.labels).forEach((c, n) => {
        if (!out.has(c)) out.set(c, []);
        out.get(c).push(rev.get(n));
    });
    return [...out.values()].filter((c) => c.length > 1);
}
const codeClusters = clusters(
    subs.map((s) => s.id),
    codePairs.filter((p) => p.sim >= 0.6).map((p) => [p.a, p.b]),
);
const pageClusters = clusters(
    pages.map((p) => p.id),
    [...dupPairs].map((k) => k.split("|").map(Number)),
);

const results = {};
const flagged = new Set(codePairs.filter((p) => p.sim >= 0.6).flatMap((p) => [`${p.a}|${p.b}`]));
const planted = subs.filter((s) => s.copiedFrom).map((s) => [s.copiedFrom, s.id].sort().join("|"));
results[`every planted copy is flagged (${planted.filter((k) => flagged.has(k)).length}/${planted.length})`] =
    planted.every((k) => flagged.has(k));
results["the S08/S11/S17 ring forms one cluster"] = codeClusters.some((c) =>
    ["S08", "S11", "S17"].every((x) => c.includes(x)),
);
const renamedPair = codePairs.find((p) => [p.a, p.b].sort().join() === "S02,S05");
results[`renaming identifiers and adding comments does not hide copying (similarity ${renamedPair?.sim.toFixed(2)})`] =
    renamedPair && renamedPair.sim > 0.8 && renamedPair.align.tokens > 60;
const partial = codePairs.find((p) => [p.a, p.b].sort().join() === "S31,S40");
results["a partial copy aligns to the copied line range only"] =
    partial && partial.align.a[1] - partial.align.a[0] < byId.get("S31").code.split("\n").length;
const templateOnly = winnow(lex(TEMPLATE));
results["the instructor's template is subtracted from every submission"] = [...templateOnly.keys()].every((h) =>
    subs.every((s) => !s.fp.has(h)),
);
const truthDups = pages.filter((p) => p.dupOf !== null).map((p) => [p.dupOf, p.id].sort((x, y) => x - y).join("|"));
for (const a of pages)
    for (const b of pages)
        if (a.dupOf !== null && a.dupOf === b.dupOf && a.id < b.id) truthDups.push(`${a.id}|${b.id}`); // two copies of one page are duplicates too
const recall = truthDups.filter((k) => dupPairs.has(k)).length / truthDups.length;
const falseDup = [...dupPairs].filter((k) => !truthDups.includes(k)).length;
results[
    `SimHash near-duplicates: recall ${(recall * 100).toFixed(0)}%, ${falseDup} false pairs, ${compared.n} comparisons instead of ${((pages.length * (pages.length - 1)) / 2).toLocaleString()}`
] = recall > 0.9 && falseDup <= 5;
results[`TF-IDF cosine separates topics (${mean(sameTopic).toFixed(3)} vs ${mean(crossTopic).toFixed(3)})`] =
    mean(sameTopic) > mean(crossTopic) * 2;

print("similarity-detection");
print(
    `  code       ${subs.length} submissions, k=${K}, w=${W}; ${pairShared.size} candidate pairs from the fingerprint index, ${codePairs.length} above 0.5`,
);
print(
    table([
        ["pair", "shared fp", "similarity", "aligned lines A", "aligned lines B", "tokens"],
        ...codePairs
            .slice(0, 8)
            .map((p) => [
                `${p.a} ~ ${p.b}`,
                String(p.shared),
                p.sim.toFixed(2),
                `${p.align.a[0]}-${p.align.a[1]}`,
                `${p.align.b[0]}-${p.align.b[1]}`,
                String(p.align.tokens),
            ]),
    ]),
);
print(`  rings      ${codeClusters.map((c) => c.join("+")).join("  ")}`);
const [c0] = pageClusters,
    rep = pages[c0[0]],
    other = pages[c0[1]];
const d = DiffWords(rep.text, other.text)
    .filter((h) => h.op !== 0)
    .map((h) => `${h.op > 0 ? "+" : "-"}${h.text.trim()}`)
    .join(" ");
print(
    `  pages      ${pages.length} pages -> ${pageClusters.length} duplicate groups; e.g. #${rep.id} vs #${other.id}: ${d.slice(0, 90)}`,
);
const REPORT = new Template(`<h1>Similarity report</h1><ul>{{#pairs}}<li>{{a}} ~ {{b}}: {{sim}}</li>{{/pairs}}</ul>`);
print(
    `  report     ${REPORT.render({ pairs: codePairs.slice(0, 3).map((p) => ({ ...p, sim: p.sim.toFixed(2) })) }).length} bytes of HTML`,
);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
