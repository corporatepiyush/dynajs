// Search platform — a full-text search engine: analysis chain, positional inverted index in compressed segments,
// a boolean/phrase/field query language, BM25F ranking, typo tolerance, facets, highlighting, segment merging,
// persistence, an HTTP API and relevance evaluation against judged queries.
//
// WHAT IT IS
//   The core of Lucene/Elasticsearch, small enough to read, over a catalogue of 3,000 product documents:
//
//   analysis     Unicode folding (é -> e), lowercase, a stop list, and a light suffix stemmer, applied
//                identically at index and query time; positions are kept for phrase queries
//   index        documents are buffered and flushed into immutable SEGMENTS: a term dictionary (sorted, with a
//                Trie for prefix lookups) and postings lists stored as delta-encoded varints; deletes are a
//                tombstone bitset; a tiered merge policy combines small segments in the background
//   queries      a parser for `title:"trail running" (waterproof OR gore-tex) -kids brand:salomon shoe*`:
//                fields, phrases, OR, negation, prefix, and fuzzy terms (`jaket~1`)
//   ranking      BM25F: per-field term frequencies with field boosts (title 3, brand 2, body 1) and per-field
//                length normalisation, plus a small popularity prior
//   typos        a query term that matches nothing is corrected through the term dictionary by Damerau–
//                Levenshtein distance ("did you mean")
//   facets       counts per brand and price band over the full match set, filters applied from the request
//   highlight    the best snippet window around matched terms with <em> markup, escaped
//   persistence  segments are written as CBOR + zstd files with a manifest; a fresh process reopens them
//   evaluation   judged queries with graded relevance give nDCG@10 and MRR for BM25F vs plain TF-IDF
//   autocomplete GET /suggest?prefix= ranks title terms from every segment's Trie by live document count
//   caching      result pages sit in an LRU keyed by the index GENERATION, which every flush, delete and merge
//                bumps, so DELETE /docs/:id invalidates without tracking which pages held the document
//   oracles      BM25F recomputed from the stored text alone (no postings, no dictionaries) must equal the
//                index's ranking and scores; autocomplete must equal a scan of every live title
//
//   lookups      prefix terms come from each segment's Trie (keysWithPrefix), typo candidates from a deletion index
//                (SymSpell: a Multimap of every term's one- and two-character deletions) so a fuzzy term scores a
//                handful of candidates instead of the whole vocabulary; the top-k is chosen by dyna:simd
//                topkIndices over a Float32Array of scores, and facets are Multisets; per query, document
//                frequencies and clause expansions are computed once (not once per segment), negated terms
//                become a BitSet per segment, and the snippet window is found with two cursors
//
// MODULES  dyna:encoding (varints) · dyna:structures (Trie, BitSet, Multimap, Multiset, LRU) · dyna:matcher (DamerauLevenshtein)
//          · dyna:simd (topkIndices)
//          · dyna:serialize (CBOR) · dyna:compress (zstd) · dyna:http (App) · dyna:html (escape via Template)
//          · dyna:file · dyna:random · dyna:cli (Table) · dyna:bytes (concat)
//
// RUN      dynajs examples/text/search-platform.js     indexes a generated catalogue, serves queries, evaluates relevance

import { PutUvarint, Uvarint } from "dyna:encoding";
import { concat } from "dyna:bytes";
import { Trie, BitSet, Multimap, Multiset, LRU } from "dyna:structures";
import { topkIndices } from "dyna:simd";
import { DamerauLevenshtein } from "dyna:matcher";
import { CBOREncode, CBORDecode } from "dyna:serialize";
import { zstd, unzstd } from "dyna:compress";
import { App } from "dyna:http";
import { Template } from "dyna:html";
import { makeTempDir, writeFile, readFile, readBytes, readDir, remove, removeAll } from "dyna:file";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("search");
const rng = new Random(1234);

// =============================================================================
// Analysis
// =============================================================================
const STOP = new Set(
    "a an and are as at be by for from has in is it its of on or that the to was were will with this these your our".split(
        " ",
    ),
);
function stem(w) {
    if (w.length <= 3) return w;
    for (const [suf, rep] of [
        ["ies", "y"],
        ["sses", "ss"],
        ["ing", ""],
        ["edly", ""],
        ["ed", ""],
        ["ers", "er"],
        ["ness", ""],
        ["ly", ""],
        ["es", "e"],
        ["s", ""],
    ])
        if (w.endsWith(suf) && w.length - suf.length >= 3) return w.slice(0, -suf.length) + rep;
    return w;
}
function analyze(text) {
    const out = [];
    let pos = 0;
    for (const m of text
        .normalize("NFD")
        .replace(/\p{M}/gu, "")
        .toLowerCase()
        .matchAll(/[\p{L}\p{N}]+(?:-[\p{L}\p{N}]+)*/gu)) {
        for (const part of m[0].split("-").length > 1 ? [m[0].replace(/-/g, ""), ...m[0].split("-")] : [m[0]]) {
            if (STOP.has(part)) continue;
            out.push({ term: stem(part), pos, start: m.index, end: m.index + m[0].length });
        }
        pos++;
    }
    return out;
}

