/* dyn-pgwire-model.h -- PostgreSQL protocol 3.0 backend-message model shared by tests/postgres/test_pgwire_core.c
 * (seeded rng) and src/fuzz/fuzz_pgwire.c (fuzz bytes). pm_gen writes a stream of T/D/E/N/C/Z/K/S/A/R messages and
 * records every field, column and error field; pm_check_stream reassembles it from random chunks the way
 * postgres-protocol.c does and checks each message against the record (and E_LEN at the first frame over maxmsg);
 * pm_check_raw compares the iterators with independent references on arbitrary bytes; pm_check_values round-trips
 * bytea hex, int8 text (against strtoll) and uuid. Each returns NULL or a message naming the broken rule. */
#ifndef DYN_PGWIRE_MODEL_H
#define DYN_PGWIRE_MODEL_H

#include "dyn-pgwire.h"
#include "dyn-fuzzkit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PM_CAP 16384
#define PM_MSGS 64
#define PM_ITEMS 1024

typedef struct {
    const uint8_t* p;
    size_t n;
    fk_rng_t* r;
} pm_src_t;

typedef struct {
    char type;
    size_t off, len, first, count;
} pm_msg_rec_t;

typedef struct {
    size_t off, len;
    uint32_t a, b;
    uint16_t c, d;
    char code;
} pm_item_t;

typedef struct {
    uint8_t buf[PM_CAP];
    size_t len, nmsg, nit;
    int full;
    pm_msg_rec_t msg[PM_MSGS];
    pm_item_t it[PM_ITEMS], junk;
} pm_stream_t;

static uint32_t pm_pick(pm_src_t* s, uint32_t k)
{
    uint32_t v;
    if (s->r)
        return fk_below(s->r, k);
    if (!s->n || !k)
        return 0;
    v = *s->p++;
    s->n--;
    return v % k;
}

static void pm_put(pm_stream_t* m, const void* p, size_t n)
{
    if (m->len + n > PM_CAP) {
        m->full = 1;
        return;
    }
    memcpy(m->buf + m->len, p, n);
    m->len += n;
}

