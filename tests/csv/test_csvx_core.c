/*
 * test_csvx_core.c -- src/core/csvx (the dyna:csv structural index and
 * column kernels) as a standalone C library.
 *
 * Oracle: the same index built on one thread. A multi-thread build must give
 * the identical separator array, row starts and row-first indices; the chunk
 * starts are picked by csvx_build, so the inputs place quoted fields holding
 * delimiters and newlines (and CRLF pairs) everywhere, and one test puts the
 * only irregular quote of a 4 MiB input exactly at the two-chunk split point:
 * a closing quote as the last byte of chunk 0 followed by text in chunk 1,
 * which only the carried "previous byte closed a quote" bit can catch.
 * Number parsing is checked against strtod/strtoll on seeded random text, and
 * the parallel dictionary build against a serial one.
 *
 * The row parser (csvx_scan / csvx_parse_table, moved out of dyna:csv) is
 * checked against a byte-at-a-time reference written here, on seeded random
 * text over the alphabet {a, delimiter, quote, \r, \n} in strict and lenient
 * mode, so the SWAR quote scan and the SIMD find_first_of path must agree with
 * it cell for cell, error row included; cells longer than one 64 KiB arena
 * block and tables spanning many blocks are compared byte for byte.
 *
 * The edit serializer (csvx_view_serialize: drop a row, set a cell, drop a
 * column, append a column; untouched rows copied as raw runs) is checked
 * against a reference that parses the same text with csvx_parse_table (not the
 * index the serializer reads), applies the edit to that table and writes it
 * back quoting only fields holding , " CR or LF. The random inputs are in that
 * canonical form, ragged rows included, so every byte must match; an appended
 * column pads short rows to the header row's width (CsvxIndex.width), not the
 * widest row.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-csvx.h"
#include "dyn-simdk.h"
#include "../lib/dyn-xalloc.h"

static int fails, checks;

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(uint32_t k)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng % k);
}

static size_t gen_csv(uint8_t* out, size_t cap, int crlf)
{
    static const char* const pieces[] = { "plain", "\"q,uo\"\"te\"", "\"multi\nline\"", "\"\"", "42", "-3.5e2", "\"a\r\nb\"", "" };
    size_t n = 0;
    while (n + 64 < cap) {
        int c, w = 5;
        for (c = 0; c < w; c++) {
            const char* p = pieces[rnd(8)];
            size_t l = strlen(p);
            memcpy(out + n, p, l);
            n += l;
            out[n++] = c + 1 < w ? ',' : '\n';
        }
        if (crlf && rnd(2)) {
            out[n - 1] = '\r';
            out[n++] = '\n';
        }
    }
    return n;
}

static int same_index(const CsvxIndex* a, const CsvxIndex* b)
{
    return a->nsep == b->nsep && a->nrows == b->nrows && a->width == b->width && a->ragged == b->ragged
        && memcmp(a->sep, b->sep, a->nsep * 4) == 0
        && memcmp(a->row_first, b->row_first, (a->nrows + 1) * 4) == 0
        && memcmp(a->row_start, b->row_start, a->nrows * 4) == 0;
}

static void test_threads_match_serial(void)
{
    size_t cap = (size_t)9 << 20, n;
    uint8_t* buf = (uint8_t*)malloc(cap);
    int round;
    for (round = 0; round < 6; round++) {
        CsvxIndex one, many;
        n = gen_csv(buf, cap - (size_t)rnd(1 << 20), round & 1);
        CHECK(csvx_build(&one, buf, n, ',', '"', 1) == CSVX_OK, "serial build");
        CHECK(csvx_build(&many, buf, n, ',', '"', 8) == CSVX_OK, "parallel build");
        CHECK(same_index(&one, &many), "round %d: parallel index == serial index (%zu bytes, %zu rows)", round, n, one.nrows);
        CHECK(!one.ragged && one.width == 5, "round %d: width 5, not ragged", round);
        csvx_free(&one);
        csvx_free(&many);
    }
    free(buf);
}

static void test_irregular_at_split(void)
{
    size_t len = (size_t)4 << 20, split = (len / 2) & ~(size_t)63, i;
    uint8_t* buf = (uint8_t*)malloc(len);
    CsvxIndex ix;
    for (i = 0; i < len; i++)
        buf[i] = (i % 16 == 15) ? '\n' : 'a';
    buf[split - 4] = '\n';
    buf[split - 3] = '"';
    buf[split - 2] = 'q';
    buf[split - 1] = '"';
    buf[split] = 'x';
    CHECK(csvx_build(&ix, buf, len, ',', '"', 2) == CSVX_IRREGULAR, "text after a closing quote that ends chunk 0 is irregular");
    csvx_free(&ix);
    buf[split] = ',';
    CHECK(csvx_build(&ix, buf, len, ',', '"', 2) == CSVX_OK, "a closing quote then a delimiter across the split is regular");
    csvx_free(&ix);
    buf[split - 3] = 'z';
    CHECK(csvx_build(&ix, buf, len, ',', '"', 2) == CSVX_IRREGULAR, "a quote opening mid-field is irregular");
    csvx_free(&ix);
    free(buf);
}

static void test_shapes(void)
{
    CsvxIndex ix;
    const uint8_t* p;
    size_t n;
    const char* s = "a,b\r\n\"x\"\"y\",\r\nlast";
    CHECK(csvx_build(&ix, (const uint8_t*)s, strlen(s), ',', '"', 1) == CSVX_OK, "small build");
    CHECK(ix.nrows == 3 && ix.ragged, "3 rows, ragged (last row has 1 field)");
    CHECK(csvx_field(&ix, 1, 0, &p, &n) == 1 && n == 6 && memcmp(p, "\"x\"\"y\"", 6) == 0, "quoted span is raw");
    CHECK(csvx_field(&ix, 1, 1, &p, &n) == 0 && n == 0, "empty field before CRLF");
    CHECK(csvx_field(&ix, 2, 0, &p, &n) == 0 && n == 4 && memcmp(p, "last", 4) == 0, "final row without newline");
    CHECK(csvx_field(&ix, 2, 3, &p, &n) == 0 && n == 0, "a cell past the row's end is empty");
    {
        uint8_t out[8];
        size_t k = csvx_unquote((const uint8_t*)"\"x\"\"y\"", 6, '"', out);
        CHECK(k == 3 && memcmp(out, "x\"y", 3) == 0, "unquote collapses doubled quotes");
    }
    csvx_free(&ix);
    CHECK(csvx_build(&ix, (const uint8_t*)"", 0, ',', '"', 1) == CSVX_OK && ix.nrows == 0, "empty input");
    csvx_free(&ix);
    CHECK(csvx_build(&ix, (const uint8_t*)"\"open", 5, ',', '"', 1) == CSVX_IRREGULAR, "unterminated quote is irregular");
    csvx_free(&ix);
}

static void test_numbers(void)
{
    char buf[64];
    int i;
    for (i = 0; i < 200000; i++) {
        double want, got = 0;
        int64_t iv = 0;
        long long wl;
        int len;
        switch (rnd(4)) {
        case 0:
            len = snprintf(buf, sizeof buf, "%u.%0*u", rnd(1000000000), (int)(1 + rnd(9)), rnd(1000000000));
            break;
        case 1:
            len = snprintf(buf, sizeof buf, "%s%u%u%u", rnd(2) ? "-" : "", rnd(1000000000), rnd(1000000000), rnd(100));
            break;
        case 2:
            len = snprintf(buf, sizeof buf, "%.17g", (rnd(2) ? -1.0 : 1.0) * (double)rnd(1u << 31) * pow(10.0, (double)rnd(40) - 20.0));
            break;
        default:
            len = snprintf(buf, sizeof buf, "%u", rnd(4000000000u));
            break;
        }
        want = strtod(buf, NULL);
        CHECK(csvx_parse_f64((const uint8_t*)buf, (size_t)len, &got) == 0 && got == want, "f64 '%s': got %.17g want %.17g", buf, got, want);
        if (!strpbrk(buf, ".eE")) {
            wl = strtoll(buf, NULL, 10);
            if (len < 19)
                CHECK(csvx_parse_i64((const uint8_t*)buf, (size_t)len, &iv) == 0 && iv == wl, "i64 '%s'", buf);
        }
        if (fails > 20)
            return;
    }
    {
        int64_t v = 0;
        CHECK(csvx_parse_i64((const uint8_t*)"9223372036854775807", 19, &v) == 0 && v == INT64_MAX, "i64 max");
        CHECK(csvx_parse_i64((const uint8_t*)"-9223372036854775808", 20, &v) == 0 && v == INT64_MIN, "i64 min");
        CHECK(csvx_parse_i64((const uint8_t*)"9223372036854775808", 19, &v) != 0, "i64 overflow");
        CHECK(csvx_parse_i64((const uint8_t*)"12a", 3, &v) != 0, "i64 garbage");
    }
}

static void test_dict_parallel(void)
{
    size_t rows = 300000, i, n = 0, cap = rows * 16 + 64;
    uint8_t* buf = (uint8_t*)malloc(cap);
    CsvxIndex ix;
    CsvxColumnJob a, b;
    int ok = 1;
    for (i = 0; i < rows; i++)
        n += (size_t)snprintf((char*)buf + n, cap - n, "k%u,%u\n", rnd(5000), (unsigned)i);
    CHECK(csvx_build(&ix, buf, n, ',', '"', 8) == CSVX_OK, "dict input builds");
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.kind = b.kind = CSVX_COL_DICT;
    a.nrows = b.nrows = ix.nrows;
    a.out = malloc(ix.nrows * 4);
    b.out = malloc(ix.nrows * 4);
    csvx_dict_init(&a.dict, 64);
    csvx_dict_init(&b.dict, 64);
    a.dict.seed = b.dict.seed = 7;
    CHECK(csvx_column(&ix, &a, 1) == 0 && csvx_column(&ix, &b, 8) == 0, "dict columns");
    CHECK(a.dict.n == b.dict.n, "same distinct count (%u vs %u)", a.dict.n, b.dict.n);
    for (i = 0; ok && i < ix.nrows; i++) {
        int32_t ca = ((int32_t*)a.out)[i], cb = ((int32_t*)b.out)[i];
        ok = ca == cb && a.dict.len[ca] == b.dict.len[cb];
    }
    CHECK(ok, "parallel dictionary codes == serial codes (first-appearance order)");
    free(a.out);
    free(b.out);
    csvx_dict_free(&a.dict);
    csvx_dict_free(&b.dict);
    csvx_free(&ix);
    free(buf);
}

typedef struct {
    char cells[4096][64];
    size_t lens[4096];
    int nulls[4096];
    size_t rowlen[512];
    size_t nrows, ncells;
} RefTab;

static int ref_parse(const char* b, size_t len, int strict, int* errrow, char d, char q, RefTab* o)
{
    size_t i = 0, rc = 0;
    o->nrows = o->ncells = 0;
    if (!len)
        return 0;
    for (;;) {
        char cell[64];
        size_t cn = 0;
        int closed = 0, quoted = 0;
        if (i < len && b[i] == q) {
            quoted = 1;
            i++;
            while (i < len) {
                if (b[i] != q) {
                    cell[cn++] = b[i++];
                    continue;
                }
                if (i + 1 < len && b[i + 1] == q) {
                    cell[cn++] = q;
                    i += 2;
                    continue;
                }
                i++;
                closed = 1;
                break;
            }
            if (strict && (!closed || (i < len && b[i] != d && b[i] != '\n' && b[i] != '\r'))) {
                *errrow = (int)o->nrows + 1;
                return closed ? CSVX_SYNTAX_GARBAGE : CSVX_SYNTAX_QUOTE;
            }
        } else
            while (i < len && b[i] != d && b[i] != '\n' && b[i] != '\r')
                cell[cn++] = b[i++];
        (void)quoted;
        memcpy(o->cells[o->ncells], cell, cn);
        o->lens[o->ncells] = cn;
        o->nulls[o->ncells] = cn == 0;
        o->ncells++;
        rc++;
        if (i >= len) {
            o->rowlen[o->nrows++] = rc;
            break;
        }
        if (b[i] == d) {
            i++;
            continue;
        }
        if (b[i] == '\r' && i + 1 < len && b[i + 1] == '\n')
            i += 2;
        else
            i++;
        o->rowlen[o->nrows++] = rc;
        rc = 0;
        if (i >= len)
            break;
    }
    return 0;
}

static void test_parse_random(void)
{
    static RefTab ref;
    static const char D[2] = { ',', ';' }, Q[2] = { '"', '\'' };
    char buf[600];
    int it;
    for (it = 0; it < 4000; it++) {
        char d = D[it & 1], q = Q[(it >> 1) & 1], al[6];
        size_t len = rnd(40) + (it % 7 == 0 ? rnd(500) : 0), i, r, c, k = 0;
        int strict = (it >> 2) & 1, e1 = -7, e2 = -7, r1, r2;
        CsvxTable t;
        al[0] = 'a';
        al[1] = d;
        al[2] = q;
        al[3] = '\r';
        al[4] = '\n';
        al[5] = 'b';
        for (i = 0; i < len; i++)
            buf[i] = al[rnd(6)];
        r1 = ref_parse(buf, len, strict, &e1, d, q, &ref);
        r2 = csvx_parse_table((const uint8_t*)buf, len, &t, strict, &e2, (uint8_t)d, (uint8_t)q);
        CHECK(r1 == r2 && (r1 == 0 || e1 == e2), "iter %d: rc %d/%d errrow %d/%d", it, r1, r2, e1, e2);
        if (r1 == 0 && r2 == 0) {
            CHECK(t.n == ref.nrows, "iter %d: %zu rows, reference %zu", it, t.n, ref.nrows);
            for (r = 0; r < t.n && r < ref.nrows; r++) {
                CHECK(t.r[r].n == ref.rowlen[r], "iter %d row %zu: %zu cells, reference %zu", it, r, t.r[r].n, ref.rowlen[r]);
                for (c = 0; c < t.r[r].n && c < ref.rowlen[r]; c++, k++) {
                    const char* s = t.r[r].f[c];
                    CHECK(ref.nulls[k] ? s == NULL : (s && strlen(s) == ref.lens[k] && memcmp(s, ref.cells[k], ref.lens[k]) == 0),
                        "iter %d cell %zu,%zu differs", it, r, c);
                }
                k += ref.rowlen[r] > t.r[r].n ? ref.rowlen[r] - t.r[r].n : 0;
            }
        }
        if (r2 >= 0)
            csvx_table_free(&t);
    }
}

static void test_parse_arena(void)
{
    size_t big = (size_t)CSVX_ARENA_BLOCK * 2 + 17, n = big + 4000 * 12 + 16, i, len = 0;
    char* buf = xmalloc(n);
    CsvxTable t;
    int er = 0, rc;
    buf[len++] = '"';
    for (i = 0; i < big; i++)
        buf[len++] = (char)('a' + (i % 26));
    buf[len++] = '"';
    buf[len++] = '\n';
    for (i = 0; i < 4000; i++)
        len += (size_t)snprintf(buf + len, n - len, "%06zu,x%zu\n", i, i % 10);
    rc = csvx_parse_table((const uint8_t*)buf, len, &t, 1, &er, ',', '"');
    CHECK(rc == 0 && t.n == 4001, "arena table: rc %d rows %zu", rc, t.n);
    if (rc == 0) {
        CHECK(strlen(t.r[0].f[0]) == big && t.r[0].f[0][big - 1] == (char)('a' + ((big - 1) % 26)), "big cell content");
        for (i = 0; i < 4000; i++) {
            char want[16];
            snprintf(want, sizeof want, "%06zu", i);
            if (strcmp(t.r[i + 1].f[0], want) != 0) {
                CHECK(0, "arena cell %zu = %s", i, t.r[i + 1].f[0]);
                break;
            }
        }
        CHECK(t.nblk >= 2, "expected several arena blocks, got %zu", t.nblk);
        csvx_table_free(&t);
    }
    free(buf);
}

static void ref_field(CsvxBuf* o, const char* s)
{
    size_t n = s ? strlen(s) : 0, i;
    int q = 0;
    for (i = 0; i < n; i++)
        q |= s[i] == ',' || s[i] == '"' || s[i] == '\n' || s[i] == '\r';
    if (!q) {
        csvx_buf_put(o, s ? s : "", n);
        return;
    }
    csvx_buf_putc(o, '"');
    for (i = 0; i < n; i++) {
        if (s[i] == '"')
            csvx_buf_putc(o, '"');
        csvx_buf_putc(o, s[i]);
    }
    csvx_buf_putc(o, '"');
}

static void test_serialize(void)
{
    static const char AL[] = "ab,\"\n \r";
    int it;
    for (it = 0; it < 3000; it++) {
        CsvxBuf in, want, got;
        CsvxTable t;
        CsvxIndex ix;
        CsvxView v;
        CsvxOverlay ov;
        size_t nr = 1 + rnd(12), r, i, width = 0;
        int er = 0, op = it % 4;
        char setv[8];
        memset(&in, 0, sizeof in);
        memset(&want, 0, sizeof want);
        memset(&got, 0, sizeof got);
        for (r = 0; r < nr; r++) {
            size_t nf = 1 + rnd(5), c;
            for (c = 0; c < nf; c++) {
                char cell[8];
                size_t cl = c == 0 ? 1 + rnd(4) : rnd(5), k;
                for (k = 0; k < cl; k++)
                    cell[k] = AL[rnd(7)];
                if (c == 0 && (cell[0] == ',' || cell[0] == '"' || cell[0] == '\n' || cell[0] == ' ' || cell[0] == '\r'))
                    cell[0] = 'a';
                cell[cl] = 0;
                if (c)
                    csvx_buf_putc(&in, ',');
                ref_field(&in, cell);
            }
            csvx_buf_putc(&in, '\n');
        }
        if (csvx_parse_table((const uint8_t*)in.p, in.len, &t, 1, &er, ',', '"') != 0
            || csvx_build(&ix, (const uint8_t*)in.p, in.len, ',', '"', 1) != CSVX_OK) {
            CHECK(0, "iter %d: input does not parse", it);
            csvx_buf_free(&in);
            continue;
        }
        width = t.r[0].n;
        csvx_overlay_init(&ov);
        if (op == 0)
            ov.drop_row = rnd((uint32_t)t.n);
        else if (op == 1) {
            snprintf(setv, sizeof setv, "%c\"%c", AL[rnd(7)], AL[rnd(7)]);
            ov.set_row = rnd((uint32_t)t.n);
            ov.set_col = rnd(7);
            ov.set_val = setv;
            ov.set_len = strlen(setv);
        } else if (op == 2)
            ov.drop_col = rnd(6);
        else {
            ov.add_col = 1;
            ov.add_head = "H,1";
            ov.add_head_len = 3;
            ov.add_def = it & 8 ? "d\"q" : "";
            ov.add_def_len = strlen(ov.add_def);
        }
        for (r = 0; r < t.n; r++) {
            const CsvxRow* row = &t.r[r];
            size_t nout = row->n, c;
            int first = 1;
            if (r == ov.drop_row)
                continue;
            if (r == ov.set_row && ov.set_col >= nout)
                nout = ov.set_col + 1;
            if (ov.add_col && nout < width)
                nout = width;
            for (c = 0; c < nout; c++) {
                if (c == ov.drop_col && c < row->n)
                    continue;
                if (!first)
                    csvx_buf_putc(&want, ',');
                first = 0;
                ref_field(&want, r == ov.set_row && c == ov.set_col ? setv : (c < row->n ? row->f[c] : NULL));
            }
            if (ov.add_col) {
                if (!first)
                    csvx_buf_putc(&want, ',');
                ref_field(&want, r == 0 ? ov.add_head : ov.add_def);
            }
            csvx_buf_putc(&want, '\n');
        }
        v.ix = &ix;
        v.tmp = NULL;
        v.tcap = 0;
        CHECK(csvx_view_serialize(&v, &ov, &got) == 0, "iter %d: serialize failed", it);
        CHECK(got.len == want.len && memcmp(got.p, want.p, got.len) == 0, "iter %d op %d: got [%.*s] want [%.*s] from [%.*s]",
            it, op, (int)got.len, got.p, (int)want.len, want.p, (int)in.len, in.p);
        free(v.tmp);
        csvx_free(&ix);
        csvx_table_free(&t);
        csvx_buf_free(&in);
        csvx_buf_free(&want);
        csvx_buf_free(&got);
    }
}

int main(void)
{
    simd_init();
    test_parse_random();
    test_parse_arena();
    test_serialize();
    test_shapes();
    test_irregular_at_split();
    test_threads_match_serial();
    test_numbers();
    test_dict_parallel();
    printf("test_csvx_core: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
