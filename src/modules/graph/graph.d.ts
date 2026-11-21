/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:graph
/** dyna:graph — integer-id graphs, edge types 0..31; paths, PageRank, analytics on CSR snapshots.
 *  Persists via serialize()/save()/load() like dyna:structures. */
declare module "dyna:graph" {
    type PersistSaveOptions = import("dyna:structures").PersistSaveOptions;
    type PersistLoadOptions = import("dyna:structures").PersistLoadOptions;
    type PersistSaveResult = import("dyna:structures").PersistSaveResult;

    /** Adjacency-list graph over integer node ids. Traversals and path methods run on a CSR snapshot taken at the call, so a
     *  visitor or heuristic that mutates the graph is reported (TypeError) while the walk itself stays on the snapshot; a graph
     *  with more than 2^32-1 stored arcs (an undirected edge stores two, a self loop one) is a RangeError for them.
     *  TYPED EDGES: every edge carries a type id, an integer 0..31 (an edge added without one is type 0; an undirected edge has its type on both arcs;
     *  parallel edges may differ in type). A GraphTypeMask selects types: bit t (mask & (1 << t)) admits type t, 0 admits none, -1 (or 0xFFFFFFFF) all.
     *  Traversals given `types` follow only admitted arcs (the source itself is always visited); without `types` they behave exactly as before.
     *  A graph whose edges are all type 0 stores, exports and serializes exactly as an untyped graph (no extra memory, same record bytes);
     *  once an edge of a nonzero type exists, exportCSR adds `types` and serialize() writes the typed record (one type byte per arc),
     *  which older builds reject as truncated. Option bags are strict (TypeError naming the valid keys). */
    class Graph {
        /** Creates a graph; `directed` and `weighted` are fixed at construction. */
        constructor(opts?: { directed?: boolean; weighted?: boolean });
        /** Rebuilds a graph from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Graph;
        /** Reads a file written by save(); see the dyna:structures module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Graph;
        /** Builds a graph from parallel edge arrays in one call: edge i is src[i] -> dst[i] (both directions when undirected), added in index order, so
         *  the result equals that sequence of addEdge calls. `weights` needs `weighted: true`; `types` holds type ids 0..31; every given array has the
         *  same length (TypeError). nodeCount is max(opts.nodes, largest id + 1). A node id outside [0, 2^26), a non-finite weight or a type > 31 is a
         *  RangeError and builds nothing. `directed`/`weighted` must be booleans. */
        static fromEdges(src: Int32Array, dst: Int32Array, opts?: { directed?: boolean; weighted?: boolean; weights?: Float64Array; types?: Uint8Array; nodes?: number }): Graph;
        /** Adds a node and returns its id; a graph holds at most 2^26 nodes (RangeError past that). */
        addNode(): number;
        /** Adds an edge (both directions when undirected); nodes grow on demand up to 2^26 (a larger id is a RangeError); `w` is honoured only on a `{ weighted: true }` graph, else 1.0; `type` is the edge's type id, an integer 0..31 (default 0; RangeError outside, TypeError for a non-number). */
        addEdge(u: number, v: number, w?: number, type?: number): this;
        /** Targets of u's outgoing edges. */
        neighbors(u: number): number[];
        /** Writes u's out-neighbors into `out` and returns the degree; RangeError (nothing written) when `out` is too short. Never allocates. */
        neighborsInto(u: number, out: Int32Array): number;
        /** The adjacency as CSR in fresh arrays: edges[offsets[u] .. offsets[u+1]] are u's neighbors in insertion order (an undirected edge appears under both ends, a self loop once); `weights` (weighted graphs only) and `types` (only once an edge has a nonzero type) are aligned with `edges`. The result is a `GraphCSR`, so it can be handed straight to the `graph` functions. */
        exportCSR(): { offsets: Int32Array; edges: Int32Array; weights?: Float64Array; types?: Uint8Array; directed: boolean };
        /** False if `u` is out of range or no such edge exists. */
        hasEdge(u: number, v: number): boolean;
        /** The number of nodes. */
        readonly nodeCount: number;
        /** The number of `addEdge` calls (an undirected edge is one call). */
        readonly edgeCount: number;
        /** Node ids in breadth-first order from `src`. */
        bfs(src: number): number[];
        /** Node ids in depth-first order from `src`. */
        dfs(src: number): number[];
        /** BFS order over the arcs whose type `types` admits (the order of bfs(src) on the graph with the other arcs removed). The options must set visit and/or types. */
        bfs(src: number, opts: { types: GraphTypeMask }): number[];
        /** DFS order over the admitted arcs, as dfs(src) on the filtered graph. */
        dfs(src: number, opts: { types: GraphTypeMask }): number[];
        /** Visitor form: calls visit(node) in BFS order, stops when it returns true, returns the count visited; allocates nothing per node; `types` filters arcs. */
        bfs(src: number, opts: { visit: (node: number) => boolean | void; types?: GraphTypeMask }): number;
        /** Visitor form of dfs (smaller ids first); same contract as the bfs visitor. */
        dfs(src: number, opts: { visit: (node: number) => boolean | void; types?: GraphTypeMask }): number;
        /** Shortest distances; a single distance to dst when given. */
        dijkstra(src: number): number[];
        dijkstra(src: number, dst: number): number;
        /** Shortest distances from `src`; negative edge weights are allowed. */
        bellmanFord(src: number): number[];
        /** Node ids in topological order; a cycle throws RangeError. */
        topologicalSort(): number[];
        /** Visitor form: visits in topological order with early exit; a cycle still throws RangeError. */
        topologicalSort(opts: { visit: (node: number) => boolean | void }): number;
        /** Component id per node (weak components on a directed graph). */
        connectedComponents(): number[];
        /** All-pairs distance matrix; refused for n > 1024. */
        floydWarshall(): number[][];
        /** Minimum spanning forest. */
        mst(): { weight: number; edges: [number, number, number][] };
        /** A* search; closed nodes reopen on a shorter path, so an inconsistent heuristic stays optimal within a push budget of 64*n+1024 (RangeError beyond). */
        aStar(src: number, dst: number, heuristic: (node: number) => number): { dist: number; path: number[] };
        /** Dijkstra from `src` to `dst` with the path: `{ dist, path }` as aStar returns it (dist Infinity and an empty path when
         *  unreachable, [src] at 0 for src === dst). Parallel edges count at their lightest; a negative weight is a RangeError. The
         *  path follows shortestPathTree's predecessors. `types` restricts the search to admitted arcs (mask 0 leaves only src). */
        shortestPath(src: number, dst: number, opts?: { types?: GraphTypeMask }): { dist: number; path: number[] };
        /** The k-hop neighbourhood of `src` over admitted arcs (all when `types` is omitted): `nodes` in BFS order starting with src, `dist[i]` the hop
         *  count of nodes[i] (0 for src, at most k). k is an integer >= 0 (RangeError otherwise, TypeError for a non-number); src out of range is a RangeError. */
        neighborhood(src: number, k: number, opts?: { types?: GraphTypeMask }): { nodes: Int32Array; dist: Int32Array };
        /** Typed path pattern A-[hops[0]]->B-[hops[1]]->C...: the distinct nodes reached from `sources` by walks of exactly hops.length arcs whose i-th arc
         *  has a type admitted by hops[i], ascending. Walks may revisit nodes; an empty `hops` returns the distinct sources. O(hops.length * (n + m)).
         *  sources is a node id or an array of them (RangeError when out of range); at most 1024 hops (RangeError). */
        pathEndpoints(sources: number | ArrayLike<number>, hops: ArrayLike<GraphTypeMask>): Int32Array;
        /** Every walk matching the pattern of pathEndpoints, written as rows of hops.length + 1 node ids into `outNodes` and, when given, the hops.length arc
         *  types of the row into `outTypes`; returns the TOTAL number of matching walks. Order: sources as listed (a repeated source repeats its walks), then
         *  depth-first with each node's arcs in insertion order; parallel arcs that both match give separate rows (told apart by their types).
         *  Capacity: rows = outNodes.length / (hops.length + 1); when the total exceeds it, the first `rows` walks are written, the rest are only counted,
         *  and the total is still returned (grow and call again); slots past the written rows are untouched.
         *  TypeError: outNodes not an Int32Array or its length not a multiple of hops.length + 1, outTypes not a Uint8Array of exactly rows * hops.length,
         *  a detached or out-of-bounds view, an immutable buffer, outNodes and outTypes sharing memory. RangeError past 10^9 arc steps (enumeration budget).
         *  sources and hops are read before the views. */
        pathsInto(sources: number | ArrayLike<number>, hops: ArrayLike<GraphTypeMask>, outNodes: Int32Array, outTypes?: Uint8Array): number;
        /** Dijkstra's shortest-path tree from `src`: `dist[v]` (Infinity when unreachable) and `pred[v]`, the node before v on the
         *  path Dijkstra settled first (-1 for src and unreachable nodes; among equal-length paths, the first one relaxed). Distances
         *  equal dijkstra(src) and networkx.single_source_dijkstra; a negative weight is a RangeError. */
        shortestPathTree(src: number): { dist: Float64Array; pred: Int32Array };
        /** Longest (critical) path of a directed acyclic graph, networkx.dag_longest_path semantics: unweighted edges count 1,
         *  parallel edges at their heaviest, and a path restarts at v when its best incoming sum is negative, so `length` >= 0 and
         *  `dist[v]` is the longest path ending at v (never below 0). Ties are deterministic: the smallest predecessor id, then the
         *  smallest end node; an empty graph gives length 0 and an empty path. A cycle (a self loop included) is a RangeError, an
         *  undirected graph a TypeError. O(n + m). */
        criticalPath(): { length: number; path: number[]; dist: Float64Array };
        /** PageRank by pull power iteration (networkx.pagerank semantics: dangling mass and teleport follow the personalization vector, stop when the L1 change < n * tolerance). Weighted graphs use edge weights (negative rejects); parallel edges add up. Defaults damping 0.85, tolerance 1e-6, maxIterations 100; RangeError "no convergence within N iterations" when the limit is hit. See the Analytics note on `graph`. */
        pageRank(opts?: GraphPageRankOptions): Promise<Float64Array>;
        /** Topic-sensitive personalized PageRank: one PageRank per topic, all topics in one pass (each power-iteration sweep over the arcs updates every
         *  topic). Topic t's personalization puts weight on its seeds (uniform, or `weights` normalized; a repeated seed adds up). Per topic the iteration is
         *  pageRank's (networkx.pagerank semantics): start 1/n, x' = damping * (pull over arcs + dangling mass * p) + (1 - damping) * p, where dangling mass (nodes
         *  with no admitted out-weight) follows the topic's personalization; stop when the L1 change < n * tolerance and return that iterate.
         *  Arcs: only types admitted by `types` (default all), weight = edge weight (1 unweighted) * typeWeights[type] (default 1); a weight of 0 removes the arc
         *  from the out-weight, so typeWeights[t] = 0 equals clearing bit t. Each topic stops at its own convergence, so a topic's row is identical whether it
         *  runs alone or in a batch. Resolves to a Float64Array of topics.length * nodeCount entries, row-major: result[t * nodeCount + v]; [] topics gives an
         *  empty array. Rejects with RangeError "topic T: no convergence within N iterations" when any topic misses maxIterations (defaults damping 0.85,
         *  tolerance 1e-6, maxIterations 100, as pageRank). Topics, options and the graph are read when called (later mutations do not affect the run); large
         *  runs ((n + m) * 16 * topics >= 2^16) compute off the JS thread. RangeError: a seed out of range, no seeds, a negative or non-finite seed weight or
         *  type weight, a zero seed-weight sum, a negative edge weight, more than 65536 topics or topics * nodes > 2^27; TypeError: a topic that is neither
         *  an array of seeds nor { seeds, weights? }, unknown keys in either bag. */
        topicPageRank(topics: GraphTopic[], opts?: GraphTopicPageRankOptions): Promise<Float64Array>;
        /** Asynchronous label propagation on the underlying undirected graph (weighted votes on weighted graphs), seeded shuffle each round, ties broken by the seeded generator; stops when every node holds a most frequent neighbour label or after maxRounds (default 100). Labels are compacted to 0..k-1 by first appearance. */
        labelPropagation(opts?: { seed?: number; maxRounds?: number }): Promise<Int32Array>;
        /** Louvain community detection (networkx.louvain_communities semantics: level stops when modularity gains <= threshold) on the underlying undirected graph; a directed graph is clustered on w(u,v) + w(v,u). Defaults resolution 1, threshold 1e-7, seed 0 (deterministic for a seed). `modularity` is that of `communities` on the input graph. */
        louvain(opts?: { resolution?: number; threshold?: number; seed?: number }): Promise<{ communities: Int32Array; count: number; modularity: number; levels: number }>;
        /** Newman modularity of a partition (ids 0..n-1, one per node): undirected Q = sum_c L_c/m - resolution*(d_c/2m)^2, directed Q = sum_c L_c/m - resolution*out_c*in_c/m^2; a self loop counts once in m and twice in a degree, parallel edges add; an edgeless graph scores 0. */
        modularity(communities: ArrayLike<number>, opts?: { resolution?: number }): Promise<number>;
        /** k-core number of every node (Batagelj-Zaversnik bucket peeling, O(n + m)) on the underlying undirected simple graph: self loops and parallel edges are ignored, a directed graph's arcs count once per neighbour pair. */
        coreNumbers(): Promise<Int32Array>;
        /** Triangles through each node and in total, on the underlying undirected simple graph (degree-ordered orientation with sorted-merge/galloping intersections, multi-threaded). */
        triangles(): Promise<{ total: number; perVertex: Float64Array }>;
        /** Local clustering coefficient per node, its mean over all nodes (networkx.average_clustering, 0 for the empty graph) and transitivity (3 * triangles / connected triples), on the underlying undirected simple graph. */
        clustering(): Promise<{ local: Float64Array; average: number; transitivity: number }>;
        /** Brandes betweenness (networkx.betweenness_centrality semantics, normalization included), over shortest paths of the graph with self loops removed and parallel edges merged (minimum weight). `weighted` defaults to whether the graph is weighted and then needs every weight > 0. `samples: k` uses k distinct seeded sources (k < n rescales like networkx's k=), `sources` names them; independent sources run on all cores and the result is bit-identical for any thread count. */
        betweenness(opts?: GraphBetweennessOptions): Promise<Float64Array>;
        /** Closeness centrality from distances TO each node (incoming on a directed graph), Wasserman-Faust scaled by default (networkx.closeness_centrality); `weighted` as for betweenness (weights >= 0). One BFS/Dijkstra per node, multi-threaded. */
        closeness(opts?: { wfImproved?: boolean; weighted?: boolean }): Promise<Float64Array>;
        /** Strongly connected components by iterative Tarjan: ids are a reverse topological order of the condensation (no arc goes from a lower to a higher id; sinks first). On an undirected graph they equal the connected components. */
        stronglyConnectedComponents(): Promise<{ count: number; labels: Int32Array }>;
        /** Weakly connected components by union-find; ids are numbered in order of each component's smallest node (connectedComponents' numbering). */
        weaklyConnectedComponents(): Promise<{ count: number; labels: Int32Array }>;
        /** Topological generation of every node of a directed acyclic graph: 0 for sources, else 1 + the largest level of a predecessor (networkx.topological_generations); `count` is the number of levels. A cycle rejects with RangeError, an undirected graph with TypeError. */
        topologicalLevels(): Promise<{ count: number; labels: Int32Array }>;
        /** Maximum flow from `source` to `sink` by Dinic's algorithm; capacities are the edge weights (1 on an unweighted graph, negative rejects), an undirected edge carries flow either way, parallel edges add. `flows` is the flow on each arc in exportCSR order, `sourceSide` marks the source side of a minimum cut. */
        maxFlow(source: number, sink: number): Promise<{ value: number; flows: Float64Array; sourceSide: Uint8Array }>;
        /** Maximum-cardinality bipartite matching by Hopcroft-Karp on the underlying undirected simple graph; `left` gives the sides (truthy = left), otherwise they come from a BFS 2-colouring. `mate[v]` is v's partner or -1. A graph that is not bipartite (under `left`) rejects with RangeError. */
        bipartiteMatching(opts?: { left?: ArrayLike<number | boolean> }): Promise<{ size: number; mate: Int32Array }>;
        /** Link-prediction scores for node pairs ([[u, v], ...] or a flat [u0, v0, u1, v1, ...] array) on the underlying undirected simple graph, as networkx defines them: jaccard (default) |N(u)&N(v)| / |N(u)|N(v)|, adamicAdar sum 1/ln deg(w), resourceAllocation sum 1/deg(w), commonNeighbors, preferentialAttachment deg(u)*deg(v); common neighbours exclude u and v. */
        linkPrediction(pairs: ArrayLike<number> | [number, number][], opts?: { method?: GraphLinkMethod }): Promise<Float64Array>;
        /** The graph as a compact type-tagged record with delta-varint encoded adjacency. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the dyna:structures module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** A set of edge types as a bitmask: bit t admits type t (0..31); an integer in [-2^31, 2^32-1] taken modulo 2^32, so 1 << 31 and -1 work (RangeError otherwise, TypeError for a non-number). */
    type GraphTypeMask = number;
    /** Options of Graph#pageRank / graph.pageRank. */
    interface GraphPageRankOptions {
        /** Probability of following an edge, in [0, 1]; default 0.85. */
        damping?: number;
        /** Stop when the L1 change of an iteration is below n * tolerance; default 1e-6. */
        tolerance?: number;
        /** Iteration cap, >= 1; default 100. */
        maxIterations?: number;
        /** Teleport (and dangling) distribution, one non-negative entry per node, normalized internally; personalized PageRank. */
        personalization?: Float64Array | number[];
    }
    /** A topicPageRank topic: its seed nodes (an array or integer typed array), or seeds with one non-negative weight each. */
    type GraphTopic = ArrayLike<number> | { seeds: ArrayLike<number>; weights?: Float64Array | number[] };
    /** Options of Graph#topicPageRank. */
    interface GraphTopicPageRankOptions {
        /** Probability of following an arc, in [0, 1]; default 0.85. */
        damping?: number;
        /** Stop a topic when its L1 change is below n * tolerance; default 1e-6. */
        tolerance?: number;
        /** Iteration cap per topic, >= 1; default 100. */
        maxIterations?: number;
        /** Edge types the walk may follow (default all). */
        types?: GraphTypeMask;
        /** 32 finite non-negative multipliers, one per edge type, applied to every arc's weight. */
        typeWeights?: Float64Array | number[];
    }
    /** Options of Graph#betweenness / graph.betweenness. */
    interface GraphBetweennessOptions {
        /** Divide by the number of pairs (default true), as networkx does. */
        normalized?: boolean;
        /** Count the path endpoints too (default false). */
        endpoints?: boolean;
        /** Estimate from this many distinct sources drawn with `seed` (1..n). */
        samples?: number;
        /** Seed of the source sample; default 0. */
        seed?: number;
        /** The exact distinct source nodes to accumulate from (instead of `samples`). */
        sources?: ArrayLike<number>;
        /** Shortest paths by edge weight; defaults to whether the graph has weights. */
        weighted?: boolean;
    }
    /** A link-prediction score. */
    type GraphLinkMethod = "jaccard" | "adamicAdar" | "resourceAllocation" | "commonNeighbors" | "preferentialAttachment";
    /** A graph as compressed sparse rows: node u's out-arcs are edges[offsets[u] .. offsets[u+1]] (n + 1 offsets starting at 0, never decreasing, ending at edges.length; targets < n; at most 2^26 nodes). `weights` (finite) aligns with `edges`; `directed` defaults to true -- pass false when every undirected edge is stored under both ends (Graph#exportCSR output is accepted as is). */
    interface GraphCSR {
        offsets: Int32Array | Uint32Array;
        edges: Int32Array | Uint32Array;
        weights?: Float64Array | number[];
        directed?: boolean;
    }

