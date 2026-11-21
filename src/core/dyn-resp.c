#include "dyn-resp.h"
#include "dtoa.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define RESP_DEFAULT_MAXBULK (64u * 1024u * 1024u)
#define RESP_MIN_VALUE_LEN 3

const char* dyn_resp_strerror(int code)
{
    switch (code) {
    case DYN_RESP_OK:
        return "ok";
    case DYN_RESP_INCOMPLETE:
        return "reply incomplete";
    case DYN_RESP_E_TYPE:
        return "unknown RESP type byte";
    case DYN_RESP_E_SYNTAX:
        return "malformed RESP: bad number or terminator";
    case DYN_RESP_E_DEPTH:
        return "RESP nesting past its limit";
    case DYN_RESP_E_TOOBIG:
        return "RESP length past its limit";
    case DYN_RESP_E_INT:
        return "RESP integer out of range";
    case DYN_RESP_E_COUNT:
        return "RESP element count exceeds the message";
    default:
        return "unknown error";
    }
}

int dyn_resp_looks_like_tls(const uint8_t* buf, size_t len)
{
    return len >= 3 && (buf[0] == 0x16 || buf[0] == 0x15) && buf[1] == 0x03 && buf[2] <= 0x04;
}

static int resp_line(const uint8_t* b, size_t len, size_t pos, size_t* end)
{
    size_t i, stop = pos + DYN_RESP_MAX_LINE;
    if (stop > len)
        stop = len;
    for (i = pos; i < stop; i++) {
        if (b[i] == '\n')
            return DYN_RESP_E_SYNTAX;
        if (b[i] == '\r') {
            if (i + 1 >= len)
                return DYN_RESP_INCOMPLETE;
            if (b[i + 1] != '\n')
                return DYN_RESP_E_SYNTAX;
            *end = i;
            return DYN_RESP_OK;
        }
    }
    return stop < len ? DYN_RESP_E_TOOBIG : DYN_RESP_INCOMPLETE;
}

static int resp_int(const uint8_t* b, size_t s, size_t e, int64_t* out)
{
    int neg = 0;
    size_t i = s;
    int64_t v = 0;

    if (i >= e)
        return DYN_RESP_E_SYNTAX;
    if (b[i] == '-') {
        neg = 1;
        i++;
    } else if (b[i] == '+') {
        i++;
    }
    if (i >= e)
        return DYN_RESP_E_SYNTAX;

    if (neg) {
        for (; i < e; i++) {
            int d;
            if (b[i] < '0' || b[i] > '9')
                return DYN_RESP_E_SYNTAX;
            d = b[i] - '0';
            if (v < INT64_MIN / 10)
                return DYN_RESP_E_INT;
            v *= 10;
            if (v < INT64_MIN + d)
                return DYN_RESP_E_INT;
            v -= d;
        }
    } else {
        for (; i < e; i++) {
            int d;
            if (b[i] < '0' || b[i] > '9')
                return DYN_RESP_E_SYNTAX;
            d = b[i] - '0';
            if (v > (INT64_MAX - d) / 10)
                return DYN_RESP_E_INT;
            v = v * 10 + d;
        }
    }
    *out = v;
    return DYN_RESP_OK;
}