// =============================================================================
// Segments
// =============================================================================
const FIELDS = { title: 3, brand: 2, body: 1 };
const encodePostings = (list) => {
    // [doc, freq, pos...] with doc and pos deltas
    const parts = [PutUvarint(list.length)];
    let prevDoc = 0;
    for (const [doc, positions] of list) {
        parts.push(PutUvarint(doc - prevDoc), PutUvarint(positions.length));
        let prev = 0;
        for (const p of positions) {
            parts.push(PutUvarint(p - prev));
            prev = p;
        }
        prevDoc = doc;
    }
    return concat(parts);
};
function decodePostings(buf) {
    let off = 0;
    const next = () => {
        const [v, n] = Uvarint(buf.subarray(off));
        off += n;
        return Number(v);
    };
    const n = next(),
        out = [];
    let doc = 0;
    for (let i = 0; i < n; i++) {
        doc += next();
        const f = next(),
            pos = [];
        let p = 0;
        for (let k = 0; k < f; k++) {
            p += next();
            pos.push(p);
        }
        out.push([doc, pos]);
    }
    return out;
}
class Segment {
    static build(name, docs) {
        // docs: [{ id, fields, stored }]
        const terms = new Map(),
            lengths = {},
            stored = [];
        for (const f of Object.keys(FIELDS)) lengths[f] = [];
        docs.forEach((d, local) => {
            stored.push(d.stored);
            for (const f of Object.keys(FIELDS)) {
                const toks = analyze(d.fields[f] || "");
                lengths[f].push(toks.length);
                for (const t of toks) {
                    const key = `${f}:${t.term}`;
                    if (!terms.has(key)) terms.set(key, new Map());
                    const pl = terms.get(key);
                    if (!pl.has(local)) pl.set(local, []);
                    pl.get(local).push(t.pos);
                }
            }
        });
        const dict = {};
        for (const [k, pl] of [...terms].sort((a, b) => (a[0] < b[0] ? -1 : 1))) dict[k] = encodePostings([...pl]);
        return new Segment(name, { dict, lengths, stored, deleted: [] });
    }
    constructor(name, data) {
        this.name = name;
        this.data = data;
        this.n = data.stored.length;
        this.deleted = new BitSet(this.n);
        for (const d of data.deleted) this.deleted.set(d);
        this.trie = new Trie();
        for (const k of Object.keys(data.dict)) this.trie.insert(k);
        this.avg = Object.fromEntries(
            Object.keys(FIELDS).map((f) => [f, data.lengths[f].reduce((a, b) => a + b, 0) / Math.max(1, this.n)]),
        );
        this.cache = new Map();
    }
    postings(key) {
        if (!this.cache.has(key)) this.cache.set(key, this.data.dict[key] ? decodePostings(this.data.dict[key]) : []);
        return this.cache.get(key);
    }
    live(local) {
        return !this.deleted.get(local);
    }
    save(dir) {
        this.data.deleted = this.deleted.toArray();
        writeFile(dir.join(`${this.name}.seg`), zstd(CBOREncode(this.data), { level: 5 }));
    }
    static load(dir, name) {
        return new Segment(name, CBORDecode(unzstd(readBytes(dir.join(`${name}.seg`)))));
    }
}

