/* dyna:yaml -- the YAML 1.2 core schema, block and flow, and NOTHING else.
   Anchors, aliases, tags, merge keys and directives are REFUSED by name rather
   than ignored: a config parser that silently drops an anchor returns a wrong
   document. Full grammar and the refusal list: see the parser source. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_YAML)

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/dyn-sb.h"
#include "dyna-simd-kernels.h"   /* simd.find_u8 for the line split */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define YML_MAX_DEPTH  128
#define YML_MAX_INPUT  (64u << 20)
/* The line table is 24 bytes per line: without a cap, 64 MB of "x\n" would
 * allocate ~768 MB (~12x the input) before parsing even starts. 4M lines is
 * far past any real config and bounds the table at ~96 MB. */
#define YML_MAX_LINES  4000000u

/* A native module does not reach for the engine's DynBuf; the shared core
   (core/dyn-sb.h) is pure C, so yb_t wraps it and keeps its own seed (128)
   and sticky-oom convention here. */
typedef struct { uint8_t *p; size_t n, cap; int oom; } yb_t;

static void yb_init(yb_t *b) { b->p = NULL; b->n = b->cap = 0; b->oom = 0; }
static void yb_free(yb_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void yb_write(yb_t *b, const void *p, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void **)&b->p, &b->cap, b->n + n, 128)) {
        b->oom = 1;
        return;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void yb_put(yb_t *b, uint8_t c) { yb_write(b, &c, 1); }
static void yb_puts(yb_t *b, const char *t) { yb_write(b, t, strlen(t)); }

/* ------------------------------------------------------------------ lines */

typedef struct {
    size_t raw, start, end;             /* line start, content start, line end */
    int    indent;
    int    blank;                       /* empty, or a comment on its own */
    int    tab;                         /* a tab indents a CONTENT line (the
                                           refusal is applied where the
                                           content is consumed, not up front:
                                           a quoted scalar's escaped break
                                           EATS the next line's leading
                                           whitespace, tabs included, so
                                           there the tab is not indentation) */
} yml_line_t;

typedef struct {
    JSContext  *ctx;
    const char *s;
    size_t      n;
    yml_line_t *ln;
    uint32_t    nln, li;
    int         depth;
    int         max_depth;              /*lowering of YML_MAX_DEPTH */
    char        err[192];
} yml_t;

static JSValue yml_fail(yml_t *p, const char *what)
{
    uint32_t line = p->li < p->nln ? p->li + 1 : p->nln;
    if (!p->err[0])
        snprintf(p->err, sizeof p->err, "%s at line %u", what, (unsigned)line);
    return JS_EXCEPTION;
}

static int yml_split(yml_t *p)
{
    size_t i = 0;
    uint32_t cap = 16;

    p->ln = (yml_line_t *)malloc(cap * sizeof *p->ln);
    if (!p->ln)
        return -1;
    while (i <= p->n) {
        yml_line_t L;
        size_t j = i, k, t;
        /* Line end via the memchr-class kernel jump (deep-sweep D-5);
           SIZE_MAX means no newline -- take the rest of the buffer. */
        t = simd.find_u8((const uint8_t *)p->s + j, '\n', p->n - j);
        j = (t == (size_t)-1) ? p->n : j + t;
        L.raw = i;
        L.end = j;
        if (L.end > L.raw && p->s[L.end - 1] == '\r')
            L.end--;                    /* a CRLF file is not a different file */
        k = i;
        while (k < L.end && p->s[k] == ' ') k++;
        L.indent = (int)(k - i);
        L.start = k;
        {
            /* A line of spaces and tabs -- or a comment -- carries no node;
               only spaces COUNT toward indentation, but a tab-only line is
               still blank, not content. */
            size_t kb = k, kw = i;
            int hastab = 0;
            while (kb < L.end && (p->s[kb] == ' ' || p->s[kb] == '\t')) kb++;
            L.blank = (kb >= L.end) || p->s[kb] == '#';
            while (kw < L.end && (p->s[kw] == ' ' || p->s[kw] == '\t')) {
                if (p->s[kw] == '\t')
                    hastab = 1;
                kw++;
            }
            L.tab = hastab && kw < L.end && p->s[kw] != '#';
        }
        if (p->nln >= YML_MAX_LINES) {
            snprintf(p->err, sizeof p->err, "the input exceeds %u lines",
                     (unsigned)YML_MAX_LINES);
            return -1;
        }
        if (p->nln == cap) {
            yml_line_t *np;
            cap += cap / 2;
            np = (yml_line_t *)realloc(p->ln, cap * sizeof *p->ln);
            if (!np)
                return -1;
            p->ln = np;
        }
        p->ln[p->nln++] = L;
        if (j >= p->n)
            break;
        i = j + 1;
        if (i >= p->n)
            break;                      /* the final newline ends the last line */
    }
    return 0;
}

/* A tab in the INDENTATION of a line that then has content is an error in
   YAML, and the failure it otherwise produces is a structure the author did
   not write. A whitespace-only line -- or a tab-indented comment -- has no
   indentation semantics and is left to the blank-line handling. The refusal
   is applied WHEN the line's content is consumed (yml_tab_refuse at the
   token positions), not up front: the leading whitespace a double-quoted
   scalar's ESCAPED break eats is not indentation -- PyYAML folds it away --
   while a tab indenting any other content line stays refused. */
static int yml_tab_refuse(yml_t *p, uint32_t li)
{
    if (li < p->nln && p->ln[li].tab) {
        p->li = li;
        yml_fail(p, "a tab may not indent a line");
        return 1;
    }
    return 0;
}

static void yml_skip_blank(yml_t *p)
{
    while (p->li < p->nln && p->ln[p->li].blank)
        p->li++;
}

/* A marker line: the three bytes, then blank. `---a` is NOT a marker -- it
   is the plain scalar the author wrote -- and `--- 5` carries the document
   node on the marker line itself. */
static int yml_marker_at(yml_t *p, uint32_t li, const char *m)
{
    const yml_line_t *L;
    size_t k;
    if (li >= p->nln)
        return 0;
    L = &p->ln[li];
    if (L->end - L->start < 3 || memcmp(p->s + L->start, m, 3) != 0)
        return 0;
    k = L->start + 3;
    return k >= L->end || p->s[k] == ' ' || p->s[k] == '\t';
}

/* ---------------------------------------------------------------- scalars */

/* The end of a plain scalar: an unquoted ` #` after whitespace starts a
   comment; trailing spaces and tabs are never content. */
static size_t yml_plain_end(const char *s, size_t start, size_t end)
{
    size_t i = start, last = start;
    while (i < end) {
        if (s[i] == '#' && i > start
            && (s[i - 1] == ' ' || s[i - 1] == '\t'))
            break;
        if (s[i] != ' ' && s[i] != '\t')
            last = i + 1;
        i++;
    }
    return last;
}

/* Digits in one radix, or 0. Overflow is a refusal, not a wrap. */
static int yml_digits(const char *s, size_t i, size_t n, int base, int64_t *v)
{
    if (i >= n)
        return 0;
    for (; i < n; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (base == 16 && s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else if (base == 16 && s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
        else return 0;
        if (d >= base || *v > (INT64_MAX - d) / base)
            return 0;
        *v = *v * base + d;
    }
    return 1;
}

static int yml_is_int(const char *s, size_t n, int64_t *out)
{
    size_t i = 0;
    int neg = 0, base = 10;
    int64_t v = 0;

    if (!n)
        return 0;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    if (i + 2 < n && s[i] == '0') {
        if (s[i + 1] == 'x' || s[i + 1] == 'X') { base = 16; i += 2; }
        else if (s[i + 1] == 'o' || s[i + 1] == 'O') { base = 8; i += 2; }
    }
    if (!yml_digits(s, i, n, base, &v))
        return 0;
    *out = neg ? -v : v;
    return 1;
}

static int yml_is_float(const char *s, size_t n)
{
    size_t i = 0;
    int digits = 0, dot = 0;
    if (!n)
        return 0;
    if (s[0] == '-' || s[0] == '+') i = 1;
    for (; i < n; i++) {
        if (s[i] >= '0' && s[i] <= '9') { digits = 1; continue; }
        if (s[i] == '.' && !dot) { dot = 1; continue; }
        if ((s[i] == 'e' || s[i] == 'E') && digits) {
            i++;
            if (i < n && (s[i] == '-' || s[i] == '+')) i++;
            if (i >= n) return 0;
            for (; i < n; i++)
                if (s[i] < '0' || s[i] > '9') return 0;
            return 1;
        }
        return 0;
    }
    return digits && dot;
}

static int yml_eq(const char *s, size_t n, const char *lit)
{
    return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

/* YAML 1.2 CORE SCHEMA. `yes`, `no`, `on` and `off` are STRINGS here, which is
   the 1.2 rule and the end of the Norway problem. */
static JSValue yml_scalar(JSContext *ctx, const char *s, size_t n)
{
    int64_t iv;

    /* BYPASS: the first byte decides which family is even possible. Every
       keyword starts with ~nNtTfF. and every number with +-. or a digit, so a
       plain scalar starting with an ordinary letter -- the common case -- skips
       17 comparisons and two full scans. Exact: no skipped branch could match. */
    if (n != 0) {
        switch (s[0]) {
        case '~': case 'n': case 'N': case 't': case 'T': case 'f': case 'F':
        case '.': case '+': case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            break;
        default:
            return JS_NewStringLen(ctx, s, n);
        }
    }

    if (n == 0 || yml_eq(s, n, "~") || yml_eq(s, n, "null")
        || yml_eq(s, n, "Null") || yml_eq(s, n, "NULL"))
        return JS_NULL;
    if (yml_eq(s, n, "true") || yml_eq(s, n, "True") || yml_eq(s, n, "TRUE"))
        return JS_TRUE;
    if (yml_eq(s, n, "false") || yml_eq(s, n, "False") || yml_eq(s, n, "FALSE"))
        return JS_FALSE;
    /* The 1.2 core-schema float rule is [-+]?(\.inf|\.Inf|\.INF): every one of
       the twelve spellings is a number, and reading any of them as a string
       silently retypes the value. */
    if (yml_eq(s, n, ".inf") || yml_eq(s, n, ".Inf") || yml_eq(s, n, ".INF")
        || yml_eq(s, n, "+.inf") || yml_eq(s, n, "+.Inf") || yml_eq(s, n, "+.INF"))
        return JS_NewFloat64(ctx, INFINITY);
    if (yml_eq(s, n, "-.inf") || yml_eq(s, n, "-.Inf") || yml_eq(s, n, "-.INF"))
        return JS_NewFloat64(ctx, -INFINITY);
    if (yml_eq(s, n, ".nan") || yml_eq(s, n, ".NaN") || yml_eq(s, n, ".NAN"))
        return JS_NewFloat64(ctx, NAN);
    if (yml_is_int(s, n, &iv))
        return JS_NewInt64(ctx, iv);
    if (yml_is_float(s, n)) {
        /* The engine's own ToNumber: correctly rounded, and locale-independent
           where strtod would read LC_NUMERIC for the radix character. */
        JSValue sv = JS_NewStringLen(ctx, s, n), r;
        double d;
        if (JS_IsException(sv))
            return sv;
        r = JS_ToFloat64(ctx, &d, sv) < 0 ? JS_EXCEPTION : JS_NewFloat64(ctx, d);
        JS_FreeValue(ctx, sv);
        return r;
    }
    return JS_NewStringLen(ctx, s, n);
}

/* One code point as UTF-8. Surrogates never reach here: they are rejected or
   joined by the \u/\U case before the write. */
static void yml_utf8_put(yb_t *b, uint32_t cp)
{
    if (cp < 0x80) {
        yb_put(b, (uint8_t)cp);
    } else if (cp < 0x800) {
        yb_put(b, (uint8_t)(0xC0 | (cp >> 6)));
        yb_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        yb_put(b, (uint8_t)(0xE0 | (cp >> 12)));
        yb_put(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        yb_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else {
        yb_put(b, (uint8_t)(0xF0 | (cp >> 18)));
        yb_put(b, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
        yb_put(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        yb_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    }
}

/* Hex digits of the given width, or -1. */
static int yml_hex(const char *s, size_t n, int width, uint32_t *out)
{
    uint32_t cp = 0;
    int k;

    for (k = 0; k < width; k++) {
        char h = s[n + (size_t)k];
        int d = (h >= '0' && h <= '9') ? h - '0'
              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
        if (d < 0)
            return -1;
        cp = cp * 16 + (uint32_t)d;
    }
    *out = cp;
    return 0;
}

/* A `---` / `...` at a continuation line's head (the raw column 0, the
   position every YAML reader tests): the document separator is not scalar
   content and must not fold into one. YAML 1.2's c-directives-end /
   c-document-end are terminated markers -- the three bytes must be followed
   by a blank or end-of-line (the same rule yml_marker_at enforces at block
   level): `---x`, `---- b`, `...x` and `.... b` are scalar CONTENT and fold
   exactly like PyYAML 6.0.3 folds them. */
static int yml_docsep_at(const yml_t *p, uint32_t li)
{
    const yml_line_t *L = &p->ln[li];
    size_t k;
    if (L->end - L->raw < 3)
        return 0;
    if (memcmp(p->s + L->raw, "---", 3) != 0
        && memcmp(p->s + L->raw, "...", 3) != 0)
        return 0;
    k = L->raw + 3;
    return k >= L->end || p->s[k] == ' ' || p->s[k] == '\t';
}

/* A quoted scalar's content: the YAML 1.2 flow-scalar rules (spec 7.3), with
   folding for scalars that span lines. Single quotes take '' and nothing
   else; double quotes take the full escape table.
   `multiline` allows the scalar to continue past the current line's end (the
   block-value shape); without it a break is an unterminated scalar, because
   the single-line callers (keys, flow collections) have bounds that are not
   the physical line end. p->li ends on the line holding the closing quote.

   Folding (matched against PyYAML 6.0.3, the differential oracle in
   tests/test_yaml_folding.js):
   - raw trailing spaces/tabs before an UNESCAPED break are dropped, as is
     the continuation line's leading whitespace;
   - one break folds to a single space; a run of n >= 2 breaks folds to n-1
     line feeds (a whitespace-only line inside the run is one more break);
   - an escaped break (double-quoted backslash at end of line) eats the break
     AND the next line's leading whitespace, contributing nothing; each BREAK
     AFTER it contributes one line feed (blank-line paragraphs after a
     continuation keep them);
   - escaped spaces (and other escapes) are content and never trimmed;
   - a `---` / `...` document separator at the head of a continuation line is
     refused by name: it is not scalar content, it ends the document. */
static JSValue yml_quoted(yml_t *p, size_t *pi, size_t end, int multiline)
{
    char q = p->s[*pi];
    size_t i = *pi + 1;
    yb_t b;
    size_t hard = 0;   /* b.n past the last non-trimmable byte */

    yb_init(&b);
    for (;;) {
    int esc_break = 0;
    while (i < end) {
        char c = p->s[i];
        if (c == q) {
            if (q == '\'' && i + 1 < end && p->s[i + 1] == '\'') {
                yb_put(&b, (uint8_t)'\'');
                hard = b.n;
                i += 2;
                continue;
            }
            i++;
            *pi = i;
            {
                JSValue v = JS_NewStringLen(p->ctx, (const char *)b.p, b.n);
                yb_free(&b);
                return v;
            }
        }
        if (q == '"' && c == '\\') {
            i++;
            if (i >= end) {
                /* the backslash escapes the line break */
                esc_break = 1;
                break;
            }
            switch (p->s[i]) {
            case 'n': yb_put(&b, (uint8_t)'\n'); break;
            case 't': yb_put(&b, (uint8_t)'\t'); break;
            case 'r': yb_put(&b, (uint8_t)'\r'); break;
            case '0': yb_put(&b, (uint8_t)'\0'); break;
            case 'a': yb_put(&b, (uint8_t)'\a'); break;
            case 'b': yb_put(&b, (uint8_t)'\b'); break;
            case 'v': yb_put(&b, (uint8_t)'\v'); break;
            case 'f': yb_put(&b, (uint8_t)'\f'); break;
            case 'e': yb_put(&b, (uint8_t)0x1B); break;
            case ' ': yb_put(&b, (uint8_t)' '); break;
            case '\\': yb_put(&b, (uint8_t)'\\'); break;
            case '"': yb_put(&b, (uint8_t)'"'); break;
            case '/': yb_put(&b, (uint8_t)'/'); break;
            case 'N': yb_puts(&b, "\xC2\x85"); break;      /* NEL U+0085 */
            case '_': yb_puts(&b, "\xC2\xA0"); break;      /* NBSP U+00A0 */
            case 'L': yb_puts(&b, "\xE2\x80\xA8"); break;  /* LS U+2028 */
            case 'P': yb_puts(&b, "\xE2\x80\xA9"); break;  /* PS U+2029 */
            case 'x': case 'u': case 'U': {
                /* \xHH, \uHHHH, \UHHHHHHHH: the same resolution, wider hex.
                   A surrogate is corrupt alone; \u joins a well-formed pair. */
                int width = p->s[i] == 'x' ? 2 : p->s[i] == 'u' ? 4 : 8;
                uint32_t cp;
                if (i + (size_t)width >= end
                    || yml_hex(p->s, i + 1, width, &cp) < 0) {
                    yb_free(&b);
                    return yml_fail(p, "bad \\u escape");
                }
                if (cp > 0x10FFFF) {
                    yb_free(&b);
                    return yml_fail(p, "code point past U+10FFFF");
                }
                i += (size_t)width;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* high surrogate: a well-formed pair is "\uXXXX\uYYYY";
                       join it, a lone one is corrupt UTF-8 like XML's rule */
                    uint32_t lo = 0;
                    int ok = 0;
                    if (i + 7 < end && p->s[i + 1] == '\\' && p->s[i + 2] == 'u') {
                        ok = yml_hex(p->s, i + 3, 4, &lo) == 0
                             && lo >= 0xDC00 && lo <= 0xDFFF;
                    }
                    if (!ok) { yb_free(&b); return yml_fail(p, "lone surrogate \\u escape"); }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6; /* consumed "\uXXXX" of the low half too */
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    yb_free(&b);
                    return yml_fail(p, "lone surrogate \\u escape");
                }
                yml_utf8_put(&b, cp);
                break;
            }
            default:
                yb_free(&b);
                return yml_fail(p, "unknown escape in a double-quoted scalar");
            }
            hard = b.n;         /* escaped content is never trimmed */
            i++;
            continue;
        }
        if (c == ' ' || c == '\t') {
            yb_put(&b, (uint8_t)c);   /* trimmable while a break follows */
            i++;
            continue;
        }
        yb_put(&b, (uint8_t)(uint8_t)c);
        hard = b.n;
        i++;
    }
    /* Line exhausted without the closing quote. */
    if (!multiline || p->li + 1 >= p->nln) {
        yb_free(&b);
        return yml_fail(p, "unterminated quoted scalar");
    }
    if (esc_break) {
        /* The escaped break eats the break and the next line's leading
           whitespace, contributing nothing; every FURTHER break of the run
           contributes one line feed (the blank-line paragraph form). */
        uint32_t li = p->li + 1;
        size_t k = p->ln[li].raw;
        for (;;) {
            if (yml_docsep_at(p, li)) {
                yb_free(&b);
                return yml_fail(p, "unexpected document separator in a "
                                   "quoted scalar");
            }
            k = p->ln[li].raw;
            while (k < p->ln[li].end && (p->s[k] == ' ' || p->s[k] == '\t')) k++;
            if (k < p->ln[li].end)
                break;
            if (li + 1 >= p->nln) {     /* whitespace-only: one more break */
                yb_free(&b);
                return yml_fail(p, "unterminated quoted scalar");
            }
            yb_put(&b, (uint8_t)'\n');
            li++;
        }
        p->li = li;
        i = k;
        end = p->ln[li].end;
        continue;
    }
    {
        /* An unescaped break: trim the trailing whitespace run, fold the
           break run (one break -> a space; n >= 2 -> n-1 line feeds), and
           resume at the continuation line's first content byte. */
        uint32_t li = p->li + 1, extra = 0;
        size_t k;
        b.n = hard;             /* raw trailing spaces/tabs are not content */
        for (;;) {
            if (yml_docsep_at(p, li)) {
                yb_free(&b);
                return yml_fail(p, "unexpected document separator in a "
                                   "quoted scalar");
            }
            k = p->ln[li].raw;
            while (k < p->ln[li].end && (p->s[k] == ' ' || p->s[k] == '\t')) k++;
            if (k < p->ln[li].end)
                break;          /* content: the run ends here */
            if (li + 1 >= p->nln) {
                yb_free(&b);
                return yml_fail(p, "unterminated quoted scalar");
            }
            extra++;
            li++;
        }
        if (yml_tab_refuse(p, li)) {
            yb_free(&b);
            return JS_EXCEPTION;
        }
        if (extra) {
            while (extra--)
                yb_put(&b, (uint8_t)'\n');
        } else {
            yb_put(&b, (uint8_t)' ');
        }
        p->li = li;
        i = k;
        end = p->ln[li].end;
        continue;
    }
    }
}

/* Every construct this subset does not implement, refused BY NAME. Silently
   ignoring an anchor or a tag returns a document the author did not write. */
static const char *yml_unsupported(const char *s, size_t n)
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

/* ------------------------------------------------------------------ flow */

/* After a complete node on a line, only spaces and a comment may follow:
   `key: "x" junk` would otherwise drop the junk and return a document the
   author did not write. */
static int yml_tail(yml_t *p, size_t i, size_t end)
{
    while (i < end && (p->s[i] == ' ' || p->s[i] == '\t'))
        i++;
    if (i >= end || p->s[i] == '#')
        return 0;
    yml_fail(p, "unexpected content after the value");
    return -1;
}

static JSValue yml_flow(yml_t *p, size_t *pi, size_t end);

/* A plain or quoted scalar inside a flow collection: it ends at `,`, `]` or
   `}`, and the surrounding whitespace is not content. A tab is separation
   here exactly as a space is. */
static JSValue yml_flow_scalar(yml_t *p, size_t *pi, size_t end)
{
    size_t i = *pi, st;
    const char *bad;

    while (i < end && (p->s[i] == ' ' || p->s[i] == '\t')) i++;
    if (i < end && (p->s[i] == '"' || p->s[i] == '\'')) {
        *pi = i;
        return yml_quoted(p, pi, end, 0);
    }
    st = i;
    while (i < end && p->s[i] != ',' && p->s[i] != ']' && p->s[i] != '}')
        i++;
    while (i > st && (p->s[i - 1] == ' ' || p->s[i - 1] == '\t'))
        i--;
    *pi = i;
    bad = yml_unsupported(p->s + st, i - st);
    if (bad) {
        snprintf(p->err, sizeof p->err, "%s at line %u", bad,
                 (unsigned)(p->li + 1));
        return JS_EXCEPTION;
    }
    return yml_scalar(p->ctx, p->s + st, i - st);
}

/* Where the entry's `key` ends: the colon is outside quotes, and a `,]}` or
   the line end with no colon means this is not a mapping entry at all. */
static size_t yml_flow_colon(const char *s, size_t st, size_t end)
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
        } else if (c == ':' || c == ',' || c == ']' || c == '}') {
            return c == ':' ? i : (size_t)-1;
        }
        i++;
    }
    return (size_t)-1;
}

/* One `key: value` entry of a flow mapping, consumed from *pi into `out`.
   The caller owns `out` and frees it when this returns -1 with the error
   recorded in p->err. */
static int yml_flow_entry(yml_t *p, size_t *pi, size_t end, JSValue out)
{
    size_t i = *pi, ks, ke;
    JSValue kv, v;
    JSAtom key;

    ks = i;
    i = yml_flow_colon(p->s, ks, end);
    if (i == (size_t)-1) {
        yml_fail(p, "a flow mapping entry needs a colon");
        return -1;
    }
    ke = i;
    while (ke > ks && (p->s[ke - 1] == ' ' || p->s[ke - 1] == '\t'))
        ke--;
    if (ke > ks && (p->s[ks] == '"' || p->s[ks] == '\'')) {
        size_t qp = ks;
        kv = yml_quoted(p, &qp, ke, 0);
        if (!JS_IsException(kv) && qp != ke) {
            JS_FreeValue(p->ctx, kv);
            yml_fail(p, "unexpected content after the quoted key");
            return -1;
        }
    } else {
        const char *bad = yml_unsupported(p->s + ks, ke - ks);
        if (bad) {
            snprintf(p->err, sizeof p->err, "%s at line %u", bad,
                     (unsigned)(p->li + 1));
            return -1;
        }
        kv = JS_NewStringLen(p->ctx, p->s + ks, ke - ks);
    }
    if (JS_IsException(kv))
        return -1;
    key = JS_ValueToAtom(p->ctx, kv);
    JS_FreeValue(p->ctx, kv);
    if (key == JS_ATOM_NULL)
        return -1;
    i++;
    v = yml_flow(p, &i, end);
    /* DEFINE: a document with a __proto__ key produces an own property
       and cannot retarget a prototype. DUPLICATE keys are refused
       rather than last-writer-wins (matches TOML). */
    if (!JS_IsException(v)) {
        /* OWN-property check: JS_HasProperty would walk the chain and
           false-positive on __proto__ */
        JSPropertyDescriptor d;
        if (JS_GetOwnProperty(p->ctx, &d, out, key) > 0) {
            JS_FreeValue(p->ctx, v);
            JS_FreeAtom(p->ctx, key);
            yml_fail(p, "duplicate mapping key");
            return -1;
        }
    }
    if (JS_IsException(v)
        || JS_DefinePropertyValue(p->ctx, out, key, v, JS_PROP_C_W_E) < 0) {
        /* JS_DefinePropertyValue consumes v on BOTH paths: nothing to free */
        JS_FreeAtom(p->ctx, key);
        return -1;
    }
    JS_FreeAtom(p->ctx, key);
    *pi = i;
    return 0;
}

/* A single-pair entry of a flow SEQUENCE, `[key: value]` (1.2
   ns-flow-pair-entry): the colon splits only when followed by whitespace or
   the entry's end, so a plain `[http://x]` stays one scalar. */
static int yml_flow_is_pair(const char *s, size_t st, size_t end)
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

static JSValue yml_flow(yml_t *p, size_t *pi, size_t end)
{
    size_t i = *pi;
    JSValue out;
    uint32_t k = 0;
    int seq;

    while (i < end && (p->s[i] == ' ' || p->s[i] == '\t')) i++;
    if (i >= end)
        return yml_fail(p, "an empty flow collection needs its brackets");
    if (p->s[i] != '[' && p->s[i] != '{') {
        *pi = i;
        return yml_flow_scalar(p, pi, end);
    }
    seq = p->s[i] == '[';
    i++;
    if (p->depth >= p->max_depth)
        return yml_fail(p, "nesting exceeds the depth limit");
    out = seq ? JS_NewArray(p->ctx) : JS_NewObject(p->ctx);
    if (JS_IsException(out))
        return out;
    p->depth++;
    for (;;) {
        JSValue v;
        while (i < end && (p->s[i] == ' ' || p->s[i] == '\t' || p->s[i] == ',')) i++;
        if (i < end && p->s[i] == (seq ? ']' : '}')) { i++; break; }
        if (i >= end) {
            JS_FreeValue(p->ctx, out);
            p->depth--;
            return yml_fail(p, "unterminated flow collection");
        }
        if (seq) {
            if (p->s[i] == '[' || p->s[i] == '{') {
                v = yml_flow(p, &i, end);
            } else if (yml_flow_is_pair(p->s, i, end)) {
                /* `[key: value]` is a sequence OF MAPPINGS, not a string
                   that happens to contain a colon */
                v = JS_NewObject(p->ctx);
                if (!JS_IsException(v)
                    && yml_flow_entry(p, &i, end, v) < 0) {
                    JS_FreeValue(p->ctx, v);
                    goto fail;
                }
            } else {
                v = yml_flow_scalar(p, &i, end);
            }
            if (JS_IsException(v)
                || JS_DefinePropertyValueUint32(p->ctx, out, k++, v,
                                                JS_PROP_C_W_E) < 0)
                goto fail;              /* the Define call consumed v */
            continue;
        }
        {
            if (yml_flow_entry(p, &i, end, out) < 0)
                goto fail;
        }
    }
    p->depth--;
    *pi = i;
    return out;
fail:
    JS_FreeValue(p->ctx, out);
    p->depth--;
    return JS_EXCEPTION;
}

/* ---------------------------------------------------------- block scalars */

/* `|` keeps newlines, `>` folds them; `-` strips the trailing ones, `+` keeps
   them all. The body is every following line indented past the parent. */
/* `|`/`>` with the chomping indicator and an optional explicit indent. */
static int yml_block_header(yml_t *p, size_t st, size_t end, int *chomp, int *ind)
{
    size_t k;

    *chomp = 0;
    *ind = -1;
    for (k = st + 1; k < end; k++) {
        if (p->s[k] == '-') *chomp = -1;
        else if (p->s[k] == '+') *chomp = 1;
        else if (p->s[k] >= '1' && p->s[k] <= '9') *ind = p->s[k] - '0';
        else if (p->s[k] == ' ') continue;
        else if (p->s[k] == '#') break;
        else return -1;
    }
    return 0;
}

/* `|` keeps newlines, `>` folds them; `-` strips the trailing ones, `+` keeps
   them all. The body is every following line indented past the parent. */
static JSValue yml_block_scalar(yml_t *p, int parent_indent, size_t st, size_t end)
{
    int literal = p->s[st] == '|', chomp, ind;
    yb_t b;
    JSValue v;

    if (yml_block_header(p, st, end, &chomp, &ind) < 0)
        return yml_fail(p, "unknown block scalar indicator");
    p->li++;
    yb_init(&b);
    while (p->li < p->nln) {
        const yml_line_t *L = &p->ln[p->li];
        size_t kc = L->start;
        int content;
        while (kc < L->end && (p->s[kc] == ' ' || p->s[kc] == '\t'))
            kc++;
        content = kc < L->end;          /* a whitespace-only line is blank */
        if (content && L->tab) {
            yb_free(&b);
            if (yml_tab_refuse(p, p->li))
                return JS_EXCEPTION;
        }
        if (content && L->indent <= parent_indent)
            break;
        if (ind < 0 && content)
            ind = L->indent;
        if (content && L->indent < ind)
            break;
        if (!content) {
            if (!literal && b.n && b.p[b.n - 1] == ' ')
                b.n--;                  /* a blank line ends the fold */
            yb_put(&b, (uint8_t)'\n');
        } else {
            size_t from = L->raw + (size_t)(ind < 0 ? 0 : ind);
            if (from > L->end) from = L->end;
            yb_write(&b, p->s + from, L->end - from);
            yb_put(&b, (uint8_t)(literal ? '\n' : ' '));
        }
        p->li++;
    }
    if (!literal && b.n && b.p[b.n - 1] == ' ')
        b.p[b.n - 1] = '\n';
    if (chomp < 0) {
        while (b.n && b.p[b.n - 1] == '\n') b.n--;
    } else if (chomp == 0) {
        while (b.n > 1 && b.p[b.n - 1] == '\n' && b.p[b.n - 2] == '\n')
            b.n--;
    }
    v = JS_NewStringLen(p->ctx, (const char *)b.p, b.n);
    yb_free(&b);
    return v;
}

/* ------------------------------------------------------------ block nodes */

static JSValue yml_node(yml_t *p, int min_indent);

/* The colon that ends a mapping key: followed by a space, a tab, or the line
   end -- or directly after a closing quote, which is how `"a":1` reads -- and
   outside quotes and flow brackets. The quote scan honors \\ escapes, or
   `"a\"b": v` finds no colon and its value silently disappears. */
static size_t yml_key_colon(const char *s, size_t st, size_t end)
{
    size_t i = st;
    int flow = 0, qclosed = 0;
    char q = 0;

    while (i < end) {
        char c = s[i];
        if (q) {
            if (q == '"' && c == '\\' && i + 1 < end) {
                i += 2;
                continue;
            }
            if (c == q) {
                q = 0;
                qclosed = 1;
                i++;
                continue;
            }
            qclosed = 0;
        } else if (c == '"' || c == '\'') {
            q = c;
            qclosed = 0;
        } else if (c == '[' || c == '{') {
            flow++;
        } else if (c == ']' || c == '}') {
            if (flow) flow--;
        } else if (c == '#' && i > st && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
            break;
        } else if (c == ':' && !flow
                   && (i + 1 >= end || s[i + 1] == ' ' || s[i + 1] == '\t'
                       || qclosed)) {
            return i;
        } else {
            qclosed = 0;
        }
        i++;
    }
    return (size_t)-1;
}

/* A value written on the same line as its key or its `- `. */
static JSValue yml_inline_value(yml_t *p, int indent, size_t st, size_t end)
{
    const char *bad;

    while (st < end && (p->s[st] == ' ' || p->s[st] == '\t')) st++;
    if (st >= end)
        return JS_NULL;
    if (p->s[st] == '|' || p->s[st] == '>')
        return yml_block_scalar(p, indent, st, end);
    bad = yml_unsupported(p->s + st, end - st);
    if (bad) {
        snprintf(p->err, sizeof p->err, "%s at line %u", bad, (unsigned)(p->li + 1));
        return JS_EXCEPTION;
    }
    if (p->s[st] == '[' || p->s[st] == '{') {
        size_t i = st;
        JSValue v = yml_flow(p, &i, end);
        if (!JS_IsException(v) && yml_tail(p, i, end) < 0) {
            JS_FreeValue(p->ctx, v);
            return JS_EXCEPTION;
        }
        p->li++;
        return v;
    }
    if (p->s[st] == '"' || p->s[st] == '\'') {
        size_t i = st;
        JSValue v = yml_quoted(p, &i, end, 1);
        /* yml_quoted leaves p->li on the line holding the closing quote
           (the first line, unless folding carried it onward), so the tail
           check belongs to THAT line. */
        if (!JS_IsException(v) && yml_tail(p, i, p->ln[p->li].end) < 0) {
            JS_FreeValue(p->ctx, v);
            return JS_EXCEPTION;
        }
        p->li++;
        return v;
    }
    {
        size_t e = yml_plain_end(p->s, st, end);
        JSValue v = yml_scalar(p->ctx, p->s + st, e - st);
        p->li++;
        return v;
    }
}

/* The key of the entry on the current line, or JS_ATOM_NULL having thrown.
   *pcolon is where its value begins. */
static JSAtom yml_map_key(yml_t *p, size_t st, size_t end, size_t *pcolon)
{
    size_t colon = yml_key_colon(p->s, st, end), ke;
    const char *bad;
    JSValue kv;

    if (colon == (size_t)-1) {
        bad = yml_unsupported(p->s + st, end - st);
        if (bad)
            snprintf(p->err, sizeof p->err, "%s at line %u", bad,
                     (unsigned)(p->li + 1));
        else
            yml_fail(p, "a mapping entry needs `key: value`");
        return JS_ATOM_NULL;
    }
    ke = colon;
    while (ke > st && (p->s[ke - 1] == ' ' || p->s[ke - 1] == '\t')) ke--;
    bad = yml_unsupported(p->s + st, ke - st);
    if (bad) {
        snprintf(p->err, sizeof p->err, "%s at line %u", bad, (unsigned)(p->li + 1));
        return JS_ATOM_NULL;
    }
    if (ke > st && (p->s[st] == '"' || p->s[st] == '\'')) {
        size_t q = st;
        kv = yml_quoted(p, &q, ke, 0);
        /* `"a" junk: v` must not lose the junk: a quoted key ends at its
           colon, never somewhere short of it. */
        if (!JS_IsException(kv) && q != ke) {
            JS_FreeValue(p->ctx, kv);
            yml_fail(p, "unexpected content after the quoted key");
            return JS_ATOM_NULL;
        }
    } else {
        kv = JS_NewStringLen(p->ctx, p->s + st, ke - st);
    }
    if (JS_IsException(kv))
        return JS_ATOM_NULL;
    *pcolon = colon;
    {
        JSAtom a = JS_ValueToAtom(p->ctx, kv);
        JS_FreeValue(p->ctx, kv);
        return a;
    }
}

static JSValue yml_map(yml_t *p, int indent)
{
    JSValue out = JS_NewObject(p->ctx);

    if (JS_IsException(out))
        return out;
    for (;;) {
        size_t st, end, colon = 0, vs;
        JSValue v;
        JSAtom key;

        yml_skip_blank(p);
        if (p->li >= p->nln || p->ln[p->li].indent < indent)
            break;
        if (yml_tab_refuse(p, p->li))
            { JS_FreeValue(p->ctx, out); return JS_EXCEPTION; }
        st = p->ln[p->li].start;
        end = p->ln[p->li].end;
        if (p->s[st] == '-' && (st + 1 >= end || p->s[st + 1] == ' '))
            break;                      /* a sequence at this level, not a key */
        if (yml_marker_at(p, p->li, "---") || yml_marker_at(p, p->li, "..."))
            break;
        if (p->ln[p->li].indent > indent) {
            JS_FreeValue(p->ctx, out);
            return yml_fail(p, "unexpected indentation");
        }
        key = yml_map_key(p, st, end, &colon);
        if (key == JS_ATOM_NULL) {
            JS_FreeValue(p->ctx, out);
            return JS_EXCEPTION;
        }
        vs = colon + 1;
        while (vs < end && (p->s[vs] == ' ' || p->s[vs] == '\t')) vs++;
        if (vs >= end || p->s[vs] == '#') {
            p->li++;
            v = yml_node(p, indent + 1);
        } else {
            v = yml_inline_value(p, indent, vs, end);
        }
        /* DEFINE: a document with a __proto__ key produces an own property.
           DUPLICATE keys are refused rather than last-writer-wins. */
        if (!JS_IsException(v)) {
            JSPropertyDescriptor d;
            if (JS_GetOwnProperty(p->ctx, &d, out, key) > 0) {
                JS_FreeValue(p->ctx, v);
                JS_FreeAtom(p->ctx, key);
                JS_FreeValue(p->ctx, out);
                return yml_fail(p, "duplicate mapping key");
            }
        }
        if (JS_IsException(v)
            || JS_DefinePropertyValue(p->ctx, out, key, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(p->ctx, key);
            JS_FreeValue(p->ctx, out);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(p->ctx, key);
    }
    return out;
}

static JSValue yml_seq(yml_t *p, int indent)
{
    JSValue out = JS_NewArray(p->ctx);
    uint32_t k = 0;

    if (JS_IsException(out))
        return out;
    for (;;) {
        size_t st, end, vs;
        JSValue v;

        yml_skip_blank(p);
        if (p->li >= p->nln || p->ln[p->li].indent < indent)
            break;
        if (yml_tab_refuse(p, p->li))
            { JS_FreeValue(p->ctx, out); return JS_EXCEPTION; }
        st = p->ln[p->li].start;
        end = p->ln[p->li].end;
        if (p->s[st] != '-' || (st + 1 < end && p->s[st + 1] != ' '))
            break;
        if (p->ln[p->li].indent > indent) {
            JS_FreeValue(p->ctx, out);
            return yml_fail(p, "unexpected indentation");
        }
        vs = st + 1;
        while (vs < end && (p->s[vs] == ' ' || p->s[vs] == '\t')) vs++;
        if (vs >= end || p->s[vs] == '#') {
            p->li++;
            v = yml_node(p, indent + 1);
        } else {
            /* The tail of a `- ` line is a node at its own column: `- key: v`,
               `- - 1` and `- scalar` are then one case, not three. */
            p->ln[p->li].start = vs;
            p->ln[p->li].indent = (int)(vs - p->ln[p->li].raw);
            v = yml_node(p, p->ln[p->li].indent);
        }
        if (JS_IsException(v)
            || JS_DefinePropertyValueUint32(p->ctx, out, k++, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(p->ctx, out);
            return JS_EXCEPTION;
        }
    }
    return out;
}

static JSValue yml_node(yml_t *p, int min_indent)
{
    int ind;
    JSValue v;

    yml_skip_blank(p);
    if (p->li >= p->nln)
        return JS_NULL;
    if (yml_tab_refuse(p, p->li))
        return JS_EXCEPTION;
    ind = p->ln[p->li].indent;
    if (ind < min_indent)
        return JS_NULL;                 /* the parent's value is empty */
    if (yml_marker_at(p, p->li, "---") || yml_marker_at(p, p->li, "..."))
        return JS_NULL;
    if (p->depth >= p->max_depth)
        return yml_fail(p, "nesting exceeds the depth limit");
    p->depth++;
    {
        size_t st = p->ln[p->li].start, end = p->ln[p->li].end;
        if (p->s[st] == '-' && (st + 1 >= end || p->s[st + 1] == ' '))
            v = yml_seq(p, ind);
        else if (yml_key_colon(p->s, st, end) != (size_t)-1)
            v = yml_map(p, ind);
        else
            v = yml_inline_value(p, ind - 1, st, end);
    }
    p->depth--;
    return v;
}

/* ------------------------------------------------------------ entry points */

/* Validate arg, take an OWNED copy (so ParseStream's iterator can outlive
   the call that created it without holding engine C strings), split lines,
   check tabs. On success *out_buf is js_malloc'd and MUST be released with
   js_free once parsing is over; on failure it is freed here. */
static int yml_start(JSContext *ctx, yml_t *p, JSValueConst arg, char **out_buf)
{
    const char *src;
    size_t n;

    memset(p, 0, sizeof *p);
    *out_buf = NULL;
    if (!JS_IsString(arg)) {
        JS_ThrowTypeError(ctx, "text must be a string");
        return -1;
    }
    src = JS_ToCStringLen(ctx, &n, arg);
    if (!src)
        return -1;
    if (n > YML_MAX_INPUT) {
        JS_FreeCString(ctx, src);
        JS_ThrowRangeError(ctx, "input exceeds %u bytes", YML_MAX_INPUT);
        return -1;
    }
    *out_buf = js_malloc(ctx, n + 1);
    if (!*out_buf) {
        JS_FreeCString(ctx, src);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(*out_buf, src, n);
    (*out_buf)[n] = '\0';
    JS_FreeCString(ctx, src);
    p->ctx = ctx;
    p->s = *out_buf;
    p->n = n;
    /* A leading UTF-8 BOM is an encoding signature, not content: skip it
       before the line table is built (p->s advances into the copy). */
    if (n >= 3 && (unsigned char)(*out_buf)[0] == 0xEF
              && (unsigned char)(*out_buf)[1] == 0xBB
              && (unsigned char)(*out_buf)[2] == 0xBF) {
        p->s += 3;
        p->n -= 3;
    }
    if (yml_split(p) < 0) {
        if (p->err[0])
            JS_ThrowSyntaxError(ctx, "%s", p->err);
        else
            JS_ThrowOutOfMemory(ctx);
        free(p->ln);
        p->ln = NULL;
        js_free(ctx, *out_buf);
        *out_buf = NULL;
        return -1;
    }
    return 0;
}

/* Whether everything past a marker on its line is blank or a comment. A
   non-blank tail after `...` is a refusal, not something to drop. */
static int yml_marker_tail_ok(yml_t *p, uint32_t li)
{
    const yml_line_t *L = &p->ln[li];
    size_t vs = L->start + 3;
    while (vs < L->end && (p->s[vs] == ' ' || p->s[vs] == '\t'))
        vs++;
    return vs >= L->end || p->s[vs] == '#';
}

/* A `---` line starts a document; `...` ends one. */
static int yml_at_doc_start(yml_t *p)
{
    return yml_marker_at(p, p->li, "---");
}

/* A `...` line ENDS the current document. It must be consumed (or the line
   index never advances and ParseAll appends empty documents forever), but it
   starts no document, so unlike yml_at_doc_start it does not belong inside
   yml_one: the document loops consume it themselves. */
static int yml_at_doc_end(yml_t *p)
{
    return yml_marker_at(p, p->li, "...");
}

static JSValue yml_one(yml_t *p)
{
    yml_skip_blank(p);
    if (yml_at_doc_start(p)) {
        yml_line_t *L = &p->ln[p->li];
        size_t vs = L->start + 3;
        while (vs < L->end && (p->s[vs] == ' ' || p->s[vs] == '\t'))
            vs++;
        if (vs < L->end && p->s[vs] != '#') {
            /* `--- 5`: the document node starts ON the marker line, at the
               column where its content begins. */
            L->start = vs;
            L->indent = (int)(vs - L->raw);
            return yml_node(p, 0);
        }
        p->li++;
    }
    return yml_node(p, 0);
}

/* strict options (rollout convention, after the dyna:csv / dyna:file
 * pilots): an unknown own enumerable string key in an options bag throws
 *   TypeError: unknown option "X" (valid: a, b, c)
 * instead of being silently ignored. Symbol keys and non-enumerable
 * properties are not options and stay invisible (JS_GPN_STRING_MASK |
 * JS_GPN_ENUM_ONLY); null/undefined and primitive bags keep their historic
 * reading at each call site. Getters are NOT invoked here; values are read
 * afterwards, so a throwing getter on a KNOWN key still propagates.
 * Returns 0 when the bag is clean, -1 with the TypeError pending. */
static int yml_opts_check(JSContext *ctx, JSValueConst o,
                          const char *const *keys, int nkeys)
{
    JSPropertyEnum *props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(o))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) { bad = 1; break; }
        for (k = 0; k < nkeys; k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char *)js_malloc(ctx, need);
            if (!valid) { JS_FreeCString(ctx, name); bad = 1; break; }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) { *w++ = ','; *w++ = ' '; }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                              name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static const char *const yml_parse_keys[] = { "maxDepth", "schema" };
/*the Stringify bag -- indent existed; width/sortKeys/flow are new.
 All four are checked. */
static const char *const yml_stringify_keys[] = { "indent", "width", "sortKeys", "flow" };

/*the option bag. Strict: these keys are new surface, so wrong types
 * are refused rather than ignored.
 *   { maxDepth: N }    lower the nesting cap (the fuzzer hardening bound).
 *                      1..YML_MAX_DEPTH; the 128 ceiling is a C-stack guard
 *                      and cannot be raised from JS.
 *   { schema }         "core" (default) is the only implemented schema.
 *                      "full" is REFUSED by name, not silently treated as
 *                      core: a wrong-document parse is worse than an error.
 * There is deliberately NO {maxAliases}: anchors and aliases are refused
 * outright by the parser, which is stricter than any alias cap could be. */
/* `pfx` names the entry point in type errors ("Parse" for Parse/ParseAll,
 * whose bags are identical, "ParseStream" for ). */
static int yml_read_opts(JSContext *ctx, JSValueConst o, int *max_depth,
                         const char *pfx)
{
    JSValue v;

    *max_depth = YML_MAX_DEPTH;
    if (JS_IsUndefined(o) || JS_IsNull(o))
        return 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "%s(text, options): options must be an object",
                          pfx);
        return -1;
    }
    /*made the VALUE types strict but the key set stayed open --
     * {maxDeph: 4} silently read as the default cap. Refuse unknown keys
     * BEFORE any option value is read (shared by Parse and ParseAll). */
    if (yml_opts_check(ctx, o, yml_parse_keys,
                       countof(yml_parse_keys)) < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, o, "maxDepth");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        double d;
        if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) ||
            d != (double)(int64_t)d || d < 1 || d > (double)YML_MAX_DEPTH) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "%s: maxDepth must be an integer 1..%d",
                              pfx, YML_MAX_DEPTH);
            return -1;
        }
        *max_depth = (int)d;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "schema");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (!s) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (strcmp(s, "core") == 0) {
            ;
        } else if (strcmp(s, "full") == 0) {
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx,
                "%s: the YAML 1.2 full schema is not implemented -- "
                "this parser resolves the core schema only", pfx);
            return -1;
        } else {
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "%s: schema must be \"core\" or \"full\"",
                              pfx);
            return -1;
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return 0;
}

