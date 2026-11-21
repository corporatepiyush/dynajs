/* dyn-resp-model.h -- RESP3 model shared by tests/redis/test_resp_codec.c (seeded rng) and src/fuzz/fuzz_net.c (fuzz
 * bytes). rm_gen builds a random value tree and its wire bytes and records the items dyn_resp_next must return in
 * pre-order; rm_check_tree checks the codec against that record; rm_check_raw checks invariants on arbitrary bytes;
 * rm_check_cmd round-trips dyn_resp_cmd_encode. Each check returns NULL or a message naming the broken rule.
 * rm_nprefix: proper prefixes sampled per message (24 in the ctest; fuzz_net sets 4 for exec/s, its volume covers the rest). */
#ifndef DYN_RESP_MODEL_H
#define DYN_RESP_MODEL_H

#include "dyn-resp.h"
#include "dyn-fuzzkit.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RM_CAP 65536
#define RM_ITEMS 4096

static size_t rm_nprefix = 24;

typedef struct {
    const uint8_t* p;
    size_t n;
    fk_rng_t* r;
} rm_src_t;

typedef struct {
    int type, isnull, depth;
    int64_t ival, count, eff;
    double dval;
    size_t soff, slen, blen;
} rm_item_t;

typedef struct {
    uint8_t buf[RM_CAP + 64];
    size_t len, tail;
    rm_item_t it[RM_ITEMS];
    size_t nit;
    int full;
} rm_msg_t;

static uint32_t rm_pick(rm_src_t* s, uint32_t k)
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

static void rm_put(rm_msg_t* m, const void* p, size_t n)
{
    if (m->len + n > RM_CAP) {
        m->full = 1;
        return;
    }
    memcpy(m->buf + m->len, p, n);
    m->len += n;
}

static void rm_puts(rm_msg_t* m, const char* s)
{
    rm_put(m, s, strlen(s));
}

static void rm_dec(rm_msg_t* m, int64_t v)
{
    char t[32];
    snprintf(t, sizeof t, "%lld", (long long)v);
    rm_puts(m, t);
}

static rm_item_t* rm_item(rm_msg_t* m, int type, int depth)
{
    rm_item_t* i;
    if (m->nit == RM_ITEMS) {
        m->full = 1;
        return &m->it[RM_ITEMS - 1];
    }
    i = &m->it[m->nit++];
    memset(i, 0, sizeof *i);
    i->type = type;
    i->depth = depth;
    return i;
}

static const char* const RM_DBL[] = { "1.5", "-0", "inf", "-inf", "nan", "3", "1e10", "-2.25e-3", "0.1", "1E+2" };
static const double RM_DBLV[] = { 1.5, -0.0, 1e308 * 10, -(1e308 * 10), 0, 3, 1e10, -2.25e-3, 0.1, 100 };
static const uint8_t RM_TYPES[] = { '+', '-', ':', '$', '*', '_', ',', '#', '!', '=', '(', '%', '~', '|', '>' };

static void rm_gen_value(rm_src_t* s, rm_msg_t* m, int depth, int* budget, int deep);

static void rm_line_text(rm_src_t* s, rm_msg_t* m, rm_item_t* i, size_t n, int digits)
{
    size_t k;
    i->soff = m->len;
    for (k = 0; k < n; k++) {
        uint8_t c = digits ? (uint8_t)('0' + rm_pick(s, 10)) : (uint8_t)(rm_pick(s, 4) ? 32 + rm_pick(s, 95) : rm_pick(s, 256));
        if (c == '\r' || c == '\n')
            c = '.';
        rm_put(m, &c, 1);
    }
    i->slen = n;
}

