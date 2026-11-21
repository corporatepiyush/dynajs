#include <stdlib.h>
#include "dd.h"

#define PL_MAX_INTERVALS 1024
#define PL_CARD_SAMPLE 64
#define PL_KEY_COST 2

enum { FB_ALL = 0,
    FB_POINTS,
    FB_RANGE };

typedef struct {
    int kind;
    dyn_ks_buf pts;
    uint32_t* poff;
    uint32_t npts;
    dyn_ks_buf lo, hi;
    int lo_incl, hi_incl;
    int maybe_null;
} fbound;

typedef struct {
    uint32_t lo_off, lo_n, hi_off, hi_n;
    uint8_t lo_incl, hi_incl;
} iventry;

typedef struct {
    dyn_ks_buf buf;
    iventry* e;
    uint32_t n, cap;
} ivset;

static void fb_free(fbound* b)
{
    dyn_ks_free(&b->pts);
    dyn_ks_free(&b->lo);
    dyn_ks_free(&b->hi);
    free(b->poff);
    memset(b, 0, sizeof *b);
}

static void ivs_free(ivset* s)
{
    dyn_ks_free(&s->buf);
    free(s->e);
    memset(s, 0, sizeof *s);
}

static int ivs_add(ivset* s, const uint8_t* pre, size_t pn, const uint8_t* lo, size_t ln, int li, const uint8_t* hi,
    size_t hn, int hincl)
{
    iventry* v;
    if (s->n == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 8;
        iventry* ne = realloc(s->e, (size_t)nc * sizeof *ne);
        if (!ne)
            return -1;
        s->e = ne;
        s->cap = nc;
    }
    v = &s->e[s->n];
    v->lo_off = (uint32_t)s->buf.n;
    dyn_ks_raw(&s->buf, pre, pn);
    dyn_ks_raw(&s->buf, lo, ln);
    v->lo_n = (uint32_t)(pn + ln);
    v->hi_off = (uint32_t)s->buf.n;
    dyn_ks_raw(&s->buf, pre, pn);
    dyn_ks_raw(&s->buf, hi, hn);
    v->hi_n = (uint32_t)(pn + hn);
    v->lo_incl = (uint8_t)li;
    v->hi_incl = (uint8_t)hincl;
    s->n++;
    return s->buf.oom ? -1 : 0;
}

static int bcmp_prefix(const uint8_t* a, size_t an, const uint8_t* b, size_t bn)
{
    size_t m = an < bn ? an : bn;
    int c = memcmp(a, b, m);
    if (c)
        return c;
    return an < bn ? -1 : an > bn ? 1
                                  : 0;
}

static int arr_has_array(dyn_bson_val v)
{
    dyn_bson_iter it;
    dyn_bson_iter_init(&it, v.val, v.vlen);
    while (dyn_bson_iter_next(&it) > 0)
        if (it.type == DYN_BSON_ARRAY)
            return 1;
    return 0;
}

static int op_bound(const dd_fnode* n, fbound* out)
{
    dyn_bson_val a = n->arg;
    int rank;
    memset(out, 0, sizeof *out);
    if (n->kind != DD_F_FIELD || n->neg)
        return 0;
    switch (n->op) {
    case DD_OP_EQ:
        if (a.type == DYN_BSON_ARRAY || a.type == DYN_BSON_UNDEFINED)
            return 0;
        out->kind = FB_POINTS;
        out->poff = malloc(sizeof(uint32_t));
        if (!out->poff)
            return -1;
        out->poff[0] = 0;
        out->npts = 1;
        dyn_ks_value(&out->pts, a, 0);
        out->maybe_null = a.type == DYN_BSON_NULL;
        return out->pts.oom ? -1 : 1;
    case DD_OP_IN: {
        uint32_t i;
        if (n->ninre || arr_has_array(a))
            return 0;
        out->kind = FB_POINTS;
        out->poff = malloc((size_t)(n->nin ? n->nin : 1) * sizeof(uint32_t));
        if (!out->poff)
            return -1;
        for (i = 0; i < n->nin; i++) {
            out->poff[i] = (uint32_t)out->pts.n;
            dyn_ks_raw(&out->pts, n->inkeys[i], n->inklen[i]);
        }
        out->npts = n->nin;
        out->maybe_null = n->in_null;
        return out->pts.oom ? -1 : 1;
    }
    case DD_OP_GT:
    case DD_OP_GTE:
    case DD_OP_LT:
    case DD_OP_LTE:
        if (a.type == DYN_BSON_MINKEY || a.type == DYN_BSON_MAXKEY || a.type == DYN_BSON_ARRAY)
            return 0;
        rank = dyn_bson_canon_rank(a.type == DYN_BSON_UNDEFINED ? DYN_BSON_NULL : a.type);
        out->kind = FB_RANGE;
        out->maybe_null = rank == 5;
        if (n->op == DD_OP_GT || n->op == DD_OP_GTE) {
            dyn_ks_value(&out->lo, a, 0);
            out->lo_incl = n->op == DD_OP_GTE;
            dyn_ks_type_ceil(&out->hi, rank, 0);
            out->hi_incl = 0;
        } else {
            dyn_ks_type_floor(&out->lo, rank, 0);
            out->lo_incl = 1;
            dyn_ks_value(&out->hi, a, 0);
            out->hi_incl = n->op == DD_OP_LTE;
        }
        return out->lo.oom || out->hi.oom ? -1 : 1;
    case DD_OP_REGEX:
        out->kind = FB_RANGE;
        dyn_ks_type_floor(&out->lo, 15, 0);
        out->lo_incl = 1;
        dyn_ks_type_ceil(&out->hi, 15, 0);
        out->hi_incl = 0;
        return out->lo.oom || out->hi.oom ? -1 : 1;
    default:
        return 0;
    }
}

