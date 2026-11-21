#include "../dyn-csvx.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int csvx_emit_field_n(CsvxBuf* b, const uint8_t* s, size_t n)
{
    size_t i, run = 0;
    for (i = 0; i < n; i++)
        if (s[i] == ',' || s[i] == '"' || s[i] == '\n' || s[i] == '\r')
            break;
    if (i == n)
        return csvx_buf_put(b, (const char*)s, n);
    if (csvx_buf_reserve(b, n + n / 4 + 2) || csvx_buf_putc(b, '"'))
        return -1;
    for (i = 0; i < n; i++)
        if (s[i] == '"') {
            if (csvx_buf_put(b, (const char*)s + run, i + 1 - run))
                return -1;
            run = i;
        }
    if (csvx_buf_put(b, (const char*)s + run, n - run))
        return -1;
    return csvx_buf_putc(b, '"');
}

int csvx_view_field(CsvxView* v, size_t r, uint32_t c, const uint8_t** pp, size_t* pn)
{
    const uint8_t* p;
    size_t n;
    if (csvx_field(v->ix, r, c, &p, &n)) {
        if (csvx_needs_unescape(p, n, v->ix->quote)) {
            if (n > v->tcap) {
                uint8_t* t = (uint8_t*)realloc(v->tmp, n);
                if (!t)
                    return -1;
                v->tmp = t;
                v->tcap = n;
            }
            n = csvx_unquote(p, n, v->ix->quote, v->tmp);
            p = v->tmp;
        } else {
            p++;
            n = n >= 2 ? n - 2 : 0;
        }
    }
    *pp = p;
    *pn = n;
    return 0;
}

void csvx_overlay_init(CsvxOverlay* o)
{
    memset(o, 0, sizeof(*o));
    o->drop_row = SIZE_MAX;
    o->set_row = SIZE_MAX;
    o->drop_col = UINT32_MAX;
}

static int row_plain(const CsvxIndex* ix, size_t r, size_t* ps, size_t* pe)
{
    uint32_t f0 = ix->row_first[r], f1 = ix->row_first[r + 1];
    size_t s, e;
    if (f1 == f0)
        return 0;
    s = ix->row_start[r];
    e = ix->sep[f1 - 1];
    if (e >= ix->len || ix->buf[e] != '\n' || (e > s && memchr(ix->buf + s, ix->quote, e - s)))
        return 0;
    *ps = s;
    *pe = e + 1;
    return 1;
}

static int emit_plain(const CsvxIndex* ix, const CsvxOverlay* ov, size_t r, size_t s, size_t e, CsvxBuf* out)
{
    uint32_t nf = csvx_row_fields(ix, r), f0 = ix->row_first[r], c;
    const char* b = (const char*)ix->buf;
    if (ov->add_col) {
        if (csvx_buf_put(out, b + s, e - 1 - s))
            return -1;
        for (c = nf; c < ix->width; c++)
            if (csvx_buf_putc(out, ','))
                return -1;
        if (csvx_buf_putc(out, ','))
            return -1;
        if (r == 0 ? csvx_emit_field_n(out, (const uint8_t*)ov->add_head, ov->add_head_len) : csvx_buf_put(out, ov->add_def_q, ov->add_def_qlen))
            return -1;
        return csvx_buf_putc(out, '\n');
    }
    c = ov->drop_col;
    if (c == 0)
        return nf == 1 ? csvx_buf_putc(out, '\n') : csvx_buf_put(out, b + ix->sep[f0] + 1, e - ix->sep[f0] - 1);
    if (csvx_buf_put(out, b + s, ix->sep[f0 + c - 1] - s))
        return -1;
    return csvx_buf_put(out, b + ix->sep[f0 + c], e - ix->sep[f0 + c]);
}

static int emit_row(CsvxView* v, const CsvxOverlay* ov, size_t r, CsvxBuf* out)
{
    uint32_t nf = csvx_row_fields(v->ix, r), nout = nf, c;
    int first = 1;
    size_t ps, pe;
    if (r != ov->set_row && (ov->add_col || ov->drop_col < nf) && row_plain(v->ix, r, &ps, &pe))
        return emit_plain(v->ix, ov, r, ps, pe, out);
    if (r == ov->set_row && ov->set_col >= nout)
        nout = ov->set_col + 1;
    if (ov->add_col && nout < v->ix->width)
        nout = v->ix->width;
    for (c = 0; c < nout; c++) {
        const uint8_t* p = (const uint8_t*)"";
        size_t n = 0;
        if (c == ov->drop_col && c < nf)
            continue;
        if (!first && csvx_buf_putc(out, ','))
            return -1;
        first = 0;
        if (r == ov->set_row && c == ov->set_col) {
            p = (const uint8_t*)ov->set_val;
            n = ov->set_len;
        } else if (c < nf && csvx_view_field(v, r, c, &p, &n))
            return -1;
        if (csvx_emit_field_n(out, p, n))
            return -1;
    }
    if (ov->add_col) {
        if (!first && csvx_buf_putc(out, ','))
            return -1;
        if (r == 0 ? csvx_emit_field_n(out, (const uint8_t*)ov->add_head, ov->add_head_len) : csvx_emit_field_n(out, (const uint8_t*)ov->add_def, ov->add_def_len))
            return -1;
    }
    return csvx_buf_putc(out, '\n');
}

static int serialize_rows(CsvxView* v, const CsvxOverlay* ov, CsvxBuf* out);

int csvx_view_serialize(CsvxView* v, const CsvxOverlay* ov, CsvxBuf* out)
{
    const CsvxIndex* ix = v->ix;
    if (csvx_buf_reserve(out, ix->len + ix->len / 16 + 64))
        return -1;
    if (ov->add_col) {
        CsvxBuf q;
        CsvxOverlay o2 = *ov;
        int rc;
        memset(&q, 0, sizeof(q));
        if (csvx_emit_field_n(&q, (const uint8_t*)ov->add_def, ov->add_def_len) || csvx_buf_putc(&q, 0)) {
            csvx_buf_free(&q);
            return -1;
        }
        o2.add_def_q = q.p;
        o2.add_def_qlen = q.len - 1;
        rc = serialize_rows(v, &o2, out);
        csvx_buf_free(&q);
        return rc;
    }
    return serialize_rows(v, ov, out);
}

static int serialize_rows(CsvxView* v, const CsvxOverlay* ov, CsvxBuf* out)
{
    const CsvxIndex* ix = v->ix;
    size_t r, run = 0, runend = 0;
    int open_run = 0;
    for (r = 0; r < ix->nrows; r++) {
        size_t s, e;
        int touched = r == ov->drop_row || r == ov->set_row || ov->add_col
            || (ov->drop_col != UINT32_MAX && ov->drop_col < csvx_row_fields(ix, r));
        if (!touched && row_plain(ix, r, &s, &e)) {
            if (!open_run) {
                run = s;
                open_run = 1;
            }
            runend = e;
            continue;
        }
        if (open_run && csvx_buf_put(out, (const char*)ix->buf + run, runend - run))
            return -1;
        open_run = 0;
        if (r == ov->drop_row)
            continue;
        if (emit_row(v, ov, r, out))
            return -1;
    }
    if (open_run && csvx_buf_put(out, (const char*)ix->buf + run, runend - run))
        return -1;
    return 0;
}
