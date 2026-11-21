/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:vec
/** dyna:vec — approximate and exact nearest-neighbour search over f32 vectors (HNSW and Flat), with 64-bit ids, removal and id filters. */
declare module "dyna:vec" {
    /** Distance metric. l2 = squared Euclidean distance; cosine = 1 - cos(a, b), computed on vectors scaled to unit length and rounded to f32 when stored; ip = -dot(a, b), so that a smaller distance always ranks first. */
    type VectorMetric = "l2" | "cosine" | "ip";

    /** Index kind. flat = scan of every vector (exact with f32; with sq8/binary see VectorQuantization); hnsw = hierarchical navigable small world graph (approximate); "ivfpq" is rejected with RangeError (unsupported in this build). */
    type VectorIndexKind = "hnsw" | "ivfpq" | "flat";

    /**
     * Stored vector format. "f32" stores 32-bit floats and every distance is computed from them.
     *
     * "sq8" stores each component as a signed byte code c in [-127, 127]: c = clamp(floor((x - mid_j) / s + 0.5), -127, 127),
     * where mid_j = (lo_j + hi_j) / 2 for each dimension j and one step s = max_j (hi_j - lo_j) / 254 is shared by all
     * dimensions (s = 0 gives code 0). [lo_j, hi_j] covers the stored components (for cosine, after scaling to unit
     * length): the first add sets it to that batch's per-dimension minimum and maximum. A later add with a component
     * outside the range widens each such dimension to cover the batch, plus 25% of the widened width on every side that
     * grew, and re-encodes every stored vector: from its f32 copy, or with dyna.rerank false from its decoded codes
     * (mid_j + s * c_j), which adds rounding. The query is encoded the same way and clamped to the range. Candidate
     * distances are integer sums over the codes a (query) and b (stored): l2 = s^2 * sum_j (a_j - b_j)^2;
     * dot = sum_j mid_j^2 + s * (sum_j mid_j a_j + sum_j mid_j b_j) + s^2 * sum_j a_j b_j, so ip = -dot and cosine = 1 - dot.
     *
     * "binary" stores one bit per dimension, set when the stored component is greater than 0, packed little-endian into
     * 64-bit words (bits past dims are 0). The candidate distance is the Hamming distance (popcount of the XOR) for
     * every metric, so recall depends on data centred on zero, as embedding models produce; it is lower than sq8.
     *
     * With sq8 or binary, the candidate stage ranks by the candidate distance: the hnsw beam, the flat scan, and the
     * filtered exact fallback. With dyna.rerank true (the default), the candidate pool P = min(max(efSearch, k) * dyna.rerankFactor,
     * size) is widened before reranking: the hnsw beam and the flat scan keep the best P by the candidate distance (for
     * size, tombstones count), and those P are re-scored with the stored f32 copies, and the k nearest by that distance are returned with the
     * same distances an f32 index computes. With dyna.rerank false, no f32 copy is kept and the returned distances are
     * candidate distances (sq8: the formula above; binary: bit counts). The hnsw graph is built with f32 distances when
     * the f32 copy is kept, and otherwise with candidate distances between stored codes.
     *
     * Bytes per vector, before ids and links: f32 4*dims. sq8 adds dims, plus 8 for ip and cosine (the per-vector
     * sum_j mid_j b_j). binary adds 8*ceil(dims/64). With rerank false, only the codes are kept.
     */
    type VectorQuantization = "f32" | "sq8" | "binary";

    /**
     * dyna-specific meta configuration (not part of the portable VectorIndex shape). Every feature is on by default;
     * these keys only tune or switch off costly ones. hnsw only: any key here throws TypeError with index "flat", except
     * efSearch, rerank and rerankFactor, which a flat index with sq8 or binary accepts. Unknown keys throw TypeError.
     */
    interface VectorIndexMetaOptions {
        /** Default beam width for search, an integer in [1, 65536]; a search's efSearch overrides it and the beam used is max(efSearch, k). Default 128 (at 64, recall@10 on clustered cosine data fell to 0.925). */
        efSearch?: number;
        /**
         * Default true. When a search has a filter and its cardinality c (counted over the filter, including ids absent
         * from the index) satisfies c <= P (the candidate pool; max(efSearch, k) without a rerank; see VectorQuantization) or c * 16 <= size, the search scans the filter's ids instead of
         * the beam (exact with f32; with sq8 or binary it ranks by the candidate distance and reranks). false always uses the filtered beam, which then walks most of the graph for a
         * selective filter and is no cheaper; it exists only to measure the beam.
         */
        exactFallback?: boolean;
        /** Default true: neighbours are chosen with the HNSW diversity heuristic; for ip the heuristic compares Euclidean distances between the stored vectors, because -dot is not a metric and the plain rule leaves low-norm vectors with no in-links (unreachable by search and filters). false keeps the closest m (cheaper builds, much lower recall on clustered data). */
        heuristic?: boolean;
        /** Default true. sq8 and binary only (TypeError with f32): keep an f32 copy of every vector and re-score the candidates with it; see VectorQuantization. false keeps only the codes (4x smaller for sq8, 32x for binary) and returns candidate distances. */
        rerank?: boolean;
        /**
         * dyna extension, sq8 and binary with rerank true only (TypeError with f32 or rerank false): an integer in [1, 64]
         * that multiplies max(efSearch, k) into the candidate pool that is reranked (see VectorQuantization); saved with
         * the index. Default 12 for binary and 1 for sq8. 64 bits per vector order neighbours coarsely, so binary reranks
         * 12x the beam: on 4000 vectors of 64 dims, recall@10 rose from 0.45-0.74 (factor 1) to 0.958-0.993 (factor 12;
         * factor 8 gave 0.905 on clustered l2). The pool is fixed by k and efSearch, not by size, so recall at the same
         * factor falls as the index grows; raise it (or efSearch) for large indexes, lower it for speed.
         */
        rerankFactor?: number;
        /** Seed for the layer draw, an integer in [0, 2^53-1]. Default 0. The same seed, options and call sequence build the same graph and return the same results. */
        seed?: number;
    }

