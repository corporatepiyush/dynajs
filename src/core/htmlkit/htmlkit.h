#ifndef DYN_HTMLKIT_HTMLKIT_H
#define DYN_HTMLKIT_HTMLKIT_H

#include "../dyn-common.h"
#include "../dyn-simdk.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define HT_MAX_DEPTH 256
#define HT_MAX_ATTRS 512
#define HT_MAX_INPUT (64u << 20)

#define HT_SIMD_MIN 64
#define TPL_SIMD_MIN 64

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} hb_t;

void hb_write_grow(hb_t* b, const void* p, size_t n);

static inline void hb_write(hb_t* b, const void* p, size_t n)
{
    if (b->n + n <= b->cap && !b->oom) {
        if (n)
            memcpy(b->p + b->n, p, n);
        b->n += n;
        return;
    }
    hb_write_grow(b, p, n);
}

void hb_reserve(hb_t* b, size_t need);

static inline void hb_init(hb_t* b)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->oom = 0;
}

static inline void hb_free(hb_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static inline void hb_put(hb_t* b, uint8_t c)
{
    if (b->n < b->cap && !b->oom) {
        b->p[b->n++] = c;
        return;
    }
    hb_write_grow(b, &c, 1);
}

static inline void hb_puts(hb_t* b, const char* s) { hb_write(b, s, strlen(s)); }

static inline int ht_lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static inline int ht_ieq(const char* a, size_t an, const char* b)
{
    size_t i;
    for (i = 0; i < an; i++)
        if (b[i] == '\0' || ht_lower((unsigned char)a[i]) != b[i])
            return 0;
    return b[an] == '\0';
}

extern const char* const HT_VOID[14];

extern const char* const HT_RAW[4];

extern const char* const HT_RAW_ESCAPELESS[2];

extern const char* const HT_CLOSES_P[26];

static inline int ht_in(const char* const* set, size_t n, const char* s, size_t len)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (ht_ieq(s, len, set[i]))
            return 1;
    return 0;
}

static inline int ht_is_void(const char* s, size_t n)
{
    int c;
    if (n < 2 || n > 6)
        return 0;
    c = ht_lower((unsigned char)s[0]);
    switch (n) {
    case 2:
        return (c == 'b' && ht_ieq(s, 2, "br")) || (c == 'h' && ht_ieq(s, 2, "hr"));
    case 3:
        return (c == 'c' && ht_ieq(s, 3, "col")) || (c == 'i' && ht_ieq(s, 3, "img"))
            || (c == 'w' && ht_ieq(s, 3, "wbr"));
    case 4:
        return (c == 'a' && ht_ieq(s, 4, "area")) || (c == 'b' && ht_ieq(s, 4, "base"))
            || (c == 'l' && ht_ieq(s, 4, "link")) || (c == 'm' && ht_ieq(s, 4, "meta"));
    case 5:
        return (c == 'e' && ht_ieq(s, 5, "embed")) || (c == 'i' && ht_ieq(s, 5, "input"))
            || (c == 'p' && ht_ieq(s, 5, "param")) || (c == 't' && ht_ieq(s, 5, "track"));
    default:
        return c == 's' && ht_ieq(s, 6, "source");
    }
}

#define HT_IS_VOID(s, n) ht_is_void((s), (n))
#define HT_IS_RAW(s, n) ht_in(HT_RAW, countof(HT_RAW), (s), (n))

int ht_entity(const char* s, size_t n, size_t* pi, hb_t* out);

#define HT_CLOSES(s, n) ht_in(HT_CLOSES_P, countof(HT_CLOSES_P), (s), (n))
#define HT_IS_RAW_ESCAPELESS(s, n) ht_in(HT_RAW_ESCAPELESS, countof(HT_RAW_ESCAPELESS), (s), (n))

extern const uint8_t HT_ESC[256];
extern const uint8_t HT_ESC_SET[5];
extern const uint8_t TPL_ESC[256];
extern const uint8_t TPL_ESC_SET[5];

typedef struct {
    const char* n;
    size_t nlen;
    size_t voff, vlen;
} ht_attr_t;

static inline int ht_tag_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline int ht_name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':' || c >= 0x80;
}

static inline int ht_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

void ht_escape(hb_t* b, const char* s, size_t n, int attr);

static inline int ht_tag_is(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        if (ca != *b)
            return 0;
    }
    return *a == 0 && *b == 0;
}