static int resp_one(const uint8_t* b, size_t len, size_t* pos, size_t maxbulk,
    int* type, int64_t* nelem)
{
    size_t p = *pos, e;
    int rc;
    int64_t n;
    uint8_t t;

    if (p >= len)
        return DYN_RESP_INCOMPLETE;
    t = b[p];
    *type = t;
    *nelem = 0;

    switch (t) {
    case DYN_RESP_SIMPLE:
    case DYN_RESP_ERROR:
    case DYN_RESP_INT:
    case DYN_RESP_NULL:
    case DYN_RESP_DOUBLE:
    case DYN_RESP_BOOL:
    case DYN_RESP_BIGNUM:
        rc = resp_line(b, len, p + 1, &e);
        if (rc != DYN_RESP_OK)
            return rc;
        if (t == DYN_RESP_INT) {
            int64_t v;
            rc = resp_int(b, p + 1, e, &v);
            if (rc != DYN_RESP_OK)
                return rc;
        } else if (t == DYN_RESP_NULL) {
            if (e != p + 1)
                return DYN_RESP_E_SYNTAX;
        } else if (t == DYN_RESP_BOOL) {
            if (e != p + 2 || (b[p + 1] != 't' && b[p + 1] != 'f'))
                return DYN_RESP_E_SYNTAX;
        }
        *pos = e + 2;
        return DYN_RESP_OK;

    case DYN_RESP_BULK:
    case DYN_RESP_BLOBERR:
    case DYN_RESP_VERB:
        rc = resp_line(b, len, p + 1, &e);
        if (rc != DYN_RESP_OK)
            return rc;
        if (t == DYN_RESP_BULK && e == p + 2 && b[p + 1] == '?') {
            *nelem = -1;
            *pos = e + 2;
            return DYN_RESP_OK;
        }
        rc = resp_int(b, p + 1, e, &n);
        if (rc != DYN_RESP_OK)
            return rc;
        if (n < 0) {
            if (n != -1 || t != DYN_RESP_BULK)
                return DYN_RESP_E_SYNTAX;
            *pos = e + 2;
            return DYN_RESP_OK;
        }
        if ((uint64_t)n > (uint64_t)maxbulk)
            return DYN_RESP_E_TOOBIG;
        if (e + 2 + (size_t)n + 2 > len)
            return DYN_RESP_INCOMPLETE;
        if (b[e + 2 + (size_t)n] != '\r' || b[e + 3 + (size_t)n] != '\n')
            return DYN_RESP_E_SYNTAX;
        *pos = e + 2 + (size_t)n + 2;
        return DYN_RESP_OK;

    case DYN_RESP_ARRAY:
    case DYN_RESP_SET:
    case DYN_RESP_PUSH:
    case DYN_RESP_MAP:
    case DYN_RESP_ATTR:
        rc = resp_line(b, len, p + 1, &e);
        if (rc != DYN_RESP_OK)
            return rc;
        if (t != DYN_RESP_PUSH && e == p + 2 && b[p + 1] == '?') {
            *nelem = -1;
            *pos = e + 2;
            return DYN_RESP_OK;
        }
        rc = resp_int(b, p + 1, e, &n);
        if (rc != DYN_RESP_OK)
            return rc;
        if (n < 0) {
            if (n != -1 || t != DYN_RESP_ARRAY)
                return DYN_RESP_E_SYNTAX;
            *pos = e + 2;
            return DYN_RESP_OK;
        }
        if (t == DYN_RESP_MAP || t == DYN_RESP_ATTR) {
            if (n > INT64_MAX / 2)
                return DYN_RESP_E_TOOBIG;
            n *= 2;
        }
        if ((uint64_t)n > (uint64_t)(maxbulk / RESP_MIN_VALUE_LEN))
            return DYN_RESP_E_COUNT;
        *nelem = n;
        *pos = e + 2;
        return DYN_RESP_OK;

    default:
        return DYN_RESP_E_TYPE;
    }
}

void dyn_resp_scan_init(dyn_resp_scan_t* st)
{
    memset(st, 0, sizeof(*st));
    st->want[0] = 1;
}

static int resp_scan_fail(dyn_resp_scan_t* st, int rc, size_t* consumed)
{
    if (rc != DYN_RESP_INCOMPLETE)
        dyn_resp_scan_init(st);
    *consumed = 0;
    return rc;
}

static int resp_chunk(const uint8_t* b, size_t len, size_t* pos, size_t maxbulk,
    size_t* acc, int* done)
{
    size_t p = *pos, e;
    int64_t n;
    int rc;

    *done = 0;
    if (p >= len)
        return DYN_RESP_INCOMPLETE;
    if (b[p] != ';')
        return DYN_RESP_E_SYNTAX;
    rc = resp_line(b, len, p + 1, &e);
    if (rc != DYN_RESP_OK)
        return rc;
    rc = resp_int(b, p + 1, e, &n);
    if (rc != DYN_RESP_OK)
        return rc;
    if (n < 0)
        return DYN_RESP_E_SYNTAX;
    if (n == 0) {
        *done = 1;
        *pos = e + 2;
        return DYN_RESP_OK;
    }
    if ((uint64_t)n > (uint64_t)(maxbulk - *acc))
        return DYN_RESP_E_TOOBIG;
    if (e + 2 + (size_t)n + 2 > len)
        return DYN_RESP_INCOMPLETE;
    if (b[e + 2 + (size_t)n] != '\r' || b[e + 3 + (size_t)n] != '\n')
        return DYN_RESP_E_SYNTAX;
    *acc += (size_t)n;
    *pos = e + 4 + (size_t)n;
    return DYN_RESP_OK;
}

