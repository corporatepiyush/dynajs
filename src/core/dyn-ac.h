/*
 * dyn-ac -- Aho-Corasick multi-pattern matching. PURE C: no JSValue, no
 * JSContext. Compiles with -Isrc/core alone.
 *
 * One pass over the text finds every occurrence of every pattern, and the cost
 * of that pass does not grow with the number of patterns. That is the whole
 * property, and it is why the crossover for the JS `MultiMatcher` built on this
 * is in the PATTERN COUNT (~36) rather than in the number of uses: its cost is
 * flat where N separate substring searches grow linearly.
 *
 * The automaton is a byte trie with two materializations, chosen at build
 * time by measured table size (audit E10-11 redesign; bench/bench_ac.c is the
 * arbiting bench and every constant below traces to a row in its output):
 *
 * - DENSE (n_states <= DYN_AC_DENSE_MAX): the original layout. Each state
 *   holds 256 goto slots -- 1 KiB per state -- every edge materialized after
 *   the failure pass, so a scan step is one indexed load with no fail
 *   chasing. Below a few thousand states the whole table is cache-resident
 *   and this is the fastest possible scan; the rows are 99.6% redundant
 *   (fill = edges/(states*256) = 0.004 across every bench set), but at this
 *   size redundancy is cheaper than indirection.
 *
 * - COMPACT (n_states > DYN_AC_DENSE_MAX): a double-array trie. One slot
 *   array holds {next, check} pairs (8 B); state s's row sits at base[s] and
 *   a hit probe is ONE load of tab[base[s]+byte]. Missing edges follow the
 *   fail chain -- each step strictly decreases depth and depth grows by at
 *   most 1 per input byte, so total fail steps are <= n over ANY text,
 *   including adversarial. The root's row is fully materialized (missing
 *   bytes self-loop), which ends every walk. Measured (bench/bench_ac.c):
 *   ~31 B/state vs 1037, build ~4x faster, scan 1.7-3x FASTER than dense
 *   once the dense table stops fitting in cache (the dense table at 75364
 *   states is 75 MiB and scanned at 25-34 MB/s; the compact one at 2.3 MiB
 *   scans the same text at 77-101 MB/s). Costs: on tiny cache-resident
 *   automata the base[s] indirection makes some rows up to ~1.5x slower --
 *   which is exactly what the dense path below the threshold avoids.
 *
 * `fail` is the standard suffix link. `out` is the index of a pattern ending
 * at this state or -1, and `out_link` chains to the next shorter pattern
 * that also ends there, so {he, she, his, hers} over "ushers" reports
 * she@1, he@2, hers@2 -- including the he@2 that a naive trie walk misses --
 * without a per-state list allocation. Both materializations compute the
 * same fail/out_link values (children are processed in ascending byte order
 * in both), so the two paths emit identical hit streams; bench/bench_ac.c
 * --diff pins that over a systematic corpus.
 *
 * This lives in the core rather than in the matcher binding because it has two
 * consumers: `dyna:matcher`'s MultiMatcher, and the token-substitution
 * dictionary codec in dyn-compress, which needs the automaton from C with no JS
 * anywhere near it.
 *
 * Allocation: unlike the other cores, this one DOES own heap memory, because an
 * automaton is a compiled artefact whose size is not a function of any single
 * call's input. It is malloc-backed and freed by dyn_ac_free; there is no
 * hidden global and no lazy initialisation, so it is reentrant and every
 * instance is independent.
 */
#ifndef DYN_AC_H
#define DYN_AC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_AC_ALPHABET 256

/* Hard ceiling on automaton states. Without it, a large pattern set grows
 * silently (audit E10-11); with it, inserts past the ceiling fail like OOM
 * (dyn_ac_insert returns -1): a loud refusal beats an unbudgeted build.
 * 2^18 is chosen against the consumers' own input caps: dyn-dict accepts at
 * most DYN_DICT_MAX_PHRASES (65535) phrases, and 65535 fully-distinct 4-byte
 * phrases -- the zero-prefix-sharing worst case at that count -- build
 * 4*65535+1 = 262141 states, which still fits, so the state cap refuses
 * nothing the phrase-count cap accepts at that size (real phrase sets share
 * prefixes and fit far more); dyna-matcher caps total pattern BYTES (16 MiB)
 * instead, whose one-state-per-byte worst case (~16M states) is exactly the
 * blowup this ceiling exists to refuse. */
