/* test_pgwire_core -- src/core/dyn-pgwire.c vectors (PostgreSQL protocol 3.0 backend messages): framing at every
 * split, RowDescription / DataRow iterators (truncation vs column-past-end), ErrorResponse fields, bytea hex,
 * int8 text bounds, uuid; then seeded properties over tests/lib/dyn-pgwire-model.h: pgw-stream (generated message
 * streams fed in random chunks match the record; E_LEN at the first frame over maxmsg), pgw-raw (mutated bytes match
 * the reference iterators), pgw-values (bytea hex, int8 text vs strtoll, uuid). */
#include "../lib/dyn-pgwire-model.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c, ...) FK_CHECK(c, __VA_ARGS__)

static void t_frame(void)
{
    static const uint8_t m[] = { 'Z', 0, 0, 0, 5, 'I', 'C', 0, 0, 0, 13, 'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0 };
    const uint8_t* body;
    size_t blen, cut;
    char type;
    long r;
    for (cut = 0; cut < 6; cut++)
        CHECK(dyn_pgw_frame(m, cut, 1024, &type, &body, &blen) == 0, "a %zu-byte prefix needs more", cut);
    r = dyn_pgw_frame(m, sizeof m, 1024, &type, &body, &blen);
    CHECK(r == 6 && type == 'Z' && blen == 1 && body[0] == 'I', "ReadyForQuery frame: r=%ld", r);
    r = dyn_pgw_frame(m + 6, sizeof m - 6, 1024, &type, &body, &blen);
    CHECK(r == 14 && type == 'C' && blen == 9 && !memcmp(body, "SELECT 1", 9), "CommandComplete frame: r=%ld", r);
    CHECK(dyn_pgw_frame(m + 6, 13, 1024, &type, &body, &blen) == 0, "one byte short needs more");
    CHECK(dyn_pgw_frame(m + 6, sizeof m - 6, 12, &type, &body, &blen) == DYN_PGW_E_LEN, "length 13 over maxmsg 12");
    CHECK(dyn_pgw_frame(m + 6, sizeof m - 6, 13, &type, &body, &blen) == 14, "length 13 at maxmsg 13");
    {
        static const uint8_t bad[] = { 'Z', 0, 0, 0, 3, 'I' };
        static const uint8_t four[] = { 'I', 0, 0, 0, 4 };
        CHECK(dyn_pgw_frame(bad, sizeof bad, 1024, &type, &body, &blen) == DYN_PGW_E_LEN, "length 3 is below the 4-byte minimum");
        CHECK(dyn_pgw_frame(four, sizeof four, 1024, &type, &body, &blen) == 5 && blen == 0, "length 4 is an empty body");
    }
}

static void t_rowdesc(void)
{
    uint8_t m[64];
    dyn_pgw_iter_t it;
    dyn_pgw_field_t f;
    size_t n = 0;
    m[n++] = 0;
    m[n++] = 2;
    memcpy(m + n, "id", 3);
    n += 3;
    memcpy(m + n, "\0\0\x40\x01\0\x01\0\0\0\x17\0\x04\xff\xff\xff\xff\0\x01", 18);
    n += 18;
    m[n++] = 0;
    memcpy(m + n, "\0\0\0\0\0\0\0\0\0\x19\xff\xff\xff\xff\xff\xff\0\0", 18);
    n += 18;
    CHECK(dyn_pgw_rowdesc_begin(&it, m, n) == 2, "two fields");
    CHECK(dyn_pgw_rowdesc_next(&it, &f) == 1 && f.nlen == 2 && !memcmp(f.name, "id", 2) && f.table_oid == 0x4001
            && f.column == 1 && f.type_oid == 23 && f.format == 1,
        "field 1: id, table 16385, column 1, int4, binary");
    CHECK(dyn_pgw_rowdesc_next(&it, &f) == 1 && f.nlen == 0 && f.type_oid == 25 && f.format == 0, "field 2: empty name, text");
    CHECK(dyn_pgw_rowdesc_next(&it, &f) == 0, "end after count");
    CHECK(dyn_pgw_rowdesc_begin(&it, m, n - 1) == 2 && dyn_pgw_rowdesc_next(&it, &f) == 1
            && dyn_pgw_rowdesc_next(&it, &f) == DYN_PGW_E_TRUNC,
        "a field one byte short is malformed");
    CHECK(dyn_pgw_rowdesc_begin(&it, m, 4) == 2 && dyn_pgw_rowdesc_next(&it, &f) == DYN_PGW_E_TRUNC, "an unterminated name is malformed");
    CHECK(dyn_pgw_rowdesc_begin(&it, m, 1) == DYN_PGW_E_TRUNC, "a 1-byte RowDescription is truncated");
}

static void t_datarow(void)
{
    static const uint8_t m[] = { 0, 3, 0, 0, 0, 2, 'h', 'i', 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0 };
    dyn_pgw_iter_t it;
    const uint8_t* c;
    uint32_t l;
    CHECK(dyn_pgw_datarow_begin(&it, m, sizeof m) == 3, "three columns");
    CHECK(dyn_pgw_datarow_next(&it, &c, &l) == 1 && l == 2 && !memcmp(c, "hi", 2), "column 1 'hi'");
    CHECK(dyn_pgw_datarow_next(&it, &c, &l) == 1 && l == DYN_PGW_NULL, "column 2 NULL");
    CHECK(dyn_pgw_datarow_next(&it, &c, &l) == 1 && l == 0, "column 3 empty");
    CHECK(dyn_pgw_datarow_next(&it, &c, &l) == 0, "end");
    CHECK(dyn_pgw_datarow_begin(&it, m, 7) == 3 && dyn_pgw_datarow_next(&it, &c, &l) == DYN_PGW_E_PAST, "a value past the end");
    CHECK(dyn_pgw_datarow_begin(&it, m, 5) == 3 && dyn_pgw_datarow_next(&it, &c, &l) == DYN_PGW_E_TRUNC, "a length cut short");
    CHECK(dyn_pgw_datarow_begin(&it, m, 12) == 3 && dyn_pgw_datarow_next(&it, &c, &l) == 1
            && dyn_pgw_datarow_next(&it, &c, &l) == 1 && dyn_pgw_datarow_next(&it, &c, &l) == DYN_PGW_E_TRUNC,
        "NULL needs no bytes; the third length is missing");
}

static void t_fields(void)
{
    static const uint8_t m[] = "SERROR\0C42P01\0Mrelation \"x\" does not exist\0Rparser\0\0";
    size_t at = 0, sl;
    const uint8_t* s;
    char code, got[8];
    int k = 0;
    while (dyn_pgw_field_next(m, sizeof m - 1, &at, &code, &s, &sl) && k < 7)
        got[k++] = code;
    CHECK(k == 4 && !memcmp(got, "SCMR", 4), "four fields S C M R, got %d", k);
    at = 0;
    dyn_pgw_field_next(m, 6, &at, &code, &s, &sl);
    CHECK(sl == 5 && at == 6 && !memcmp(s, "ERROR", 5), "an unterminated last field ends at the message end");
    CHECK(!dyn_pgw_field_next(m, 6, &at, &code, &s, &sl), "nothing after the end");
    at = 0;
    CHECK(dyn_pgw_cstr((const uint8_t*)"ab\0cd", 5, &at, &s) == 2 && at == 3, "cstr stops at NUL");
}

static void t_values(void)
{
    uint8_t out[8];
    char hex[40];
    int64_t v = 0;
    static const uint8_t u[16] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(dyn_pgw_bytea_hex((const uint8_t*)"\\x00fFa9", 8, out) == 3 && out[0] == 0 && out[1] == 0xff && out[2] == 0xa9, "bytea hex");
    CHECK(dyn_pgw_bytea_hex((const uint8_t*)"\\x", 2, out) == 0, "empty bytea");
    CHECK(dyn_pgw_bytea_hex((const uint8_t*)"\\x0", 3, out) < 0, "odd digits");
    CHECK(dyn_pgw_bytea_hex((const uint8_t*)"\\x0g", 4, out) < 0, "non-hex digit");
    CHECK(dyn_pgw_bytea_hex((const uint8_t*)"\\000", 4, out) < 0, "escape format is not hex");
    dyn_pgw_hex(u, 3, hex);
    CHECK(!memcmp(hex, "\\x123456", 8), "hex encode");
    dyn_pgw_uuid(u, hex);
    CHECK(!memcmp(hex, "12345678-9abc-def0-0102-030405060708", 36), "uuid");
    CHECK(dyn_pgw_int8_text((const uint8_t*)"9223372036854775807", 19, &v) && v == INT64_MAX, "int8 max");
    CHECK(dyn_pgw_int8_text((const uint8_t*)"-9223372036854775808", 20, &v) && v == INT64_MIN, "int8 min");
    CHECK(!dyn_pgw_int8_text((const uint8_t*)"9223372036854775808", 19, &v), "int8 max + 1");
    CHECK(!dyn_pgw_int8_text((const uint8_t*)"-9223372036854775809", 20, &v), "int8 min - 1");
    CHECK(dyn_pgw_int8_text((const uint8_t*)"-0", 2, &v) && v == 0, "-0");
    CHECK(!dyn_pgw_int8_text((const uint8_t*)"-", 1, &v), "lone minus");
    CHECK(!dyn_pgw_int8_text((const uint8_t*)"+1", 2, &v), "plus sign");
    CHECK(!dyn_pgw_int8_text((const uint8_t*)" 1", 2, &v), "leading space");
    CHECK(!dyn_pgw_int8_text((const uint8_t*)"1 ", 2, &v), "trailing space");
    CHECK(!dyn_pgw_int8_text((const uint8_t*)"", 0, &v), "empty");
    {
        char z[49];
        memset(z, '0', 48);
        z[47] = '1';
        CHECK(!dyn_pgw_int8_text((const uint8_t*)z, 48, &v), "48 chars is over the 47-char text cap");
        CHECK(dyn_pgw_int8_text((const uint8_t*)z + 1, 47, &v) && v == 1, "47 zero-padded chars");
    }
}

static pm_stream_t g_s;

static void prop_stream(int raw)
{
    long it, N = (long)fk_iters(raw ? 1500 : 800);
    fk_rng_t r = fk_stream(raw ? "pgw-raw" : "pgw-stream");
    pm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e;
        pm_gen(&s, &g_s);
        if (raw) {
            size_t n = g_s.len, k = 1 + fk_below(&r, 6);
            fk_mutate(&r, g_s.buf, n, fk_below(&r, 4));
            e = pm_check_raw(g_s.buf, n);
            if (!e && g_s.nmsg)
                e = pm_check_raw(g_s.buf + g_s.msg[0].off, n - g_s.msg[0].off);
            while (!e && k-- && n)
                e = pm_check_raw(g_s.buf + fk_below(&r, (uint32_t)n), fk_below(&r, 64));
        } else
            e = pm_check_stream(&g_s, &s);
        FK_CHECK(e == NULL, "%s (item %ld, %zu bytes, %zu messages)", e, it, g_s.len, g_s.nmsg);
    }
}

