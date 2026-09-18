/*
 * ac_cand.c -- double-array Aho-Corasick candidate. See ac_cand.h.
 *
 * Design against the baseline numbers (tests/bench_ac.c, pass 1):
 *   - the dense table is 1037 B/state and 99.6% of its slots are redundant
 *     (shape pass: fill = edges/(states*256) = 0.004): a trie with E edges
 *     was storing E entries in a 256*states-slot array;
 *   - the cost of that redundancy is not just memory: at 75364 states the
 *     table is 75 MiB and scan drops from 450 MB/s (root-hot) to 25 MB/s on
 *     match-dense text -- the scan is cache/TLB bound on the table, so
 *     shrinking the table is a throughput change too, not only a memory one;
 *   - post-build ROW dedup can't pay: 85-90% of densified rows are distinct
 *     on realistic sets (shape pass), so compaction must happen pre-build.
 *
 * Structure: classic double-array. One slot array tab[]; state s's row sits
 * at base[s]; slot k = base[s] + byte holds {next = target state, check =
 * owner state + 1} (0 = free). Slot pair is 8 B and one probe is ONE load.
 * next[] and check[] are interleaved so a hit probe is a single cacheline
 * access; a full dense row exists only for the root (256 slots, 2 KiB).
 *
 * The table stores the SPARSE trie; missing edges follow the fail chain:
 *     while (tab[base[s]+c].check != s+1) s = fail[s];
 * This is the standard AC scan: each fail step strictly decreases depth and
 * depth grows by at most 1 per input byte, so total fail steps are <= n over
 * any text -- amortized O(n), including adversarial input. The root's row is
 * fully materialized (missing bytes self-loop to root), so the walk always
 * terminates at the root in one step per level.
 *
 * Build places rows BFS (children ascending by byte, same order dyn-ac's
 * dense BFS enqueues them), so fail/out_link end up identical to the dense
 * implementation and the emit streams match hit for hit.
 */
#include "ac_cand.h"

#include <stdlib.h>
#include <string.h>

/* ---- trie construction (insert phase) ------------------------------------ */

static int cand_new_state(dyn_ac_cand_t *a)
{
    /* same contract as dyn_ac_new_state: refuse at the audited ceiling with
     * the existing -1/OOM meaning */
    if (a->n_states >= DYN_AC_MAX_STATES)
        return -1;
    if (a->n_states == a->cap_states) {
        size_t cap = a->cap_states ? a->cap_states * 2 : 16;
        int32_t *ch = (int32_t *)realloc(a->child_head, cap * sizeof(int32_t));
        uint32_t *b;
        int32_t *f;
        da_oo *oo;
        if (!ch)
            return -1;
        a->child_head = ch;
        b = (uint32_t *)realloc(a->base, cap * sizeof(uint32_t));
        if (!b) return -1;
        a->base = b;
        f = (int32_t *)realloc(a->fail, cap * sizeof(int32_t));
        if (!f) return -1;
        a->fail = f;
        oo = (da_oo *)realloc(a->oo, cap * sizeof(da_oo));
        if (!oo) return -1;
        a->oo = oo;
        a->cap_states = cap;
    }
    a->child_head[a->n_states] = -1;
    a->base[a->n_states] = 0;
    a->fail[a->n_states] = 0;
    a->oo[a->n_states].out = -1;
    a->oo[a->n_states].out_link = -1;
    return (int)a->n_states++;
}

static int cand_new_edge(dyn_ac_cand_t *a, uint8_t byte, int32_t target,
                         int32_t next)
{
    if (a->n_edges == a->cap_edges) {
        size_t cap = a->cap_edges ? a->cap_edges * 2 : 32;
        uint8_t *eb = (uint8_t *)realloc(a->e_byte, cap);
        int32_t *et, *en;
        if (!eb)
            return -1;
        a->e_byte = eb;
        et = (int32_t *)realloc(a->e_target, cap * sizeof(int32_t));
        if (!et) return -1;
        a->e_target = et;
        en = (int32_t *)realloc(a->e_next, cap * sizeof(int32_t));
        if (!en) return -1;
        a->e_next = en;
        a->cap_edges = cap;
    }
    a->e_byte[a->n_edges] = byte;
    a->e_target[a->n_edges] = target;
    a->e_next[a->n_edges] = next;
    return (int)a->n_edges++;
}

dyn_ac_cand_t *dyn_da_new(size_t n_pat)
{
    dyn_ac_cand_t *a = (dyn_ac_cand_t *)calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->n_pat = n_pat;
    if (n_pat) {
        a->plen = (size_t *)calloc(n_pat, sizeof(size_t));
        if (!a->plen) {
            free(a);
            return NULL;
        }
    }
    if (cand_new_state(a) < 0) {    /* the root */
        dyn_da_free(a);
        return NULL;
    }
    return a;
}