static int same_path(const dd_path* a, const dd_path* b)
{
    return a && b && a->tlen == b->tlen && memcmp(a->text, b->text, a->tlen) == 0;
}

static void fb_take(fbound* dst, fbound* src)
{
    fb_free(dst);
    *dst = *src;
    memset(src, 0, sizeof *src);
}

static int fb_merge(fbound* acc, fbound* nb, int multikey)
{
    if (acc->kind == FB_ALL) {
        fb_take(acc, nb);
        return 0;
    }
    if (multikey) {
        if (nb->kind == FB_POINTS && acc->kind == FB_RANGE)
            fb_take(acc, nb);
        fb_free(nb);
        return 0;
    }
    if (acc->kind == FB_POINTS) {
        if (nb->kind == FB_POINTS && nb->npts < acc->npts)
            fb_take(acc, nb);
        fb_free(nb);
        return 0;
    }
    if (nb->kind == FB_POINTS) {
        fb_take(acc, nb);
        return 0;
    }
    {
        int c = bcmp_prefix(nb->lo.p, nb->lo.n, acc->lo.p, acc->lo.n);
        if (c > 0 || (c == 0 && !nb->lo_incl)) {
            dyn_ks_buf t = acc->lo;
            acc->lo = nb->lo;
            nb->lo = t;
            acc->lo_incl = nb->lo_incl;
        }
        c = bcmp_prefix(nb->hi.p, nb->hi.n, acc->hi.p, acc->hi.n);
        if (c < 0 || (c == 0 && !nb->hi_incl)) {
            dyn_ks_buf t = acc->hi;
            acc->hi = nb->hi;
            nb->hi = t;
            acc->hi_incl = nb->hi_incl;
        }
        acc->maybe_null = acc->maybe_null && nb->maybe_null;
    }
    fb_free(nb);
    return 0;
}

static int gather_ops(const dd_fnode* n, const dd_path* want, fbound* acc, int multikey)
{
    uint32_t i;
    if (n->kind == DD_F_AND && !n->owns_path) {
        for (i = 0; i < n->nkids; i++)
            if (gather_ops(n->kids[i], want, acc, multikey) < 0)
                return -1;
        return 0;
    }
    if (n->kind == DD_F_AND && n->owns_path) {
        if (!same_path(n->path, want))
            return 0;
        for (i = 0; i < n->nkids; i++)
            if (gather_ops(n->kids[i], want, acc, multikey) < 0)
                return -1;
        return 0;
    }
    if (n->kind == DD_F_FIELD && same_path(n->path, want)) {
        fbound nb;
        int r = op_bound(n, &nb);
        if (r < 0) {
            fb_free(&nb);
            return -1;
        }
        if (r > 0)
            fb_merge(acc, &nb, multikey);
        else
            fb_free(&nb);
    }
    return 0;
}

static void invert(uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)~p[i];
}