/* magic 0 = Parse (one document), 1 = ParseAll (every document) */
static JSValue yml_parse(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv, int magic)
{
    yml_t p;
    char *buf;
    JSValue out;
    int max_depth;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(text): text is required",
                                 magic ? "ParseAll" : "Parse");
    /* options parse BEFORE any work */
    if (yml_read_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
                      &max_depth, "Parse") < 0)
        return JS_EXCEPTION;
    if (yml_start(ctx, &p, argv[0], &buf) < 0)
        return JS_EXCEPTION;
    p.max_depth = max_depth;
    if (magic == 0) {
        out = yml_one(&p);
        if (!JS_IsException(out)) {
            int ended = 0;
            yml_skip_blank(&p);
            /* `...` terminates the document properly; what may follow is
               another stream, not trailing garbage. */
            if (p.li < p.nln && yml_at_doc_end(&p)) {
                if (!yml_marker_tail_ok(&p, p.li)) {
                    JS_FreeValue(ctx, out);
                    yml_fail(&p, "content after the document-end marker");
                    out = JS_EXCEPTION;
                    goto done;
                }
                p.li++;
                ended = 1;
                yml_skip_blank(&p);
            }
            if (p.li < p.nln) {
                JS_FreeValue(ctx, out);
                yml_fail(&p, ended || yml_at_doc_start(&p)
                    ? "the input holds more than one document -- use ParseAll"
                    : "unexpected content after the document");
                out = JS_EXCEPTION;
            }
        }
    } else {
        uint32_t k = 0;
        out = JS_NewArray(ctx);
        while (!JS_IsException(out)) {
            JSValue d;
            yml_skip_blank(&p);
            if (p.li >= p.nln)
                break;
            /* `...` ends the current document: consume the marker here. It
               starts nothing, so it contributes no document of its own --
               and its line may hold nothing but a comment. */
            if (yml_at_doc_end(&p)) {
                if (!yml_marker_tail_ok(&p, p.li)) {
                    JS_FreeValue(ctx, out);
                    yml_fail(&p, "content after the document-end marker");
                    out = JS_EXCEPTION;
                    break;
                }
                p.li++;
                continue;
            }
            d = yml_one(&p);
            if (JS_IsException(d)) {
                JS_FreeValue(ctx, out);
                out = JS_EXCEPTION;
                break;
            }
            if (JS_DefinePropertyValueUint32(ctx, out, k++, d, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, out);
                out = JS_EXCEPTION;
                break;
            }
        }
    }
