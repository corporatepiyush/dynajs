#include <stdlib.h>
#include "dd.h"

int dd_sort_parse(dd_sort* s, const uint8_t* spec, dd_err* e)
{
    dyn_bson_iter it;
    int n = 0;
    memset(s, 0, sizeof *s);
    dyn_bson_iter_init(&it, spec, dyn_bson_rd32(spec));
    while (dyn_bson_iter_next(&it) > 0)
        n++;
    if (n == 0)
        return 0;
    if (n > DD_MAX_PATH_PARTS)
        return dd_fail(e, DD_E_BADVALUE, "sort has more than %d keys", DD_MAX_PATH_PARTS);
    s->k = calloc((size_t)n, sizeof *s->k);
    if (!s->k)
        return dd_fail(e, DD_E_OOM, "out of memory");
    dyn_bson_iter_init(&it, spec, dyn_bson_rd32(spec));
    while (dyn_bson_iter_next(&it) > 0) {
        dyn_bson_val v = dyn_bson_iter_val(&it);
        double d = dyn_bson_is_number(v.type) ? dyn_bson_num_f64(v) : 0;
        if (d != 1 && d != -1) {
            dd_sort_free(s);
            return dd_fail(e, DD_E_BADVALUE, "$sort key ordering must be 1 (for ascending) or -1 (for descending)");
        }
        if (dd_path_parse(&s->k[s->n].path, it.key, it.klen, e) < 0) {
            dd_sort_free(s);
            return -1;
        }
        s->k[s->n].dir = d < 0 ? -1 : 1;
        s->n++;
    }
    return 0;
}

void dd_sort_free(dd_sort* s)
{
    int i;
    for (i = 0; i < s->n; i++)
        dd_path_free(&s->k[i].path);
    free(s->k);
    memset(s, 0, sizeof *s);
}

typedef struct {
    dyn_ks_buf* kb;
    size_t best_off;
    size_t best_n;
    int have;
    int want_max;
    int saw_empty_array;
    int saw_any;
} sk_ctx;

static int sk_visit(void* ud, dyn_bson_val v, int is_elem, int32_t aidx)
{
    sk_ctx* c = ud;
    size_t start = c->kb->n;
    (void)aidx;
    if (!is_elem && v.type == DYN_BSON_ARRAY) {
        if (v.vlen <= 5)
            c->saw_empty_array = 1;
        return 0;
    }
    c->saw_any = 1;
    if (v.type == 0)
        dyn_ks_missing(c->kb, 0);
    else
        dyn_ks_value(c->kb, v, 0);
    if (c->kb->oom)
        return 1;
    if (!c->have) {
        c->have = 1;
        c->best_off = start;
        c->best_n = c->kb->n - start;
        return 0;
    }
    {
        size_t n = c->kb->n - start;
        size_t m = n < c->best_n ? n : c->best_n;
        int r = memcmp(c->kb->p + start, c->kb->p + c->best_off, m);
        if (r == 0)
            r = n < c->best_n ? -1 : n > c->best_n ? 1
                                                   : 0;
        if ((c->want_max && r > 0) || (!c->want_max && r < 0)) {
            memmove(c->kb->p + c->best_off, c->kb->p + start, n);
            c->best_n = n;
        }
        c->kb->n = c->best_off + c->best_n;
    }
    return 0;
}

void dd_sort_key(const dd_sort* s, const uint8_t* doc, dyn_ks_buf* kb)
{
    int i;
    for (i = 0; i < s->n; i++) {
        size_t from = kb->n, j;
        sk_ctx c;
        memset(&c, 0, sizeof c);
        c.kb = kb;
        c.want_max = s->k[i].dir < 0;
        dd_path_walk(doc, &s->k[i].path, sk_visit, &c);
        if (kb->oom)
            return;
        if (!c.have) {
            kb->n = from;
            if (c.saw_empty_array)
                dyn_ks_type_floor(kb, 1, 0);
            else
                dyn_ks_missing(kb, 0);
        }
        if (s->k[i].dir < 0 && !kb->oom)
            for (j = from; j < kb->n; j++)
                kb->p[j] = (uint8_t)~kb->p[j];
    }
}

static int rec_cmp(const dd_sortrec* a, const dd_sortrec* b, const uint8_t* keys)
{
    int r;
    uint32_t m;
    if (a->pre != b->pre)
        return a->pre < b->pre ? -1 : 1;
    m = a->len < b->len ? a->len : b->len;
    r = memcmp(keys + a->off, keys + b->off, m);
    if (r)
        return r;
    if (a->len != b->len)
        return a->len < b->len ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id ? 1
                                              : 0;
}

static void heap_down(dd_sortrec* h, uint32_t n, uint32_t i, const uint8_t* keys)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        dd_sortrec t;
        if (l < n && rec_cmp(&h[l], &h[m], keys) > 0)
            m = l;
        if (r < n && rec_cmp(&h[r], &h[m], keys) > 0)
            m = r;
        if (m == i)
            return;
        t = h[i];
        h[i] = h[m];
        h[m] = t;
        i = m;
    }
}

static int merge_sort(dd_sortrec* r, uint32_t n, const uint8_t* keys)
{
    dd_sortrec* tmp;
    uint32_t w, i;
    if (n < 2)
        return 0;
    tmp = malloc((size_t)n * sizeof *tmp);
    if (!tmp)
        return -1;
    for (i = 0; i < n; i += 16) {
        uint32_t hi = i + 16 < n ? i + 16 : n, a, b;
        for (a = i + 1; a < hi; a++) {
            dd_sortrec x = r[a];
            for (b = a; b > i && rec_cmp(&r[b - 1], &x, keys) > 0; b--)
                r[b] = r[b - 1];
            r[b] = x;
        }
    }
    for (w = 16; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            uint32_t mid = i + w < n ? i + w : n, hi = i + 2 * w < n ? i + 2 * w : n, a = i, b = mid, o = i;
            if (mid >= hi || rec_cmp(&r[mid - 1], &r[mid], keys) <= 0) {
                memcpy(tmp + i, r + i, (size_t)(hi - i) * sizeof *r);
                continue;
            }
            while (a < mid && b < hi)
                tmp[o++] = rec_cmp(&r[a], &r[b], keys) <= 0 ? r[a++] : r[b++];
            while (a < mid)
                tmp[o++] = r[a++];
            while (b < hi)
                tmp[o++] = r[b++];
        }
        memcpy(r, tmp, (size_t)n * sizeof *r);
        if (w > n / 2)
            break;
    }
    free(tmp);
    return 0;
}

int dd_sort_records(dd_sortrec* r, uint32_t n, const uint8_t* keys, uint64_t limit)
{
    uint32_t i, k;
    for (i = 0; i < n; i++) {
        uint64_t p = 0;
        uint32_t j;
        for (j = 0; j < 8; j++)
            p = (p << 8) | (j < r[i].len ? keys[r[i].off + j] : 0);
        r[i].pre = p;
    }
    if (limit == 0 || limit >= n || limit > n / 4) {
        if (merge_sort(r, n, keys) < 0)
            return -1;
        return (int)(limit && limit < n ? limit : n);
    }
    k = (uint32_t)limit;
    for (i = k / 2; i-- > 0;)
        heap_down(r, k, i, keys);
    for (i = k; i < n; i++) {
        if (rec_cmp(&r[i], &r[0], keys) < 0) {
            r[0] = r[i];
            heap_down(r, k, 0, keys);
        }
    }
    if (merge_sort(r, k, keys) < 0)
        return -1;
    return (int)k;
}
