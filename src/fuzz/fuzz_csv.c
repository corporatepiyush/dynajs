#include "dynajs.h"
#include "dyna-nat.h"
#include "src/modules/csv/csv-internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void simd_init(void);

static int inited;

typedef struct {
    csv_sink base;
    uint8_t* b;
    size_t bn, bcap;
    size_t* off;
    size_t* len;
    size_t nf, fcap;
    uint32_t* rowf;
    size_t nr, rcap;
    uint32_t cur;
} Rec;

static void* grow(void* p, size_t* cap, size_t need, size_t el)
{
    size_t c = *cap ? *cap : 16;
    void* q;
    if (need <= *cap)
        return p;
    while (c < need)
        c *= 2;
    q = realloc(p, c * el);
    if (!q)
        abort();
    *cap = c;
    return q;
}

static int rec_field(csv_sink* k, const char* p, size_t n, int stable)
{
    Rec* r = (Rec*)k;
    (void)stable;
    r->b = (uint8_t*)grow(r->b, &r->bcap, r->bn + n + 1, 1);
    r->off = (size_t*)grow(r->off, &r->fcap, r->nf + 1, sizeof(size_t));
    r->len = (size_t*)realloc(r->len, r->fcap * sizeof(size_t));
    if (!r->len)
        abort();
    if (n)
        memcpy(r->b + r->bn, p, n);
    r->off[r->nf] = r->bn;
    r->len[r->nf] = n;
    r->bn += n;
    r->nf++;
    r->cur++;
    return 0;
}

static int rec_row(csv_sink* k)
{
    Rec* r = (Rec*)k;
    r->rowf = (uint32_t*)grow(r->rowf, &r->rcap, r->nr + 1, sizeof(uint32_t));
    r->rowf[r->nr++] = r->cur;
    r->cur = 0;
    return 0;
}

static void rec_free(Rec* r)
{
    free(r->b);
    free(r->off);
    free(r->len);
    free(r->rowf);
}

static void check_index(const CsvxIndex* ix, const Rec* rec)
{
    size_t r, f = 0;
    uint8_t* tmp = NULL;
    if (ix->nrows != rec->nr)
        abort();
    for (r = 0; r < rec->nr; r++) {
        uint32_t c, nf = csvx_row_fields(ix, r);
        if (nf != rec->rowf[r])
            abort();
        for (c = 0; c < nf; c++, f++) {
            const uint8_t* p;
            size_t n;
            if (csvx_field(ix, r, c, &p, &n)) {
                uint8_t* t = (uint8_t*)realloc(tmp, n ? n : 1);
                if (!t)
                    abort();
                tmp = t;
                n = csvx_unquote(p, n, ix->quote, tmp);
                p = tmp;
            }
            if (n != rec->len[f] || (n && memcmp(p, rec->b + rec->off[f], n) != 0))
                abort();
        }
    }
    free(tmp);
}

static void put(Buf* b, const void* p, size_t n)
{
    if (buf_put(b, (const char*)p, n))
        abort();
}

static void ref_field(Buf* b, const uint8_t* p, size_t n)
{
    size_t i;
    int q = 0;
    for (i = 0; i < n; i++)
        if (p[i] == ',' || p[i] == '"' || p[i] == '\n' || p[i] == '\r')
            q = 1;
    if (!q) {
        put(b, p, n);
        return;
    }
    put(b, "\"", 1);
    for (i = 0; i < n; i++) {
        if (p[i] == '"')
            put(b, "\"", 1);
        put(b, p + i, 1);
    }
    put(b, "\"", 1);
}