#define DYN_AC_MAX_STATES (1 << 18)

/* The dense goto row is 1 KiB -- the per-state cost the dense-mode math and
 * the comments in dyn-ac.c do their math with. Compact-mode states cost ~31 B
 * (see the bench numbers in the file comment), so a cap-bound automaton is
 * bounded by max(2048 * 1 KiB, 2^18 * ~31 B + plen) -- a few MiB either way,
 * where the pre-redesign ceiling was ~262 MiB. */
_Static_assert(sizeof(int32_t) * DYN_AC_ALPHABET == 1024, "goto row is 1 KiB");

/* Below this many states the automaton materializes DENSE; above, COMPACT.
 * 2048 states = 2 MiB of dense table, the measured crossover where the dense
 * table stops being cache-free: at 1847 states (set A2) dense and compact
 * scan within noise of each other; at 6882 (M1000) compact is already 1.5x
 * faster and 39x smaller. Override only with a bench row in hand. */
#ifndef DYN_AC_DENSE_MAX
#define DYN_AC_DENSE_MAX 2048
#endif

typedef struct { uint32_t next; uint32_t check; } dyn_ac_slot;
typedef struct { int32_t out; int32_t out_link; } dyn_ac_oo;

typedef struct {
    /* -- dense materialization -- */
    int32_t *go;        /* n_states * 256, only when compact == 0 */

    /* -- compact materialization -- */
    dyn_ac_slot *tab;   /* transition slots, only when compact == 1 */
    uint32_t *base;     /* row bases; base[0] == 0 (root), compact only */
    dyn_ac_oo *oo;      /* packed out/out_link pairs (compact only): the
                         * scan's per-byte output check is then one 8 B load */

    /* -- both -- */
    int32_t *fail;      /* suffix links */
    int32_t *out;       /* pattern index ending here, or -1 */
    int32_t *out_link;  /* next state in this state's output chain, or -1
                         * (dense keeps the two arrays; compact packs the
                         * same values into `oo` pairs after build and frees
                         * them) */
    uint32_t compact;   /* materialization tag: 0 dense, 1 compact */
    size_t n_states, cap_states;
    size_t tab_cap;     /* compact: logical slot count (>= 256, grows x2) */
    size_t *plen;       /* pattern byte lengths, for reporting the span */
    size_t n_pat;
    int ascii;          /* every byte of every pattern < 0x80 */

    /* -- insert-phase trie temp (freed by dyn_ac_build) --
     * Children of a state live in a linked list of edges; the materializer
     * walks these once and frees them, so insert never allocates a dense
     * row and the transient cost is ~9 B per trie edge. */
    int32_t *child_head;                /* n_states: head edge of child list */
    uint8_t *e_byte;                    /* edge: the byte consumed */
    int32_t *e_target;                  /* edge: child state */
    int32_t *e_next;                    /* edge: next sibling edge */
    size_t n_edges, cap_edges;
} dyn_ac_t;

/* Allocate an empty automaton with a root state. Returns NULL on OOM.
 * `n_pat` is the number of patterns the caller intends to insert; it sizes the
 * length table up front. */
dyn_ac_t *dyn_ac_new(size_t n_pat);

void dyn_ac_free(dyn_ac_t *a);

/* Insert one pattern at index `idx` (0 <= idx < n_pat). Returns 0, or -1 on
 * OOM (or past DYN_AC_MAX_STATES). On an exact duplicate the FIRST pattern
 * wins, which is what makes the dictionary codec's id assignment stable. */
int dyn_ac_insert(dyn_ac_t *a, const uint8_t *p, size_t len, int idx);

/* Build the failure and output links and materialize the automaton (dense or
 * compact by DYN_AC_DENSE_MAX). Call once, after every insert and before any
 * run. Returns 0, or -1 on OOM. */
int dyn_ac_build(dyn_ac_t *a);

/* One pass over the text. `emit` is called for every (pattern index, end byte)
 * hit, in the order the hits END; returning non-zero from it stops the scan and
 * makes dyn_ac_run return 1. The span of a hit is
 * [end_byte - a->plen[pat], end_byte). */
typedef int (*dyn_ac_emit_t)(void *ud, int pat, size_t end_byte);

int dyn_ac_run(const dyn_ac_t *a, const uint8_t *t, size_t tlen,
               dyn_ac_emit_t emit, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* DYN_AC_H */
