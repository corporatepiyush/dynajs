/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:structures
/** dyna:structures — heaps, LRU, tries, B-trees, sorted maps, segment trees, bitsets, sketches, range sets, roaring bitmaps, k-d trees, BM25 indexes.
 *  Each persists via serialize()/deserialize(); bytes are untrusted (lengths validated before allocating; at most 512 MiB + 64x the record's payload per record).
 *  FILES  x.save(path, opts?) writes one file and Class.load(path, opts?) reads it back; both are synchronous. The format is dyna persist v1: a superblock, two header slots A/B (each with its own crc32c; load uses the intact one with the higher generation), and sections with one crc32c each. Large sections are 16 KiB aligned. BloomFilter, CountMinSketch and HyperLogLog store their counter arrays raw, written straight from memory with writev. Every other class stores its serialize() record as one section. The writer's buffers are reused per thread.
 *  save is atomic: it writes a temp file in the same directory, fsyncs it, renames it over `path`, then fsyncs the directory. A crash leaves the old file or the new one; a stray "<path>.tmp.<pid>.<n>" may remain.
 *  With fsync (the default) a returned save is durable across power loss: on macOS plain fsync() leaves data in the drive's cache, so every sync there is fcntl(F_FULLFSYNC), falling back to fsync() only where the filesystem refuses it. That costs milliseconds per save on Apple SSDs; pass fsync: false when atomicity alone is enough.
 *  An incremental save never overwrites existing data. It appends the changed 16 KiB chunks and an overlay table, fsyncs, then rewrites the OLDER header slot and fsyncs again. A crash or power loss at any step loads as the previous or the new state.
 *  It falls back to a full save when: the instance did not save this file or load it last (lineage/generation mismatch); the first save after a load; the layout changed; or the appended space exceeds the base size (compaction).
 *  load checks before trusting anything: magic, version, byte order, superblock/slot/overlay/section checksums, every offset and length, then the structure's own invariants. A file of another class is EPERSIST_TYPE.
 *  mmap: true maps the file privately. The raw classes use their arrays in place: writes stay copy-on-write and the file is never modified, and the instance keeps the mapping and one descriptor until it is collected. Another process truncating or rewriting the file in place while it is mapped raises SIGBUS; dyna's own saves never do either (full saves rename, incremental saves only append). The default is a private copy.
 *  Errors are Error objects with .code. EPERSIST_IO also carries .errno. The other codes: EPERSIST_MAGIC, EPERSIST_VERSION, EPERSIST_ENDIAN, EPERSIST_TRUNCATED, EPERSIST_CORRUPT, EPERSIST_CHECKSUM, EPERSIST_TYPE, EPERSIST_MISSING, EPERSIST_TOO_LARGE, EPERSIST_NOMEM. A record that fails to decode (reachable with verify: false) throws EPERSIST_CHECKSUM or EPERSIST_CORRUPT; Heap.load rebuilds through push, so its compare function runs during load; an error it throws comes through unchanged (no .code added). A path that is not a dyna:file Path, or a non-boolean option, is a TypeError.
 *  Stored arrays are written index by index with holes as undefined; serialize() throws RangeError for an array with more than 2^24 holes beyond its real elements. */
declare module "dyna:structures" {
    /** Options for `save(path, opts)` on every dyna:structures class. */
    interface PersistSaveOptions {
        /** Durably sync the file, then its directory (default true; F_FULLFSYNC on macOS, see FILES); false trades durability for speed, atomicity is kept. */
        fsync?: boolean;
        /** Default false. BloomFilter, CountMinSketch and HyperLogLog only: append just the changed 16 KiB chunks and commit through the older header slot; other classes, and every case listed under FILES, write a full save. */
        incremental?: boolean;
    }

    /** Options for `Class.load(path, opts)`. */
    interface PersistLoadOptions {
        /** Map the file (MAP_PRIVATE) instead of reading it (default false). BloomFilter, CountMinSketch and HyperLogLog then use their arrays in place, copy-on-write; see FILES for the SIGBUS hazard. */
        mmap?: boolean;
        /** Check every section checksum (default true); false skips only those, never the header checksum or the bounds checks. */
        verify?: boolean;
    }

    /** What `save` reports. */
    interface PersistSaveResult {
        /** The path argument, as passed. */
        path: import("dyna:file").Path;
        /** The file's logical size after the save. */
        bytes: number;
        /** Bytes this save wrote (smaller than `bytes` for an incremental save). */
        written: number;
        /** True when the save appended chunks instead of rewriting the file. */
        incremental: boolean;
    }