done:
    if (JS_IsException(out) && p.err[0])
        JS_ThrowSyntaxError(ctx, "%s: %s", magic ? "ParseAll" : "Parse", p.err);
    free(p.ln);
    js_free(ctx, buf);
    return out;
}

/* -------------------------------------------------------------- Stringify */

typedef struct {
    JSContext *ctx;
    yb_t       b;
    int        indent, depth;
    /*{ width, sortKeys, flow }. width 0 = never fold, which is the
       emitter's historic behavior byte-for-byte (it never wrapped). */
    int        width;
    int        sort_keys;
    int        flow;
} yml_w_t;

/* One contract: write this value's lines, and emit a leading newline+indent
   before every entry EXCEPT the first when the caller already positioned the
   cursor (a `- ` prefix, or the start of the document). */
static int yml_emit_value(yml_w_t *w, JSValueConst v, int depth, int same_line);

/* Sorted (sortKeys) or insertion-order own enumerable string keys, with the
   names already materialized. The names are malloc'd copies, so the table
   owns everything and one free releases it. */
typedef struct { JSPropertyEnum e; char *name; } yml_prop_t;

static int yml_prop_cmp(const void *a, const void *b)
{
    return strcmp(((const yml_prop_t *)a)->name, ((const yml_prop_t *)b)->name);
}

