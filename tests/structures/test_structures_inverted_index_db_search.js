// covers: dyna:structures.InvertedIndex
// Differential tests for KDTree, ConsistentHash and InvertedIndex.
//   KDTree: knn / radius / box against brute force over random, clustered and
//     duplicate-heavy point sets in 1..8 dims, with ties broken by index.
//   ConsistentHash: the published properties of the ring (Karger et al. 1997):
//     balance, monotonicity (adding a node only moves keys TO it), and the
//     placement being a pure function of the node set (insertion order and a
//     round trip through another process change nothing). Jump hash (Lamping
//     & Veach 2014): a key moves only to the newly added bucket.
//   InvertedIndex: BM25 scores against a brute-force JS scorer that uses the
//     documented formula (Robertson & Zaragoza 2009, idf = ln(1 + (N-df+.5)/(df+.5))).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_inverted_index_db_search");
import { KDTree, ConsistentHash, InvertedIndex } from "dyna:structures";
import { Exec } from "dyna:sys";
import { XXHash64 } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; if (fails < 40) print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }
function same(a, b, m) { const x = String(JSON.stringify(a)), y = String(JSON.stringify(b)); check(x === y, m + "\n   got  " + x.slice(0, 300) + "\n   want " + y.slice(0, 300)); }
function rng(seed) { let s = seed >>> 0 || 1; return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; }; }

// ---- KDTree -----------------------------------------------------------------
function d2(a, b) { let s = 0; for (let i = 0; i < a.length; i++) { const x = a[i] - b[i]; s += x * x; } return s; }

__kitS.section("Hello, World! foo_bar 42x", () => {
    same(InvertedIndex.tokenize("Hello, World! foo_bar 42x"), ["hello", "world", "foo_bar", "42x"], "tokenize ascii");
    same(InvertedIndex.tokenize("Ünïcode café—naïve x、y"), ["Ünïcode", "café", "naïve", "x", "y"], "tokenize: non-ASCII letters stay, punctuation splits, case folds ASCII only");
    same(InvertedIndex.tokenize(""), [], "tokenize empty");
    const long = "a".repeat(200) + " b";
    same(InvertedIndex.tokenize(long).map(t => t.length), [128, 1], "tokenize caps a token at 128 bytes");
    const ml = "é".repeat(100);
    const tk = InvertedIndex.tokenize(ml)[0];
    eq(tk, "é".repeat(64), "tokenize cuts at a UTF-8 boundary");
});

function bm25Ref(docs, query, opts) {
    const tok = (s) => InvertedIndex.tokenize(s).filter(t => !opts.stop.has(t));
    const D = new Map(); for (const [id, text] of docs) D.set(id, tok(text));
    const N = D.size; let total = 0; for (const t of D.values()) total += t.length;
    const avg = N ? total / N : 0;
    const qt = [...new Set(tok(query))].slice(0, 32);
    const groups = qt.map((t, gi) => {
        if (opts.prefix && gi === qt.length - 1) { const all = new Set(); for (const t2 of D.values()) for (const w of t2) if (w.startsWith(t)) all.add(w); return [...all]; }
        return [t];
    });
    const scores = new Map(), masks = new Map();
    groups.forEach((terms, g) => {
        for (const term of terms) {
            let df = 0; for (const t2 of D.values()) if (t2.includes(term)) df++;
            if (!df) continue;
            const idf = Math.log(1 + (N - df + 0.5) / (df + 0.5));
            for (const [id, t2] of D) {
                const tf = t2.filter(w => w === term).length; if (!tf) continue;
                const s = idf * tf * (opts.k1 + 1) / (tf + opts.k1 * (1 - opts.b + opts.b * t2.length / avg));
                scores.set(id, (scores.get(id) || 0) + s); masks.set(id, (masks.get(id) || 0) | (1 << g));
            }
        }
    });
    const full = (1 << qt.length) - 1;
    return [...scores].filter(([id]) => !opts.and || masks.get(id) === full).sort((a, b) => b[1] - a[1] || a[0] - b[0]).slice(0, opts.limit);
}

function sameRanked(got, want, m) {
    let ok = got.length === want.length;
    for (let i = 0; ok && i < got.length; i++) ok = got[i][0] === want[i][0] && Math.abs(got[i][1] - want[i][1]) <= 1e-9 * Math.max(1, want[i][1]);
    check(ok, m + "\n   got  " + JSON.stringify(got).slice(0, 240) + "\n   want " + JSON.stringify(want).slice(0, 240));
}