static void prop_values(void)
{
    long it, N = (long)fk_iters(8000);
    fk_rng_t r = fk_stream("pgw-values");
    pm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e = pm_check_values(&s);
        FK_CHECK(e == NULL, "%s (item %ld)", e, it);
    }
}

static void t_types(void)
{
    static const uint8_t n125[] = { 0, 2, 0, 0, 0, 0, 0, 2, 0, 1, 0x09, 0xC4 };
    static const uint8_t nz3[] = { 0, 0, 0, 0, 0, 0, 0, 3 };
    static const uint8_t nneg[] = { 0, 1, 0xFF, 0xFF, 0x40, 0, 0, 4, 0, 1 };
    static const uint8_t n1e4[] = { 0, 1, 0, 1, 0, 0, 0, 0, 0, 1 };
    static const uint8_t nbig[] = { 0, 3, 0, 1, 0, 0, 0, 1, 0x04, 0xD2, 0x16, 0x2E, 0x23, 0x28 };
    static const uint8_t nnan[] = { 0, 0, 0, 0, 0xC0, 0, 0, 0 };
    static const uint8_t nninf[] = { 0, 0, 0, 0, 0xF0, 0, 0, 0 };
    static const uint8_t bdig[] = { 0, 1, 0, 0, 0, 0, 0, 0, 0x27, 0x10 };
    static const uint8_t bscale[] = { 0, 0, 0, 0, 0, 0, 0x40, 0 };
    static const uint8_t bsign[] = { 0, 0, 0, 0, 0x12, 0x34, 0, 0 };
    static const uint8_t bnan[] = { 0, 1, 0, 0, 0xC0, 0, 0, 0, 0, 1 };
    char o[64];
    size_t need = 0;
    int64_t us = 0;
    int32_t d = 0, m = 0;
    int64_t bc1 = (dyn_pgw_days_from_civil(0, 1, 1) - DYN_PGW_EPOCH_DAYS) * 86400000000LL;
    CHECK(dyn_pgw_numeric_bin(n125, sizeof n125, o, sizeof o, &need) == 4 && !strcmp(o, "1.25"), "numeric 1.25: %s", o);
    CHECK(dyn_pgw_numeric_bin(n125, sizeof n125, o, 3, &need) == -2 && need >= 5, "numeric 1.25 into 3 bytes is -2, need %zu", need);
    CHECK(dyn_pgw_numeric_bin(nz3, sizeof nz3, o, sizeof o, NULL) == 5 && !strcmp(o, "0.000"), "numeric 0.000: %s", o);
    CHECK(dyn_pgw_numeric_bin(nneg, sizeof nneg, o, sizeof o, NULL) == 7 && !strcmp(o, "-0.0001"), "numeric -0.0001: %s", o);
    CHECK(dyn_pgw_numeric_bin(n1e4, sizeof n1e4, o, sizeof o, NULL) == 5 && !strcmp(o, "10000"), "numeric 10000: %s", o);
    CHECK(dyn_pgw_numeric_bin(nbig, sizeof nbig, o, sizeof o, NULL) == 10 && !strcmp(o, "12345678.9"), "numeric 12345678.9: %s", o);
    CHECK(dyn_pgw_numeric_bin(nnan, sizeof nnan, o, sizeof o, NULL) == 3 && !strcmp(o, "NaN"), "numeric NaN: %s", o);
    CHECK(dyn_pgw_numeric_bin(nninf, sizeof nninf, o, sizeof o, NULL) == 9 && !strcmp(o, "-Infinity"), "numeric -Infinity: %s", o);
    CHECK(dyn_pgw_numeric_bin(bdig, sizeof bdig, o, sizeof o, NULL) == -1, "numeric digit 10000 is malformed");
    CHECK(dyn_pgw_numeric_bin(bscale, sizeof bscale, o, sizeof o, NULL) == -1, "numeric dscale 0x4000 is malformed");
    CHECK(dyn_pgw_numeric_bin(bsign, sizeof bsign, o, sizeof o, NULL) == -1, "numeric sign 0x1234 is malformed");
    CHECK(dyn_pgw_numeric_bin(bnan, sizeof bnan, o, sizeof o, NULL) == -1, "numeric NaN with digits is malformed");
    CHECK(dyn_pgw_numeric_bin(n125, sizeof n125 - 1, o, sizeof o, NULL) == -1 && dyn_pgw_numeric_bin(n125, 7, o, sizeof o, NULL) == -1, "numeric length mismatch");
    CHECK(dyn_pgw_ts_text("2000-01-01", 10, 0, &us) == 0 && us == 0, "date 2000-01-01");
    CHECK(dyn_pgw_ts_text("1970-01-01 00:00:00", 19, 1, &us) == 0 && us == -946684800000000LL, "timestamp 1970: %lld", (long long)us);
    CHECK(dyn_pgw_ts_text("2000-01-01 05:30:00+05:30", 25, 2, &us) == 0 && us == 0, "timestamptz +05:30: %lld", (long long)us);
    CHECK(dyn_pgw_ts_text("2000-01-01 00:00:00.5-00:00:01", 30, 2, &us) == 0 && us == 1500000, "timestamptz seconds offset: %lld", (long long)us);
    CHECK(dyn_pgw_ts_text("0001-01-01 BC", 13, 0, &us) == 0 && us == bc1, "date 0001-01-01 BC");
    CHECK(dyn_pgw_ts_text("infinity", 8, 1, &us) == 1 && dyn_pgw_ts_text("-infinity", 9, 2, &us) == 2, "infinities");
    CHECK(dyn_pgw_ts_text("1900-02-29", 10, 0, &us) == -1 && dyn_pgw_ts_text("2000-02-29", 10, 0, &us) == 0, "leap days");
    CHECK(dyn_pgw_ts_text("2000-01-01 24:00:00", 19, 1, &us) == -1, "hour 24");
    CHECK(dyn_pgw_ts_text("2000-01-01 00:00:00", 19, 2, &us) == -1, "timestamptz without an offset");
    CHECK(dyn_pgw_ts_text("2000-1-01", 9, 0, &us) == -1 && dyn_pgw_ts_text("2000-01-01 ", 11, 0, &us) == -1, "short month, trailing space");
    CHECK(dyn_pgw_ts_text("2000-01-01\0", 11, 0, &us) == -1, "NUL inside the length");
    CHECK(dyn_pgw_ts_text("294277-12-31 23:59:59", 21, 1, &us) == -1, "past the int64 microsecond range");
    CHECK(dyn_pgw_ts_format(0, 2, o, sizeof o) == 22 && !strcmp(o, "2000-01-01 00:00:00+00"), "format tz: %s", o);
    CHECK(dyn_pgw_ts_format(1, 1, o, sizeof o) > 0 && !strcmp(o, "2000-01-01 00:00:00.000001"), "format 1us: %s", o);
    CHECK(dyn_pgw_ts_format(-1, 1, o, sizeof o) > 0 && !strcmp(o, "1999-12-31 23:59:59.999999"), "format -1us: %s", o);
    CHECK(dyn_pgw_ts_format(bc1, 0, o, sizeof o) > 0 && !strcmp(o, "0001-01-01 BC"), "format BC: %s", o);
    CHECK(dyn_pgw_ts_format(0, 2, o, 5) == -1 && dyn_pgw_ts_format(0, 2, o, 22) == -1, "format into a short buffer fails");
    CHECK(dyn_pgw_interval_text("1 year 2 mons 3 days 04:05:06.5", 31, &us, &d, &m) == 0 && m == 14 && d == 3 && us == 14706500000LL, "interval mixed");
    CHECK(dyn_pgw_interval_text("-1 days +00:00:01", 17, &us, &d, &m) == 0 && m == 0 && d == -1 && us == 1000000, "interval signs");
    CHECK(dyn_pgw_interval_text("-00:00:00.000001", 16, &us, &d, &m) == 0 && us == -1, "interval -1us");
    CHECK(dyn_pgw_interval_text("-178956970 years -8 mons", 24, &us, &d, &m) == 0 && m == INT32_MIN, "interval INT32_MIN months");
    CHECK(dyn_pgw_interval_text("178956970 years 8 mons", 22, &us, &d, &m) == -1, "interval months past INT32_MAX");
    CHECK(dyn_pgw_interval_text("2562047788:00:54.775807", 23, &us, &d, &m) == 0 && us == INT64_MAX, "interval INT64_MAX us");
    CHECK(dyn_pgw_interval_text("2562047789:00:00", 16, &us, &d, &m) == -1, "interval hours past int64");
    CHECK(dyn_pgw_interval_text("", 0, &us, &d, &m) == -1 && dyn_pgw_interval_text("1 fortnight", 11, &us, &d, &m) == -1
            && dyn_pgw_interval_text("1:00:00", 7, &us, &d, &m) == -1 && dyn_pgw_interval_text("@ 1 hour", 8, &us, &d, &m) == -1,
        "interval rejects: empty, unknown unit, 1-digit hour, non-postgres style");
}