static inline void ht_decode(const char* s, size_t n, hb_t* out)
{
    size_t i = 0;

    if (out->cap == 0 && n > 1024)
        hb_reserve(out, n);
    while (i < n) {
        size_t start = i;
        size_t probe = i + 64;
        size_t j = i;

        if (probe > n)
            probe = n;
        while (j < probe && s[j] != '&')
            j++;
        if (j == probe && j < n) {
            size_t t = simd.find_first_of((const uint8_t*)s + j, n - j,
                (const uint8_t*)"&", 1);
            j = (t == (size_t)-1) ? n : j + t;
        }
        hb_write(out, s + start, j - start);
        i = j;
        if (i >= n)
            break;
        if (!ht_entity(s, n, &i, out)) {
            hb_put(out, '&');
            i++;
        }
    }
}

static inline int ht_raw_end(const char* s, size_t n, size_t i, size_t tlen)
{
    return i + 2 + tlen >= n || ht_space((unsigned char)s[i + 2 + tlen])
        || s[i + 2 + tlen] == '>' || s[i + 2 + tlen] == '/';
}

static inline void ht_write_raw(hb_t* b, const char* s, size_t n)
{
    size_t i, run = 0;
    for (i = 0; i < n; i++)
        if (s[i] == '\0') {
            hb_write(b, s + run, i - run);
            hb_write(b, "\xEF\xBF\xBD", 3);
            run = i + 1;
        }
    hb_write(b, s + run, n - run);
}

static inline int ht_is_single_url_attr(const char* a, size_t n)
{
    static const char* const URLS[] = {
        "href",
        "src",
        "action",
        "formaction",
        "cite",
        "data",
        "poster",
        "background",
        "ping",
        "xlink:href",
        "longdesc",
    };
    size_t k;
    char tmp[32];
    size_t i;
    if (n >= sizeof tmp)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)a[i];
        tmp[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    tmp[n] = '\0';
    for (k = 0; k < countof(URLS); k++)
        if (!strcmp(tmp, URLS[k]))
            return 1;
    return 0;
}

static inline int ht_is_srcset_attr(const char* a, size_t n)
{
    return n == 6 && (a[0] == 's' || a[0] == 'S') && (a[1] == 'r' || a[1] == 'R') && (a[2] == 'c' || a[2] == 'C') && (a[3] == 's' || a[3] == 'S') && (a[4] == 'e' || a[4] == 'E') && (a[5] == 't' || a[5] == 'T');
}

static inline void tpl_trim(const char* s, uint32_t* a, uint32_t* b)
{
    while (*a < *b && (s[*a] == ' ' || s[*a] == '\t'))
        (*a)++;
    while (*b > *a && (s[*b - 1] == ' ' || s[*b - 1] == '\t'))
        (*b)--;
}

static inline void tpl_escape(hb_t* b, const char* s, size_t n)
{
    size_t i = 0, run;
    while (i < n) {
        run = i;
        {
            size_t probe = i + TPL_SIMD_MIN;
            if (probe > n)
                probe = n;
            while (i < probe && !TPL_ESC[(unsigned char)s[i]])
                i++;
            if (i == probe && i < n) {
                size_t k = simd.find_first_of((const uint8_t*)s + i, n - i,
                    TPL_ESC_SET, sizeof TPL_ESC_SET);
                i = (k == (size_t)-1) ? n : i + k;
            }
        }
        hb_write(b, s + run, i - run);
        if (i >= n)
            break;
        switch (s[i]) {
        case '<':
            hb_puts(b, "&lt;");
            break;
        case '>':
            hb_puts(b, "&gt;");
            break;
        case '&':
            hb_puts(b, "&amp;");
            break;
        case '"':
            hb_puts(b, "&quot;");
            break;
        default:
            hb_puts(b, "&#39;");
            break;
        }
        i++;
    }
}

typedef struct {
    char *a, *b;
    uint32_t sub;
    uint8_t kind, op;
} sel_part_t;

_Static_assert(sizeof(sel_part_t) == 2 * sizeof(void*) + 8,
    "sel_part_t regained padding: keep a/b first, scalars last");

char* sel_dup(const char* s, size_t n);

#define HT_MAX_SEL 4096

enum { SP_TAG,
    SP_ID,
    SP_CLASS,
    SP_ATTR,
    SP_FIRST,
    SP_LAST,
    SP_ANY };

enum { AO_HAS,
    AO_EQ,
    AO_PREFIX,
    AO_SUFFIX,
    AO_CONTAINS,
    AO_WORD };

enum { CB_DESC,
    CB_CHILD };

