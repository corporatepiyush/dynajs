/*
 * dyn-ac -- Aho-Corasick, pure C. See dyn-ac.h.
 *
 * Lifted verbatim from src/dyna-matcher.c, where it was already pure: the
 * automaton never touched a JSValue, only the constructor and the scan
 * wrappers did. That is the rare case the entanglement survey (CLAUDE.md §12)
 * calls a real lift rather than a rewrite -- the cut was already there.
 *
 * The E10-11 redesign (audit follow-up) split the single dense table into two
 * materializations of the same trie; the measured reasoning lives in the
 * header comment and every number it cites is a bench/bench_ac.c row. The
 * shape that made this cheap: insert builds a plain edge-list trie (~9 B per
 * edge) and the ONE build call materializes either layout from it, so the
 * old dense code survives verbatim as the small-automaton path.
 */
#include "dyn-ac.h"

#include <stdlib.h>
#include <string.h>

/* ---- insert phase: edge-list trie ---------------------------------------- */

static int ac_new_state(dyn_ac_t *a)
{
    /* Refuse past DYN_AC_MAX_STATES before any allocation: -1 here is the
     * existing OOM contract -- callers already treat a failed insert as
     * "free the automaton", so a capped set fails loudly rather than
     * answering with an unbudgeted build (audit E10-11). */
    if (a->n_states >= DYN_AC_MAX_STATES)
        return -1;
    if (a->n_states == a->cap_states) {
        size_t cap = a->cap_states ? a->cap_states * 2 : 16;
        int32_t *ch = (int32_t *)realloc(a->child_head, cap * sizeof(int32_t));
        uint32_t *b;
        int32_t *f, *o, *ol;
        if (!ch)
            return -1;
        a->child_head = ch;
        b = (uint32_t *)realloc(a->base, cap * sizeof(uint32_t));
        if (!b) return -1;
        a->base = b;
        f = (int32_t *)realloc(a->fail, cap * sizeof(int32_t));
        if (!f) return -1;
        a->fail = f;
        o = (int32_t *)realloc(a->out, cap * sizeof(int32_t));
        if (!o) return -1;
        a->out = o;
        ol = (int32_t *)realloc(a->out_link, cap * sizeof(int32_t));
        if (!ol) return -1;
        a->out_link = ol;
        a->cap_states = cap;
    }
    a->child_head[a->n_states] = -1;
    a->base[a->n_states] = 0;
    a->fail[a->n_states] = 0;
    a->out[a->n_states] = -1;
    a->out_link[a->n_states] = -1;
    return (int)a->n_states++;
}

static int ac_new_edge(dyn_ac_t *a, uint8_t byte, int32_t target, int32_t next)
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

dyn_ac_t *dyn_ac_new(size_t n_pat)
{
    dyn_ac_t *a = (dyn_ac_t *)calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->n_pat = n_pat;
    a->ascii = 1;
    if (n_pat) {
        a->plen = (size_t *)calloc(n_pat, sizeof(size_t));
        if (!a->plen) {
            free(a);
            return NULL;
        }
    }
    if (ac_new_state(a) < 0) { /* the root */
        dyn_ac_free(a);
        return NULL;
    }
    return a;
}

void dyn_ac_free(dyn_ac_t *a)
{
    if (!a)
        return;
    free(a->go);
    free(a->tab);
    free(a->base);
    free(a->oo);
    free(a->fail);
    free(a->out);
    free(a->out_link);
    free(a->plen);
    free(a->child_head);
    free(a->e_byte);
    free(a->e_target);
    free(a->e_next);
    free(a);
}

int dyn_ac_insert(dyn_ac_t *a, const uint8_t *p, size_t len, int idx)
{
    int32_t s = 0;
    size_t i;

    if (idx >= 0 && (size_t)idx < a->n_pat)
        a->plen[idx] = len;
    for (i = 0; i < len; i++) {
        int32_t e = a->child_head[s];
        if (p[i] >= 0x80)
            a->ascii = 0;
        while (e >= 0 && a->e_byte[e] != p[i])
            e = a->e_next[e];
        if (e < 0) {
            int ns = ac_new_state(a);
            if (ns < 0)
                return -1;
            e = ac_new_edge(a, p[i], (int32_t)ns, a->child_head[s]);
            if (e < 0)
                return -1;
            a->child_head[s] = e;
            s = (int32_t)ns;
        } else {
            s = a->e_target[e];
        }
    }
    if (a->out[s] < 0)
        a->out[s] = idx;         /* first pattern wins on an exact duplicate */
    return 0;
}