static void prop_types(void)
{
    long it, N = (long)fk_iters(20000);
    fk_rng_t r = fk_stream("pgw-types");
    pm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e = pm_check_numgen(&s);
        if (!e)
            e = pm_check_tsgen(&s);
        if (!e)
            e = pm_check_ivgen(&s);
        FK_CHECK(e == NULL, "%s (item %ld)", e, it);
        if (e)
            break;
    }
}

static int arr_text(const char* s, int* nd, int32_t* dims, dyn_pgw_span_t* sp, char* out, size_t* nel)
{
    return dyn_pgw_array_text(s, strlen(s), nd, dims, sp, 64, out, nel);
}

static void t_arrays(void)
{
    dyn_pgw_span_t sp[64];
    char out[256];
    int32_t dims[DYN_PGW_MAXDIM];
    int nd = 0;
    size_t nel = 0;
    static const uint8_t b1[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 23, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 7, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t b0[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 23 };
    static const uint8_t b7[] = { 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 23 };
    dyn_pgw_arrbin_t h;
    size_t at = 0;
    const uint8_t* e;
    int32_t len;
    CHECK(arr_text("{}", &nd, dims, sp, out, &nel) == 0 && nd == 0 && nel == 0, "empty array");
    CHECK(arr_text("{1,2,3}", &nd, dims, sp, out, &nel) == 0 && nd == 1 && dims[0] == 3 && nel == 3 && sp[2].len == 1 && out[sp[2].off] == '3', "flat");
    CHECK(arr_text("{{1,2},{3,4},{5,6}}", &nd, dims, sp, out, &nel) == 0 && nd == 2 && dims[0] == 3 && dims[1] == 2 && nel == 6, "2-D");
    CHECK(arr_text("{NULL,\"NULL\",null,\"\",\"a\\\"b\\\\c\"}", &nd, dims, sp, out, &nel) == 0 && nel == 5 && sp[0].len == -1 && sp[1].len == 4 && sp[2].len == -1
            && sp[3].len == 0 && sp[4].len == 5 && !memcmp(out + sp[4].off, "a\"b\\c", 5), "NULL vs \"NULL\", empty, escapes");
    CHECK(arr_text("{ a b , \\NULL ,x\\ }", &nd, dims, sp, out, &nel) == 0 && nel == 3 && sp[0].len == 3 && !memcmp(out + sp[0].off, "a b", 3) && sp[1].len == 4 && sp[2].len == 2, "whitespace trim, escaped NULL is text, escaped trailing space kept");
    CHECK(arr_text("[0:2]={7,8,9}", &nd, dims, sp, out, &nel) == 0 && nel == 3, "bounds decoration");
    CHECK(arr_text("[-1:0][1:1]={{1},{2}}", &nd, dims, sp, out, &nel) == 0 && nd == 2, "2-D bounds with a negative lower bound");
    CHECK(arr_text("[0:3]={7,8,9}", &nd, dims, sp, out, &nel) == -1, "bounds that disagree with the shape");
    CHECK(arr_text("{{1,2},{3}}", &nd, dims, sp, out, &nel) == -1 && arr_text("{{1},2}", &nd, dims, sp, out, &nel) == -1 && arr_text("{1,{2}}", &nd, dims, sp, out, &nel) == -1
            && arr_text("{{1,2},{3},{4,5,6}}", &nd, dims, sp, out, &nel) == -1, "ragged and mixed");
    CHECK(arr_text("{{{{{{{1}}}}}}}", &nd, dims, sp, out, &nel) == -1 && arr_text("{{{{{{1}}}}}}", &nd, dims, sp, out, &nel) == 0 && nd == 6, "7 dims rejected, 6 accepted");
    CHECK(arr_text("{1,}", &nd, dims, sp, out, &nel) == -1 && arr_text("{,1}", &nd, dims, sp, out, &nel) == -1 && arr_text("{1}x", &nd, dims, sp, out, &nel) == -1
            && arr_text("{\"a}", &nd, dims, sp, out, &nel) == -1 && arr_text("{a\"b}", &nd, dims, sp, out, &nel) == -1 && arr_text("{{}}", &nd, dims, sp, out, &nel) == -1
            && arr_text("1,2", &nd, dims, sp, out, &nel) == -1 && arr_text("{1", &nd, dims, sp, out, &nel) == -1,
        "malformed literals");
    CHECK(dyn_pgw_array_text("{1,2,3}", 7, &nd, dims, sp, 2, out, &nel) == -2 && nel == 3, "two spans for three elements is -2 with nel 3");
    CHECK(dyn_pgw_array_bin(b1, sizeof b1, &h) == 0 && h.ndim == 1 && h.nel == 2 && h.elem == 23 && h.dims[0] == 2 && h.lb[0] == 1, "binary int4[2]");
    CHECK(dyn_pgw_array_bin_next(&h, &at, &e, &len) && len == 4 && e[3] == 7 && dyn_pgw_array_bin_next(&h, &at, &e, &len) && len == -1, "binary elements: 7 then NULL");
    CHECK(dyn_pgw_array_bin(b0, sizeof b0, &h) == 0 && h.nel == 0, "binary empty");
    {
        uint8_t bx[sizeof b1 + 1];
        memcpy(bx, b1, sizeof b1);
        bx[sizeof b1] = 0;
        CHECK(dyn_pgw_array_bin(bx, sizeof bx, &h) == -1, "binary: a trailing byte after the last element");
    }
    CHECK(dyn_pgw_array_bin(b7, sizeof b7, &h) == -1 && dyn_pgw_array_bin(b1, sizeof b1 - 1, &h) == -1 && dyn_pgw_array_bin(b1, 11, &h) == -1, "binary: 7 dims, truncated, short header");
    CHECK(dyn_pgw_array_elem(1007) == 23 && dyn_pgw_array_elem(3807) == 3802 && dyn_pgw_array_elem(23) == 0, "array element OIDs");
}

static void prop_arrays(void)
{
    long it, N = (long)fk_iters(20000);
    fk_rng_t r = fk_stream("pgw-arrays");
    pm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e = pm_check_arrtext_gen(&s);
        if (!e)
            e = pm_check_arrbin_gen(&s);
        FK_CHECK(e == NULL, "%s (item %ld)", e, it);
        if (e)
            break;
    }
}