// =============================================================================
// Index: buffer, flush, delete, merge, persist
// =============================================================================
class Index {
    constructor(dir) {
        this.dir = dir;
        this.segments = [];
        this.buffer = [];
        this.seq = 0;
        this.merges = 0;
        this.ids = new Set();
        this.gen = 0; // bumped by every change a reader can observe
    }
    add(doc) {
        if (this.ids.has(doc.stored.id)) this.delete(doc.stored.id);
        this.ids.add(doc.stored.id);
        this.buffer.push(doc);
        if (this.buffer.length >= 250) this.flush();
    }
    delete(id) {
        for (const s of this.segments)
            s.data.stored.forEach((d, i) => {
                if (d.id === id) s.deleted.set(i);
            });
        this.buffer = this.buffer.filter((d) => d.stored.id !== id);
        this.ids.delete(id);
        this.gen++;
    }
    flush() {
        if (!this.buffer.length) return;
        this.segments.push(Segment.build(`seg${String(this.seq++).padStart(4, "0")}`, this.buffer));
        this.buffer = [];
        this.gen++;
        this.maybeMerge();
    }
    maybeMerge() {
        // tiered: merge 4 segments of similar size
        for (;;) {
            const sorted = [...this.segments].sort((a, b) => a.n - b.n);
            const tier = sorted.filter((s) => s.n <= sorted[0].n * 3).slice(0, 4);
            if (tier.length < 4) return;
            const docs = tier.flatMap((s) =>
                s.data.stored
                    .filter((st, i) => s.live(i))
                    .map((st) => ({ fields: { title: st.title, brand: st.brand, body: st.body }, stored: st })),
            );
            this.segments = this.segments.filter((s) => !tier.includes(s));
            this.segments.push(Segment.build(`seg${String(this.seq++).padStart(4, "0")}`, docs));
            this.merges++;
            this.gen++;
        }
    }
    commit() {
        this.flush();
        for (const f of readDir(this.dir))
            if (f.name.endsWith(".seg") && !this.segments.some((s) => `${s.name}.seg` === f.name))
                remove(this.dir.join(f.name));
        for (const s of this.segments) s.save(this.dir);
        writeFile(
            this.dir.join("manifest.json"),
            JSON.stringify({ segments: this.segments.map((s) => s.name), seq: this.seq }),
        );
    }
    static open(dir) {
        const m = JSON.parse(readFile(dir.join("manifest.json")));
        const ix = new Index(dir);
        ix.seq = m.seq;
        ix.segments = m.segments.map((n) => Segment.load(dir, n));
        return ix;
    }
    get docCount() {
        return this.segments.values().reduce((s, seg) => s + seg.n - seg.deleted.count, 0);
    }
    vocabulary() {
        if (this.vocab?.gen !== this.gen)
            this.vocab = {
                gen: this.gen,
                terms: new Set(
                    this.segments.values().flatMap((s) =>
                        Object.keys(s.data.dict)
                            .values()
                            .map((k) => k.slice(k.indexOf(":") + 1)),
                    ),
                ),
            };
        return this.vocab.terms;
    }
    docFreq(fields, term) {
        // documents (live or tombstoned, as BM25 conventionally counts) holding the term in any of the fields
        return this.segments
            .values()
            .map(
                (sg) =>
                    new Set(
                        fields.values().flatMap((f) =>
                            sg
                                .postings(`${f}:${term}`)
                                .values()
                                .map(([d]) => d),
                        ),
                    ).size,
            )
            .reduce((a, b) => a + b, 0);
    }
}

// =============================================================================
// Query language
// =============================================================================
function parseQuery(q) {
    const clauses = [];
    const re = /(-)?(?:(\w+):)?(?:"([^"]+)"|\(([^)]+)\)|([^\s()"]+))/g;
    for (const m of q.matchAll(re)) {
        const neg = !!m[1],
            field = m[2] && FIELDS[m[2]] ? m[2] : null;
        if (m[3]) clauses.push({ type: "phrase", neg, field, terms: analyze(m[3]).map((t) => t.term) });
        else if (m[4])
            clauses.push({
                type: "or",
                neg,
                field,
                options: m[4]
                    .split(/\s+OR\s+/i)
                    .map((s) => analyze(s)[0]?.term)
                    .filter(Boolean),
            });
        else {
            let raw = m[5],
                fuzzy = 0,
                prefix = false;
            const fz = /~(\d)$/.exec(raw);
            if (fz) {
                fuzzy = Number(fz[1]);
                raw = raw.slice(0, -2);
            }
            if (raw.endsWith("*")) {
                prefix = true;
                raw = raw.slice(0, -1);
            }
            if (m[2] && !FIELDS[m[2]]) {
                clauses.push({ type: "filter", neg, key: m[2], value: raw.toLowerCase() });
                continue;
            }
            const t = prefix ? raw.toLowerCase() : analyze(raw)[0]?.term;
            if (t) clauses.push({ type: prefix ? "prefix" : fuzzy ? "fuzzy" : "term", neg, field, term: t, fuzzy });
        }
    }
    return clauses;
}

// =============================================================================
// Search: candidate generation, BM25F scoring, facets, highlighting
// =============================================================================
const K1 = 1.2,
    B = 0.75;
