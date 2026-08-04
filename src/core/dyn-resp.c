/* dyn-resp -- see dyn-resp.h. */
#include "dyn-resp.h"
/* js_atod: a correctly-rounded, locale-free decimal parse. dtoa is standalone
 * pure C -- zero engine identifiers -- so it is allowlisted by core-purity. */
#include "dtoa.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Redis's own proto-max-bulk-len defaults to 512 MiB; a client has no reason
 * to be that permissive about what it will buffer for one reply. */
#define RESP_DEFAULT_MAXBULK  (64u * 1024u * 1024u)
/* The smallest complete value is three octets, "_\r\n". Element counts are
 * checked against the bytes that remain divided by this, which bounds an
 * allocation bomb without inventing a limit. */
#define RESP_MIN_VALUE_LEN    3

const char *dyn_resp_strerror(int code)
{
    switch (code) {
    case DYN_RESP_OK:         return "ok";
    case DYN_RESP_INCOMPLETE: return "reply incomplete";
    case DYN_RESP_E_TYPE:     return "unknown RESP type byte";
    case DYN_RESP_E_SYNTAX:   return "malformed RESP: bad number or terminator";
    case DYN_RESP_E_DEPTH:    return "RESP nesting past its limit";
    case DYN_RESP_E_TOOBIG:   return "RESP length past its limit";
    case DYN_RESP_E_COUNT:    return "RESP element count exceeds the message";
    default:                  return "unknown error";
    }
}

int dyn_resp_looks_like_tls(const uint8_t *buf, size_t len)
{
    /* content type handshake/alert, then TLS 1.x (SSL 3.0 shares the major). */
    return len >= 3 && (buf[0] == 0x16 || buf[0] == 0x15) &&
           buf[1] == 0x03 && buf[2] <= 0x04;
}

/* Locate the CRLF that ends the line starting at `pos`; `*end` is the '\r'. A
 * bare LF is refused: accepting it would let one reply be read as two. */
static int resp_line(const uint8_t *b, size_t len, size_t pos, size_t *end)
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

static int resp_int(const uint8_t *b, size_t s, size_t e, int64_t *out)
{
    int neg = 0;
    size_t i = s;
    int64_t v = 0;

    if (i >= e)
        return DYN_RESP_E_SYNTAX;
    if (b[i] == '-') { neg = 1; i++; }
    else if (b[i] == '+') { i++; }
    if (i >= e)
        return DYN_RESP_E_SYNTAX;
    for (; i < e; i++) {
        int d;
        if (b[i] < '0' || b[i] > '9')
            return DYN_RESP_E_SYNTAX;
        d = b[i] - '0';
        if (v > (INT64_MAX - d) / 10)
            return DYN_RESP_E_TOOBIG;
        v = v * 10 + d;
    }
    *out = neg ? -v : v;
    return DYN_RESP_OK;
}

/* Parse one value header at `*pos`, advancing past it and past any inline
 * payload. `*nelem` is how many values must follow (0 for a scalar). */
static int resp_one(const uint8_t *b, size_t len, size_t *pos, size_t maxbulk,
                    int *type, int64_t *nelem)
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
    case DYN_RESP_SIMPLE: case DYN_RESP_ERROR: case DYN_RESP_INT:
    case DYN_RESP_NULL:   case DYN_RESP_DOUBLE: case DYN_RESP_BOOL:
    case DYN_RESP_BIGNUM:
        rc = resp_line(b, len, p + 1, &e);
        if (rc != DYN_RESP_OK)
            return rc;
        if (t == DYN_RESP_INT) {                 /* must be a number */
            int64_t v;
            rc = resp_int(b, p + 1, e, &v);
            if (rc != DYN_RESP_OK)
                return rc;
        } else if (t == DYN_RESP_NULL) {
            if (e != p + 1)
                return DYN_RESP_E_SYNTAX;        /* "_\r\n" carries no payload */
        } else if (t == DYN_RESP_BOOL) {
            if (e != p + 2 || (b[p + 1] != 't' && b[p + 1] != 'f'))
                return DYN_RESP_E_SYNTAX;
        }
        *pos = e + 2;
        return DYN_RESP_OK;

    case DYN_RESP_BULK: case DYN_RESP_BLOBERR: case DYN_RESP_VERB:
        rc = resp_line(b, len, p + 1, &e);
        if (rc != DYN_RESP_OK)
            return rc;
        rc = resp_int(b, p + 1, e, &n);
        if (rc != DYN_RESP_OK)
            return rc;
        if (n < 0) {
            /* -1 is RESP2's null bulk string. Any other negative is a length
             * the protocol cannot express and is refused rather than clamped. */
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

    case DYN_RESP_ARRAY: case DYN_RESP_SET: case DYN_RESP_PUSH:
    case DYN_RESP_MAP:   case DYN_RESP_ATTR:
        rc = resp_line(b, len, p + 1, &e);
        if (rc != DYN_RESP_OK)
            return rc;
        rc = resp_int(b, p + 1, e, &n);
        if (rc != DYN_RESP_OK)
            return rc;
        if (n < 0) {
            if (n != -1 || t != DYN_RESP_ARRAY)
                return DYN_RESP_E_SYNTAX;        /* only *-1 is a null array */
            *pos = e + 2;
            return DYN_RESP_OK;
        }
        if (t == DYN_RESP_MAP || t == DYN_RESP_ATTR) {
            if (n > INT64_MAX / 2)
                return DYN_RESP_E_TOOBIG;
            n *= 2;                              /* the count is of PAIRS */
        }
        /* Bound the count against the CAP, not against what has arrived: a
         * scanner sees partial replies, so "*2\r\n" with nothing after it is
         * incomplete, not a lie. Above cap/3 the reply can never complete
         * whatever arrives, so it is refused at the header. */
        if ((uint64_t)n > (uint64_t)(maxbulk / RESP_MIN_VALUE_LEN))
            return DYN_RESP_E_COUNT;
        *nelem = n;
        *pos = e + 2;
        return DYN_RESP_OK;

    default:
        return DYN_RESP_E_TYPE;
    }
}