static int rng(const char* s, dyn_pgw_range_t* r, char* out)
{
    return dyn_pgw_range_text(s, strlen(s), r, out);
}

static void t_ranges(void)
{
    dyn_pgw_range_t r;
    char out[256];
    size_t at = 0;
    int k = 0, c = 0;
    static const uint8_t b1[] = { 0x02, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 5 };
    static const uint8_t binf[] = { 0x18 };
    static const uint8_t bemp[] = { 0x01 };
    static const uint8_t bemp2[] = { 0x01, 0, 0, 0, 0 };
    static const uint8_t bbad[] = { 0x0A };
    static const uint8_t bflag[] = { 0x38 };
    static const uint8_t mr[] = { 0, 0, 0, 2, 0, 0, 0, 1, 0x18, 0, 0, 0, 1, 0x01 };
    CHECK(rng("empty", &r, out) == 0 && r.flags == DYN_PGW_RANGE_EMPTY && rng("  EmPtY  ", &r, out) == 0, "empty in any case");
    CHECK(rng("[1,5)", &r, out) == 0 && r.flags == DYN_PGW_RANGE_LB_INC && r.lo_len == 1 && r.lo[0] == '1' && r.hi_len == 1 && r.hi[0] == '5', "[1,5)");
    CHECK(rng("(,5]", &r, out) == 0 && r.flags == (DYN_PGW_RANGE_LB_INF | DYN_PGW_RANGE_UB_INC) && r.lo_len == -1, "(,5]");
    CHECK(rng("[1,)", &r, out) == 0 && r.flags == (DYN_PGW_RANGE_LB_INC | DYN_PGW_RANGE_UB_INF) && rng("(,)", &r, out) == 0 && r.flags == (DYN_PGW_RANGE_LB_INF | DYN_PGW_RANGE_UB_INF), "[1,) and (,)");
    CHECK(rng("[\"a\"\"b\\\\c\",\"\"]", &r, out) == 0 && r.lo_len == 5 && !memcmp(r.lo, "a\"b\\c", 5) && r.hi_len == 0, "quoted bounds: \"\" is a quote, \\\\ a backslash, \"\" an empty bound");
    CHECK(rng("[\"2024-01-02 03:04:05+00\",\"2024-01-03 00:00:00+00\")", &r, out) == 0 && r.lo_len == 22, "timestamptz bounds with spaces");
    CHECK(rng("[1,5", &r, out) == -1 && rng("1,5)", &r, out) == -1 && rng("[15)", &r, out) == -1 && rng("[1,5)x", &r, out) == -1 && rng("[\"1,5)", &r, out) == -1 && rng("emptyx", &r, out) == -1, "malformed ranges");
    CHECK(dyn_pgw_range_bin(b1, sizeof b1, &r) == 0 && r.lo_len == 4 && r.lo[3] == 1 && r.hi_len == 4 && r.hi[3] == 5, "binary [1,5)");
    CHECK(dyn_pgw_range_bin(binf, 1, &r) == 0 && r.lo_len == -1 && r.hi_len == -1 && dyn_pgw_range_bin(bemp, 1, &r) == 0, "binary (,) and empty");
    CHECK(dyn_pgw_range_bin(bemp2, sizeof bemp2, &r) == -1 && dyn_pgw_range_bin(bbad, 1, &r) == -1 && dyn_pgw_range_bin(bflag, 1, &r) == -1
            && dyn_pgw_range_bin(b1, sizeof b1 - 1, &r) == -1 && dyn_pgw_range_bin(b1, 0, &r) == -1, "binary: EMPTY with data, infinite and inclusive, null-bound flag, truncated, zero length");
    while ((k = dyn_pgw_multirange_text("{[1,2), (3,4] }", 15, &at, &r, out)) > 0)
        c++;
    CHECK(k == 0 && c == 2, "multirange text {[1,2), (3,4] }: %d ranges, end %d", c, k);
    at = 0;
    c = 0;
    while ((k = dyn_pgw_multirange_text("{}", 2, &at, &r, out)) > 0)
        c++;
    CHECK(k == 0 && c == 0, "multirange text {}");
    at = 0;
    CHECK(dyn_pgw_multirange_text("{[1,2) [3,4)}", 13, &at, &r, out) == 1 && dyn_pgw_multirange_text("{[1,2) [3,4)}", 13, &at, &r, out) == -1, "multirange text without a comma");
    at = 0;
    CHECK(dyn_pgw_multirange_text("{[1,2)}x", 8, &at, &r, out) == 1 && dyn_pgw_multirange_text("{[1,2)}x", 8, &at, &r, out) == -1, "multirange text with trailing junk");
    at = 0;
    c = 0;
    while ((k = dyn_pgw_multirange_bin(mr, sizeof mr, &at, &r)) > 0)
        c++;
    CHECK(k == 0 && c == 2, "multirange binary of two ranges");
    at = 0;
    CHECK(dyn_pgw_multirange_bin(mr, sizeof mr - 1, &at, &r) == -1, "multirange binary truncated");
    CHECK(dyn_pgw_range_sub(3910) == 1184 && dyn_pgw_multirange_range(4451) == 3904 && dyn_pgw_range_sub(23) == 0 && dyn_pgw_array_elem(3905) == 3904, "range OIDs");
    {
        pm_src_t s2 = { (const uint8_t*)"\035\277", 2, NULL };
        CHECK(pm_check_rng_gen(&s2) == NULL, "an exhausted input ends the multirange generator (fuzz timeout reproducer)");
    }
}