int dyn_resp_scan_resume(dyn_resp_scan_t* st, const uint8_t* buf, size_t len,
    size_t maxbulk, size_t* consumed)
{
    int rc, type;
    int64_t n;

    if (!st || !buf || !consumed)
        return DYN_RESP_E_SYNTAX;
    if (maxbulk == 0)
        maxbulk = RESP_DEFAULT_MAXBULK;

    if (st->pos == 0)
        st->streamed = 0;
    for (;;) {
        uint32_t bit;
        size_t hdr;
        if (st->chunk) {
            int done;
            rc = resp_chunk(buf, len, &st->pos, maxbulk, &st->sbytes, &done);
            if (rc != DYN_RESP_OK)
                return resp_scan_fail(st, rc, consumed);
            if (done) {
                st->chunk = 0;
                st->sbytes = 0;
            }
            continue;
        }
        while (st->sp >= 0 && st->want[st->sp] == 0)
            st->sp--;
        if (st->sp < 0)
            break;
        bit = 1u << st->sp;
        if (st->want[st->sp] < 0 && st->pos < len && buf[st->pos] == '.') {
            size_t e;
            rc = resp_line(buf, len, st->pos + 1, &e);
            if (rc == DYN_RESP_OK && (e != st->pos + 1 || ((st->pair & bit) && (-(st->want[st->sp] + 1) & 1))))
                rc = DYN_RESP_E_SYNTAX;
            if (rc != DYN_RESP_OK)
                return resp_scan_fail(st, rc, consumed);
            st->want[st->sp] = (st->attr & bit) ? 1 : 0;
            st->pair &= ~bit;
            st->attr &= ~bit;
            st->pos = e + 2;
            continue;
        }
        hdr = st->pos;
        rc = resp_one(buf, len, &st->pos, maxbulk, &type, &n);
        if (rc != DYN_RESP_OK)
            return resp_scan_fail(st, rc, consumed);
        st->want[st->sp]--;
        if (st->want[st->sp] < 0 && (uint64_t)(-(st->want[st->sp] + 1)) > (uint64_t)(maxbulk / RESP_MIN_VALUE_LEN))
            return resp_scan_fail(st, DYN_RESP_E_COUNT, consumed);
        if (n < 0) {
            st->streamed = 1;
            if (type == DYN_RESP_BULK) {
                st->chunk = 1;
                st->sbytes = 0;
                continue;
            }
            if (st->sp + 1 >= DYN_RESP_MAX_DEPTH) {
                size_t q = st->pos, have = len - q < 3 ? len - q : 3;
                if (type == DYN_RESP_ATTR || memcmp(buf + q, ".\r\n", have) != 0)
                    return resp_scan_fail(st, DYN_RESP_E_DEPTH, consumed);
                if (have < 3) {
                    st->pos = hdr;
                    st->want[st->sp]++;
                    return resp_scan_fail(st, DYN_RESP_INCOMPLETE, consumed);
                }
                st->pos = q + 3;
                continue;
            }
            st->want[++st->sp] = -1;
            bit = 1u << st->sp;
            st->pair = (st->pair & ~bit) | ((type == DYN_RESP_MAP || type == DYN_RESP_ATTR) ? bit : 0);
            st->attr = (st->attr & ~bit) | (type == DYN_RESP_ATTR ? bit : 0);
            continue;
        }
        if (type == DYN_RESP_ATTR)
            n += 1;
        if (n > 0) {
            if (st->sp + 1 >= DYN_RESP_MAX_DEPTH)
                return resp_scan_fail(st, DYN_RESP_E_DEPTH, consumed);
            st->want[++st->sp] = n;
        }
    }
    *consumed = st->pos;
    {
        uint8_t streamed = st->streamed;
        dyn_resp_scan_init(st);
        st->streamed = streamed;
    }
    return DYN_RESP_OK;
}

int dyn_resp_scan(const uint8_t* buf, size_t len, size_t maxbulk,
    size_t* consumed)
{
    dyn_resp_scan_t st;

    dyn_resp_scan_init(&st);
    return dyn_resp_scan_resume(&st, buf, len, maxbulk, consumed);
}