static void rm_gen_value(rm_src_t* s, rm_msg_t* m, int depth, int* budget, int deep)
{
    uint8_t t = deep ? (uint8_t)(rm_pick(s, 2) ? '*' : '|') : RM_TYPES[rm_pick(s, sizeof RM_TYPES)];
    rm_item_t* i = rm_item(m, t, depth);
    size_t k, n;
    (*budget)--;
    rm_put(m, &t, 1);
    switch (t) {
    case '+':
    case '-':
        rm_line_text(s, m, i, rm_pick(s, 3) ? rm_pick(s, 12) : rm_pick(s, 300), 0);
        break;
    case '(':
        if (rm_pick(s, 2))
            rm_puts(m, "-");
        rm_line_text(s, m, i, 1 + rm_pick(s, 40), 1);
        i->soff -= (m->buf[i->soff - 1] == '-');
        i->slen = m->len - i->soff;
        break;
    case ':': {
        static const int64_t edge[] = { 0, 1, -1, INT64_MAX, INT64_MIN, 9, 10, -10 };
        int64_t v = rm_pick(s, 2) ? edge[rm_pick(s, 8)] : (int64_t)((uint64_t)rm_pick(s, 65536) << rm_pick(s, 48)) - (int64_t)rm_pick(s, 3);
        if (v >= 0 && rm_pick(s, 4) == 0)
            rm_puts(m, "+");
        rm_dec(m, v);
        i->ival = v;
        break;
    }
    case '_':
        i->isnull = 1;
        break;
    case ',':
        k = rm_pick(s, sizeof RM_DBL / sizeof *RM_DBL);
        i->soff = m->len;
        rm_puts(m, RM_DBL[k]);
        i->slen = strlen(RM_DBL[k]);
        i->dval = k == 4 ? NAN : RM_DBLV[k];
        break;
    case '#':
        i->ival = (int64_t)rm_pick(s, 2);
        rm_puts(m, i->ival ? "t" : "f");
        break;
    case '$':
    case '!':
    case '=':
        if (t == '$' && rm_pick(s, 8) == 0) {
            rm_puts(m, "-1");
            i->isnull = 1;
            break;
        }
        n = rm_pick(s, 4) ? rm_pick(s, 20) : rm_pick(s, 2000);
        rm_dec(m, (int64_t)n);
        rm_puts(m, "\r\n");
        i->soff = m->len;
        for (k = 0; k < n; k++) {
            uint8_t c = rm_pick(s, 3) ? (uint8_t)"\r\nab"[rm_pick(s, 4)] : (uint8_t)rm_pick(s, 256);
            rm_put(m, &c, 1);
        }
        i->slen = i->blen = n;
        break;
    default:
        if (t == '*' && !deep && rm_pick(s, 10) == 0) {
            rm_puts(m, "-1");
            i->isnull = 1;
            break;
        }
        n = *budget <= 0 ? 0 : deep ? 1 + rm_pick(s, 2) : rm_pick(s, 5);
        rm_dec(m, (int64_t)n);
        i->count = (int64_t)n;
        i->eff = (t == '%' || t == '|') ? 2 * (int64_t)n : (int64_t)n;
        rm_puts(m, "\r\n");
        for (k = 0; k < (size_t)i->eff; k++)
            rm_gen_value(s, m, depth + 1, budget, deep && k == 0 && deep > 1 ? deep - 1 : 0);
        if (t == '|')
            rm_gen_value(s, m, depth + 1, budget, deep && i->eff == 0 && deep > 1 ? deep - 1 : 0);
        return;
    }
    rm_puts(m, "\r\n");
}

static void rm_gen(rm_src_t* s, rm_msg_t* m)
{
    int deep = rm_pick(s, 8) == 0 ? 25 + (int)rm_pick(s, 12) : 0, budget = deep ? 400 : 1 + (int)rm_pick(s, 60);
    m->len = m->nit = 0;
    m->full = 0;
    rm_gen_value(s, m, 0, &budget, deep);
    m->tail = rm_pick(s, 3);
    memcpy(m->buf + m->len, "+tail-no-crlf" + 0, m->tail ? 13 : 0);
    m->tail = m->tail ? 13 : 0;
}