const deletions = (w, depth) => {
    const out = new Set([w]);
    let frontier = [w];
    for (let d = 0; d < depth; d++) {
        const next = [];
        for (const x of frontier)
            for (let i = 0; i < x.length; i++) {
                const y = x.slice(0, i) + x.slice(i + 1);
                if (!out.has(y)) {
                    out.add(y);
                    next.push(y);
                }
            }
        frontier = next;
    }
    return out;
};
const fuzzyIndex = new WeakMap(); // Index -> { size, deletes: Multimap }
function candidates(ix, term, maxEdits) {
    // vocabulary terms within maxEdits (<= 2)
    let f = fuzzyIndex.get(ix);
    const vocab = ix.vocabulary();
    if (!f || f.size !== vocab.size) {
        f = { size: vocab.size, deletes: new Multimap() };
        for (const v of vocab) for (const d of deletions(v, 2)) f.deletes.put(d, v);
        fuzzyIndex.set(ix, f);
    }
    const out = new Set();
    for (const d of deletions(term, maxEdits)) for (const v of f.deletes.get(d)) out.add(v);
    return [...out].filter((v) => DamerauLevenshtein(v, term, { max: maxEdits }) <= maxEdits);
}
function expandTerms(ix, c) {
    if (c.type === "term") return [c.term];
    if (c.type === "or") return c.options;
    if (c.type === "phrase") return c.terms;
    if (c.type === "prefix")
        return [
            ...new Set(
                ix.segments.flatMap((s) =>
                    Object.keys(FIELDS).flatMap((f) =>
                        s.trie.keysWithPrefix(`${f}:${c.term}`).map((k) => k.slice(f.length + 1)),
                    ),
                ),
            ),
        ]
            .sort()
            .slice(0, 50);
    return candidates(ix, c.term, c.fuzzy);
}
function search(ix, q, { size = 10, filters = {}, scorer = "bm25f" } = {}) {
    const clauses = parseQuery(q),
        N = ix.docCount;
    const results = [],
        suggestions = [];
    for (const c of clauses)
        if (
            c.type === "term" &&
            !c.neg &&
            !ix.segments.some((s) => Object.keys(FIELDS).some((f) => s.data.dict[`${f}:${c.term}`]))
        ) {
            const best = candidates(ix, c.term, 2)
                .map((v) => [v, DamerauLevenshtein(v, c.term, { max: 2 })])
                .sortWith([(a, b) => a[1] - b[1], (a, b) => (a[0] < b[0] ? -1 : 1)])[0];
            if (best) {
                suggestions.push([c.term, best[0]]);
                c.term = best[0];
            }
        }
    const expansions = new Map(clauses.map((c) => [c, c.type === "filter" ? [] : expandTerms(ix, c)]));
    const required = clauses.filter((c) => !c.neg && c.type !== "filter"),
        negTerms = clauses.filter((c) => c.neg && c.type !== "filter").flatMap((c) => expansions.get(c)),
        negFilters = clauses.filter((c) => c.neg && c.type === "filter"),
        posFilters = clauses.filter((c) => c.type === "filter" && !c.neg);
    const dfs = new Map(
        required.flatMap((c) => {
            const fields = c.field ? [c.field] : Object.keys(FIELDS);
            return expansions.get(c).map((term) => [`${fields}|${term}`, ix.docFreq(fields, term)]);
        }),
    );
    for (const seg of ix.segments) {
        const scores = new Map(),
            matchedAll = new Map(),
            excludedDocs = new BitSet(seg.n);
        for (const t of negTerms)
            for (const f of Object.keys(FIELDS)) for (const [x] of seg.postings(`${f}:${t}`)) excludedDocs.set(x);
        required.forEach((c, ci) => {
            const fields = c.field ? [c.field] : Object.keys(FIELDS);
            const hit = new Set();
            for (const term of expansions.get(c)) {
                const df = dfs.get(`${fields}|${term}`);
                const idf = Math.log(1 + (N - df + 0.5) / (df + 0.5));
                const perDoc = new Map();
                for (const f of fields)
                    for (const [d, pos] of seg.postings(`${f}:${term}`)) {
                        if (!seg.live(d)) continue;
                        const len = seg.data.lengths[f][d];
                        const w =
                            scorer === "bm25f"
                                ? (FIELDS[f] * pos.length) / (1 - B + (B * len) / seg.avg[f])
                                : pos.length;
                        perDoc.set(d, (perDoc.get(d) || 0) + w);
                    }
                for (const [d, tf] of perDoc) {
                    if (c.type === "phrase") continue;
                    const s =
                        scorer === "bm25f"
                            ? (idf * tf * (K1 + 1)) / (tf + K1)
                            : (1 + Math.log(tf)) * Math.log(N / Math.max(1, df));
                    scores.set(d, (scores.get(d) || 0) + s);
                    hit.add(d);
                }
            }
            if (c.type === "phrase") {
                // positional check across consecutive terms
                for (const f of fields) {
                    const lists = c.terms.map((t) => new Map(seg.postings(`${f}:${t}`)));
                    for (const [d, pos0] of lists[0] || []) {
                        if (!seg.live(d) || !lists.every((l) => l.has(d))) continue;
                        if (pos0.some((p) => lists.every((l, k) => l.get(d).includes(p + k)))) {
                            scores.set(d, (scores.get(d) || 0) + 3 * FIELDS[f] * c.terms.length);
                            hit.add(d);
                        }
                    }
                }
            }
            for (const d of hit) matchedAll.set(d, (matchedAll.get(d) || 0) | (1 << ci));
        });
        const full = (1 << required.length) - 1;
        for (const [d, mask] of matchedAll) {
            if (mask !== full) continue;
            const doc = seg.data.stored[d];
            const excluded =
                excludedDocs.get(d) || negFilters.some((c) => String(doc[c.key]).toLowerCase() === c.value);
            const filtered = posFilters.every((c) => String(doc[c.key]).toLowerCase() === c.value);
            if (excluded || !filtered) continue;
            results.push({ doc, score: scores.get(d) + (scorer === "bm25f" ? 0.1 * Math.log1p(doc.popularity) : 0) });
        }
    }
    const band = (p) => (p < 50 ? "<50" : p < 150 ? "50-150" : "150+");
    const brands = new Multiset(),
        prices = new Multiset();
    for (const r of results) {
        brands.add(r.doc.brand);
        prices.add(band(r.doc.price));
    }
    const facets = { brand: Object.fromEntries(brands.entrySet()), price: Object.fromEntries(prices.entrySet()) };
    const visible = results.filter(
        (r) =>
            (!filters.brand || r.doc.brand === filters.brand) &&
            (!filters.price || band(r.doc.price) === filters.price),
    );
    const scores = Float32Array.from(visible, (r) => r.score);
    const top = (visible.length > size ? [...topkIndices(scores, size)].map((i) => visible[i]) : visible).sortWith([
        (a, b) => b.score - a.score,
        (a, b) => a.doc.id - b.doc.id,
    ]);
    const qterms = new Set(clauses.filter((c) => !c.neg).flatMap((c) => expansions.get(c)));
    return {
        total: visible.length,
        hits: top.map((r) => ({
            id: r.doc.id,
            title: r.doc.title,
            brand: r.doc.brand,
            price: r.doc.price,
            score: Number(r.score.toFixed(3)),
            snippet: highlight(r.doc.body, qterms),
        })),
        facets,
        suggestions,
    };
}
function suggest(ix, prefix, k = 5) {
    // title terms starting with the prefix, ranked by how many live documents carry them
    const df = new Multiset();
    const key = `title:${analyze(prefix)[0]?.term ?? prefix.toLowerCase()}`;
    for (const seg of ix.segments)
        for (const t of seg.trie.keysWithPrefix(key)) {
            const live = seg
                .postings(t)
                .values()
                .filter(([d]) => seg.live(d))
                .count();
            if (live) df.add(t.slice(6), live);
        }
    return df
        .entrySet()
        .sortWith([(a, b) => b[1] - a[1], (a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)])
        .slice(0, k);
}
const esc = (s) => new Template("{{s}}").render({ s });
function highlight(text, terms) {
    const toks = analyze(text),
        hits = toks.filter((t) => terms.has(t.term));
    if (!hits.length) return esc(text.slice(0, 120));
    // the 120-character window holding the most matches: hits are in text order, so two cursors suffice
    let best = hits[0],
        bestCount = 0;
    for (let i = 0, j = 0; i < hits.length; i++) {
        while (j < hits.length && hits[j].start < hits[i].start + 120) j++;
        if (j - i > bestCount) [best, bestCount] = [hits[i], j - i];
    }
    const from = Math.max(0, best.start - 30),
        to = Math.min(text.length, from + 150);
    let out = "",
        at = from;
    for (const h of hits.filter((x) => x.start >= from && x.end <= to)) {
        out += esc(text.slice(at, h.start)) + "<em>" + esc(text.slice(h.start, h.end)) + "</em>";
        at = h.end;
    }
    return (from ? "…" : "") + out + esc(text.slice(at, to)) + (to < text.length ? "…" : "");
}

