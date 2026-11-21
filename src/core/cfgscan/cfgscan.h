#ifndef DYN_CFGSCAN_CFGSCAN_H
#define DYN_CFGSCAN_CFGSCAN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../dyn-ctype.h"

#define DYN_ENV_KEY_MAX 4096

typedef struct {
    const char* s;
    size_t n, i;
} dyn_cur_t;

int dyn_next_line(dyn_cur_t* c, size_t* b, size_t* e);

static inline int dyn_cfg_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'
        || c == '\v';
}

static inline void dyn_trim(const char* s, size_t* b, size_t* e)
{
    while (*b < *e && dyn_cfg_space(s[*b]))
        (*b)++;
    while (*e > *b && dyn_cfg_space(s[*e - 1]))
        (*e)--;
}

static inline void dyn_cfg_bom(const char** ps, size_t* pn)
{
    if (*pn >= 3 && (unsigned char)(*ps)[0] == 0xEF
        && (unsigned char)(*ps)[1] == 0xBB
        && (unsigned char)(*ps)[2] == 0xBF) {
        *ps += 3;
        *pn -= 3;
    }
}

int dyn_frontmatter_find(const char* src, size_t n, size_t* pdstart, size_t* pdend, size_t* pbstart, const char** plang);

void dyn_env_read_value(const char* s, size_t n, size_t* i, size_t* vb,
    size_t* ve, char* q);

int dyn_env_key_ok(const char* k, size_t n);

int dyn_ini_needs_quotes(const char* s, size_t n);

static inline int dyn_env_next_record(const char* s, size_t n, size_t* i)
{
    for (;;) {
        while (*i < n && dyn_in_set64((unsigned char)s[*i], DYN_WS_XML))
            (*i)++;
        if (*i >= n)
            return 0;
        if (s[*i] != '#')
            return 1;
        while (*i < n && s[*i] != '\n')
            (*i)++;
    }
}

static inline int dyn_env_needs_quotes(const char* s, size_t n)
{
    size_t i;
    if (n == 0)
        return 1;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '#' || c == '"' || c == '\'')
            return 1;
    }
    return 0;
}

typedef struct {
    char* p;
    size_t n, cap;
    int oom;
} dyn_tml_sb_t;

void dyn_tml_sb_init(dyn_tml_sb_t* b);
void dyn_tml_sb_free(dyn_tml_sb_t* b);
void dyn_tml_sb_put_slow(dyn_tml_sb_t* b, const char* s, size_t n);

static inline void dyn_tml_sb_put(dyn_tml_sb_t* b, const char* s, size_t n)
{
    if (n && !b->oom && n <= b->cap - b->n) {
        memcpy(b->p + b->n, s, n);
        b->n += n;
        return;
    }
    dyn_tml_sb_put_slow(b, s, n);
}

static inline void dyn_tml_sb_putc(dyn_tml_sb_t* b, char c)
{
    if (!b->oom && b->n < b->cap) {
        b->p[b->n++] = c;
        return;
    }
    dyn_tml_sb_put_slow(b, &c, 1);
}

static inline void dyn_tml_sb_puts(dyn_tml_sb_t* b, const char* s) { dyn_tml_sb_put(b, s, strlen(s)); }

enum {
    DYN_TML_E_NONE,
    DYN_TML_E_BAD_U,
    DYN_TML_E_NOT_SCALAR,
    DYN_TML_E_BAD_ESCAPE,
    DYN_TML_E_UNTERM,
    DYN_TML_E_CTRL,
    DYN_TML_E_UNTERM_LIT,
    DYN_TML_E_CTRL_LIT,
    DYN_TML_E_UNTERM_ML,
    DYN_TML_E_QUOTES,
    DYN_TML_E_CTRL_ML,
    DYN_TML_E_ML_ESCAPE,
    DYN_TML_E_UNTERM_MLL,
    DYN_TML_E_APOS,
    DYN_TML_E_CTRL_MLL,
    DYN_TML_E_KEY,
    DYN_TML_E_NUM_LONG,
    DYN_TML_E_INT_RANGE,
    DYN_TML_E_DATETIME,
    DYN_TML_E_VALUE,
    DYN_TML_E_NUM,
    DYN_TML_E_UNDERSCORE,
    DYN_TML_E_DOT_BEFORE,
    DYN_TML_E_DOT_AFTER,
    DYN_TML_E_EXP,
    DYN_TML_E_FLOAT,
    DYN_TML_E_INT,
    DYN_TML_E_BASE,
    DYN_TML_E_LEADING_ZERO,
    DYN_TML_E_OOM,
    DYN_TML_E_COUNT
};