static int yml_props(JSContext *ctx, JSValueConst v, int sort,
                     yml_prop_t **out, uint32_t *outn)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, k;
    yml_prop_t *t;

    if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;
    t = (yml_prop_t *)malloc(n ? n * sizeof *t : sizeof *t);
    if (!t) {
        JS_FreePropertyEnum(ctx, tab, n);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (k = 0; k < n; k++) {
        size_t l = 0;
        const char *s = JS_AtomToCStringLen(ctx, &l, tab[k].atom);
        t[k].e = tab[k];
        t[k].name = NULL;
        if (!s)
            goto fail;
        t[k].name = (char *)malloc(l + 1);
        if (!t[k].name) {
            JS_FreeCString(ctx, s);
            goto fail;
        }
        memcpy(t[k].name, s, l + 1);
        JS_FreeCString(ctx, s);
    }
    JS_FreePropertyEnum(ctx, tab, n);
    if (sort)
        qsort(t, n, sizeof *t, yml_prop_cmp);
    *out = t;
    *outn = n;
    return 0;
fail:
    for (k = 0; k < n; k++)
        free(t[k].name);
    free(t);
    if (tab)
        JS_FreePropertyEnum(ctx, tab, n);
    return -1;
}

static void yml_props_free(yml_prop_t *t, uint32_t n)
{
    uint32_t k;
    if (!t)
        return;
    for (k = 0; k < n; k++)
        free(t[k].name);
    free(t);
}