// =============================================================================
// Corpus
// =============================================================================
const BRANDS = ["Salomon", "Patagonia", "Arc'teryx", "Hoka", "Mammut", "Deuter", "Petzl", "Black Diamond"];
const KINDS = [
    ["trail running shoe", ["grip", "lightweight", "cushioned", "trail", "running"]],
    ["rain jacket", ["waterproof", "gore-tex", "breathable", "hooded"]],
    ["hiking backpack", ["liters", "hip belt", "rain cover", "frame"]],
    ["headlamp", ["lumens", "rechargeable", "beam", "battery"]],
    ["climbing harness", ["belay loop", "gear loops", "adjustable", "lightweight"]],
    ["insulated jacket", ["down", "warm", "packable", "hooded"]],
];
const docs = [];
for (let i = 0; i < 3000; i++) {
    const [kind, words] = KINDS[i % KINDS.length],
        brand = BRANDS[rng.nextBounded(BRANDS.length)],
        kids = rng.nextFloat() < 0.1;
    const adj = Array.from({ length: 3 }, () => words[rng.nextBounded(words.length)]);
    const title = `${brand} ${kids ? "Kids " : ""}${["Pro", "Ultra", "Trail", "Summit", "Alpine"][rng.nextBounded(5)]} ${kind}`;
    const body = `The ${title} is ${adj.join(", ")} and built for ${["mountain days", "long distances", "wet weather", "alpine starts"][rng.nextBounded(4)]}. Customers love the ${adj[0]} design. ${rng.nextFloat() < 0.3 ? "Fully waterproof with taped seams." : "Recycled materials."}`;
    docs.push({
        fields: { title, brand, body },
        stored: {
            id: `P${i}`,
            title,
            brand,
            body,
            price: 20 + rng.nextBounded(300),
            popularity: rng.nextBounded(1000),
            kind,
            kids,
        },
    });
}