typedef struct {
    uint8_t comb;
    uint32_t first, n;
} sel_unit_t;

typedef struct {
    sel_unit_t* units;
    uint32_t nunit, cunit;
    sel_part_t* parts;
    uint32_t npart, cpart;
    uint32_t* groups;
    uint32_t ngroup, cgroup;
    uint8_t pos_used;
} sel_t;

typedef struct {
    char* tag;
    char** attrs;
    uint32_t nattr;
} san_rule_t;

typedef struct {
    san_rule_t* rules;
    char** protos;
    uint32_t n;
    uint32_t nproto;
} san_t;

_Static_assert(sizeof(san_t) == 2 * sizeof(void*) + 8,
    "san_t regained padding: keep the two counts adjacent");

static inline const san_rule_t* san_find(const san_t* s, const char* tag, size_t n)
{
    uint32_t i;
    for (i = 0; i < s->n; i++)
        if (ht_ieq(tag, n, s->rules[i].tag))
            return &s->rules[i];
    return NULL;
}

static inline int san_attr_ok(const san_rule_t* r, const char* a, size_t n)
{
    uint32_t k;
    for (k = 0; k < r->nattr; k++)
        if (ht_ieq(a, n, r->attrs[k]))
            return 1;
    return 0;
}

void san_free(san_t* s);
void san_free_v(void* p);
char* san_lower_dup(const char* s);
static inline int san_is_url_attr(const char* a, size_t n)
{
    static const char* const URL_ATTRS[] = {
        "href",
        "src",
        "action",
        "formaction",
        "xlink:href",
        "poster",
        "cite",
        "data",
        "srcset",
        "background",
        "ping",
        "style",
    };
    return ht_in(URL_ATTRS, countof(URL_ATTRS), a, n);
}

static inline int san_safe_scheme(const char* s)
{
    static const char* const SAFE[] = { "http", "https", "mailto", "ftp" };
    uint32_t i;
    for (i = 0; i < countof(SAFE); i++)
        if (strcmp(s, SAFE[i]) == 0)
            return 1;
    return 0;
}

static inline int san_scheme_of(const char* val, size_t vn, char* out, size_t cap)
{
    size_t k = 0, sn = 0;
    while (k < vn && (unsigned char)val[k] <= 0x20)
        k++;
    while (k < vn) {
        unsigned char c = (unsigned char)val[k];
        if (c == ':')
            break;
        if (c == '/' || c == '?' || c == '#')
            return 0;
        if (c <= 0x20) {
            k++;
            continue;
        }
        if (sn + 1 < cap)
            out[sn++] = (char)ht_lower(c);
        k++;
    }
    if (k >= vn)
        return 0;
    out[sn] = 0;
    return 1;
}

static inline int san_name_ok(const char* p, size_t n)
{
    size_t k;
    if (!n)
        return 0;
    for (k = 0; k < n; k++) {
        unsigned char c = (unsigned char)p[k];
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\'' || c == '>'
            || c == '<' || c == '/' || c == '=' || c == '&' || c == '\\')
            return 0;
    }
    return 1;
}

static inline int san_proto_ok(const san_t* s, const char* tag, size_t tn,
    const char* attr, size_t an, const char* val, size_t vn)
{
    char key[128];
    uint32_t i;
    size_t j = 0, k;
    int have_rule = 0;
    char scheme[32];

    if ((int)tn + (int)an + 2 > (int)sizeof key)
        return 0;
    for (k = 0; k < tn; k++)
        key[j++] = (char)ht_lower((unsigned char)tag[k]);
    key[j++] = '.';
    for (k = 0; k < an; k++)
        key[j++] = (char)ht_lower((unsigned char)attr[k]);
    key[j] = 0;
    for (i = 0; i < s->nproto; i += 2)
        if (strcmp(s->protos[i], key) == 0) {
            have_rule = 1;
            break;
        }

    if (!have_rule && !san_is_url_attr(attr, an))
        return 1;

    if (!san_scheme_of(val, vn, scheme, sizeof scheme))
        return 1;

    if (have_rule) {
        for (i = 0; i < s->nproto; i += 2)
            if (strcmp(s->protos[i], key) == 0 && strcmp(s->protos[i + 1], scheme) == 0)
                return 1;
        return 0;
    }
    return san_safe_scheme(scheme);
}

sel_t* sel_compile(const char* s, size_t n, char* err, size_t errsz);
void sel_free(sel_t* s);
void sel_free_v(void* p);

#endif