/* A scalar that would re-parse as something else must be quoted, or the writer
   is lossy in exactly the way the Norway problem describes. */
/* `flow` adds the characters that only end a plain scalar inside a flow
   collection: a bare `,`, `[`, `]`, `{` or `}` there is a collection
   separator, so `x,y` written plain re-reads as two entries (PyYAML: "expected
   <block end>, but found ','"). */
static int yml_needs_quote(const char *s, size_t n, int flow)
{
    size_t i;
    int64_t iv;

    if (n == 0)
        return 1;
    /* BYPASS, same rule as yml_scalar: every keyword below starts with one of
       these bytes, so an ordinary word -- the common case -- skips the whole
       literal chain and drops to the character scan. Exact: no skipped branch
       could match. */
    switch (s[0]) {
    case '~': case 'n': case 'N': case 't': case 'T': case 'f': case 'F':
    case 'y': case 'Y': case 'o': case 'O':
    case '.': case '+': case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        break;
    default:
        goto chars;
    }
    if (yml_eq(s, n, "~") || yml_eq(s, n, "null") || yml_eq(s, n, "Null")
        || yml_eq(s, n, "NULL") || yml_eq(s, n, "true") || yml_eq(s, n, "True")
        || yml_eq(s, n, "TRUE") || yml_eq(s, n, "false") || yml_eq(s, n, "False")
        || yml_eq(s, n, "FALSE") || yml_is_int(s, n, &iv) || yml_is_float(s, n))
        return 1;
    /* 1.2 reads these as strings and so does this parser, but a 1.1 reader --
       which most YAML tools still are -- reads them as booleans. Quoting on
       output costs nothing and is the difference between a round trip that
       agrees with itself and one that agrees with the world. */
    if (yml_eq(s, n, "yes") || yml_eq(s, n, "Yes") || yml_eq(s, n, "YES")
        || yml_eq(s, n, "no") || yml_eq(s, n, "No") || yml_eq(s, n, "NO")
        || yml_eq(s, n, "on") || yml_eq(s, n, "On") || yml_eq(s, n, "ON")
        || yml_eq(s, n, "off") || yml_eq(s, n, "Off") || yml_eq(s, n, "OFF"))
        return 1;                       /* NOT bare y/n: no mainstream reader
                                           resolves those, and quoting every
                                           one-letter string is worse */
    /* The .inf/.nan spellings this parser resolves to non-finite NUMBERS, so
       a string with one of them must be quoted or the round trip silently
       changes its type -- the .inf cousin of the Norway problem above. The
       list mirrors yml_scalar exactly, spellings included. */
    if (yml_eq(s, n, ".inf") || yml_eq(s, n, ".Inf") || yml_eq(s, n, ".INF")
        || yml_eq(s, n, "+.inf") || yml_eq(s, n, "+.Inf") || yml_eq(s, n, "+.INF")
        || yml_eq(s, n, "-.inf") || yml_eq(s, n, "-.Inf") || yml_eq(s, n, "-.INF")
        || yml_eq(s, n, ".nan") || yml_eq(s, n, ".NaN") || yml_eq(s, n, ".NAN"))
        return 1;
    /* A bare document marker would re-parse as a marker, not a string. */
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

/* The body of a double-quoted scalar (between the quotes): the escapes the
   reader's yml_quoted undoes, one for one. */
static void yml_put_escaped_body(yb_t *b, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  yb_puts(b, "\\\""); break;
        case '\\': yb_puts(b, "\\\\"); break;
        case '\n': yb_puts(b, "\\n"); break;
        case '\t': yb_puts(b, "\\t"); break;
        case '\r': yb_puts(b, "\\r"); break;
        default:
            if (c < 0x20) {
                char t[8];
                int k = snprintf(t, sizeof t, "\\u%04x", c);
                yb_write(b, t, (size_t)k);
            } else {
                yb_put(b, c);
            }
        }
    }
}

