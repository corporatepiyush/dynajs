/*
 * dyn-sb -- the ONE decaying-growth string-builder core. The round-8 census
 * counted ten verbatim copies of the same { p, n, cap, oom } builder plus
 * growth loop across the native modules (url, term, httpmsg, xml, asn1,
 * vserialize, yaml, proc, json5, protobuf), each carrying the same
 * overshoot-costs-absolute-bytes comment and the same three-tier factor.
 * This header is the shared core they consolidate onto: pure C, no engine
 * types, field-address based so every caller keeps its own struct name,
 * element type and helper verbs -- the same rule dyn-pct.h set for the
 * percent codecs.
 *
 * ERROR STYLE: the core returns ok/fail and stores nothing. The copies
 * disagree on the convention -- most set a sticky oom flag and return void,
 * asn1 and proc set the flag AND return -1 -- and folding either style into
 * the core would change the other callers' observable returns. So the flag
 * and the return stay in the wrapper; the core's only promise is that a
 * failed reserve leaves p and cap untouched, which every copy guaranteed.
 *
 * THE CURVE: 2x below 64K, 1.5x below 1M, 1.25x beyond, boundaries strict
 * (<, not <=). Capacity after an append sequence IS observable through
 * allocation counts, so the tiers are copied exactly, not re-derived. None
 * of the consolidated builders has an inline threshold (only dyna-log's line
 * builder does, which is why it stayed out); the seed is the sole tuning
 * knob and each caller's value is preserved.
 */
#ifndef DYNA_CORE_SB_H
#define DYNA_CORE_SB_H

#include <stddef.h>
#include <stdlib.h>

/* Overshoot costs ABSOLUTE bytes, so the factor decays with size: doubling
   is free while small and wastes megabytes at scale. `seed` is the first
   allocation's size when growing from empty. */
static inline size_t dyn_sb_grow_cap(size_t cur, size_t need, size_t seed)
{
    size_t nc = cur ? cur : seed;
    while (nc < need) {
        if (nc < (1u << 16))      nc *= 2;
        else if (nc < (1u << 20)) nc += nc / 2;
        else                      nc += nc / 4;
    }
    return nc;
}

/* Grow the (p, cap) pair so *pcap >= need. Returns 1 when the capacity
   already suffices or the realloc succeeded, 0 on realloc failure with both
   fields left untouched. pp and pcap point at the caller struct's p and cap
   fields: every consolidated builder's element type is char or unsigned
   char, and both pointer kinds share void *'s representation and alignment
   (C11 6.2.5p28) -- that is what lets one core serve builders whose element
   type differs without templating or callbacks on this path. */
static inline int dyn_sb_reserve(void **pp, size_t *pcap, size_t need,
                                 size_t seed)
{
    size_t nc;
    void *np;

    if (need <= *pcap)
        return 1;
    nc = dyn_sb_grow_cap(*pcap, need, seed);
    np = realloc(*pp, nc);
    if (!np)
        return 0;
    *pp = np;
    *pcap = nc;
    return 1;
}

#endif /* DYNA_CORE_SB_H */