void dyn_resp_reader_init(dyn_resp_reader_t* r, const uint8_t* buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

static int dyn_resp_parse_double(const uint8_t* p, size_t n, double* out)
{
    char stackbuf[64], *s = stackbuf, *heap = NULL;
    const char* end = NULL;
    JSATODTempMem tmp;
    double v;

    if (n == 0)
        return DYN_RESP_E_SYNTAX;
    if (n + 1 > sizeof(stackbuf)) {
        heap = (char*)malloc(n + 1);
        if (!heap)
            return DYN_RESP_E_SYNTAX;
        s = heap;
    }
    memcpy(s, p, n);
    s[n] = '\0';
    v = js_atod(s, &end, 10, 0, &tmp);
    if (!end || end != s + n || v != v) {
        free(heap);
        return DYN_RESP_E_SYNTAX;
    }
    free(heap);
    *out = v;
    return DYN_RESP_OK;
}

int dyn_resp_next(dyn_resp_reader_t* r, dyn_resp_item_t* it)
{
    const uint8_t* b = r->buf;
    size_t p = r->pos, e;
    int rc;
    uint8_t t;

    memset(it, 0, sizeof(*it));
    if (p >= r->len)
        return DYN_RESP_INCOMPLETE;
    t = b[p];
    it->type = t;

    rc = resp_line(b, r->len, p + 1, &e);
    if (rc != DYN_RESP_OK)
        return rc;
    it->str = b + p + 1;
    it->slen = e - (p + 1);
    r->pos = e + 2;

    switch (t) {
    case DYN_RESP_SIMPLE:
    case DYN_RESP_ERROR:
    case DYN_RESP_BIGNUM:
        return DYN_RESP_OK;
    case DYN_RESP_NULL:
        it->isnull = 1;
        it->str = NULL;
        it->slen = 0;
        return DYN_RESP_OK;
    case DYN_RESP_BOOL:
        it->ival = (it->slen == 1 && it->str[0] == 't');
        return DYN_RESP_OK;
    case DYN_RESP_INT:
        return resp_int(b, p + 1, e, &it->ival);
    case DYN_RESP_DOUBLE: {
        if (it->slen == 3 && memcmp(it->str, "inf", 3) == 0) {
            it->dval = 1e308 * 10;
            return DYN_RESP_OK;
        }
        if (it->slen == 4 && memcmp(it->str, "-inf", 4) == 0) {
            it->dval = -(1e308 * 10);
            return DYN_RESP_OK;
        }
        if (it->slen == 3 && memcmp(it->str, "nan", 3) == 0) {
            it->dval = (1e308 * 10) - (1e308 * 10);
            return DYN_RESP_OK;
        }
        if (it->slen == 0)
            return DYN_RESP_E_SYNTAX;
        return dyn_resp_parse_double(it->str, it->slen, &it->dval);
    }
    case DYN_RESP_BULK:
    case DYN_RESP_BLOBERR:
    case DYN_RESP_VERB: {
        int64_t n;
        rc = resp_int(b, p + 1, e, &n);
        if (rc != DYN_RESP_OK)
            return rc;
        if (n < 0) {
            it->isnull = 1;
            it->str = NULL;
            it->slen = 0;
            return DYN_RESP_OK;
        }
        if (e + 2 + (size_t)n + 2 > r->len)
            return DYN_RESP_INCOMPLETE;
        it->str = b + e + 2;
        it->slen = (size_t)n;
        r->pos = e + 2 + (size_t)n + 2;
        return DYN_RESP_OK;
    }
    case DYN_RESP_ARRAY:
    case DYN_RESP_SET:
    case DYN_RESP_PUSH:
    case DYN_RESP_MAP:
    case DYN_RESP_ATTR: {
        int64_t n;
        rc = resp_int(b, p + 1, e, &n);
        if (rc != DYN_RESP_OK)
            return rc;
        it->str = NULL;
        it->slen = 0;
        if (n < 0) {
            it->isnull = 1;
            it->count = 0;
            return DYN_RESP_OK;
        }
        it->count = n;
        return DYN_RESP_OK;
    }
    default:
        return DYN_RESP_E_TYPE;
    }
}

static size_t resp_declen(size_t v)
{
    size_t n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

size_t dyn_resp_cmd_size(int argc, const char* const* argv, const size_t* lens)
{
    size_t total = 1 + resp_declen((size_t)argc) + 2;
    int i;
    for (i = 0; i < argc; i++) {
        size_t l = (lens && lens[i] != (size_t)-1) ? lens[i] : strlen(argv[i]);
        total += 1 + resp_declen(l) + 2 + l + 2;
    }
    return total;
}

static size_t resp_put_dec(uint8_t* out, size_t v)
{
    char tmp[24];
    size_t n = 0, i;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)tmp[n - 1 - i];
    return n;
}

int dyn_resp_cmd_encode(uint8_t* out, size_t outcap, int argc,
    const char* const* argv, const size_t* lens,
    size_t* need)
{
    size_t total, w = 0;
    int i;

    if (!out || !argv || argc <= 0)
        return DYN_RESP_E_SYNTAX;
    total = dyn_resp_cmd_size(argc, argv, lens);
    if (need)
        *need = total;
    if (total > outcap)
        return DYN_RESP_E_TOOBIG;

    out[w++] = '*';
    w += resp_put_dec(out + w, (size_t)argc);
    out[w++] = '\r';
    out[w++] = '\n';
    for (i = 0; i < argc; i++) {
        size_t l = (lens && lens[i] != (size_t)-1) ? lens[i] : strlen(argv[i]);
        out[w++] = '$';
        w += resp_put_dec(out + w, l);
        out[w++] = '\r';
        out[w++] = '\n';
        if (l)
            memcpy(out + w, argv[i], l);
        w += l;
        out[w++] = '\r';
        out[w++] = '\n';
    }
    return (int)w;
}

size_t dyn_resp_canon_cap(size_t used)
{
    if (used > SIZE_MAX / 4)
        return 0;
    return ((used + used / 1024 + 51) & ~(size_t)3) + 4 * (used / 7 + 1);
}

static size_t resp_eol(const uint8_t* b, size_t n, size_t p)
{
    const uint8_t* r = p < n ? (const uint8_t*)memchr(b + p, '\r', n - p) : NULL;
    return r ? (size_t)(r - b) : n;
}

static int resp_is_stream(const uint8_t* b, size_t p, size_t e)
{
    uint8_t t = b[p];
    return e == p + 2 && b[p + 1] == '?' && (t == '$' || t == '*' || t == '~' || t == '%' || t == '|');
}

static int resp_is_blob(uint8_t t)
{
    return t == '$' || t == '!' || t == '=';
}

static int resp_is_agg(uint8_t t)
{
    return t == '*' || t == '~' || t == '%' || t == '|' || t == '>';
}

static int resp_canon_chunks(const uint8_t* in, size_t n, size_t* pos, uint8_t* out, size_t* w, uint64_t* sum)
{
    size_t p = *pos, e;
    int64_t m;
    for (;;) {
        e = resp_eol(in, n, p);
        if (e + 2 > n || in[p] != ';' || resp_int(in, p + 1, e, &m) != DYN_RESP_OK || m < 0)
            return DYN_RESP_E_SYNTAX;
        p = e + 2;
        if (!m)
            break;
        if ((uint64_t)m + 2 > (uint64_t)(n - p))
            return DYN_RESP_E_SYNTAX;
        if (out)
            memcpy(out + *w, in + p, (size_t)m);
        if (w)
            *w += (size_t)m;
        if (sum)
            *sum += (uint64_t)m;
        p += (size_t)m + 2;
    }
    *pos = p;
    return DYN_RESP_OK;
}

static int resp_canon_count(const uint8_t* in, size_t n, uint32_t* cnt, size_t maxc)
{
    int64_t want[DYN_RESP_MAX_DEPTH + 1], m;
    size_t at[DYN_RESP_MAX_DEPTH + 1], ncnt = 0, p = 0, e;
    uint64_t attrm = 0;
    int sp = 0;

    want[0] = 1;
    at[0] = 0;
    while (p < n) {
        uint8_t t;
        while (sp >= 0 && want[sp] == 0)
            sp--;
        if (sp < 0)
            break;
        t = in[p];
        e = resp_eol(in, n, p);
        if (e + 2 > n)
            return DYN_RESP_E_SYNTAX;
        if (t == '.') {
            if (want[sp] >= 0 || e != p + 1)
                return DYN_RESP_E_SYNTAX;
            want[sp] = (int64_t)((attrm >> sp) & 1);
            attrm &= ~((uint64_t)1 << sp);
            p = e + 2;
            continue;
        }
        if (want[sp] > 0)
            want[sp]--;
        else if (cnt[at[sp]] == UINT32_MAX)
            return DYN_RESP_E_TOOBIG;
        else
            cnt[at[sp]]++;
        if (resp_is_stream(in, p, e)) {
            if (ncnt >= maxc)
                return DYN_RESP_E_TOOBIG;
            cnt[ncnt] = 0;
            p = e + 2;
            if (t == '$') {
                uint64_t sum = 0;
                if (resp_canon_chunks(in, n, &p, NULL, NULL, &sum) != DYN_RESP_OK)
                    return DYN_RESP_E_SYNTAX;
                if (sum > UINT32_MAX)
                    return DYN_RESP_E_TOOBIG;
                cnt[ncnt++] = (uint32_t)sum;
                continue;
            }
            if (sp + 1 > DYN_RESP_MAX_DEPTH)
                return DYN_RESP_E_DEPTH;
            want[++sp] = -1;
            at[sp] = ncnt++;
            attrm = (attrm & ~((uint64_t)1 << sp)) | ((uint64_t)(t == '|') << sp);
            continue;
        }
        if (resp_is_blob(t) || resp_is_agg(t)) {
            if (resp_int(in, p + 1, e, &m) != DYN_RESP_OK)
                return DYN_RESP_E_SYNTAX;
        } else {
            m = -1;
        }
        p = e + 2;
        if (resp_is_blob(t) && m >= 0) {
            if ((uint64_t)m + 2 > (uint64_t)(n - p))
                return DYN_RESP_E_SYNTAX;
            p += (size_t)m + 2;
        } else if (resp_is_agg(t) && (m > 0 || t == '|')) {
            if (m < 0 || m > INT64_MAX / 4)
                return DYN_RESP_E_SYNTAX;
            if (t == '%' || t == '|')
                m *= 2;
            if (t == '|')
                m++;
            if (sp + 1 > DYN_RESP_MAX_DEPTH)
                return DYN_RESP_E_DEPTH;
            want[++sp] = m;
            attrm &= ~((uint64_t)1 << sp);
        }
    }
    while (sp >= 0 && want[sp] == 0)
        sp--;
    return p == n && sp < 0 ? DYN_RESP_OK : DYN_RESP_E_SYNTAX;
}

int dyn_resp_canon(const uint8_t* in, size_t n, uint8_t* out, size_t cap, size_t* outlen)
{
    uint32_t* cnt;
    size_t p = 0, w = 0, e, i = 0, need = dyn_resp_canon_cap(n), lim = (n + n / 1024 + 51) & ~(size_t)3;
    int64_t m;
    int rc;

    if (!in || !out || !outlen || n == 0 || need == 0 || cap < need)
        return DYN_RESP_E_SYNTAX;
    cnt = (uint32_t*)(void*)(out + lim);
    rc = resp_canon_count(in, n, cnt, n / 7 + 1);
    if (rc != DYN_RESP_OK)
        return rc;
    while (p < n) {
        uint8_t t = in[p];
        size_t s = p;
        e = resp_eol(in, n, p);
        if (t == '.') {
            p = e + 2;
            continue;
        }
        if (resp_is_stream(in, p, e)) {
            uint64_t k = cnt[i++];
            if (w + 24 > lim || (t == '$' && k + 2 > lim - w - 24))
                return DYN_RESP_E_TOOBIG;
            out[w++] = t;
            if (t == '%' || t == '|')
                k /= 2;
            w += resp_put_dec(out + w, (size_t)k);
            out[w++] = '\r';
            out[w++] = '\n';
            p = e + 2;
            if (t == '$') {
                if (resp_canon_chunks(in, n, &p, out, &w, NULL) != DYN_RESP_OK)
                    return DYN_RESP_E_SYNTAX;
                out[w++] = '\r';
                out[w++] = '\n';
            }
            continue;
        }
        p = e + 2;
        if (resp_is_blob(t) && resp_int(in, s + 1, e, &m) == DYN_RESP_OK && m >= 0)
            p += (size_t)m + 2;
        if (p > n || p - s > lim - w)
            return DYN_RESP_E_TOOBIG;
        memcpy(out + w, in + s, p - s);
        w += p - s;
    }
    *outlen = w;
    return DYN_RESP_OK;
}
