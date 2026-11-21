#include "../dyn-csvx.h"
#include "../dyn-par.h"
#include "dtoa.h"

#include <math.h>
#include <stdlib.h>

static const double csvx_p10[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

size_t csvx_unquote(const uint8_t* p, size_t n, uint8_t quote, uint8_t* out)
{
    size_t i = 1, o = 0, end, k;
    const uint8_t* q;
    if (n < 64) {
        for (; i + 1 < n; i++) {
            out[o++] = p[i];
            if (p[i] == quote)
                i++;
        }
        return o;
    }
    end = n - 1;
    while (i < end) {
        q = memchr(p + i, quote, end - i);
        k = q ? (size_t)(q - p) + 1 : end;
        memcpy(out + o, p + i, k - i);
        o += k - i;
        i = k + 1;
    }
    return o;
}

static inline int is_digit(uint8_t c)
{
    return (uint8_t)(c - '0') < 10;
}

static inline int eight_digits(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return ((v & 0xF0F0F0F0F0F0F0F0ULL) | (((v + 0x0606060606060606ULL) & 0xF0F0F0F0F0F0F0F0ULL) >> 4)) == 0x3333333333333333ULL;
}

static inline uint32_t eight_value(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    v = ((v & 0x0F0F0F0F0F0F0F0FULL) * 2561) >> 8;
    v = ((v & 0x00FF00FF00FF00FFULL) * 6553601) >> 16;
    return (uint32_t)(((v & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32);
}

int csvx_parse_f64(const uint8_t* p, size_t n, double* out)
{
    size_t i = 0, sig = 0;
    uint64_t m = 0;
    int neg = 0, ex = 0, eneg = 0, eval = 0, any = 0;

    if (n == 0)
        return -1;
    if (p[0] == '+' || p[0] == '-') {
        neg = p[0] == '-';
        i++;
    }
    while (i < n && p[i] == '0') {
        any = 1;
        i++;
    }
    while (i + 8 <= n && sig + 8 <= 19 && eight_digits(p + i)) {
        m = m * 100000000ULL + eight_value(p + i);
        sig += 8;
        i += 8;
        any = 1;
    }
    while (i < n && is_digit(p[i])) {
        any = 1;
        if (m || p[i] != '0') {
            if (sig < 19)
                m = m * 10 + (uint64_t)(p[i] - '0');
            else
                ex++;
            sig++;
        }
        i++;
    }
    if (i < n && p[i] == '.') {
        i++;
        while (i < n && is_digit(p[i])) {
            any = 1;
            if (m || p[i] != '0') {
                if (sig < 19) {
                    m = m * 10 + (uint64_t)(p[i] - '0');
                    ex--;
                }
                sig++;
            } else {
                ex--;
            }
            i++;
        }
    }
    if (!any)
        return -1;
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        int edig = 0;
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-')) {
            eneg = p[i] == '-';
            i++;
        }
        while (i < n && is_digit(p[i])) {
            if (eval < 100000)
                eval = eval * 10 + (p[i] - '0');
            edig = 1;
            i++;
        }
        if (!edig)
            return -1;
    }
    if (i != n)
        return -1;
    ex += eneg ? -eval : eval;
    if (m == 0) {
        *out = neg ? -0.0 : 0.0;
        return 0;
    }
    if (sig <= 15 && ex >= -22 && ex <= 22) {
        double v = (double)m;
        v = ex < 0 ? v / csvx_p10[-ex] : v * csvx_p10[ex];
        *out = neg ? -v : v;
        return 0;
    }
    {
        char sbuf[128];
        char* s = n < sizeof(sbuf) ? sbuf : (char*)malloc(n + 1);
        const char* next = NULL;
        JSATODTempMem tm;
        double v;
        if (!s)
            return -2;
        memcpy(s, p, n);
        s[n] = 0;
        v = js_atod(s, &next, 10, 0, &tm);
        i = (size_t)(next - s);
        if (s != sbuf)
            free(s);
        if (i != n)
            return -1;
        *out = v;
    }
    return 0;
}

int csvx_parse_i64(const uint8_t* p, size_t n, int64_t* out)
{
    size_t i = 0;
    uint64_t v = 0, lim;
    int neg = 0;
    if (n == 0)
        return -1;
    if (p[0] == '+' || p[0] == '-') {
        neg = p[0] == '-';
        i++;
    }
    if (i == n)
        return -1;
    lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    while (i + 8 <= n && v < 100000000000ULL && eight_digits(p + i)) {
        v = v * 100000000ULL + eight_value(p + i);
        i += 8;
    }
    for (; i < n; i++) {
        if (!is_digit(p[i]))
            return -1;
        if (v > (lim - (uint64_t)(p[i] - '0')) / 10)
            return -1;
        v = v * 10 + (uint64_t)(p[i] - '0');
    }
    if (v > lim)
        return -1;
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return 0;
}

static inline uint64_t dict_hash(const uint8_t* p, size_t n, uint64_t seed)
{
    uint64_t h = seed ^ (0x9E3779B97F4A7C15ULL * (n + 1));
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 31;
        p += 8;
        n -= 8;
    }
    if (n) {
        uint64_t w = 0;
        memcpy(&w, p, n);
        h = (h ^ w) * 0x94D049BB133111EBULL;
    }
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 32;
    return h;
}

