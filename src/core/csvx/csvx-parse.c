#include "../dyn-csvx.h"
#include "../dyn-simdk.h"

#include <stdlib.h>
#include <string.h>

#define CSVX_SWAR_ONES 0x0101010101010101ULL
#define CSVX_SWAR_HIGHS 0x8080808080808080ULL

static inline uint64_t csvx_swar_has_byte(uint64_t v, uint8_t b)
{
    v ^= CSVX_SWAR_ONES * b;
    return (v - CSVX_SWAR_ONES) & ~v & CSVX_SWAR_HIGHS;
}

int csvx_buf_reserve(CsvxBuf* b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return 0;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + extra)
        nc *= 2;
    char* np = (char*)realloc(b->p, nc);
    if (!np)
        return -1;
    b->p = np;
    b->cap = nc;
    return 0;
}

int csvx_row_push(CsvxRow* row, char* s)
{
    if (row->n == row->cap) {
        size_t nc = row->cap ? row->cap * 2 : 8;
        char** nv = (char**)realloc(row->f, nc * sizeof(char*));
        if (!nv)
            return -1;
        row->f = nv;
        row->cap = nc;
    }
    row->f[row->n++] = s;
    return 0;
}

int csvx_table_push(CsvxTable* t, CsvxRow row)
{
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64;
        CsvxRow* nr = (CsvxRow*)realloc(t->r, nc * sizeof(CsvxRow));
        if (!nr)
            return -1;
        t->r = nr;
        t->cap = nc;
    }
    t->r[t->n++] = row;
    return 0;
}

static char* csvx_block_new(CsvxTable* t, size_t cap, size_t off)
{
    CsvxBlock* b;
    if (t->nblk == t->capblk) {
        size_t nc = t->capblk ? t->capblk * 2 : 8;
        CsvxBlock** nv = (CsvxBlock**)realloc(t->blk, nc * sizeof(CsvxBlock*));
        if (!nv)
            return NULL;
        t->blk = nv;
        t->capblk = nc;
    }
    b = (CsvxBlock*)malloc(sizeof(*b) + cap);
    if (!b)
        return NULL;
    b->cap = cap;
    b->off = off;
    t->blk[t->nblk++] = b;
    return b->data;
}

char* csvx_tcell_dup(CsvxTable* t, const char* s, size_t n)
{
    char* d;
    CsvxBlock* cur;
    if (n == 0)
        return NULL;
    if (n + 1 > CSVX_ARENA_BLOCK) {
        d = csvx_block_new(t, n + 1, n + 1);
        if (!d)
            return NULL;
        memcpy(d, s, n);
        d[n] = 0;
        return d;
    }
    cur = t->cur ? t->blk[t->cur - 1] : NULL;
    if (!cur || cur->off + n + 1 > cur->cap) {
        if (!csvx_block_new(t, CSVX_ARENA_BLOCK, 0))
            return NULL;
        t->cur = t->nblk;
        cur = t->blk[t->cur - 1];
    }
    d = cur->data + cur->off;
    memcpy(d, s, n);
    d[n] = 0;
    cur->off += n + 1;
    return d;
}

void csvx_table_free(CsvxTable* t)
{
    size_t i;
    for (i = 0; i < t->n; i++)
        free(t->r[i].f);
    free(t->r);
    for (i = 0; i < t->nblk; i++)
        free(t->blk[i]);
    free(t->blk);
    t->blk = NULL;
    t->nblk = t->capblk = t->cur = 0;
    t->r = NULL;
    t->n = t->cap = 0;
}

