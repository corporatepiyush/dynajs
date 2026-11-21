#ifndef DYN_YAMLSCALAR_YAMLSCALAR_H
#define DYN_YAMLSCALAR_YAMLSCALAR_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dyn-common.h"
#include "../dyn-sb.h"

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} yb_t;

static inline void yb_init(yb_t* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}

static inline void yb_free(yb_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

__attribute__((noinline)) int yb_grow(yb_t* b, size_t n);

static inline int yb_room(yb_t* b, size_t n)
{
    return (b->n + n <= b->cap && !b->oom) || yb_grow(b, n);
}

static inline void yb_write(yb_t* b, const void* p, size_t n)
{
    if (!n || !yb_room(b, n))
        return;
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static inline void yb_put(yb_t* b, uint8_t c)
{
    if (yb_room(b, 1))
        b->p[b->n++] = c;
}

#define yml_eq(s, n, lit) ((n) == sizeof("" lit) - 1 && memcmp((s), "" lit, sizeof("" lit) - 1) == 0)

int yml_is_int(const char* s, size_t n, int64_t* out);
int yml_is_int_syntax(const char* s, size_t n);
int yml_is_float(const char* s, size_t n);
size_t yml_plain_end(const char* s, size_t start, size_t end);
size_t yml_key_colon(const char* s, size_t st, size_t end);
void yml_put_escaped_body(yb_t* b, const char* s, size_t n);

static inline void yb_puts(yb_t* b, const char* t) { yb_write(b, t, strlen(t)); }

static inline void yml_utf8_put(yb_t* b, uint32_t cp)
{
    uint8_t t[4];
    yb_write(b, t, (size_t)dyn_utf8_encode(cp, t));
}

static inline int yml_hex(const char* s, size_t n, int width, uint32_t* out)
{
    uint32_t cp = 0;
    int k;

    for (k = 0; k < width; k++) {
        char h = s[n + (size_t)k];
        int d = dyn_hex_digit((unsigned char)h);
        if (d < 0)
            return -1;
        cp = cp * 16 + (uint32_t)d;
    }
    *out = cp;
    return 0;
}

static inline int yml_utf8_unit(const char* p, size_t i, size_t n)
{
    unsigned char c = (unsigned char)p[i];
    int len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3
        : c >= 0xc0                     ? 2
                                        : 1;

    if (i + (size_t)len > n)
        len = (int)(n - i);
    return len;
}

static inline int yml_needs_quote(const char* s, size_t n, int flow)
{
    size_t i;

    if (n == 0)
        return 1;
    switch (s[0]) {
    case '~':
    case 'n':
    case 'N':
    case 't':
    case 'T':
    case 'f':
    case 'F':
    case 'y':
    case 'Y':
    case 'o':
    case 'O':
    case '.':
    case '+':
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        break;
    default:
        goto chars;
    }
    if (yml_eq(s, n, "~") || yml_eq(s, n, "null") || yml_eq(s, n, "Null")
        || yml_eq(s, n, "NULL") || yml_eq(s, n, "true") || yml_eq(s, n, "True")
        || yml_eq(s, n, "TRUE") || yml_eq(s, n, "false") || yml_eq(s, n, "False")
        || yml_eq(s, n, "FALSE") || yml_is_int_syntax(s, n) || yml_is_float(s, n))
        return 1;
    if (yml_eq(s, n, "yes") || yml_eq(s, n, "Yes") || yml_eq(s, n, "YES")
        || yml_eq(s, n, "no") || yml_eq(s, n, "No") || yml_eq(s, n, "NO")
        || yml_eq(s, n, "on") || yml_eq(s, n, "On") || yml_eq(s, n, "ON")
        || yml_eq(s, n, "off") || yml_eq(s, n, "Off") || yml_eq(s, n, "OFF"))
        return 1;
    if (yml_eq(s, n, ".inf") || yml_eq(s, n, ".Inf") || yml_eq(s, n, ".INF")
        || yml_eq(s, n, "+.inf") || yml_eq(s, n, "+.Inf") || yml_eq(s, n, "+.INF")
        || yml_eq(s, n, "-.inf") || yml_eq(s, n, "-.Inf") || yml_eq(s, n, "-.INF")
        || yml_eq(s, n, ".nan") || yml_eq(s, n, ".NaN") || yml_eq(s, n, ".NAN"))
        return 1;
    if (yml_eq(s, n, "---") || yml_eq(s, n, "..."))
        return 1;
chars:
    if (s[0] == ' ' || s[n - 1] == ' ' || s[0] == '-' || s[0] == '?'
        || s[0] == '#' || s[0] == '&' || s[0] == '*' || s[0] == '!'
        || s[0] == '%' || s[0] == '[' || s[0] == '{' || s[0] == '>'
        || s[0] == '|' || s[0] == '"' || s[0] == '\'' || s[0] == '@'
        || s[0] == '`')
        return 1;
    for (i = 0; i < n; i++)
        if (s[i] == ':' || s[i] == '\n' || s[i] == '#'
            || (unsigned char)s[i] < 0x20)
            return 1;
    if (flow)
        for (i = 0; i < n; i++)
            if (s[i] == ',' || s[i] == '[' || s[i] == ']'
                || s[i] == '{' || s[i] == '}')
                return 1;
    return 0;
}

static inline size_t yml_flow_colon(const char* s, size_t st, size_t end)
{
    size_t i = st;
    char q = 0;
    int jsonlike = (i < end
        && (s[i] == '"' || s[i] == '\'' || s[i] == '[' || s[i] == '{'));

    while (i < end) {
        char c = s[i];
        if (q) {
            if (q == '"' && c == '\\' && i + 1 < end) {
                i += 2;
                continue;
            }
            if (c == q)
                q = 0;
        } else if (c == '"' || c == '\'') {
            q = c;
        } else if (c == ':' || c == ',' || c == ']' || c == '}') {
            if (c != ':')
                return (size_t)-1;
            if (jsonlike || i + 1 >= end || s[i + 1] == ' ' || s[i + 1] == '\t'
                || s[i + 1] == ',' || s[i + 1] == ']' || s[i + 1] == '}'
                || s[i + 1] == '[' || s[i + 1] == '{')
                return i;
            return (size_t)-1;
        }
        i++;
    }
    return (size_t)-1;
}

static inline int yml_flow_is_pair(const char* s, size_t st, size_t end)
{
    size_t i = st;
    char q = 0;

    while (i < end) {
        char c = s[i];
        if (q) {
            if (q == '"' && c == '\\' && i + 1 < end) {
                i += 2;
                continue;
            }
            if (c == q)
                q = 0;
        } else if (c == '"' || c == '\'') {
            q = c;
        } else if (c == ':'
            && (i + 1 >= end || s[i + 1] == ' ' || s[i + 1] == '\t'))
            return 1;
        else if (c == ',' || c == ']' || c == '}') {
            return 0;
        }
        i++;
    }
    return 0;
}

static inline const char* yml_unsupported(const char* s, size_t n)
{
    if (!n)
        return NULL;
    if (s[0] == '&')
        return "an anchor (&name) -- this parser has no anchors or aliases";
    if (s[0] == '*')
        return "an alias (*name) -- this parser has no anchors or aliases";
    if (s[0] == '!')
        return "a tag (!name) -- this parser resolves the core schema only";
    if (s[0] == '%')
        return "a directive (%YAML/%TAG)";
    if (n >= 2 && s[0] == '?' && (n == 1 || s[1] == ' '))
        return "an explicit key (? ) -- keys here are scalars";
    if (n >= 2 && s[0] == '<' && s[1] == '<')
        return "a merge key (<<) -- this parser has no anchors to merge from";
    return NULL;
}

#endif