int csvx_dict_init(CsvxDict* d, uint32_t hint)
{
    uint32_t cap = 64;
    memset(d, 0, sizeof(*d));
    while (cap < hint * 2 && cap < (1u << 24))
        cap *= 2;
    d->slot = (uint32_t*)malloc((size_t)cap * sizeof(uint32_t));
    if (!d->slot)
        return -1;
    memset(d->slot, 0xFF, (size_t)cap * sizeof(uint32_t));
    d->mask = cap - 1;
    return 0;
}

static int dict_grow_slots(CsvxDict* d)
{
    uint32_t ncap = (d->mask + 1) * 2, i;
    uint32_t* ns;
    if (ncap == 0 || ncap > (1u << 31))
        return -1;
    ns = (uint32_t*)malloc((size_t)ncap * sizeof(uint32_t));
    if (!ns)
        return -1;
    memset(ns, 0xFF, (size_t)ncap * sizeof(uint32_t));
    for (i = 0; i < d->n; i++) {
        uint32_t q = (uint32_t)d->hash[i] & (ncap - 1);
        while (ns[q] != UINT32_MAX)
            q = (q + 1) & (ncap - 1);
        ns[q] = i;
    }
    free(d->slot);
    d->slot = ns;
    d->mask = ncap - 1;
    return 0;
}

static int dict_add(CsvxDict* d, const uint8_t* p, size_t n, uint64_t h, uint32_t pos, int32_t* code)
{
    uint32_t k;
    if (d->n == d->cap) {
        uint32_t nc = d->cap ? d->cap * 2 : 64;
        uint32_t *no, *nl;
        uint64_t* nh;
        if (d->cap >= (uint32_t)INT32_MAX / 2)
            return -1;
        no = (uint32_t*)realloc(d->off, (size_t)nc * sizeof(uint32_t));
        if (!no)
            return -1;
        d->off = no;
        nl = (uint32_t*)realloc(d->len, (size_t)nc * sizeof(uint32_t));
        if (!nl)
            return -1;
        d->len = nl;
        nh = (uint64_t*)realloc(d->hash, (size_t)nc * sizeof(uint64_t));
        if (!nh)
            return -1;
        d->hash = nh;
        d->cap = nc;
    }
    if (d->nbytes + n > d->bcap) {
        size_t nb = d->bcap ? d->bcap * 2 : 4096;
        uint8_t* np;
        while (nb < d->nbytes + n)
            nb *= 2;
        if (nb > UINT32_MAX)
            return -1;
        np = (uint8_t*)realloc(d->bytes, nb);
        if (!np)
            return -1;
        d->bytes = np;
        d->bcap = nb;
    }
    memcpy(d->bytes + d->nbytes, p, n);
    k = d->n++;
    d->off[k] = (uint32_t)d->nbytes;
    d->len[k] = (uint32_t)n;
    d->hash[k] = h;
    d->nbytes += n;
    d->slot[pos] = k;
    *code = (int32_t)k;
    if ((d->n + 1) * 2 > d->mask + 1)
        return dict_grow_slots(d);
    return 0;
}