static int rm_expect(const rm_msg_t* m, size_t maxbulk)
{
    size_t k;
    size_t mb = maxbulk ? maxbulk : 64u * 1024u * 1024u;
    for (k = 0; k < m->nit; k++) {
        const rm_item_t* i = &m->it[k];
        int agg = strchr("*~>%|", i->type) != NULL && !i->isnull;
        if (strchr("$!=", i->type) && !i->isnull && i->blen > mb)
            return DYN_RESP_E_TOOBIG;
        if (agg && (uint64_t)i->eff > (uint64_t)(mb / 3))
            return DYN_RESP_E_COUNT;
        if (agg && (i->eff > 0 || i->type == '|') && i->depth + 1 >= DYN_RESP_MAX_DEPTH)
            return DYN_RESP_E_DEPTH;
    }
    return DYN_RESP_OK;
}

static int rm_resume(const uint8_t* b, size_t len, size_t maxbulk, rm_src_t* s, size_t* used)
{
    dyn_resp_scan_t st;
    size_t at = 0;
    int rc = DYN_RESP_INCOMPLETE;
    dyn_resp_scan_init(&st);
    while (rc == DYN_RESP_INCOMPLETE) {
        at += 1 + rm_pick(s, len < 128 && rm_pick(s, 2) ? 4 : 64 + (uint32_t)(len / 16));
        if (at > len)
            at = len;
        rc = dyn_resp_scan_resume(&st, b, at, maxbulk, used);
        if (at == len)
            break;
    }
    return rc;
}

#define RM_SCAP (RM_CAP * 4 + 256)

static int rm_sout(uint8_t* o, size_t cap, size_t* w, const void* src, size_t len)
{
    if (*w + len > cap)
        return -1;
    memcpy(o + *w, src, len);
    *w += len;
    return 0;
}

static int rm_sfy(rm_src_t* s, const uint8_t* b, size_t n, size_t* p, uint8_t* o, size_t cap, size_t* w)
{
    size_t q = *p, e = q + 1, j, k, cnt;
    long long v = 0;
    int neg = 0, plain = 0, stream;
    uint8_t t;
    char num[40];
    if (q >= n)
        return -1;
    t = b[q];
    while (e < n && b[e] != '\r')
        e++;
    if (e + 1 >= n)
        return -1;
    if (t && strchr("$!=*~%|>", t)) {
        j = q + 1;
        if (j < e && b[j] == '-') {
            neg = 1;
            j++;
        }
        for (; j < e; j++) {
            if (b[j] < '0' || b[j] > '9' || j - q > 18)
                return -1;
            v = v * 10 + (b[j] - '0');
        }
        if (neg)
            v = -v;
        snprintf(num, sizeof num, "%lld", v);
        plain = strlen(num) == e - q - 1 && !memcmp(num, b + q + 1, e - q - 1);
    }
    if (t && strchr("$!=", t) && v >= 0) {
        size_t end = e + 2 + (size_t)v + 2, at = e + 2, left = (size_t)v;
        if (end > n)
            return -1;
        *p = end;
        if (!(t == '$' && plain && rm_pick(s, 2)))
            return rm_sout(o, cap, w, b + q, end - q);
        if (rm_sout(o, cap, w, "$?\r\n", 4))
            return -1;
        while (left) {
            size_t c = rm_pick(s, 4) ? 1 + rm_pick(s, (uint32_t)(left < 64 ? left : 64)) : left;
            snprintf(num, sizeof num, ";%zu\r\n", c);
            if (rm_sout(o, cap, w, num, strlen(num)) || rm_sout(o, cap, w, b + at, c) || rm_sout(o, cap, w, "\r\n", 2))
                return -1;
            at += c;
            left -= c;
        }
        return rm_sout(o, cap, w, ";0\r\n", 4);
    }
    if (t && strchr("*~%|>", t) && v >= 0) {
        char hd[4] = { (char)t, '?', '\r', '\n' };
        cnt = (size_t)v * (t == '%' || t == '|' ? 2 : 1);
        stream = t != '>' && plain && rm_pick(s, 2);
        *p = e + 2;
        if (stream ? rm_sout(o, cap, w, hd, 4) : rm_sout(o, cap, w, b + q, e + 2 - q))
            return -1;
        for (k = 0; k < cnt; k++)
            if (rm_sfy(s, b, n, p, o, cap, w))
                return -1;
        if (stream && rm_sout(o, cap, w, ".\r\n", 3))
            return -1;
        return t == '|' ? rm_sfy(s, b, n, p, o, cap, w) : 0;
    }
    *p = e + 2;
    return rm_sout(o, cap, w, b + q, e + 2 - q);
}