static void yml_put_scalar_str(yml_w_t *w, const char *s, size_t n, int flow)
{
    if (!yml_needs_quote(s, n, flow)) {
        yb_write(&w->b, s, n);
        return;
    }
    yb_put(&w->b, (uint8_t)'"');
    yml_put_escaped_body(&w->b, s, n);
    yb_put(&w->b, (uint8_t)'"');
}

/* Bytes of the whole UTF-8 character starting at p[i] (>= 1, never past n).
   A stray continuation byte is its own unit: it is already invalid UTF-8, and
   inventing a longer unit would split a DIFFERENT character instead. */
static int yml_utf8_unit(const char *p, size_t i, size_t n)
{
    unsigned char c = (unsigned char)p[i];
    int len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;

    if (i + (size_t)len > n)
        len = (int)(n - i);
    return len;
}

/* The folded form (width): ONE double-quoted scalar broken across lines
   with a backslash continuation, so every break contributes NOTHING to the
   value. `cont_col` is the column continuation lines start at. Budget: the
   first line carries at most `width` CHARACTERS of the scalar's quoted form
   (the key prefix is not counted), each continuation at most `width -
   cont_col`; a budget under 16 is clamped up.
   Two portability rules, both measured against PyYAML: a break never falls
   inside a UTF-8 character or an escape (the reader would see U+FFFD, and a
   split `\uXXXX` is an error), and never immediately BEFORE a space -- a
   continuation line's leading spaces are eaten as indentation, so that one
   loses content in every conforming reader. A break AFTER a space is safe
   (trailing white space before an escaped break is content). */
static void yml_put_scalar_folded(yml_w_t *w, const char *s, size_t n,
                                  int cont_col)
{
    yb_t e;
    size_t i = 0, line_start = 0, last_ok = 0, last_word = 0, br;
    int budget, col, line_has_output;

    yb_init(&e);
    yml_put_escaped_body(&e, s, n);
    budget = w->width;
    if (budget < 16)
        budget = 16;
    yb_put(&w->b, (uint8_t)'"');
    col = 0;
    line_has_output = 0;
    while (i < e.n) {
        int ul;
        if (e.p[i] == '\\')
            ul = (i + 1 < e.n && e.p[i + 1] == 'u') ? 6 : 2;
        else if ((unsigned char)e.p[i] >= 0x80)
            ul = yml_utf8_unit((const char *)e.p, i, e.n);
        else
            ul = 1;
        if (line_has_output && col + 1 > budget) {
            br = last_word > line_start ? last_word : last_ok;
            if (i < e.n && e.p[i] == ' ') {
                /* A space run ends this line even when it overshoots the
                   budget: the whole run belongs BEFORE the break, because
                   leading spaces on a continuation line are indentation to
                   every other reader. */
                size_t run = i;
                while (run < e.n && e.p[run] == ' ')
                    run++;
                if (run < e.n) {        /* content follows the run */
                    yb_write(&w->b, e.p + i, run - i);
                    col += (int)(run - i);
                    i = run;
                    br = i;
                } else {
                    br = 0;             /* trailing spaces: nothing to fold */
                }
            }
            if (br > line_start) {
                col -= (int)(i - br);   /* one column per unit */
                w->b.n -= i - br;       /* these are re-emitted below */
                i = br;
                last_ok = last_word = 0;
                yb_put(&w->b, (uint8_t)'\\');
                yb_put(&w->b, (uint8_t)'\n');
                {
                    int q;
                    for (q = 0; q < cont_col; q++)
                        yb_put(&w->b, (uint8_t)' ');
                }
                line_start = i;
                col = 0;
                budget = w->width - cont_col;
                if (budget < 16)
                    budget = 16;
                line_has_output = 0;
                continue;
            }
        }
        yb_write(&w->b, e.p + i, (size_t)ul);
        i += (size_t)ul;
        col++;
        line_has_output = 1;
        /* The boundary just reached is a candidate when the unit starting
           there is not a space; one directly after a space is the preferred
           word-boundary form. */
        if (i < e.n && e.p[i] != ' ') {
            last_ok = i;
            if (e.p[i - 1] == ' ')
                last_word = i;
        }
    }
    yb_put(&w->b, (uint8_t)'"');
    yb_free(&e);
}