static void prop_ranges(void)
{
    long it, N = (long)fk_iters(20000);
    fk_rng_t r = fk_stream("pgw-ranges");
    pm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e = pm_check_rng_gen(&s);
        FK_CHECK(e == NULL, "%s (item %ld)", e, it);
        if (e)
            break;
    }
}

static void t_uri(void)
{
    static const char* const v[][2] = {
        { "postgresql://", "" },
        { "postgresql://localhost", "H=localhost\n" },
        { "postgresql://localhost:5433", "H=localhost\nO=5433\n" },
        { "postgresql://localhost/mydb", "H=localhost\nD=mydb\n" },
        { "postgresql://user@localhost", "U=user\nH=localhost\n" },
        { "postgresql://user:secret@localhost", "U=user\nP=secret\nH=localhost\n" },
        { "postgresql://other@localhost/otherdb?connect_timeout=10&application_name=myapp", "U=other\nH=localhost\nD=otherdb\nQconnect_timeout=10\nQapplication_name=myapp\n" },
        { "postgresql://host1:123,host2:456/somedb?target_session_attrs=any&application_name=myapp", "H=host1,host2\nO=123,456\nD=somedb\nQtarget_session_attrs=any\nQapplication_name=myapp\n" },
        { "postgresql://[2001:db8::1234]/database", "H=2001:db8::1234\nD=database\n" },
        { "postgresql://[::1]:5433", "H=::1\nO=5433\n" },
        { "postgresql:///mydb?host=localhost&port=5433", "D=mydb\nQhost=localhost\nQport=5433\n" },
        { "postgresql://%2Fvar%2Flib%2Fpostgresql/dbname", "H=/var/lib/postgresql\nD=dbname\n" },
        { "postgres://:12345/", "O=12345\n" },
        { "postgres://@host", "H=host\n" },
        { "postgres://host:/", "H=host\n" },
        { "postgres://h1,h2:5/d", "H=h1,h2\nO=,5\nD=d\n" },
        { "postgres://%zz@h", "Ex\n" },
        { "postgres://[::1", "Eb\n" },
        { "postgres://[]", "Ee\n" },
        { "postgres://[::1]x", "Ea\n" },
        { "postgres://h/d%00", "H=h\nEz\n" },
        { "postgres://h/ d ", "H=h\nD=d\n" },
        { "postgres://h/d x", "H=h\nEw\n" },
        { "postgres://h?a=1=2", "H=h\nEq\n" },
        { "postgres://h?a", "H=h\nEm\n" },
        { "postgres://h?a=1&", "H=h\nQa=1\n" },
        { "postgres://h?&a=1", "H=h\nEm\n" },
        { "postgre://h", "Es\n" },
        { "postgres://u:p:q@h", "U=u\nP=p:q\nH=h\n" },
        { "postgres://h?u@x", "U=h?u\nH=x\n" },
        { "postgres://u%40x:p%2fw@h/d%20b?application_name=a%26p", "U=u@x\nP=p/w\nH=h\nD=d b\nQapplication_name=a&p\n" },
    };
    static char o[PM_URI_OUT], m[PM_URI_OUT];
    char d[16];
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        pm_uri_core(v[i][0], strlen(v[i][0]), o);
        pm_uri_model(v[i][0], m);
        CHECK(!strcmp(o, v[i][1]), "uri %s: core gave [%s]", v[i][0], o);
        CHECK(!strcmp(m, v[i][1]), "uri %s: model gave [%s]", v[i][0], m);
    }
    CHECK(dyn_pgw_uri_decode("%4", 2, d) == DYN_PGW_DEC_HEX && dyn_pgw_uri_decode("%", 1, d) == DYN_PGW_DEC_HEX && dyn_pgw_uri_decode("%g1", 3, d) == DYN_PGW_DEC_HEX,
        "decode: short or bad escapes");
    CHECK(dyn_pgw_uri_decode("a%00", 4, d) == DYN_PGW_DEC_NUL && dyn_pgw_uri_decode("a b", 3, d) == DYN_PGW_DEC_SPACE && dyn_pgw_uri_decode("  ab  ", 6, d) == 2,
        "decode: %%00, inner space, outer spaces");
    CHECK(pm_check_uri_raw((const uint8_t*)"postgres://u@h\0x", 16) == NULL, "a NUL ends the uri for core and model");
}

