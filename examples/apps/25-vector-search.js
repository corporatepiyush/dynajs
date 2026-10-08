// 25 · Vector search — exact nearest-neighbour search over embeddings with SIMD kernels.
//
// WHAT IT SHOWS
//   - dyna:simd gemv: one call scores a query against every stored vector
//   - storing vectors in ONE contiguous Float32Array (row-major), not an array of arrays
//   - normalizing once at insert, so cosine similarity becomes a plain dot product
//   - topkIndices: select the k best scores, then sort only those k
//   - persisting the index as raw bytes with a small header
//
// RUN      dynajs examples/apps/25-vector-search.js
//          Embeddings here come from a toy hashing embedder so the file has no
//          dependencies; in production plug in vectors from your model.

import { gemv, normL2, scale, topkIndices, dot } from "dyna:simd";
import { XXHash32 } from "dyna:hash";
import { makeTempDir, writeFile, readBytes, removeAll } from "dyna:file";

const DIM = 256;

// ---- a toy embedder: hashed bag of word stems -------------------------------
// Similar texts share stems, so they land near each other. It is no language
// model, but it has the property that matters here: fixed-size dense vectors.
const STOP = new Set("a an and as do for how i is my of the to with without get need".split(" "));
function embed(text) {
    const v = new Float32Array(DIM);
    for (const word of text.toLowerCase().match(/[a-z0-9]+/g) ?? []) {
        if (STOP.has(word)) continue;
        const h = XXHash32(word.slice(0, 5));         // crude stemming: "forgot" ~ "forgotten"
        v[h % DIM] += (h >>> 16) & 1 ? 1 : -1;        // signed hashing reduces the bias of collisions
    }
    const norm = normL2(v);
    if (norm > 0) scale(v, 1 / norm);                 // unit length: cosine == dot
    return v;
}

// ---- the index -------------------------------------------------------------
class VectorIndex {
    constructor(dim, capacity = 1024) {
        this.dim = dim; this.count = 0;
        this.data = new Float32Array(dim * capacity);  // all vectors, back to back
        this.docs = [];
    }
    add(doc, vector) {
        if ((this.count + 1) * this.dim > this.data.length) {         // grow by doubling
            const bigger = new Float32Array(this.data.length * 2);
            bigger.set(this.data);
            this.data = bigger;
        }
        this.data.set(vector, this.count * this.dim);
        this.docs[this.count++] = doc;
    }
    search(query, k = 3) {
        const rows = this.data.subarray(0, this.count * this.dim);
        const scores = new Float32Array(this.count);
        gemv(scores, rows, query, this.count, this.dim, 0);           // scores = rows · query
        // topkIndices selects the k best but does not order them; sort those few.
        return [...topkIndices(scores, Math.min(k, this.count))]
            .map((i) => ({ doc: this.docs[i], score: scores[i] }))
            .sort((a, b) => b.score - a.score);
    }
    // File layout: "VIDX" | u32 dim | u32 count | u32 json length | json docs | padding | float32 data
    save(path) {
        const meta = new TextEncoder().encode(JSON.stringify(this.docs));
        const headerLen = Math.ceil((16 + meta.length) / 4) * 4;       // keep the floats 4-byte aligned
        const out = new Uint8Array(headerLen + this.count * this.dim * 4);
        const view = new DataView(out.buffer);
        out.set(new TextEncoder().encode("VIDX"));
        view.setUint32(4, this.dim, true); view.setUint32(8, this.count, true); view.setUint32(12, meta.length, true);
        out.set(meta, 16);
        out.set(new Uint8Array(this.data.buffer, 0, this.count * this.dim * 4), headerLen);
        writeFile(path, out);
    }
    static load(path) {
        const bytes = readBytes(path);
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        if (new TextDecoder().decode(bytes.subarray(0, 4)) !== "VIDX") throw new Error("not a vector index file");
        const dim = view.getUint32(4, true), count = view.getUint32(8, true), metaLen = view.getUint32(12, true);
        const headerLen = Math.ceil((16 + metaLen) / 4) * 4;
        // Validate the declared sizes against the real file before trusting them.
        if (headerLen + count * dim * 4 !== bytes.length) throw new Error("vector index file is truncated or corrupt");
        const index = new VectorIndex(dim, Math.max(count, 1));
        index.docs = JSON.parse(new TextDecoder().decode(bytes.subarray(16, 16 + metaLen)));
        index.data.set(new Float32Array(bytes.buffer.slice(bytes.byteOffset + headerLen, bytes.byteOffset + bytes.length)));
        index.count = count;
        return index;
    }
}

// ---- build and query -------------------------------------------------------
const corpus = [
    "How to reset a forgotten account password",
    "Password reset link expired, request a new one",
    "Refund policy for annual subscriptions",
    "Cancel a subscription and get a prorated refund",
    "Configure two-factor authentication with an authenticator app",
    "Export invoices as CSV for accounting",
    "Download a PDF invoice for a past payment",
    "API rate limits and how to request an increase",
    "Rotate API keys without downtime",
    "Invite team members and assign roles",
];
const index = new VectorIndex(DIM, 4);                // tiny capacity to exercise growth
for (const text of corpus) index.add(text, embed(text));

const ask = (q) => index.search(embed(q), 3);
for (const q of ["I forgot my password", "refund my subscription", "need an invoice pdf"]) {
    console.log(q);
    for (const hit of ask(q)) console.log(`   ${hit.score.toFixed(3)}  ${hit.doc}`);
}

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
check(ask("I forgot my password")[0].doc.includes("password"), "password questions find password articles");
check(ask("refund for my subscription")[0].doc.toLowerCase().includes("refund"), "refund questions find refund articles");
check(ask("rotate api keys")[0].doc === "Rotate API keys without downtime", "a near-verbatim query ranks its article first");
const self = index.search(embed(corpus[4]), 1)[0];
check(Math.abs(self.score - 1) < 1e-5, "a document's similarity to itself is 1");
// gemv must agree with scoring one row at a time.
const q = embed("invoice"), row3 = index.data.subarray(5 * DIM, 6 * DIM);
check(Math.abs(index.search(q, 10).find((h) => h.doc === corpus[5]).score - dot(row3, q)) < 1e-5, "batch and single scoring agree");

const dir = makeTempDir("vidx");
index.save(dir.join("help.vidx"));
const reloaded = VectorIndex.load(dir.join("help.vidx"));
check(reloaded.count === corpus.length && reloaded.search(embed("two-factor"), 1)[0].doc === corpus[4], "the index survives a save and load");
console.log(`self-test passed: ${index.count} vectors of ${DIM} dims`);
removeAll(dir);
