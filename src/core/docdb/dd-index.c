#include <stdio.h>
#include <stdlib.h>
#include "../dyn-par.h"
#include "dd.h"

#define IX_MAX_FIELDS 32

typedef struct {
    dyn_ks_buf* kb;
    uint32_t* offs;
    uint32_t n, cap;
    int desc;
    int from_array;
    int oom;
} ix_collect;

static int ix_has_key(const ix_collect* c, size_t start)
{
    uint32_t i;
    size_t len = c->kb->n - start;
    for (i = 0; i < c->n; i++) {
        size_t a = c->offs[i], b = i + 1 < c->n ? c->offs[i + 1] : start;
        if (b - a == len && memcmp(c->kb->p + a, c->kb->p + start, len) == 0)
            return 1;
    }
    return 0;
}

static int ix_visit(void* ud, dyn_bson_val v, int is_elem, int32_t aidx)
{
    ix_collect* c = ud;
    size_t start = c->kb->n;
    if (is_elem || aidx >= 0)
        c->from_array = 1;
    if (!is_elem && v.type == DYN_BSON_ARRAY) {
        if (v.vlen > 5) {
            c->from_array = 1;
            return 0;
        }
        c->from_array = 1;
        v.type = DYN_BSON_UNDEFINED;
        v.vlen = 0;
        dyn_ks_type_floor(c->kb, 1, c->desc);
    } else if (v.type == 0) {
        dyn_ks_missing(c->kb, c->desc);
    } else {
        dyn_ks_value(c->kb, v, c->desc);
    }
    if (c->kb->oom) {
        c->oom = 1;
        return 1;
    }
    if (ix_has_key(c, start)) {
        c->kb->n = start;
        return 0;
    }
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 8;
        uint32_t* np = realloc(c->offs, (size_t)nc * sizeof *np);
        if (!np) {
            c->oom = 1;
            return 1;
        }
        c->offs = np;
        c->cap = nc;
    }
    c->offs[c->n++] = (uint32_t)start;
    return 0;
}

static int ix_field_present(const uint8_t* doc, const dd_path* p)
{
    dyn_bson_val v;
    int i;
    v.type = DYN_BSON_DOC;
    v.val = doc;
    for (i = 0; i < p->n; i++) {
        if (v.type != DYN_BSON_DOC)
            return 1;
        if (!dyn_bson_find(v.val, p->part[i], p->plen[i], &v))
            return 0;
    }
    return 1;
}

static int ix_keys_one(const dd_index* ix, const uint8_t* doc, dyn_ks_buf* out, uint32_t** offs, uint32_t* nkeys,
    uint32_t* cap, dd_err* e)
{
    ix_collect col;
    memset(&col, 0, sizeof col);
    col.kb = out;
    col.desc = ix->f[0].desc;
    col.offs = *offs;
    col.cap = *cap;
    dd_path_walk(doc, &ix->f[0].path, ix_visit, &col);
    *offs = col.offs;
    *cap = col.cap;
    if (col.oom || out->oom)
        return dd_fail(e, DD_E_OOM, "out of memory");
    if (col.n == 0) {
        if (*cap < 1) {
            uint32_t* np = realloc(*offs, sizeof *np);
            if (!np)
                return dd_fail(e, DD_E_OOM, "out of memory");
            *offs = np;
            *cap = 1;
        }
        dyn_ks_type_floor(out, 1, col.desc);
        if (out->oom)
            return dd_fail(e, DD_E_OOM, "out of memory");
        (*offs)[0] = 0;
        col.n = 1;
    }
    *nkeys = col.n;
    return 0;
}

static __attribute__((noinline)) int ix_keys_multi(const dd_index* ix, const uint8_t* doc, dyn_ks_buf* out,
    uint32_t** offs, uint32_t* nkeys, uint32_t* cap, dd_err* e)
{
    dyn_ks_buf fk[IX_MAX_FIELDS];
    ix_collect col[IX_MAX_FIELDS];
    int i, multi = -1, rc = 0;
    uint32_t k;
    for (i = 0; i < ix->nf; i++) {
        dyn_ks_init(&fk[i]);
        memset(&col[i], 0, sizeof col[i]);
        col[i].kb = &fk[i];
        col[i].desc = ix->f[i].desc;
    }
    for (i = 0; i < ix->nf; i++) {
        dd_path_walk(doc, &ix->f[i].path, ix_visit, &col[i]);
        if (col[i].oom) {
            rc = dd_fail(e, DD_E_OOM, "out of memory");
            goto done;
        }
        if (col[i].n == 0) {
            dyn_ks_type_floor(&fk[i], 1, col[i].desc);
            col[i].offs = malloc(sizeof(uint32_t));
            if (!col[i].offs) {
                rc = dd_fail(e, DD_E_OOM, "out of memory");
                goto done;
            }
            col[i].offs[0] = 0;
            col[i].n = 1;
            col[i].cap = 1;
        }
        if (col[i].from_array || col[i].n > 1) {
            if (multi >= 0) {
                rc = dd_fail(e, DD_E_PARALLELARRAYS, "cannot index parallel arrays [%s] [%s]", ix->f[multi].path.text,
                    ix->f[i].path.text);
                goto done;
            }
            multi = i;
        }
    }
    {
        uint32_t nk = multi >= 0 ? col[multi].n : 1;
        if (nk > *cap) {
            uint32_t* np = realloc(*offs, (size_t)nk * sizeof *np);
            if (!np) {
                rc = dd_fail(e, DD_E_OOM, "out of memory");
                goto done;
            }
            *offs = np;
            *cap = nk;
        }
        for (k = 0; k < nk; k++) {
            (*offs)[k] = (uint32_t)out->n;
            for (i = 0; i < ix->nf; i++) {
                uint32_t j = i == multi ? k : 0;
                size_t a = col[i].offs[j], b = j + 1 < col[i].n ? col[i].offs[j + 1] : fk[i].n;
                dyn_ks_raw(out, fk[i].p + a, b - a);
            }
        }
        *nkeys = nk;
        if (out->oom)
            rc = dd_fail(e, DD_E_OOM, "out of memory");
    }
done:
    for (i = 0; i < ix->nf; i++) {
        dyn_ks_free(&fk[i]);
        free(col[i].offs);
    }
    return rc;
}