    /** Constructor options; unknown keys throw TypeError. m and efConstruction throw TypeError with index "flat". */
    interface VectorIndexOptions {
        /** Vector length, an integer in [1, 65536]; required. */
        dims: number;
        /** Default "l2". */
        metric?: VectorMetric;
        /** Default "hnsw". */
        index?: VectorIndexKind;
        /** Default "f32". */
        quantization?: VectorQuantization;
        /** Links per node on the upper layers, an integer in [2, 256]; layer 0 keeps 2*m. Default 16. */
        m?: number;
        /** Beam width while inserting, an integer in [1, 65536]. Default 200. */
        efConstruction?: number;
        /** dyna-specific meta configuration; see VectorIndexMetaOptions. */
        dyna?: VectorIndexMetaOptions;
    }

    /** Search options; unknown keys throw TypeError. */
    interface VectorSearchOptions {
        /** Beam width for this call, an integer in [1, 65536]; the beam used is max(efSearch, k). With sq8 or binary and dyna.rerank true, the beam and the flat scan keep max(efSearch, k) * dyna.rerankFactor candidates (at most size) for the rerank; with rerank false, max(efSearch, k); a flat f32 index ignores it. */
        efSearch?: number;
        /**
         * Allowed ids. Only ids in the filter are returned; ids in the filter that are not in the index are ignored. The
         * beam still walks through disallowed and removed nodes. See dyna.exactFallback.
         * A RoaringBitmap or BitSet holds uint32, so index ids at or above 2^32 never pass one. For 64-bit ids, pass a
         * BigUint64Array sorted strictly ascending (no duplicates); anything else rejects with RangeError. It is a plain
         * typed array, not a new class, because ids already travel as BigUint64Array, a sorted array tests membership by
         * binary search with no build step, and its length is the cardinality the exact fallback needs. The array is
         * copied when search is called. The BigUint64Array and BitSet forms are dyna-specific extensions.
         */
        filter?: import("dyna:structures").RoaringBitmap | import("dyna:structures").BitSet | BigUint64Array;
    }

    /** Nearest neighbours of one query, nearest first; equal distances (compared in double precision) rank by the smaller id. Length is min(k, matching vectors). */
    interface VectorSearchResult {
        ids: BigUint64Array;
        /** Distances computed in double precision over the stored f32 components, per the metric, then rounded to f32; with sq8 or binary and dyna.rerank false, the candidate distances (see VectorQuantization). */
        distances: Float32Array;
    }

    /** dyna-specific. Nearest neighbours of many queries, k slots per query in query order. */
    interface VectorBatchResult {
        /** Row q holds slots [q*k, q*k+k); slots past counts[q] hold 18446744073709551615n. */
        ids: BigUint64Array;
        /** Slots past counts[q] hold Infinity. */
        distances: Float32Array;
        /** Number of results found for each query. */
        counts: Uint32Array;
    }