/* children of s ascending by byte: the order the dense BFS has always
 * enqueued them in, and therefore the order that fixes identical
 * fail/out_link values (and identical emit streams) for both layouts */
static int ac_children(const dyn_ac_t *a, int32_t s, uint8_t *bytes,
                       int32_t *targets)
{
    int n = 0;
    int32_t e = a->child_head[s];
    while (e >= 0) {
        int i, pos = n;
        for (i = 0; i < n; i++)
            if (bytes[i] > a->e_byte[e]) { pos = i; break; }
        memmove(bytes + pos + 1, bytes + pos, (size_t)(n - pos));
        memmove(targets + pos + 1, targets + pos,
                (size_t)(n - pos) * sizeof *targets);
        bytes[pos] = a->e_byte[e];
        targets[pos] = a->e_target[e];
        n++;
        e = a->e_next[e];
    }
    return n;
}

/* ---- dense materialization (the original layout, small automata) ---------- */

static int ac_build_dense(dyn_ac_t *a)
{
    int32_t *queue = (int32_t *)malloc(a->n_states * sizeof(int32_t));
    size_t head = 0, tail = 0;
    uint8_t *bytes;
    int32_t *targets;
    int c;
    if (!queue)
        return -1;
    bytes = (uint8_t *)malloc(DYN_AC_ALPHABET);
    targets = (int32_t *)malloc(DYN_AC_ALPHABET * sizeof(int32_t));
    if (!bytes || !targets) {
        free(queue); free(bytes); free(targets);
        return -1;
    }

    /* materialize the trie rows from the edge list, then run the original
     * densification BFS over them unchanged */
    a->go = (int32_t *)malloc(a->n_states * DYN_AC_ALPHABET * sizeof(int32_t));
    if (!a->go)
        goto fail;
    for (size_t s = 0; s < a->n_states; s++)
        memset(a->go + s * DYN_AC_ALPHABET, 0xff,
               DYN_AC_ALPHABET * sizeof(int32_t));      /* -1 everywhere */
    for (size_t s = 0; s < a->n_states; s++) {
        int n = ac_children(a, (int32_t)s, bytes, targets);
        for (int i = 0; i < n; i++)
            a->go[s * DYN_AC_ALPHABET + bytes[i]] = targets[i];
    }

    for (c = 0; c < DYN_AC_ALPHABET; c++) {
        int32_t s = a->go[c];
        if (s < 0)
            a->go[c] = 0;                    /* root: missing edge stays put */
        else {
            a->fail[s] = 0;
            queue[tail++] = s;
        }
    }
    while (head < tail) {
        int32_t s = queue[head++];
        /* a state inherits its failure state's output chain, which is what
         * makes "she" report "he" as well without storing a list per state */
        a->out_link[s] = a->out[a->fail[s]] >= 0 ? a->fail[s]
                                                 : a->out_link[a->fail[s]];
        for (c = 0; c < DYN_AC_ALPHABET; c++) {
            int32_t nxt = a->go[(size_t)s * DYN_AC_ALPHABET + c];
            if (nxt < 0) {
                a->go[(size_t)s * DYN_AC_ALPHABET + c] =
                    a->go[(size_t)a->fail[s] * DYN_AC_ALPHABET + c];
            } else {
                a->fail[nxt] = a->go[(size_t)a->fail[s] * DYN_AC_ALPHABET + c];
                queue[tail++] = nxt;
            }
        }
    }
    free(queue);
    free(bytes);
    free(targets);
    return 0;

fail:
    free(queue);
    free(bytes);
    free(targets);
    return -1;
}

/* ---- compact materialization: double-array (large automata) ---------------- */

/* slot table growth: physical allocation carries 256 zeroed slack slots past
 * the logical size, because a probe computes base[s] + byte even for states
 * that own nothing (leaves) and byte reaches 0xFF -- the walk must read zeros
 * (free -> keep failing) there instead of past the allocation. Slots in the
 * slack are never claimable: rows must fit under the logical size, or a
 * realloc would re-zero owned slots. */