static int ix_intervals(const dd_index* ix, const dd_fnode* root, ivset* out, int* field_n, int* rejected_sparse)
{
    fbound fb[32] DD_SCRATCH;
    ivset cur, nxt;
    int i, used = 0, rc = 0, nf = ix->nf < 32 ? ix->nf : 32;
    *field_n = 0;
    *rejected_sparse = 0;
    if (nf <= 0)
        return 0;
    memset(fb, 0, (size_t)nf * sizeof fb[0]);
    if (gather_ops(root, &ix->f[0].path, &fb[0], ix->multikey) < 0) {
        fb_free(&fb[0]);
        return -1;
    }
    if (fb[0].kind == FB_ALL)
        return 0;
    memset(&cur, 0, sizeof cur);
    memset(&nxt, 0, sizeof nxt);
    for (i = 1; i < nf; i++)
        if (gather_ops(root, &ix->f[i].path, &fb[i], ix->multikey) < 0) {
            rc = -1;
            goto done;
        }
    if (ix->sparse && fb[0].maybe_null) {
        *rejected_sparse = 1;
        goto done;
    }
    if (ivs_add(&cur, NULL, 0, NULL, 0, 1, NULL, 0, 1) < 0) {
        rc = -1;
        goto done;
    }
    for (i = 0; i < nf; i++) {
        uint32_t j, k;
        int desc = ix->f[i].desc;
        if (fb[i].kind == FB_ALL)
            break;
        if (fb[i].kind == FB_POINTS && (uint64_t)cur.n * (fb[i].npts ? fb[i].npts : 1) > PL_MAX_INTERVALS)
            break;
        if (fb[i].kind == FB_POINTS && desc)
            invert(fb[i].pts.p, fb[i].pts.n);
        if (fb[i].kind == FB_RANGE && desc) {
            dyn_ks_buf t = fb[i].lo;
            int ti = fb[i].lo_incl;
            invert(fb[i].lo.p, fb[i].lo.n);
            invert(fb[i].hi.p, fb[i].hi.n);
            fb[i].lo = fb[i].hi;
            fb[i].lo_incl = fb[i].hi_incl;
            fb[i].hi = t;
            fb[i].hi_incl = ti;
        }
        for (j = 0; j < cur.n; j++) {
            const uint8_t* pre = cur.buf.p ? cur.buf.p + cur.e[j].lo_off : NULL;
            size_t pn = cur.e[j].lo_n;
            if (fb[i].kind == FB_POINTS) {
                for (k = 0; k < fb[i].npts; k++) {
                    size_t a = fb[i].poff[k], b = k + 1 < fb[i].npts ? fb[i].poff[k + 1] : fb[i].pts.n;
                    if (ivs_add(&nxt, pre, pn, fb[i].pts.p + a, b - a, 1, fb[i].pts.p + a, b - a, 1) < 0) {
                        rc = -1;
                        goto done;
                    }
                }
            } else if (ivs_add(&nxt, pre, pn, fb[i].lo.p, fb[i].lo.n, fb[i].lo_incl, fb[i].hi.p, fb[i].hi.n,
                           fb[i].hi_incl)
                < 0) {
                rc = -1;
                goto done;
            }
        }
        ivs_free(&cur);
        cur = nxt;
        memset(&nxt, 0, sizeof nxt);
        used++;
        if (fb[i].kind == FB_RANGE)
            break;
    }
    *field_n = used;
    *out = cur;
    memset(&cur, 0, sizeof cur);
done:
    for (i = 0; i < nf; i++)
        fb_free(&fb[i]);
    ivs_free(&cur);
    ivs_free(&nxt);
    return rc;
}

static int scan_from_ivset(dd_ixplan* s, int ixno, ivset* iv, int field_n, int whole)
{
    uint32_t i;
    memset(s, 0, sizeof *s);
    s->ix = ixno;
    s->field_n = field_n;
    s->bounds = iv->buf.p;
    s->bounds_n = iv->buf.n;
    iv->buf.p = NULL;
    s->iv = calloc(iv->n ? iv->n : 1, sizeof *s->iv);
    if (!s->iv)
        return -1;
    for (i = 0; i < iv->n; i++) {
        s->iv[i].lo = s->bounds + iv->e[i].lo_off;
        s->iv[i].lo_n = iv->e[i].lo_n;
        s->iv[i].lo_incl = iv->e[i].lo_incl;
        s->iv[i].hi = s->bounds + iv->e[i].hi_off;
        s->iv[i].hi_n = iv->e[i].hi_n;
        s->iv[i].hi_incl = iv->e[i].hi_incl;
        s->iv[i].exact = whole && s->iv[i].lo_incl && s->iv[i].hi_incl && s->iv[i].lo_n == s->iv[i].hi_n
            && memcmp(s->iv[i].lo, s->iv[i].hi, s->iv[i].lo_n) == 0;
    }
    s->niv = (int)iv->n;
    return 0;
}