static void t_pgpass(void)
{
    static const struct {
        const char* f;
        size_t n;
        const char *h, *p, *d, *u, *want;
    } v[] = {
        { "# c\nlocalhost:5432:db:u:p1\n", 0, "localhost", "5432", "db", "u", "p1" },
        { "*:*:*:*:any\n", 0, "h", "1", "d", "u", "any" },
        { "h\\:x:1:d:u:esc\\:aped\\\\\n", 0, "h:x", "1", "d", "u", "esc:aped\\" },
        { "h:1:d:u:pw:extra\n", 0, "h", "1", "d", "u", "pw" },
        { "h:1:d:u:p\r\n", 0, "h", "1", "d", "u", "p" },
        { "#h:1:d:u:p\n", 0, "h", "1", "d", "u", NULL },
        { "h:1:d:u\n", 0, "h", "1", "d", "u", NULL },
        { "h:1:d:u:\n", 0, "h", "1", "d", "u", "" },
        { "*x:1:d:u:p\n", 0, "*x", "1", "d", "u", "p" },
        { "h:1:d:u:a\\", 0, "h", "1", "d", "u", "a\\" },
        { "h:1:d:u:first\nh:1:d:u:second\n", 0, "h", "1", "d", "u", "first" },
        { "h:1:d:u:p\0x\n", 12, "h", "1", "d", "u", "p" },
        { "h:2:d:u:no\nh:1:d:u:yes", 0, "h", "1", "d", "u", "yes" },
        { "H:1:d:u:case\n", 0, "h", "1", "d", "u", NULL },
        { "\n\n\r\nh:1:d:u:after-blank\n", 0, "h", "1", "d", "u", "after-blank" },
        { "h:1:*:u:dbwild\n", 0, "h", "1", "d", "u", "dbwild" },
        { "h:1:d:*x:p\n", 0, "h", "1", "d", "u", NULL },
    };
    char out[256], mo[256];
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        size_t n = v[i].n ? v[i].n : strlen(v[i].f);
        long r = dyn_pgw_pgpass_find(v[i].f, n, v[i].h, v[i].p, v[i].d, v[i].u, out);
        long m = pm_pgpass_model(v[i].f, n, v[i].h, v[i].p, v[i].d, v[i].u, mo);
        int ok = v[i].want ? r == (long)strlen(v[i].want) && !memcmp(out, v[i].want, (size_t)r) : r == -1;
        CHECK(ok, "pgpass vector %zu: got %ld", i, r);
        CHECK(m == r && (r <= 0 || !memcmp(out, mo, (size_t)r)), "pgpass vector %zu: model gave %ld", i, m);
    }
    CHECK(pm_check_pgpass_raw((const uint8_t*)"ab*:a\\:b:*:a:x\n", 15) == NULL, "pgpass raw check");
}