static int dict_code_h(CsvxDict* d, const uint8_t* p, size_t n, uint64_t h, int32_t* code)
{
    uint32_t pos = (uint32_t)h & d->mask, k;
    while ((k = d->slot[pos]) != UINT32_MAX) {
        if (d->hash[k] == h && d->len[k] == n && memcmp(d->bytes + d->off[k], p, n) == 0) {
            *code = (int32_t)k;
            return 0;
        }
        pos = (pos + 1) & d->mask;
    }
    return dict_add(d, p, n, h, pos, code);
}

int csvx_dict_code(CsvxDict* d, const uint8_t* p, size_t n, int32_t* code)
{
    return dict_code_h(d, p, n, dict_hash(p, n, d->seed), code);
}

void csvx_dict_free(CsvxDict* d)
{
    free(d->slot);
    free(d->off);
    free(d->len);
    free(d->hash);
    free(d->bytes);
    memset(d, 0, sizeof(*d));
}

#define CSVX_COL_MIN_ROWS 65536
#define CSVX_COL_MAX_WORKERS 64

typedef struct {
    const CsvxIndex* ix;
    CsvxColumnJob* job;
    unsigned nparts;
    size_t bad[CSVX_COL_MAX_WORKERS];
    int status[CSVX_COL_MAX_WORKERS];
    CsvxDict dicts[CSVX_COL_MAX_WORKERS];
    int32_t* remap[CSVX_COL_MAX_WORKERS];
} ColCtx;

static int col_cell(const CsvxIndex* ix, size_t r, uint32_t c, const uint8_t** pp, size_t* pn, uint8_t** tmp, size_t* tcap)
{
    if (csvx_field(ix, r, c, pp, pn)) {
        const uint8_t* p = *pp;
        size_t n = *pn;
        if (csvx_needs_unescape(p, n, ix->quote)) {
            if (n > *tcap) {
                uint8_t* t = (uint8_t*)realloc(*tmp, n);
                if (!t)
                    return -1;
                *tmp = t;
                *tcap = n;
            }
            *pn = csvx_unquote(p, n, ix->quote, *tmp);
            *pp = *tmp;
        } else {
            *pp = p + 1;
            *pn = n >= 2 ? n - 2 : 0;
        }
    }
    return 0;
}

static void col_part(ColCtx* cc, unsigned w, size_t a, size_t b)
{
    const CsvxIndex* ix = cc->ix;
    CsvxColumnJob* job = cc->job;
    uint32_t col = job->col;
    uint8_t* tmp = NULL;
    size_t tcap = 0, r;
    int st = 0;
    for (r = a; r < b; r++) {
        const uint8_t* p;
        size_t n;
        size_t src = job->row0 + r;
        if (col_cell(ix, src, col, &p, &n, &tmp, &tcap)) {
            st = CSVX_OOM;
            break;
        }
        switch (job->kind) {
        case CSVX_COL_F64: {
            double* o = (double*)job->out;
            int rc;
            if (n == 0) {
                o[r] = (double)NAN;
                break;
            }
            rc = csvx_parse_f64(p, n, &o[r]);
            if (rc == -2)
                st = CSVX_OOM;
            else if (rc)
                st = 1;
            break;
        }
        case CSVX_COL_I32: {
            int64_t v;
            if (csvx_parse_i64(p, n, &v) || v < INT32_MIN || v > INT32_MAX)
                st = 1;
            else
                ((int32_t*)job->out)[r] = (int32_t)v;
            break;
        }
        case CSVX_COL_I64:
            if (csvx_parse_i64(p, n, &((int64_t*)job->out)[r]))
                st = 1;
            break;
        default:
            if (csvx_dict_code(&cc->dicts[w], p, n, &((int32_t*)job->out)[r]))
                st = CSVX_OOM;
            break;
        }
        if (st)
            break;
    }
    free(tmp);
    cc->status[w] = st;
    cc->bad[w] = st == 1 ? r : SIZE_MAX;
}