static const char* rm_check_stream(const uint8_t* b, size_t n, rm_src_t* s)
{
    static uint8_t sb[RM_SCAP], cb[RM_SCAP * 2];
    size_t sl = 0, p = 0, used = 7, cl = 0, k;
    dyn_resp_scan_t st;
    int rc;
    if (rm_sfy(s, b, n, &p, sb, sizeof sb, &sl) != 0 || p != n)
        return NULL;
    dyn_resp_scan_init(&st);
    rc = dyn_resp_scan_resume(&st, sb, sl, 0, &used);
    if (rc != DYN_RESP_OK || used != sl)
        return "a streamed re-encoding of a valid message does not scan to its own length";
    if (!st.streamed != (sl == n && !memcmp(sb, b, n)))
        return "the streamed flag differs from whether the re-encoding streamed";
    if (rm_resume(sb, sl, 0, s, &used) != DYN_RESP_OK || used != sl)
        return "resumed scan of a streamed message differs from one-shot";
    for (k = 0; k < 8 && sl > 1; k++) {
        size_t cut = k < 4 ? sl - 1 - (k < sl - 1 ? k : 0) : rm_pick(s, (uint32_t)sl);
        if (dyn_resp_scan(sb, cut, 0, &used) != DYN_RESP_INCOMPLETE)
            return "a proper prefix of a streamed message is not INCOMPLETE";
    }
    if (dyn_resp_canon_cap(sl) > sizeof cb || dyn_resp_canon(sb, sl, cb, sizeof cb, &cl) != DYN_RESP_OK || cl != n || memcmp(cb, b, n))
        return "the canonical form of a streamed message differs from the unstreamed original";
    for (k = 0; k < 4; k++) {
        size_t mb = 1 + rm_pick(s, (uint32_t)(n + 8));
        if ((dyn_resp_scan(sb, sl, mb, &used) == DYN_RESP_OK) != (dyn_resp_scan(b, n, mb, &used) == DYN_RESP_OK))
            return "a maxbulk limit accepts the streamed and the fixed form differently";
    }
    return NULL;
}