static void scan_free(dd_ixplan* s)
{
    free(s->iv);
    free(s->bounds);
    free(s->iv2);
    memset(s, 0, sizeof *s);
}

static int add_rejected(dd_plan* p, const char* name)
{
    char** nr = realloc(p->rejected, (size_t)(p->nrejected + 1) * sizeof *nr);
    size_t n = strlen(name);
    if (!nr)
        return -1;
    p->rejected = nr;
    nr[p->nrejected] = malloc(n + 1);
    if (!nr[p->nrejected])
        return -1;
    memcpy(nr[p->nrejected], name, n + 1);
    p->nrejected++;
    return 0;
}

static int push_scan(dd_plan* p, dd_ixplan* s)
{
    dd_ixplan* ns = realloc(p->scans, (size_t)(p->nscans + 1) * sizeof *ns);
    if (!ns)
        return -1;
    p->scans = ns;
    p->scans[p->nscans++] = *s;
    memset(s, 0, sizeof *s);
    return 0;
}

static uint64_t scan_cost(const dd_ixplan* s)
{
    uint64_t k = s->keys > UINT64_MAX / PL_KEY_COST ? UINT64_MAX : s->keys * PL_KEY_COST;
    return s->est > UINT64_MAX - k ? UINT64_MAX : s->est + k;
}

static int same_bound_fields(const dd_coll* c, const dd_ixplan* a, const dd_ixplan* b)
{
    const dd_index *x = c->ix[a->ix], *y = c->ix[b->ix];
    int i;
    if (a->field_n != b->field_n)
        return 0;
    for (i = 0; i < a->field_n; i++)
        if (!same_path(&x->f[i].path, &y->f[i].path))
            return 0;
    return 1;
}

static int scan_better(const dd_coll* c, const dd_ixplan* a, const dd_ixplan* b)
{
    uint64_t ca = scan_cost(a), cb = scan_cost(b);
    if (same_bound_fields(c, a, b) && a->keys != b->keys)
        return a->keys < b->keys;
    if (ca != cb)
        return ca < cb;
    if (a->field_n != b->field_n)
        return a->field_n > b->field_n;
    return c->ix[a->ix]->nf < c->ix[b->ix]->nf;
}

static int best_scans(dd_coll* c, const dd_fnode* root, dd_ixplan* best, dd_ixplan* second, int* nusable)
{
    int i;
    *nusable = 0;
    memset(best, 0, sizeof *best);
    memset(second, 0, sizeof *second);
    best->ix = second->ix = -1;
    for (i = 0; i < c->nix; i++) {
        dd_index* ix = c->ix[i];
        ivset iv;
        int fnum, sparse_rej;
        dd_ixplan s;
        memset(&iv, 0, sizeof iv);
        if (ix->partial)
            continue;
        if (ix_intervals(ix, root, &iv, &fnum, &sparse_rej) < 0) {
            ivs_free(&iv);
            return -1;
        }
        if (fnum == 0) {
            ivs_free(&iv);
            continue;
        }
        if (scan_from_ivset(&s, i, &iv, fnum, fnum == ix->nf) < 0) {
            ivs_free(&iv);
            scan_free(&s);
            return -1;
        }
        ivs_free(&iv);
        s.est = dd_index_scan_card(ix, s.iv, s.niv, &s.keys, PL_CARD_SAMPLE);
        if (s.est == UINT64_MAX) {
            scan_free(&s);
            return -1;
        }
        (*nusable)++;
        if (best->ix < 0 || scan_better(c, &s, best)) {
            scan_free(second);
            *second = *best;
            *best = s;
        } else if (second->ix < 0 || scan_better(c, &s, second)) {
            scan_free(second);
            *second = s;
        } else {
            scan_free(&s);
        }
    }
    return 0;
}