// =============================================================================
// Build, update, persist, reopen, serve
// =============================================================================
const ix = new Index(work);
let t = performance.now();
for (const d of docs) ix.add(d);
ix.commit();
const indexMs = performance.now() - t;
ix.add({ ...docs[5], stored: { ...docs[5].stored, price: 1 } }); // an update replaces the doc
ix.flush();
ix.delete("P7");
ix.commit();
const reopened = Index.open(work);
const liveAfterReopen = reopened.docCount;
const bytes = readDir(work)
    .filter((f) => f.name.endsWith(".seg"))
    .reduce((s, f) => s + readBytes(work.join(f.name)).length, 0);

const resultCache = new LRU(64);
const app = new App({ port: 0 });
app.get("/search", (req) => {
    // a cached page is valid for exactly one index generation: any update, delete or merge changes the key
    const key = JSON.stringify([reopened.gen, req.query.q, req.query.size, req.query.brand, req.query.price]);
    let page = resultCache.get(key);
    if (!page)
        resultCache.set(
            key,
            (page = search(reopened, req.query.q || "", {
                size: Number(req.query.size) || 10,
                filters: { brand: req.query.brand, price: req.query.price },
            })),
        );
    return page;
});
app.get("/suggest", (req) => ({ terms: suggest(reopened, req.query.prefix || "", Number(req.query.k) || 5) }));
app.del("/docs/:id", (req) => {
    if (
        !reopened.ids.has(req.params.id) &&
        !reopened.segments.some((s) => s.data.stored.some((d) => d.id === req.params.id))
    )
        return { status: 404, body: "no such document" };
    reopened.delete(req.params.id);
    reopened.commit();
    return { status: 204, body: "" };
});
app.start();
const get = async (q, extra = "") =>
    (await fetch(`http://127.0.0.1:${app.port}/search?q=${encodeURIComponent(q)}${extra}`)).json();
const r1 = await get('title:"rain jacket" waterproof -kids');
const r2 = await get("jaket~1 hooded");
const r3 = await get("headlamp", "&brand=Petzl");
const r4 = await get("hedlamp rechargeable");
const r5 = await get("head*");
const r6 = await get('"trail running shoe" (grip OR cushioned) brand:salomon');
const base = `http://127.0.0.1:${app.port}`;
// the cache: a repeat is a hit; deleting the top hit changes the generation, so the same query is recomputed without it
const before = await get("headlamp", "&brand=Petzl"),
    statsAfterRepeat = { ...resultCache.stats };