int dd_index_doc_keys(const dd_index* ix, const uint8_t* doc, dyn_ks_buf* out, uint32_t** offs, uint32_t* nkeys,
    uint32_t* cap, dd_err* e)
{
    int i;
    *nkeys = 0;
    out->n = 0;
    out->oom = 0;
    if (ix->sparse) {
        int any = 0;
        for (i = 0; i < ix->nf && !any; i++)
            any = ix_field_present(doc, &ix->f[i].path);
        if (!any)
            return 0;
    }
    if (ix->partial) {
        dd_match_ctx mc = { .pos = -1, .err = NULL };
        if (!dd_filter_match(ix->partial, doc, &mc))
            return 0;
    }
    if (ix->nf == 1)
        return ix_keys_one(ix, doc, out, offs, nkeys, cap, e);
    return ix_keys_multi(ix, doc, out, offs, nkeys, cap, e);
}

static int ix_push_tail(dd_index* ix, uint32_t id)
{
    if (ix->ntail == ix->captail) {
        uint32_t nc = ix->captail ? ix->captail * 2 : 64;
        uint32_t* nt = realloc(ix->tail, (size_t)nc * sizeof *nt);
        if (!nt)
            return -1;
        ix->tail = nt;
        ix->captail = nc;
    }
    ix->tail[ix->ntail++] = id;
    ix->inord[id] = 1;
    return 0;
}

static int ix_new_key(dd_index* ix, const uint8_t* k, size_t n, uint32_t* kid)
{
    uint32_t id = ix->nkeys, existing;
    int r;
    if (ix->nkeys == ix->capkeys) {
        uint32_t nc = ix->capkeys ? ix->capkeys * 2 : 64;
        dyn_roar_t* np = realloc(ix->post, (size_t)nc * sizeof *np);
        uint32_t *no, *nl;
        uint8_t* ni;
        if (!np)
            return -1;
        ix->post = np;
        no = realloc(ix->koff, (size_t)nc * sizeof *no);
        if (!no)
            return -1;
        ix->koff = no;
        nl = realloc(ix->klen, (size_t)nc * sizeof *nl);
        if (!nl)
            return -1;
        ix->klen = nl;
        ni = realloc(ix->inord, nc);
        if (!ni)
            return -1;
        ix->inord = ni;
        ix->capkeys = nc;
    }
    r = dd_kmap_put(&ix->keys, k, n, id, &existing);
    if (r < 0)
        return -1;
    if (r == 1) {
        *kid = existing;
        if (!ix->inord[existing] && ix_push_tail(ix, existing) < 0)
            return -1;
        return 0;
    }
    memset(&ix->post[id], 0, sizeof ix->post[id]);
    ix->koff[id] = (uint32_t)(ix->keys.pool_n - n);
    ix->klen[id] = (uint32_t)n;
    ix->inord[id] = 0;
    ix->nkeys++;
    if (ix_push_tail(ix, id) < 0)
        return -1;
    *kid = id;
    return 1;
}

int dd_index_lookup_eq(const dd_index* ix, const uint8_t* k, size_t n, uint32_t* kid)
{
    return dd_kmap_get(&ix->keys, k, n, kid);
}

static int ix_add_keys(dd_index* ix, uint32_t docno, const dyn_ks_buf* kb, const uint32_t* offs, uint32_t nk, dd_err* e)
{
    uint32_t k;
    if (nk > 1)
        ix->multikey = 1;
    for (k = 0; k < nk; k++) {
        size_t a = offs[k], b = k + 1 < nk ? offs[k + 1] : kb->n;
        uint32_t kid;
        int created = ix_new_key(ix, kb->p + a, b - a, &kid);
        if (created < 0)
            return dd_fail(e, DD_E_OOM, "out of memory");
        if (!created && dyn_roar_card(&ix->post[kid]) == 0 && ix->nempty > 0)
            ix->nempty--;
        if (dyn_roar_add(&ix->post[kid], docno) < 0)
            return dd_fail(e, DD_E_OOM, "out of memory");
        ix->entries++;
    }
    return 0;
}