static const dd_fnode* find_id_eq(const dd_fnode* root)
{
    uint32_t i;
    if (root->kind != DD_F_AND)
        return NULL;
    for (i = 0; i < root->nkids; i++) {
        const dd_fnode* h = root->kids[i];
        if (h->kind == DD_F_AND && h->owns_path && h->path->tlen == 3 && memcmp(h->path->text, "_id", 3) == 0
            && h->nkids == 1) {
            const dd_fnode* k = h->kids[0];
            if (k->kind == DD_F_AND && k->nkids == 1)
                k = k->kids[0];
            if (k->kind == DD_F_FIELD && k->op == DD_OP_EQ && !k->neg && k->arg.type != DYN_BSON_DOC
                && k->arg.type != DYN_BSON_ARRAY && k->arg.type != DYN_BSON_NULL && k->arg.type != DYN_BSON_UNDEFINED)
                return k;
        }
    }
    return NULL;
}

static int sort_index(dd_coll* c, const dd_sort* s, int* rev)
{
    int i, j;
    if (!s || s->n == 0)
        return -1;
    for (i = 0; i < c->nix; i++) {
        dd_index* ix = c->ix[i];
        int dir = 0, ok = 1;
        if (ix->multikey || ix->sparse || ix->partial || ix->nf < s->n)
            continue;
        for (j = 0; j < s->n && ok; j++) {
            int same = (s->k[j].dir < 0) == (ix->f[j].desc != 0);
            int d = same ? 1 : -1;
            if (!same_path(&s->k[j].path, &ix->f[j].path) || (dir && d != dir))
                ok = 0;
            dir = d;
        }
        if (ok) {
            *rev = dir < 0;
            return i;
        }
    }
    return -1;
}

static int lead_sort_index(dd_coll* c, const dd_sort* s, int* rev)
{
    int i;
    for (i = 0; i < c->nix; i++) {
        dd_index* ix = c->ix[i];
        if (ix->multikey || ix->sparse || ix->partial || !same_path(&s->k[0].path, &ix->f[0].path))
            continue;
        *rev = (s->k[0].dir < 0) != (ix->f[0].desc != 0);
        return i;
    }
    return -1;
}

int dd_plan_make(dd_coll* c, const dd_filter* f, const dd_sort* sort, dd_plan* p, dd_err* e)
{
    const dd_fnode* root = f ? f->root : NULL;
    dd_ixplan best, second;
    int nusable = 0;
    uint32_t i;
    memset(p, 0, sizeof *p);
    p->sort_ix = -1;
    p->topk_ix = -1;
    p->stage = DD_PLAN_COLLSCAN;
    p->est = c->count;
    if (root && !f->empty) {
        const dd_fnode* id = find_id_eq(root);
        if (id) {
            p->stage = DD_PLAN_IDHACK;
            p->id_found = dd_coll_find_id(c, id->arg, &p->id_docno);
            if (!p->id_found)
                p->stage = DD_PLAN_EOF;
            p->est = p->id_found ? 1 : 0;
            return 0;
        }
        if (best_scans(c, root, &best, &second, &nusable) < 0)
            return dd_fail(e, DD_E_OOM, "out of memory planning query");
        if (best.ix >= 0) {
            int and = second.ix >= 0 && best.est > 64 && second.est <= best.est * 4
                && !same_path(&c->ix[best.ix]->f[0].path, &c->ix[second.ix]->f[0].path);
            p->stage = and ? DD_PLAN_IXAND : DD_PLAN_IXSCAN;
            p->est = best.est;
            if (push_scan(p, &best) < 0)
                goto oom;
            if (and) {
                if (push_scan(p, &second) < 0)
                    goto oom;
            } else if (second.ix >= 0) {
                if (add_rejected(p, c->ix[second.ix]->name) < 0)
                    goto oom;
                scan_free(&second);
            }
            goto with_sort;
        }
        for (i = 0; i < root->nkids; i++) {
            const dd_fnode* orn = root->kids[i];
            uint32_t b;
            uint64_t est = 0;
            if (orn->kind != DD_F_OR)
                continue;
            for (b = 0; b < orn->nkids; b++) {
                dd_ixplan bb, bs;
                int nu;
                if (best_scans(c, orn->kids[b], &bb, &bs, &nu) < 0)
                    goto oom;
                scan_free(&bs);
                if (bb.ix < 0)
                    break;
                est += bb.est;
                if (push_scan(p, &bb) < 0) {
                    scan_free(&bb);
                    goto oom;
                }
            }
            if (b == orn->nkids) {
                p->stage = DD_PLAN_OR;
                p->est = est < c->count ? est : c->count;
                goto with_sort;
            }
            while (p->nscans)
                scan_free(&p->scans[--p->nscans]);
            break;
        }
    }
with_sort:
    if (p->stage == DD_PLAN_COLLSCAN) {
        int rev = 0;
        p->sort_ix = sort_index(c, sort, &rev);
        p->sort_rev = rev;
        if (p->sort_ix >= 0) {
            p->topk = 3;
            p->topk_ix = p->sort_ix;
            p->topk_rev = rev;
        }
    } else if (p->stage != DD_PLAN_IDHACK && p->stage != DD_PLAN_EOF && sort && sort->n > 0) {
        const dd_index* ix = p->nscans == 1 ? c->ix[p->scans[0].ix] : NULL;
        int rev = 0;
        if (p->stage == DD_PLAN_IXSCAN && ix && ix->nf == 1 && !ix->multikey
            && same_path(&sort->k[0].path, &ix->f[0].path)) {
            p->topk = 1;
            p->topk_rev = (sort->k[0].dir < 0) != (ix->f[0].desc != 0);
        } else if ((p->topk_ix = lead_sort_index(c, sort, &rev)) >= 0) {
            p->topk = 2;
            p->topk_rev = rev;
        }
    }
    return 0;
oom:
    scan_free(&best);
    scan_free(&second);
    dd_plan_free(p);
    return dd_fail(e, DD_E_OOM, "out of memory planning query");
}