static int ac_tab_reserve(dyn_ac_t *a, size_t need)
{
    size_t cap = a->tab_cap ? a->tab_cap : 256;
    size_t old = a->tab_cap;
    dyn_ac_slot *t;
    while (cap < need)
        cap *= 2;
    if (cap == a->tab_cap)
        return 0;
    t = (dyn_ac_slot *)realloc(a->tab, (cap + 256) * sizeof(dyn_ac_slot));
    if (!t)
        return -1;
    memset(t + old, 0, (cap + 256 - old) * sizeof(dyn_ac_slot));
    a->tab = t;
    a->tab_cap = cap;
    return 0;
}

/* lowest free slot at or after hint */
static size_t ac_tab_free(const dyn_ac_t *a, size_t hint)
{
    while (hint < a->tab_cap && a->tab[hint].check)
        hint++;
    return hint;
}

/* claim the slot for byte b of state s's row (base[s] + b). The slot lives
 * at the first free position, which pins s's row base; free slots are always
 * above the root's 0..255 block, so base[s] = j - b never underflows. */
static int ac_place_one(dyn_ac_t *a, size_t hint, uint32_t s, uint32_t t,
                        uint8_t b, size_t *pos)
{
    size_t j = ac_tab_free(a, hint);
    if (j >= a->tab_cap) {
        if (ac_tab_reserve(a, a->tab_cap ? a->tab_cap * 2 : 256) < 0)
            return -1;
        j = ac_tab_free(a, j);
        if (j >= a->tab_cap)
            return -1;
    }
    a->tab[j].next = t;
    a->tab[j].check = s + 1;
    a->base[s] = (uint32_t)((ptrdiff_t)j - (ptrdiff_t)b);
    *pos = j;
    return 0;
}

static int ac_build_compact(dyn_ac_t *a)
{
    int32_t *queue = (int32_t *)malloc(a->n_states * sizeof(int32_t));
    dyn_ac_oo *oo = (dyn_ac_oo *)malloc(a->n_states * sizeof(dyn_ac_oo));
    size_t head = 0, tail = 0, hint;
    uint8_t *bytes;
    int32_t *targets;
    int nroot, i;

    if (!queue || !oo) {
        free(queue); free(oo);
        return -1;
    }
    bytes = (uint8_t *)malloc(DYN_AC_ALPHABET);
    targets = (int32_t *)malloc(DYN_AC_ALPHABET * sizeof(int32_t));
    if (!bytes || !targets) {
        free(queue); free(oo); free(bytes); free(targets);
        return -1;
    }

    /* the root owns slots 0..255 outright; missing bytes self-loop to root,
     * which is what terminates every fail walk (the dense root row's "missing
     * edge stays put" in compact form) */
    if (ac_tab_reserve(a, 256) < 0)
        goto fail;
    for (i = 0; i < 256; i++) {
        a->tab[i].next = 0;
        a->tab[i].check = 1;
    }
    a->base[0] = 0;
    nroot = ac_children(a, 0, bytes, targets);
    for (i = 0; i < nroot; i++) {
        a->tab[bytes[i]].next = (uint32_t)targets[i];
        a->fail[targets[i]] = 0;
        queue[tail++] = targets[i];
    }
    hint = 256;                     /* everything above the root row is free */

    while (head < tail) {
        int32_t s = queue[head++];
        int n = ac_children(a, s, bytes, targets);
        int32_t fs = a->fail[s];

        if (n == 0)
            continue;               /* a leaf owns no slots; its base stays
                                     * whatever its parent's placement left,
                                     * and every probe on it just misses */

        if (n == 1) {
            size_t j;
            if (ac_place_one(a, hint, (uint32_t)s, (uint32_t)targets[0],
                             bytes[0], &j) < 0)
                goto fail;
            if (j == hint)
                hint = ac_tab_free(a, hint + 1);
        } else {
            /* multi-child rows: try each free slot as the anchor of the
             * lowest byte and require the whole row clear */
            size_t j = ac_tab_free(a, hint);
            ptrdiff_t cand;
            for (;;) {
                int ok = 1;
                if (j >= a->tab_cap) {
                    if (ac_tab_reserve(a, a->tab_cap * 2) < 0)
                        goto fail;
                    j = ac_tab_free(a, j);
                    if (j >= a->tab_cap)
                        goto fail;
                }
                cand = (ptrdiff_t)j - (ptrdiff_t)bytes[0];
                if (cand >= 0) {
                    for (i = 0; i < n; i++) {
                        if (a->tab[cand + bytes[i]].check) { ok = 0; break; }
                    }
                    if (ok && (size_t)(cand + bytes[n - 1]) < a->tab_cap)
                        break;
                }
                j = ac_tab_free(a, j + 1);
            }
            for (i = 0; i < n; i++) {
                a->tab[cand + bytes[i]].next = (uint32_t)targets[i];
                a->tab[cand + bytes[i]].check = (uint32_t)s + 1;
            }
            a->base[s] = (uint32_t)cand;    /* the row base is s's, once */
            if ((size_t)j == hint)
                hint = ac_tab_free(a, hint + 1);
        }

        /* fail links for the new children: walk the fail chain of s until a
         * row owns byte b; the root's full row ends every walk. fail is a
         * proper suffix state, so its depth is strictly below t's: the walk
         * can never land on t itself. */
        for (i = 0; i < n; i++) {
            uint8_t b = bytes[i];
            int32_t t = targets[i], f = fs;
            for (;;) {
                uint32_t k = a->base[f] + b;
                if (a->tab[k].check == (uint32_t)f + 1) {
                    a->fail[t] = (int32_t)a->tab[k].next;
                    break;
                }
                f = a->fail[f];
            }
            a->out_link[t] = a->out[a->fail[t]] >= 0 ? a->fail[t]
                                                     : a->out_link[a->fail[t]];
            queue[tail++] = t;
        }
    }

    free(queue);
    free(bytes);
    free(targets);

    /* pack the output chain into 8 B pairs for the scan (one load per byte
     * on the no-emit path) and drop the separate arrays */
    for (size_t s2 = 0; s2 < a->n_states; s2++) {
        oo[s2].out = a->out[s2];
        oo[s2].out_link = a->out_link[s2];
    }
    free(a->out);
    free(a->out_link);
    a->out = NULL;
    a->out_link = NULL;
    a->oo = oo;
    return 0;

fail:
    free(queue);
    free(oo);
    free(bytes);
    free(targets);
    return -1;
}