static const char* rm_check_tree(rm_msg_t* m, rm_src_t* s)
{
    const char* e0;
    size_t used = 7, k, n = m->len;
    int want = rm_expect(m, 0), rc;
    dyn_resp_reader_t r;
    dyn_resp_item_t it;
    size_t maxb = 0;
    if (m->full)
        return NULL;
    rc = dyn_resp_scan(m->buf, n + m->tail, 0, &used);
    if (rc != want)
        return "scan verdict differs from the model";
    if (rc == DYN_RESP_OK && used != n)
        return "scan consumed differs from the message length";
    if (rc != DYN_RESP_OK && used != 0)
        return "failed scan reported consumed bytes";
    if (rm_resume(m->buf, n + m->tail, 0, s, &used) != want || (want == DYN_RESP_OK && used != n))
        return "resumed scan over growing prefixes differs from one-shot";
    if (want != DYN_RESP_OK)
        return NULL;
    for (k = 0; k < rm_nprefix && n > 1; k++) {
        size_t cut = k < 8 && k < n ? n - 1 - k : rm_pick(s, (uint32_t)n);
        if (dyn_resp_scan(m->buf, cut, 0, &used) != DYN_RESP_INCOMPLETE)
            return "a proper prefix of a valid message is not INCOMPLETE";
    }
    dyn_resp_reader_init(&r, m->buf, n);
    for (k = 0; k < m->nit; k++) {
        const rm_item_t* e = &m->it[k];
        rc = dyn_resp_next(&r, &it);
        if (rc != DYN_RESP_OK || it.type != e->type || it.isnull != e->isnull)
            return "reader item type/null differs from the model";
        if ((e->type == ':' || e->type == '#') && it.ival != e->ival)
            return "reader integer/bool differs from the model";
        if (strchr("*~>%|", e->type) && !e->isnull && it.count != e->count)
            return "reader aggregate count differs from the model";
        if (strchr("+-($!=,", e->type) && !e->isnull && (it.slen != e->slen || (e->slen && memcmp(it.str, m->buf + e->soff, e->slen))))
            return "reader string differs from the model";
        if (e->type == ',' && !(it.dval == e->dval || (isnan(e->dval) && isnan(it.dval))) )
            return "reader double differs from the model";
        if (e->type == ',' && e->dval == 0 && signbit(e->dval) != signbit(it.dval))
            return "reader double sign of zero differs from the model";
        if (e->blen > maxb)
            maxb = e->blen;
    }
    if (r.pos != n)
        return "reader did not end at the message end";
    if ((e0 = rm_check_stream(m->buf, n, s)) != NULL)
        return e0;
    for (k = 0; k < m->nit; k++) {
        const rm_item_t* e = &m->it[(k + rm_pick(s, (uint32_t)m->nit)) % m->nit];
        if (!strchr("$!=", e->type) || e->isnull)
            continue;
        m->buf[e->soff + e->slen] ^= 0x40;
        rc = dyn_resp_scan(m->buf, n, 0, &used);
        m->buf[e->soff + e->slen] ^= 0x40;
        if (rc != DYN_RESP_E_SYNTAX)
            return "a bulk string without its CRLF is not E_SYNTAX";
        break;
    }
    for (k = 0; k < 3; k++) {
        size_t mb = 1 + rm_pick(s, (uint32_t)(maxb + 8));
        if (dyn_resp_scan(m->buf, n, mb, &used) != rm_expect(m, mb))
            return "scan verdict under a maxbulk limit differs from the model";
    }
    return NULL;
}

