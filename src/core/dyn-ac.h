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
 * The automaton is a byte trie in a flat array. Each state holds 256 goto slots
 * -- 1 KiB per state, which is exactly why this is built once and reused rather
 * than per call. `fail` is the standard suffix link. `out` is the index of a
 * pattern ending at this state or -1, and `out_link` chains to the next shorter
 * pattern that also ends there, so {he, she, his, hers} over "ushers" reports
 * she@1, he@2, hers@2 -- including the he@2 that a naive trie walk misses --
 * without a per-state list allocation.
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

typedef struct {
    int32_t *go;        /* n_states * 256 */
    int32_t *fail;
    int32_t *out;       /* pattern index ending here, or -1 */
    int32_t *out_link;  /* next state in this state's output chain, or -1 */
    size_t n_states, cap_states;
    size_t *plen;       /* pattern byte lengths, for reporting the span */
    size_t n_pat;
    int ascii;          /* every byte of every pattern < 0x80 */
} dyn_ac_t;

/* Allocate an empty automaton with a root state. Returns NULL on OOM.
 * `n_pat` is the number of patterns the caller intends to insert; it sizes the
 * length table up front. */
dyn_ac_t *dyn_ac_new(size_t n_pat);

void dyn_ac_free(dyn_ac_t *a);

/* Insert one pattern at index `idx` (0 <= idx < n_pat). Returns 0, or -1 on
 * OOM. On an exact duplicate the FIRST pattern wins, which is what makes the
 * dictionary codec's id assignment stable. */
int dyn_ac_insert(dyn_ac_t *a, const uint8_t *p, size_t len, int idx);

/* Build the failure and output links. Call once, after every insert and before
 * any run. Returns 0, or -1 on OOM. */
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