static void col_worker(void* arg, unsigned w, unsigned nw, dyn_par_t* par)
{
    ColCtx* cc = (ColCtx*)arg;
    CsvxColumnJob* job = cc->job;
    size_t n = job->nrows, a, b, r;
    unsigned i, parts = nw;
    if (w == 0)
        cc->nparts = parts;
    a = n * w / parts;
    b = n * (w + 1) / parts;
    cc->status[w] = 0;
    if (job->kind == CSVX_COL_DICT && w > 0) {
        if (csvx_dict_init(&cc->dicts[w], 256))
            cc->status[w] = CSVX_OOM;
        cc->dicts[w].seed = cc->dicts[0].seed;
    }
    if (!cc->status[w])
        col_part(cc, w, a, b);
    if (job->kind != CSVX_COL_DICT)
        return;
    dyn_par_barrier(par);
    if (w == 0) {
        for (i = 0; i < parts; i++)
            if (cc->status[i])
                goto merged;
        for (i = 1; i < parts; i++) {
            CsvxDict* d = &cc->dicts[i];
            uint32_t k;
            cc->remap[i] = (int32_t*)malloc((size_t)(d->n ? d->n : 1) * sizeof(int32_t));
            if (!cc->remap[i]) {
                cc->status[0] = CSVX_OOM;
                goto merged;
            }
            for (k = 0; k < d->n; k++)
                if (dict_code_h(&cc->dicts[0], d->bytes + d->off[k], d->len[k], d->hash[k], &cc->remap[i][k])) {
                    cc->status[0] = CSVX_OOM;
                    goto merged;
                }
        }
    merged:;
    }
    dyn_par_barrier(par);
    if (w > 0 && cc->remap[w] && !cc->status[0]) {
        int32_t* o = (int32_t*)job->out;
        const int32_t* m = cc->remap[w];
        for (r = a; r < b; r++)
            o[r] = m[o[r]];
    }
}

int csvx_column(const CsvxIndex* ix, CsvxColumnJob* job, unsigned max_threads)
{
    ColCtx* cc = (ColCtx*)calloc(1, sizeof(ColCtx));
    unsigned nw, i;
    int st = 0;
    size_t bad = SIZE_MAX;
    if (!cc)
        return CSVX_OOM;
    cc->ix = ix;
    cc->job = job;
    job->status = 0;
    job->bad_row = SIZE_MAX;
    if (job->kind == CSVX_COL_DICT) {
        cc->dicts[0] = job->dict;
        memset(&job->dict, 0, sizeof(job->dict));
    }
    nw = job->nrows >= 2 * CSVX_COL_MIN_ROWS ? dyn_par_plan(job->nrows, CSVX_COL_MIN_ROWS, max_threads) : 1;
    if (nw < 1)
        nw = 1;
    if (nw > CSVX_COL_MAX_WORKERS)
        nw = CSVX_COL_MAX_WORKERS;
    dyn_par_run(nw, col_worker, cc);
    for (i = 0; i < cc->nparts; i++) {
        if (cc->status[i] == CSVX_OOM)
            st = CSVX_OOM;
        else if (cc->status[i] == 1 && st != CSVX_OOM) {
            st = 1;
            if (cc->bad[i] < bad)
                bad = cc->bad[i];
        }
    }
    if (job->kind == CSVX_COL_DICT) {
        job->dict = cc->dicts[0];
        for (i = 1; i < CSVX_COL_MAX_WORKERS; i++) {
            csvx_dict_free(&cc->dicts[i]);
            free(cc->remap[i]);
        }
    }
    job->status = st;
    job->bad_row = bad;
    free(cc);
    return st == CSVX_OOM ? CSVX_OOM : 0;
}