void dd_plan_free(dd_plan* p)
{
    int i;
    for (i = 0; i < p->nscans; i++)
        scan_free(&p->scans[i]);
    free(p->scans);
    for (i = 0; i < p->nrejected; i++)
        free(p->rejected[i]);
    free(p->rejected);
    free(p->scratch);
    memset(p, 0, sizeof *p);
    p->sort_ix = -1;
    p->topk_ix = -1;
}

const char* dd_plan_stage_name(int stage)
{
    switch (stage) {
    case DD_PLAN_IDHACK:
        return "IDHACK";
    case DD_PLAN_IXSCAN:
        return "IXSCAN";
    case DD_PLAN_IXAND:
        return "AND_SORTED";
    case DD_PLAN_OR:
        return "OR";
    case DD_PLAN_EOF:
        return "EOF";
    default:
        return "COLLSCAN";
    }
}

static int fill_roar(const dyn_roar_t* r, dd_idvec* out)
{
    uint64_t n = dyn_roar_card(r);
    if (n == 0)
        return 0;
    if (out->n + n > out->cap) {
        uint32_t* np = realloc(out->ids, (size_t)(out->n + n) * sizeof *np);
        if (!np)
            return -1;
        out->ids = np;
        out->cap = (uint32_t)(out->n + n);
    }
    dyn_roar_fill(r, out->ids + out->n);
    out->n += (uint32_t)n;
    return 0;
}

static int walk_index_order(dd_index* ix, int rev, dd_idvec* out, dd_stats* st)
{
    uint32_t i;
    if (dd_index_merge(ix) < 0)
        return -1;
    for (i = 0; i < ix->nsorted; i++) {
        uint32_t kid = ix->sorted[rev ? ix->nsorted - 1 - i : i];
        if (st)
            st->keys_examined++;
        if (fill_roar(&ix->post[kid], out) < 0)
            return -1;
    }
    return 0;
}

int dd_plan_candidates(dd_coll* c, dd_plan* p, dd_idvec* out, int* all, dd_stats* st)
{
    dyn_roar_t acc, tmp;
    int i, rc = 0;
    uint64_t ke = 0;
    *all = 0;
    out->n = 0;
    switch (p->stage) {
    case DD_PLAN_EOF:
        return 0;
    case DD_PLAN_IDHACK:
        if (st)
            st->keys_examined++;
        return p->id_found ? dd_idvec_push(out, p->id_docno) : 0;
    case DD_PLAN_COLLSCAN:
        if (p->sort_ix >= 0 && p->sort_ix < c->nix) {
            if (st)
                st->used_index_sort = 1;
            return walk_index_order(c->ix[p->sort_ix], p->sort_rev, out, st);
        }
        *all = 1;
        return 0;
    default:
        break;
    }
    memset(&acc, 0, sizeof acc);
    for (i = 0; i < p->nscans && rc == 0; i++) {
        dd_ixplan* s = &p->scans[i];
        if (s->ix < 0 || s->ix >= c->nix) {
            rc = -1;
            break;
        }
        if (i == 0 || p->stage == DD_PLAN_OR) {
            rc = dd_index_scan(c->ix[s->ix], s->iv, s->niv, &acc, &ke);
        } else {
            memset(&tmp, 0, sizeof tmp);
            rc = dd_index_scan(c->ix[s->ix], s->iv, s->niv, &tmp, &ke);
            if (rc == 0)
                rc = dyn_roar_op(&acc, &tmp, DYN_ROAR_AND);
            dyn_roar_clear(&tmp);
        }
    }
    if (rc == 0)
        rc = fill_roar(&acc, out);
    dyn_roar_clear(&acc);
    if (st)
        st->keys_examined += ke;
    return rc;
}

