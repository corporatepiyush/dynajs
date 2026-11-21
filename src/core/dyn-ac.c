#include "dyn-ac.h"

#include <stdlib.h>
#include <string.h>

static int ac_new_state(dyn_ac_t* a)
{
    if (a->n_states >= DYN_AC_MAX_STATES)
        return -1;
    if (a->n_states == a->cap_states) {
        size_t cap = a->cap_states ? a->cap_states * 2 : 16;
        int32_t* ch = (int32_t*)realloc(a->child_head, cap * sizeof(int32_t));
        uint32_t* b;
        int32_t *f, *o, *ol;
        if (!ch)
            return -1;
        a->child_head = ch;
        b = (uint32_t*)realloc(a->base, cap * sizeof(uint32_t));
        if (!b)
            return -1;
        a->base = b;
        f = (int32_t*)realloc(a->fail, cap * sizeof(int32_t));
        if (!f)
            return -1;
        a->fail = f;
        o = (int32_t*)realloc(a->out, cap * sizeof(int32_t));
        if (!o)
            return -1;
        a->out = o;
        ol = (int32_t*)realloc(a->out_link, cap * sizeof(int32_t));
        if (!ol)
            return -1;
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

static int ac_new_edge(dyn_ac_t* a, uint8_t byte, int32_t target, int32_t next)
{
    if (a->n_edges == a->cap_edges) {
        size_t cap = a->cap_edges ? a->cap_edges * 2 : 32;
        uint8_t* eb = (uint8_t*)realloc(a->e_byte, cap);
        int32_t *et, *en;
        if (!eb)
            return -1;
        a->e_byte = eb;
        et = (int32_t*)realloc(a->e_target, cap * sizeof(int32_t));
        if (!et)
            return -1;
        a->e_target = et;
        en = (int32_t*)realloc(a->e_next, cap * sizeof(int32_t));
        if (!en)
            return -1;
        a->e_next = en;
        a->cap_edges = cap;
    }
    a->e_byte[a->n_edges] = byte;
    a->e_target[a->n_edges] = target;
    a->e_next[a->n_edges] = next;
    return (int)a->n_edges++;
}

dyn_ac_t* dyn_ac_new(size_t n_pat)
{
    dyn_ac_t* a = (dyn_ac_t*)calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->n_pat = n_pat;
    a->ascii = 1;
    if (n_pat) {
        a->plen = (size_t*)calloc(n_pat, sizeof(size_t));
        if (!a->plen) {
            free(a);
            return NULL;
        }
    }
    if (ac_new_state(a) < 0) {
        dyn_ac_free(a);
        return NULL;
    }
    return a;
}

void dyn_ac_free(dyn_ac_t* a)
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

int dyn_ac_insert(dyn_ac_t* a, const uint8_t* p, size_t len, int idx)
{
    int32_t s = 0;
    size_t i;

    if (a->built || idx < 0 || (size_t)idx >= a->n_pat)
        return -1;
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
        a->out[s] = idx;
    return 0;
}

static int ac_children(const dyn_ac_t* a, int32_t s, uint8_t* bytes,
    int32_t* targets)
{
    int n = 0;
    int32_t e = a->child_head[s];
    while (e >= 0) {
        int i, pos = n;
        for (i = 0; i < n; i++)
            if (bytes[i] > a->e_byte[e]) {
                pos = i;
                break;
            }
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

static int ac_build_dense(dyn_ac_t* a)
{
    int32_t* queue = (int32_t*)malloc(a->n_states * sizeof(int32_t));
    size_t head = 0, tail = 0;
    uint8_t* bytes;
    int32_t* targets;
    int c;
    if (!queue)
        return -1;
    bytes = (uint8_t*)malloc(DYN_AC_ALPHABET);
    targets = (int32_t*)malloc(DYN_AC_ALPHABET * sizeof(int32_t));
    if (!bytes || !targets) {
        free(queue);
        free(bytes);
        free(targets);
        return -1;
    }

    a->go = (int32_t*)malloc(a->n_states * DYN_AC_ALPHABET * sizeof(int32_t));
    if (!a->go)
        goto fail;
    for (size_t s = 0; s < a->n_states; s++)
        memset(a->go + s * DYN_AC_ALPHABET, 0xff,
            DYN_AC_ALPHABET * sizeof(int32_t));
    for (size_t s = 0; s < a->n_states; s++) {
        int n = ac_children(a, (int32_t)s, bytes, targets);
        for (int i = 0; i < n; i++)
            a->go[s * DYN_AC_ALPHABET + bytes[i]] = targets[i];
    }

    for (c = 0; c < DYN_AC_ALPHABET; c++) {
        int32_t s = a->go[c];
        if (s < 0)
            a->go[c] = 0;
        else {
            a->fail[s] = 0;
            queue[tail++] = s;
        }
    }
    while (head < tail) {
        int32_t s = queue[head++];
        a->out_link[s] = a->out[a->fail[s]] >= 0 ? a->fail[s]
                                                 : a->out_link[a->fail[s]];
        for (c = 0; c < DYN_AC_ALPHABET; c++) {
            int32_t nxt = a->go[(size_t)s * DYN_AC_ALPHABET + c];
            if (nxt < 0) {
                a->go[(size_t)s * DYN_AC_ALPHABET + c] = a->go[(size_t)a->fail[s] * DYN_AC_ALPHABET + c];
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

static int ac_tab_reserve(dyn_ac_t* a, size_t need)
{
    size_t cap = a->tab_cap ? a->tab_cap : 256;
    size_t old = a->tab_cap;
    dyn_ac_slot* t;
    while (cap < need)
        cap *= 2;
    if (cap == a->tab_cap)
        return 0;
    t = (dyn_ac_slot*)realloc(a->tab, (cap + 256) * sizeof(dyn_ac_slot));
    if (!t)
        return -1;
    memset(t + old, 0, (cap + 256 - old) * sizeof(dyn_ac_slot));
    a->tab = t;
    a->tab_cap = cap;
    return 0;
}

static size_t ac_tab_free(const dyn_ac_t* a, size_t hint)
{
    while (hint < a->tab_cap && a->tab[hint].check)
        hint++;
    return hint;
}

static int ac_place_one(dyn_ac_t* a, size_t hint, uint32_t s, uint32_t t,
    uint8_t b, size_t* pos)
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

static int ac_build_compact(dyn_ac_t* a)
{
    int32_t* queue = (int32_t*)malloc(a->n_states * sizeof(int32_t));
    dyn_ac_oo* oo = (dyn_ac_oo*)malloc(a->n_states * sizeof(dyn_ac_oo));
    size_t head = 0, tail = 0, hint;
    uint8_t* bytes;
    int32_t* targets;
    int nroot, i;

    if (!queue || !oo) {
        free(queue);
        free(oo);
        return -1;
    }
    bytes = (uint8_t*)malloc(DYN_AC_ALPHABET);
    targets = (int32_t*)malloc(DYN_AC_ALPHABET * sizeof(int32_t));
    if (!bytes || !targets) {
        free(queue);
        free(oo);
        free(bytes);
        free(targets);
        return -1;
    }

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
    hint = 256;

    while (head < tail) {
        int32_t s = queue[head++];
        int n = ac_children(a, s, bytes, targets);
        int32_t fs = a->fail[s];

        if (n == 0)
            continue;

        if (n == 1) {
            size_t j;
            if (ac_place_one(a, hint, (uint32_t)s, (uint32_t)targets[0],
                    bytes[0], &j)
                < 0)
                goto fail;
            if (j == hint)
                hint = ac_tab_free(a, hint + 1);
        } else {
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
                        if (a->tab[cand + bytes[i]].check) {
                            ok = 0;
                            break;
                        }
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
            a->base[s] = (uint32_t)cand;
            if ((size_t)j == hint)
                hint = ac_tab_free(a, hint + 1);
        }

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

int dyn_ac_build(dyn_ac_t* a)
{
    int rc;
    if (a->built)
        return -1;
    rc = a->n_states > DYN_AC_DENSE_MAX ? ac_build_compact(a)
                                        : ac_build_dense(a);
    if (rc != 0)
        return rc;
    free(a->child_head);
    free(a->e_byte);
    free(a->e_target);
    free(a->e_next);
    a->child_head = NULL;
    a->e_byte = NULL;
    a->e_target = NULL;
    a->e_next = NULL;
    a->n_edges = a->cap_edges = 0;
    a->compact = a->go == NULL;
    a->built = 1;
    return 0;
}

static int ac_run_dense(const dyn_ac_t* a, const uint8_t* t, size_t tlen,
    dyn_ac_emit_t emit, void* ud)
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

static int ac_run_compact(const dyn_ac_t* a, const uint8_t* t, size_t tlen,
    dyn_ac_emit_t emit, void* ud)
{
    const dyn_ac_slot* tab = a->tab;
    const uint32_t* base = a->base;
    const dyn_ac_oo* oo = a->oo;
    uint32_t s = 0;
    size_t i;

    for (i = 0; i < tlen; i++) {
        uint32_t c = t[i], k;
        int32_t o;
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

int dyn_ac_run(const dyn_ac_t* a, const uint8_t* t, size_t tlen,
    dyn_ac_emit_t emit, void* ud)
{
    if (!a->built)
        return 0;
    if (a->compact)
        return ac_run_compact(a, t, tlen, emit, ud);
    return ac_run_dense(a, t, tlen, emit, ud);
}