static void ref_serialize(const Rec* rec, const CsvOverlay* ov, Buf* out)
{
    size_t r, f = 0;
    uint32_t width = rec->nr ? rec->rowf[0] : 0;
    for (r = 0; r < rec->nr; f += rec->rowf[r], r++) {
        uint32_t nf = rec->rowf[r], nout = nf, c;
        int first = 1;
        if (r == ov->drop_row)
            continue;
        if (r == ov->set_row && ov->set_col >= nout)
            nout = ov->set_col + 1;
        if (ov->add_col && nout < width)
            nout = width;
        for (c = 0; c < nout; c++) {
            if (c == ov->drop_col && c < nf)
                continue;
            if (!first)
                put(out, ",", 1);
            first = 0;
            if (r == ov->set_row && c == ov->set_col)
                ref_field(out, (const uint8_t*)ov->set_val, ov->set_len);
            else if (c < nf)
                ref_field(out, rec->b + rec->off[f + c], rec->len[f + c]);
        }
        if (ov->add_col) {
            if (!first)
                put(out, ",", 1);
            if (r == 0)
                ref_field(out, (const uint8_t*)ov->add_head, ov->add_head_len);
            else
                ref_field(out, (const uint8_t*)ov->add_def, ov->add_def_len);
        }
        put(out, "\n", 1);
    }
}

static void check_overlay(const CsvxIndex* ix, const Rec* rec, uint8_t pick)
{
    CsvView v;
    CsvOverlay ov;
    Buf got, want;
    size_t nr = rec->nr ? rec->nr : 1;
    memset(&v, 0, sizeof(v));
    memset(&got, 0, sizeof(got));
    memset(&want, 0, sizeof(want));
    v.ix = *ix;
    csv_overlay_init(&ov);
    switch (pick % 5) {
    case 1:
        ov.drop_row = (pick / 5) % nr;
        break;
    case 2:
        ov.set_row = (pick / 5) % nr;
        ov.set_col = (pick / 5) % 4;
        ov.set_val = "v,\"x\r";
        ov.set_len = 5;
        break;
    case 3:
        ov.drop_col = (pick / 5) % 4;
        break;
    case 4:
        ov.add_col = 1;
        ov.add_head = "h\"";
        ov.add_head_len = 2;
        ov.add_def = "d,\n";
        ov.add_def_len = 3;
        break;
    default:
        break;
    }
    if (csv_view_serialize(&v, &ov, &got))
        abort();
    ref_serialize(rec, &ov, &want);
    if (got.len != want.len || (got.len && memcmp(got.p, want.p, got.len) != 0))
        abort();
    free(v.tmp);
    buf_free(&got);
    buf_free(&want);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const uint8_t dialects[4][2] = { { ',', '"' }, { ';', '\'' }, { '\t', '"' }, { '|', '"' } };
    uint8_t delim, quote, pick;
    Rec rec;
    CsvxIndex ix;
    int rc, errrow = 0;

    if (!inited) {
        simd_init();
        inited = 1;
    }
    if (size < 2 || size > (1u << 20))
        return 0;
    delim = dialects[data[0] & 3][0];
    quote = dialects[data[0] & 3][1];
    pick = data[1];
    data += 2;
    size -= 2;

    memset(&rec, 0, sizeof(rec));
    rec.base.field = rec_field;
    rec.base.row = rec_row;
    if (csv_scan_sink(data, size, &rec.base, 0, &errrow, delim, quote) != 0)
        abort();

    rc = csvx_build(&ix, data, size, delim, quote, 0);
    if (rc == CSVX_IRREGULAR) {
        Buf cb;
        if (csv_canonicalise(data, size, 0, &errrow, delim, quote, &cb) != 0)
            abort();
        rc = csvx_build(&ix, cb.p ? (const uint8_t*)cb.p : (const uint8_t*)"", cb.len, delim, quote, 0);
        if (rc != CSVX_OK)
            abort();
        check_index(&ix, &rec);
        if (delim == ',' && quote == '"')
            check_overlay(&ix, &rec, pick);
        csvx_free(&ix);
        buf_free(&cb);
    } else if (rc == CSVX_OK) {
        check_index(&ix, &rec);
        if (delim == ',' && quote == '"')
            check_overlay(&ix, &rec, pick);
        csvx_free(&ix);
    } else {
        abort();
    }
    rec_free(&rec);
    return 0;
}