static void prop_uri(void)
{
    long it, N = (long)fk_iters(20000);
    fk_rng_t r = fk_stream("pgw-uri");
    pm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e = pm_check_uri_gen(&s);
        FK_CHECK(e == NULL, "%s (item %ld)", e, it);
        if (e)
            break;
    }
}

static void prop_pgpass(void)
{
    long it, N = (long)fk_iters(20000);
    fk_rng_t r = fk_stream("pgw-pgpass");
    pm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e = pm_check_pgpass_gen(&s);
        FK_CHECK(e == NULL, "%s (item %ld)", e, it);
        if (e)
            break;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    fk_init("test_pgwire_core");
    t_frame();
    t_rowdesc();
    t_datarow();
    t_fields();
    t_values();
    t_types();
    t_arrays();
    t_ranges();
    t_uri();
    t_pgpass();
    if (fk_only("pgw-stream"))
        prop_stream(0);
    if (fk_only("pgw-raw"))
        prop_stream(1);
    if (fk_only("pgw-values"))
        prop_values();
    if (fk_only("pgw-types"))
        prop_types();
    if (fk_only("pgw-arrays"))
        prop_arrays();
    if (fk_only("pgw-ranges"))
        prop_ranges();
    if (fk_only("pgw-uri"))
        prop_uri();
    if (fk_only("pgw-pgpass"))
        prop_pgpass();
    return fk_done("test_pgwire_core");
}