typedef double (*dyn_tml_atod_fn)(const char* buf, const char** end);

typedef struct {
    const char* src;
    size_t n, i;
    int line, col;
    int ecode, eline, ecol, earg;
} dyn_tml_lex_t;

#define DYN_TML_KEY_INL 128
#define DYN_TML_KEY_SEGS 4

typedef struct {
    size_t off, len;
} dyn_tml_seg_t;

typedef struct {
    char inl[DYN_TML_KEY_INL];
    dyn_tml_seg_t seginl[DYN_TML_KEY_SEGS];
    char* p;
    size_t n, cap;
    dyn_tml_seg_t* seg;
    size_t nseg, segcap;
    int oom;
} dyn_tml_key_t;

enum {
    DYN_TML_V_BOOL,
    DYN_TML_V_INT,
    DYN_TML_V_FLOAT,
    DYN_TML_V_DATETIME
};

typedef struct {
    int kind;
    int b;
    int64_t i;
    double d;
    size_t off, len;
} dyn_tml_scalar_t;

static inline void dyn_tml_lex_init(dyn_tml_lex_t* t, const char* src, size_t n)
{
    memset(t, 0, sizeof *t);
    t->src = src;
    t->n = n;
    t->line = 1;
    t->col = 1;
}

static inline int dyn_tml_cur(const dyn_tml_lex_t* t) { return t->i < t->n ? (unsigned char)t->src[t->i] : -1; }

static inline int dyn_tml_peek(const dyn_tml_lex_t* t, int k) { return t->i + (size_t)k < t->n ? (unsigned char)t->src[t->i + k] : -1; }

static inline void dyn_tml_adv(dyn_tml_lex_t* t)
{
    if (t->src[t->i] == '\n') {
        t->line++;
        t->col = 1;
    } else
        t->col++;
    t->i++;
}

static inline void dyn_tml_skip(dyn_tml_lex_t* t, int k)
{
    t->i += (size_t)k;
    t->col += k;
}

static inline void dyn_tml_ws(dyn_tml_lex_t* t)
{
    int c;
    while ((c = dyn_tml_cur(t)) == ' ' || c == '\t')
        dyn_tml_adv(t);
}

static inline void dyn_tml_blank(dyn_tml_lex_t* t)
{
    for (;;) {
        int c = dyn_tml_cur(t);
        if (dyn_in_set64((unsigned)c, DYN_WS_XML)) {
            dyn_tml_adv(t);
            continue;
        }
        if (c == '#') {
            while ((c = dyn_tml_cur(t)) != -1 && c != '\n')
                dyn_tml_adv(t);
            continue;
        }
        return;
    }
}

static inline int dyn_tml_digit(int c) { return c >= '0' && c <= '9'; }

static inline int dyn_tml_key_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || dyn_tml_digit(c) || c == '_' || c == '-';
}

int dyn_tml_fail(dyn_tml_lex_t* t, int code, int arg);
const char* dyn_tml_errstr(int code);
int dyn_tml_format_error(const dyn_tml_lex_t* t, char* buf, size_t cap);
int dyn_tml_string(dyn_tml_lex_t* t, dyn_tml_sb_t* sb);
void dyn_tml_key_init(dyn_tml_key_t* k);
void dyn_tml_key_free(dyn_tml_key_t* k);
int dyn_tml_key_parse(dyn_tml_lex_t* t, dyn_tml_key_t* k);
int dyn_tml_scalar(dyn_tml_lex_t* t, dyn_tml_scalar_t* v, dyn_tml_atod_fn atod);

#endif