const victim = before.hits[0].id;
const delStatus = (await fetch(`${base}/docs/${victim}`, { method: "DELETE" })).status;
const missingStatus = (await fetch(`${base}/docs/NOPE`, { method: "DELETE" })).status;
const after = await get("headlamp", "&brand=Petzl");
const prefixes = ["h", "s", "p", "ra", "al"];
const suggestions = {};
for (const pre of prefixes) suggestions[pre] = (await (await fetch(`${base}/suggest?prefix=${pre}&k=4`)).json()).terms;
app.close();

// =============================================================================
// Relevance evaluation: graded judgements derived from the generator's ground truth
// =============================================================================
const JUDGED = [
    [
        "waterproof rain jacket",
        (d) =>
            d.kind === "rain jacket" ? (d.body.includes("waterproof") ? 3 : 2) : d.body.includes("waterproof") ? 1 : 0,
    ],
    [
        "lightweight climbing harness",
        (d) => (d.kind === "climbing harness" ? (d.body.includes("lightweight") ? 3 : 2) : 0),
    ],
    [
        "warm packable jacket",
        (d) =>
            d.kind === "insulated jacket" ? (d.body.includes("packable") ? 3 : 2) : d.kind === "rain jacket" ? 1 : 0,
    ],
    ["rechargeable headlamp", (d) => (d.kind === "headlamp" ? (d.body.includes("rechargeable") ? 3 : 1) : 0)],
];
const byId = new Map(reopened.segments.values().flatMap((s) => s.data.stored.values().map((d) => [d.id, d])));
function evaluate(scorer) {
    let ndcg = 0,
        mrr = 0;
    for (const [q, grade] of JUDGED) {
        const hits = search(reopened, q, { size: 10, scorer }).hits.map((h) => byId.get(h.id));
        const gains = hits.map(grade),
            ideal = docs
                .map((d) => grade(d.stored))
                .sort((a, b) => b - a)
                .slice(0, 10);
        const dcg = (g) => g.reduce((s, x, i) => s + (2 ** x - 1) / Math.log2(i + 2), 0);
        ndcg += dcg(gains) / dcg(ideal);
        const first = gains.findIndex((g) => g >= 2);
        mrr += first < 0 ? 0 : 1 / (first + 1);
    }
    return { ndcg: ndcg / JUDGED.length, mrr: mrr / JUDGED.length };
}
const evalBm = evaluate("bm25f"),
    evalTf = evaluate("tfidf");

const results = {};
results["analysis folds, stops and stems"] =
    analyze("The Running shoes, waterproofed jackets")
        .map((t) => t.term)
        .join(" ") === "runn shoe waterproof jacket" && analyze("Café")[0].term === "cafe";
results["postings round-trip through delta varints"] =
    decodePostings(
        encodePostings([
            [3, [1, 5]],
            [9, [0]],
        ]),
    )
        .flat(2)
        .join() === "3,1,5,9,0";
results[`reopened index holds ${liveAfterReopen} live docs after ${ix.merges} merges`] =
    liveAfterReopen === 2999 && ix.merges > 0;
results["phrase + field + negation"] =
    r1.hits.length && r1.hits.every((h) => /rain jacket/i.test(h.title) && !/Kids/.test(h.title));
results["snippets highlight matches and escape everything else"] =
    r1.hits.every((h) => h.snippet.includes("<em>")) && !r1.hits.some((h) => /<(?!\/?em>)/.test(h.snippet));
results["fuzzy term jaket~1 finds jackets"] = r2.total > 0 && r2.hits.every((h) => /jacket/i.test(h.title));
results["a brand filter narrows hits but facets count the whole match set"] =
    r3.hits.every((h) => h.brand === "Petzl") && Object.keys(r3.facets.brand).length > 3;
results["a misspelled term is corrected from the dictionary"] =
    r4.suggestions.some(([a, b]) => a === "hedlamp" && b === "headlamp") && r4.total > 0;
results["prefix query head*"] = r5.hits.every((h) => /head|hood/i.test(h.title + h.snippet));
results["phrase + OR group + keyword filter combined"] = r6.hits.every(
    (h) => h.brand === "Salomon" && /trail running shoe/i.test(h.title),
);
results["deletes and updates are visible after reopening"] =
    !search(reopened, "jacket", { size: 3000 }).hits.some((h) => h.id === "P7") &&
    search(reopened, "insulated", { size: 3000 })
        .hits.filter((h) => h.id === "P5")
        .map((h) => h.price)
        .join() === "1";
results[`BM25F nDCG@10 ${evalBm.ndcg.toFixed(3)} (TF-IDF ${evalTf.ndcg.toFixed(3)}) on the judged queries`] =
    evalBm.ndcg >= evalTf.ndcg && evalBm.ndcg > 0.7;