    /** Graph analytics over CSR arrays: the same kernels as the Graph methods of the same names, for graphs held as typed arrays (Graph#exportCSR, CSRGraph-style builders, files).
     *  Analytics note (Graph methods and these functions alike): every call returns a Promise. Arguments are read and validated and the graph is SNAPSHOTTED on the calling thread when the call starts (mutating the Graph or the arrays afterwards cannot change the result); bad arguments REJECT the promise, never throw synchronously. A call whose work is large (about 64K node+edge visits) runs on a thread created for that one call, as dyna:ml does, so the event loop keeps running; inside it PageRank, betweenness, closeness, triangles and link prediction split their work over all cores, with results bit-identical for any thread count. Results are fresh typed arrays indexed by node id. Runtime shutdown interrupts a running call (InternalError "interrupted"). */
    namespace graph {
        function pageRank(csr: GraphCSR, opts?: GraphPageRankOptions): Promise<Float64Array>;
        function labelPropagation(csr: GraphCSR, opts?: { seed?: number; maxRounds?: number }): Promise<Int32Array>;
        function louvain(csr: GraphCSR, opts?: { resolution?: number; threshold?: number; seed?: number }): Promise<{ communities: Int32Array; count: number; modularity: number; levels: number }>;
        function modularity(csr: GraphCSR, communities: ArrayLike<number>, opts?: { resolution?: number }): Promise<number>;
        function coreNumbers(csr: GraphCSR): Promise<Int32Array>;
        function triangles(csr: GraphCSR): Promise<{ total: number; perVertex: Float64Array }>;
        function clustering(csr: GraphCSR): Promise<{ local: Float64Array; average: number; transitivity: number }>;
        function betweenness(csr: GraphCSR, opts?: GraphBetweennessOptions): Promise<Float64Array>;
        function closeness(csr: GraphCSR, opts?: { wfImproved?: boolean; weighted?: boolean }): Promise<Float64Array>;
        function stronglyConnectedComponents(csr: GraphCSR): Promise<{ count: number; labels: Int32Array }>;
        function weaklyConnectedComponents(csr: GraphCSR): Promise<{ count: number; labels: Int32Array }>;
        /** A `directed: false` CSR rejects with TypeError. */
        function topologicalLevels(csr: GraphCSR): Promise<{ count: number; labels: Int32Array }>;
        /** `flows` follows the order of `csr.edges`. */
        function maxFlow(csr: GraphCSR, source: number, sink: number): Promise<{ value: number; flows: Float64Array; sourceSide: Uint8Array }>;
        function bipartiteMatching(csr: GraphCSR, opts?: { left?: ArrayLike<number | boolean> }): Promise<{ size: number; mate: Int32Array }>;
        function linkPrediction(csr: GraphCSR, pairs: ArrayLike<number> | [number, number][], opts?: { method?: GraphLinkMethod }): Promise<Float64Array>;
    }
}