    /** Capacity-bounded string-to-value cache with LRU eviction. */
    class LRU<V = unknown> {
        /** LRU cache of `capacity` entries; `ttlMs` expires entries, `onEvict(key, value)` observes evictions. */
        constructor(capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: V) => void });
        /** Rebuilds from serialize() bytes; T flows through, so no cast is needed. */
        static deserialize<T>(this: new (capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: T) => void }) => LRU<T>, bytes: Uint8Array | ArrayBuffer): LRU<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new (capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: T) => void }) => LRU<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): LRU<T>;
        /** The value and moves the entry to MRU. An expired entry is dropped (counted in `expired`) before the miss. */
        get(key: string): V | undefined;
        /** Inserts or updates, marking the entry MRU; past capacity the LRU entry is evicted. */
        put(key: string, value: V): this;
        /** Alias for `put`. */
        set(key: string, value: V): this;
        /** Stores an entry with its own lifetime in ms. */
        setWithTTL(key: string, value: V, ms: number): this;
        /** Presence without touching recency or counting a hit; an expired key reads absent. */
        has(key: string): boolean;
        /** Removes the entry if present. */
        delete(key: string): boolean;
        /** Reclaims every expired entry now; returns the count removed. */
        purgeExpired(): number;
        /** The number of entries. */
        readonly size: number;
        /** The capacity bound. */
        readonly capacity: number;
        /** `{ hits, misses, evictions, expired, size, capacity }`. */
        readonly stats: { hits: number; misses: number; evictions: number; expired: number; size: number; capacity: number };
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Binary heap ordered by a JS comparator or natural number order. */
    class Heap<V = number> {
        /** Creates a binary heap. The comparator must not push/pop its own heap (reentrancy throws `TypeError`). */
        constructor(comparator?: (a: V, b: V) => number);
        /** Rebuilds from serialize() bytes with the comparator; T flows from it. */
        static deserialize<T>(bytes: Uint8Array | ArrayBuffer, cmp: (a: T, b: T) => number): Heap<T>;
        static deserialize<T>(this: new (comparator?: (a: T, b: T) => number) => Heap<T>, bytes: Uint8Array | ArrayBuffer): Heap<T>;
        /** Reads a file written by save(); `compare` is the comparator (natural order when omitted). See the module notes on FILES. */
        static load<T>(path: import("dyna:file").Path, opts?: PersistLoadOptions & { compare?: (a: T, b: T) => number }): Heap<T>;
        /** Insert and sift; returns the new size. */
        push(v: V): number;
        /** Removes and returns the root; `undefined` when empty. */
        pop(): V | undefined;
        /** The root without removal; never calls the comparator. */
        peek(): V | undefined;
        /** The element count. Getter; `Heap.length` is the same getter. */
        readonly size: number;
        readonly length: number;
        /** The contents as a compact type-tagged record. A comparator is not data, so the record cannot carry it. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Atkinson/Sack/Santoro/Strothotte min-max heap. */
    class MinMaxHeap<V = number> {
        /** Creates an empty min-max heap. */
        constructor();
        /** Rebuilds a min-max heap from its record. */
        static deserialize<T>(this: new () => MinMaxHeap<T>, bytes: Uint8Array | ArrayBuffer): MinMaxHeap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => MinMaxHeap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): MinMaxHeap<T>;
        /** Inserts a value with a numeric priority. */
        push(priority: number, value?: V): this;
        /** Removes and returns the minimum's stored value. */
        popMin(): V | undefined;
        /** Removes and returns the maximum's stored value. */
        popMax(): V | undefined;
        /** The minimum without removal. */
        peekMin(): V | undefined;
        /** The maximum without removal. */
        peekMax(): V | undefined;
        /** The element count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Set of numbers in sorted order (the same ordered-tree core as BTree). */
    class SortedSet {
        /** Creates an empty set. NaN keys are rejected on insert (`RangeError`). */
        constructor();
        /** Rebuilds a set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): SortedSet;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): SortedSet;
        /** Inserts the key if absent; duplicates are no-ops. */
        add(x: number): this;
        /** Whether the key is present. */
        has(x: number): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(x: number): boolean;
        /** The smallest key. */
        first(): number | undefined;
        /** The largest key. */
        last(): number | undefined;
        /** The largest element <= x (ceil: the smallest >= x). */
        floor(x: number): number | undefined;
        /** The smallest key ≥ `x`; `undefined` if none or `x` is NaN. */
        ceil(x: number): number | undefined;
        /** Ascending keys in [lo, hi]. */
        rangeQuery(lo: number, hi: number): number[];
        /** All keys ascending. */
        toArray(): number[];
        /** The key count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<number>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Sorted map from numeric key to value (same core as BTree, but a DISTINCT serialize format and no [Symbol.iterator]). */
    class SortedMap<V = unknown> {
        /** Creates an empty map. NaN keys are rejected on insert (`RangeError`). */
        constructor();
        /** Rebuilds a map from its record. */
        static deserialize<T>(this: new () => SortedMap<T>, bytes: Uint8Array | ArrayBuffer): SortedMap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => SortedMap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): SortedMap<T>;
        /** Inserts or replaces. A NaN key throws `RangeError`. */
        set(k: number, v: V): this;
        /** The value; `undefined` for absent or NaN keys. */
        get(k: number): V | undefined;
        /** Whether the key is present. */
        has(k: number): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: number): boolean;
        /** The smallest key. */
        firstKey(): number | undefined;
        /** The largest key. */
        lastKey(): number | undefined;
        /** The largest key <= k (ceilKey: the smallest >= k). */
        floorKey(k: number): number | undefined;
        /** The smallest key ≥ `k`; `undefined` if none or `k` is NaN. */
        ceilKey(k: number): number | undefined;
        /** Ascending `[k, v]` pairs in `[lo, hi]`. */
        rangeQuery(lo: number, hi: number): [number, V][];
        /** Ascending keys. */
        keys(): number[];
        /** The key count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Ordered-map options: `keys` is "number" (default; NaN refused, -0 stored as +0) or "string" (UTF-8 byte order). */
    interface OrderedTreeOptions {
        /** The key kind. */
        keys?: "number" | "string";
    }

    /** Ordered map; O(log n) updates, in-order iteration. The name is historical: storage is the
     * counted B-tree shared with WBTree and SkipList (31 keys per node; each node is its own allocation
     * addressed by a u32 slot table, leaves without child links or counts; see WBTree for the D4 layout), not a red-black tree.
     * Iteration order, answers and save() records are byte-identical to the old red-black tree. Up to
     * 128 entries the map is one sorted array grown 2, 4, ... 128 (no node pool), then it is
     * bulk-loaded into the B-tree. Measured against the red-black tree: at N=16..512 every op is
     * 1.0-1.9x faster at 37-60 B/entry (was 61-85); at N >= 1024 set 1.25-2.8x, get 1.15-1.7x,
     * delete 1.3-2.4x, floorKey/ceilKey 1.25-1.9x faster at 37-47 B/entry (was 60); repeated
     * build/drop runs plateau. */
    class RBTree<K extends number | string = number, V = unknown> {
        /** Creates an empty tree. */
        constructor(options?: OrderedTreeOptions);
        /** Inserts or replaces. */
        set(k: K, v: V): this;
        /** The value, or `undefined`. */
        get(k: K): V | undefined;
        /** Whether the key is present. */
        has(k: K): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: K): boolean;
        /** Ascending keys. */
        keys(): K[];
        /** Values in key order. */
        values(): V[];
        /** Ascending `[key, value]` pairs. */
        entries(): [K, V][];
        [Symbol.iterator](): Iterator<[K, V]>;
        /** The smallest key. */
        firstKey(): K | undefined;
        /** The largest key. */
        lastKey(): K | undefined;
        /** The largest key <= `k`. */
        floorKey(k: K): K | undefined;
        /** The smallest key >= `k`. */
        ceilKey(k: K): K | undefined;
        /** The key count. */
        readonly size: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): RBTree;
    }

    /** Ordered map with O(log n) rank and select. The name is historical: storage is the counted
     * B-tree shared with RBTree and SkipList, not a weight-balanced tree. Iteration order, answers
     * and save() records are byte-identical to the old weight-balanced tree. Up to 128 entries the
     * map is one sorted array (see RBTree). Layout (D4, 11-21): one allocation per node (no 32-node chunk slack);
     * unused number-key slots hold +Infinity so get/rank/floorKey/ceilKey count all 31 slots without a
     * data-dependent branch (set/delete keep the branchy scan, whose speculation wins on writes); rank adds the
     * child counts left of the descent with one masked NEON/SSE2 sum; a full leaf receiving a key past its last
     * (or before its first) key on the tree's right (left) edge keeps 30 keys and starts a new edge leaf, so
     * ascending/descending fills stay near 30 of 31 slots. Measured against the weight-balanced tree (best of 5
     * interleaved, number keys, random order): every op at N=16..1M is 1.05-2.4x faster (rank 1.25x at 16,
     * 1.31x at 256, 1.19x at 1024, 1.55x at 64K, 2.25x at 1M); memory 36-49 B/entry at N=128..64K random and
     * 28-49 ascending (was 48-51), 37.3 at 1M (was 48.3), never above the old tree at any swept N; repeated
     * build/drop runs plateau. Against the pre-D4 B-tree: every op 1.0-1.4x faster except iterate at 1M (0.97x),
     * and the chunk sawtooth (62 B/entry at 768, 70 ascending) is gone. */
    class WBTree<K extends number | string = number, V = unknown> {
        /** Creates an empty tree. */
        constructor(options?: OrderedTreeOptions);
        /** Inserts or replaces. */
        set(k: K, v: V): this;
        /** The value, or `undefined`. */
        get(k: K): V | undefined;
        /** Whether the key is present. */
        has(k: K): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: K): boolean;
        /** Ascending keys. */
        keys(): K[];
        /** Values in key order. */
        values(): V[];
        /** Ascending `[key, value]` pairs. */
        entries(): [K, V][];
        [Symbol.iterator](): Iterator<[K, V]>;
        /** The smallest key. */
        firstKey(): K | undefined;
        /** The largest key. */
        lastKey(): K | undefined;
        /** The largest key <= `k`. */
        floorKey(k: K): K | undefined;
        /** The smallest key >= `k`. */
        ceilKey(k: K): K | undefined;
        /** The key count. */
        readonly size: number;
        /** The number of keys below `k`. */
        rank(k: K): number;
        /** The `[key, value]` at sorted position `i` (negative counts from the end). */
        at(i: number): [K, V] | undefined;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): WBTree;
    }

    /** Ordered map with O(log n) rank and select. The name is historical: storage is a counted B-tree
     * (31 keys per node, per-child subtree counts in inner nodes only, each node its own allocation; see WBTree), not a skip list.
     * Iteration order, rank/at answers and save() records are byte-identical to the old skip list.
     * Measured against it at N=128..100K: set 1.5-1.8x, get 1.5-2.1x, delete 1.3-1.6x,
     * rank 1.5-2.1x, at 1.3-1.9x faster; 37-47 B/entry at N >= 1024 (was 66); up to 128 entries it
     * is one sorted array (see RBTree): 60 B/entry at 16 (was 1147 on the first B-tree), 37 at 128
     * (was 73 on the skip list). Splitting leaves into their own pool cost 3-8% CPU on set, get and
     * rank at N=512..100K against the single-pool B-tree for 20% less memory (46.9 -> 37.5 B/entry
     * at 100K). floorKey/ceilKey take one descent. */
    class SkipList<K extends number | string = number, V = unknown> {
        /** Creates an empty list. */
        constructor(options?: OrderedTreeOptions);
        /** Inserts or replaces. */
        set(k: K, v: V): this;
        /** The value, or `undefined`. */
        get(k: K): V | undefined;
        /** Whether the key is present. */
        has(k: K): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: K): boolean;
        /** Ascending keys. */
        keys(): K[];
        /** Values in key order. */
        values(): V[];
        /** Ascending `[key, value]` pairs. */
        entries(): [K, V][];
        [Symbol.iterator](): Iterator<[K, V]>;
        /** The smallest key. */
        firstKey(): K | undefined;
        /** The largest key. */
        lastKey(): K | undefined;
        /** The largest key <= `k`. */
        floorKey(k: K): K | undefined;
        /** The smallest key >= `k`. */
        ceilKey(k: K): K | undefined;
        /** The key count. */
        readonly size: number;
        /** The number of keys below `k`. */
        rank(k: K): number;
        /** The `[key, value]` at sorted position `i` (negative counts from the end). */
        at(i: number): [K, V] | undefined;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): SkipList;
    }

    /** Sparse array over 64-bit indices (a radix tree with 3 mark bits per slot); indices above 2^53 are BigInts. */
    class XArray<V = unknown> {
        /** Creates an empty array. */
        constructor();
        /** Stores `v` at index `i` (a safe integer or a BigInt below 2^64). */
        set(i: number | bigint, v: V): this;
        /** The value at `i`, or `undefined`. */
        get(i: number | bigint): V | undefined;
        /** Whether index `i` holds a value. */
        has(i: number | bigint): boolean;
        /** Removes index `i`, reporting whether it held a value. */
        delete(i: number | bigint): boolean;
        /** The first `[index, value]` with `from <= index <= max`. */
        next(from?: number | bigint, max?: number | bigint): [number | bigint, V] | undefined;
        /** The first `[index, value]` in `[from, max]` carrying `mark` (0..2). */
        nextMarked(mark: number, from?: number | bigint, max?: number | bigint): [number | bigint, V] | undefined;
        /** Sets (or with `on` false clears) `mark` on an occupied slot; false when the slot is empty. */
        mark(i: number | bigint, mark: number, on?: boolean): boolean;
        /** Whether slot `i` carries `mark`. */
        isMarked(i: number | bigint, mark: number): boolean;
        /** `[index, value]` pairs in index order within `[from, max]`. */
        entries(from?: number | bigint, max?: number | bigint): [number | bigint, V][];
        [Symbol.iterator](): Iterator<[number | bigint, V]>;
        /** The occupied slot count. */
        readonly size: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): XArray;
    }

    /** Path-compressed binary trie over string, byte or CIDR keys; a trie holds one key space at a time. serialize/save
     * write record version 2 (raw keys in walk order, loaded with a path cursor in about linear time); version 1
     * records still load, and a build that predates version 2 refuses it as a truncated DYNS record.
     * Storage: 16-byte nodes with u32 children in 1024-entry chunks (the first chunk grows 8, 16, .. 1024 so small tries stay
     * small and repeated build/drop runs plateau), plus one value slot per entry holding keys up to 16 bytes inline (longer keys
     * own one heap block). Lookups test only the branch bits and compare the full key once at the end; walk order, records and
     * longestPrefix results are unchanged. About 60-68 B/entry for short string keys (was 116), 112 for 35-byte keys (was 173). */
    class PatriciaTrie<V = unknown> {
        /** Creates a trie over UTF-8 strings (default) or `Uint8Array` keys (`keys: "bytes"`). */
        constructor(options?: { keys?: "string" | "bytes" });
        /** Inserts or replaces. */
        set(k: string | Uint8Array, v: V): this;
        /** The value, or `undefined`. */
        get(k: string | Uint8Array): V | undefined;
        /** Whether the key is present. */
        has(k: string | Uint8Array): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: string | Uint8Array): boolean;
        /** The `[key, value]` of the longest stored key that prefixes `k`. */
        longestPrefix(k: string | Uint8Array): [string | Uint8Array, V] | undefined;
        /** Every `[key, value]` whose key starts with `prefix`, in byte order. */
        withPrefix(prefix?: string | Uint8Array): [string | Uint8Array, V][];
        /** Stores `v` under an IPv4/IPv6 block such as "10.0.0.0/8" (no length: a host route). */
        addCIDR(cidr: string, v: V): this;
        /** The value of the most specific block containing `ip`; IPv4 and IPv6 never match each other. */
        matchIP(ip: string): V | undefined;
        /** Removes a block, reporting whether it was present. */
        deleteCIDR(cidr: string): boolean;
        /** The key count. */
        readonly size: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): PatriciaTrie;
    }

    /** Immutable graph in compressed sparse row form, nodes 0..n-1. */
    class CSRGraph {
        /** Builds from `[src, dst, weight = 1]` edges over 0..2^26 nodes; undirected (`directed: false`) stores each edge both ways. */
        constructor(nodes: number, edges: [number, number, number?][], options?: { directed?: boolean });
        /** The node count. */
        readonly nodes: number;
        /** The stored edge count (twice the input for undirected graphs). */
        readonly edgeCount: number;
        /** Out-neighbours of `v`. */
        neighbors(v: number): Uint32Array;
        /** Out-edge weights of `v`, aligned with `neighbors(v)`. */
        weights(v: number): Float64Array;
        /** The out-degree of `v`. */
        degree(v: number): number;
        /** Hop counts from `src`; -1 where unreachable. */
        bfs(src: number): Int32Array;
        /** Shortest distances and predecessors from `src` (-1: none); a negative weight throws `RangeError`. */
        dijkstra(src: number): { dist: Float64Array; prev: Int32Array };
        /** PageRank by power iteration; dangling mass is spread evenly (defaults 0.85, 100, 1e-10). */
        pagerank(options?: { damping?: number; iterations?: number; tolerance?: number }): Float64Array;
        /** Weakly connected components: a label per node and their count. */
        components(): { count: number; labels: Uint32Array };
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): CSRGraph;
    }

    /** R*-tree of axis-aligned boxes with numeric ids, 1..8 dimensions. */
    class RStarTree {
        /** Creates a tree of `dims` dimensions (default 2). */
        constructor(dims?: number);
        /** Adds box `[lo, hi]` with id `id` (a non-negative safe integer). */
        insert(lo: number[], hi: number[], id: number): this;
        /** Removes the entry with exactly this box and id, reporting whether it existed. */
        remove(lo: number[], hi: number[], id: number): boolean;
        /** Ids of every box intersecting `[lo, hi]` (edges inclusive). */
        search(lo: number[], hi: number[]): number[];
        /** The `k` (1..100000) boxes nearest `point`, closest first, with Euclidean box distance. */
        nearest(point: number[], k?: number): { id: number; distance: number }[];
        /** The entry count. */
        readonly size: number;
        /** The tree height (1 for a single leaf). */
        readonly height: number;
        /** The dimension count. */
        readonly dims: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. The tree is bulk-built (STR packing), so its height, node layout, search() order and serialize() bytes may differ from the saved tree; the entry set and every query result set are the same. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): RStarTree;
    }

    /** Suffix array (SA-IS) over a string or bytes; string positions and lengths are UTF-16 units, order is UTF-8 byte order. */
    class SuffixArray {
        /** Builds the array; text over 4 GiB of UTF-8 throws `RangeError`. `cache` (default off): `bucket` (or `cache: true`)
         * builds a table of where each first byte (texts of 1024+ bytes, 1 KiB) or first two bytes (2^18+ bytes, 256 KiB)
         * start in the array, so count/find of patterns at least that long search one block: about 1.3-1.5x faster below
         * 2^18 bytes, 2x at 2^18, 3x at 10 MB. Results never change; a loaded or deserialized array starts without it.
         * Unknown option keys throw TypeError. */
        constructor(text: string | Uint8Array, options?: { cache?: boolean | { bucket?: boolean } });
        /** Suffix start positions in sorted order. */
        array(): Uint32Array;
        /** LCP of each suffix with the previous one in `array()` order (0 first). */
        lcp(): Uint32Array;
        /** How many times `pattern` occurs (0 for the empty pattern). */
        count(pattern: string | Uint8Array): number;
        /** Ascending start positions of every occurrence of `pattern`. */
        find(pattern: string | Uint8Array): number[];
        /** The longest substring that occurs at least twice, or `undefined`. */
        longestRepeat(): { start: number; length: number } | undefined;
        /** The text length (UTF-16 units for a string, bytes otherwise). */
        readonly length: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): SuffixArray;
    }

    /** Ordered map on numeric keys as a B-tree of order 32; iterable over [key, value]; its serialize format differs from SortedMap's. */
    class BTree<V = unknown> {
        /** Creates an empty tree. NaN keys throw `TypeError` ("keys must be ordered numbers, not NaN"). */
        constructor();
        /** Rebuilds a tree from its record. */
        static deserialize<T>(this: new () => BTree<T>, bytes: Uint8Array | ArrayBuffer): BTree<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => BTree<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): BTree<T>;
        /** Inserts or replaces. O(log n). */
        set(k: number, v: V): this;
        /** The value. O(log n). */
        get(k: number): V | undefined;
        /** Whether the key is present. O(log n). */
        has(k: number): boolean;
        /** Removes the key, reporting whether it was present. O(log n). */
        delete(k: number): boolean;
        /** The smallest key. */
        firstKey(): number | undefined;
        /** The largest key. */
        lastKey(): number | undefined;
        /** The largest key <= k (ceilKey: the smallest >= k). */
        floorKey(k: number): number | undefined;
        /** The smallest key ≥ `k`; `undefined` if none. */
        ceilKey(k: number): number | undefined;
        /** Ascending `[k, v]` pairs in `[lo, hi]`. */
        rangeQuery(lo: number, hi: number): [number, V][];
        /** Ascending keys. */
        keys(): number[];
        /** The key count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[number, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Double-ended queue with O(1) push/pop at both ends. */
    class Deque<V = unknown> {
        /** Creates an empty deque; the buffer grows from 8. */
        constructor();
        /** Rebuilds a deque from its record. */
        static deserialize<T>(this: new () => Deque<T>, bytes: Uint8Array | ArrayBuffer): Deque<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => Deque<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): Deque<T>;
        /** Appends at the back and returns the new length. */
        pushBack(v: V): number;
        /** Prepends at the front and returns the new length. */
        pushFront(v: V): number;
        /** Removes and returns the front; `undefined` when empty. */
        popFront(): V | undefined;
        /** Removes and returns the back; `undefined` when empty. */
        popBack(): V | undefined;
        /** The front without removal. */
        peekFront(): V | undefined;
        /** The back without removal. */
        peekBack(): V | undefined;
        /** The element at index `i`. */
        get(i: number): V | undefined;
        /** The element count. */
        readonly length: number;
        /** A snapshot of the elements. */
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Doubly-linked list of JS values; element identity is stable. */
    class List<V = unknown> {
        /** Creates an empty list. */
        constructor();
        /** Rebuilds a list from its record. */
        static deserialize<T>(this: new () => List<T>, bytes: Uint8Array | ArrayBuffer): List<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => List<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): List<T>;
        /** Prepends and returns the new length. */
        pushFront(v: V): number;
        /** Appends and returns the new length. */
        pushBack(v: V): number;
        /** Removes and returns the front. */
        popFront(): V | undefined;
        /** Removes and returns the back. */
        popBack(): V | undefined;
        /** The front without removal. */
        front(): V | undefined;
        /** The back without removal. */
        back(): V | undefined;
        /** The element count. */
        readonly length: number;
        /** A head-to-tail snapshot. */
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Fixed-capacity circular buffer; push overwrites the oldest when full. */
    class RingBuffer<V = unknown> {
        /** Creates a fixed-capacity circular buffer. */
        constructor(capacity: number);
        /** Rebuilds a buffer from its record. */
        static deserialize<T>(this: new (capacity: number) => RingBuffer<T>, bytes: Uint8Array | ArrayBuffer): RingBuffer<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new (capacity: number) => RingBuffer<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): RingBuffer<T>;
        /** Appends, evicting the oldest when full; returns the count. */
        push(v: V): number;
        /** The element at index `i`. */
        get(i: number): V | undefined;
        /** The current count. */
        readonly length: number;
        /** The capacity. */
        readonly capacity: number;
        /** Whether the buffer holds `capacity` elements. */
        readonly full: boolean;
        /** The elements in insertion order. */
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Dynamic bit set backed by 64-bit words. */
    class BitSet {
        /** `nbits` is an initial capacity hint (up to 2^30 bits); the set grows on demand. `cache` (default off) keeps
         * derived acceleration state: `count` (or `cache: true`) keeps the population count up to date on set/clear/flip
         * and recounts once after and/or/xor, so `count` is O(1) instead of O(words); it costs no memory and nothing
         * measurable per write. The cache never changes results or serialize() bytes, and a deserialized or loaded set
         * starts with it off. Unknown option keys throw TypeError. */
        constructor(nbits?: number, options?: { cache?: boolean | { count?: boolean } });
        /** Rebuilds a bit set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BitSet;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): BitSet;
        /** Sets the bit; indices above the current size grow the set. Refuses indices ≥ 2^30 with `RangeError`. */
        set(i: number): this;
        /** Clears the bit. */
        clear(i: number): this;
        /** Flips the bit; indices above the current size grow the set. Refuses indices ≥ 2^30 with `RangeError`. */
        flip(i: number): this;
        /** False when the bit is past the allocated size. */
        get(i: number): boolean;
        /** The first set bit at position >= from, or -1. */
        nextSet(from: number): number;
        /** Number of set bits. */
        readonly count: number;
        /** In-place word AND; bits past `other` clear. */
        and(other: BitSet): this;
        /** In-place word OR; `this` grows as needed. */
        or(other: BitSet): this;
        /** In-place word XOR; `this` grows as needed. */
        xor(other: BitSet): this;
        /** Ascending indices of set bits. */
        toArray(): number[];
        [Symbol.iterator](): Iterator<number>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Disjoint-set forest with path halving and union by rank. */
    class UnionFind {
        /** Elements are 0..n-1 (n up to 2^26). */
        constructor(n?: number);
        /** Rebuilds a forest from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): UnionFind;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): UnionFind;
        /** The representative of the set containing x. */
        find(x: number): number;
        /** Merges the sets of x and y; false when they were already joined. */
        union(x: number, y: number): boolean;
        /** Whether both elements are in the same component. */
        connected(x: number, y: number): boolean;
        /** Number of disjoint components. */
        readonly count: number;
        /** Element count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Fenwick tree over a fixed-size vector of doubles. */
    class Fenwick {
        /** Creates a Fenwick tree. */
        constructor(n: number);
        /** Rebuilds a tree from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Fenwick;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Fenwick;
        /** Adds `delta` to position `i`. Out-of-range throws `RangeError`. */
        update(i: number, delta: number): this;
        /** Sum of positions [0..i] inclusive. */
        prefixSum(i: number): number;
        /** Sum of [lo..hi] inclusive; 0 for an empty range. */
        rangeQuery(lo: number, hi: number): number;
        /** The number of positions. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Iterative segment tree with an associative fold: "sum"|"min"|"max". */
    class SegTree {
        /** Creates a segment tree. Defaults: op "sum". */
        constructor(n: number, op?: SegOp);
        /** Rebuilds a tree from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): SegTree;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): SegTree;
        /** Assigns leaf `i`, re-folding the path to the root. */
        update(i: number, value: number): this;
        /** The fold over `[lo..hi]`; the identity for an empty (`lo > hi`) range. */
        rangeQuery(lo: number, hi: number): number;
        /** The number of leaves. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Probabilistic set membership; no false negatives. */
    class BloomFilter {
        /** `bits` up to 2^30; `hashes` defaults to 3 (clamped to 1..32). */
        constructor(bits: number, hashes?: number);
        /** Rebuilds a filter from its record; accepts every size the constructor does. A record claiming more than 2^30 bits, or carrying fewer words than it claims, is a TypeError. The sparse encoding stops at 2^22 words (a tiny record must not decode to hundreds of MB); serialize writes larger filters raw, 8 bytes per word. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BloomFilter;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): BloomFilter;
        /** Sets the key's `hashes` bits. */
        add(key: string): this;
        /** False means definitely absent; true means probably present. */
        mayContain(key: string): boolean;
        /** The number of bits. */
        readonly bits: number;
        /** The number of hashes. */
        readonly hashes: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Set of byte strings with prefix queries. */
    class Trie {
        /** Creates an empty trie. */
        constructor();
        /** Rebuilds a trie from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Trie;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Trie;
        /** Stores the key; re-inserting an existing key is a no-op. */
        insert(key: string): this;
        /** Whether the key is present. */
        has(key: string): boolean;
        /** Removes the key, reporting whether it was present; the branch the key leaves empty is unlinked (its node memory is kept until the trie is freed), so every reachable node still leads to a stored key. */
        delete(key: string): boolean;
        /** Every stored key starting with `prefix`. Ascending order is *not* guaranteed (sibling insertion order). */
        keysWithPrefix(prefix: string): string[];
        /** Whether some stored key starts with `prefix` (UTF-8 bytes); "" is true for a non-empty trie. One walk of O(len prefix), allocating nothing and building no key list, unlike keysWithPrefix(prefix).length > 0. */
        hasPrefix(prefix: string): boolean;
        /** The longest stored key that is a prefix of str, or "". */
        longestPrefix(str: string): string;
        /** The number of stored keys. */
        readonly size: number;
        [Symbol.iterator](): Iterator<string>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** String key to uint64 count, saturating at 2^64-1; counts above 2^53-1 read back as the nearest double. */
    class Multiset {
        /** Creates an empty multiset. */
        constructor();
        /** Rebuilds a multiset from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Multiset;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Multiset;
        /** The key's new count. Defaults: n 1. */
        add(key: string, n?: number): number;
        /** Subtracts, returning the new count (flooring at 0; the record is dropped at 0). Defaults: n 1. */
        remove(key: string, n?: number): number;
        /** The key's count. */
        count(key: string): number;
        /** Whether the key is present. */
        has(key: string): boolean;
        /** Sets the count exactly. */
        setCount(key: string, count: number): this;
        /** Removes the key entirely, reporting whether it was present. */
        delete(key: string): boolean;
        /** Drops all entries. */
        clear(): void;
        /** The distinct keys. */
        elementSet(): string[];
        /** The keys with their counts. */
        entrySet(): [string, number][];
        /** Distinct key count. */
        readonly size: number;
        /** Sum of all counts. */
        readonly totalSize: number;
        [Symbol.iterator](): Iterator<[string, number]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** String key to a growing value array. */
    class Multimap<V = unknown> {
        /** Creates an empty multimap. */
        constructor();
        /** Rebuilds a multimap from its record. */
        static deserialize<T>(this: new () => Multimap<T>, bytes: Uint8Array | ArrayBuffer): Multimap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => Multimap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): Multimap<T>;
        /** Appends one value. */
        put(key: string, value: V): this;
        /** A fresh array of all values for the key (empty if none). */
        get(key: string): V[];
        /** The number of values for the key. */
        count(key: string): number;
        /** Removes every value for the key; returns how many. */
        delete(key: string): number;
        /** Removes the value at `index`. Both arguments required (`TypeError` otherwise). */
        removeAt(key: string, index: number): V | undefined;
        /** The distinct keys. */
        keys(): string[];
        /** Flattened pairs. */
        entries(): [string, V][];
        /** Total values. */
        readonly size: number;
        /** Distinct keys. */
        readonly keyCount: number;
        [Symbol.iterator](): Iterator<[string, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Two-way string-to-string map. */
    class BiMap {
        /** Creates an empty bimap. */
        constructor();
        /** Rebuilds a bimap from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BiMap;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): BiMap;
        /** Throws `TypeError` if the value is already bound to another key. */
        set(key: string, value: string): this;
        /** Sets the pair, evicting any entry that already holds the value. */
        forceSet(key: string, value: string): this;
        /** Forward lookup. */
        get(key: string): string | undefined;
        /** The key mapped to `value` (the inverse lookup). */
        keyOf(value: string): string | undefined;
        /** Whether the key is bound. */
        has(key: string): boolean;
        /** Whether the value is bound. */
        hasValue(value: string): boolean;
        /** Drops the key and its value, reporting whether it was present. */
        delete(key: string): boolean;
        /** Drops the value and its key, reporting whether it was present. */
        deleteValue(value: string): boolean;
        /** The forward pairs. */
        entries(): [string, string][];
        /** The inverse pairs. */
        inverseEntries(): [string, string][];
        /** Drops all pairs. */
        clear(): void;
        /** The number of pairs. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, string]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Sparse two-dimensional string-to-string-to-value map. */
    class Table<V = unknown> {
        /** Creates an empty table. */
        constructor();
        /** Rebuilds a table from its record. */
        static deserialize<T>(this: new () => Table<T>, bytes: Uint8Array | ArrayBuffer): Table<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => Table<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): Table<T>;
        /** Sets a cell, replacing the old value. */
        put(row: string, col: string, value: V): this;
        /** The cell value. */
        get(row: string, col: string): V | undefined;
        /** Whether the cell is present. */
        has(row: string, col: string): boolean;
        /** Removes a cell, reporting whether it was present. */
        delete(row: string, col: string): boolean;
        row(r: string): [string, V][];
        /** The column's cells; a linear scan of the sparse cell array. */
        column(c: string): [string, V][];
        /** Every occupied cell. */
        cells(): [string, string, V][];
        /** The cell count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, string, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** A set of numeric intervals [lo, hi), kept disjoint and merged on insert. */
    class RangeSet {
        /** Creates an empty range set. */
        constructor();
        /** Rebuilds a range set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): RangeSet;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): RangeSet;
        /** Unions an interval; overlapping neighbours merge as needed. NaN bounds throw `RangeError`. */
        add(lo: number, hi: number): this;
        /** Subtracts an interval; overlapping neighbours split as needed. */
        remove(lo: number, hi: number): this;
        /** Whether the point is covered. */
        contains(x: number): boolean;
        /** True when [lo, hi) lies entirely inside the set. */
        encloses(lo: number, hi: number): boolean;
        /** Whether the interval overlaps any stored range. */
        intersects(lo: number, hi: number): boolean;
        /** The disjoint ranges in order. */
        ranges(): [number, number][];
        /** The gaps of [lo, hi) outside the set. */
        complement(lo: number, hi: number): [number, number][];
        /** Drops all ranges. */
        clear(): void;
        /** The number of disjoint ranges. */
        readonly size: number;
        /** The total covered length. */
        readonly measure: number;
        [Symbol.iterator](): Iterator<[number, number]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Map from numeric intervals [lo, hi) to values; overlapping puts split. */
    class RangeMap<V = unknown> {
        /** Creates an empty range map. */
        constructor();
        /** Rebuilds a range map from its record. */
        static deserialize<T>(this: new () => RangeMap<T>, bytes: Uint8Array | ArrayBuffer): RangeMap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => RangeMap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): RangeMap<T>;
        /** Assigns the whole span; an empty range (`hi <= lo`) stores nothing. */
        put(lo: number, hi: number, value: V): this;
        /** The value of the range containing `x`, or `undefined` if uncovered. */
        get(x: number): V | undefined;
        /** Clears a span, splitting neighbour ranges. */
        remove(lo: number, hi: number): this;
        /** The stored ranges in order. */
        entries(): [number, number, V][];
        /** The number of stored ranges. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[number, number, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Interval store with overlap enumeration ([lo, hi) spans). */
    class IntervalTree<V = unknown> {
        /** Creates an empty interval tree. */
        constructor();
        /** Rebuilds an interval tree from its record. */
        static deserialize<T>(this: new () => IntervalTree<T>, bytes: Uint8Array | ArrayBuffer): IntervalTree<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => IntervalTree<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): IntervalTree<T>;
        /** `lo` must not exceed `hi`; an inverted pair or a NaN bound is a RangeError. */
        insert(lo: number, hi: number, value: V): this;
        /** Entries whose interval overlaps [lo, hi); at(x) returns those containing x. */
        overlapping(lo: number, hi: number): [number, number, V][];
        /** The degenerate point query `[x, x]`. */
        at(x: number): [number, number, V][];
        /** The interval count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Count-min sketch: depth rows of width saturating counters. */
    class CountMinSketch {
        /** `depth` defaults to 5 (at most 64); width * depth must not exceed 2^24 counters. */
        constructor(width: number, depth?: number);
        /** Rebuilds a sketch from its record; accepts every size the constructor does. A record claiming more than 2^24 counters, or carrying fewer counters than it claims, is a TypeError. The sparse encoding stops at 2^22 counters (a tiny record must not decode to hundreds of MB); serialize writes larger sketches raw, 8 bytes per counter. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): CountMinSketch;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): CountMinSketch;
        /** Counts `n` occurrences. Defaults: n 1. */
        add(key: string, n?: number): this;
        /** The estimate for the key; an overestimate (an upper bound). */
        count(key: string): number;
        /** Adds another sketch of the same shape into this one. */
        merge(other: CountMinSketch): this;
        /** The number of counters per row. */
        readonly width: number;
        /** The number of rows. */
        readonly depth: number;
        /** The total number of occurrences added. */
        readonly totalCount: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Cardinality estimator over string keys. */
    class HyperLogLog {
        /** `precision` is 4..18, default 14 (16 KiB of registers). */
        constructor(precision?: number);
        /** Rebuilds an estimator from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): HyperLogLog;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): HyperLogLog;
        /** Folds the key's hash into one register. */
        add(key: string): this;
        /** The cardinality estimate. */
        count(): number;
        /** Unions the registers. Requires equal precision (else `TypeError`). */
        merge(other: HyperLogLog): this;
        /** The exponent: the estimator holds `2^precision` registers. */
        readonly precision: number;
        /** The number of registers (`2^precision`). */
        readonly registers: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Redis-style sorted set: unique string members ordered by (score, member bytes), with O(log n) rank queries. */
    class ZSet {
        /** Creates an empty sorted set. */
        constructor();
        /** Rebuilds a sorted set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): ZSet;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): ZSet;
        /** Sets the member's score; true when the member is new. A NaN score is a TypeError. */
        add(member: string, score: number): boolean;
        /** Adds `delta` to the score (a missing member starts at 0) and returns the new score. */
        incrBy(member: string, delta: number): number;
        /** The member's score, or undefined. */
        score(member: string): number | undefined;
        /** Whether the member is present. */
        has(member: string): boolean;
        /** Removes the member; false when it was absent. */
        delete(member: string): boolean;
        /** The 0-based position in ascending order, or undefined. */
        rank(member: string): number | undefined;
        /** The 0-based position in descending order, or undefined. */
        revRank(member: string): number | undefined;
        /** The entry at a 0-based rank; a negative index counts from the end. */
        at(index: number): [string, number] | undefined;
        /** Entries from rank `start` to `stop` inclusive (negatives count from the end, as ZRANGE). */
        range(start: number, stop: number, opts?: { reverse?: boolean }): [string, number][];
        /** Every entry in ascending order. */
        entries(): [string, number][];
        /** Entries with min <= score <= max; `reverse` walks from max down. */
        rangeByScore(min: number, max: number, opts?: { offset?: number; limit?: number; reverse?: boolean }): [string, number][];
        /** The number of members with min <= score <= max, without materialising them. */
        countByScore(min: number, max: number): number;
        /** Removes members with min <= score <= max and returns how many. */
        removeRangeByScore(min: number, max: number): number;
        /** Removes ranks `start`..`stop` inclusive (ZREMRANGEBYRANK) and returns how many. */
        removeRangeByRank(start: number, stop: number): number;
        /** Removes and returns the lowest entry. */
        popMin(): [string, number] | undefined;
        /** Removes and returns the highest entry. */
        popMax(): [string, number] | undefined;
        /** Removes every member. */
        clear(): void;
        /** The member count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, number]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Sorted map over string keys in UTF-8 byte order (code-point order), with O(1) get and O(log n) seeks and ranks. */
    class OrderedMap<V = unknown> {
        /** Creates an empty map. */
        constructor();
        /** Rebuilds a map from its record. */
        static deserialize<T>(this: new () => OrderedMap<T>, bytes: Uint8Array | ArrayBuffer): OrderedMap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => OrderedMap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): OrderedMap<T>;
        /** Inserts or replaces the value for `key`. */
        set(key: string, value: V): this;
        /** The value for `key`, or undefined. */
        get(key: string): V | undefined;
        /** Whether `key` is present. */
        has(key: string): boolean;
        /** Removes `key`; false when it was absent. */
        delete(key: string): boolean;
        /** The smallest key. */
        firstKey(): string | undefined;
        /** The largest key. */
        lastKey(): string | undefined;
        /** The largest key <= `key`. */
        floorKey(key: string): string | undefined;
        /** The smallest key >= `key`. */
        ceilKey(key: string): string | undefined;
        /** The largest key < `key`. */
        lowerKey(key: string): string | undefined;
        /** The smallest key > `key`. */
        higherKey(key: string): string | undefined;
        /** The number of keys < `key`. */
        rank(key: string): number;
        /** The entry at a 0-based position; a negative index counts from the end. */
        at(index: number): [string, V] | undefined;
        /** Entries with lo <= key <= hi; `reverse` walks from hi down. */
        rangeQuery(lo: string, hi: string, opts?: { limit?: number; reverse?: boolean }): [string, V][];
        /** Entries whose key starts with `prefix`, in key order. */
        prefix(prefix: string, opts?: { limit?: number }): [string, V][];
        /** Removes keys with lo <= key <= hi and returns how many. */
        deleteRange(lo: string, hi: string): number;
        /** Every key in order. */
        keys(): string[];
        /** Every entry in key order. */
        entries(): [string, V][];
        /** Removes every key. */
        clear(): void;
        /** The key count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Compressed set of uint32 values (array, bitmap and full 2^16 containers); set algebra runs in C per 64 Ki chunk. */
    class RoaringBitmap {
        /** Creates a bitmap, optionally from integers in 0..2^32-1. */
        constructor(values?: ArrayLike<number> | Uint32Array | Int32Array);
        /** Rebuilds a bitmap from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): RoaringBitmap;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): RoaringBitmap;
        /** Adds `x` (an integer in 0..2^32-1). */
        add(x: number): this;
        /** Adds `x`; true when it was not already present. */
        tryAdd(x: number): boolean;
        /** Adds every value; the input is sorted first, so any order is fine. */
        addMany(values: ArrayLike<number> | Uint32Array | Int32Array): this;
        /** Adds the half-open range [lo, hi); bounds are integers in 0..2^32. */
        addRange(lo: number, hi: number): this;
        /** Removes the half-open range [lo, hi). */
        removeRange(lo: number, hi: number): this;
        /** Removes `x`; false when it was absent. */
        delete(x: number): boolean;
        /** Whether `x` is present. */
        has(x: number): boolean;
        /** The number of values <= `x`. */
        rank(x: number): number;
        /** The value at a 0-based position in ascending order. */
        select(i: number): number | undefined;
        /** The number of values in [lo, hi). */
        rangeCardinality(lo: number, hi: number): number;
        /** Intersects in place. */
        and(other: RoaringBitmap): this;
        /** Unions in place. */
        or(other: RoaringBitmap): this;
        /** Symmetric difference in place. */
        xor(other: RoaringBitmap): this;
        /** Removes every value of `other` in place. */
        andNot(other: RoaringBitmap): this;
        /** |this AND other| without building the result. */
        andCardinality(other: RoaringBitmap): number;
        /** |this OR other| without building the result. */
        orCardinality(other: RoaringBitmap): number;
        /** |this XOR other| without building the result. */
        xorCardinality(other: RoaringBitmap): number;
        /** |this AND NOT other| without building the result. */
        andNotCardinality(other: RoaringBitmap): number;
        /** Whether the two share a value. */
        intersects(other: RoaringBitmap): boolean;
        /** Whether every value is also in `other`. */
        isSubsetOf(other: RoaringBitmap): boolean;
        /** Whether both hold the same values. */
        equals(other: RoaringBitmap): boolean;
        /** |A AND B| / |A OR B|; 1 for two empty bitmaps. */
        jaccard(other: RoaringBitmap): number;
        /** The smallest value. */
        min(): number | undefined;
        /** The largest value. */
        max(): number | undefined;
        /** The values in ascending order; more than 2^28 values is a RangeError. */
        toArray(): Uint32Array;
        /** An independent copy. */
        clone(): RoaringBitmap;
        /** Removes every value. */
        clear(): void;
        /** Whether no value is present. */
        isEmpty(): boolean;
        /** The number of values (up to 2^32). */
        readonly size: number;
        /** Approximate bytes held by the containers. */
        readonly bytes: number;
        [Symbol.iterator](): Iterator<number>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Merging t-digest (k1 scale): streaming quantiles whose error shrinks toward the tails. */
    class TDigest {
        /** `compression` (delta, 10..10000) defaults to 100; about 1.6 * delta centroids are kept. */
        constructor(compression?: number);
        /** Rebuilds a digest from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): TDigest;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): TDigest;
        /** Adds a finite value with a positive weight (default 1). */
        add(x: number, weight?: number): this;
        /** Adds every value of an array or a typed array; a non-finite value is a RangeError. */
        addMany(values: ArrayLike<number> | Float64Array | Float32Array): this;
        /** The estimated value at quantile q in 0..1; NaN when empty. Reads (quantile, quantiles, cdf, centroids, merge's source, serialize) never flush the insert buffer, so a digest that was queried or checkpointed estimates exactly like one that was not. */
        quantile(q: number): number;
        /** quantile() for each q, in one call. */
        quantiles(qs: ArrayLike<number> | Float64Array): Float64Array;
        /** The estimated fraction of weight <= x; NaN when empty. */
        cdf(x: number): number;
        /** Adds another digest's centroids (and its unflushed values) into this one; `other` is left unchanged. */
        merge(other: TDigest): this;
        /** The total weight added. */
        readonly count: number;
        /** The smallest value added, or undefined. */
        readonly min: number | undefined;
        /** The largest value added, or undefined. */
        readonly max: number | undefined;
        /** The exact weighted mean, or undefined. */
        readonly mean: number | undefined;
        /** The centroid count a compression of the current contents yields. */
        readonly centroids: number;
        /** The delta given to the constructor. */
        readonly compression: number;
        /** The contents as a compact type-tagged record, insert buffer included: deserialize() restores the exact state, so later estimates match a digest that was never serialized. Records from earlier versions (no buffer) still load. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Space-Saving heavy hitters: k counters; any key seen more than total/k times is guaranteed to be listed. */
    class TopK {
        /** `k` is 1..2^20. */
        constructor(k: number);
        /** Rebuilds a summary from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): TopK;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): TopK;
        /** Counts `n` (default 1) occurrences; returns the key it evicted, if any. */
        add(key: string, n?: number): string | undefined;
        /** The estimated count (an upper bound), or 0 when untracked. */
        count(key: string): number;
        /** Whether the key holds a counter. */
        has(key: string): boolean;
        /** The overestimate bound for a tracked key; count - error is a lower bound. */
        error(key: string): number | undefined;
        /** [key, count, error] for every counter, highest count first (ties by key). */
        list(): [string, number, number][];
        /** The counter capacity. */
        readonly k: number;
        /** The counters in use. */
        readonly size: number;
        /** The total of every count added. */
        readonly total: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Cuckoo filter (4-slot buckets, 16-bit fingerprints): membership with deletion; false positives about 0.01%. */
    class CuckooFilter {
        /** Sized for `capacity` keys (1..2^26) at a 95% load factor. */
        constructor(capacity: number);
        /** Rebuilds a filter from its record (hashing is fixed, so records move between processes). */
        static deserialize(bytes: Uint8Array | ArrayBuffer): CuckooFilter;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): CuckooFilter;
        /** Inserts the key; false when the filter is full. */
        add(key: string): boolean;
        /** False means definitely absent; true means probably present. */
        mayContain(key: string): boolean;
        /** Removes one copy of a key that was added; deleting a never-added key can remove a colliding one. */
        delete(key: string): boolean;
        /** The number of keys held. */
        readonly size: number;
        /** The slot count. */
        readonly capacity: number;
        /** size / capacity. */
        readonly loadFactor: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Binary heap keyed by uint32 id: change any id's priority in O(log n) (Dijkstra, schedulers, timers). */
    class IndexedHeap {
        /** A min-heap by default; `max: true` pops the highest priority first. Ties pop the smaller id. */
        constructor(opts?: { max?: boolean });
        /** Rebuilds a heap from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): IndexedHeap;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): IndexedHeap;
        /** Inserts `id` or moves it to the new priority. */
        push(id: number, priority: number): this;
        /** push() for every pair; the two lists must be the same length. */
        pushMany(ids: ArrayLike<number> | Uint32Array | Int32Array, priorities: ArrayLike<number> | Float64Array): this;
        /** The id's priority, or undefined. */
        priority(id: number): number | undefined;
        /** Whether the id is queued. */
        has(id: number): boolean;
        /** Removes the id; false when it was absent. */
        delete(id: number): boolean;
        /** Removes and returns the top id. */
        pop(): number | undefined;
        /** Removes and returns the top [id, priority]. */
        popEntry(): [number, number] | undefined;
        /** Pops every id whose priority is <= limit (>= for a max-heap), at most `max` of them. */
        popUntil(limit: number, max?: number): Uint32Array;
        /** The top id without removing it. */
        peek(): number | undefined;
        /** The top priority. */
        peekPriority(): number | undefined;
        /** Removes every id. */
        clear(): void;
        /** The queued id count. */
        readonly size: number;
        /** Whether this is a max-heap. */
        readonly isMax: boolean;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Static k-d tree over finite points (1..64 dims, Euclidean); queries return point indices in input order. */
    class KDTree {
        /** From number[][], or from a flat array / typed array of n*dims coordinates with `dims`. */
        constructor(points: number[][] | ArrayLike<number> | Float64Array | Float32Array, dims?: number);
        /** Rebuilds a tree from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): KDTree;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): KDTree;
        /** The k nearest points, nearest first (ties by index). */
        knn(query: ArrayLike<number> | Float64Array, k: number): { indices: Int32Array; distances: Float64Array };
        /** The index of the nearest point, or -1 when empty. */
        nearest(query: ArrayLike<number> | Float64Array): number;
        /** knn() for m flat queries in one call; row i of the result is entries i*k..i*k+k-1 (k capped at size). */
        knnMany(queries: ArrayLike<number> | Float64Array, k: number): { indices: Int32Array; distances: Float64Array };
        /** Points within `r` of the query, nearest first. */
        radius(query: ArrayLike<number> | Float64Array, r: number): { indices: Int32Array; distances: Float64Array };
        /** Indices of points inside the closed box [lo, hi] per dimension, ascending. */
        box(lo: ArrayLike<number> | Float64Array, hi: ArrayLike<number> | Float64Array): Int32Array;
        /** The coordinates of point i. */
        point(i: number): Float64Array | undefined;
        /** The point count. */
        readonly size: number;
        /** The dimension count. */
        readonly dims: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Options of `new BKDTree` / `BKDTree.build`. */
    interface BKDTreeOptions {
        /** Value type (default "f64"). "f64": numbers ordered like Lucene's NumericUtils.doubleToSortableLong: -Infinity < ... < -0 < +0 < ... < +Infinity < NaN, every NaN one key. "i64": signed 64-bit integers given as bigint or integer numbers (BigInt64Array accepted); a value outside the int64 range or a fractional number is a RangeError. "ip": 1-d, IPv4 or IPv6 address strings stored as 16-byte keys in IPv6 order, an IPv4 address as its mapped form ::ffff:a.b.c.d (Lucene InetAddressPoint), so "1.2.3.4" and "::ffff:1.2.3.4" are one key; zone ids (%eth0) are a RangeError. "latlon": 2-d [lat, lon] in degrees as exact float64 (not Lucene's int32 quantization), latitude in [-90, 90] and longitude in [-180, 180], else RangeError. */
        type?: "f64" | "i64" | "ip" | "latlon";
        /** Dimensions per point, 1..4. Defaults to the row length for number[][] rows, else 1. A flat list must be a multiple of dims. */
        dims?: number;
        /** The uint32 document id of each point (default: the point's index). Ids may repeat (multi-valued documents). */
        docIds?: ArrayLike<number> | Uint32Array;
        /** Points per leaf block, 2..65536 (default 512). */
        leafSize?: number;
    }

    /** Immutable block k-d tree over 1..4-dimensional numeric points, IP addresses and lat/lon points (Lucene PointValues / BKD). The build splits on the
     *  widest dimension at the median into a balanced tree of leaves of at most leafSize points; inner split values live
     *  in one flat array. A query classifies each cell against the box: a cell inside the box adds all its doc ids in
     *  bulk with no per-point test, a crossing leaf tests each point, and a cell outside is skipped. Results are
     *  RoaringBitmap sets of doc ids. Bounds are inclusive in the type's key order; a bound with lo > hi on any
     *  dimension matches nothing. Queries run synchronously on the calling thread; `BKDTree.build` runs the build on the
     *  shared io pool and settles on the loop. */
    class BKDTree {
        /** Builds from number[] (1-d), number[][] rows, a flat array / typed array with `dims`, for "i64" a BigInt64Array or (bigint | number)[], for "ip" string[], for "latlon" [lat, lon][] or a flat [lat0, lon0, lat1, ...]. An empty list gives an empty tree. Synchronous: O(n log n) on the calling thread. */
        constructor(values: ArrayLike<number> | ArrayLike<ArrayLike<number>> | Float64Array | Float32Array | BigInt64Array | ArrayLike<bigint | number> | ArrayLike<ArrayLike<bigint | number>>, opts?: BKDTreeOptions);
        /** Same as the constructor, but the inputs are read on the loop and the O(n log n) build runs on the io pool; input errors reject the promise. */
        static build(values: ArrayLike<number> | ArrayLike<ArrayLike<number>> | Float64Array | Float32Array | BigInt64Array | ArrayLike<bigint | number> | ArrayLike<ArrayLike<bigint | number>>, opts?: BKDTreeOptions): Promise<BKDTree>;
        /** Doc ids of points with lo[d] <= p[d] <= hi[d] on every dimension (a box when dims > 1). For dims 1 the bounds may be scalars. For "i64", -Infinity / Infinity bounds mean the int64 extremes. */
        range(lo: number | bigint | ArrayLike<number | bigint>, hi: number | bigint | ArrayLike<number | bigint>): RoaringBitmap;
        /** The number of matching POINTS (not distinct doc ids) of range(lo, hi), without building the bitmap. */
        rangeCount(lo: number | bigint | ArrayLike<number | bigint>, hi: number | bigint | ArrayLike<number | bigint>): number;
        /** ("ip" trees) Doc ids of addresses inside the CIDR prefix, bounds inclusive: an IPv4 prefix a.b.c.d/n is the IPv6 range ::ffff:a.b.c.d/(96+n), so "0.0.0.0/0" matches every IPv4 address and no other IPv6 one, and "::/0" matches everything. Host bits are masked ("1.2.3.7/30" is 1.2.3.4..1.2.3.7). A non-string prefix is a TypeError; a malformed one (no /n, n above 32 or 128, a leading zero, a zone id) is a RangeError. Another tree type is a TypeError. */
        cidr(prefix: string): RoaringBitmap;
        /** ("latlon" trees) Doc ids of points with minLat <= lat <= maxLat and minLon <= lon <= maxLon, all inclusive. minLon > maxLon crosses the antimeridian (lon >= minLon or lon <= maxLon), as in Lucene LatLonPoint.newBoxQuery; minLat > maxLat matches nothing. Out-of-range degrees are a RangeError. */
        box(minLat: number, maxLat: number, minLon: number, maxLon: number): RoaringBitmap;
        /** ("latlon" trees) Doc ids of points within `meters` (finite, >= 0) of (lat, lon) by the haversine formula on a sphere of R = 6371008.7714 m: h = sin((p2-p1)/2)^2 + cos(p1) cos(p2) sin((l2-l1)/2)^2 with angles in radians (deg * PI / 180), d = 2 R asin(sqrt(min(1, h))), match when d <= meters. The formula is exact in float64 as written, so two spellings of one place (a pole at two longitudes, lon 180 and -180) are about 1e-9 m apart and radius 0 matches only the exact coordinates. Cells are pruned by the circle's bounding box (split at the antimeridian, all longitudes when it covers a pole); a cell is taken whole only when its four corners are within the radius and its longitudes lie within 90 degrees of the center, otherwise every point is checked. meters >= PI * R matches every point. */
        distance(lat: number, lon: number, meters: number): RoaringBitmap;
        /** The point count. */
        readonly size: number;
        /** The dimension count. */
        readonly dims: number;
        /** The value type. */
        readonly type: "f64" | "i64" | "ip" | "latlon";
        /** Points per leaf block. */
        readonly leafSize: number;
        /** The tree as a compact type-tagged record (type id 51): the built tree itself (split dims, split values, leaf-ordered keys and doc ids), so deserialize does not rebuild. */
        serialize(): Uint8Array;
        /** Rebuilds a tree from its record (record version 1; records written before "ip" and "latlon" existed load unchanged). The record is validated: header fields, split dims, every point inside its leaf cell and latlon degrees in range, so a forged record is a TypeError or a tree whose answers match a scan of its points. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BKDTree;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): BKDTree;
    }

    /** Consistent-hash ring: replica r of node n sits at XXH64(UTF-8 "n#r", seed 0), a key at XXH64(key, 0); the owner is the
     *  first point at or after the key, wrapping (ties by name), so any language with xxHash64 can place keys identically. */
    class ConsistentHash {
        /** `replicas` (virtual nodes per unit of weight, 1..1000) defaults to 160. `cache` (default off): `jump` (or
         * `cache: true`) keeps a table indexed by the top bits of the key hash (about 8 ring points per bucket, 0.5 byte
         * per point) so get/getN search one bucket: about 1.4x faster at 10K points, 2x at 1M. A ring change marks the
         * table stale and the next lookup rebuilds it (O(points)), so a change followed by a lookup costs about 1.3-1.5x
         * more. Routing never changes; a loaded or deserialized ring starts with it off. Unknown keys inside `cache`
         * throw TypeError. */
        constructor(opts?: { replicas?: number; cache?: boolean | { jump?: boolean } });
        /** Rebuilds a ring from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): ConsistentHash;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): ConsistentHash;
        /** Jump consistent hash (Lamping & Veach 2014) of XXH64(key, 0): a bucket in 0..buckets-1 that moves minimally as buckets grow. */
        static jump(key: string, buckets: number): number;
        /** Adds a node or changes its weight (1..1000, default 1). Only this node's points are hashed and merged into the sorted ring: O(ring) per call, routing identical to a full rebuild; a change whose ring would pass 4,194,304 points throws and leaves the ring as it was. */
        addNode(name: string, weight?: number): this;
        /** Removes a node; false when it was absent. O(ring): its points are filtered out of the sorted ring, nothing is re-hashed. */
        removeNode(name: string): boolean;
        /** Whether the node is on the ring. */
        hasNode(name: string): boolean;
        /** The node owning the key, or undefined when the ring is empty. */
        get(key: string): string | undefined;
        /** Up to n distinct nodes clockwise from the key (replica placement). */
        getN(key: string, n: number): string[];
        /** The node names. */
        nodes(): string[];
        /** The node count. */
        readonly size: number;
        /** Virtual nodes per unit of weight. */
        readonly replicas: number;
        /** The ring point count. */
        readonly points: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** BM25 full-text index over uint32 document ids; tokens are runs of letters, digits, `_` and non-ASCII. */
    class InvertedIndex {
        /** BM25 defaults: k1 1.2, b 0.75. Stop words go through the same tokenizer. */
        constructor(opts?: { k1?: number; b?: number; stopWords?: string[] });
        /** Rebuilds an index from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): InvertedIndex;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): InvertedIndex;
        /** The tokens the index would store for `text` (ASCII lower-cased, at most 128 bytes each). */
        static tokenize(text: string): string[];
        /** Indexes `text` under `id`, replacing an earlier document with that id. */
        add(id: number, text: string): this;
        /** Removes a document; false when it was absent. */
        remove(id: number): boolean;
        /** Whether the document is indexed. */
        has(id: number): boolean;
        /** The document's token count, or undefined. */
        docLength(id: number): number | undefined;
        /** [id, score] best first (ties by id). Defaults: mode "or", limit 10; `prefix` expands the last term. */
        search(query: string, opts?: { mode?: "and" | "or"; limit?: number; prefix?: boolean }): [number, number][];
        /** The number of documents containing the term. */
        docFreq(term: string): number;
        /** [term, docFreq] for indexed terms starting with `prefix`, most frequent first. Defaults: limit 10. */
        suggest(prefix: string, limit?: number): [string, number][];
        /** The document count. */
        readonly size: number;
        /** The number of terms that appear in some document. */
        readonly termCount: number;
        /** The mean document length in tokens. */
        readonly avgLength: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** String interner: dense uint32 ids in first-seen order (dictionary encoding), strings kept in arena chunks. */
    class Interner {
        /** Creates an empty interner. */
        constructor();
        /** Rebuilds an interner from its record; ids are preserved. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Interner;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Interner;
        /** The id for `s`, assigning the next id when new. */
        intern(s: string): number;
        /** The id for `s`, or undefined without assigning one. */
        lookup(s: string): number | undefined;
        /** Whether `s` has an id. */
        has(s: string): boolean;
        /** The string for an id, or undefined. */
        get(id: number): string | undefined;
        /** intern() for every string, in one call. */
        internMany(strings: string[]): Uint32Array;
        /** get() for every id; unknown ids give undefined. */
        getMany(ids: ArrayLike<number> | Uint32Array | Int32Array): (string | undefined)[];
        /** The number of distinct strings. */
        readonly size: number;
        /** Bytes of string storage. */
        readonly bytes: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }
}
