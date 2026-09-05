/*
 * dyn-pct -- the ONE percent-encode/decode core (RFC 3986 / x-www-form-
 * urlencoded). The round-8 census counted 17 percent-family implementations
 * across 6 modules; three pairs were byte-identical duplicates that drifted
 * by copy. This header is the shared core they consolidate onto: pure C,
 * sink-callback based so each caller keeps its own buffer type.
 *
 * SINK CONTRACT: called with 1..3 bytes to append, in order. A per-call
 * indirect jump is fine here -- this is wire-shaping, not a hot loop; the
 * corpus-verified wrappers in dyna-url/oauth2/schema measure identical to
 * their former inlined copies.
 */
#ifndef DYNA_CORE_PCT_H
#define DYNA_CORE_PCT_H

#include <stddef.h>
#include <string.h>

typedef void (*dyn_pct_sink)(void *ud, const char *bytes, size_t n);

/* The common plain-buffer sink: callers keep their own buffer type by
 * wrapping this struct (the round-9 refactor pass deduped the two identical
 * copies that had grown in oauth2 and schema). */
typedef struct { char *p; size_t n; } dyn_pct_buf_t;
static inline void dyn_pct_buf_sink(void *ud, const char *b, size_t k)
{
    dyn_pct_buf_t *o = (dyn_pct_buf_t *)ud;
    memcpy(o->p + o->n, b, k);
    o->n += k;
}

/* Encode forms:
 *  0 -- unreserved + *-._ + !'()~  (encodeURIComponentStrict's set)
 *  1 -- unreserved + *-._, space -> '+'  (x-www-form-urlencoded)
 *  2 -- the WHATWG urlencoded set (C0/DEL/non-ASCII/space/"#<> escape,
 *       space -> '+'): URLSearchParams serialization.
 * Hex digits are UPPERCASE, matching every spec example. */
static inline int dyn_pct_safe_0(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.'
        || c == '_' || c == '~' || c == '!' || c == '\'' || c == '(' || c == ')';
}

static inline int dyn_pct_safe_1(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_';
}

static inline int dyn_pct_safe_2(unsigned char c)
{
    return !(c <= 0x1F || c >= 0x7F || c == ' ' || c == '"' || c == '#'
             || c == '<' || c == '>');
}

static inline void dyn_pct_encode_core(void *ud, dyn_pct_sink sink,
                                       const char *s, size_t n, int form)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int safe = form == 2 ? dyn_pct_safe_2(c)
                             : (form ? dyn_pct_safe_1(c) : dyn_pct_safe_0(c));
        if (safe) {
            sink(ud, &s[i], 1);
        } else if (form && c == ' ') {
            sink(ud, "+", 1);
        } else {
            char trip[3] = { '%', HEX[c >> 4], HEX[c & 0xF] };
            sink(ud, trip, 3);
        }
    }
}

/* Case-insensitive hex digit, or -1: six hand-rolled copies of this existed
 * while cutils had one all along. */
static inline int dyn_pct_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode %XX over RAW BYTES. A malformed escape is left LITERAL rather than
 * dropped: silently losing bytes is how a decoder turns one request into a
 * different one. `plus_space` maps '+' to ' ' (form decoding). */
static inline void dyn_pct_decode_core(void *ud, dyn_pct_sink sink,
                                       const char *s, size_t n, int plus_space)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int h = dyn_pct_hexval((unsigned char)s[i + 1]);
            int l = dyn_pct_hexval((unsigned char)s[i + 2]);
            if (h >= 0 && l >= 0) {
                char b = (char)((h << 4) | l);
                sink(ud, &b, 1);
                i += 2;
                continue;
            }
        }
        if (plus_space && s[i] == '+') {
            sink(ud, " ", 1);
        } else {
            sink(ud, &s[i], 1);
        }
    }
}

#endif /* DYNA_CORE_PCT_H */
