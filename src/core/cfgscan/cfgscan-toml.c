#include "cfgscan.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char* const dyn_tml_msgs[DYN_TML_E_COUNT] = {
    [DYN_TML_E_NONE] = "",
    [DYN_TML_E_BAD_U] = "invalid unicode escape",
    [DYN_TML_E_NOT_SCALAR] = "escape is not a Unicode scalar value",
    [DYN_TML_E_BAD_ESCAPE] = "invalid escape \\%c",
    [DYN_TML_E_UNTERM] = "unterminated string",
    [DYN_TML_E_CTRL] = "control character in a string",
    [DYN_TML_E_UNTERM_LIT] = "unterminated literal string",
    [DYN_TML_E_CTRL_LIT] = "control character in a literal string",
    [DYN_TML_E_UNTERM_ML] = "unterminated multiline string",
    [DYN_TML_E_QUOTES] = "too many quotation marks",
    [DYN_TML_E_CTRL_ML] = "control character in a multiline string",
    [DYN_TML_E_ML_ESCAPE] = "invalid multiline escape",
    [DYN_TML_E_UNTERM_MLL] = "unterminated multiline literal",
    [DYN_TML_E_APOS] = "too many apostrophes",
    [DYN_TML_E_CTRL_MLL] = "control character in a multiline literal",
    [DYN_TML_E_KEY] = "expected a key",
    [DYN_TML_E_NUM_LONG] = "number too long",
    [DYN_TML_E_INT_RANGE] = "integer out of 64-bit range",
    [DYN_TML_E_DATETIME] = "invalid date-time",
    [DYN_TML_E_VALUE] = "expected a value",
    [DYN_TML_E_NUM] = "malformed number",
    [DYN_TML_E_UNDERSCORE] = "malformed number (underscore placement)",
    [DYN_TML_E_DOT_BEFORE] = "malformed float (no digit before the dot)",
    [DYN_TML_E_DOT_AFTER] = "malformed float (no digit after the dot)",
    [DYN_TML_E_EXP] = "malformed float (no digit after the exponent)",
    [DYN_TML_E_FLOAT] = "malformed float",
    [DYN_TML_E_INT] = "malformed integer",
    [DYN_TML_E_BASE] = "digit outside the number base",
    [DYN_TML_E_LEADING_ZERO] = "leading zero in the integer part",
    [DYN_TML_E_OOM] = "out of memory",
};

void dyn_tml_sb_init(dyn_tml_sb_t* b)
{
    b->p = NULL;
    b->n = 0;
    b->cap = 0;
    b->oom = 0;
}