{
    // autocomplete against a scan of every live document's analysed title
    const titles = reopened.segments.flatMap((s) =>
        s.data.stored.filter((_, i) => s.live(i)).map((d) => new Set(analyze(d.title).map((t) => t.term))),
    );
    let same = true;
    for (const pre of prefixes) {
        const counts = new Map(),
            stemPre = analyze(pre)[0]?.term ?? pre;
        for (const terms of titles)
            for (const term of terms) if (term.startsWith(stemPre)) counts.set(term, (counts.get(term) || 0) + 1);
        const want = [...counts]
            .sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : 1))
            .slice(0, 4)
            .map(([t, n]) => `${t}=${n}`)
            .join();
        if (want !== suggestions[pre].map(([t, n]) => `${t}=${n}`).join()) same = false;
    }
    results[
        `autocomplete ranks title terms by live document count (${suggestions.h.map(([t, n]) => `${t} ${n}`).join(", ")}), equal to a full scan`
    ] = same && prefixes.every((pre) => suggestions[pre].length > 0);
}
results[`a repeated query is served from the LRU; deleting its top hit (${victim}) invalidates it`] =
    statsAfterRepeat.hits >= 1 &&
    delStatus === 204 &&
    missingStatus === 404 &&
    !after.hits.some((h) => h.id === victim) &&
    after.total === before.total - 1;
{
    // BM25F from the stored text alone: re-analyse every document, no postings, no segment dictionaries
    const q = "waterproof hooded jacket",
        terms = analyze(q).map((t) => t.term);
    const N = reopened.docCount,
        rows = [];
    const perSeg = reopened.segments.map((s) =>
        s.data.stored.map((d) => {
            const tf = {},
                len = {};
            for (const f of Object.keys(FIELDS)) {
                const toks = analyze(d[f]);
                len[f] = toks.length;
                tf[f] = {};
                for (const t of toks) tf[f][t.term] = (tf[f][t.term] || 0) + 1;
            }
            return { d, tf, len };
        }),
    );
    const df = (term) => {
        let n = 0;
        for (const seg of perSeg) for (const x of seg) if (Object.keys(FIELDS).some((f) => x.tf[f][term])) n++;
        return n;
    };
    const idf = Object.fromEntries(terms.map((t) => [t, Math.log(1 + (N - df(t) + 0.5) / (df(t) + 0.5))]));
    perSeg.forEach((seg, si) => {
        const avg = {};
        for (const f of Object.keys(FIELDS)) avg[f] = seg.reduce((a, x) => a + x.len[f], 0) / Math.max(1, seg.length);
        seg.forEach((x, i) => {
            if (!reopened.segments[si].live(i)) return;
            let score = 0;
            for (const t of terms) {
                let w = 0;
                for (const f of Object.keys(FIELDS))
                    if (x.tf[f][t]) w += (FIELDS[f] * x.tf[f][t]) / (1 - B + (B * x.len[f]) / avg[f]);
                if (!w) return;
                score += (idf[t] * w * (K1 + 1)) / (w + K1);
            }
            rows.push({ id: x.d.id, score: score + 0.1 * Math.log1p(x.d.popularity) });
        });
    });
    rows.sort((a, b) => b.score - a.score || (a.id < b.id ? -1 : 1));
    const got = search(reopened, q, { size: 10 });
    results[
        `BM25F from the index equals a brute-force BM25F over the stored text (${rows.length} matches, top ${rows[0]?.id})`
    ] =
        got.total === rows.length &&
        got.hits.every((h, i) => h.id === rows[i].id && Math.abs(h.score - rows[i].score) < 1e-3);
}

print("search-platform");
print(
    `  index      ${docs.length} docs in ${indexMs.toFixed(0)} ms -> ${reopened.segments.length} segments (${(bytes / 1024).toFixed(0)} KiB on disk), ${reopened.vocabulary().size} terms`,
);
print(`  query      ${'title:"rain jacket" waterproof -kids'} -> ${r1.total} hits; top: ${r1.hits[0]?.title}`);
print(`  snippet    ${r1.hits[0]?.snippet}`);
print(
    `  did-you-mean ${r4.suggestions.map(([a, b]) => `${a} -> ${b}`).join(", ")}; facets ${JSON.stringify(r3.facets.price)}`,
);
print(
    table([
        ["ranking", "nDCG@10", "MRR"],
        ["BM25F", evalBm.ndcg.toFixed(3), evalBm.mrr.toFixed(3)],
        ["TF-IDF", evalTf.ndcg.toFixed(3), evalTf.mrr.toFixed(3)],
    ]),
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
