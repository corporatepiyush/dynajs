#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DFA_TD_COMPRESSION 100.0
#define DFA_TD_HALF_PI 1.5707963267948966
#define DFA_TD_NORM (DFA_TD_COMPRESSION / (2.0 * 3.14159265358979323846))
#define DFA_SS_EMPTY 0xFFFFFFFFu
#define DFA_SS_DEAD 0xFFFFFFFEu

typedef struct {
    uint64_t count;
    uint32_t code;
} dfa_codecount;

uint64_t dfa_canon_bits(double v)
{
    uint64_t b;
    if (v != v)
        return 0x7FF8000000000000ULL;
    if (v == 0.0)
        v = 0.0;
    memcpy(&b, &v, sizeof(b));
    return b;
}

double dfa_bits_value(uint64_t b)
{
    double v;
    memcpy(&v, &b, sizeof(v));
    return v;
}

uint64_t dfa_hash_bits(uint64_t bits)
{
    return dyn_mix64(bits);
}

uint64_t dfa_row_hash(const DFColumn* c, const DFBound* b, uint32_t i)
{
    if (b->type == DF_STR) {
        uint32_t code = (uint32_t)((const int32_t*)b->p)[i];
        const char* s;
        if (code >= c->dict_len)
            return 0;
        s = c->dict[code];
        return dyn_xxh64((const uint8_t*)s, strlen(s), 0);
    }
    return dfa_hash_bits(dfa_canon_bits(df_get(b->p, b->type, i)));
}