for (const [seed, k1, b, stopList] of [[1, 1.2, 0.75, []], [2, 2.0, 0.3, ["the", "of"]], [3, 0.5, 1, ["a"]]]) {
    const r = rng(seed), vocab = [];
    for (let i = 0; i < 300; i++) vocab.push(["the", "of", "a"][i] || ("w" + (i * 7919 % 1000).toString(36) + (i % 3 ? "" : "é")));
    const zipf = () => vocab[Math.min(vocab.length - 1, Math.floor(1 / Math.max(r(), 1e-6)) % vocab.length)];
    const docs = new Map();
    const ii = new InvertedIndex({ k1, b, stopWords: stopList });
    const opts = { k1, b, stop: new Set(stopList) };
    for (let step = 0; step < 900; step++) {
        const id = Math.floor(r() * 400);
        if (r() < 0.8) { const len = 1 + Math.floor(r() * 30); const words = []; for (let i = 0; i < len; i++) words.push(r() < 0.1 ? zipf().toUpperCase() : zipf()); const text = words.join(r() < 0.5 ? " " : ", "); ii.add(id, text); docs.set(id, text); }
        else eq(ii.remove(id), docs.delete(id), `ii[${seed}] remove`);
        if (step % 60 === 59) {
            eq(ii.size, docs.size, `ii[${seed}] size`);
            for (let t = 0; t < 10; t++) {
                const q = [zipf(), zipf(), r() < 0.5 ? zipf() : "nomatch"].join(" ");
                const limit = 1 + Math.floor(r() * 15);
                sameRanked(ii.search(q, { limit }), bm25Ref(docs, q, { ...opts, limit }), `ii[${seed}] or "${q}"`);
                sameRanked(ii.search(q, { limit, mode: "and" }), bm25Ref(docs, q, { ...opts, limit, and: true }), `ii[${seed}] and "${q}"`);
                const pq = zipf() + " " + zipf().slice(0, 2);
                sameRanked(ii.search(pq, { limit, prefix: true }), bm25Ref(docs, pq, { ...opts, limit, prefix: true }), `ii[${seed}] prefix "${pq}"`);
            }
            const term = zipf();
            let df = 0; for (const text of docs.values()) if (InvertedIndex.tokenize(text).includes(term.toLowerCase()) && !opts.stop.has(term)) df++;
            eq(ii.docFreq(term), opts.stop.has(term) ? 0 : df, `ii[${seed}] docFreq(${term})`);
        }
    }
    const back = InvertedIndex.deserialize(ii.serialize());
    for (let t = 0; t < 20; t++) { const q = zipf() + " " + zipf(); same(back.search(q, { limit: 20 }), ii.search(q, { limit: 20 }), `ii[${seed}] serialize keeps search results`); }
    const sug = ii.suggest("w", 5);
    check(sug.length <= 5 && sug.every((e, i) => e[0].startsWith("w") && (i === 0 || sug[i - 1][1] >= e[1])), `ii[${seed}] suggest sorted by docFreq`);
}

__kitS.section("ii empty index", () => {
    const ii = new InvertedIndex();
    same(ii.search("anything"), [], "ii empty index");
    ii.add(1, "alpha beta");
    same(ii.search(""), [], "ii empty query");
    same(ii.search("alpha", { limit: 0 }), [], "ii limit 0");
    eq(ii.docLength(1), 2, "ii docLength");
    ii.add(1, "gamma");
    same(ii.search("alpha"), [], "ii re-adding an id replaces the document");
    eq(ii.search("gamma")[0][0], 1, "ii replaced document searchable");
    let threw = false; try { ii.search("x", { mode: "xor" }); } catch (e) { threw = e instanceof RangeError; } check(threw, "ii bad mode refused");
    threw = false; try { new InvertedIndex({ b: 2 }); } catch (e) { threw = e instanceof RangeError; } check(threw, "ii b out of range refused");
    threw = false; try { ii.add(-1, "x"); } catch (e) { threw = e instanceof RangeError; } check(threw, "ii negative id refused");
    const many = new InvertedIndex(); many.add(1, Array.from({ length: 40 }, (_, i) => "t" + i).join(" "));
    eq(many.search(Array.from({ length: 40 }, (_, i) => "t" + i).join(" "), { mode: "and" }).length, 1, "ii a 40-term AND query uses the first 32 terms");
});
__kitS.end();

print(`test_structures_db_search: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