void dyn_da_free(dyn_ac_cand_t *a)
{
    if (!a)
        return;
    free(a->tab);
    free(a->base);
    free(a->fail);
    free(a->oo);
    free(a->child_head);
    free(a->e_byte);
    free(a->e_target);
    free(a->e_next);
    free(a->plen);
    free(a);
}

int dyn_da_insert(dyn_ac_cand_t *a, const uint8_t *p, size_t len, int idx)
{
    int32_t s = 0;
    size_t i;

    if (idx >= 0 && (size_t)idx < a->n_pat)
        a->plen[idx] = len;
    for (i = 0; i < len; i++) {
        int32_t e = a->child_head[s];
        while (e >= 0 && a->e_byte[e] != p[i])
            e = a->e_next[e];
        if (e < 0) {
            int ns = cand_new_state(a);
            if (ns < 0)
                return -1;
            e = cand_new_edge(a, p[i], (int32_t)ns, a->child_head[s]);
            if (e < 0)
                return -1;
            a->child_head[s] = e;
            s = (int32_t)ns;
        } else {
            s = a->e_target[e];
        }
    }
    if (a->oo[s].out < 0)
        a->oo[s].out = idx;         /* first pattern wins on a duplicate */
    return 0;
}

/* ---- build: place rows into the double array, wire fail/out_link --------- */

/* children of s, ascending by byte (the order dyn-ac's dense BFS enqueues) */
static int gather_children(const dyn_ac_cand_t *a, int32_t s, uint8_t *bytes,
                           int32_t *targets)
{
    int n = 0;
    int32_t e = a->child_head[s];
    while (e >= 0) {
        int i, pos = n;
        for (i = 0; i < n; i++)
            if (bytes[i] > a->e_byte[e]) { pos = i; break; }
        memmove(bytes + pos + 1, bytes + pos, (size_t)(n - pos));
        memmove(targets + pos + 1, targets + pos, (size_t)(n - pos) * sizeof *targets);
        bytes[pos] = a->e_byte[e];
        targets[pos] = a->e_target[e];
        n++;
        e = a->e_next[e];
    }
    return n;
}

static int tab_reserve(dyn_ac_cand_t *a, size_t need)
{
    size_t cap = a->tab_cap ? a->tab_cap : 256;
    size_t old = a->tab_cap;
    da_slot *t;
    while (cap < need)
        cap *= 2;
    if (cap == a->tab_cap)
        return 0;
    /* physical slots = logical cap + 256 zeroed slack: a probe computes
     * base[s] + byte for states that own nothing (leaves) or sit near the
     * top of the table, and byte reaches 0xFF -- the walk must read zeros
     * (free -> keep failing) there instead of past the allocation */
    t = (da_slot *)realloc(a->tab, (cap + 256) * sizeof(da_slot));
    if (!t)
        return -1;
    memset(t + old, 0, (cap + 256 - old) * sizeof(da_slot));
    a->tab = t;
    a->tab_cap = cap;
    return 0;
}

/* lowest free slot at or after hint */
static size_t tab_free(const dyn_ac_cand_t *a, size_t hint)
{
    while (hint < a->tab_cap && a->tab[hint].check)
        hint++;
    return hint;
}

/* claim the slot for byte b of state s's row (base[s] + b); the slot lives
 * at the first free position, which pins s's row base. `t` is only the
 * transition target stored IN the slot -- the base belongs to the owner. */
static int place_one(dyn_ac_cand_t *a, size_t hint, uint32_t s, uint32_t t,
                     uint8_t b, size_t *pos)
{
    size_t j = tab_free(a, hint);
    if (j >= a->tab_cap) {
        if (tab_reserve(a, a->tab_cap ? a->tab_cap * 2 : 256) < 0)
            return -1;
        j = tab_free(a, j);
        if (j >= a->tab_cap)
            return -1;
    }
    a->tab[j].next = t;
    a->tab[j].check = s + 1;
    a->base[s] = (uint32_t)((ptrdiff_t)j - (ptrdiff_t)b);
    *pos = j;
    return 0;
}

