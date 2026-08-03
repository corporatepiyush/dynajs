/*
 * dyn-ac -- Aho-Corasick, pure C. See dyn-ac.h.
 *
 * Lifted verbatim from src/dyna-matcher.c, where it was already pure: the
 * automaton never touched a JSValue, only the constructor and the scan
 * wrappers did. That is the rare case the entanglement survey (CLAUDE.md §12)
 * calls a real lift rather than a rewrite -- the cut was already there.
 */
#include "dyn-ac.h"

#include <stdlib.h>
#include <string.h>

static int dyn_ac_new_state(dyn_ac_t *a)
{
    if (a->n_states == a->cap_states) {
        size_t cap = a->cap_states ? a->cap_states * 2 : 16;
        int32_t *g = (int32_t *)realloc(a->go,
                                        cap * DYN_AC_ALPHABET * sizeof(int32_t));
        int32_t *f, *o, *ol;
        if (!g)
            return -1;
        a->go = g;
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
    memset(a->go + a->n_states * DYN_AC_ALPHABET, 0xff,
           DYN_AC_ALPHABET * sizeof(int32_t));      /* -1 everywhere */
    a->fail[a->n_states] = 0;
    a->out[a->n_states] = -1;
    a->out_link[a->n_states] = -1;
    return (int)a->n_states++;
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
    if (dyn_ac_new_state(a) < 0) { /* the root */
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
    free(a->fail);
    free(a->out);
    free(a->out_link);
    free(a->plen);
    free(a);
}

int dyn_ac_insert(dyn_ac_t *a, const uint8_t *p, size_t len, int idx)
{
    int32_t s = 0;
    size_t i;

    if (idx >= 0 && (size_t)idx < a->n_pat)
        a->plen[idx] = len;
    for (i = 0; i < len; i++) {
        int32_t *row = a->go + (size_t)s * DYN_AC_ALPHABET;
        if (p[i] >= 0x80)
            a->ascii = 0;
        if (row[p[i]] < 0) {
            int ns = dyn_ac_new_state(a);
            if (ns < 0)
                return -1;
            /* the row pointer may have moved: re-derive it */
            a->go[(size_t)s * DYN_AC_ALPHABET + p[i]] = ns;
            s = ns;
        } else {
            s = row[p[i]];
        }
    }
    if (a->out[s] < 0)
        a->out[s] = idx;         /* first pattern wins on an exact duplicate */
    return 0;
}

int dyn_ac_build(dyn_ac_t *a)
{
    int32_t *queue = (int32_t *)malloc(a->n_states * sizeof(int32_t));
    size_t head = 0, tail = 0;
    int c;
    if (!queue)
        return -1;
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
    return 0;
}

int dyn_ac_run(const dyn_ac_t *a, const uint8_t *t, size_t tlen,
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