/* A scalar value. `cont_col` is the continuation column used when folding
 kicks in (width); negative = never fold (flow style, keys). */
static int yml_emit_scalar(yml_w_t *w, JSValueConst v, int cont_col, int flow)
{
    const char *s;
    size_t n;
    JSValue sv;

    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        yb_puts(&w->b, "null");
        return 0;
    }
    if (JS_IsBool(v)) {
        yb_puts(&w->b, JS_ToBool(w->ctx, v) ? "true" : "false");
        return 0;
    }
    sv = JS_ToString(w->ctx, v);
    if (JS_IsException(sv))
        return -1;
    s = JS_ToCStringLen(w->ctx, &n, sv);
    JS_FreeValue(w->ctx, sv);
    if (!s)
        return -1;
    if (JS_IsNumber(v)) {
        double d;
        JS_ToFloat64(w->ctx, &d, v);
        /* A finite number needs no quoting. NaN/Infinity written bare would
           RE-PARSE AS STRINGS; YAML 1.2 core spells them .nan/.inf/-.inf,
           which the reader resolves back to numbers, so the round trip keeps
           their number-ness. */
        if (isnan(d))
            yb_puts(&w->b, ".nan");
        else if (isinf(d))
            yb_puts(&w->b, d > 0 ? ".inf" : "-.inf");
        else
            yb_write(&w->b, s, n);
    }
    else if (JS_IsString(v) && cont_col >= 0 && w->width > 0
             && n > (size_t)w->width)
        /*a string whose single-line form exceeds width folds into
           the double-quoted continuation form; numbers are never long
           enough to bother and keys are never folded at all. */
        yml_put_scalar_folded(w, s, n, cont_col);
    else
        yml_put_scalar_str(w, s, n, flow);
    JS_FreeCString(w->ctx, s);
    return 0;
}

static void yml_nl_indent(yml_w_t *w, int depth)
{
    int k;
    yb_put(&w->b, (uint8_t)'\n');
    for (k = 0; k < depth * w->indent; k++)
        yb_put(&w->b, (uint8_t)' ');
}

/* A collection worth putting on its own lines; anything else is inline. */
static int yml_is_block(JSContext *ctx, JSValueConst v, int64_t *plen)
{
    JSValue lv;
    int64_t len = 0;

    if (!JS_IsObject(v) || JS_IsFunction(ctx, v))
        return 0;
    if (JS_IsArray(ctx, v) == 1) {
        lv = JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) {
            JS_FreeValue(ctx, lv);
            return -1;
        }
        JS_FreeValue(ctx, lv);
        *plen = len;
        return len > 0 ? 1 : 0;
    }
    {
        JSPropertyEnum *tab = NULL;
        uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return -1;
        JS_FreePropertyEnum(ctx, tab, n);
        *plen = n;
        return n > 0 ? 1 : 0;
    }
}

static int yml_emit_seq(yml_w_t *w, JSValueConst v, int depth, int same_line,
                        int64_t len)
{
    int64_t i;

    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(w->ctx, v, (uint32_t)i);
        int64_t elen = 0;
        int block, rc;
        if (JS_IsException(e))
            return -1;
        if (i || !same_line)
            yml_nl_indent(w, depth);
        yb_puts(&w->b, "- ");
        block = yml_is_block(w->ctx, e, &elen);
        /* A nested collection continues on the dash line, one level deeper:
           `- key: v` then the siblings aligned under `key`. */
        rc = block < 0 ? -1 : yml_emit_value(w, e, depth + 1, 1);
        JS_FreeValue(w->ctx, e);
        if (rc < 0)
            return -1;
    }
    return 0;
}

static int yml_emit_map(yml_w_t *w, JSValueConst v, int depth, int same_line)
{
    yml_prop_t *tab = NULL;
    uint32_t len = 0, k;
    int rc = 0;

    if (yml_props(w->ctx, v, w->sort_keys, &tab, &len) < 0)
        return -1;
    for (k = 0; k < len && rc == 0; k++) {
        JSValue e = JS_GetPropertyStr(w->ctx, v, tab[k].name);
        int64_t elen = 0;
        int block;

        if (JS_IsException(e)) { rc = -1; break; }
        if (k || !same_line)
            yml_nl_indent(w, depth);
        yml_put_scalar_str(w, tab[k].name, strlen(tab[k].name), 0);
        yb_put(&w->b, (uint8_t)':');
        block = yml_is_block(w->ctx, e, &elen);
        if (block < 0) {
            rc = -1;
        } else if (block) {
            rc = yml_emit_value(w, e, depth + 1, 0);
        } else {
            yb_put(&w->b, (uint8_t)' ');
            rc = yml_emit_value(w, e, depth + 1, 1);
        }
        JS_FreeValue(w->ctx, e);
    }
    yml_props_free(tab, len);
    return rc;
}

/* Flow style ({flow: true}): every collection is `[a, b]` / `{k: v}`
   with ", " separators, recursively. Never folded: width applies to block
   scalars only (a flow line may exceed width; that is the documented
   trade for keeping the flow emitter a single pass). */
static int yml_emit_flow(yml_w_t *w, JSValueConst v, int depth)
{
    int64_t len = 0;
    int block;

    if (depth >= YML_MAX_DEPTH) {
        JS_ThrowRangeError(w->ctx, "Stringify: nesting exceeds %d", YML_MAX_DEPTH);
        return -1;
    }
    block = yml_is_block(w->ctx, v, &len);
    if (block < 0)
        return -1;
    if (block <= 0) {
        /* Scalars -- and empty collections, whose `[]`/`{}` is already
           flow-shaped -- go through the one scalar writer. */
        if (JS_IsObject(v) && !JS_IsFunction(w->ctx, v)) {
            yb_puts(&w->b, JS_IsArray(w->ctx, v) == 1 ? "[]" : "{}");
            return 0;
        }
        return yml_emit_scalar(w, v, -1, 1);   /* flow: quoted, never folded */
    }
    if (JS_IsArray(w->ctx, v) == 1) {
        int64_t i;
        yb_put(&w->b, (uint8_t)'[');
        for (i = 0; i < len; i++) {
            JSValue e = JS_GetPropertyUint32(w->ctx, v, (uint32_t)i);
            int rc;
            if (JS_IsException(e))
                return -1;
            if (i)
                yb_puts(&w->b, ", ");
            rc = yml_emit_flow(w, e, depth + 1);
            JS_FreeValue(w->ctx, e);
            if (rc < 0)
                return -1;
        }
        yb_put(&w->b, (uint8_t)']');
        return 0;
    }
    {
        yml_prop_t *tab = NULL;
        uint32_t n = 0, k;
        int rc = 0;
        if (yml_props(w->ctx, v, w->sort_keys, &tab, &n) < 0)
            return -1;
        yb_put(&w->b, (uint8_t)'{');
        for (k = 0; k < n; k++) {
            JSValue e = JS_GetPropertyStr(w->ctx, v, tab[k].name);
            int rc;
            if (JS_IsException(e)) { rc = -1; break; }
            if (k)
                yb_puts(&w->b, ", ");
            yml_put_scalar_str(w, tab[k].name, strlen(tab[k].name), 1);
            yb_puts(&w->b, ": ");
            /* EVERY non-function object recurses -- yml_emit_flow renders
               an empty collection as `[]`/`{}` itself, so routing one
               through the scalar writer would emit "[object Object]".
               Null/scalars/functions take the scalar writer. */
            rc = (JS_IsObject(e) && !JS_IsFunction(w->ctx, e))
                ? yml_emit_flow(w, e, depth + 1)
                : yml_emit_scalar(w, e, -1, 1);
            JS_FreeValue(w->ctx, e);
            if (rc < 0)
                break;
        }
        yb_put(&w->b, (uint8_t)'}');
        yml_props_free(tab, n);
        return rc;
    }
}

static int yml_emit_value(yml_w_t *w, JSValueConst v, int depth, int same_line)
{
    int64_t len = 0;
    int block;

    if (depth >= YML_MAX_DEPTH) {
        JS_ThrowRangeError(w->ctx, "Stringify: nesting exceeds %d", YML_MAX_DEPTH);
        return -1;
    }
    block = yml_is_block(w->ctx, v, &len);
    if (block < 0)
        return -1;
    if (w->flow)
        return yml_emit_flow(w, v, depth);
    if (!block) {
        /* An empty collection has no block form: `[]` and `{}` are the only
           way to write one, and they must round-trip. */
        if (JS_IsObject(v) && !JS_IsFunction(w->ctx, v)) {
            yb_puts(&w->b, JS_IsArray(w->ctx, v) == 1 ? "[]" : "{}");
            return 0;
        }
        /* Continuation column one indent past the owning line, always more
           indented than a `key: ` or `- ` line at depth - 1. */
        return yml_emit_scalar(w, v, depth * w->indent + 2, 0);
    }
    if (JS_IsArray(w->ctx, v) == 1)
        return yml_emit_seq(w, v, depth, same_line, len);
    return yml_emit_map(w, v, depth, same_line);
}