int dd_index_add(dd_index* ix, uint32_t docno, const uint8_t* doc, dd_err* e)
{
    uint32_t nk = 0;
    int rc = dd_index_doc_keys(ix, doc, &ix->skb, &ix->soffs, &nk, &ix->scap, e);
    if (rc == 0)
        rc = ix_add_keys(ix, docno, &ix->skb, ix->soffs, nk, e);
    return rc < 0 ? -1 : 0;
}

void dd_index_remove(dd_index* ix, uint32_t docno, const uint8_t* doc)
{
    uint32_t nk = 0, k;
    if (dd_index_doc_keys(ix, doc, &ix->skb, &ix->soffs, &nk, &ix->scap, NULL) != 0)
        return;
    for (k = 0; k < nk; k++) {
        size_t a = ix->soffs[k], b = k + 1 < nk ? ix->soffs[k + 1] : ix->skb.n;
        uint32_t kid;
        if (dd_kmap_get(&ix->keys, ix->skb.p + a, b - a, &kid) && dyn_roar_has(&ix->post[kid], docno)) {
            dyn_roar_remove(&ix->post[kid], docno);
            ix->entries--;
            if (dyn_roar_card(&ix->post[kid]) == 0)
                ix->nempty++;
        }
    }
}

static int ix_unique_ok(const dd_index* ix, uint32_t docno, const uint8_t* doc, const dyn_ks_buf* kb, const uint32_t* offs,
    uint32_t nk, dd_err* e);

int dd_index_check_unique(dd_index* ix, uint32_t docno, const uint8_t* doc, dd_err* e)
{
    uint32_t nk = 0;
    int rc;
    if (!ix->unique && ix->nf == 1)
        return 0;
    rc = dd_index_doc_keys(ix, doc, &ix->skb, &ix->soffs, &nk, &ix->scap, e);
    if (rc == 0 && ix->unique)
        rc = ix_unique_ok(ix, docno, doc, &ix->skb, ix->soffs, nk, e);
    return rc < 0 ? -1 : 0;
}

static int ix_unique_ok(const dd_index* ix, uint32_t docno, const uint8_t* doc, const dyn_ks_buf* kb, const uint32_t* offs,
    uint32_t nk, dd_err* e)
{
    uint32_t k;
    int rc = 0;
    for (k = 0; k < nk; k++) {
        size_t a = offs[k], b = k + 1 < nk ? offs[k + 1] : kb->n;
        uint32_t kid;
        uint64_t card;
        if (!dd_kmap_get(&ix->keys, kb->p + a, b - a, &kid))
            continue;
        card = dyn_roar_card(&ix->post[kid]);
        if (card == 0 || (card == 1 && docno != UINT32_MAX && dyn_roar_has(&ix->post[kid], docno)))
            continue;
        {
            char vals[240];
            size_t o = 0;
            int i;
            vals[0] = 0;
            for (i = 0; i < ix->nf && o + 8 < sizeof vals; i++) {
                dyn_bson_val v;
                char r[120];
                if (!dd_path_get_exact(doc, &ix->f[i].path, &v))
                    v.type = DYN_BSON_NULL;
                o += (size_t)snprintf(vals + o, sizeof vals - o, "%s%s: %s", i ? ", " : "", ix->f[i].path.text,
                    dd_repr(v, r, sizeof r));
            }
            rc = dd_fail(e, DD_E_DUPKEY, "E11000 duplicate key error collection: %s.%s index: %s dup key: { %s }",
                ix->owner && ix->owner->db ? ix->owner->db->name : "", ix->owner ? ix->owner->name : "", ix->name, vals);
        }
        break;
    }
    return rc;
}

static const uint8_t* ix_key(const dd_index* ix, uint32_t kid, uint32_t* n)
{
    *n = ix->klen[kid];
    return dd_kmap_key(&ix->keys, ix->koff[kid]);
}

static int ix_kcmp(const dd_index* ix, uint32_t a, uint32_t b)
{
    uint32_t na, nb;
    const uint8_t* pa = ix_key(ix, a, &na);
    const uint8_t* pb = ix_key(ix, b, &nb);
    uint32_t m = na < nb ? na : nb;
    int c = memcmp(pa, pb, m);
    if (c)
        return c;
    return na < nb ? -1 : na > nb ? 1
                                  : 0;
}

static void ix_sort_ids(const dd_index* ix, uint32_t* v, uint32_t* tmp, uint32_t n)
{
    uint32_t w, i;
    if (n < 2)
        return;
    for (w = 1; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            uint32_t lo = i, mid = i + w < n ? i + w : n, hi = i + 2 * w < n ? i + 2 * w : n;
            uint32_t a = lo, b = mid, o = lo;
            while (a < mid && b < hi)
                tmp[o++] = ix_kcmp(ix, v[a], v[b]) <= 0 ? v[a++] : v[b++];
            while (a < mid)
                tmp[o++] = v[a++];
            while (b < hi)
                tmp[o++] = v[b++];
        }
        memcpy(v, tmp, (size_t)n * sizeof *v);
    }
}