/* ---- build dispatch and scan ---------------------------------------------- */

int dyn_ac_build(dyn_ac_t *a)
{
    int rc = a->n_states > DYN_AC_DENSE_MAX ? ac_build_compact(a)
                                            : ac_build_dense(a);
    /* the edge list and per-state scratch have one owner: the build. Both
     * layouts derive everything they need from them above. */
    free(a->child_head);
    free(a->e_byte);
    free(a->e_target);
    free(a->e_next);
    a->child_head = NULL;
    a->e_byte = NULL;
    a->e_target = NULL;
    a->e_next = NULL;
    a->n_edges = a->cap_edges = 0;
    if (rc == 0)
        a->compact = a->go == NULL;  /* compact never allocates go; dense
                                      * never allocates tab */
    return rc;
}

static int ac_run_dense(const dyn_ac_t *a, const uint8_t *t, size_t tlen,
                        dyn_ac_emit_t emit, void *ud)
{
    int32_t s = 0;
    size_t i;
    for (i = 0; i < tlen; i++) {
        int32_t o;
        s = a->go[(size_t)s * DYN_AC_ALPHABET + t[i]];
        for (o = a->out[s] >= 0 ? s : a->out_link[s]; o >= 0; o = a->out_link[o]) {
            if (emit(ud, a->out[o], i + 1))
                return 1;
        }
    }
    return 0;
}

static int ac_run_compact(const dyn_ac_t *a, const uint8_t *t, size_t tlen,
                          dyn_ac_emit_t emit, void *ud)
{
    const dyn_ac_slot *tab = a->tab;
    const uint32_t *base = a->base;
    const dyn_ac_oo *oo = a->oo;
    uint32_t s = 0;
    size_t i;

    for (i = 0; i < tlen; i++) {
        uint32_t c = t[i], k;
        int32_t o;
        /* base[0] == 0 by construction, so the root never needs the base
         * load -- the no-match steady state is the hottest loop shape */
        k = s ? base[s] + c : c;
        while (tab[k].check != s + 1) {
            s = (uint32_t)a->fail[s];
            k = s ? base[s] + c : c;
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

int dyn_ac_run(const dyn_ac_t *a, const uint8_t *t, size_t tlen,
               dyn_ac_emit_t emit, void *ud)
{
    if (a->compact)
        return ac_run_compact(a, t, tlen, emit, ud);
    return ac_run_dense(a, t, tlen, emit, ud);
}
