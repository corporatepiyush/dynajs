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

int dyn_resp_scan_resume(dyn_resp_scan_t* st, const uint8_t* buf, size_t len,
    size_t maxbulk, size_t* consumed)
{
    int rc, type;
    int64_t n;

    if (!st || !buf || !consumed)
        return DYN_RESP_E_SYNTAX;
    if (maxbulk == 0)
        maxbulk = RESP_DEFAULT_MAXBULK;

    for (;;) {
        while (st->sp >= 0 && st->want[st->sp] == 0)
            st->sp--;
        if (st->sp < 0)
            break;
        rc = resp_one(buf, len, &st->pos, maxbulk, &type, &n);
        if (rc != DYN_RESP_OK) {
            if (rc != DYN_RESP_INCOMPLETE)
                dyn_resp_scan_init(st);
            *consumed = 0;
            return rc;
        }
        st->want[st->sp]--;
        if (type == DYN_RESP_ATTR)
            n += 1;
        if (n > 0) {
            if (st->sp + 1 >= DYN_RESP_MAX_DEPTH) {
                dyn_resp_scan_init(st);
                *consumed = 0;
                return DYN_RESP_E_DEPTH;
            }
            st->want[++st->sp] = n;
        }
    }
    *consumed = st->pos;
    dyn_resp_scan_init(st);
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