void dyn_tml_sb_free(dyn_tml_sb_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void dyn_tml_sb_put_slow(dyn_tml_sb_t* b, const char* s, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        char* np;
        while (nc < b->n + n)
            nc *= 2;
        np = (char*)realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

int dyn_tml_fail(dyn_tml_lex_t* t, int code, int arg)
{
    t->ecode = code;
    t->eline = t->line;
    t->ecol = t->col;
    t->earg = arg;
    return -1;
}

const char* dyn_tml_errstr(int code)
{
    if (code <= DYN_TML_E_NONE || code >= DYN_TML_E_COUNT)
        return "";
    return dyn_tml_msgs[code];
}

int dyn_tml_format_error(const dyn_tml_lex_t* t, char* buf, size_t cap)
{
    char msg[192];
    if (t->ecode == DYN_TML_E_BAD_ESCAPE)
        snprintf(msg, sizeof msg, "invalid escape \\%c", t->earg);
    else
        snprintf(msg, sizeof msg, "%s", dyn_tml_errstr(t->ecode));
    return snprintf(buf, cap, "line %d col %d: %s", t->eline, t->ecol, msg);
}

static int dyn_tml_hexdig(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int dyn_tml_ind(int c, int lo, int hi)
{
    c |= 32;
    return c >= lo && c <= hi;
}

static void dyn_tml_utf8(dyn_tml_sb_t* sb, uint32_t cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    dyn_tml_sb_put(sb, b, n);
}

static int dyn_tml_escape(dyn_tml_lex_t* t, dyn_tml_sb_t* sb)
{
    int c = dyn_tml_cur(t);
    if (c == -1) {
        t->col++;
        return dyn_tml_fail(t, DYN_TML_E_BAD_ESCAPE, c);
    }
    dyn_tml_adv(t);
    switch (c) {
    case 'b':
        dyn_tml_sb_putc(sb, '\b');
        return 0;
    case 't':
        dyn_tml_sb_putc(sb, '\t');
        return 0;
    case 'n':
        dyn_tml_sb_putc(sb, '\n');
        return 0;
    case 'f':
        dyn_tml_sb_putc(sb, '\f');
        return 0;
    case 'r':
        dyn_tml_sb_putc(sb, '\r');
        return 0;
    case '"':
        dyn_tml_sb_putc(sb, '"');
        return 0;
    case '\\':
        dyn_tml_sb_putc(sb, '\\');
        return 0;
    case 'u':
    case 'U': {
        int want = (c == 'u') ? 4 : 8, k;
        uint32_t cp = 0;
        for (k = 0; k < want; k++) {
            int h = dyn_tml_cur(t);
            if (!dyn_tml_hexdig(h))
                return dyn_tml_fail(t, DYN_TML_E_BAD_U, 0);
            cp = cp * 16 + (uint32_t)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
            dyn_tml_adv(t);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return dyn_tml_fail(t, DYN_TML_E_NOT_SCALAR, 0);
        dyn_tml_utf8(sb, cp);
        return 0;
    }
    default:
        return dyn_tml_fail(t, DYN_TML_E_BAD_ESCAPE, c);
    }
}

static int dyn_tml_basic_body(dyn_tml_lex_t* t, dyn_tml_sb_t* sb)
{
    int c;
    for (;;) {
        c = dyn_tml_cur(t);
        if (c == -1 || c == '\n')
            return dyn_tml_fail(t, DYN_TML_E_UNTERM, 0);
        if (c == '"') {
            dyn_tml_adv(t);
            return 0;
        }
        if (c == '\\') {
            dyn_tml_adv(t);
            if (dyn_tml_escape(t, sb) < 0)
                return -1;
            continue;
        }
        if ((c < 0x20 && c != '\t') || c == 0x7F)
            return dyn_tml_fail(t, DYN_TML_E_CTRL, 0);
        dyn_tml_sb_putc(sb, (char)c);
        dyn_tml_adv(t);
    }
}

static int dyn_tml_lit_body(dyn_tml_lex_t* t, dyn_tml_sb_t* sb)
{
    int c;
    for (;;) {
        c = dyn_tml_cur(t);
        if (c == -1 || c == '\n')
            return dyn_tml_fail(t, DYN_TML_E_UNTERM_LIT, 0);
        if (c == '\'') {
            dyn_tml_adv(t);
            return 0;
        }
        if ((c < 0x20 && c != '\t') || c == 0x7F)
            return dyn_tml_fail(t, DYN_TML_E_CTRL_LIT, 0);
        dyn_tml_sb_putc(sb, (char)c);
        dyn_tml_adv(t);
    }
}

static void dyn_tml_ml_open(dyn_tml_lex_t* t)
{
    dyn_tml_skip(t, 3);
    if (dyn_tml_cur(t) == '\r' && dyn_tml_peek(t, 1) == '\n') {
        dyn_tml_adv(t);
        dyn_tml_adv(t);
    } else if (dyn_tml_cur(t) == '\n')
        dyn_tml_adv(t);
}

static int dyn_tml_ml_close(dyn_tml_lex_t* t, dyn_tml_sb_t* sb, int q, int too_many)
{
    int run = 0, k;
    while (dyn_tml_peek(t, run) == q)
        run++;
    if (run > 5)
        return dyn_tml_fail(t, too_many, 0);
    if (run < 3)
        return 0;
    for (k = 0; k < run - 3; k++)
        dyn_tml_sb_putc(sb, (char)q);
    dyn_tml_skip(t, run);
    return 1;
}

static int dyn_tml_ml_basic(dyn_tml_lex_t* t, dyn_tml_sb_t* sb)
{
    int c, r;
    dyn_tml_ml_open(t);
    for (;;) {
        c = dyn_tml_cur(t);
        if (c == -1)
            return dyn_tml_fail(t, DYN_TML_E_UNTERM_ML, 0);
        if (c == '"' && dyn_tml_peek(t, 1) == '"') {
            if ((r = dyn_tml_ml_close(t, sb, '"', DYN_TML_E_QUOTES)) != 0)
                return r < 0 ? -1 : 0;
        }
        if ((c < 0x20 && c != '\t' && c != '\n' && !(c == '\r' && dyn_tml_peek(t, 1) == '\n')) || c == 0x7F)
            return dyn_tml_fail(t, DYN_TML_E_CTRL_ML, 0);
        if (c == '\\') {
            dyn_tml_adv(t);
            c = dyn_tml_cur(t);
            if (c == ' ' || c == '\t') {
                while (c == ' ' || c == '\t') {
                    dyn_tml_adv(t);
                    c = dyn_tml_cur(t);
                }
                if (c != '\n' && !(c == '\r' && dyn_tml_peek(t, 1) == '\n'))
                    return dyn_tml_fail(t, DYN_TML_E_ML_ESCAPE, 0);
            }
            if (c == '\n' || (c == '\r' && dyn_tml_peek(t, 1) == '\n')) {
                while ((c = dyn_tml_cur(t)) == ' ' || c == '\t' || c == '\n' || (c == '\r' && dyn_tml_peek(t, 1) == '\n'))
                    dyn_tml_adv(t);
                continue;
            }
            if (dyn_tml_escape(t, sb) < 0)
                return -1;
            continue;
        }
        if (c == '\r' && dyn_tml_peek(t, 1) == '\n') {
            dyn_tml_adv(t);
            continue;
        }
        dyn_tml_sb_putc(sb, (char)c);
        dyn_tml_adv(t);
    }
}

static int dyn_tml_ml_lit(dyn_tml_lex_t* t, dyn_tml_sb_t* sb)
{
    int c, r;
    dyn_tml_ml_open(t);
    for (;;) {
        c = dyn_tml_cur(t);
        if (c == -1)
            return dyn_tml_fail(t, DYN_TML_E_UNTERM_MLL, 0);
        if (c == '\'' && dyn_tml_peek(t, 1) == '\'') {
            if ((r = dyn_tml_ml_close(t, sb, '\'', DYN_TML_E_APOS)) != 0)
                return r < 0 ? -1 : 0;
        }
        if ((c < 0x20 && c != '\t' && c != '\n' && !(c == '\r' && dyn_tml_peek(t, 1) == '\n')) || c == 0x7F)
            return dyn_tml_fail(t, DYN_TML_E_CTRL_MLL, 0);
        if (c == '\r' && dyn_tml_peek(t, 1) == '\n') {
            dyn_tml_adv(t);
            continue;
        }
        dyn_tml_sb_putc(sb, (char)c);
        dyn_tml_adv(t);
    }
}

int dyn_tml_string(dyn_tml_lex_t* t, dyn_tml_sb_t* sb)
{
    int c = dyn_tml_cur(t), r;
    if (c == '"' && dyn_tml_peek(t, 1) == '"' && dyn_tml_peek(t, 2) == '"')
        r = dyn_tml_ml_basic(t, sb);
    else if (c == '\'' && dyn_tml_peek(t, 1) == '\'' && dyn_tml_peek(t, 2) == '\'')
        r = dyn_tml_ml_lit(t, sb);
    else {
        dyn_tml_adv(t);
        r = c == '"' ? dyn_tml_basic_body(t, sb) : dyn_tml_lit_body(t, sb);
    }
    if (r == 0 && sb->oom)
        return dyn_tml_fail(t, DYN_TML_E_OOM, 0);
    return r;
}

void dyn_tml_key_init(dyn_tml_key_t* k)
{
    memset(k, 0, sizeof *k);
    k->p = k->inl;
    k->cap = DYN_TML_KEY_INL;
    k->seg = k->seginl;
    k->segcap = DYN_TML_KEY_SEGS;
}

void dyn_tml_key_free(dyn_tml_key_t* k)
{
    if (k->p != k->inl)
        free(k->p);
    if (k->seg != k->seginl)
        free(k->seg);
    dyn_tml_key_init(k);
}

static int dyn_tml_key_grow(dyn_tml_key_t* k, size_t bytes)
{
    if (k->n + bytes > k->cap) {
        size_t nc = k->cap * 2;
        char* np;
        while (nc < k->n + bytes)
            nc *= 2;
        np = (char*)realloc(k->p == k->inl ? NULL : k->p, nc);
        if (!np) {
            k->oom = 1;
            return -1;
        }
        if (k->p == k->inl)
            memcpy(np, k->inl, k->n);
        k->p = np;
        k->cap = nc;
    }
    return 0;
}

static int dyn_tml_key_pushseg(dyn_tml_key_t* k, const char* s, size_t len)
{
    if (k->nseg == k->segcap) {
        size_t nc = k->segcap * 2;
        dyn_tml_seg_t* ns = (dyn_tml_seg_t*)realloc(k->seg == k->seginl ? NULL : k->seg,
            nc * sizeof *k->seg);
        if (!ns) {
            k->oom = 1;
            return -1;
        }
        if (k->seg == k->seginl)
            memcpy(ns, k->seginl, k->nseg * sizeof *k->seg);
        k->seg = ns;
        k->segcap = nc;
    }
    if (dyn_tml_key_grow(k, len) < 0)
        return -1;
    if (len)
        memcpy(k->p + k->n, s, len);
    k->seg[k->nseg].off = k->n;
    k->seg[k->nseg].len = len;
    k->n += len;
    k->nseg++;
    return 0;
}

int dyn_tml_key_parse(dyn_tml_lex_t* t, dyn_tml_key_t* k)
{
    int c;
    dyn_tml_ws(t);
    for (;;) {
        c = dyn_tml_cur(t);
        if (c == '"' || c == '\'') {
            dyn_tml_sb_t sb;
            int r;
            dyn_tml_sb_init(&sb);
            dyn_tml_adv(t);
            r = c == '"' ? dyn_tml_basic_body(t, &sb) : dyn_tml_lit_body(t, &sb);
            if (r == 0 && (sb.oom || dyn_tml_key_pushseg(k, sb.p ? sb.p : "", sb.n) < 0))
                r = dyn_tml_fail(t, DYN_TML_E_OOM, 0);
            dyn_tml_sb_free(&sb);
            if (r < 0)
                return -1;
        } else if (dyn_tml_key_char(c)) {
            size_t start = t->i;
            while ((c = dyn_tml_cur(t)) != -1 && dyn_tml_key_char(c))
                dyn_tml_adv(t);
            if (dyn_tml_key_pushseg(k, t->src + start, t->i - start) < 0)
                return dyn_tml_fail(t, DYN_TML_E_OOM, 0);
        } else
            return dyn_tml_fail(t, DYN_TML_E_KEY, 0);
        dyn_tml_ws(t);
        c = dyn_tml_cur(t);
        if (c != '.')
            return 0;
        dyn_tml_adv(t);
        dyn_tml_ws(t);
    }
}

static int dyn_tml_number(dyn_tml_lex_t* t, const char* tok, size_t n, int is_float,
    dyn_tml_scalar_t* v, dyn_tml_atod_fn atod)
{
    char clean[128];
    size_t j, k;
    if (n >= sizeof clean)
        return dyn_tml_fail(t, DYN_TML_E_NUM_LONG, 0);
    k = 0;
    for (j = 0; j < n; j++)
        if (tok[j] != '_')
            clean[k++] = tok[j];
    clean[k] = 0;
    if (is_float) {
        v->kind = DYN_TML_V_FLOAT;
        v->d = atod(clean, NULL);
        return 0;
    }
    {
        int sign = 1, base = 10;
        uint64_t u = 0;
        j = 0;
        if (clean[0] == '+' || clean[0] == '-') {
            sign = clean[0] == '-' ? -1 : 1;
            j = 1;
        }
        if (clean[j] == '0' && j + 1 < k && (clean[j + 1] | 32) == 'x') {
            base = 16;
            j += 2;
        } else if (clean[j] == '0' && j + 1 < k && (clean[j + 1] | 32) == 'o') {
            base = 8;
            j += 2;
        } else if (clean[j] == '0' && j + 1 < k && (clean[j + 1] | 32) == 'b') {
            base = 2;
            j += 2;
        }
        for (; j < k; j++) {
            int d = clean[j] <= '9' ? clean[j] - '0' : (clean[j] | 32) - 'a' + 10;
            if (u > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
                return dyn_tml_fail(t, DYN_TML_E_INT_RANGE, 0);
            u = u * (uint64_t)base + (uint64_t)d;
        }
        v->kind = DYN_TML_V_INT;
        if (sign > 0) {
            if (u > (uint64_t)INT64_MAX)
                return dyn_tml_fail(t, DYN_TML_E_INT_RANGE, 0);
            v->i = (int64_t)u;
            return 0;
        }
        if (u > (uint64_t)INT64_MAX + 1)
            return dyn_tml_fail(t, DYN_TML_E_INT_RANGE, 0);
        v->i = u == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)u;
        return 0;
    }
}

static int dyn_tml_d2(const char* s, size_t i) { return dyn_tml_digit((unsigned char)s[i]) && dyn_tml_digit((unsigned char)s[i + 1]); }

static int dyn_tml_v2(const char* s, size_t i) { return (s[i] - '0') * 10 + (s[i + 1] - '0'); }

static int dyn_tml_hms(const dyn_tml_lex_t* t, size_t* pi)
{
    const char* s = t->src;
    size_t i = *pi;
    if (i + 8 > t->n || !dyn_tml_d2(s, i) || s[i + 2] != ':' || !dyn_tml_d2(s, i + 3) || s[i + 5] != ':' || !dyn_tml_d2(s, i + 6))
        return -1;
    if (dyn_tml_v2(s, i) > 23 || dyn_tml_v2(s, i + 3) > 59 || dyn_tml_v2(s, i + 6) > 60)
        return -1;
    i += 8;
    if (i < t->n && s[i] == '.') {
        i++;
        if (i >= t->n || !dyn_tml_digit((unsigned char)s[i]))
            return -1;
        while (i < t->n && dyn_tml_digit((unsigned char)s[i]))
            i++;
    }
    *pi = i;
    return 0;
}

static int dyn_tml_datetime(dyn_tml_lex_t* t, dyn_tml_scalar_t* v)
{
    size_t i = t->i;
    const char* s = t->src;
    if (dyn_tml_peek(t, 2) == ':') {
        if (dyn_tml_hms(t, &i) < 0)
            goto bad;
    } else {
        int mo, dd;
        if (i + 10 > t->n || !dyn_tml_d2(s, i) || !dyn_tml_d2(s, i + 2) || s[i + 4] != '-' || !dyn_tml_d2(s, i + 5) || s[i + 7] != '-' || !dyn_tml_d2(s, i + 8))
            goto bad;
        mo = dyn_tml_v2(s, i + 5);
        dd = dyn_tml_v2(s, i + 8);
        if (mo < 1 || mo > 12 || dd < 1 || dd > 31)
            goto bad;
        i += 10;
        if (i < t->n && (s[i] == 'T' || s[i] == 't' || (s[i] == ' ' && i + 3 < t->n && dyn_tml_d2(s, i + 1) && s[i + 3] == ':'))) {
            i++;
            if (dyn_tml_hms(t, &i) < 0)
                goto bad;
            if (i < t->n && (s[i] == 'Z' || s[i] == 'z'))
                i++;
            else if (i < t->n && (s[i] == '+' || s[i] == '-')) {
                i++;
                if (i + 5 > t->n || !dyn_tml_d2(s, i) || s[i + 2] != ':' || !dyn_tml_d2(s, i + 3))
                    goto bad;
                if (dyn_tml_v2(s, i) > 23 || dyn_tml_v2(s, i + 3) > 59)
                    goto bad;
                i += 5;
            }
        }
    }
    v->kind = DYN_TML_V_DATETIME;
    v->off = t->i;
    v->len = i - t->i;
    t->col += (int)(i - t->i);
    t->i = i;
    return 0;
bad:
    return dyn_tml_fail(t, DYN_TML_E_DATETIME, 0);
}

static int dyn_tml_numch(int c)
{
    return dyn_tml_digit(c) || c == '_' || c == '.' || c == 'e' || c == 'E' || c == 'x' || c == 'X' || c == 'o' || c == 'O' || c == 'b' || c == 'B' || c == 'a' || c == 'f' || c == 'A' || c == 'F' || c == 'c' || c == 'C' || c == 'd' || c == 'D';
}

static int dyn_tml_prefixed(const char* p)
{
    return p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'o' || p[1] == 'O' || p[1] == 'b' || p[1] == 'B');
}

static int dyn_tml_numtok(dyn_tml_lex_t* t, size_t start, dyn_tml_scalar_t* v, dyn_tml_atod_fn atod)
{
    const char* tok = t->src + start;
    size_t n = t->i - start, k = 0, j, tj;
    int digits = 0, seen_dot = 0, seen_exp = 0, hi = '9', lo = '0', is_float = 0;
    if (!((n >= 2 && dyn_tml_prefixed(tok)) || (n >= 3 && (tok[0] == '+' || tok[0] == '-') && dyn_tml_prefixed(tok + 1)))) {
        for (tj = 0; tj < n; tj++)
            if (tok[tj] == '.' || tok[tj] == 'e' || tok[tj] == 'E')
                is_float = 1;
    }
    if (tok[0] == '+' || tok[0] == '-')
        k = 1;
    if (k + 1 < n && tok[k] == '0' && (tok[k + 1] == 'x' || tok[k + 1] == 'X')) {
        hi = 'f';
        k += 2;
    } else if (k + 1 < n && tok[k] == '0' && (tok[k + 1] == 'o' || tok[k + 1] == 'O')) {
        hi = '7';
        k += 2;
    } else if (k + 1 < n && tok[k] == '0' && (tok[k + 1] == 'b' || tok[k + 1] == 'B')) {
        hi = '1';
        k += 2;
    }
    for (j = k; j < n; j++) {
        char ch = tok[j];
        if (ch == '_') {
            if (j == 0 || !dyn_tml_ind(tok[j - 1], lo, hi) || j + 1 >= n || !dyn_tml_ind(tok[j + 1], lo, hi))
                return dyn_tml_fail(t, DYN_TML_E_UNDERSCORE, 0);
            continue;
        }
        if (is_float) {
            if (dyn_tml_digit((unsigned char)ch)) {
                digits++;
                continue;
            }
            if (ch == '.' && !seen_dot && !seen_exp) {
                if (digits == 0)
                    return dyn_tml_fail(t, DYN_TML_E_DOT_BEFORE, 0);
                seen_dot = 1;
                if (j + 1 >= n || !dyn_tml_digit((unsigned char)tok[j + 1]))
                    return dyn_tml_fail(t, DYN_TML_E_DOT_AFTER, 0);
                continue;
            }
            if ((ch == 'e' || ch == 'E') && !seen_exp && digits > 0) {
                size_t e2 = j + 1;
                seen_exp = 1;
                if (e2 < n && (tok[e2] == '+' || tok[e2] == '-'))
                    e2++;
                if (e2 >= n || !dyn_tml_digit((unsigned char)tok[e2]))
                    return dyn_tml_fail(t, DYN_TML_E_EXP, 0);
                if (j + 1 < n && (tok[j + 1] == '+' || tok[j + 1] == '-'))
                    j++;
                continue;
            }
            return dyn_tml_fail(t, DYN_TML_E_FLOAT, 0);
        }
        if (!dyn_tml_hexdig((unsigned char)ch))
            return dyn_tml_fail(t, DYN_TML_E_INT, 0);
        if ((ch | 32) > hi || (ch | 32) < lo)
            return dyn_tml_fail(t, DYN_TML_E_BASE, 0);
        digits++;
    }
    if (digits == 0)
        return dyn_tml_fail(t, DYN_TML_E_NUM, 0);
    {
        size_t k0 = (tok[0] == '+' || tok[0] == '-') ? 1 : 0;
        if (!(n >= k0 + 3 && tok[k0] == '0' && ((tok[k0 + 1] | 32) == 'x' || (tok[k0 + 1] | 32) == 'o' || (tok[k0 + 1] | 32) == 'b'))) {
            if (k0 < n && tok[k0] == '0' && k0 + 1 < n && tok[k0 + 1] != '.' && tok[k0 + 1] != 'e' && tok[k0 + 1] != 'E')
                return dyn_tml_fail(t, DYN_TML_E_LEADING_ZERO, 0);
        }
    }
    return dyn_tml_number(t, tok, n, is_float, v, atod);
}

static int dyn_tml_word(const dyn_tml_lex_t* t, const char* w, size_t n)
{
    return t->i + n <= t->n && memcmp(t->src + t->i, w, n) == 0;
}

static int dyn_tml_special(dyn_tml_lex_t* t, dyn_tml_scalar_t* v, int len, double d)
{
    dyn_tml_skip(t, len);
    v->kind = DYN_TML_V_FLOAT;
    v->d = d;
    return 0;
}

int dyn_tml_scalar(dyn_tml_lex_t* t, dyn_tml_scalar_t* v, dyn_tml_atod_fn atod)
{
    int c = dyn_tml_cur(t);
    size_t start;
    if (c == 't' && dyn_tml_word(t, "true", 4)) {
        dyn_tml_skip(t, 4);
        v->kind = DYN_TML_V_BOOL;
        v->b = 1;
        return 0;
    }
    if (c == 'f' && dyn_tml_word(t, "false", 5)) {
        dyn_tml_skip(t, 5);
        v->kind = DYN_TML_V_BOOL;
        v->b = 0;
        return 0;
    }
    if (c == '+' || c == '-') {
        if (dyn_tml_peek(t, 1) == 'i' && dyn_tml_peek(t, 2) == 'n' && dyn_tml_peek(t, 3) == 'f')
            return dyn_tml_special(t, v, 4, c == '-' ? -HUGE_VAL : HUGE_VAL);
        if (dyn_tml_peek(t, 1) == 'n' && dyn_tml_peek(t, 2) == 'a' && dyn_tml_peek(t, 3) == 'n')
            return dyn_tml_special(t, v, 4, (double)NAN);
    }
    if (c == 'i' && dyn_tml_word(t, "inf", 3))
        return dyn_tml_special(t, v, 3, HUGE_VAL);
    if (c == 'n' && dyn_tml_word(t, "nan", 3))
        return dyn_tml_special(t, v, 3, (double)NAN);
    if (dyn_tml_digit(c) && ((dyn_tml_peek(t, 4) == '-' && dyn_tml_peek(t, 7) == '-') || dyn_tml_peek(t, 2) == ':'))
        return dyn_tml_datetime(t, v);
    start = t->i;
    if (c == '+' || c == '-') {
        dyn_tml_adv(t);
        c = dyn_tml_cur(t);
    }
    while (c != -1 && dyn_tml_numch(c)) {
        dyn_tml_adv(t);
        c = dyn_tml_cur(t);
        if ((c == '+' || c == '-') && (t->src[t->i - 1] == 'e' || t->src[t->i - 1] == 'E')) {
            dyn_tml_adv(t);
            c = dyn_tml_cur(t);
        }
    }
    if (t->i == start || (t->i == start + 1 && (t->src[start] == '+' || t->src[start] == '-')))
        return dyn_tml_fail(t, DYN_TML_E_VALUE, 0);
    if ((c = dyn_tml_cur(t)) != -1 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || dyn_tml_digit(c)))
        return dyn_tml_fail(t, DYN_TML_E_NUM, 0);
    return dyn_tml_numtok(t, start, v, atod);
}
