#include "dfk.h"

#include <stdio.h>
#include "dtoa.h"

typedef struct {
    const uint8_t* b;
    size_t len, i;
    uint8_t delim, quote;
    int strict;
    uint8_t* tmp;
    size_t tcap;
    int pend;
} DfkTok;

enum { COL_NUM,
    COL_F64,
    COL_I32,
    COL_STR,
    COL_LATE };

typedef struct {
    int state;
    int forced;
    uint32_t lead_empty;
    double* f;
    int32_t* iv;
    int32_t* codes;
    char** dict;
    uint32_t* dlen;
    uint32_t dict_len, dict_cap;
    uint32_t* slots;
    uint64_t* shash;
    uint32_t slot_mask;
} DfkCol;

typedef struct {
    DfkTok t;
    const DfkCsvSpec* s;
    DfkCsvOut* out;
    uint32_t hdrw;
    int32_t* map;
    DfkCol* c;
    uint32_t nout;
    size_t cap;
    size_t data_start;
    uint64_t rec;
    int pass2;
} DfkRd;

static const double dfk_p10[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

static int tok_reserve(DfkTok* t, size_t need)
{
    size_t nc;
    uint8_t* p;
    if (need <= t->tcap)
        return 0;
    nc = t->tcap ? t->tcap : 64;
    while (nc < need)
        nc *= 2;
    p = realloc(t->tmp, nc);
    if (!p)
        return -1;
    t->tmp = p;
    t->tcap = nc;
    return 0;
}

static inline uint64_t swar_zero(uint64_t v)
{
    return (v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL;
}

static size_t scan_plain(const DfkTok* t, size_t i)
{
    const uint8_t* b = t->b;
    size_t len = t->len;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const uint64_t kd = 0x0101010101010101ULL * t->delim;
    const uint64_t kn = 0x0101010101010101ULL * '\n';
    const uint64_t kr = 0x0101010101010101ULL * '\r';
    while (i + 8 <= len) {
        uint64_t v, m;
        memcpy(&v, b + i, 8);
        m = swar_zero(v ^ kd) | swar_zero(v ^ kn) | swar_zero(v ^ kr);
        if (m)
            return i + ((size_t)__builtin_ctzll(m) >> 3);
        i += 8;
    }
#endif
    while (i < len && b[i] != t->delim && b[i] != '\n' && b[i] != '\r')
        i++;
    return i;
}

static int tok_field(DfkTok* t, const uint8_t** fp, size_t* fn, int* end)
{
    const uint8_t* b = t->b;
    size_t len = t->len, i = t->i;

    if (t->pend) {
        t->pend = 0;
        *fp = b + len;
        *fn = 0;
        *end = 1;
        return 0;
    }
    if (i < len && b[i] == t->quote) {
        size_t n = 0;
        int closed = 0;
        i++;
        for (;;) {
            const uint8_t* q = i < len ? memchr(b + i, t->quote, len - i) : NULL;
            size_t chunk = q ? (size_t)(q - (b + i)) : len - i;
            if (tok_reserve(t, n + chunk + 1))
                return DFK_CSV_E_OOM;
            memcpy(t->tmp + n, b + i, chunk);
            n += chunk;
            i += chunk;
            if (!q)
                break;
            if (i + 1 < len && b[i + 1] == t->quote) {
                t->tmp[n++] = t->quote;
                i += 2;
                continue;
            }
            i++;
            closed = 1;
            break;
        }
        if (!closed && t->strict)
            return DFK_CSV_E_QUOTE;
        if (i < len && b[i] != t->delim && b[i] != '\n' && b[i] != '\r') {
            size_t e;
            if (t->strict)
                return DFK_CSV_E_GARBAGE;
            e = scan_plain(t, i);
            if (tok_reserve(t, n + (e - i) + 1))
                return DFK_CSV_E_OOM;
            memcpy(t->tmp + n, b + i, e - i);
            n += e - i;
            i = e;
        }
        *fp = t->tmp;
        *fn = n;
    } else {
        size_t e = scan_plain(t, i);
        *fp = b + i;
        *fn = e - i;
        i = e;
    }
    if (i >= len) {
        *end = 1;
    } else if (b[i] == t->delim) {
        *end = 0;
        i++;
        if (i >= len)
            t->pend = 1;
    } else {
        *end = 1;
        if (b[i] == '\r' && i + 1 < len && b[i + 1] == '\n')
            i += 2;
        else
            i++;
    }
    t->i = i;
    return 0;
}

static int parse_f64(const uint8_t* p, size_t n, double* out)
{
    size_t i = 0, sig = 0;
    uint64_t m = 0;
    int neg = 0, ex = 0, eneg = 0, eval = 0, any = 0;

    if (p[0] == '+' || p[0] == '-') {
        neg = p[0] == '-';
        i++;
    }
    while (i < n && p[i] >= '0' && p[i] <= '9') {
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
        while (i < n && p[i] >= '0' && p[i] <= '9') {
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
        while (i < n && p[i] >= '0' && p[i] <= '9') {
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
        v = ex < 0 ? v / dfk_p10[-ex] : v * dfk_p10[ex];
        *out = neg ? -v : v;
        return 0;
    }
    {
        char sbuf[128];
        char* s = n < sizeof(sbuf) ? sbuf : malloc(n + 1);
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

static int parse_i32(const uint8_t* p, size_t n, int32_t* out)
{
    size_t i = 0;
    int64_t v = 0;
    int neg = 0;
    if (n == 0)
        return -1;
    if (p[0] == '+' || p[0] == '-') {
        neg = p[0] == '-';
        i++;
    }
    if (i == n)
        return -1;
    for (; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        v = v * 10 + (p[i] - '0');
        if (v > 2147483648LL)
            return -1;
    }
    if (neg)
        v = -v;
    if (v > INT32_MAX || v < INT32_MIN)
        return -1;
    *out = (int32_t)v;
    return 0;
}

static inline uint64_t str_hash(const uint8_t* p, size_t n, uint64_t seed)
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

static int col_intern(DfkCol* c, const uint8_t* p, size_t n, uint64_t seed, int32_t* code)
{
    uint64_t h = str_hash(p, n, seed);
    uint32_t pos, k;
    char* s;

    if (!c->slots || (c->dict_len + 1) * 2 > c->slot_mask + 1) {
        uint32_t ncap = c->slots ? (c->slot_mask + 1) * 2 : 64, i;
        uint32_t* ns = malloc((size_t)ncap * sizeof(*ns));
        if (!ns)
            return -1;
        memset(ns, 0xFF, (size_t)ncap * sizeof(*ns));
        for (i = 0; i < c->dict_len; i++) {
            uint32_t q = (uint32_t)c->shash[i] & (ncap - 1);
            while (ns[q] != UINT32_MAX)
                q = (q + 1) & (ncap - 1);
            ns[q] = i;
        }
        free(c->slots);
        c->slots = ns;
        c->slot_mask = ncap - 1;
    }
    pos = (uint32_t)h & c->slot_mask;
    while ((k = c->slots[pos]) != UINT32_MAX) {
        if (c->shash[k] == h && c->dlen[k] == n && memcmp(c->dict[k], p, n) == 0) {
            *code = (int32_t)k;
            return 0;
        }
        pos = (pos + 1) & c->slot_mask;
    }
    if (c->dict_len == c->dict_cap) {
        uint32_t nc = c->dict_cap ? c->dict_cap * 2 : 16;
        char** nd;
        uint32_t* nl;
        uint64_t* nh;
        if (c->dict_cap > (uint32_t)INT32_MAX / 2)
            return -1;
        nd = realloc(c->dict, (size_t)nc * sizeof(*nd));
        if (!nd)
            return -1;
        c->dict = nd;
        nl = realloc(c->dlen, (size_t)nc * sizeof(*nl));
        if (!nl)
            return -1;
        c->dlen = nl;
        nh = realloc(c->shash, (size_t)nc * sizeof(*nh));
        if (!nh)
            return -1;
        c->shash = nh;
        c->dict_cap = nc;
    }
    s = malloc(n + 1);
    if (!s)
        return -1;
    memcpy(s, p, n);
    s[n] = 0;
    k = c->dict_len++;
    c->dict[k] = s;
    c->dlen[k] = (uint32_t)n;
    c->shash[k] = h;
    c->slots[pos] = k;
    *code = (int32_t)k;
    return 0;
}

static void col_release(DfkCol* c)
{
    uint32_t i;
    free(c->f);
    free(c->iv);
    free(c->codes);
    for (i = 0; i < c->dict_len; i++)
        free(c->dict[i]);
    free(c->dict);
    free(c->dlen);
    free(c->slots);
    free(c->shash);
    memset(c, 0, sizeof(*c));
}

static void set_err(DfkRd* r, uint64_t rec, const char* what, const char* name,
    const uint8_t* cell, size_t n)
{
    char ex[48];
    size_t k = n < 40 ? n : 40, j;
    for (j = 0; j < k; j++)
        ex[j] = (cell[j] >= 0x20 && cell[j] < 0x7f) ? (char)cell[j] : '?';
    ex[k] = 0;
    if (n > 40)
        memcpy(ex + 37, "...", 4);
    r->out->err_record = rec;
    snprintf(r->out->err_text, sizeof(r->out->err_text), "column '%.100s': '%s' is %s", name ? name : "", ex, what);
}

static int grow_rows(DfkRd* r, size_t need)
{
    size_t nc = r->cap ? r->cap : 1024, j;
    if (need <= r->cap)
        return 0;
    while (nc < need)
        nc *= 2;
    for (j = 0; j < r->nout; j++) {
        DfkCol* c = &r->c[j];
        if (c->f) {
            double* p = realloc(c->f, nc * sizeof(*p));
            if (!p)
                return -1;
            c->f = p;
        }
        if (c->iv) {
            int32_t* p = realloc(c->iv, nc * sizeof(*p));
            if (!p)
                return -1;
            c->iv = p;
        }
        if (c->codes) {
            int32_t* p = realloc(c->codes, nc * sizeof(*p));
            if (!p)
                return -1;
            c->codes = p;
        }
    }
    r->cap = nc;
    return 0;
}

static int to_str(DfkRd* r, DfkCol* c)
{
    uint32_t k;
    int32_t code;
    c->codes = malloc(r->cap * sizeof(*c->codes));
    if (!c->codes)
        return -1;
    c->state = COL_STR;
    if (c->lead_empty) {
        if (col_intern(c, (const uint8_t*)"", 0, r->s->seed, &code))
            return -1;
        for (k = 0; k < c->lead_empty; k++)
            c->codes[k] = code;
    }
    free(c->f);
    c->f = NULL;
    return 0;
}

static int put_cell(DfkRd* r, uint32_t oc, uint32_t row, const uint8_t* p, size_t n)
{
    DfkCol* c = &r->c[oc];
    int32_t code;
    int rc;

    switch (c->state) {
    case COL_NUM:
        if (n == 0) {
            c->f[row] = (double)NAN;
            if (c->lead_empty == row)
                c->lead_empty++;
            return 0;
        }
        rc = parse_f64(p, n, &c->f[row]);
        if (rc == 0) {
            c->lead_empty = UINT32_MAX;
            return 0;
        }
        if (rc == -2)
            return DFK_CSV_E_OOM;
        if (c->lead_empty == row) {
            if (to_str(r, c))
                return DFK_CSV_E_OOM;
            break;
        }
        c->state = COL_LATE;
        free(c->f);
        c->f = NULL;
        r->pass2 = 1;
        return 0;
    case COL_F64:
        if (n == 0) {
            c->f[row] = (double)NAN;
            return 0;
        }
        rc = parse_f64(p, n, &c->f[row]);
        if (rc == -2)
            return DFK_CSV_E_OOM;
        if (rc) {
            set_err(r, r->rec, "not a number", r->out->cols[oc].name, p, n);
            return DFK_CSV_E_NUMBER;
        }
        return 0;
    case COL_I32:
        if (parse_i32(p, n, &c->iv[row])) {
            set_err(r, r->rec, "not a 32-bit integer", r->out->cols[oc].name, p, n);
            return DFK_CSV_E_INT;
        }
        return 0;
    case COL_LATE:
        return 0;
    default:
        break;
    }
    if (col_intern(c, p, n, r->s->seed, &code))
        return DFK_CSV_E_OOM;
    c->codes[row] = code;
    return 0;
}

static int read_header(DfkRd* r)
{
    DfkTok* t = &r->t;
    uint32_t cap = 0, i;
    int end = 0, rc;

    r->out->hdr = NULL;
    while (!end) {
        const uint8_t* p;
        size_t n;
        char* s;
        if ((rc = tok_field(t, &p, &n, &end)) != 0) {
            r->out->err_record = 1;
            return rc;
        }
        if (r->hdrw == cap) {
            uint32_t nc = cap ? cap * 2 : 16;
            char** nh;
            if (cap >= 1u << 20) {
                r->out->err_record = 1;
                snprintf(r->out->err_text, sizeof(r->out->err_text), "the header has more than %u columns", 1u << 20);
                return DFK_CSV_E_WIDTH;
            }
            nh = realloc(r->out->hdr, (size_t)nc * sizeof(*nh));
            if (!nh)
                return DFK_CSV_E_OOM;
            r->out->hdr = nh;
            cap = nc;
        }
        s = malloc(n + 1);
        if (!s)
            return DFK_CSV_E_OOM;
        memcpy(s, p, n);
        s[n] = 0;
        r->out->hdr[r->hdrw++] = s;
    }
    r->out->nhdr = r->hdrw;
    for (i = 0; i < r->hdrw; i++) {
        uint32_t j;
        for (j = 0; j < i; j++)
            if (strcmp(r->out->hdr[i], r->out->hdr[j]) == 0) {
                r->out->err_record = 1;
                snprintf(r->out->err_text, sizeof(r->out->err_text), "duplicate column '%.120s' in the header", r->out->hdr[i]);
                return DFK_CSV_E_DUP;
            }
    }
    return 0;
}

static int find_hdr(const DfkRd* r, const char* name)
{
    uint32_t i;
    for (i = 0; i < r->hdrw; i++)
        if (strcmp(r->out->hdr[i], name) == 0)
            return (int)i;
    return -1;
}

static int plan_columns(DfkRd* r)
{
    const DfkCsvSpec* s = r->s;
    uint32_t i, nout = s->columns ? s->ncolumns : r->hdrw;

    r->map = malloc((size_t)(r->hdrw ? r->hdrw : 1) * sizeof(*r->map));
    r->c = calloc(nout ? nout : 1, sizeof(*r->c));
    r->out->cols = calloc(nout ? nout : 1, sizeof(*r->out->cols));
    if (!r->map || !r->c || !r->out->cols)
        return DFK_CSV_E_OOM;
    for (i = 0; i < r->hdrw; i++)
        r->map[i] = s->columns ? -1 : (int32_t)i;
    r->nout = nout;
    r->out->ncols = nout;
    for (i = 0; i < nout; i++) {
        int h = s->columns ? find_hdr(r, s->columns[i]) : (int)i;
        if (h < 0) {
            snprintf(r->out->err_text, sizeof(r->out->err_text), "no such column '%.120s'", s->columns[i]);
            return DFK_CSV_E_NOCOL;
        }
        if (r->map[h] >= 0 && s->columns) {
            snprintf(r->out->err_text, sizeof(r->out->err_text), "column '%.120s' is listed twice", s->columns[i]);
            return DFK_CSV_E_DUP;
        }
        r->map[h] = (int32_t)i;
        r->out->cols[i].name = r->out->hdr[h];
    }
    for (i = 0; i < s->ndtypes; i++) {
        int h = find_hdr(r, s->dtype_names[i]);
        DfkCol* c;
        if (h < 0 || r->map[h] < 0) {
            snprintf(r->out->err_text, sizeof(r->out->err_text), "dtypes names '%.120s', which is not %s",
                s->dtype_names[i], h < 0 ? "a column" : "a selected column");
            return DFK_CSV_E_NOCOL;
        }
        c = &r->c[r->map[h]];
        c->forced = 1;
        c->state = s->dtype_kinds[i] == DFK_CSV_F64 ? COL_F64 : s->dtype_kinds[i] == DFK_CSV_I32 ? COL_I32
            : s->dtype_kinds[i] == DFK_CSV_STR                                                   ? COL_STR
                                                                                                 : COL_NUM;
    }
    for (i = 0; i < nout; i++) {
        DfkCol* c = &r->c[i];
        if (c->state == COL_NUM || c->state == COL_F64)
            c->f = malloc(r->cap * sizeof(*c->f));
        else if (c->state == COL_I32)
            c->iv = malloc(r->cap * sizeof(*c->iv));
        else
            c->codes = malloc(r->cap * sizeof(*c->codes));
        if (!c->f && !c->iv && !c->codes)
            return DFK_CSV_E_OOM;
    }
    return 0;
}

static int skip_blank(DfkTok* t)
{
    if (t->i < t->len && (t->b[t->i] == '\n' || t->b[t->i] == '\r')) {
        if (t->b[t->i] == '\r' && t->i + 1 < t->len && t->b[t->i + 1] == '\n')
            t->i += 2;
        else
            t->i++;
        return 1;
    }
    return 0;
}

static int emit(DfkRd* r, int late_only, uint32_t oc, uint32_t row, const uint8_t* p, size_t n)
{
    DfkCol* c = &r->c[oc];
    int32_t code;
    if (!late_only)
        return put_cell(r, oc, row, p, n);
    if (c->state != COL_LATE)
        return 0;
    if (col_intern(c, p, n, r->s->seed, &code))
        return DFK_CSV_E_OOM;
    c->codes[row] = code;
    return 0;
}

static int scan_rows(DfkRd* r, int late_only, uint32_t* pnrows)
{
    DfkTok* t = &r->t;
    const DfkCsvSpec* s = r->s;
    uint64_t data = 0;
    uint32_t row = 0;
    int rc;

    t->i = r->data_start;
    r->rec = 1;
    while (t->i < t->len) {
        uint32_t f = 0;
        int end = 0, keep;
        if (r->hdrw > 1 && skip_blank(t)) {
            r->rec++;
            continue;
        }
        r->rec++;
        if (row >= s->limit)
            break;
        keep = data >= s->offset;
        data++;
        if (keep && row >= r->cap && grow_rows(r, (size_t)row + 1))
            return DFK_CSV_E_OOM;
        if (keep && row == UINT32_MAX) {
            snprintf(r->out->err_text, sizeof(r->out->err_text), "more than %u data rows", UINT32_MAX - 1);
            return DFK_CSV_E_ROWS;
        }
        while (!end) {
            const uint8_t* p;
            size_t n;
            if ((rc = tok_field(t, &p, &n, &end)) != 0) {
                r->out->err_record = r->rec;
                return rc;
            }
            if (keep && f < r->hdrw && r->map[f] >= 0 && (rc = emit(r, late_only, (uint32_t)r->map[f], row, p, n)) != 0)
                return rc;
            f++;
        }
        if (f != r->hdrw && s->strict) {
            r->out->err_record = r->rec;
            snprintf(r->out->err_text, sizeof(r->out->err_text), "%u fields where the header has %u", f, r->hdrw);
            return DFK_CSV_E_WIDTH;
        }
        if (keep) {
            for (; f < r->hdrw; f++)
                if (r->map[f] >= 0 && (rc = emit(r, late_only, (uint32_t)r->map[f], row, (const uint8_t*)"", 0)) != 0)
                    return rc;
            row++;
        }
    }
    *pnrows = row;
    return 0;
}

int dfk_csv_read(const uint8_t* buf, size_t len, const DfkCsvSpec* spec, DfkCsvOut* out)
{
    DfkRd r;
    uint32_t i, nrows = 0;
    int rc;
    size_t est;

    memset(out, 0, sizeof(*out));
    memset(&r, 0, sizeof(r));
    if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        buf += 3;
        len -= 3;
    }
    r.t.b = buf;
    r.t.len = len;
    r.t.delim = spec->delim;
    r.t.quote = spec->quote;
    r.t.strict = spec->strict;
    r.s = spec;
    r.out = out;
    if (len == 0) {
        snprintf(out->err_text, sizeof(out->err_text), "the input is empty (no header)");
        return DFK_CSV_E_EMPTY;
    }
    if ((rc = read_header(&r)) != 0)
        goto fail;
    r.data_start = r.t.i;
    est = 1;
    {
        const uint8_t* p = buf + r.t.i;
        const uint8_t* e = buf + len;
        while (p < e && (p = memchr(p, '\n', (size_t)(e - p))) != NULL) {
            est++;
            p++;
        }
    }
    if (spec->limit < est)
        est = (size_t)spec->limit + 1;
    r.cap = est;
    if ((rc = plan_columns(&r)) != 0)
        goto fail;
    if ((rc = scan_rows(&r, 0, &nrows)) != 0)
        goto fail;
    if (r.pass2) {
        for (i = 0; i < r.nout; i++) {
            DfkCol* c = &r.c[i];
            if (c->state == COL_LATE) {
                c->codes = malloc((r.cap ? r.cap : 1) * sizeof(*c->codes));
                if (!c->codes) {
                    rc = DFK_CSV_E_OOM;
                    goto fail;
                }
            }
        }
        if ((rc = scan_rows(&r, 1, &nrows)) != 0)
            goto fail;
    }
    out->nrows = nrows;
    for (i = 0; i < r.nout; i++) {
        DfkCol* c = &r.c[i];
        DfkCsvCol* o = &out->cols[i];
        if (c->state == COL_NUM || c->state == COL_F64) {
            o->kind = DFK_CSV_F64;
            o->f64 = c->f;
            c->f = NULL;
        } else if (c->state == COL_I32) {
            o->kind = DFK_CSV_I32;
            o->i32 = c->iv;
            c->iv = NULL;
        } else {
            o->kind = DFK_CSV_STR;
            o->codes = c->codes;
            o->dict = c->dict;
            o->dict_len = c->dict_len;
            c->codes = NULL;
            c->dict = NULL;
            c->dict_len = 0;
        }
        col_release(c);
    }
    free(r.c);
    free(r.map);
    free(r.t.tmp);
    return 0;
fail:
    if (r.c)
        for (i = 0; i < r.nout; i++)
            col_release(&r.c[i]);
    free(r.c);
    free(r.map);
    free(r.t.tmp);
    if (rc == DFK_CSV_E_QUOTE)
        snprintf(out->err_text, sizeof(out->err_text), "unterminated quoted field");
    else if (rc == DFK_CSV_E_GARBAGE)
        snprintf(out->err_text, sizeof(out->err_text), "unexpected text after a closing quote");
    else if (rc == DFK_CSV_E_OOM)
        snprintf(out->err_text, sizeof(out->err_text), "out of memory");
    dfk_csv_free(out);
    return rc;
}

void dfk_csv_free(DfkCsvOut* out)
{
    uint32_t i, k;
    if (out->cols)
        for (i = 0; i < out->ncols; i++) {
            DfkCsvCol* o = &out->cols[i];
            free(o->f64);
            free(o->i32);
            free(o->codes);
            for (k = 0; k < o->dict_len; k++)
                free(o->dict[k]);
            free(o->dict);
        }
    free(out->cols);
    out->cols = NULL;
    out->ncols = 0;
    if (out->hdr)
        for (i = 0; i < out->nhdr; i++)
            free(out->hdr[i]);
    free(out->hdr);
    out->hdr = NULL;
    out->nhdr = 0;
}