typedef struct {
    dd_coll* c;
    const dd_filter* f;
    dd_idvec* out;
    uint64_t want, got;
    uint64_t visited, budget;
} topk_walk;

static int topk_take(void* arg, const dyn_roar_t* post)
{
    topk_walk* w = arg;
    uint32_t i, j, from = w->out->n;
    if (fill_roar(post, w->out) < 0)
        return -1;
    w->visited += w->out->n - from;
    for (i = j = from; i < w->out->n; i++) {
        uint32_t id = w->out->ids[i];
        const uint8_t* doc = id < w->c->nslots ? w->c->doc[id] : NULL;
        dd_err e = { 0 };
        dd_match_ctx mc = { .pos = -1, .err = &e };
        if (!doc)
            continue;
        w->out->ids[j++] = id;
        if (!w->f || w->f->empty || dd_filter_match(w->f, doc, &mc) || mc.failed)
            w->got++;
    }
    w->out->n = j;
    if (w->got >= w->want)
        return 1;
    return w->visited > w->budget ? 2 : 0;
}

static int walk_all(dd_index* ix, int rev, topk_walk* w, uint64_t* ke)
{
    uint32_t i;
    int rc;
    if (dd_index_merge(ix) < 0)
        return -1;
    for (i = 0; i < ix->nsorted; i++) {
        uint32_t kid = ix->sorted[rev ? ix->nsorted - 1 - i : i];
        (*ke)++;
        if (dyn_roar_card(&ix->post[kid]) == 0)
            continue;
        rc = topk_take(w, &ix->post[kid]);
        if (rc)
            return rc < 0 ? -1 : rc - 1;
    }
    return 0;
}

static int topk_sorted(dd_coll* c, dd_plan* p, const dd_filter* f, uint64_t want, dd_idvec* out, dd_stats* st)
{
    topk_walk w = { c, f, out, want, 0, 0, UINT64_MAX };
    uint64_t ke = 0, v;
    int rc;
    if (p->topk_ix < 0 || p->topk_ix >= c->nix)
        return 1;
    if (p->topk == 2) {
        if (want >= c->count || p->est == 0)
            return 1;
        v = want * c->count / p->est;
        if (v > p->est / 8)
            return 1;
        w.budget = p->est / 4 + 64 < v * 4 + 1024 ? p->est / 4 + 64 : v * 4 + 1024;
    }
    out->n = 0;
    rc = walk_all(c->ix[p->topk_ix], p->topk_rev, &w, &ke);
    if (rc == 0 && p->topk == 3 && st)
        st->used_index_sort = 1;
    if (rc > 0)
        out->n = 0;
    if (st)
        st->keys_examined += ke;
    return rc;
}

int dd_plan_topk(dd_coll* c, dd_plan* p, const dd_filter* f, uint64_t want, dd_idvec* out, dd_stats* st)
{
    topk_walk w = { c, f, out, want, 0, 0, UINT64_MAX };
    uint64_t ke = 0;
    dd_ixplan* s;
    int rc;
    if (!p->topk || !want)
        return 1;
    if (p->topk >= 2)
        return topk_sorted(c, p, f, want, out, st);
    if (p->stage != DD_PLAN_IXSCAN || p->nscans != 1)
        return 1;
    s = &p->scans[0];
    if (s->ix < 0 || s->ix >= c->nix)
        return 1;
    out->n = 0;
    rc = dd_index_walk(c->ix[s->ix], s->iv, s->niv, p->topk_rev, topk_take, &w, &ke);
    if (rc > 0)
        out->n = 0;
    if (st)
        st->keys_examined += ke;
    return rc;
}