int csvx_scan(const uint8_t* buf, size_t len, CsvxSink* k, int strict,
    int* errrow, uint8_t delim, uint8_t quote)
{
    uint8_t stop[3] = { delim, '\n', '\r' };
    size_t i = 0;
    CsvxBuf fld;

    if (len == 0)
        return 0;
    memset(&fld, 0, sizeof(fld));
    for (;;) {
        int closed = 0;
        if (i < len && buf[i] == quote) {
            fld.len = 0;
            i++;
            while (i < len) {
                while (i + 8 <= len) {
                    uint64_t v;
                    memcpy(&v, buf + i, 8);
                    if (!csvx_swar_has_byte(v, quote)) {
                        if (csvx_buf_put(&fld, (const char*)buf + i, 8))
                            goto oom;
                        i += 8;
                        continue;
                    }
                    while (buf[i] != quote) {
                        if (csvx_buf_putc(&fld, (char)buf[i]))
                            goto oom;
                        i++;
                    }
                    goto quote;
                }
                while (i < len && buf[i] != quote) {
                    if (csvx_buf_putc(&fld, (char)buf[i]))
                        goto oom;
                    i++;
                }
                if (i >= len)
                    break;
            quote:
                if (i + 1 < len && buf[i + 1] == quote) {
                    if (csvx_buf_putc(&fld, (char)quote))
                        goto oom;
                    i += 2;
                    continue;
                }
                i++;
                closed = 1;
                break;
            }
            if (strict) {
                if (!closed) {
                    *errrow = (int)k->rows + 1;
                    csvx_buf_free(&fld);
                    return CSVX_SYNTAX_QUOTE;
                }
                if (i < len && buf[i] != delim && buf[i] != '\n' && buf[i] != '\r') {
                    *errrow = (int)k->rows + 1;
                    csvx_buf_free(&fld);
                    return CSVX_SYNTAX_GARBAGE;
                }
            }
            if (k->field(k, fld.p, fld.len, 0))
                goto oom;
        } else {
            size_t p = simd.find_first_of(buf + i, len - i, stop, sizeof stop);
            if (p == SIZE_MAX)
                p = len - i;
            if (k->field(k, (const char*)(buf + i), p, 1))
                goto oom;
            i += p;
        }
        if (i >= len) {
            if (k->row(k))
                goto oom;
            k->rows++;
            break;
        }
        {
            uint8_t c = buf[i];
            if (c == delim) {
                i++;
                continue;
            }
            if (c == '\r' && i + 1 < len && buf[i + 1] == '\n')
                i += 2;
            else
                i++;
            if (k->row(k))
                goto oom;
            k->rows++;
            if (i >= len)
                break;
        }
    }
    csvx_buf_free(&fld);
    return 0;
oom:
    csvx_buf_free(&fld);
    return -1;
}

typedef struct {
    CsvxSink base;
    CsvxTable* t;
    CsvxRow row;
} CsvxTableSink;

static int csvx_table_field(CsvxSink* k, const char* p, size_t n, int stable)
{
    CsvxTableSink* ts = (CsvxTableSink*)k;
    char* s = NULL;
    (void)stable;
    if (n) {
        s = csvx_tcell_dup(ts->t, p, n);
        if (!s)
            return -1;
    }
    return csvx_row_push(&ts->row, s);
}

static int csvx_table_row(CsvxSink* k)
{
    CsvxTableSink* ts = (CsvxTableSink*)k;
    if (csvx_table_push(ts->t, ts->row))
        return -1;
    memset(&ts->row, 0, sizeof(ts->row));
    return 0;
}

int csvx_parse_table(const uint8_t* buf, size_t len, CsvxTable* t, int strict,
    int* errrow, uint8_t delim, uint8_t quote)
{
    CsvxTableSink ts;
    int r;
    memset(t, 0, sizeof(*t));
    if (len == 0)
        return 0;

    size_t est = simd.count_u8(buf, '\n', len) + 1;
    t->cap = est < 64 ? 64 : est;
    if (t->cap > SIZE_MAX / sizeof(CsvxRow))
        return -1;
    t->r = (CsvxRow*)malloc(t->cap * sizeof(CsvxRow));
    if (!t->r)
        return -1;
    memset(&ts, 0, sizeof(ts));
    ts.base.field = csvx_table_field;
    ts.base.row = csvx_table_row;
    ts.t = t;
    r = csvx_scan(buf, len, &ts.base, strict, errrow, delim, quote);
    free(ts.row.f);
    if (r < 0)
        csvx_table_free(t);
    return r;
}