int dd_index_merge(dd_index* ix)
{
    uint32_t *tmp, *merged, a = 0, b = 0, o = 0, total;
    if (ix->ntail == 0 && ix->nempty * 4 <= ix->nsorted + 16)
        return 0;
    total = ix->nsorted + ix->ntail;
    tmp = malloc((size_t)(ix->ntail ? ix->ntail : 1) * sizeof *tmp);
    merged = malloc((size_t)(total ? total : 1) * sizeof *merged);
    if (!tmp || !merged) {
        free(tmp);
        free(merged);
        return -1;
    }
    ix_sort_ids(ix, ix->tail, tmp, ix->ntail);
    while (a < ix->nsorted || b < ix->ntail) {
        uint32_t pick;
        if (b >= ix->ntail || (a < ix->nsorted && ix_kcmp(ix, ix->sorted[a], ix->tail[b]) <= 0))
            pick = ix->sorted[a++];
        else
            pick = ix->tail[b++];
        if (dyn_roar_card(&ix->post[pick]) == 0) {
            ix->inord[pick] = 0;
            continue;
        }
        merged[o++] = pick;
    }
    free(tmp);
    free(ix->sorted);
    ix->sorted = merged;
    ix->nsorted = o;
    ix->capsorted = total ? total : 1;
    ix->ntail = 0;
    ix->nempty = 0;
    return 0;
}

static int ix_cmp_bound(const dd_index* ix, uint32_t kid, const uint8_t* k, uint32_t n)
{
    uint32_t na;
    const uint8_t* pa = ix_key(ix, kid, &na);
    uint32_t m = na < n ? na : n;
    int c = memcmp(pa, k, m);
    if (c)
        return c;
    return na < n ? -1 : 0;
}