static int dfa_total_order(double a, double b)
{
    int na = (a != a), nb = (b != b);
    if (na || nb)
        return na - nb;
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

JSValue dfa_result_pair(JSContext* ctx, JSValue keys, double* vals,
    uint32_t n)
{
    JSValue values, res;

    if (JS_IsException(keys)) {
        free(vals);
        return JS_EXCEPTION;
    }
    values = df_to_typed_array(ctx, vals, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    if (JS_IsException(values)) {
        JS_FreeValue(ctx, keys);
        return JS_EXCEPTION;
    }
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, keys);
        JS_FreeValue(ctx, values);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "values", values,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

JSValue dfa_exact_distinct_str(JSContext* ctx, const DFColumn* c,
    const DFBound* b, const uint8_t* mask)
{
    const int32_t* codes = b->p;
    uint32_t *seen, i, nwords, distinct = 0;

    nwords = (c->dict_len + 31) / 32;
    seen = calloc(nwords ? nwords : 1, sizeof(uint32_t));
    if (!seen)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < b->n; i++) {
        uint32_t code, w, bit;
        if (mask && !mask[i])
            continue;
        code = (uint32_t)codes[i];
        if (code >= c->dict_len)
            continue;
        w = code >> 5;
        bit = 1u << (code & 31);
        if (!(seen[w] & bit)) {
            seen[w] |= bit;
            distinct++;
        }
    }
    free(seen);
    return JS_NewInt64(ctx, distinct);
}

static double dfa_td_k(double q)
{
    if (q <= 0.0)
        return -DFA_TD_NORM * DFA_TD_HALF_PI;
    if (q >= 1.0)
        return DFA_TD_NORM * DFA_TD_HALF_PI;
    return DFA_TD_NORM * asin(2.0 * q - 1.0);
}

static double dfa_td_q(double k)
{
    double t = k / DFA_TD_NORM;
    if (t <= -DFA_TD_HALF_PI)
        return 0.0;
    if (t >= DFA_TD_HALF_PI)
        return 1.0;
    return (sin(t) + 1.0) / 2.0;
}

static double dfa_td_merge_mean(double ma, double wa, double mb, double wb)
{
    if (ma == mb)
        return ma;
    if (!isfinite(ma) || !isfinite(mb))
        return wa >= wb ? ma : mb;
    return ma + (mb - ma) * (wb / (wa + wb));
}

void dfa_td_init(dfa_tdigest* t)
{
    t->nmain = 0;
    t->nbuf = 0;
    t->total = 0.0;
    t->vmin = DYN_INFINITY;
    t->vmax = -DYN_INFINITY;
}

static void dfa_td_flush(dfa_tdigest* t)
{
    uint32_t i, j, n = 0, out = 0;
    double w_so_far, q_limit;
    dfa_centroid cur;

    if (t->nbuf == 0)
        return;
    if (dfo_pair_sort(t->buf, t->nbuf, DFO_SMALL_RADIX_MIN))
        dfo_pair_small_sort(t->buf, t->nbuf);

    i = 0;
    j = 0;
    while (i < t->nmain || j < t->nbuf) {
        if (j >= t->nbuf || (i < t->nmain && t->main[i].mean <= t->buf[j].mean))
            t->merge[n++] = t->main[i++];
        else
            t->merge[n++] = t->buf[j++];
    }
    t->nbuf = 0;
    if (n == 0) {
        t->nmain = 0;
        return;
    }

    w_so_far = 0.0;
    q_limit = dfa_td_q(dfa_td_k(0.0) + 1.0);
    cur = t->merge[0];
    for (i = 1; i < n; i++) {
        double proposed = (w_so_far + cur.weight + t->merge[i].weight) / t->total;
        if (proposed <= q_limit || out == DFA_TD_MAIN_CAP - 1) {
            cur.mean = dfa_td_merge_mean(cur.mean, cur.weight,
                t->merge[i].mean, t->merge[i].weight);
            cur.weight += t->merge[i].weight;
        } else {
            t->main[out++] = cur;
            w_so_far += cur.weight;
            q_limit = dfa_td_q(dfa_td_k(w_so_far / t->total) + 1.0);
            cur = t->merge[i];
        }
    }
    t->main[out++] = cur;
    t->nmain = out;
}

void dfa_td_add_w(dfa_tdigest* t, double v, double w)
{
    if (v != v)
        return;
    if (!(w > 0.0) || w != w)
        return;
    if (v < t->vmin)
        t->vmin = v;
    if (v > t->vmax)
        t->vmax = v;
    t->total += w;
    t->buf[t->nbuf].mean = v;
    t->buf[t->nbuf].weight = w;
    t->nbuf++;
    if (t->nbuf == DFA_TD_BUF_CAP)
        dfa_td_flush(t);
}

void dfa_td_add(dfa_tdigest* t, double v)
{
    dfa_td_add_w(t, v, 1.0);
}

static double dfa_td_lerp(double left, double right, double frac,
    double lo, double hi)
{
    double d = right - left, v;
    v = (d == 0.0 || !isfinite(d)) ? left : left + frac * d;
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

double dfa_td_quantile(dfa_tdigest* t, double q)
{
    uint32_t i;
    double index, w_so_far;

    dfa_td_flush(t);
    if (q <= 0.0)
        return t->vmin;
    if (q >= 1.0)
        return t->vmax;
    if (t->nmain == 1)
        return t->main[0].mean;

    index = q * t->total;
    w_so_far = 0.0;
    for (i = 0; i < t->nmain; i++) {
        double w = t->main[i].weight;
        double centre = w_so_far + w / 2.0;
        if (index <= centre) {
            double left = (i == 0) ? t->vmin : t->main[i - 1].mean;
            double cleft = (i == 0) ? 0.0
                                    : w_so_far - t->main[i - 1].weight / 2.0;
            double span = centre - cleft;
            double frac = span > 0.0 ? (index - cleft) / span : 0.0;
            return dfa_td_lerp(left, t->main[i].mean, frac, t->vmin, t->vmax);
        }
        w_so_far += w;
    }
    {
        dfa_centroid last = t->main[t->nmain - 1];
        double cleft = t->total - last.weight / 2.0;
        double span = t->total - cleft;
        double frac = span > 0.0 ? (index - cleft) / span : 1.0;
        return dfa_td_lerp(last.mean, t->vmax, frac, t->vmin, t->vmax);
    }
}

void dfa_ss_free(dfa_ss* s)
{
    free(s->c);
    free(s->idx);
    s->c = NULL;
    s->idx = NULL;
}

int dfa_ss_init(dfa_ss* s, uint32_t m)
{
    s->m = m;
    s->n = 0;
    s->used = 0;
    s->islots = 4 * m;
    s->c = malloc((size_t)m * sizeof(*s->c));
    s->idx = malloc((size_t)s->islots * sizeof(uint32_t));
    if (!s->c || !s->idx) {
        dfa_ss_free(s);
        return -1;
    }
    memset(s->idx, 0xFF, (size_t)s->islots * sizeof(uint32_t));
    return 0;
}

static uint32_t dfa_ss_slot(const dfa_ss* s, uint64_t bits, uint64_t h,
    int* found)
{
    uint32_t mask = s->islots - 1;
    uint32_t p = (uint32_t)(h & mask), dead = DFA_SS_EMPTY;

    *found = 0;
    for (;;) {
        uint32_t e = s->idx[p];
        if (e == DFA_SS_EMPTY)
            return dead == DFA_SS_EMPTY ? p : dead;
        if (e == DFA_SS_DEAD) {
            if (dead == DFA_SS_EMPTY)
                dead = p;
        } else if (s->c[e].bits == bits) {
            *found = 1;
            return p;
        }
        p = (p + 1) & mask;
    }
}

static void dfa_ss_reindex(dfa_ss* s)
{
    uint32_t mask = s->islots - 1, i, p;

    memset(s->idx, 0xFF, (size_t)s->islots * sizeof(uint32_t));
    for (i = 0; i < s->n; i++) {
        p = (uint32_t)(dfa_hash_bits(s->c[i].bits) & mask);
        while (s->idx[p] != DFA_SS_EMPTY)
            p = (p + 1) & mask;
        s->idx[p] = i;
        s->c[i].islot = p;
    }
    s->used = s->n;
}

static void dfa_ss_place(dfa_ss* s, uint32_t pos, const dfa_ss_counter* v)
{
    s->c[pos] = *v;
    s->idx[s->c[pos].islot] = pos;
}

static void dfa_ss_sift_up(dfa_ss* s, uint32_t pos)
{
    dfa_ss_counter v = s->c[pos];

    while (pos > 0) {
        uint32_t parent = (pos - 1) / 2;
        if (s->c[parent].count <= v.count)
            break;
        dfa_ss_place(s, pos, &s->c[parent]);
        pos = parent;
    }
    dfa_ss_place(s, pos, &v);
}

static void dfa_ss_sift_down(dfa_ss* s, uint32_t pos)
{
    dfa_ss_counter v = s->c[pos];

    for (;;) {
        uint32_t l = 2 * pos + 1, r = l + 1, sm = pos;
        uint64_t best = v.count;
        if (l < s->n && s->c[l].count < best) {
            sm = l;
            best = s->c[l].count;
        }
        if (r < s->n && s->c[r].count < best) {
            sm = r;
        }
        if (sm == pos)
            break;
        dfa_ss_place(s, pos, &s->c[sm]);
        pos = sm;
    }
    dfa_ss_place(s, pos, &v);
}

void dfa_ss_add(dfa_ss* s, uint64_t bits)
{
    uint64_t h = dfa_hash_bits(bits);
    int found;
    uint32_t slot = dfa_ss_slot(s, bits, h, &found);

    if (found) {
        uint32_t pos = s->idx[slot];
        s->c[pos].count++;
        dfa_ss_sift_down(s, pos);
        return;
    }
    if (s->n < s->m) {
        dfa_ss_counter v;
        v.bits = bits;
        v.count = 1;
        v.islot = slot;
        if (s->idx[slot] == DFA_SS_EMPTY)
            s->used++;
        s->c[s->n] = v;
        s->idx[slot] = s->n;
        s->n++;
        dfa_ss_sift_up(s, s->n - 1);
    } else {
        s->idx[s->c[0].islot] = DFA_SS_DEAD;
        if (s->idx[slot] == DFA_SS_EMPTY)
            s->used++;
        s->c[0].bits = bits;
        s->c[0].count++;
        s->c[0].islot = slot;
        s->idx[slot] = 0;
        dfa_ss_sift_down(s, 0);
    }
    if (s->used * 2 >= s->islots)
        dfa_ss_reindex(s);
}

int dfa_ss_cmp(const void* pa, const void* pb)
{
    const dfa_ss_counter *a = pa, *b = pb;

    if (a->count != b->count)
        return a->count > b->count ? -1 : 1;
    return dfa_total_order(dfa_bits_value(a->bits), dfa_bits_value(b->bits));
}

static int dfa_cmp_codecount(const void* pa, const void* pb)
{
    const dfa_codecount *a = pa, *b = pb;

    if (a->count != b->count)
        return a->count > b->count ? -1 : 1;
    return a->code < b->code ? -1 : (a->code > b->code ? 1 : 0);
}

JSValue dfa_exact_topk_str(JSContext* ctx, const DFColumn* c,
    const DFBound* b, const uint8_t* mask,
    uint32_t k)
{
    const int32_t* codes = b->p;
    uint64_t* counts;
    dfa_codecount* rank = NULL;
    double* vals = NULL;
    JSValue keys;
    uint32_t i, nseen = 0, nout;

    counts = calloc(c->dict_len ? c->dict_len : 1, sizeof(uint64_t));
    if (!counts)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < b->n; i++) {
        uint32_t code;
        if (mask && !mask[i])
            continue;
        code = (uint32_t)codes[i];
        if (code < c->dict_len)
            counts[code]++;
    }
    rank = malloc((size_t)(c->dict_len ? c->dict_len : 1) * sizeof(*rank));
    if (!rank) {
        free(counts);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < c->dict_len; i++) {
        if (counts[i] == 0)
            continue;
        rank[nseen].count = counts[i];
        rank[nseen].code = i;
        nseen++;
    }
    free(counts);
    qsort(rank, nseen, sizeof(*rank), dfa_cmp_codecount);

    nout = k < nseen ? k : nseen;
    vals = malloc((size_t)(nout ? nout : 1) * sizeof(double));
    if (!vals) {
        free(rank);
        return JS_ThrowOutOfMemory(ctx);
    }
    keys = JS_NewArray(ctx);
    for (i = 0; i < nout && !JS_IsException(keys); i++) {
        JSValue key = JS_NewString(ctx, c->dict[rank[i].code]);
        vals[i] = (double)rank[i].count;
        if (JS_IsException(key) || JS_DefinePropertyValueUint32(ctx, keys, i, key, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            keys = JS_EXCEPTION;
        }
    }
    free(rank);
    return dfa_result_pair(ctx, keys, vals, nout);
}

void dfa_kmv_add(dfa_kmv* s, uint64_t h)
{
    uint32_t i, pos;

    if (s->n == DFA_KMV_K && h >= s->h[0])
        return;
    for (i = 0; i < s->n; i++)
        if (s->h[i] == h)
            return;
    if (s->n < DFA_KMV_K) {
        pos = s->n++;
        s->h[pos] = h;
        while (pos > 0 && s->h[(pos - 1) / 2] < s->h[pos]) {
            uint64_t t = s->h[pos];
            s->h[pos] = s->h[(pos - 1) / 2];
            s->h[(pos - 1) / 2] = t;
            pos = (pos - 1) / 2;
        }
        return;
    }
    s->h[0] = h;
    pos = 0;
    for (;;) {
        uint32_t l = 2 * pos + 1, r = l + 1, big = pos;
        if (l < s->n && s->h[l] > s->h[big])
            big = l;
        if (r < s->n && s->h[r] > s->h[big])
            big = r;
        if (big == pos)
            break;
        {
            uint64_t t = s->h[pos];
            s->h[pos] = s->h[big];
            s->h[big] = t;
        }
        pos = big;
    }
}

static int dfa_cmp_u64(const void* pa, const void* pb)
{
    uint64_t a = *(const uint64_t*)pa, b = *(const uint64_t*)pb;
    return a < b ? -1 : (a > b ? 1 : 0);
}

double dfa_kmv_jaccard(dfa_kmv* a, dfa_kmv* b)
{
    uint32_t ia = 0, ib = 0, taken = 0, matches = 0;

    qsort(a->h, a->n, sizeof(uint64_t), dfa_cmp_u64);
    qsort(b->h, b->n, sizeof(uint64_t), dfa_cmp_u64);
    while (taken < DFA_KMV_K && (ia < a->n || ib < b->n)) {
        if (ib >= b->n || (ia < a->n && a->h[ia] < b->h[ib])) {
            ia++;
        } else if (ia >= a->n || b->h[ib] < a->h[ia]) {
            ib++;
        } else {
            ia++;
            ib++;
            matches++;
        }
        taken++;
    }
    if (taken == 0)
        return DYN_NAN;
    return (double)matches / (double)taken;
}

#endif
