#include "urlrec.h"

void dyn_sp_list_free(dyn_sp_list_t* l)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        free(l->a[i].k);
        free(l->a[i].v);
    }
    free(l->a);
    memset(l, 0, sizeof(*l));
}

static int dyn_sp_list_grow(dyn_sp_list_t* l)
{
    size_t nc = l->cap ? l->cap * 2 : 8;
    dyn_sp_pair_t* na = (dyn_sp_pair_t*)realloc(l->a, nc * sizeof(*na));
    if (!na)
        return -1;
    l->a = na;
    l->cap = nc;
    return 0;
}

int dyn_sp_list_put(dyn_sp_list_t* l, const char* k, size_t kn,
    const char* v, size_t vn)
{
    if (l->n == l->cap && dyn_sp_list_grow(l) < 0)
        return -1;
    l->a[l->n].k = dyn_strndup(k, kn);
    if (!l->a[l->n].k)
        return -1;
    l->a[l->n].v = dyn_strndup(v, vn);
    if (!l->a[l->n].v) {
        free(l->a[l->n].k);
        return -1;
    }
    l->a[l->n].kn = kn;
    l->a[l->n].vn = vn;
    l->n++;
    return 0;
}

void dyn_sp_free(void* p)
{
    dyn_sp_t* sp = (dyn_sp_t*)p;
    dyn_sp_list_free(&sp->list);
    free(sp->query);
    if (sp->host && --sp->host->refs == 0)
        dyn_url_free(sp->host);
    free(sp);
}

int dyn_sp_set_query(dyn_sp_t* sp, const char* q, size_t n)
{
    char** slot = sp->host ? &sp->host->query : &sp->query;
    char* nq = dyn_strndup(q, n);
    if (!nq)
        return -1;
    free(*slot);
    *slot = nq;
    if (sp->host)
        sp->host->query_version++;
    else
        sp->query_version++;
    return 0;
}

static int dyn_sp_query_canonical(const char* q, size_t n)
{
    size_t i = 0, seg = 0;
    int has_eq = 0;
    for (i = 0; i <= n; i++) {
        unsigned char c = (i < n) ? (unsigned char)q[i] : '&';
        if (c == '&') {
            if (i == seg || !has_eq)
                return 0;
            seg = i + 1;
            has_eq = 0;
        } else {
            if (c == '=')
                has_eq = 1;
            else if (c == '%' || c == '+' || c == ' ' || c == '"' || c == '#' || c == '<' || c == '>' || c <= 0x1F || c >= 0x7F)
                return 0;
        }
    }
    return 1;
}

dyn_sp_list_t* dyn_sp_list_ref(dyn_sp_t* sp)
{
    uint32_t ver = dyn_sp_version(sp);
    if (!sp->list_valid || ver != sp->cached_version) {
        const char* q = dyn_sp_query_str(sp);
        dyn_sp_list_free(&sp->list);
        dyn_sp_list_init(&sp->list);
        dyn_sp_parse(q, strlen(q), &sp->list);
        sp->list_valid = 1;
        sp->list_canonical = dyn_sp_query_canonical(q, strlen(q));
        sp->cached_version = ver;
    }
    return &sp->list;
}

static size_t dyn_sp_utf8_seq(const unsigned char* s, size_t n)
{
    unsigned char c = s[0], lo = 0x80, hi = 0xBF;
    size_t need, k;
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF)
        need = 1;
    else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        if (c == 0xE0)
            lo = 0xA0;
        else if (c == 0xED)
            hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        if (c == 0xF0)
            lo = 0x90;
        else if (c == 0xF4)
            hi = 0x8F;
    } else
        return 0;
    for (k = 1; k <= need; k++) {
        if (k >= n || s[k] < lo || s[k] > hi)
            return 0;
        lo = 0x80;
        hi = 0xBF;
    }
    return need + 1;
}

static size_t dyn_sp_utf8_bad_len(const unsigned char* s, size_t n)
{
    unsigned char c = s[0], lo = 0x80, hi = 0xBF;
    size_t need, k;
    if (c >= 0xC2 && c <= 0xDF)
        need = 1;
    else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        if (c == 0xE0)
            lo = 0xA0;
        else if (c == 0xED)
            hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        if (c == 0xF0)
            lo = 0x90;
        else if (c == 0xF4)
            hi = 0x8F;
    } else
        return 1;
    for (k = 1; k <= need && k < n && s[k] >= lo && s[k] <= hi; k++) {
        lo = 0x80;
        hi = 0xBF;
    }
    return k;
}