int dyn_resp_scan(const uint8_t *buf, size_t len, size_t maxbulk,
                  size_t *consumed)
{
    int64_t want[DYN_RESP_MAX_DEPTH];
    int sp = 0, rc, type;
    size_t pos = 0;

    if (!buf || !consumed)
        return DYN_RESP_E_SYNTAX;
    if (maxbulk == 0)
        maxbulk = RESP_DEFAULT_MAXBULK;
    want[0] = 1;                       /* one value at the top level */

    for (;;) {
        int64_t n;
        while (sp >= 0 && want[sp] == 0)
            sp--;
        if (sp < 0)
            break;
        want[sp]--;
        rc = resp_one(buf, len, &pos, maxbulk, &type, &n);
        if (rc != DYN_RESP_OK)
            return rc;
        /* An attribute is metadata ATTACHED to the next reply, not a reply. A
         * scan that stopped here would hand the caller a value it never asked
         * for and leave the real one in the buffer, one command out of step.
         * EVERY top-level attribute is skipped: a second consecutive one is
         * legal and treating it as the reply swallows the value it attaches
         * to. */
        if (type == DYN_RESP_ATTR && sp == 0) {
            want[0] = 1;
        }
        if (n > 0) {
            if (sp + 1 >= DYN_RESP_MAX_DEPTH)
                return DYN_RESP_E_DEPTH;
            want[++sp] = n;
        }
    }
    *consumed = pos;
    return DYN_RESP_OK;
}