static uint32_t ix_lower(const dd_index* ix, const uint8_t* k, uint32_t n, int incl)
{
    uint32_t lo = 0, hi = ix->nsorted;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = ix_cmp_bound(ix, ix->sorted[mid], k, n);
        if (c < 0 || (c == 0 && !incl))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static uint32_t ix_upper(const dd_index* ix, const uint8_t* k, uint32_t n, int incl)
{
    uint32_t lo = 0, hi = ix->nsorted;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = ix_cmp_bound(ix, ix->sorted[mid], k, n);
        if (c < 0 || (c == 0 && incl))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static const dyn_roar_t* ix_exact(const dd_index* ix, const dd_interval* iv)
{
    uint32_t kid;
    if (!dd_kmap_get(&ix->keys, iv->lo, iv->lo_n, &kid) || !ix->inord[kid])
        return NULL;
    return &ix->post[kid];
}

#define IX_OR_BATCH 256

static int ix_or_flush(dyn_roar_t* out, const dyn_roar_t** pend, uint32_t* np)
{
    uint32_t i, n = *np;
    int rc = 0;
    *np = 0;
    if (n > 4)
        return dyn_roar_or_many(out, pend, n);
    for (i = 0; i < n && rc == 0; i++)
        rc = dyn_roar_op(out, pend[i], DYN_ROAR_OR);
    return rc;
}

static int ix_or_push(dyn_roar_t* out, const dyn_roar_t** pend, uint32_t* np, const dyn_roar_t* p)
{
    if (dyn_roar_card(p) == 0)
        return 0;
    pend[(*np)++] = p;
    return *np == IX_OR_BATCH ? ix_or_flush(out, pend, np) : 0;
}

int dd_index_scan(dd_index* ix, const dd_interval* iv, int niv, dyn_roar_t* out, uint64_t* keys_examined)
{
    const dyn_roar_t* pend[IX_OR_BATCH] DD_SCRATCH;
    uint32_t np = 0;
    int i;
    if (dd_index_merge(ix) < 0)
        return -1;
    for (i = 0; i < niv; i++) {
        uint32_t a, b;
        if (iv[i].exact) {
            const dyn_roar_t* p = ix_exact(ix, &iv[i]);
            if (!p)
                continue;
            if (keys_examined)
                (*keys_examined)++;
            if (ix_or_push(out, pend, &np, p) < 0)
                return -1;
            continue;
        }
        a = ix_lower(ix, iv[i].lo, iv[i].lo_n, iv[i].lo_incl);
        b = ix_upper(ix, iv[i].hi, iv[i].hi_n, iv[i].hi_incl);
        for (; a < b; a++) {
            if (keys_examined)
                (*keys_examined)++;
            if (ix_or_push(out, pend, &np, &ix->post[ix->sorted[a]]) < 0)
                return -1;
        }
    }
    return ix_or_flush(out, pend, &np);
}

int dd_index_walk(dd_index* ix, const dd_interval* iv, int niv, int rev, dd_ix_take take, void* arg,
    uint64_t* keys_examined)
{
    uint32_t span[2 * 64];
    int i, rc;
    if (niv > 64 || dd_index_merge(ix) < 0)
        return niv > 64 ? 1 : -1;
    for (i = 0; i < niv; i++) {
        span[2 * i] = ix_lower(ix, iv[i].lo, iv[i].lo_n, iv[i].lo_incl);
        span[2 * i + 1] = ix_upper(ix, iv[i].hi, iv[i].hi_n, iv[i].hi_incl);
        if (span[2 * i + 1] < span[2 * i])
            span[2 * i + 1] = span[2 * i];
        if (i && span[2 * i] < span[2 * i - 1])
            return 1;
    }
    for (i = 0; i < niv; i++) {
        int k = rev ? niv - 1 - i : i;
        uint32_t a = span[2 * k], b = span[2 * k + 1], j;
        for (j = 0; j < b - a; j++) {
            uint32_t kid = ix->sorted[rev ? b - 1 - j : a + j];
            if (keys_examined)
                (*keys_examined)++;
            if (dyn_roar_card(&ix->post[kid]) == 0)
                continue;
            rc = take(arg, &ix->post[kid]);
            if (rc)
                return rc < 0 ? -1 : 0;
        }
    }
    return 0;
}

uint64_t dd_index_scan_card(dd_index* ix, const dd_interval* iv, int niv, uint64_t* keys_examined, uint64_t cap)
{
    uint64_t total = 0;
    int i;
    if (dd_index_merge(ix) < 0)
        return UINT64_MAX;
    for (i = 0; i < niv; i++) {
        uint32_t a, b, span, j, look;
        uint64_t part = 0;
        if (iv[i].exact) {
            const dyn_roar_t* p = ix_exact(ix, &iv[i]);
            if (p && keys_examined)
                (*keys_examined)++;
            total += p ? dyn_roar_card(p) : 0;
            continue;
        }
        a = ix_lower(ix, iv[i].lo, iv[i].lo_n, iv[i].lo_incl);
        b = ix_upper(ix, iv[i].hi, iv[i].hi_n, iv[i].hi_incl);
        span = b > a ? b - a : 0;
        look = span < cap ? span : (uint32_t)cap;
        for (j = 0; j < look; j++)
            part += dyn_roar_card(&ix->post[ix->sorted[a + j]]);
        if (keys_examined)
            *keys_examined += span;
        total += look == span ? part : (look ? part * span / look : 0);
    }
    return total;
}

void dd_index_free(dd_index* ix)
{
    uint32_t i;
    int f;
    if (!ix)
        return;
    for (i = 0; i < ix->nkeys; i++)
        dyn_roar_clear(&ix->post[i]);
    free(ix->post);
    free(ix->koff);
    free(ix->klen);
    free(ix->sorted);
    free(ix->tail);
    free(ix->inord);
    dyn_ks_free(&ix->skb);
    free(ix->soffs);
    dd_kmap_free(&ix->keys);
    for (f = 0; f < ix->nf; f++)
        dd_path_free(&ix->f[f].path);
    free(ix->f);
    if (ix->partial) {
        dd_filter_free(ix->partial);
        free(ix->partial);
    }
    free(ix->spec);
    free(ix->name);
    free(ix);
}

dd_index* dd_index_find(dd_coll* c, const char* name, size_t n)
{
    int i;
    for (i = 0; i < c->nix; i++)
        if (strlen(c->ix[i]->name) == n && memcmp(c->ix[i]->name, name, n) == 0)
            return c->ix[i];
    return NULL;
}

static int ix_default_name(dd_index* ix, const uint8_t* keys)
{
    dyn_bson_iter it;
    size_t n = 0, cap = 64;
    char* s = malloc(cap);
    if (!s)
        return -1;
    dyn_bson_iter_init(&it, keys, dyn_bson_rd32(keys));
    while (dyn_bson_iter_next(&it) > 0) {
        char num[32];
        int nl;
        dyn_bson_val v = dyn_bson_iter_val(&it);
        double d = dyn_bson_num_f64(v);
        nl = snprintf(num, sizeof num, "%s%g", n ? "_" : "", d);
        if (n + it.klen + (size_t)nl + 4 > cap) {
            char* ns;
            while (n + it.klen + (size_t)nl + 4 > cap)
                cap *= 2;
            ns = realloc(s, cap);
            if (!ns) {
                free(s);
                return -1;
            }
            s = ns;
        }
        if (n)
            s[n++] = '_';
        memcpy(s + n, it.key, it.klen);
        n += it.klen;
        nl = snprintf(s + n, cap - n, "_%g", d);
        n += (size_t)nl;
    }
    s[n] = 0;
    ix->name = s;
    return 0;
}

static int ix_spec_build(dd_index* ix, const uint8_t* keys, dd_err* e)
{
    dyn_bson_w w;
    dyn_bson_w_init(&w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_int32(&w, "v", 1, 2);
    dyn_bson_w_key(&w, DYN_BSON_DOC, "key", 3);
    dyn_bson_w_bytes(&w, keys, dyn_bson_rd32(keys));
    dyn_bson_w_string(&w, "name", 4, ix->name, strlen(ix->name));
    if (ix->unique)
        dyn_bson_w_bool(&w, "unique", 6, 1);
    if (ix->sparse)
        dyn_bson_w_bool(&w, "sparse", 6, 1);
    if (ix->partial) {
        dyn_bson_w_key(&w, DYN_BSON_DOC, "partialFilterExpression", 23);
        dyn_bson_w_bytes(&w, ix->partial->src, ix->partial->src_n);
    }
    dyn_bson_w_doc_end(&w);
    if (w.oom) {
        dyn_bson_w_free(&w);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    ix->spec = w.p;
    ix->spec_n = w.n;
    return 0;
}

static int ix_same_keys(const dd_index* ix, const uint8_t* keys)
{
    dyn_bson_val k;
    if (!dyn_bson_find(ix->spec, "key", 3, &k))
        return 0;
    return dyn_bson_rd32(k.val) == dyn_bson_rd32(keys) && memcmp(k.val, keys, dyn_bson_rd32(keys)) == 0;
}

static int ix_create(dd_coll* c, const uint8_t* keys, const uint8_t* opts, dd_index** out, dd_err* e, int fill)
{
    dd_index* ix;
    dyn_bson_iter it;
    dyn_bson_val v;
    int nf = 0, i;
    *out = NULL;
    dyn_bson_iter_init(&it, keys, dyn_bson_rd32(keys));
    while (dyn_bson_iter_next(&it) > 0) {
        double dir;
        if (!dyn_bson_is_number(it.type))
            return dd_fail(e, DD_E_BADVALUE, "Values in the index key pattern can only be 1 or -1 (field '%.*s')", (int)it.klen, it.key);
        dir = dyn_bson_num_f64(dyn_bson_iter_val(&it));
        if (dir != 1 && dir != -1)
            return dd_fail(e, DD_E_BADVALUE, "Values in the index key pattern can only be 1 or -1 (field '%.*s')", (int)it.klen, it.key);
        nf++;
    }
    if (nf == 0)
        return dd_fail(e, DD_E_BADVALUE, "Index keys cannot be an empty object");
    if (nf > IX_MAX_FIELDS)
        return dd_fail(e, DD_E_BADVALUE, "Index has more than %d fields", IX_MAX_FIELDS);
    ix = calloc(1, sizeof *ix);
    if (ix)
        ix->owner = c;
    if (!ix)
        return dd_fail(e, DD_E_OOM, "out of memory");
    ix->f = calloc((size_t)nf, sizeof *ix->f);
    if (!ix->f) {
        free(ix);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    dd_kmap_init(&ix->keys);
    dyn_bson_iter_init(&it, keys, dyn_bson_rd32(keys));
    while (dyn_bson_iter_next(&it) > 0) {
        if (dd_path_parse(&ix->f[ix->nf].path, it.key, it.klen, e) < 0) {
            dd_index_free(ix);
            return -1;
        }
        ix->f[ix->nf].desc = dyn_bson_num_f64(dyn_bson_iter_val(&it)) < 0;
        ix->nf++;
    }
    if (opts) {
        if (dyn_bson_find(opts, "unique", 6, &v) && v.type == DYN_BSON_BOOL)
            ix->unique = v.val[0];
        if (dyn_bson_find(opts, "sparse", 6, &v) && v.type == DYN_BSON_BOOL)
            ix->sparse = v.val[0];
        if (dyn_bson_find(opts, "partialFilterExpression", 23, &v)) {
            if (v.type != DYN_BSON_DOC) {
                dd_index_free(ix);
                return dd_fail(e, DD_E_BADVALUE, "partialFilterExpression must be an object");
            }
            ix->partial = calloc(1, sizeof *ix->partial);
            if (!ix->partial || dd_filter_compile(ix->partial, v.val, NULL, e) < 0) {
                if (!ix->partial)
                    dd_fail(e, DD_E_OOM, "out of memory");
                dd_index_free(ix);
                return -1;
            }
            if (ix->partial->has_regex) {
                dd_index_free(ix);
                return dd_fail(e, DD_E_BADVALUE, "a partialFilterExpression may not use $regex");
            }
        }
        if (dyn_bson_find(opts, "name", 4, &v)) {
            if (v.type != DYN_BSON_STRING || dyn_bson_rd32(v.val) <= 1) {
                dd_index_free(ix);
                return dd_fail(e, DD_E_BADVALUE, "index name must be a non-empty string");
            }
            ix->name = strndup((const char*)v.val + 4, dyn_bson_rd32(v.val) - 1);
        }
    }
    if (!ix->name && ix_default_name(ix, keys) < 0) {
        dd_index_free(ix);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    if (!ix->name) {
        dd_index_free(ix);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    if (strcmp(ix->name, "_id_") == 0) {
        dd_index_free(ix);
        return dd_fail(e, DD_E_BADVALUE, "the _id_ index is implicit");
    }
    if (ix_spec_build(ix, keys, e) < 0) {
        dd_index_free(ix);
        return -1;
    }
    for (i = 0; i < c->nix; i++) {
        dd_index* o = c->ix[i];
        int same_name = strcmp(o->name, ix->name) == 0, same_keys = ix_same_keys(o, keys);
        if (same_name && same_keys && o->unique == ix->unique && o->sparse == ix->sparse && !o->partial == !ix->partial) {
            dd_index_free(ix);
            *out = o;
            return 0;
        }
        if (same_name) {
            int code = same_keys ? DD_E_INDEXOPTIONSCONFLICT : DD_E_INDEXKEYSPECSCONFLICT;
            dd_index_free(ix);
            return dd_fail(e, code, "An existing index has the same name as the requested index but different %s", same_keys ? "options" : "keys");
        }
        if (same_keys && !o->partial && !ix->partial) {
            dd_index_free(ix);
            return dd_fail(e, DD_E_INDEXOPTIONSCONFLICT, "Index already exists with a different name: %s", o->name);
        }
    }
    if (fill && dd_index_fill(c, ix, e) < 0) {
        dd_index_free(ix);
        return -1;
    }
    {
        dd_index** np = realloc(c->ix, (size_t)(c->nix + 1) * sizeof *np);
        if (!np) {
            dd_index_free(ix);
            return dd_fail(e, DD_E_OOM, "out of memory");
        }
        c->ix = np;
        c->ix[c->nix++] = ix;
    }
    *out = ix;
    return 1;
}

static int ix_fill_seq(const dd_coll* c, dd_index* ix, dd_err* e)
{
    dyn_ks_buf kb;
    uint32_t *offs = NULL, nk = 0, cap = 0, d;
    int rc = 0;
    dyn_ks_init(&kb);
    for (d = 0; d < c->nslots && rc == 0; d++) {
        if (!c->doc[d])
            continue;
        rc = dd_index_doc_keys(ix, c->doc[d], &kb, &offs, &nk, &cap, e);
        if (rc == 0 && ix->unique)
            rc = ix_unique_ok(ix, UINT32_MAX, c->doc[d], &kb, offs, nk, e);
        if (rc == 0)
            rc = ix_add_keys(ix, d, &kb, offs, nk, e);
    }
    dyn_ks_free(&kb);
    free(offs);
    return rc < 0 ? -1 : 0;
}

#define IX_PAR_MIN_DOCS 4096
#define IX_PAR_MAX 64

typedef struct {
    dd_kmap local;
    uint32_t* loff;
    uint32_t* llen;
    uint32_t nloc, caploc;
    uint32_t* lids;
    uint32_t nl, capl;
    uint32_t* cnt;
    uint32_t fail_at;
    int oom;
    dd_err err;
} ix_part;

typedef struct {
    const dd_coll* c;
    dd_index** ix;
    int nix;
    ix_part* parts;
    dd_err* err;
    int* rc;
} ix_fill_job;

static int part_push(uint32_t** v, uint32_t* n, uint32_t* cap, uint32_t x)
{
    if (*n == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 256;
        uint32_t* np = realloc(*v, (size_t)nc * sizeof *np);
        if (!np)
            return -1;
        *v = np;
        *cap = nc;
    }
    (*v)[(*n)++] = x;
    return 0;
}

static int part_keys(ix_part* pt, const dyn_ks_buf* kb, const uint32_t* offs, uint32_t nk)
{
    uint32_t k;
    for (k = 0; k < nk; k++) {
        size_t a = offs[k], b = k + 1 < nk ? offs[k + 1] : kb->n;
        uint32_t lid = pt->nloc, have;
        int r = dd_kmap_put(&pt->local, kb->p + a, b - a, lid, &have);
        if (r < 0)
            return -1;
        if (r == 1) {
            lid = have;
        } else {
            uint32_t cl = pt->caploc;
            if (part_push(&pt->loff, &pt->nloc, &pt->caploc, (uint32_t)(pt->local.pool_n - (b - a))) < 0)
                return -1;
            pt->nloc--;
            pt->caploc = cl;
            if (part_push(&pt->llen, &pt->nloc, &pt->caploc, (uint32_t)(b - a)) < 0)
                return -1;
        }
        if (part_push(&pt->lids, &pt->nl, &pt->capl, lid) < 0)
            return -1;
    }
    return 0;
}

static void ix_fill_keys(ix_fill_job* j, unsigned w, unsigned nw)
{
    const dd_coll* c = j->c;
    uint32_t lo = (uint32_t)((uint64_t)c->nslots * w / nw), hi = (uint32_t)((uint64_t)c->nslots * (w + 1) / nw), d;
    dyn_ks_buf kb;
    uint32_t *offs = NULL, cap = 0;
    int i;
    dyn_ks_init(&kb);
    for (i = 0; i < j->nix; i++) {
        ix_part* pt = &j->parts[(size_t)w * (size_t)j->nix + (size_t)i];
        pt->fail_at = UINT32_MAX;
        pt->cnt = malloc((size_t)(hi > lo ? hi - lo : 1) * sizeof *pt->cnt);
        if (!pt->cnt) {
            pt->oom = 1;
            pt->fail_at = lo;
        }
    }
    for (d = lo; d < hi; d++) {
        const uint8_t* doc = c->doc[d];
        for (i = 0; i < j->nix; i++) {
            ix_part* pt = &j->parts[(size_t)w * (size_t)j->nix + (size_t)i];
            uint32_t nk = 0;
            if (pt->fail_at != UINT32_MAX)
                continue;
            if (doc && dd_index_doc_keys(j->ix[i], doc, &kb, &offs, &nk, &cap, &pt->err) < 0) {
                pt->fail_at = d;
                continue;
            }
            if (part_keys(pt, &kb, offs, nk) < 0) {
                pt->oom = 1;
                pt->fail_at = d;
                continue;
            }
            pt->cnt[d - lo] = nk;
        }
    }
    dyn_ks_free(&kb);
    free(offs);
}

static int ix_fill_merge(ix_fill_job* j, int i, unsigned nw, dd_err* e)
{
    const dd_coll* c = j->c;
    dd_index* ix = j->ix[i];
    uint32_t* l2g = NULL;
    unsigned w;
    int rc = 0;
    for (w = 0; w < nw && rc == 0; w++) {
        ix_part* pt = &j->parts[(size_t)w * (size_t)j->nix + (size_t)i];
        uint32_t lo = (uint32_t)((uint64_t)c->nslots * w / nw), hi = (uint32_t)((uint64_t)c->nslots * (w + 1) / nw);
        uint32_t d, l, at = 0;
        uint32_t* nl = realloc(l2g, (size_t)(pt->nloc ? pt->nloc : 1) * sizeof *nl);
        if (!nl) {
            rc = dd_fail(e, DD_E_OOM, "out of memory");
            break;
        }
        l2g = nl;
        for (l = 0; l < pt->nloc; l++)
            if (ix_new_key(ix, dd_kmap_key(&pt->local, pt->loff[l]), pt->llen[l], &l2g[l]) < 0) {
                rc = dd_fail(e, DD_E_OOM, "out of memory");
                break;
            }
        for (d = lo; d < hi && rc == 0; d++) {
            uint32_t nk, k;
            if (d == pt->fail_at) {
                if (pt->oom)
                    rc = dd_fail(e, DD_E_OOM, "out of memory");
                else {
                    *e = pt->err;
                    rc = -1;
                }
                break;
            }
            nk = pt->cnt[d - lo];
            if (ix->unique)
                for (k = 0; k < nk; k++)
                    if (dyn_roar_card(&ix->post[l2g[pt->lids[at + k]]]) > 0) {
                        dyn_ks_buf kb;
                        uint32_t *offs = NULL, n2 = 0, cap = 0;
                        dyn_ks_init(&kb);
                        rc = dd_index_doc_keys(ix, c->doc[d], &kb, &offs, &n2, &cap, e);
                        if (rc == 0)
                            rc = ix_unique_ok(ix, UINT32_MAX, c->doc[d], &kb, offs, n2, e);
                        dyn_ks_free(&kb);
                        free(offs);
                        if (rc == 0)
                            rc = dd_fail(e, DD_E_INTERNAL, "index fill lost a duplicate");
                        break;
                    }
            if (rc)
                break;
            if (nk > 1)
                ix->multikey = 1;
            for (k = 0; k < nk; k++, at++) {
                if (dyn_roar_add(&ix->post[l2g[pt->lids[at]]], d) < 0) {
                    rc = dd_fail(e, DD_E_OOM, "out of memory");
                    break;
                }
                ix->entries++;
            }
        }
    }
    free(l2g);
    return rc < 0 ? -1 : 0;
}

static void ix_fill_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    ix_fill_job* j = arg;
    int i;
    ix_fill_keys(j, w, nw);
    dyn_par_barrier(par);
    for (i = (int)w; i < j->nix; i += (int)nw)
        j->rc[i] = ix_fill_merge(j, i, nw, &j->err[i]);
}

void dd_index_fill_many(const dd_coll* c, dd_index** ix, int nix, dd_err* err, int* rc)
{
    ix_fill_job j;
    unsigned nw = c->nslots >= IX_PAR_MIN_DOCS ? dyn_par_plan(c->nslots, IX_PAR_MIN_DOCS / 4, IX_PAR_MAX) : 1;
    size_t k, np;
    int i;
    if (nw <= 1 || nix <= 0) {
        for (i = 0; i < nix; i++)
            rc[i] = ix_fill_seq(c, ix[i], &err[i]);
        return;
    }
    np = (size_t)IX_PAR_MAX * (size_t)nix;
    j.c = c;
    j.ix = ix;
    j.nix = nix;
    j.err = err;
    j.rc = rc;
    j.parts = calloc(np, sizeof *j.parts);
    if (!j.parts) {
        for (i = 0; i < nix; i++)
            rc[i] = ix_fill_seq(c, ix[i], &err[i]);
        return;
    }
    for (k = 0; k < np; k++)
        dd_kmap_init(&j.parts[k].local);
    dyn_par_run(nw, ix_fill_worker, &j);
    for (k = 0; k < np; k++) {
        dd_kmap_free(&j.parts[k].local);
        free(j.parts[k].loff);
        free(j.parts[k].llen);
        free(j.parts[k].lids);
        free(j.parts[k].cnt);
    }
    free(j.parts);
}

int dd_index_fill(const dd_coll* c, dd_index* ix, dd_err* e)
{
    return ix_fill_seq(c, ix, e);
}

int dd_index_create(dd_coll* c, const uint8_t* keys, const uint8_t* opts, dd_index** out, dd_err* e)
{
    return ix_create(c, keys, opts, out, e, 1);
}

int dd_index_create_unfilled(dd_coll* c, const uint8_t* keys, const uint8_t* opts, dd_index** out, dd_err* e)
{
    return ix_create(c, keys, opts, out, e, 0);
}

int dd_index_drop(dd_coll* c, const char* name, size_t n, dd_err* e)
{
    int i;
    for (i = 0; i < c->nix; i++) {
        if (strlen(c->ix[i]->name) == n && memcmp(c->ix[i]->name, name, n) == 0) {
            dd_index_free(c->ix[i]);
            memmove(c->ix + i, c->ix + i + 1, (size_t)(c->nix - i - 1) * sizeof *c->ix);
            c->nix--;
            return 0;
        }
    }
    if (n == 4 && memcmp(name, "_id_", 4) == 0)
        return dd_fail(e, DD_E_INVALIDOPTIONS, "cannot drop _id index");
    return dd_fail(e, DD_E_INDEXNOTFOUND, "index not found with name [%.*s]", (int)n, name);
}