static char* dyn_sp_decode_dup(const char* s, size_t n, size_t* outn)
{
    dyn_pct_buf_t o;
    size_t i, w, bad = 0;
    char* r;
    o.p = (char*)malloc(n + 1);
    if (!o.p)
        return NULL;
    o.n = 0;
    dyn_pct_decode_core(&o, dyn_pct_buf_sink, s, n, 1);
    for (i = 0; i < o.n;) {
        size_t q = dyn_sp_utf8_seq((const unsigned char*)o.p + i, o.n - i);
        if (q) {
            i += q;
        } else {
            i += dyn_sp_utf8_bad_len((const unsigned char*)o.p + i, o.n - i);
            bad++;
        }
    }
    if (!bad) {
        o.p[o.n] = 0;
        *outn = o.n;
        return o.p;
    }
    r = (char*)malloc(o.n + 2 * bad + 1);
    if (!r) {
        free(o.p);
        return NULL;
    }
    for (i = 0, w = 0; i < o.n;) {
        size_t q = dyn_sp_utf8_seq((const unsigned char*)o.p + i, o.n - i);
        if (q) {
            memcpy(r + w, o.p + i, q);
            w += q;
            i += q;
        } else {
            memcpy(r + w, "\xEF\xBF\xBD", 3);
            w += 3;
            i += dyn_sp_utf8_bad_len((const unsigned char*)o.p + i, o.n - i);
        }
    }
    r[w] = 0;
    free(o.p);
    *outn = w;
    return r;
}

void dyn_sp_parse(const char* q, size_t n, dyn_sp_list_t* l)
{
    size_t i = 0;
    while (i < n) {
        size_t b = i, eq;
        const char* e = (const char*)memchr(q + i, '&', n - i);
        const char* qe;
        char *k, *v;
        size_t kn = 0, vn = 0;
        i = e ? (size_t)(e - q) : n;
        if (i == b) {
            i++;
            continue;
        }
        qe = (const char*)memchr(q + b, '=', i - b);
        eq = qe ? (size_t)(qe - q) : i;
        k = dyn_sp_decode_dup(q + b, eq - b, &kn);
        v = qe ? dyn_sp_decode_dup(q + eq + 1, i - eq - 1, &vn) : dyn_strndup("", 0);
        if (k && v && (l->n < l->cap || dyn_sp_list_grow(l) == 0)) {
            l->a[l->n].k = k;
            l->a[l->n].v = v;
            l->a[l->n].kn = kn;
            l->a[l->n].vn = vn;
            l->n++;
        } else {
            free(k);
            free(v);
        }
        i++;
    }
}

int dyn_sp_serialize(const dyn_sp_list_t* l, dyn_sb_t* out)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        if (i)
            dyn_sb_putc(out, '&');
        dyn_pct_encode(out, l->a[i].k, l->a[i].kn, 2);
        dyn_sb_putc(out, '=');
        dyn_pct_encode(out, l->a[i].v, l->a[i].vn, 2);
    }
    return out->oom ? -1 : 0;
}

int dyn_sp_apply(dyn_sp_t* sp, const dyn_sp_list_t* l)
{
    dyn_sb_t out;
    int rc;
    dyn_sb_init(&out);
    rc = dyn_sp_serialize(l, &out);
    if (rc == 0)
        rc = dyn_sp_set_query(sp, out.p ? out.p : "", out.n);
    dyn_sb_free(&out);
    return rc;
}

int dyn_sp_cmp(const void* a, const void* b)
{
    const dyn_sp_sort_ent_t* x = (const dyn_sp_sort_ent_t*)a;
    const dyn_sp_sort_ent_t* y = (const dyn_sp_sort_ent_t*)b;
    size_t m = x->p.kn < y->p.kn ? x->p.kn : y->p.kn;
    int c = memcmp(x->p.k, y->p.k, m);
    if (c != 0)
        return c;
    if (x->p.kn != y->p.kn)
        return x->p.kn < y->p.kn ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

int dyn_sp_key_is(const dyn_sp_pair_t* p, const char* k, size_t kn)
{
    return p->kn == kn && memcmp(p->k, k, kn) == 0;
}

int dyn_sp_val_is(const dyn_sp_pair_t* p, const char* v, size_t vn)
{
    return p->vn == vn && memcmp(p->v, v, vn) == 0;
}