static void pm_be(pm_stream_t* m, uint32_t v, int bytes)
{
    uint8_t b[4];
    int i;
    for (i = 0; i < bytes; i++)
        b[i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
    pm_put(m, b, (size_t)bytes);
}

static pm_item_t* pm_item(pm_stream_t* m)
{
    if (m->nit >= PM_ITEMS) {
        m->full = 1;
        return &m->junk;
    }
    return &m->it[m->nit++];
}

static size_t pm_text(pm_src_t* s, pm_stream_t* m, int nul_ok)
{
    static const char al[] = "abcxyz_ 019SELECT\"'\\";
    size_t n = pm_pick(s, 8) ? pm_pick(s, 12) : pm_pick(s, 300), i;
    for (i = 0; i < n; i++) {
        uint8_t c = (uint8_t)(pm_pick(s, 16) ? al[pm_pick(s, sizeof al - 1)] : pm_pick(s, 256));
        if (!c && !nul_ok)
            c = 'z';
        pm_put(m, &c, 1);
    }
    return n;
}

static void pm_gen(pm_src_t* s, pm_stream_t* m)
{
    static const char types[] = "TDDDENCZKSAR";
    size_t k, nm = 1 + pm_pick(s, 12);
    m->len = m->nmsg = m->nit = 0;
    m->full = 0;
    for (k = 0; k < nm && !m->full; k++) {
        pm_msg_rec_t* r = &m->msg[m->nmsg++];
        size_t at, i, cnt;
        char t = types[pm_pick(s, sizeof types - 1)];
        uint8_t z = 0;
        r->type = t;
        pm_put(m, &t, 1);
        at = m->len;
        pm_be(m, 0, 4);
        r->off = m->len;
        r->first = m->nit;
        r->count = 0;
        if (t == 'T' || t == 'D') {
            cnt = pm_pick(s, 8) ? pm_pick(s, 6) : pm_pick(s, 40);
            pm_be(m, (uint32_t)cnt, 2);
            for (i = 0; i < cnt; i++) {
                pm_item_t* e = pm_item(m);
                if (t == 'T') {
                    e->off = m->len;
                    e->len = pm_text(s, m, 0);
                    pm_put(m, &z, 1);
                    e->a = pm_pick(s, 4) ? 0 : pm_pick(s, 1u << 31) * 2u + pm_pick(s, 2);
                    e->c = (uint16_t)pm_pick(s, 65536);
                    e->b = pm_pick(s, 2) ? 23 + pm_pick(s, 3000) : pm_pick(s, 1u << 31) * 2u;
                    e->d = (uint16_t)(pm_pick(s, 3) ? pm_pick(s, 2) : pm_pick(s, 65536));
                    pm_be(m, e->a, 4);
                    pm_be(m, e->c, 2);
                    pm_be(m, e->b, 4);
                    pm_be(m, pm_pick(s, 65536), 2);
                    pm_be(m, pm_pick(s, 1u << 31) * 2u + 1u, 4);
                    pm_be(m, e->d, 2);
                } else if (pm_pick(s, 6) == 0) {
                    e->len = DYN_PGW_NULL;
                    pm_be(m, DYN_PGW_NULL, 4);
                } else {
                    size_t lp = m->len;
                    pm_be(m, 0, 4);
                    e->off = m->len;
                    e->len = pm_text(s, m, 1);
                    if (!m->full) {
                        m->buf[lp] = (uint8_t)(e->len >> 24);
                        m->buf[lp + 1] = (uint8_t)(e->len >> 16);
                        m->buf[lp + 2] = (uint8_t)(e->len >> 8);
                        m->buf[lp + 3] = (uint8_t)e->len;
                    }
                }
                r->count++;
            }
        } else if (t == 'E' || t == 'N') {
            static const char codes[] = "SVCMDHPWstcdnFLR";
            cnt = pm_pick(s, 8);
            for (i = 0; i < cnt; i++) {
                pm_item_t* e = pm_item(m);
                e->code = codes[pm_pick(s, sizeof codes - 1)];
                pm_put(m, &e->code, 1);
                e->off = m->len;
                e->len = pm_text(s, m, 0);
                pm_put(m, &z, 1);
                r->count++;
            }
            pm_put(m, &z, 1);
        } else
            pm_text(s, m, 1);
        r->len = m->len - r->off;
        if (!m->full) {
            uint32_t L = (uint32_t)(r->len + 4);
            m->buf[at] = (uint8_t)(L >> 24);
            m->buf[at + 1] = (uint8_t)(L >> 16);
            m->buf[at + 2] = (uint8_t)(L >> 8);
            m->buf[at + 3] = (uint8_t)L;
        }
    }
}

static const char* pm_check_msg(const pm_stream_t* m, const pm_msg_rec_t* r, const uint8_t* body, size_t blen)
{
    dyn_pgw_iter_t it;
    size_t i, at = 0, sl;
    if (blen != r->len || memcmp(body, m->buf + r->off, blen))
        return "frame body differs from the message written";
    if (r->type == 'T') {
        dyn_pgw_field_t f;
        if (dyn_pgw_rowdesc_begin(&it, body, blen) != (int)r->count)
            return "RowDescription field count";
        for (i = 0; i < r->count; i++) {
            const pm_item_t* e = &m->it[r->first + i];
            if (dyn_pgw_rowdesc_next(&it, &f) != 1 || f.nlen != e->len || f.name != body + (e->off - r->off)
                || f.table_oid != e->a || f.column != e->c || f.type_oid != e->b || f.format != e->d)
                return "RowDescription field differs (name, table oid, column, type oid, format at offset 16)";
        }
        if (dyn_pgw_rowdesc_next(&it, &f) != 0)
            return "RowDescription iterator ran past its count";
    } else if (r->type == 'D') {
        const uint8_t* c;
        uint32_t l;
        if (dyn_pgw_datarow_begin(&it, body, blen) != (int)r->count)
            return "DataRow column count";
        for (i = 0; i < r->count; i++) {
            const pm_item_t* e = &m->it[r->first + i];
            if (dyn_pgw_datarow_next(&it, &c, &l) != 1 || l != (uint32_t)e->len
                || (l != DYN_PGW_NULL && c != body + (e->off - r->off)))
                return "DataRow column differs (length, NULL as -1, value bytes)";
        }
        if (dyn_pgw_datarow_next(&it, &c, &l) != 0)
            return "DataRow iterator ran past its count";
    } else if (r->type == 'E' || r->type == 'N') {
        const uint8_t* v;
        char code;
        for (i = 0; i < r->count; i++) {
            const pm_item_t* e = &m->it[r->first + i];
            if (!dyn_pgw_field_next(body, blen, &at, &code, &v, &sl) || code != e->code || sl != e->len
                || v != body + (e->off - r->off))
                return "ErrorResponse field differs (code, NUL-terminated value)";
        }
        if (dyn_pgw_field_next(body, blen, &at, &code, &v, &sl))
            return "ErrorResponse iterator read past the terminating NUL";
    }
    return NULL;
}

static const char* pm_check_stream(const pm_stream_t* m, pm_src_t* s)
{
    static uint8_t rb[PM_CAP];
    size_t rl = 0, fed = 0, got = 0, maxmsg = PM_CAP, i, over = (size_t)-1;
    const char* e;
    if (m->full)
        return NULL;
    if (pm_pick(s, 4) == 0 && m->nmsg) {
        maxmsg = m->msg[pm_pick(s, (uint32_t)m->nmsg)].len + 4 - pm_pick(s, 2);
        for (i = 0; i < m->nmsg; i++)
            if (m->msg[i].len + 4 > maxmsg) {
                over = i;
                break;
            }
    }
    while (fed < m->len) {
        size_t c = 1 + pm_pick(s, pm_pick(s, 3) ? 8 : 512);
        if (c > m->len - fed)
            c = m->len - fed;
        memcpy(rb + rl, m->buf + fed, c);
        rl += c;
        fed += c;
        for (;;) {
            const uint8_t* body;
            size_t blen;
            char type;
            long u = dyn_pgw_frame(rb, rl, maxmsg, &type, &body, &blen);
            if (u == 0)
                break;
            if (u < 0)
                return got == over && rl >= 5 ? NULL : "E_LEN at the wrong frame";
            if (got >= m->nmsg || got == over)
                return "a frame over maxmsg was delivered";
            if (type != m->msg[got].type || (size_t)u != m->msg[got].len + 5)
                return "frame type or length differs from the message written";
            if ((e = pm_check_msg(m, &m->msg[got], body, blen)))
                return e;
            got++;
            memmove(rb, rb + u, rl - (size_t)u);
            rl -= (size_t)u;
        }
    }
    if (rl || got != m->nmsg)
        return "the stream did not split into exactly the messages written";
    return over == (size_t)-1 ? NULL : "a frame over maxmsg was not refused";
}

static int pm_ref_rowdesc(const uint8_t* b, size_t n, uint16_t* cnt, size_t* nok)
{
    size_t at = 2, k;
    *nok = 0;
    if (n < 2)
        return -1;
    *cnt = (uint16_t)(b[0] << 8 | b[1]);
    for (k = 0; k < *cnt; k++) {
        const uint8_t* z = at < n ? memchr(b + at, 0, n - at) : NULL;
        if (!z || (size_t)(z - b) + 19 > n)
            return 1;
        at = (size_t)(z - b) + 19;
        (*nok)++;
    }
    return 0;
}

static int pm_ref_datarow(const uint8_t* b, size_t n, uint16_t* cnt, size_t* nok)
{
    size_t at = 2, k;
    *nok = 0;
    if (n < 2)
        return -1;
    *cnt = (uint16_t)(b[0] << 8 | b[1]);
    for (k = 0; k < *cnt; k++) {
        uint64_t l;
        if (at + 4 > n)
            return DYN_PGW_E_TRUNC;
        l = (uint64_t)b[at] << 24 | (uint64_t)b[at + 1] << 16 | (uint64_t)b[at + 2] << 8 | b[at + 3];
        at += 4;
        if (l != 0xffffffffu) {
            if (at + l > n)
                return DYN_PGW_E_PAST;
            at += (size_t)l;
        }
        (*nok)++;
    }
    return 0;
}

static const char* pm_check_raw(const uint8_t* b, size_t n)
{
    dyn_pgw_iter_t it;
    dyn_pgw_field_t f;
    const uint8_t* c;
    uint32_t l;
    uint16_t cnt = 0;
    size_t nok = 0, k = 0, at = 0, sl, fields = 0;
    char code, type;
    int r, rr;
    const uint8_t* body;
    size_t blen;
    long u = dyn_pgw_frame(b, n, 4096, &type, &body, &blen);
    uint64_t L = n >= 5 ? ((uint64_t)b[1] << 24 | (uint64_t)b[2] << 16 | (uint64_t)b[3] << 8 | b[4]) : 0;
    if (u != (n < 5 ? 0 : L < 4 || L > 4096 ? DYN_PGW_E_LEN : n < L + 1 ? 0 : (long)L + 1))
        return "frame verdict differs (length counts itself, excludes the type byte; 4 <= length <= maxmsg)";
    rr = pm_ref_rowdesc(b, n, &cnt, &nok);
    r = dyn_pgw_rowdesc_begin(&it, b, n);
    if (rr < 0 ? r != DYN_PGW_E_TRUNC : r != cnt)
        return "RowDescription count verdict differs";
    while (rr >= 0 && (r = dyn_pgw_rowdesc_next(&it, &f)) == 1)
        k++;
    if (rr >= 0 && (k != nok || r != (rr ? DYN_PGW_E_TRUNC : 0)))
        return "RowDescription iterator differs from the reference";
    rr = pm_ref_datarow(b, n, &cnt, &nok);
    r = dyn_pgw_datarow_begin(&it, b, n);
    if (rr < 0 && rr != DYN_PGW_E_TRUNC && rr != DYN_PGW_E_PAST ? r != DYN_PGW_E_TRUNC : r != cnt)
        return "DataRow count verdict differs";
    for (k = 0; n >= 2 && (r = dyn_pgw_datarow_next(&it, &c, &l)) == 1; k++)
        if (l != DYN_PGW_NULL && (c < b || c + l > b + n))
            return "a DataRow column points outside the message";
    if (n >= 2 && (k != nok || r != rr))
        return "DataRow iterator differs from the reference (truncated length vs column past the end)";
    while (dyn_pgw_field_next(b, n, &at, &code, &c, &sl)) {
        if (c + sl > b + n || at > n || (c + sl < b + n && c[sl]) || memchr(c, 0, sl))
            return "an ErrorResponse field is not a NUL-terminated string inside the message";
        if (++fields > n)
            return "ErrorResponse iterator does not advance";
    }
    if (at < n && b[at])
        return "ErrorResponse iterator stopped before the terminating NUL";
    return NULL;
}

static const char* pm_check_values(pm_src_t* s)
{
    uint8_t raw[64], dec[64], txt[140];
    char hex[140], ref[64];
    size_t n = pm_pick(s, 64), i;
    int64_t v, w;
    for (i = 0; i < n; i++)
        raw[i] = (uint8_t)pm_pick(s, 256);
    dyn_pgw_hex(raw, n, hex);
    if (dyn_pgw_bytea_hex((const uint8_t*)hex, 2 * n + 2, dec) != (long)n || memcmp(dec, raw, n))
        return "bytea hex does not round-trip";
    for (i = 0; i < 2 * n + 2; i++)
        txt[i] = (uint8_t)(i > 1 && pm_pick(s, 3) == 0 ? (pm_pick(s, 2) ? "0aF"[pm_pick(s, 3)] : pm_pick(s, 256)) : hex[i]);
    {
        size_t m = pm_pick(s, 4) ? 2 * n + 2 : pm_pick(s, (uint32_t)(2 * n + 3));
        int ok = m >= 2 && txt[0] == '\\' && txt[1] == 'x' && !(m & 1);
        for (i = 2; ok && i < m; i++)
            ok = (txt[i] >= '0' && txt[i] <= '9') || ((txt[i] | 32) >= 'a' && (txt[i] | 32) <= 'f');
        if ((dyn_pgw_bytea_hex(txt, m, dec) >= 0) != ok)
            return "bytea hex verdict differs (\\x prefix, even length, hex digits only)";
        if (ok)
            for (i = 0; i < (m - 2) / 2; i++)
                if (dec[i] != (uint8_t)strtoul((char[]){ (char)txt[2 + 2 * i], (char)txt[3 + 2 * i], 0 }, NULL, 16))
                    return "bytea hex decoded the wrong byte";
    }
    v = (int64_t)((uint64_t)pm_pick(s, 1u << 31) << 33 ^ (uint64_t)pm_pick(s, 1u << 31) << 2 ^ pm_pick(s, 4));
    if (pm_pick(s, 4) == 0)
        v = pm_pick(s, 2) ? INT64_MIN : INT64_MAX;
    n = (size_t)snprintf(ref, sizeof ref, "%lld", (long long)v);
    if (!dyn_pgw_int8_text((const uint8_t*)ref, n, &w) || w != v)
        return "int8 text does not round-trip";
    if ((v == INT64_MAX || v == INT64_MIN) && pm_pick(s, 2))
        ref[n - 1] = (char)(ref[n - 1] + 1 + pm_pick(s, 2));
    if (pm_pick(s, 6) == 0) {
        size_t pad = 46 + pm_pick(s, 5) - n, neg = ref[0] == '-';
        memmove(ref + neg + pad, ref + neg, n - neg);
        memset(ref + neg, '0', pad);
        n += pad;
    } else
    for (i = 0; i < n; i++)
        if (pm_pick(s, 8) == 0)
            ref[i] = "0-+ 9x\x01"[pm_pick(s, 7)];
    if (pm_pick(s, 3) == 0 && n < 60)
        ref[n++] = (char)("09 "[pm_pick(s, 3)]);
    ref[n] = 0;
    {
        char* end;
        long long q;
        int ok = n && n < 48 && (ref[0] == '-' || (ref[0] >= '0' && ref[0] <= '9'));
        errno = 0;
        q = ok ? strtoll(ref, &end, 10) : 0;
        ok = ok && !*end && errno != ERANGE;
        if (dyn_pgw_int8_text((const uint8_t*)ref, n, &w) != ok || (ok && w != q))
            return "int8 text verdict or value differs from strtoll (sign, digits only, range, < 48 chars)";
    }
    for (i = 0; i < 16; i++)
        raw[i] = (uint8_t)pm_pick(s, 256);
    dyn_pgw_uuid(raw, hex);
    snprintf(ref, sizeof ref, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", raw[0], raw[1],
        raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
    return memcmp(hex, ref, 36) ? "uuid text differs from 8-4-4-4-12 lowercase hex" : NULL;
}

static int pm_ref_copyresp(const uint8_t* b, size_t n, uint8_t* fmt, uint16_t* cnt)
{
    size_t i;
    if (n < 3 || b[0] > 1)
        return -1;
    *fmt = b[0];
    *cnt = (uint16_t)(b[1] << 8 | b[2]);
    if (n != 3 + (size_t)*cnt * 2)
        return -1;
    for (i = 3; i < n; i += 2)
        if (b[i] != 0 || b[i + 1] > 1 || (b[0] == 0 && b[i + 1] != 0))
            return -1;
    return 0;
}

static const char* pm_check_copyresp(const uint8_t* b, size_t n)
{
    uint8_t f1 = 9, f2 = 9;
    uint16_t c1 = 0, c2 = 0;
    int r = dyn_pgw_copy_response(b, n, &f1, &c1), m = pm_ref_copyresp(b, n, &f2, &c2);
    if (r > 0)
        return "copy response returned a positive code";
    if ((r == 0) != (m == 0))
        return "copy response accept/reject differs from the model";
    if (r == 0 && (f1 != f2 || c1 != c2))
        return "copy response format or column count differs from the model";
    if (r == 0 && n != 3 + 2 * (size_t)c1)
        return "copy response accepted with a length that does not match its column count";
    return NULL;
}

static const char* pm_check_copygen(pm_src_t* s)
{
    uint8_t b[3 + 2 * 64];
    uint8_t fmt = (uint8_t)pm_pick(s, 2);
    uint32_t k = pm_pick(s, 65), i;
    const char* e;
    b[0] = fmt;
    b[1] = 0;
    b[2] = (uint8_t)k;
    for (i = 0; i < k; i++) {
        b[3 + 2 * i] = 0;
        b[4 + 2 * i] = (uint8_t)(fmt ? pm_pick(s, 2) : 0);
    }
    if (dyn_pgw_copy_response(b, 3 + 2 * k, NULL, NULL) != 0)
        return "a well-formed copy response was rejected";
    if ((e = pm_check_copyresp(b, 3 + 2 * k)) != NULL)
        return e;
    for (i = pm_pick(s, 3); i; i--)
        b[pm_pick(s, 3 + 2 * k)] = (uint8_t)pm_pick(s, 256);
    return pm_check_copyresp(b, 3 + 2 * k - pm_pick(s, 2));
}

static int pm_ref_npv(const uint8_t* b, size_t n, uint32_t* minor, uint32_t* nopt)
{
    uint32_t k, z = 0;
    size_t i;
    if (n < 8)
        return -1;
    k = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
    for (i = 8; i < n; i++)
        z += b[i] == 0;
    if (k == 0 ? n != 8 : (z != k || b[n - 1] != 0))
        return -1;
    *minor = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    *nopt = k;
    return 0;
}

static const char* pm_check_npvkey(const uint8_t* b, size_t n)
{
    uint32_t m1 = 7, m2 = 7, k1 = 7, k2 = 7, p = 0;
    int r = dyn_pgw_negotiate_version(b, n, &m1, &k1), m = pm_ref_npv(b, n, &m2, &k2), v;
    if ((r == 0) != (m == 0))
        return "NegotiateProtocolVersion accept/reject differs from the model";
    if (r == 0 && (m1 != m2 || k1 != k2))
        return "NegotiateProtocolVersion fields differ from the model";
    for (v = 0; v <= 2; v += 2) {
        int want = n >= 8 && n - 4 >= 4 && n - 4 <= 256 && (v == 2 || n - 4 == 4);
        if ((dyn_pgw_backend_key(b, n, v, &p) == 0) != want)
            return "BackendKeyData length verdict differs from the model";
    }
    return NULL;
}

static const char* pm_check_npvgen(pm_src_t* s)
{
    uint8_t b[8 + 4 * 8];
    uint32_t k = pm_pick(s, 5), i, j, l;
    size_t n = 8;
    const char* e;
    memset(b, 0, 8);
    b[3] = (uint8_t)pm_pick(s, 3);
    b[7] = (uint8_t)k;
    for (i = 0; i < k; i++) {
        l = pm_pick(s, 3);
        for (j = 0; j < l; j++)
            b[n++] = (uint8_t)('a' + pm_pick(s, 26));
        b[n++] = 0;
    }
    if (dyn_pgw_negotiate_version(b, n, NULL, NULL) != 0)
        return "a well-formed NegotiateProtocolVersion was rejected";
    if (n > 8 && dyn_pgw_negotiate_version(b, n - 1, NULL, NULL) == 0)
        return "a NegotiateProtocolVersion missing its last NUL was accepted";
    if ((e = pm_check_npvkey(b, n)) != NULL)
        return e;
    return pm_check_npvkey(b, n - pm_pick(s, 2));
}

static int pm_ref_numeric(const uint8_t* b, size_t n, char* out, size_t cap)
{
    unsigned nd, sign, dscale, i, v;
    int weight;
    long pos, k, len, o = 0, start;
    char* dig;
    if (n < 8)
        return -1;
    nd = (unsigned)(b[0] << 8 | b[1]);
    weight = (int16_t)(b[2] << 8 | b[3]);
    sign = (unsigned)(b[4] << 8 | b[5]);
    dscale = (unsigned)(b[6] << 8 | b[7]);
    if (n != 8 + 2 * (size_t)nd || dscale > 0x3FFF)
        return -1;
    if (sign == 0xC000 || sign == 0xD000 || sign == 0xF000) {
        if (nd)
            return -1;
        snprintf(out, cap, "%s", sign == 0xC000 ? "NaN" : sign == 0xD000 ? "Infinity" : "-Infinity");
        return (int)strlen(out);
    }
    if (sign != 0 && sign != 0x4000)
        return -1;
    dig = (char*)malloc(4 * (size_t)nd + 5);
    if (!dig)
        return -3;
    for (i = 0; i < nd; i++) {
        v = (unsigned)(b[8 + 2 * i] << 8 | b[9 + 2 * i]);
        if (v > 9999) {
            free(dig);
            return -1;
        }
        snprintf(dig + 4 * i, 5, "%04u", v);
    }
    len = 4 * (long)nd;
    pos = 4 * ((long)weight + 1);
    if (sign == 0x4000)
        out[o++] = '-';
    start = o;
    for (k = 0; k < pos; k++) {
        char c = k < len ? dig[k] : '0';
        if (o == start && c == '0')
            continue;
        out[o++] = c;
    }
    if (o == start)
        out[o++] = '0';
    if (dscale) {
        out[o++] = '.';
        for (k = pos; k < pos + (long)dscale; k++)
            out[o++] = k >= 0 && k < len ? dig[k] : '0';
    }
    out[o] = 0;
    free(dig);
    return (int)o;
}

static const char* pm_check_numraw(const uint8_t* b, size_t n)
{
    static char ref[200000], got[200000];
    size_t need = 0;
    int r = pm_ref_numeric(b, n, ref, sizeof ref), k;
    if (r == -3)
        return NULL;
    k = dyn_pgw_numeric_bin(b, n, got, sizeof got, &need);
    if (r < 0)
        return k == -1 ? NULL : "numeric: a malformed value was accepted";
    if (k != r || memcmp(ref, got, (size_t)r + 1))
        return "numeric: text differs from the reference model";
    if (need < (size_t)k + 1)
        return "numeric: need is below the text length";
    if (dyn_pgw_numeric_bin(b, n, got, (size_t)k, NULL) != -2)
        return "numeric: a buffer one byte short did not report -2";
    if (dyn_pgw_numeric_bin(b, n, got, need, NULL) != k)
        return "numeric: a buffer of exactly need bytes failed";
    return NULL;
}

static const char* pm_check_numgen(pm_src_t* s)
{
    static char buf[200000];
    uint8_t b[8 + 2 * 12];
    unsigned nd = pm_pick(s, 9), i, v, sp = pm_pick(s, 8);
    int w = (int)pm_pick(s, 12) - 5;
    unsigned dscale = pm_pick(s, 3) ? pm_pick(s, 14) : pm_pick(s, 0x4000);
    unsigned sign = sp == 0 ? 0xC000 : sp == 1 ? 0xD000 : sp == 2 ? 0xF000 : (sp & 1) ? 0x4000 : 0;
    size_t n;
    const char* e;
    if (sign >= 0xC000) {
        nd = 0;
        dscale = 0;
    }
    n = 8 + 2 * (size_t)nd;
    b[0] = 0;
    b[1] = (uint8_t)nd;
    b[2] = (uint8_t)((unsigned)w >> 8);
    b[3] = (uint8_t)w;
    b[4] = (uint8_t)(sign >> 8);
    b[5] = (uint8_t)sign;
    b[6] = (uint8_t)(dscale >> 8);
    b[7] = (uint8_t)dscale;
    for (i = 0; i < nd; i++) {
        v = pm_pick(s, 10000);
        b[8 + 2 * i] = (uint8_t)(v >> 8);
        b[9 + 2 * i] = (uint8_t)v;
    }
    if ((e = pm_check_numraw(b, n)) != NULL)
        return e;
    if (dyn_pgw_numeric_bin(b, n, buf, sizeof buf, NULL) < 0)
        return "numeric: a well-formed value was rejected";
    if (dyn_pgw_numeric_bin(b, n - 1, buf, sizeof buf, NULL) != -1)
        return "numeric: a value one byte short was accepted";
    if (nd) {
        b[8 + 2 * pm_pick(s, nd)] = 0x27;
        b[9] = 0x10;
        b[8] = 0x27;
        if (dyn_pgw_numeric_bin(b, n, buf, sizeof buf, NULL) != -1)
            return "numeric: a base-10000 digit of 10000 was accepted";
    }
    b[6] |= (uint8_t)(0x40 << pm_pick(s, 2));
    if (dyn_pgw_numeric_bin(b, n, buf, sizeof buf, NULL) != -1)
        return "numeric: a dscale with a reserved bit was accepted";
    return NULL;
}

static long long pm_fdiv(long long a, long long b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static int pm_leap(long long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static long long pm_ref_days(long long y, int m, int d)
{
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long long p = y - 1;
    return 365 * p + pm_fdiv(p, 4) - pm_fdiv(p, 100) + pm_fdiv(p, 400) + cum[m - 1] + (m > 2 && pm_leap(y)) + d - 1 - 730119;
}

static const char* pm_check_tsgen(pm_src_t* s)
{
    static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    static char why[256];
    char t[96], f[96];
    int kind = (int)pm_pick(s, 3), bc = pm_pick(s, 5) == 0, m = 1 + (int)pm_pick(s, 12), d;
    int h = (int)pm_pick(s, 24), mi = (int)pm_pick(s, 60), se = (int)pm_pick(s, 60), fd = (int)pm_pick(s, 7);
    int th = (int)pm_pick(s, 16), tm = (int)pm_pick(s, 4) * 15, tsx = pm_pick(s, 3) == 0 ? (int)pm_pick(s, 60) : 0, tneg = (int)pm_pick(s, 2);
    long long y = bc ? 1 + pm_pick(s, 4713) : pm_pick(s, 3) ? 1 + pm_pick(s, 3000) : 1 + pm_pick(s, 290000), ay = bc ? 1 - y : y;
    long long fr = 0, fus = 0, tz = 0, want, p10 = 1;
    int64_t got = 0, back = 0;
    int o, r, k, i;
    d = 1 + (int)pm_pick(s, (uint32_t)(md[m - 1] + (m == 2 && pm_leap(ay))));
    for (i = 0; i < fd; i++)
        p10 *= 10;
    fr = fd ? (long long)pm_pick(s, (uint32_t)p10) : 0;
    fus = fr;
    for (i = fd; i < 6; i++)
        fus *= 10;
    o = snprintf(t, sizeof t, "%04lld-%02d-%02d", y, m, d);
    if (kind > 0) {
        o += snprintf(t + o, sizeof t - (size_t)o, " %02d:%02d:%02d", h, mi, se);
        if (fd)
            o += snprintf(t + o, sizeof t - (size_t)o, ".%0*lld", fd, fr);
    }
    if (kind == 2) {
        o += snprintf(t + o, sizeof t - (size_t)o, "%c%02d", tneg ? '-' : '+', th);
        if (tm || tsx)
            o += snprintf(t + o, sizeof t - (size_t)o, ":%02d", tm);
        if (tsx)
            o += snprintf(t + o, sizeof t - (size_t)o, ":%02d", tsx);
        tz = (th * 3600LL + tm * 60 + tsx) * (tneg ? -1 : 1);
    }
    if (bc)
        o += snprintf(t + o, sizeof t - (size_t)o, " BC");
    want = (pm_ref_days(ay, m, d) * 86400 + (kind ? h * 3600LL + mi * 60 + se : 0) - tz) * 1000000 + (kind ? fus : 0);
    r = dyn_pgw_ts_text(t, (size_t)o, kind, &got);
    if (r != 0 || got != want) {
        snprintf(why, sizeof why, "ts: '%s' kind %d gave r=%d %lld, want %lld", t, kind, r, (long long)got, want);
        return why;
    }
    k = dyn_pgw_ts_format(want, kind, f, sizeof f);
    if (k <= 0 || dyn_pgw_ts_text(f, (size_t)k, kind, &back) != 0 || back != want) {
        snprintf(why, sizeof why, "ts: %lld kind %d formats as '%s', which does not parse back", want, kind, k > 0 ? f : "?");
        return why;
    }
    if (dyn_pgw_ts_format(want, kind, f, (size_t)k) != -1)
        return "ts: a format buffer one byte short did not fail";
    t[pm_pick(s, (uint32_t)o)] = (char)pm_pick(s, 256);
    (void)dyn_pgw_ts_text(t, (size_t)o, kind, &got);
    (void)dyn_pgw_ts_text(t, (size_t)o - 1, kind, &got);
    return NULL;
}

static const char* pm_check_timeraw(const uint8_t* b, size_t n)
{
    char f[96];
    int64_t us, back;
    int32_t dd, mm;
    int kind, k;
    for (kind = 0; kind < 3; kind++) {
        if (dyn_pgw_ts_text((const char*)b, n, kind, &us) != 0)
            continue;
        k = dyn_pgw_ts_format(us, kind, f, sizeof f);
        if (k <= 0 || dyn_pgw_ts_text(f, (size_t)k, kind, &back) != 0 || back != us)
            return "ts: a parsed value does not survive format and parse";
    }
    (void)dyn_pgw_interval_text((const char*)b, n, &us, &dd, &mm);
    return NULL;
}

static const char* pm_check_ivgen(pm_src_t* s)
{
    static char why[256];
    char t[192];
    int o = 0;
    long long yr = pm_pick(s, 3) ? pm_pick(s, 100) : pm_pick(s, 178956971), mo = pm_pick(s, 12);
    long long dy = pm_pick(s, 3) ? pm_pick(s, 40) : pm_pick(s, 2147483647U), h = pm_pick(s, 3) ? pm_pick(s, 30) : pm_pick(s, 2000000000);
    long long mi = pm_pick(s, 60), se = pm_pick(s, 60), fr = pm_pick(s, 1000000);
    int parts = (int)pm_pick(s, 16), ny = (int)pm_pick(s, 2), nm = (int)pm_pick(s, 2), nd = (int)pm_pick(s, 2), nt = (int)pm_pick(s, 2), ff = (int)pm_pick(s, 2);
    long long wm = 0, wd = 0, wu = 0;
    int64_t us = 0;
    int32_t days = 0, months = 0;
    int r, ok;
    if (parts & 1) {
        o += snprintf(t + o, sizeof t - (size_t)o, "%s%lld year%s", ny ? "-" : "", yr, yr == 1 ? "" : "s");
        wm += (ny ? -1 : 1) * yr * 12;
    }
    if (parts & 2) {
        o += snprintf(t + o, sizeof t - (size_t)o, "%s%s%lld mon%s", o ? " " : "", nm ? "-" : "", mo, mo == 1 ? "" : "s");
        wm += (nm ? -1 : 1) * mo;
    }
    if (parts & 4) {
        o += snprintf(t + o, sizeof t - (size_t)o, "%s%s%lld day%s", o ? " " : "", nd ? "-" : "", dy, dy == 1 ? "" : "s");
        wd += (nd ? -1 : 1) * dy;
    }
    if ((parts & 8) || !(parts & 7)) {
        o += snprintf(t + o, sizeof t - (size_t)o, "%s%s%02lld:%02lld:%02lld", o ? " " : "", nt ? "-" : "", h, mi, se);
        if (ff)
            o += snprintf(t + o, sizeof t - (size_t)o, ".%06lld", fr);
        wu = (nt ? -1 : 1) * (((h * 60 + mi) * 60 + se) * 1000000 + (ff ? fr : 0));
    }
    ok = wm >= INT32_MIN && wm <= INT32_MAX && wd >= INT32_MIN && wd <= INT32_MAX;
    r = dyn_pgw_interval_text(t, (size_t)o, &us, &days, &months);
    if (!ok)
        return r == -1 ? NULL : "interval: a month or day count past int32 was accepted";
    if (r != 0 || us != wu || days != wd || months != wm) {
        snprintf(why, sizeof why, "interval: '%s' gave r=%d %d %d %lld", t, r, (int)months, (int)days, (long long)us);
        return why;
    }
    (void)dyn_pgw_interval_text(t, (size_t)o - 1, &us, &days, &months);
    return NULL;
}
typedef struct {
    int nd, nel;
    int32_t dims[DYN_PGW_MAXDIM];
    int isnull[64], len[64];
    char el[64][8];
} pm_arr_t;

static void pm_arr_gen(pm_src_t* s, pm_arr_t* a)
{
    static const char al[] = "ab\"\\{},Nul x\t";
    int d, i, j, total = 1;
    a->nd = pm_pick(s, 8) == 0 ? 0 : 1 + (int)pm_pick(s, 3);
    if (a->nd && pm_pick(s, 16) == 0)
        a->nd = DYN_PGW_MAXDIM;
    for (d = 0; d < a->nd; d++) {
        a->dims[d] = 1 + (int32_t)pm_pick(s, a->nd == DYN_PGW_MAXDIM ? 1 : 3);
        total *= a->dims[d];
    }
    a->nel = a->nd ? total : 0;
    for (i = 0; i < a->nel; i++) {
        uint32_t kind = pm_pick(s, 8);
        a->isnull[i] = kind == 0;
        if (kind == 1) {
            memcpy(a->el[i], pm_pick(s, 2) ? "NULL" : "nUlL", 4);
            a->len[i] = 4;
            continue;
        }
        a->len[i] = (int)pm_pick(s, 7);
        for (j = 0; j < a->len[i]; j++)
            a->el[i][j] = al[pm_pick(s, sizeof al - 1)];
    }
}

static int pm_arr_needq(const char* e, int len)
{
    int j;
    if (len == 0 || (len == 4 && (e[0] | 32) == 'n' && (e[1] | 32) == 'u' && (e[2] | 32) == 'l' && (e[3] | 32) == 'l'))
        return 1;
    for (j = 0; j < len; j++)
        if (e[j] == '"' || e[j] == '\\' || e[j] == '{' || e[j] == '}' || e[j] == ',' || e[j] == ' ' || e[j] == '\t')
            return 1;
    return 0;
}

static void pm_arr_elem_out(const char* e, int len, int isnull, int forceq, char* o, size_t* n)
{
    int j;
    if (isnull) {
        memcpy(o + *n, "NULL", 4);
        *n += 4;
    } else if (forceq || pm_arr_needq(e, len)) {
        o[(*n)++] = '"';
        for (j = 0; j < len; j++) {
            if (e[j] == '"' || e[j] == '\\')
                o[(*n)++] = '\\';
            o[(*n)++] = e[j];
        }
        o[(*n)++] = '"';
    } else {
        memcpy(o + *n, e, (size_t)len);
        *n += (size_t)len;
    }
}

static void pm_arr_ser_lvl(int nd, const int32_t* dims, int lvl, int* k, char* o, size_t* n, int spaces,
    const pm_arr_t* a, const dyn_pgw_span_t* sp, const char* buf)
{
    int i;
    o[(*n)++] = '{';
    for (i = 0; i < dims[lvl]; i++) {
        if (i)
            o[(*n)++] = ',';
        if (spaces)
            o[(*n)++] = ' ';
        if (lvl + 1 < nd)
            pm_arr_ser_lvl(nd, dims, lvl + 1, k, o, n, spaces, a, sp, buf);
        else {
            int e = (*k)++;
            if (a)
                pm_arr_elem_out(a->el[e], a->len[e], a->isnull[e], 0, o, n);
            else
                pm_arr_elem_out(buf + sp[e].off, sp[e].len, sp[e].len < 0, 1, o, n);
        }
        if (spaces)
            o[(*n)++] = ' ';
    }
    o[(*n)++] = '}';
}

static size_t pm_arr_ser(int nd, const int32_t* dims, char* o, int spaces, const pm_arr_t* a, const dyn_pgw_span_t* sp, const char* buf)
{
    size_t n = 0;
    int k = 0;
    if (!nd) {
        memcpy(o, "{}", 2);
        return 2;
    }
    pm_arr_ser_lvl(nd, dims, 0, &k, o, &n, spaces, a, sp, buf);
    return n;
}

static const char* pm_check_arrtext_raw(const uint8_t* b, size_t n)
{
    static dyn_pgw_span_t sp[512], sp2[512];
    static char out[65536], out2[65536], ser[300000];
    int32_t dims[DYN_PGW_MAXDIM], dims2[DYN_PGW_MAXDIM];
    int nd = 0, nd2 = 0;
    size_t nel = 0, nel2 = 0, sn, i;
    if (n > sizeof out)
        return NULL;
    if (dyn_pgw_array_text((const char*)b, n, &nd, dims, NULL, 0, NULL, &nel) != 0)
        return dyn_pgw_array_text((const char*)b, n, &nd, dims, sp, 512, out, &nel) == 0 ? "array text: the fill pass accepted what the count pass rejected" : NULL;
    if (nel > 512)
        return NULL;
    if (dyn_pgw_array_text((const char*)b, n, &nd, dims, sp, 512, out, &nel) != 0)
        return "array text: the fill pass rejected what the count pass accepted";
    sn = pm_arr_ser(nd, dims, ser, 0, NULL, sp, out);
    if (dyn_pgw_array_text(ser, sn, &nd2, dims2, sp2, 512, out2, &nel2) != 0 || nd2 != nd || nel2 != nel)
        return "array text: a parsed array does not survive serialise and parse";
    for (i = 0; i < (size_t)nd; i++)
        if (dims[i] != dims2[i])
            return "array text: dims changed after serialise and parse";
    for (i = 0; i < nel; i++)
        if (sp[i].len != sp2[i].len || (sp[i].len > 0 && memcmp(out + sp[i].off, out2 + sp2[i].off, (size_t)sp[i].len)))
            return "array text: an element changed after serialise and parse";
    return NULL;
}

static const char* pm_check_arrtext_gen(pm_src_t* s)
{
    static char why[600];
    pm_arr_t a;
    char t[4096], out[4096];
    dyn_pgw_span_t sp[64];
    int32_t dims[DYN_PGW_MAXDIM];
    int nd = -1, i, bounds = (int)pm_pick(s, 3) == 0, spaces = (int)pm_pick(s, 3) == 0;
    size_t n = 0, nel = 0;
    pm_arr_gen(s, &a);
    if (bounds && a.nd) {
        for (i = 0; i < a.nd; i++) {
            int lb = (int)pm_pick(s, 11) - 5;
            n += (size_t)snprintf(t + n, sizeof t - n, "[%d:%d]", lb, lb + a.dims[i] - 1);
        }
        t[n++] = '=';
    }
    n += pm_arr_ser(a.nd, a.dims, t + n, spaces, &a, NULL, NULL);
    if (dyn_pgw_array_text(t, n, &nd, dims, sp, 64, out, &nel) != 0 || nd != a.nd || (int)nel != a.nel) {
        snprintf(why, sizeof why, "array text: '%.*s' rejected or wrong shape (nd %d nel %zu, want %d %d)", (int)n, t, nd, nel, a.nd, a.nel);
        return why;
    }
    for (i = 0; i < nd; i++)
        if (dims[i] != a.dims[i])
            return "array text: wrong dims";
    for (i = 0; i < a.nel; i++)
        if (a.isnull[i] ? sp[i].len != -1 : (sp[i].len != a.len[i] || memcmp(out + sp[i].off, a.el[i], (size_t)a.len[i]))) {
            snprintf(why, sizeof why, "array text: element %d of '%.*s' differs", i, (int)n, t);
            return why;
        }
    if (a.nel > 1 && dyn_pgw_array_text(t, n, &nd, dims, sp, (size_t)a.nel - 1, out, &nel) != -2)
        return "array text: a span buffer one short did not report -2";
    if (dyn_pgw_array_text(t, n - 1, &nd, dims, NULL, 0, NULL, &nel) != -1)
        return "array text: a truncated array was accepted";
    t[pm_pick(s, (uint32_t)n)] = (char)pm_pick(s, 256);
    return pm_check_arrtext_raw((const uint8_t*)t, n);
}

static size_t pm_arr_bin(const pm_arr_t* a, uint8_t* b)
{
    size_t n = 0;
    int i;
#define PM_P32(v) do { uint32_t _v = (uint32_t)(v); b[n++] = (uint8_t)(_v >> 24); b[n++] = (uint8_t)(_v >> 16); b[n++] = (uint8_t)(_v >> 8); b[n++] = (uint8_t)_v; } while (0)
    PM_P32(a->nd);
    PM_P32(0);
    PM_P32(25);
    for (i = 0; i < a->nd; i++) {
        PM_P32(a->dims[i]);
        PM_P32(1 - i);
    }
    for (i = 0; i < a->nel; i++) {
        if (a->isnull[i]) {
            PM_P32(-1);
            b[7] = 1;
            continue;
        }
        PM_P32(a->len[i]);
        memcpy(b + n, a->el[i], (size_t)a->len[i]);
        n += (size_t)a->len[i];
    }
#undef PM_P32
    return n;
}

static const char* pm_check_arrbin_raw(const uint8_t* b, size_t n)
{
    dyn_pgw_arrbin_t h;
    size_t at = 0, k = 0;
    const uint8_t* e;
    int32_t len;
    if (dyn_pgw_array_bin(b, n, &h) != 0)
        return NULL;
    while (dyn_pgw_array_bin_next(&h, &at, &e, &len)) {
        if (len > 0 && (e < b || e + len > b + n))
            return "array bin: an element points outside the buffer";
        k++;
    }
    return k == h.nel ? NULL : "array bin: the walk does not visit nel elements";
}

static const char* pm_check_arrbin_gen(pm_src_t* s)
{
    pm_arr_t a;
    uint8_t b[2048];
    dyn_pgw_arrbin_t h;
    size_t n, at = 0;
    const uint8_t* e;
    int32_t len;
    int i;
    pm_arr_gen(s, &a);
    n = pm_arr_bin(&a, b);
    if (dyn_pgw_array_bin(b, n, &h) != 0 || h.ndim != a.nd || (int)h.nel != a.nel || h.elem != 25)
        return "array bin: a well-formed array was rejected or misread";
    for (i = 0; i < a.nd; i++)
        if (h.dims[i] != a.dims[i] || h.lb[i] != 1 - i)
            return "array bin: wrong dims or lower bounds";
    for (i = 0; i < a.nel; i++) {
        if (!dyn_pgw_array_bin_next(&h, &at, &e, &len) || (a.isnull[i] ? len != -1 : (len != a.len[i] || memcmp(e, a.el[i], (size_t)len))))
            return "array bin: element differs";
    }
    if (dyn_pgw_array_bin(b, n - 1, &h) != -1)
        return "array bin: a truncated array was accepted";
    if (a.nd) {
        uint8_t c[2048];
        memcpy(c, b, n);
        c[12] = 0x7f;
        c[13] = c[14] = c[15] = 0xff;
        if (dyn_pgw_array_bin(c, n, &h) != -1)
            return "array bin: a dimension past the buffer was accepted";
        if (a.nd >= 2) {
            c[12] = 0;
            c[13] = 1;
            c[14] = c[15] = 0;
            c[20] = 0;
            c[21] = 1;
            c[22] = c[23] = 0;
            if (dyn_pgw_array_bin(c, n, &h) != -1)
                return "array bin: dims whose product overflows the buffer were accepted";
        }
    }
    b[7] = 2;
    if (dyn_pgw_array_bin(b, n, &h) != -1)
        return "array bin: a null-flag of 2 was accepted";
    b[pm_pick(s, (uint32_t)n)] = (uint8_t)pm_pick(s, 256);
    return pm_check_arrbin_raw(b, n);
}

typedef struct {
    int flags, lo_len, hi_len;
    char lo[8], hi[8];
} pm_rng_t;

static void pm_rng_gen(pm_src_t* s, pm_rng_t* r)
{
    static const char al[] = "a1\"\\,()[] x";
    int j;
    memset(r, 0, sizeof(*r));
    if (pm_pick(s, 8) == 0) {
        r->flags = DYN_PGW_RANGE_EMPTY;
        r->lo_len = r->hi_len = -1;
        return;
    }
    r->flags = (pm_pick(s, 2) ? DYN_PGW_RANGE_LB_INC : 0) | (pm_pick(s, 2) ? DYN_PGW_RANGE_UB_INC : 0);
    if (pm_pick(s, 4) == 0)
        r->flags = (r->flags & ~DYN_PGW_RANGE_LB_INC) | DYN_PGW_RANGE_LB_INF;
    if (pm_pick(s, 4) == 0)
        r->flags = (r->flags & ~DYN_PGW_RANGE_UB_INC) | DYN_PGW_RANGE_UB_INF;
    r->lo_len = (r->flags & DYN_PGW_RANGE_LB_INF) ? -1 : (int)pm_pick(s, 7);
    r->hi_len = (r->flags & DYN_PGW_RANGE_UB_INF) ? -1 : (int)pm_pick(s, 7);
    for (j = 0; j < r->lo_len; j++)
        r->lo[j] = al[pm_pick(s, sizeof al - 1)];
    for (j = 0; j < r->hi_len; j++)
        r->hi[j] = al[pm_pick(s, sizeof al - 1)];
}

static void pm_rng_bound_out(const char* b, int len, char* o, size_t* n)
{
    int j, q = len == 0;
    for (j = 0; j < len; j++)
        if (strchr("\"\\()[], \t", b[j]))
            q = 1;
    if (q)
        o[(*n)++] = '"';
    for (j = 0; j < len; j++) {
        if (q && (b[j] == '"' || b[j] == '\\'))
            o[(*n)++] = b[j];
        o[(*n)++] = b[j];
    }
    if (q)
        o[(*n)++] = '"';
}

static size_t pm_rng_ser(int flags, const char* lo, int lo_len, const char* hi, int hi_len, char* o)
{
    size_t n = 0;
    if (flags & DYN_PGW_RANGE_EMPTY) {
        memcpy(o, "empty", 5);
        return 5;
    }
    o[n++] = (flags & DYN_PGW_RANGE_LB_INC) ? '[' : '(';
    if (lo_len >= 0)
        pm_rng_bound_out(lo, lo_len, o, &n);
    o[n++] = ',';
    if (hi_len >= 0)
        pm_rng_bound_out(hi, hi_len, o, &n);
    o[n++] = (flags & DYN_PGW_RANGE_UB_INC) ? ']' : ')';
    return n;
}

static int pm_rng_same(const dyn_pgw_range_t* r, int flags, const char* lo, int lo_len, const char* hi, int hi_len)
{
    if (r->flags != flags)
        return 0;
    if (flags & DYN_PGW_RANGE_EMPTY)
        return 1;
    return r->lo_len == lo_len && r->hi_len == hi_len && (lo_len <= 0 || !memcmp(r->lo, lo, (size_t)lo_len)) && (hi_len <= 0 || !memcmp(r->hi, hi, (size_t)hi_len));
}

static const char* pm_check_rngtext_raw(const uint8_t* b, size_t n)
{
    static char out[70000], out2[70000], ser[300000];
    dyn_pgw_range_t r, r2;
    size_t at = 0, sn;
    int k, guard = 0;
    if (n > sizeof out)
        return NULL;
    if (dyn_pgw_range_text((const char*)b, n, &r, out) == 0) {
        sn = pm_rng_ser(r.flags, (const char*)r.lo, r.lo_len, (const char*)r.hi, r.hi_len, ser);
        if (dyn_pgw_range_text(ser, sn, &r2, out2) != 0 || !pm_rng_same(&r2, r.flags, (const char*)r.lo, r.lo_len, (const char*)r.hi, r.hi_len))
            return "range text: a parsed range does not survive serialise and parse";
    }
    while ((k = dyn_pgw_multirange_text((const char*)b, n, &at, &r, out)) > 0 && guard++ < 100000)
        ;
    return NULL;
}

static const char* pm_check_rngbin_raw(const uint8_t* b, size_t n)
{
    dyn_pgw_range_t r;
    size_t at = 0;
    int guard = 0;
    if (dyn_pgw_range_bin(b, n, &r) == 0) {
        if (r.lo_len > 0 && (r.lo < b || r.lo + r.lo_len > b + n))
            return "range bin: lower bound outside the buffer";
        if (r.hi_len > 0 && (r.hi < b || r.hi + r.hi_len > b + n))
            return "range bin: upper bound outside the buffer";
    }
    while (dyn_pgw_multirange_bin(b, n, &at, &r) > 0 && guard++ < 100000)
        if ((r.lo_len > 0 && r.lo + r.lo_len > b + n) || (r.hi_len > 0 && r.hi + r.hi_len > b + n))
            return "multirange bin: a bound outside the buffer";
    return NULL;
}

static size_t pm_rng_bin(const pm_rng_t* r, uint8_t* b)
{
    size_t n = 0;
    int side;
    b[n++] = (uint8_t)r->flags;
    if (r->flags & DYN_PGW_RANGE_EMPTY)
        return n;
    for (side = 0; side < 2; side++) {
        int len = side ? r->hi_len : r->lo_len;
        if (len < 0)
            continue;
        b[n++] = 0;
        b[n++] = 0;
        b[n++] = 0;
        b[n++] = (uint8_t)len;
        memcpy(b + n, side ? r->hi : r->lo, (size_t)len);
        n += (size_t)len;
    }
    return n;
}

static const char* pm_check_rng_gen(pm_src_t* s)
{
    static char why[300];
    pm_rng_t g[5];
    char t[1024], out[1024];
    uint8_t b[1024], mb[2048];
    dyn_pgw_range_t r;
    size_t n, at = 0, mn = 4;
    int i, k, cnt = (int)pm_pick(s, 5), got = 0, sp = (int)pm_pick(s, 2);
    pm_rng_gen(s, &g[0]);
    n = pm_rng_ser(g[0].flags, g[0].lo, g[0].lo_len, g[0].hi, g[0].hi_len, t);
    if (dyn_pgw_range_text(t, n, &r, out) != 0 || !pm_rng_same(&r, g[0].flags, g[0].lo, g[0].lo_len, g[0].hi, g[0].hi_len)) {
        snprintf(why, sizeof why, "range text: '%.*s' rejected or misread", (int)n, t);
        return why;
    }
    if (dyn_pgw_range_text(t, n - 1, &r, out) == 0 && !(g[0].flags & DYN_PGW_RANGE_EMPTY))
        return "range text: a range missing its closing bracket was accepted";
    n = pm_rng_bin(&g[0], b);
    if (dyn_pgw_range_bin(b, n, &r) != 0 || !pm_rng_same(&r, g[0].flags, g[0].lo, g[0].lo_len, g[0].hi, g[0].hi_len))
        return "range bin: a well-formed range was rejected or misread";
    b[n] = 0;
    if (dyn_pgw_range_bin(b, n + 1, &r) != -1 || (n > 1 && dyn_pgw_range_bin(b, n - 1, &r) != -1))
        return "range bin: a trailing or missing byte was accepted";
    if (g[0].flags & DYN_PGW_RANGE_EMPTY) {
        b[0] |= DYN_PGW_RANGE_LB_INC;
        if (dyn_pgw_range_bin(b, 1, &r) != -1)
            return "range bin: EMPTY with another flag was accepted";
    }
    n = 0;
    t[n++] = '{';
    mb[0] = mb[1] = mb[2] = 0;
    mb[3] = (uint8_t)cnt;
    for (i = 0; i < cnt; i++) {
        size_t bl;
        int tries;
        pm_rng_gen(s, &g[i]);
        for (tries = 0; tries < 4 && (g[i].flags & DYN_PGW_RANGE_EMPTY); tries++)
            pm_rng_gen(s, &g[i]);
        if (g[i].flags & DYN_PGW_RANGE_EMPTY) {
            g[i].flags = DYN_PGW_RANGE_LB_INF | DYN_PGW_RANGE_UB_INF;
            g[i].lo_len = g[i].hi_len = -1;
        }
        if (i)
            t[n++] = ',';
        if (sp)
            t[n++] = ' ';
        n += pm_rng_ser(g[i].flags, g[i].lo, g[i].lo_len, g[i].hi, g[i].hi_len, t + n);
        bl = pm_rng_bin(&g[i], mb + mn + 4);
        mb[mn] = mb[mn + 1] = mb[mn + 2] = 0;
        mb[mn + 3] = (uint8_t)bl;
        mn += 4 + bl;
    }
    if (sp)
        t[n++] = ' ';
    t[n++] = '}';
    while ((k = dyn_pgw_multirange_text(t, n, &at, &r, out)) > 0) {
        if (got >= cnt || !pm_rng_same(&r, g[got].flags, g[got].lo, g[got].lo_len, g[got].hi, g[got].hi_len)) {
            snprintf(why, sizeof why, "multirange text: '%.*s' range %d misread", (int)n, t, got);
            return why;
        }
        got++;
    }
    if (k != 0 || got != cnt) {
        snprintf(why, sizeof why, "multirange text: '%.*s' ended with %d after %d of %d", (int)n, t, k, got, cnt);
        return why;
    }
    at = 0;
    got = 0;
    while ((k = dyn_pgw_multirange_bin(mb, mn, &at, &r)) > 0) {
        if (got >= cnt || !pm_rng_same(&r, g[got].flags, g[got].lo, g[got].lo_len, g[got].hi, g[got].hi_len))
            return "multirange bin: range misread";
        got++;
    }
    if (k != 0 || got != cnt)
        return "multirange bin: wrong count";
    at = 0;
    if (dyn_pgw_multirange_bin(mb, mn - 1, &at, &r) != -1 && cnt)
        return "multirange bin: a truncated multirange was accepted";
    t[pm_pick(s, (uint32_t)n)] = (char)pm_pick(s, 256);
    mb[pm_pick(s, (uint32_t)mn)] = (uint8_t)pm_pick(s, 256);
    {
        const char* e = pm_check_rngtext_raw((const uint8_t*)t, n);
        if (e)
            return e;
    }
    return pm_check_rngbin_raw(mb, mn);
}

#define PM_URI_MAX 2048
#define PM_URI_OUT (4 * PM_URI_MAX + 64)

static int pm_hexd(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static char pm_uri_dec(const char* str, char* out)
{
    const char* q = str;
    char* p = out;
    while (*q == ' ')
        q++;
    for (;;) {
        if (*q != '%') {
            if (*q == ' ' || *q == 0)
                break;
            *p++ = *q++;
        } else {
            int hi, lo;
            ++q;
            if ((hi = pm_hexd(*q++)) < 0 || (lo = pm_hexd(*q++)) < 0)
                return 'x';
            if (!((hi << 4) | lo))
                return 'z';
            *p++ = (char)((hi << 4) | lo);
        }
    }
    while (*q == ' ')
        q++;
    if (*q)
        return 'w';
    *p = 0;
    return 0;
}

static void pm_uri_err(char* out, size_t* o, char e)
{
    out[(*o)++] = 'E';
    out[(*o)++] = e;
    out[(*o)++] = '\n';
}

static void pm_uri_line(char* out, size_t* o, char tag, const char* key, const char* v, size_t vn)
{
    size_t kn = key ? strlen(key) : 0;
    out[(*o)++] = tag;
    memcpy(out + *o, key, kn);
    *o += kn;
    out[(*o)++] = '=';
    memcpy(out + *o, v, vn);
    *o += vn;
    out[(*o)++] = '\n';
}

static int pm_uri_emit(char* out, size_t* o, char tag, const char* key, const char* raw)
{
    static char d[PM_URI_MAX + 1];
    char e = pm_uri_dec(raw, d);
    if (e) {
        pm_uri_err(out, o, e);
        return -1;
    }
    pm_uri_line(out, o, tag, key ? key : "", d, strlen(d));
    return 0;
}

static size_t pm_uri_model(const char* uri, char* out)
{
    static char buf[PM_URI_MAX + 1], hb[PM_URI_MAX + 2], pb[PM_URI_MAX + 2], kd[PM_URI_MAX + 1];
    char *p, *start, *host, prev = 0, e;
    size_t o = 0, hl = 0, pl = 0, L = strlen(uri), k;
    int pre;
    if (L > PM_URI_MAX)
        L = PM_URI_MAX;
    memcpy(buf, uri, L);
    buf[L] = 0;
    pre = !strncmp(buf, "postgresql://", 13) ? 13 : !strncmp(buf, "postgres://", 11) ? 11 : 0;
    if (!pre) {
        pm_uri_err(out, &o, 's');
        goto done;
    }
    start = buf + pre;
    p = start;
    while (*p && *p != '@' && *p != '/')
        ++p;
    if (*p == '@') {
        char* user = start;
        p = user;
        while (*p != ':' && *p != '@')
            ++p;
        prev = *p;
        *p = 0;
        if (*user && pm_uri_emit(out, &o, 'U', NULL, user) < 0)
            goto done;
        if (prev == ':') {
            char* pw = p + 1;
            while (*p != '@')
                ++p;
            *p = 0;
            if (*pw && pm_uri_emit(out, &o, 'P', NULL, pw) < 0)
                goto done;
        }
        ++p;
    } else {
        p = start;
    }
    for (;;) {
        if (*p == '[') {
            host = ++p;
            while (*p && *p != ']')
                ++p;
            if (!*p) {
                pm_uri_err(out, &o, 'b');
                goto done;
            }
            if (p == host) {
                pm_uri_err(out, &o, 'e');
                goto done;
            }
            *(p++) = 0;
            if (*p && *p != ':' && *p != '/' && *p != '?' && *p != ',') {
                pm_uri_err(out, &o, 'a');
                goto done;
            }
        } else {
            host = p;
            while (*p && *p != ':' && *p != '/' && *p != '?' && *p != ',')
                ++p;
        }
        prev = *p;
        *p = 0;
        k = strlen(host);
        memcpy(hb + hl, host, k);
        hl += k;
        if (prev == ':') {
            char* port = ++p;
            while (*p && *p != '/' && *p != '?' && *p != ',')
                ++p;
            prev = *p;
            *p = 0;
            k = strlen(port);
            memcpy(pb + pl, port, k);
            pl += k;
        }
        if (prev != ',')
            break;
        ++p;
        hb[hl++] = ',';
        pb[pl++] = ',';
    }
    hb[hl] = 0;
    pb[pl] = 0;
    if (hb[0] && pm_uri_emit(out, &o, 'H', NULL, hb) < 0)
        goto done;
    if (pb[0] && pm_uri_emit(out, &o, 'O', NULL, pb) < 0)
        goto done;
    if (prev && prev != '?') {
        char* db = ++p;
        while (*p && *p != '?')
            ++p;
        prev = *p;
        *p = 0;
        if (*db && pm_uri_emit(out, &o, 'D', NULL, db) < 0)
            goto done;
    }
    if (prev) {
        ++p;
        while (*p) {
            char *kw = p, *val = NULL;
            for (;;) {
                if (*p == '=') {
                    if (val) {
                        pm_uri_err(out, &o, 'q');
                        goto done;
                    }
                    *p++ = 0;
                    val = p;
                } else if (*p == '&' || *p == 0) {
                    if (*p)
                        *p++ = 0;
                    if (!val) {
                        pm_uri_err(out, &o, 'm');
                        goto done;
                    }
                    break;
                } else {
                    ++p;
                }
            }
            if ((e = pm_uri_dec(kw, kd))) {
                pm_uri_err(out, &o, e);
                goto done;
            }
            if (pm_uri_emit(out, &o, 'Q', kd, val) < 0)
                goto done;
        }
    }
done:
    out[o] = 0;
    return o;
}

static int pm_uri_core_emit(char* out, size_t* o, char tag, const char* key, const char* s, size_t n)
{
    static char d[PM_URI_MAX + 1];
    long r = dyn_pgw_uri_decode(s, n, d);
    if (r < 0) {
        pm_uri_err(out, o, r == DYN_PGW_DEC_HEX ? 'x' : r == DYN_PGW_DEC_NUL ? 'z' : 'w');
        return -1;
    }
    pm_uri_line(out, o, tag, key ? key : "", d, (size_t)r);
    return 0;
}

static size_t pm_uri_core(const char* s, size_t n, char* out)
{
    static char kd[PM_URI_MAX + 1], hb[PM_URI_MAX + 2], pb[PM_URI_MAX + 2];
    dyn_pgw_uri_t u;
    size_t o = 0, at, h, hn, p, pn, hl = 0, pl = 0;
    int rc = dyn_pgw_uri_split(s, n, &u), hp, first = 1, k;
    long kl;
    if (rc == DYN_PGW_URI_SCHEME) {
        pm_uri_err(out, &o, 's');
        goto done;
    }
    if (u.has_user && pm_uri_core_emit(out, &o, 'U', NULL, s + u.user, u.user_n))
        goto done;
    if (u.has_pass && pm_uri_core_emit(out, &o, 'P', NULL, s + u.pass, u.pass_n))
        goto done;
    if (rc) {
        pm_uri_err(out, &o, rc == DYN_PGW_URI_BRACKET ? 'b' : rc == DYN_PGW_URI_EMPTY6 ? 'e' : 'a');
        goto done;
    }
    at = u.net;
    while (dyn_pgw_uri_host(s, u.net_end, &at, &h, &hn, &p, &pn, &hp) > 0) {
        if (!first) {
            hb[hl++] = ',';
            pb[pl++] = ',';
        }
        first = 0;
        memcpy(hb + hl, s + h, hn);
        hl += hn;
        if (hp) {
            memcpy(pb + pl, s + p, pn);
            pl += pn;
        }
    }
    if (hl && pm_uri_core_emit(out, &o, 'H', NULL, hb, hl))
        goto done;
    if (pl && pm_uri_core_emit(out, &o, 'O', NULL, pb, pl))
        goto done;
    if (u.has_db && pm_uri_core_emit(out, &o, 'D', NULL, s + u.db, u.db_n))
        goto done;
    if (u.has_q) {
        at = u.q;
        while ((k = dyn_pgw_uri_param(s, u.q + u.q_n, &at, &h, &hn, &p, &pn)) > 0) {
            kl = dyn_pgw_uri_decode(s + h, hn, kd);
            if (kl < 0) {
                pm_uri_err(out, &o, kl == DYN_PGW_DEC_HEX ? 'x' : kl == DYN_PGW_DEC_NUL ? 'z' : 'w');
                goto done;
            }
            kd[kl] = 0;
            if (pm_uri_core_emit(out, &o, 'Q', kd, s + p, pn))
                goto done;
        }
        if (k < 0)
            pm_uri_err(out, &o, k == -1 ? 'm' : 'q');
    }
done:
    out[o] = 0;
    return o;
}

static const char* const pm_uri_tk[] = { "a", "h1", "x.y", "%41", "%2C", "%3a", "%40", "%2F", "%20", "%00", "%zz", "%4", " ", "@", ":", "/", "?", "&", "=", ",",
    "[", "]", "::1", "%5B", "5432", "_" };

static void pm_uri_tok(pm_src_t* s, char* u, size_t* n, int clean)
{
    int c = (int)pm_pick(s, 3);
    while (c-- > 0 && *n < 900) {
        const char* t = pm_uri_tk[clean && pm_pick(s, 3) ? pm_pick(s, 6) : pm_pick(s, sizeof pm_uri_tk / sizeof pm_uri_tk[0])];
        size_t l = strlen(t);
        memcpy(u + *n, t, l);
        *n += l;
    }
}

static size_t pm_uri_gen(pm_src_t* s, char* u)
{
    static const char* const sch[4] = { "postgresql://", "postgres://", "postgres:/", "pg://" };
    static const char* const ky[9] = { "host", "port", "user", "password", "dbname", "application_name", "sslmode", "connect_timeout", "options" };
    const char* sc = sch[pm_pick(s, 8) < 7 ? pm_pick(s, 2) : 2 + pm_pick(s, 2)];
    size_t n = strlen(sc);
    int i, k;
    memcpy(u, sc, n);
    if (!pm_pick(s, 2)) {
        pm_uri_tok(s, u, &n, 1);
        if (pm_pick(s, 2)) {
            u[n++] = ':';
            pm_uri_tok(s, u, &n, 1);
        }
        u[n++] = '@';
    }
    k = 1 + (int)pm_pick(s, 3);
    for (i = 0; i < k; i++) {
        if (i)
            u[n++] = ',';
        if (!pm_pick(s, 5)) {
            u[n++] = '[';
            pm_uri_tok(s, u, &n, 1);
            if (pm_pick(s, 6))
                u[n++] = ']';
        } else {
            pm_uri_tok(s, u, &n, 1);
        }
        if (pm_pick(s, 2)) {
            u[n++] = ':';
            if (pm_pick(s, 3)) {
                memcpy(u + n, "5432", 4);
                n += 4;
            } else {
                pm_uri_tok(s, u, &n, 0);
            }
        }
    }
    if (pm_pick(s, 3)) {
        u[n++] = '/';
        pm_uri_tok(s, u, &n, 1);
    }
    if (pm_pick(s, 2)) {
        u[n++] = '?';
        k = (int)pm_pick(s, 4);
        for (i = 0; i < k; i++) {
            const char* key = ky[pm_pick(s, 9)];
            if (i)
                u[n++] = '&';
            if (pm_pick(s, 10)) {
                memcpy(u + n, key, strlen(key));
                n += strlen(key);
            } else {
                pm_uri_tok(s, u, &n, 0);
            }
            if (pm_pick(s, 12))
                u[n++] = '=';
            pm_uri_tok(s, u, &n, 1);
            if (!pm_pick(s, 15))
                u[n++] = '=';
        }
    }
    u[n] = 0;
    return n;
}

static const char* pm_check_uri_gen(pm_src_t* s)
{
    static char u[PM_URI_MAX + 1], o1[PM_URI_OUT], o2[PM_URI_OUT];
    size_t n = pm_uri_gen(s, u), a, b;
    a = pm_uri_core(u, n, o1);
    b = pm_uri_model(u, o2);
    if (a != b || memcmp(o1, o2, a))
        return "uri: core and model disagree on a generated uri";
    return NULL;
}

static const char* pm_check_uri_raw(const uint8_t* b, size_t n)
{
    static char cp[PM_URI_MAX + 16], o1[PM_URI_OUT], o2[PM_URI_OUT];
    size_t a, c;
    if (n > PM_URI_MAX - 16)
        return NULL;
    memcpy(cp, b, n);
    cp[n] = 0;
    a = pm_uri_core((const char*)b, n, o1);
    c = pm_uri_model(cp, o2);
    if (a != c || memcmp(o1, o2, a))
        return "uri: core and model disagree on raw bytes";
    memcpy(cp, "postgres://", 11);
    memcpy(cp + 11, b, n);
    cp[n + 11] = 0;
    a = pm_uri_core(cp, n + 11, o1);
    c = pm_uri_model(cp, o2);
    if (a != c || memcmp(o1, o2, a))
        return "uri: core and model disagree on postgres:// plus raw bytes";
    return NULL;
}

static char* pm_pwdf(char* buf, const char* token)
{
    char* tbuf = buf;
    const char* ttok = token;
    int bslash = 0;
    if (tbuf[0] == '*' && tbuf[1] == ':')
        return tbuf + 2;
    while (*tbuf != 0) {
        if (*tbuf == '\\' && !bslash) {
            tbuf++;
            bslash = 1;
        }
        if (*tbuf == ':' && *ttok == 0 && !bslash)
            return tbuf + 1;
        bslash = 0;
        if (*ttok == 0)
            return NULL;
        if (*tbuf == *ttok) {
            tbuf++;
            ttok++;
        } else {
            return NULL;
        }
    }
    return NULL;
}

static long pm_pgpass_model(const char* f, size_t n, const char* host, const char* port, const char* db, const char* user, char* out)
{
    static char line[2 * PM_URI_MAX + 2];
    size_t i = 0;
    while (i < n) {
        size_t ls = i, len;
        char *t, *p1, *p2;
        while (i < n && f[i] != '\n')
            i++;
        len = i - ls;
        if (i < n)
            i++;
        if (len > sizeof line - 1)
            continue;
        memcpy(line, f + ls, len);
        line[len] = 0;
        if (line[0] == '#')
            continue;
        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (len == 0 || !(t = pm_pwdf(line, host)) || !(t = pm_pwdf(t, port)) || !(t = pm_pwdf(t, db)) || !(t = pm_pwdf(t, user)))
            continue;
        for (p1 = p2 = t; *p1 != ':' && *p1 != 0; ++p1, ++p2) {
            if (*p1 == '\\' && p1[1] != 0)
                ++p1;
            *p2 = *p1;
        }
        memcpy(out, t, (size_t)(p2 - t));
        return (long)(p2 - t);
    }
    return -1;
}

static const char* pm_pgpass_cmp(const char* f, size_t n, char tok[4][8])
{
    static char o1[PM_URI_MAX + 1], o2[PM_URI_MAX + 1];
    long a = dyn_pgw_pgpass_find(f, n, tok[0], tok[1], tok[2], tok[3], o1);
    long b = pm_pgpass_model(f, n, tok[0], tok[1], tok[2], tok[3], o2);
    if (a != b || (a > 0 && memcmp(o1, o2, (size_t)a)))
        return "pgpass: core and model disagree";
    return NULL;
}

static const char* pm_check_pgpass_gen(pm_src_t* s)
{
    static const char al[] = "ab:\\*#\r";
    static char f[1200];
    char tok[4][8];
    size_t n = 0, l;
    int i, j, lines, fields, L;
    for (i = 0; i < 4; i++) {
        L = (int)pm_pick(s, 4);
        for (j = 0; j < L; j++)
            tok[i][j] = "ab:\\*"[pm_pick(s, 5)];
        tok[i][L] = 0;
    }
    lines = (int)pm_pick(s, 6);
    for (i = 0; i < lines && n < sizeof f - 128; i++) {
        fields = 3 + (int)pm_pick(s, 4);
        if (!pm_pick(s, 8))
            f[n++] = '#';
        for (j = 0; j < fields; j++) {
            if (j)
                f[n++] = ':';
            switch (pm_pick(s, 4)) {
            case 0:
                f[n++] = '*';
                break;
            case 1:
                if (j < 4)
                    for (l = 0; tok[j][l]; l++) {
                        if (tok[j][l] == ':' || tok[j][l] == '\\')
                            f[n++] = '\\';
                        f[n++] = tok[j][l];
                    }
                break;
            case 2:
                L = (int)pm_pick(s, 4);
                while (L-- > 0)
                    f[n++] = al[pm_pick(s, sizeof al - 1)];
                break;
            default:
                if (j < 4) {
                    l = strlen(tok[j]);
                    memcpy(f + n, tok[j], l);
                    n += l;
                }
                break;
            }
        }
        if (!pm_pick(s, 4))
            f[n++] = '\r';
        if (pm_pick(s, 6) || i + 1 < lines)
            f[n++] = '\n';
    }
    return pm_pgpass_cmp(f, n, tok);
}

static const char* pm_check_pgpass_raw(const uint8_t* b, size_t n)
{
    char tok[4][8];
    int i;
    if (n < 4 || n > PM_URI_MAX)
        return NULL;
    for (i = 0; i < 4; i++) {
        tok[i][0] = (char)(b[i] ? b[i] : 'a');
        tok[i][1] = 0;
    }
    return pm_pgpass_cmp((const char*)b + 4, n - 4, tok);
}

#endif