    /**
     * An in-memory vector index of f32 vectors, stored as f32, sq8 or binary codes (see VectorQuantization), keyed by 64-bit ids, integers in [0, 2^64-2] (2^64-1 is reserved as the
     * padding id); ids arrive as BigUint64Array or Uint32Array and are mapped to dense internal slots. Vectors are
     * Float32Array, dims floats each, copied in; NaN, Infinity, and for cosine a zero vector, are refused.
     *
     * add, search and searchBatch return Promises and run one at a time, in call order: a call made while earlier ones
     * are unsettled waits for them (so a search sees every add called before it). A call with no earlier unsettled
     * call and little work runs to completion before it returns its Promise; large ones run off the event loop.
     * Arguments are validated and copied when the method is called, and argument errors reject the returned Promise.
     * remove and has are synchronous and throw TypeError while any add or search is unsettled; close throws TypeError
     * then too (DynResource). Any use after close throws or rejects with TypeError.
     *
     * remove leaves a tombstone: the vector is never returned (with or without a filter, and in the exact fallback)
     * but keeps its memory and stays in the graph as a waypoint. A removed id may be added again; it gets a new slot.
     *
     * save(path, opts?) writes one dyna persist v1 file and VectorIndex.load(path, opts?) reads it back; both are
     * synchronous. File format, atomic rename, fsync and the Error .code values are those of dyna:structures (see its
     * FILES notes); load refuses a file of another persist type with EPERSIST_TYPE. The file holds the options, the
     * layer-draw state, each slot's vector (its f32 copy when kept, its sq8 or binary codes, and the sq8 range), id and
     * tombstone flag (the id map is rebuilt from ids and tombstones on load) and, for hnsw, each node's level and links, so a loaded index returns exactly what the saved one returned
     * and later adds build the same graph. The same index state always saves to the same bytes (fixed lineage,
     * generation 1), so save, load, save reproduces the file. incremental is accepted and always writes a full save.
     * mmap: true maps the file privately and searches the stored f32 vectors in place (codes are copied), copy-on-write, never modifying the
     * file; the first add that grows the index copies them out. Before trusting a file, load checks every size and
     * range, that tombstone flags are 0 or 1 and match the live count, that live ids are unique and not 2^64-1, that
     * every component is finite, that the quantization matches its sections and flags, that the rerank factor is in [1, 64] when an f32 copy is reranked and 0 otherwise, that the sq8 range is finite and
     * ordered and its codes are in [-127, 127], that binary codes set no bit past dims, and for hnsw that the entry point is a top-layer node, that each link list is within
     * its degree bound and that every link targets another existing node on that layer; a failed check throws
     * EPERSIST_CORRUPT.
     */
    class VectorIndex implements DynResource {
        /** @throws {TypeError} when opts is not an object, dims is missing, a key is unknown, an option has the wrong type, or an hnsw-only option is given with index "flat", or dyna.rerank with quantization "f32", or dyna.rerankFactor with quantization "f32" or dyna.rerank false. @throws {RangeError} when an option is out of range, or index "ivfpq" (unsupported in this build). */
        constructor(opts: VectorIndexOptions);
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
        /** dyna-specific. Number of vectors (removed ones excluded); while an add is unsettled it holds the size before that add. */
        readonly size: number;
        /** dyna-specific. */
        readonly dims: number;
        /** dyna-specific. */
        readonly metric: VectorMetric;
        /** dyna-specific. */
        readonly index: VectorIndexKind;
        /** dyna-specific. */
        readonly quantization: VectorQuantization;
        /**
         * Inserts ids[i] with vectors[i*dims .. i*dims+dims) in order. Rejects with RangeError on a reserved id, an id
         * twice in the batch, an id already in the index (checked when the add runs), a length other than
         * ids.length * dims, or a non-finite (or cosine zero) vector, and then leaves the index unchanged; TypeError on
         * wrong array types. Running out of memory part way rejects and keeps the vectors inserted so far.
         */
        add(ids: BigUint64Array | Uint32Array, vectors: Float32Array): Promise<void>;
        /**
         * The k nearest vectors to query, k an integer in [1, 65536]. k above the number of matching vectors returns
         * fewer results; an empty index returns empty arrays. flat f32 is exact; hnsw is approximate unless the exact
         * fallback applies; sq8 and binary are approximate (see VectorQuantization). Rejects with RangeError on a bad k or efSearch, a length other than dims, a non-finite (or
         * cosine zero) query, or a BigUint64Array filter that is not strictly ascending; TypeError on a bad filter or options.
         */
        search(query: Float32Array, k: number, opts?: VectorSearchOptions): Promise<VectorSearchResult>;
        /** dyna-specific. search() for each dims-long row of queries in one call; rejects with the errors search() rejects with. */
        searchBatch(queries: Float32Array, k: number, opts?: VectorSearchOptions): Promise<VectorBatchResult>;
        /** Removes the given ids and returns how many were present (an id listed twice counts once). @throws {TypeError} on wrong array type or while an add or search is unsettled. */
        remove(ids: BigUint64Array | Uint32Array): number;
        /** dyna-specific. Whether id is in the index (removed ids are not); a number or bigint that is not a valid id returns false. @throws {TypeError} when id is not a number or bigint, or while an add or search is unsettled. */
        has(id: number | bigint): boolean;
        /** Writes the index to path; see the class notes. @throws {TypeError} when path is not a dyna:file Path, an option is not a boolean, the index is closed, or an add or search is unsettled. @throws {Error} with .code EPERSIST_* when the write fails. */
        save(path: import("dyna:file").Path, opts?: import("dyna:structures").PersistSaveOptions): import("dyna:structures").PersistSaveResult;
        /** Reads an index written by save; see the class notes. @throws {TypeError} when path is not a dyna:file Path or an option is not a boolean. @throws {Error} with .code EPERSIST_* when the file is missing, damaged, of another type or fails a check. */
        static load(path: import("dyna:file").Path, opts?: import("dyna:structures").PersistLoadOptions): VectorIndex;
    }
}