static const char* rm_check_raw(const uint8_t* b, size_t n, size_t maxbulk, rm_src_t* s)
{
    static uint8_t* cb;
    static size_t cbcap;
    size_t used = 7, u2, k, cl = 0;
    dyn_resp_scan_t st;
    int rc, guard = 0;
    int64_t left = 1;
    dyn_resp_scan_init(&st);
    rc = dyn_resp_scan_resume(&st, b, n, maxbulk, &used);
    dyn_resp_reader_t r;
    dyn_resp_item_t it;
    if (rm_resume(b, n, maxbulk, s, &u2) != rc || (rc == DYN_RESP_OK && u2 != used))
        return "resumed scan over growing prefixes differs from one-shot";
    if (rc != DYN_RESP_OK)
        return rc != DYN_RESP_INCOMPLETE && rc != DYN_RESP_E_TYPE && rc != DYN_RESP_E_SYNTAX && rc != DYN_RESP_E_DEPTH
                && rc != DYN_RESP_E_TOOBIG && rc != DYN_RESP_E_COUNT && rc != DYN_RESP_E_INT
            ? "scan returned an unknown code"
            : used ? "failed scan reported consumed bytes" : NULL;
    if (used == 0 || used > n)
        return "scan consumed out of range";
    if (dyn_resp_scan(b, used, maxbulk, &u2) != DYN_RESP_OK || u2 != used)
        return "the consumed prefix alone does not scan to the same length";
    for (k = 0; k < rm_nprefix && used > 1; k++) {
        size_t cut = k < 4 ? used - 1 - (k < used - 1 ? k : 0) : rm_pick(s, (uint32_t)used);
        if (dyn_resp_scan(b, cut, maxbulk, &u2) != DYN_RESP_INCOMPLETE)
            return "a proper prefix of a scanned message is not INCOMPLETE";
    }
    if (st.streamed) {
        size_t need = dyn_resp_canon_cap(used);
        if (need > cbcap) {
            uint8_t* nb = (uint8_t*)realloc(cb, need);
            if (!nb)
                return NULL;
            cb = nb;
            cbcap = need;
        }
        if (dyn_resp_canon(b, used, cb, cbcap, &cl) != DYN_RESP_OK || cl > used + used / 1024 + 16)
            return "canon failed or grew past n + n/1024 + 16 on a message the scanner accepted";
        dyn_resp_scan_init(&st);
        if (dyn_resp_scan_resume(&st, cb, cl, maxbulk, &u2) != DYN_RESP_OK || u2 != cl || st.streamed)
            return "the canonical form of an accepted streamed message does not scan as one fixed message";
        b = cb;
        used = cl;
    }
    dyn_resp_reader_init(&r, b, used);
    while (left > 0 && guard++ < 100000) {
        size_t at = r.pos;
        rc = dyn_resp_next(&r, &it);
        if (rc != DYN_RESP_OK && !(rc == DYN_RESP_E_SYNTAX && it.type == ','))
            return "reader failed inside a message the scanner accepted";
        if (r.pos <= at || r.pos > used)
            return "reader position out of range";
        if (it.str && (it.str < b || it.str + it.slen > b + used))
            return "reader string out of bounds";
        left--;
        if (strchr("*~>%|", it.type) && !it.isnull)
            left += (it.type == '%' || it.type == '|') ? 2 * it.count + (it.type == '|') : it.count;
    }
    if (left != 0 || r.pos != used)
        return "reader item walk does not end where the scanner did";
    return NULL;
}

static const char* rm_check_cmd(rm_src_t* s)
{
    static uint8_t data[8][600], out[8 * 700];
    const char* argv[8];
    size_t lens[8], need = 0, used = 0, w, k;
    int argc = 1 + (int)rm_pick(s, 8), rc;
    dyn_resp_reader_t r;
    dyn_resp_item_t it;
    for (k = 0; k < (size_t)argc; k++) {
        size_t j;
        lens[k] = rm_pick(s, 3) ? rm_pick(s, 12) : rm_pick(s, 600);
        for (j = 0; j < lens[k]; j++)
            data[k][j] = (uint8_t)(rm_pick(s, 2) ? "\r\n$*0"[rm_pick(s, 5)] : rm_pick(s, 256));
        argv[k] = (const char*)data[k];
    }
    w = dyn_resp_cmd_size(argc, argv, lens);
    if (dyn_resp_cmd_encode(out, w - 1, argc, argv, lens, &need) != DYN_RESP_E_TOOBIG || need != w)
        return "cmd_encode accepted a buffer one byte short or misreported need";
    rc = dyn_resp_cmd_encode(out, w, argc, argv, lens, &need);
    if (rc < 0 || (size_t)rc != w)
        return "cmd_encode length differs from cmd_size";
    if (dyn_resp_scan(out, w, 0, &used) != DYN_RESP_OK || used != w)
        return "cmd_encode output does not scan as one message";
    dyn_resp_reader_init(&r, out, w);
    if (dyn_resp_next(&r, &it) != DYN_RESP_OK || it.type != '*' || it.count != argc)
        return "cmd_encode header differs";
    for (k = 0; k < (size_t)argc; k++)
        if (dyn_resp_next(&r, &it) != DYN_RESP_OK || it.type != '$' || it.slen != lens[k] || (lens[k] && memcmp(it.str, data[k], lens[k])))
            return "cmd_encode argument does not round-trip";
    return NULL;
}

#endif