int dyn_da_build(dyn_ac_cand_t *a)
{
    int32_t *queue = (int32_t *)malloc(a->n_states * sizeof(int32_t));
    size_t head = 0, tail = 0, hint;
    uint8_t *bytes;
    int32_t *targets;
    int nroot, i;

    if (!queue)
        return -1;
    bytes = (uint8_t *)malloc(256);
    targets = (int32_t *)malloc(256 * sizeof(int32_t));
    if (!bytes || !targets) {
        free(queue); free(bytes); free(targets);
        return -1;
    }

    /* the root owns slots 0..255 outright; missing bytes self-loop to root */
    if (tab_reserve(a, 256) < 0)
        goto fail;
    for (i = 0; i < 256; i++) {
        a->tab[i].next = 0;
        a->tab[i].check = 1;
    }
    a->base[0] = 0;
    nroot = gather_children(a, 0, bytes, targets);
    for (i = 0; i < nroot; i++) {
        a->tab[bytes[i]].next = (uint32_t)targets[i];
        a->fail[targets[i]] = 0;
        queue[tail++] = targets[i];
    }
    hint = 256;                     /* everything above the root row is free */

    while (head < tail) {
        int32_t s = queue[head++];
        int n = gather_children(a, s, bytes, targets);
        int32_t fs = a->fail[s];

        if (n == 0)
            continue;               /* a leaf: base[s] stays 0, never owned */

        if (n == 1) {
            size_t j;
            if (place_one(a, hint, (uint32_t)s, (uint32_t)targets[0],
                          bytes[0], &j) < 0)
                goto fail;
            if (j == hint)
                hint = tab_free(a, hint + 1);
        } else {
            /* multi-child rows: try each free slot as an anchor for the
             * lowest byte and require the whole row clear */
            size_t j = tab_free(a, hint);
            ptrdiff_t cand;
            for (;;) {
                int ok = 1;
                if (j >= a->tab_cap) {
                    if (tab_reserve(a, a->tab_cap * 2) < 0)
                        goto fail;
                    j = tab_free(a, j);
                    if (j >= a->tab_cap)
                        goto fail;
                }
                cand = (ptrdiff_t)j - (ptrdiff_t)bytes[0];
                if (cand >= 0) {
                    for (i = 0; i < n; i++) {
                        if (a->tab[cand + bytes[i]].check) { ok = 0; break; }
                    }
                    /* the whole row must fit under the logical cap: slots in
                     * the physical slack are scratch for probes and are
                     * re-zeroed on growth -- never claimable */
                    if (ok && (size_t)(cand + bytes[n - 1]) < a->tab_cap)
                        break;
                }
                j = tab_free(a, j + 1);
            }
            for (i = 0; i < n; i++) {
                a->tab[cand + bytes[i]].next = (uint32_t)targets[i];
                a->tab[cand + bytes[i]].check = (uint32_t)s + 1;
            }
            a->base[s] = (uint32_t)cand;    /* the row base is s's, once */
            if ((size_t)j == hint)
                hint = tab_free(a, hint + 1);
        }

        /* fail links for the new children: walk the fail chain of s until a
         * row owns byte b; the root's full row ends every walk */
        for (i = 0; i < n; i++) {
            uint8_t b = bytes[i];
            int32_t t = targets[i], f = fs;
            for (;;) {
                uint32_t k = a->base[f] + b;
                if (a->tab[k].check == (uint32_t)f + 1) {
                    /* fail is a proper suffix state, so its depth is
                     * strictly below t's: nxt can never be t itself */
                    a->fail[t] = (int32_t)a->tab[k].next;
                    break;
                }
                f = a->fail[f];
            }
            a->oo[t].out_link =
                a->oo[a->fail[t]].out >= 0 ? a->fail[t]
                                           : a->oo[a->fail[t]].out_link;
            queue[tail++] = t;
        }
    }

    free(queue);
    free(bytes);
    free(targets);
    free(a->child_head);
    free(a->e_byte);
    free(a->e_target);
    free(a->e_next);
    a->child_head = NULL;
    a->e_byte = NULL;
    a->e_target = NULL;
    a->e_next = NULL;
    a->n_edges = a->cap_edges = 0;
    return 0;

fail:
    free(queue);
    free(bytes);
    free(targets);
    return -1;
}

int dyn_da_run(const dyn_ac_cand_t *a, const uint8_t *t, size_t tlen,
               dyn_ac_emit_t emit, void *ud)
{
    const da_slot *tab = a->tab;
    const uint32_t *base = a->base;
    const da_oo *oo = a->oo;
    uint32_t s = 0;
    size_t i;

    for (i = 0; i < tlen; i++) {
        uint32_t c = t[i], k;
        int32_t o;
        /* base[0] == 0 by construction, so the root never needs the base
         * load -- this is the no-match steady state and it is the hottest
         * loop shape (benchmark: none-text) */
        k = s ? base[s] + c : c;
        while (tab[k].check != s + 1) {
            s = (uint32_t)a->fail[s];
            k = base[s] + c;
        }
        s = tab[k].next;
        for (o = oo[s].out >= 0 ? (int32_t)s : oo[s].out_link;
             o >= 0; o = oo[o].out_link) {
            if (emit(ud, oo[o].out, i + 1))
                return 1;
        }
    }
    return 0;
}

size_t dyn_da_states(const dyn_ac_cand_t *a) { return a->n_states; }

size_t dyn_da_bytes(const dyn_ac_cand_t *a)
{
    return a->n_states * (sizeof(uint32_t) + sizeof(int32_t) + sizeof(da_oo))
         + a->tab_cap * sizeof(da_slot)
         + a->n_pat * sizeof(size_t)
         + sizeof(*a);
}