static JSValue yml_stringify(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    yml_w_t w;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Stringify(value, options): a value is required");
    memset(&w, 0, sizeof w);
    w.ctx = ctx;
    w.indent = 2;
    yb_init(&w.b);
    if (argc > 1 && JS_IsObject(argv[1])) {
        /*the bag is strict; a non-object stays "no options". */
        if (yml_opts_check(ctx, argv[1], yml_stringify_keys,
                           countof(yml_stringify_keys)) < 0) {
            yb_free(&w.b);
            return JS_EXCEPTION;
        }
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "indent");
        int32_t ind = 2;
        if (JS_IsException(v)) { yb_free(&w.b); return JS_EXCEPTION; }
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &ind, v) < 0) {
            JS_FreeValue(ctx, v);
            yb_free(&w.b);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (ind < 1 || ind > 10) {
            yb_free(&w.b);
            return JS_ThrowRangeError(ctx, "Stringify: indent is 1 to 10");
        }
        w.indent = ind;
        /* {width}: 0 = never fold -- the emitter's historic behavior,
           so the default output is byte-identical to before the option
           existed. Above 0, a STRING value whose single-line form exceeds
           width is emitted as a double-quoted scalar folded with backslash
           continuations (the re-parse is byte-exact); keys and flow style
           are never folded. Strict like maxDepth: an integer 0..65536, a
           wrong type is a TypeError. */
        v = JS_GetPropertyStr(ctx, argv[1], "width");
        if (JS_IsException(v)) { yb_free(&w.b); return JS_EXCEPTION; }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) ||
                d != (double)(int64_t)d || d < 0 || d > 65536) {
                JS_FreeValue(ctx, v);
                yb_free(&w.b);
                return JS_ThrowTypeError(ctx,
                    "Stringify: width must be an integer 0..65536");
            }
            w.width = (int)d;
        }
        JS_FreeValue(ctx, v);
        /* {sortKeys}: map keys in UTF-8 byte order (strcmp) instead of
           insertion order. Strict: boolean or TypeError. */
        v = JS_GetPropertyStr(ctx, argv[1], "sortKeys");
        if (JS_IsException(v)) { yb_free(&w.b); return JS_EXCEPTION; }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (!JS_IsBool(v)) {
                JS_FreeValue(ctx, v);
                yb_free(&w.b);
                return JS_ThrowTypeError(ctx,
                    "Stringify: sortKeys must be a boolean");
            }
            w.sort_keys = JS_ToBool(ctx, v);
        }
        JS_FreeValue(ctx, v);
        /* {flow}: every collection in `[a, b]` / `{k: v}` form,
           recursively, ", "-separated. Strict: boolean or TypeError. */
        v = JS_GetPropertyStr(ctx, argv[1], "flow");
        if (JS_IsException(v)) { yb_free(&w.b); return JS_EXCEPTION; }
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (!JS_IsBool(v)) {
                JS_FreeValue(ctx, v);
                yb_free(&w.b);
                return JS_ThrowTypeError(ctx,
                    "Stringify: flow must be a boolean");
            }
            w.flow = JS_ToBool(ctx, v);
        }
        JS_FreeValue(ctx, v);
    }
    if (yml_emit_value(&w, argv[0], 0, 1) < 0) {
        yb_free(&w.b);
        return JS_EXCEPTION;
    }
    yb_put(&w.b, (uint8_t)'\n');
    out = JS_NewStringLen(ctx, (const char *)w.b.p, w.b.n);
    yb_free(&w.b);
    return out;
}

/* ------------------------------------------------------- ParseStream */

/* Line-at-a-time multi-document parsing: `ParseStream(text[, options])`
   returns a SYNC iterator that yields the documents of the `---`-separated
   stream one at a time, with the same strictness as ParseAll -- same
   markers, same refusals, same values (an empty document between markers
   yields `null` exactly the way ParseAll stores one). The input is the
   whole text (the line table is built once, up front, exactly as for
   Parse/ParseAll); what the iterator buys is NOT incremental feeding but
   LAZY DOCUMENTS: only one parsed document is alive at a time, so a
   100-document stream no longer materializes all 100 at once, and a
   malformed document N is reported AS document N instead of poisoning the
   whole array.
   Per-document errors are SyntaxErrors carrying the 1-based document
   index; the line number inside names the position in the whole text.
   After an error the iterator is dead: every further next() re-throws the
   same error (the input position is gone), and return() closes it. */

/* The {value, done} IteratorResult protocol object. */
static JSValue ystream_result(JSContext *ctx, JSValue value, int done)
{
    JSValue result = JS_NewObject(ctx);

    if (JS_IsException(result)) {
        JS_FreeValue(ctx, value);
        return result;
    }
    JS_DefinePropertyValueStr(ctx, result, "value", value, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, result, "done",
                              done ? JS_TRUE : JS_FALSE, JS_PROP_C_W_E);
    return result;
}

typedef struct {
    yml_t     p;            /* line table + cursor (p.s is an owned copy) */
    char     *buf;          /* the owned input copy (p.s points into it) */
    JSContext *ctx;
    uint32_t  ndocs;        /* documents yielded so far */
    int       done, dead;
    char      err[192];     /* the sticky dead-iterator message */
} ystream_t;

static JSClassID ystream_class_id;

static void ystream_free(JSRuntime *rt, ystream_t *st)
{
    if (!st)
        return;
    free(st->p.ln);
    st->p.ln = NULL;
    if (st->buf)
        js_free_rt(rt, st->buf);
    st->buf = NULL;
    free(st);
}

static void ystream_finalizer(JSRuntime *rt, JSValue val)
{
    ystream_free(rt, (ystream_t *)JS_GetOpaque(val, ystream_class_id));
}

static const JSClassDef ystream_class = {
    "YamlDocStream", .finalizer = ystream_finalizer,
};

/* The one shared document loop: skip blanks, consume `...` terminators
   (refusing a non-comment tail, exactly ParseAll), parse one document.
   Returns the value, or JS_EXCEPTION with *out_dead set. */
static JSValue ystream_next_doc(ystream_t *st, int *out_dead)
{
    JSValue d;

    *out_dead = 0;
    for (;;) {
        yml_skip_blank(&st->p);
        if (st->p.li >= st->p.nln)
            return JS_UNDEFINED;            /* caller answers done */
        if (yml_at_doc_end(&st->p)) {
            if (!yml_marker_tail_ok(&st->p, st->p.li)) {
                yml_fail(&st->p, "content after the document-end marker");
                *out_dead = 1;
                return JS_EXCEPTION;
            }
            st->p.li++;
            continue;
        }
        break;
    }
    d = yml_one(&st->p);
    if (JS_IsException(d))
        *out_dead = 1;
    return d;
}

/* next() -> {value, done:false} | {done:true}; errors are thrown. */
static JSValue ystream_next(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    ystream_t *st = (ystream_t *)dyn_plain_get(ctx, this_val,
                                               ystream_class_id);
    JSValue d;
    int dead;

    (void)argc; (void)argv;
    if (!st)
        return JS_EXCEPTION;
    if (st->dead)
        return JS_ThrowSyntaxError(ctx, "ParseStream: %s", st->err);
    if (st->done)
        return ystream_result(ctx, JS_UNDEFINED, 1);
    d = ystream_next_doc(st, &dead);
    if (dead) {
        st->dead = 1;
        snprintf(st->err, sizeof st->err, "document %u: %s",
                 (unsigned)(st->ndocs + 1),
                 st->p.err[0] ? st->p.err : "malformed document");
        return JS_ThrowSyntaxError(ctx, "ParseStream: %s", st->err);
    }
    if (JS_IsUndefined(d)) {
        /* the input is exhausted: release the line table and the copy now
           rather than at GC (an abandoned large stream frees eagerly) */
        st->done = 1;
        free(st->p.ln);
        st->p.ln = NULL;
        js_free_rt(JS_GetRuntime(ctx), st->buf);
        st->buf = NULL;
        return ystream_result(ctx, JS_UNDEFINED, 1);
    }
    st->ndocs++;
    return ystream_result(ctx, d, 0);
}

/* return(): for-of break/throw closes the stream. */
static JSValue ystream_return(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    ystream_t *st = (ystream_t *)dyn_plain_get(ctx, this_val,
                                               ystream_class_id);

    (void)argc; (void)argv;
    if (!st)
        return JS_EXCEPTION;
    if (!st->done && !st->dead) {
        st->done = 1;
        free(st->p.ln);
        st->p.ln = NULL;
        js_free_rt(JS_GetRuntime(ctx), st->buf);
        st->buf = NULL;
    }
    return ystream_result(ctx, JS_UNDEFINED, 1);
}

static JSValue ystream_iterator(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry ystream_proto[] = {
    JS_CFUNC_DEF("next", 0, ystream_next),
    JS_CFUNC_DEF("return", 0, ystream_return),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, ystream_iterator),
};

static JSValue yml_parse_stream(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    ystream_t *st;
    JSRuntime *rt;
    JSValue proto, obj;
    int max_depth;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ParseStream(text): text is required");
    if (yml_read_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
                      &max_depth, "ParseStream") < 0)
        return JS_EXCEPTION;
    st = (ystream_t *)calloc(1, sizeof *st);
    if (!st)
        return JS_ThrowOutOfMemory(ctx);
    st->ctx = ctx;
    if (yml_start(ctx, &st->p, argv[0], &st->buf) < 0) {
        ystream_free(JS_GetRuntime(ctx), st);
        return JS_EXCEPTION;
    }
    st->p.max_depth = max_depth;
    rt = JS_GetRuntime(ctx);
    proto = JS_GetClassProto(ctx, ystream_class_id);
    obj = JS_NewObjectProtoClass(ctx, proto, ystream_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        ystream_free(rt, st);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, st);
    return obj;
}

/* ------------------------------------------------------------ registration */

static const JSCFunctionListEntry dyn_yaml_funcs[] = {
    JS_CFUNC_MAGIC_DEF("Parse", 1, yml_parse, 0),
    JS_CFUNC_MAGIC_DEF("ParseAll", 1, yml_parse, 1),
    JS_CFUNC_DEF("ParseStream", 1, yml_parse_stream),
    JS_CFUNC_DEF("Stringify", 1, yml_stringify),
};

static int dyn_yaml_init_module(JSContext *ctx, JSModuleDef *m)
{
    /* The document iterator is not itself exported: ParseStream() hands it
       out. No constructor, no close surface -- the finalizer frees. */
    JS_NewClassID(&ystream_class_id);
    if (JS_NewClass(JS_GetRuntime(ctx), ystream_class_id, &ystream_class) < 0)
        return -1;
    {
        JSValue proto = JS_NewObject(ctx);
        if (JS_IsException(proto))
            return -1;
        JS_SetPropertyFunctionList(ctx, proto, ystream_proto,
                                   countof(ystream_proto));
        JS_SetClassProto(ctx, ystream_class_id, proto);
    }
    return JS_SetModuleExportList(ctx, m, dyn_yaml_funcs, countof(dyn_yaml_funcs));
}

int js_nat_init_yaml(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:yaml", dyn_yaml_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_yaml_funcs, countof(dyn_yaml_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_YAML */