void dyn_resp_reader_init(dyn_resp_reader_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

/* Locale-independent decimal -> double. `strtod` and `sscanf("%lf")` both read
 * LC_NUMERIC for the radix character, so under a locale where that is ',' they
 * stop at the '.' and return the integer part -- silently, and far from
 * whatever called setlocale. This accepts exactly what RESP3 emits:
 * [+-]digits[.digits][(e|E)[+-]digits]. */
/* Locale-independent decimal -> double, EXACTLY.
 *
 * `strtod` and `sscanf("%lf")` both read LC_NUMERIC for the radix character, so
 * a `setlocale` in some unrelated module would silently truncate every RESP
 * double at the decimal point. They are also far too permissive for a wire
 * grammar (0x10, leading whitespace, partial input).
 *
 * A hand-rolled digit accumulator -- which this was -- is not the answer
 * either: `v = v*10 + d` followed by a divide cannot round-trip a double.
 * MEASURED against a real Redis 8: `ZSCORE` of a member scored 1e300 came back
 * as 1.0000000000000002e+300, and DBL_MAX as 1.7976931348623145e+308. The same
 * bug was found first in the PostgreSQL client by diffing its text decode
 * against the binary wire format, then looked for here because it is the same
 * shape. js_atod is the engine's own JS number parser: correctly rounded by
 * construction, and locale-independent because the ECMAScript grammar has no
 * locale. The GRAMMAR stays ours -- the input must be consumed entirely, so
 * `0x10`, `1 2` and `` are all refused. */
static int dyn_resp_parse_double(const uint8_t *p, size_t n, double *out)
{
    char stackbuf[64], *s = stackbuf, *heap = NULL;
    const char *end = NULL;
    JSATODTempMem tmp;
    double v;

    if (n == 0)
        return DYN_RESP_E_SYNTAX;
    if (n + 1 > sizeof(stackbuf)) {
        heap = (char *)malloc(n + 1);
        if (!heap)
            return DYN_RESP_E_SYNTAX;
        s = heap;
    }
    memcpy(s, p, n);
    s[n] = '\0';
    /* radix 10, no flags: no 0x, no underscores, no leading whitespace. */
    v = js_atod(s, &end, 10, 0, &tmp);
    if (!end || end != s + n || v != v) {
        /* v != v is a NaN test. js_atod implements the ECMAScript grammar,
         * which is not this protocol's: it tolerates `1e` and hands back NaN.
         * The specials are spelled out and handled BEFORE this point, so a NaN
         * here can only mean the parser accepted what the wire cannot send. */
        free(heap);
        return DYN_RESP_E_SYNTAX;      /* trailing junk, or a shape the grammar forbids */
    }
    free(heap);
    *out = v;
    return DYN_RESP_OK;
}

int dyn_resp_next(dyn_resp_reader_t *r, dyn_resp_item_t *it)
{
    const uint8_t *b = r->buf;
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
    case DYN_RESP_SIMPLE: case DYN_RESP_ERROR: case DYN_RESP_BIGNUM:
        return DYN_RESP_OK;
    case DYN_RESP_NULL:
        it->isnull = 1;
        it->str = NULL; it->slen = 0;
        return DYN_RESP_OK;
    case DYN_RESP_BOOL:
        it->ival = (it->slen == 1 && it->str[0] == 't');
        return DYN_RESP_OK;
    case DYN_RESP_INT:
        return resp_int(b, p + 1, e, &it->ival);
    case DYN_RESP_DOUBLE: {
        /* inf/-inf/nan are spelled out and matched exactly; everything else
         * goes through a hand-rolled decimal parse rather than strtod/sscanf,
         * which honour LC_NUMERIC. Nothing in this tree calls setlocale today,
         * so those would be correct now and would silently truncate every
         * double at the decimal point the day a native module does. */
        if (it->slen == 3 && memcmp(it->str, "inf", 3) == 0)
            { it->dval = 1e308 * 10; return DYN_RESP_OK; }
        if (it->slen == 4 && memcmp(it->str, "-inf", 4) == 0)
            { it->dval = -(1e308 * 10); return DYN_RESP_OK; }
        if (it->slen == 3 && memcmp(it->str, "nan", 3) == 0)
            { it->dval = (1e308 * 10) - (1e308 * 10); return DYN_RESP_OK; }
        if (it->slen == 0)
            return DYN_RESP_E_SYNTAX;
        return dyn_resp_parse_double(it->str, it->slen, &it->dval);
    }
    case DYN_RESP_BULK: case DYN_RESP_BLOBERR: case DYN_RESP_VERB: {
        int64_t n;
        rc = resp_int(b, p + 1, e, &n);
        if (rc != DYN_RESP_OK)
            return rc;
        if (n < 0) { it->isnull = 1; it->str = NULL; it->slen = 0; return DYN_RESP_OK; }
        if (e + 2 + (size_t)n + 2 > r->len)
            return DYN_RESP_INCOMPLETE;
        it->str = b + e + 2;
        it->slen = (size_t)n;
        r->pos = e + 2 + (size_t)n + 2;
        return DYN_RESP_OK;
    }
    case DYN_RESP_ARRAY: case DYN_RESP_SET: case DYN_RESP_PUSH:
    case DYN_RESP_MAP:   case DYN_RESP_ATTR: {
        int64_t n;
        rc = resp_int(b, p + 1, e, &n);
        if (rc != DYN_RESP_OK)
            return rc;
        it->str = NULL; it->slen = 0;
        if (n < 0) { it->isnull = 1; it->count = 0; return DYN_RESP_OK; }
        it->count = n;                 /* map/attr: PAIRS, not values */
        return DYN_RESP_OK;
    }
    default:
        return DYN_RESP_E_TYPE;
    }
}

/* ---- command encoding -------------------------------------------------- */

static size_t resp_declen(size_t v)
{
    size_t n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

size_t dyn_resp_cmd_size(int argc, const char *const *argv, const size_t *lens)
{
    size_t total = 1 + resp_declen((size_t)argc) + 2;   /* *N\r\n */
    int i;
    for (i = 0; i < argc; i++) {
        size_t l = (lens && lens[i] != (size_t)-1) ? lens[i] : strlen(argv[i]);
        total += 1 + resp_declen(l) + 2 + l + 2;        /* $L\r\n<data>\r\n */
    }
    return total;
}

static size_t resp_put_dec(uint8_t *out, size_t v)
{
    char tmp[24];
    size_t n = 0, i;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)tmp[n - 1 - i];
    return n;
}

int dyn_resp_cmd_encode(uint8_t *out, size_t outcap, int argc,
                        const char *const *argv, const size_t *lens,
                        size_t *need)
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
    out[w++] = '\r'; out[w++] = '\n';
    for (i = 0; i < argc; i++) {
        size_t l = (lens && lens[i] != (size_t)-1) ? lens[i] : strlen(argv[i]);
        out[w++] = '$';
        w += resp_put_dec(out + w, l);
        out[w++] = '\r'; out[w++] = '\n';
        if (l)
            memcpy(out + w, argv[i], l);
        w += l;
        out[w++] = '\r'; out[w++] = '\n';
    }
    return (int)w;
}