static int exact_scalar(dyn_bson_val a, int* numeric)
{
    *numeric = 0;
    switch (a.type) {
    case DYN_BSON_DOUBLE: {
        double d = dyn_bson_rdf64(a.val);
        *numeric = 1;
        return d == d;
    }
    case DYN_BSON_INT32:
    case DYN_BSON_INT64:
        *numeric = 1;
        return 1;
    case DYN_BSON_STRING:
    case DYN_BSON_DATE:
    case DYN_BSON_BOOL:
    case DYN_BSON_OID:
        return 1;
    default:
        return 0;
    }
}

typedef struct {
    int points, ranges, num, num_lower;
} exact_acc;

static int exact_leaves(const dd_fnode* n, const dd_path* want, exact_acc* a)
{
    uint32_t i;
    int numeric;
    if (n->kind == DD_F_AND) {
        for (i = 0; i < n->nkids; i++)
            if (!exact_leaves(n->kids[i], want, a))
                return 0;
        return 1;
    }
    if (n->kind != DD_F_FIELD || n->neg || !same_path(n->path, want))
        return 0;
    switch (n->op) {
    case DD_OP_EQ:
    case DD_OP_IN:
        a->points++;
        return 1;
    case DD_OP_GT:
    case DD_OP_GTE:
    case DD_OP_LT:
    case DD_OP_LTE:
        if (!exact_scalar(n->arg, &numeric))
            return 0;
        a->ranges++;
        a->num |= numeric;
        if (numeric && (n->op == DD_OP_GT || n->op == DD_OP_GTE))
            a->num_lower = 1;
        return 1;
    default:
        return 0;
    }
}

int dd_plan_count(dd_coll* c, const dd_plan* p, const dd_filter* f, uint64_t* n)
{
    const dd_ixplan* s;
    dd_index* ix;
    exact_acc a = { 0, 0, 0, 0 };
    uint64_t k;
    if (!f || f->empty || !f->root || p->stage != DD_PLAN_IXSCAN || p->nscans != 1)
        return 0;
    s = &p->scans[0];
    if (s->ix < 0 || s->ix >= c->nix)
        return 0;
    ix = c->ix[s->ix];
    if (ix->nf != 1 || ix->multikey || ix->partial || !exact_leaves(f->root, &ix->f[0].path, &a))
        return 0;
    if (!(a.points == 1 && a.ranges == 0) && !(a.points == 0 && a.ranges > 0 && (!a.num || a.num_lower)))
        return 0;
    k = dd_index_scan_card(ix, s->iv, s->niv, NULL, UINT64_MAX);
    if (k == UINT64_MAX)
        return -1;
    *n = k;
    return 1;
}

static int rep_cmp(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static int rep_present(dd_coll* c, dd_index* ix, uint32_t d, dd_idvec* out)
{
    dyn_bson_val v;
    if (d >= c->nslots || !c->doc[d] || !dd_path_get_exact(c->doc[d], &ix->f[0].path, &v))
        return 0;
    if (v.type == DYN_BSON_ARRAY)
        return 2;
    return dd_idvec_push(out, d) < 0 ? -1 : 1;
}

int dd_index_distinct_reps(dd_coll* c, dd_index* ix, dd_idvec* out)
{
    dd_idvec all = { NULL, 0, 0 };
    uint32_t i, j;
    int rc = 0;
    out->n = 0;
    if (ix->nf != 1 || ix->multikey || ix->sparse || ix->partial)
        return 1;
    if (dd_index_merge(ix) < 0)
        return -1;
    for (i = 0; i < ix->nsorted && rc >= 0 && rc != 2; i++) {
        const dyn_roar_t* r = &ix->post[ix->sorted[i]];
        uint32_t d;
        if (dyn_roar_card(r) == 0)
            continue;
        rc = dyn_roar_select(r, 0, &d) ? rep_present(c, ix, d, out) : 0;
        if (rc != 0)
            continue;
        all.n = 0;
        if (fill_roar(r, &all) < 0) {
            rc = -1;
            break;
        }
        for (j = 1; j < all.n && rc == 0; j++)
            rc = rep_present(c, ix, all.ids[j], out);
    }
    dd_idvec_free(&all);
    if (rc < 0)
        return -1;
    if (rc == 2)
        return 1;
    if (out->n > 1)
        qsort(out->ids, out->n, sizeof *out->ids, rep_cmp);
    return 0;
}
