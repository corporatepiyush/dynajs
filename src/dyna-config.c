/* dyna:config -- INI, .env and front-matter splitting. Four grammars would make
   a bare `parse` ambiguous, so each is a namespace object with static methods.
   Full API: see the dyna:* module in dyna-libc.h. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CONFIG)

#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cutils.h"   /* unicode_to_utf8 for \u escapes */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Section nesting via dots. A config file is not a tree of arbitrary depth and
   an unbounded descent is a stack the caller controls. */
#define DYN_CFG_MAX_DEPTH 16

/* ------------------------------------------------------------- line cursor */

typedef struct {
    const char *s;
    size_t n, i;
} dyn_cur_t;

/* Next line into [*b,*e), excluding the terminator. Returns 0 at end of input.
   Handles LF, CRLF and a final line with no terminator. */
static int dyn_next_line(dyn_cur_t *c, size_t *b, size_t *e)
{
    size_t j;
    if (c->i >= c->n)
        return 0;
    *b = c->i;
    for (j = c->i; j < c->n && c->s[j] != '\n'; j++)
        ;
    *e = j;
    if (*e > *b && c->s[*e - 1] == '\r')
        (*e)--;
    c->i = (j < c->n) ? j + 1 : j;
    return 1;
}

static int dyn_cfg_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'
        || c == '\v';
}

static void dyn_trim(const char *s, size_t *b, size_t *e)
{
    while (*b < *e && dyn_cfg_space(s[*b]))
        (*b)++;
    while (*e > *b && dyn_cfg_space(s[*e - 1]))
        (*e)--;
}

/* A leading UTF-8 BOM is encoding metadata, not content: every parser here
   skips one instead of glueing it onto the first key. */
static void dyn_cfg_bom(const char **ps, size_t *pn)
{
    if (*pn >= 3 && (unsigned char)(*ps)[0] == 0xEF
                 && (unsigned char)(*ps)[1] == 0xBB
                 && (unsigned char)(*ps)[2] == 0xBF) {
        *ps += 3;
        *pn -= 3;
    }
}

/* A key is written with DefineProperty, never Set: a file containing
   `__proto__ = x` must produce an own property, not retarget the prototype. */
static int dyn_def(JSContext *ctx, JSValueConst obj, const char *k, size_t klen,
                   JSValue v)
{
    JSAtom a = JS_NewAtomLen(ctx, k, klen);
    int r;
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    r = JS_DefinePropertyValue(ctx, obj, a, v, JS_PROP_C_W_E);
    JS_FreeAtom(ctx, a);
    return r;
}

/* OWN properties only. JS_GetProperty walks the prototype chain, so a section
   named `__proto__` would return Object.prototype and every key under it would
   be defined on the prototype -- pollution from a config file. */
static JSValue dyn_get_own(JSContext *ctx, JSValueConst obj, const char *k,
                           size_t klen)
{
    JSAtom a = JS_NewAtomLen(ctx, k, klen);
    JSPropertyDescriptor desc;
    int r;
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = JS_GetOwnProperty(ctx, &desc, obj, a);
    JS_FreeAtom(ctx, a);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return JS_UNDEFINED;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    return desc.value;
}

/* ------------------------------------------------------------------- INI */

/* Unquote in place if the value is wrapped in matching quotes, honouring the
   backslash escapes npm-ini writes. An unquoted value is taken literally. */
static JSValue dyn_ini_value(JSContext *ctx, const char *s, size_t b, size_t e)
{
    char *buf;
    size_t o = 0, i;
    char q;
    if (e - b >= 2 && (s[b] == '"' || s[b] == '\'') && s[e - 1] == s[b]) {
        q = s[b];
        b++; e--;
    } else {
        return JS_NewStringLen(ctx, s + b, e - b);
    }
    buf = (char *)js_malloc(ctx, (e - b) + 1);
    if (!buf)
        return JS_EXCEPTION;
    for (i = b; i < e; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < e) {
            char d = s[++i];
            switch (d) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = '\0'; break;
            default:  c = d;    break;      /* \\ \" \' and anything else */
            }
        }
        buf[o++] = c;
    }
    (void)q;
    {
        JSValue v = JS_NewStringLen(ctx, buf, o);
        js_free(ctx, buf);
        return v;
    }
}

/* Walk/create the nested object for a dotted section name. */
static JSValue dyn_ini_section(JSContext *ctx, JSValueConst root,
                               const char *s, size_t b, size_t e)
{
    JSValue cur = JS_DupValue(ctx, root);
    int depth = 0;
    while (b < e) {
        size_t seg = b, kb, ke;
        JSValue next;
        while (seg < e && s[seg] != '.')
            seg++;
        kb = b; ke = seg;
        dyn_trim(s, &kb, &ke);
        if (++depth > DYN_CFG_MAX_DEPTH) {
            JS_FreeValue(ctx, cur);
            JS_ThrowRangeError(ctx, "INI.parse: section nesting deeper than %d",
                               DYN_CFG_MAX_DEPTH);
            return JS_EXCEPTION;
        }
        next = dyn_get_own(ctx, cur, s + kb, ke - kb);
        if (JS_IsException(next)) { JS_FreeValue(ctx, cur); return JS_EXCEPTION; }
        if (!JS_IsObject(next)) {
            JS_FreeValue(ctx, next);
            next = JS_NewObject(ctx);
            if (JS_IsException(next)) { JS_FreeValue(ctx, cur); return JS_EXCEPTION; }
            if (dyn_def(ctx, cur, s + kb, ke - kb, JS_DupValue(ctx, next)) < 0) {
                JS_FreeValue(ctx, next);
                JS_FreeValue(ctx, cur);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, cur);
        cur = next;
        b = (seg < e) ? seg + 1 : e;
    }
    return cur;
}

/* `key[] = v` appends. The array is created on first use and reused after. */
static int dyn_ini_append(JSContext *ctx, JSValueConst obj, const char *s,
                          size_t kb, size_t ke, JSValue val)
{
    JSValue arr = dyn_get_own(ctx, obj, s + kb, ke - kb);
    int64_t len = 0;
    if (JS_IsException(arr)) { JS_FreeValue(ctx, val); return -1; }
    if (!JS_IsArray(ctx, arr)) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        if (JS_IsException(arr)) { JS_FreeValue(ctx, val); return -1; }
        if (dyn_def(ctx, obj, s + kb, ke - kb, JS_DupValue(ctx, arr)) < 0) {
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, val);
            return -1;
        }
    } else {
        JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
        if (JS_ToInt64(ctx, &len, lv)) {
            JS_FreeValue(ctx, lv); JS_FreeValue(ctx, arr); JS_FreeValue(ctx, val);
            return -1;
        }
        JS_FreeValue(ctx, lv);
    }
    if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)len, val,
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, arr);
        return -1;
    }
    JS_FreeValue(ctx, arr);
    return 0;
}

/* INI.parse(text) -> object. `[section]` (dotted = nested), `key = value`,
   `;`/`#` comments, `key[] = v` appends, a bare key is `true`. */
static JSValue dyn_ini_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    const char *src;
    size_t n, lb, le;
    dyn_cur_t cur;
    JSValue root, sect = JS_UNDEFINED;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "INI.parse(text): argument must be a string");
    src = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    dyn_cfg_bom(&src, &n);
    root = JS_NewObject(ctx);
    if (JS_IsException(root)) { JS_FreeCString(ctx, src); return JS_EXCEPTION; }
    sect = JS_DupValue(ctx, root);

    cur.s = src; cur.n = n; cur.i = 0;
    while (dyn_next_line(&cur, &lb, &le)) {
        size_t b = lb, e = le, eq, kb, ke, vb, ve;
        int is_append = 0;
        dyn_trim(src, &b, &e);
        if (b == e || src[b] == ';' || src[b] == '#')
            continue;                                   /* blank or comment */
        if (src[b] == '[') {
            size_t sb = b + 1, se = e;
            while (se > sb && src[se - 1] != ']')
                se--;
            if (se <= sb)
                continue;                               /* unterminated header */
            se--;
            JS_FreeValue(ctx, sect);
            sect = dyn_ini_section(ctx, root, src, sb, se);
            if (JS_IsException(sect)) goto fail;
            continue;
        }
        for (eq = b; eq < e && src[eq] != '='; eq++)
            ;
        kb = b; ke = (eq < e) ? eq : e;
        dyn_trim(src, &kb, &ke);
        if (ke - kb >= 2 && src[ke - 2] == '[' && src[ke - 1] == ']') {
            is_append = 1;
            ke -= 2;
            dyn_trim(src, &kb, &ke);
        }
        if (ke == kb)
            continue;
        if (eq >= e) {                                  /* bare key -> true */
            if (dyn_def(ctx, sect, src + kb, ke - kb, JS_TRUE) < 0) goto fail;
            continue;
        }
        vb = eq + 1; ve = e;
        dyn_trim(src, &vb, &ve);
        {
            JSValue v = dyn_ini_value(ctx, src, vb, ve);
            if (JS_IsException(v)) goto fail;
            if (is_append) {
                if (dyn_ini_append(ctx, sect, src, kb, ke, v) < 0) goto fail;
            } else if (dyn_def(ctx, sect, src + kb, ke - kb, v) < 0) {
                goto fail;
            }
        }
    }
    JS_FreeValue(ctx, sect);
    JS_FreeCString(ctx, src);
    return root;
 fail:
    JS_FreeValue(ctx, sect);
    JS_FreeValue(ctx, root);
    JS_FreeCString(ctx, src);
    return JS_EXCEPTION;
}

/* ------------------------------------------------------------------- .env */

/* Expand the escapes dotenv honours inside double quotes. Single-quoted and
   bare values are literal, which is why this is only called for `"`. */
static JSValue dyn_env_unescape(JSContext *ctx, const char *s, size_t b, size_t e)
{
    char *buf = (char *)js_malloc(ctx, (e - b) + 1);
    size_t o = 0, k;
    JSValue v;
    if (!buf)
        return JS_EXCEPTION;
    for (k = b; k < e; k++) {
        char c = s[k];
        if (c == '\\' && k + 1 < e) {
            char d = s[++k];
            switch (d) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:  c = d;    break;
            }
        }
        buf[o++] = c;
    }
    v = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    return v;
}

/* Read the value at *i into [*vb,*ve) and report the quote it used (0 = bare).
   Always leaves *i at the start of the next line. */
static void dyn_env_read_value(const char *s, size_t n, size_t *i, size_t *vb,
                               size_t *ve, char *q)
{
    *q = 0;
    while (*i < n && (s[*i] == ' ' || s[*i] == '\t'))
        (*i)++;
    if (*i < n && (s[*i] == '"' || s[*i] == '\'')) {
        *q = s[*i];
        (*i)++;
        *vb = *i;
        while (*i < n && s[*i] != *q) {
            if (*q == '"' && s[*i] == '\\' && *i + 1 < n)
                (*i)++;                       /* an escaped quote is not the end */
            (*i)++;
        }
        *ve = *i;
        if (*i < n)
            (*i)++;                           /* the closing quote */
    } else {
        *vb = *i;
        while (*i < n && s[*i] != '\n' && s[*i] != '#')
            (*i)++;
        *ve = *i;
        dyn_trim(s, vb, ve);
    }
    while (*i < n && s[*i] != '\n')           /* anything after the value */
        (*i)++;
}

/* Advance past blank lines and comments; 1 if a record may start at *i. */
static int dyn_env_next_record(const char *s, size_t n, size_t *i)
{
    for (;;) {
        while (*i < n && (s[*i] == '\n' || s[*i] == '\r' || s[*i] == ' '
                          || s[*i] == '\t'))
            (*i)++;
        if (*i >= n)
            return 0;
        if (s[*i] != '#')
            return 1;
        while (*i < n && s[*i] != '\n')
            (*i)++;
    }
}

/* Env.parse(text) -> { KEY: "value" }. dotenv's grammar: `KEY=value`, optional
   `export ` prefix, `#` comments, and single or double quotes (escapes expand
   only inside double quotes, which is dotenv's actual behaviour). */
static JSValue dyn_env_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    const char *src;
    size_t n, i = 0;
    JSValue obj;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Env.parse(text): argument must be a string");
    src = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    dyn_cfg_bom(&src, &n);
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) { JS_FreeCString(ctx, src); return JS_EXCEPTION; }

    while (dyn_env_next_record(src, n, &i)) {
        size_t kb, ke, vb, ve;
        char q;
        JSValue v;
        if (n - i >= 7 && memcmp(src + i, "export ", 7) == 0)
            i += 7;
        kb = i;
        while (i < n && src[i] != '=' && src[i] != '\n')
            i++;
        if (i >= n || src[i] != '=') {          /* no '=': not a record */
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        ke = i++;
        dyn_trim(src, &kb, &ke);
        dyn_env_read_value(src, n, &i, &vb, &ve, &q);
        if (ke == kb)
            continue;
        v = (q == '"') ? dyn_env_unescape(ctx, src, vb, ve)
                       : JS_NewStringLen(ctx, src + vb, ve - vb);
        if (JS_IsException(v) || dyn_def(ctx, obj, src + kb, ke - kb, v) < 0) {
            JS_FreeValue(ctx, obj);
            JS_FreeCString(ctx, src);
            return JS_EXCEPTION;
        }
    }
    JS_FreeCString(ctx, src);
    return obj;
}

/* ------------------------------------------------------------ front matter */

/* FrontMatter.split(text) -> { data, body, lang }. `---` is YAML, `+++` is
   TOML, `;;;` is JSON; the fence must be the first line and close on its own
   line. `data` stays TEXT here -- splitting is not parsing. */
static JSValue dyn_fm_split(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    const char *src;
    size_t n, lb, le, dstart, dend = 0, bstart;
    dyn_cur_t cur;
    JSValue o;
    const char *lang = NULL;
    char fence;
    int closed = 0;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "FrontMatter.split(text): argument must be a string");
    src = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o)) { JS_FreeCString(ctx, src); return JS_EXCEPTION; }

    cur.s = src; cur.n = n; cur.i = 0;
    if (!dyn_next_line(&cur, &lb, &le))
        goto no_fm;
    {
        size_t b = lb, e = le;
        dyn_trim(src, &b, &e);
        if (e - b != 3)
            goto no_fm;
        fence = src[b];
        if (src[b + 1] != fence || src[b + 2] != fence)
            goto no_fm;
        if (fence == '-')      lang = "yaml";
        else if (fence == '+') lang = "toml";
        else if (fence == ';') lang = "json";
        else                   goto no_fm;
    }
    dstart = cur.i;
    while (dyn_next_line(&cur, &lb, &le)) {
        size_t b = lb, e = le;
        dyn_trim(src, &b, &e);
        if (e - b == 3 && src[b] == fence && src[b + 1] == fence
            && src[b + 2] == fence) {
            dend = lb;
            closed = 1;
            break;
        }
    }
    if (!closed)
        goto no_fm;                       /* an unclosed fence is not front matter */
    bstart = cur.i;
    if (dend > dstart && src[dend - 1] == '\n') dend--;
    if (dend > dstart && src[dend - 1] == '\r') dend--;
    if (JS_DefinePropertyValueStr(ctx, o, "data",
            JS_NewStringLen(ctx, src + dstart, dend - dstart), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "body",
            JS_NewStringLen(ctx, src + bstart, n - bstart), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "lang",
            JS_NewString(ctx, lang), JS_PROP_C_W_E) < 0)
        goto fail;
    JS_FreeCString(ctx, src);
    return o;
 no_fm:
    if (JS_DefinePropertyValueStr(ctx, o, "data", JS_NULL, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "body",
               JS_NewStringLen(ctx, src, n), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "lang", JS_NULL, JS_PROP_C_W_E) < 0)
        goto fail;
    JS_FreeCString(ctx, src);
    return o;
 fail:
    JS_FreeValue(ctx, o);
    JS_FreeCString(ctx, src);
    return JS_EXCEPTION;
}

/* ==================================================================== *
 *  TOML 1.0 (plan 3.13): parse + stringify                              *
 *                                                                       *
 *  Strict grammar, hand-rolled: no strtod/atoi sees bytes before the    *
 *  grammar validator passes them, and the date-time grammar is its own  *
 *  (RFC 3339 offset form), NEVER the lenient DateParser. Refusals name   *
 *  line and column. Round-trip editing cut: stringify is canonical,     *
 *  not lossless. Depth cap 64. Date-times decode to JS strings.         *
 * ==================================================================== */

#define DYN_TOML_MAX_DEPTH 64

typedef struct {
    JSContext *ctx;
    const char *src;
    size_t n, i;
    int line, col;
    /* Tables a later `[header]` may not redefine, as raw object pointers:
       header-created tables, inline tables (closed the moment they are
       written) and tables an implicit dotted-key segment created. The set
       holds NO reference -- every marked table stays reachable from the
       document root until the parse is over -- and it must not outlive one,
       so dyn_tml_parse frees it on every exit. */
    struct { void **v; size_t n, cap; } marks;
} dyn_tml_t;

static void dyn_tml_marks_free(dyn_tml_t *t)
{
    free(t->marks.v);
    t->marks.v = NULL;
    t->marks.n = t->marks.cap = 0;
}

static int dyn_tml_mark(dyn_tml_t *t, JSValue v)
{
    if (t->marks.n == t->marks.cap) {
        size_t nc = t->marks.cap ? t->marks.cap * 2 : 16;
        void **nv = (void **)realloc(t->marks.v, nc * sizeof *nv);
        if (!nv)
            return -1;
        t->marks.v = nv;
        t->marks.cap = nc;
    }
    t->marks.v[t->marks.n++] = JS_VALUE_GET_PTR(v);
    return 0;
}

static int dyn_tml_marked(dyn_tml_t *t, JSValueConst v)
{
    size_t k;
    void *p = JS_VALUE_GET_PTR(v);
    for (k = 0; k < t->marks.n; k++)
        if (t->marks.v[k] == p)
            return 1;
    return 0;
}

typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} dyn_tml_sb_t;

static void dyn_tml_sb_init(dyn_tml_sb_t *b) { b->p = NULL; b->n = 0; b->cap = 0; b->oom = 0; }
static void dyn_tml_sb_free(dyn_tml_sb_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void dyn_tml_sb_put(dyn_tml_sb_t *b, const char *s, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        char *np;
        while (nc < b->n + n) nc *= 2;
        np = (char *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
static void dyn_tml_sb_putc(dyn_tml_sb_t *b, char c) { dyn_tml_sb_put(b, &c, 1); }

static int dyn_tml_cur(dyn_tml_t *t) { return t->i < t->n ? (unsigned char)t->src[t->i] : -1; }
static int dyn_tml_peek(dyn_tml_t *t, int k) { return t->i + (size_t)k < t->n ? (unsigned char)t->src[t->i + k] : -1; }

static void dyn_tml_adv(dyn_tml_t *t)
{
    if (t->src[t->i] == '\n') { t->line++; t->col = 1; }
    else t->col++;
    t->i++;
}

static JSValue dyn_tml_err(dyn_tml_t *t, const char *fmt, ...)
{
    va_list ap;
    char buf[192], msg[256];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    snprintf(msg, sizeof msg, "TOML.parse: line %d col %d: %s", t->line, t->col, buf);
    return JS_ThrowSyntaxError(t->ctx, "%s", msg);
}

static void dyn_tml_ws(dyn_tml_t *t)
{
    int c;
    while ((c = dyn_tml_cur(t)) == ' ' || c == '\t') dyn_tml_adv(t);
}

static void dyn_tml_blank(dyn_tml_t *t)
{
    for (;;) {
        int c = dyn_tml_cur(t);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { dyn_tml_adv(t); continue; }
        if (c == '#') {
            while ((c = dyn_tml_cur(t)) != -1 && c != '\n') dyn_tml_adv(t);
            continue;
        }
        return;
    }
}

static int dyn_tml_digit(int c) { return c >= '0' && c <= '9'; }
static int dyn_tml_hexdig(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* define key = into table `obj`; dup = 1 refuses duplicates, 0 overwrites */
static int dyn_tml_define(dyn_tml_t *t, JSValue obj, const char *key, size_t klen,
                          JSValue v, int dup)
{
    JSAtom a = JS_NewAtomLen(t->ctx, key, klen);
    JSPropertyDescriptor desc;
    int r;
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(t->ctx, v);
        return -1;
    }
    r = JS_GetOwnProperty(t->ctx, &desc, obj, a);
    if (r < 0) {
        JS_FreeAtom(t->ctx, a);
        JS_FreeValue(t->ctx, v);
        return -1;
    }
    if (r && dup) {
        JS_FreeValue(t->ctx, desc.getter);
        JS_FreeValue(t->ctx, desc.setter);
        JS_FreeValue(t->ctx, desc.value);
        JS_FreeAtom(t->ctx, a);
        JS_FreeValue(t->ctx, v);
        JS_ThrowSyntaxError(t->ctx, "TOML.parse: line %d col %d: duplicate key", t->line, t->col);
        return -1;
    }
    r = JS_DefinePropertyValue(t->ctx, obj, a, v, JS_PROP_C_W_E);
    JS_FreeAtom(t->ctx, a);
    return r < 0 ? -1 : 0;
}

/* ---- strings ----------------------------------------------------------- */

static JSValue dyn_tml_str_new(dyn_tml_t *t, dyn_tml_sb_t *sb)
{
    JSValue r = JS_NewStringLen(t->ctx, sb->p ? sb->p : "", sb->n);
    dyn_tml_sb_free(sb);
    return r;
}

static int dyn_tml_escape(dyn_tml_t *t, dyn_tml_sb_t *sb)
{
    int c = dyn_tml_cur(t);
    dyn_tml_adv(t);
    switch (c) {
    case 'b': dyn_tml_sb_putc(sb, '\b'); return 0;
    case 't': dyn_tml_sb_putc(sb, '\t'); return 0;
    case 'n': dyn_tml_sb_putc(sb, '\n'); return 0;
    case 'f': dyn_tml_sb_putc(sb, '\f'); return 0;
    case 'r': dyn_tml_sb_putc(sb, '\r'); return 0;
    case '"': dyn_tml_sb_putc(sb, '"'); return 0;
    case '\\': dyn_tml_sb_putc(sb, '\\'); return 0;
    case 'u': case 'U': {
        int want = (c == 'u') ? 4 : 8, k;
        uint32_t cp = 0;
        for (k = 0; k < want; k++) {
            int h = dyn_tml_cur(t);
            if (!dyn_tml_hexdig(h)) {
                dyn_tml_err(t, "invalid unicode escape");
                return -1;
            }
            cp = cp * 16 + (uint32_t)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
            dyn_tml_adv(t);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            dyn_tml_err(t, "escape is not a Unicode scalar value");
            return -1;
        }
        {
            uint8_t b[4];
            int len = unicode_to_utf8(b, cp);
            dyn_tml_sb_put(sb, (const char *)b, (size_t)len);
        }
        return 0;
    }
    default:
        dyn_tml_err(t, "invalid escape \\%c", c);
        return -1;
    }
}

static JSValue dyn_tml_basic_string(dyn_tml_t *t)
{
    dyn_tml_sb_t sb;
    int c;
    dyn_tml_sb_init(&sb);
    dyn_tml_adv(t);                              /* opening quote */
    for (;;) {
        c = dyn_tml_cur(t);
        if (c == -1 || c == '\n') {
            dyn_tml_err(t, "unterminated string");
            dyn_tml_sb_free(&sb);
            return JS_EXCEPTION;
        }
        if (c == '"') { dyn_tml_adv(t); break; }
        if (c == '\\') {
            dyn_tml_adv(t);
            if (dyn_tml_escape(t, &sb) < 0) { dyn_tml_sb_free(&sb); return JS_EXCEPTION; }
            continue;
        }
        if (c < 0x20 && c != '\t') {
            dyn_tml_err(t, "control character in a string");
            dyn_tml_sb_free(&sb);
            return JS_EXCEPTION;
        }
        dyn_tml_sb_putc(&sb, (char)c);
        dyn_tml_adv(t);
    }
    return dyn_tml_str_new(t, &sb);
}

static JSValue dyn_tml_lit_string(dyn_tml_t *t)
{
    dyn_tml_sb_t sb;
    int c;
    dyn_tml_sb_init(&sb);
    dyn_tml_adv(t);
    for (;;) {
        c = dyn_tml_cur(t);
        if (c == -1 || c == '\n') {
            dyn_tml_err(t, "unterminated literal string");
            dyn_tml_sb_free(&sb);
            return JS_EXCEPTION;
        }
        if (c == '\'') { dyn_tml_adv(t); break; }
        if (c < 0x20 && c != '\t') {
            dyn_tml_err(t, "control character in a literal string");
            dyn_tml_sb_free(&sb);
            return JS_EXCEPTION;
        }
        dyn_tml_sb_putc(&sb, (char)c);
        dyn_tml_adv(t);
    }
    return dyn_tml_str_new(t, &sb);
}

static JSValue dyn_tml_ml_basic(dyn_tml_t *t)
{
    dyn_tml_sb_t sb;
    int c;
    dyn_tml_sb_init(&sb);
    t->i += 3; t->col += 3;
    /* trim the first newline -- a CRLF file writes that newline as CRLF */
    if (dyn_tml_cur(t) == '\r' && dyn_tml_peek(t, 1) == '\n') { dyn_tml_adv(t); dyn_tml_adv(t); }
    else if (dyn_tml_cur(t) == '\n') dyn_tml_adv(t);
    for (;;) {
        c = dyn_tml_cur(t);
        if (c == -1) {
            dyn_tml_err(t, "unterminated multiline string");
            dyn_tml_sb_free(&sb);
            return JS_EXCEPTION;
        }
        if (c == '"' && dyn_tml_peek(t, 1) == '"' && dyn_tml_peek(t, 2) == '"') {
            t->i += 3; t->col += 3;
            break;
        }
        if (c == '\\') {
            dyn_tml_adv(t);
            c = dyn_tml_cur(t);
            if (c == '\n') { dyn_tml_adv(t); continue; }          /* line-ending backslash */
            if (c == ' ' || c == '\t') {                          /* trimmed newline */
                while ((c = dyn_tml_cur(t)) == ' ' || c == '\t') dyn_tml_adv(t);
                if (c == '\n') { dyn_tml_adv(t); continue; }
                dyn_tml_err(t, "invalid multiline escape");
                dyn_tml_sb_free(&sb);
                return JS_EXCEPTION;
            }
            if (c == '\r' && dyn_tml_peek(t, 1) == '\n') {
                dyn_tml_adv(t); dyn_tml_adv(t);
                continue;
            }
            if (dyn_tml_escape(t, &sb) < 0) { dyn_tml_sb_free(&sb); return JS_EXCEPTION; }
            continue;
        }
        /* the spec stores what CRLF writes as LF */
        if (c == '\r' && dyn_tml_peek(t, 1) == '\n') {
            dyn_tml_adv(t);
            continue;
        }
        dyn_tml_sb_putc(&sb, (char)c);
        dyn_tml_adv(t);
    }
    return dyn_tml_str_new(t, &sb);
}

static JSValue dyn_tml_ml_lit(dyn_tml_t *t)
{
    dyn_tml_sb_t sb;
    int c;
    dyn_tml_sb_init(&sb);
    t->i += 3; t->col += 3;
    if (dyn_tml_cur(t) == '\r' && dyn_tml_peek(t, 1) == '\n') { dyn_tml_adv(t); dyn_tml_adv(t); }
    else if (dyn_tml_cur(t) == '\n') dyn_tml_adv(t);
    for (;;) {
        c = dyn_tml_cur(t);
        if (c == -1) {
            dyn_tml_err(t, "unterminated multiline literal");
            dyn_tml_sb_free(&sb);
            return JS_EXCEPTION;
        }
        if (c == '\'' && dyn_tml_peek(t, 1) == '\'' && dyn_tml_peek(t, 2) == '\'') {
            t->i += 3; t->col += 3;
            break;
        }
        /* as in the multi-line basic string: CRLF is stored as LF */
        if (c == '\r' && dyn_tml_peek(t, 1) == '\n') {
            dyn_tml_adv(t);
            continue;
        }
        dyn_tml_sb_putc(&sb, (char)c);
        dyn_tml_adv(t);
    }
    return dyn_tml_str_new(t, &sb);
}

/* ---- numbers and dates ------------------------------------------------- */

/* the token already passed the grammar validator */
static JSValue dyn_tml_number(dyn_tml_t *t, const char *tok, size_t n, int is_float)
{
    char buf[128], clean[128];   /* n < sizeof buf above; clean must hold n digits */
    size_t j, k;
    if (n >= sizeof buf) {
        return dyn_tml_err(t, "number too long");
    }
    memcpy(buf, tok, n);
    buf[n] = 0;
    if (is_float) {
        double d = strtod(buf, NULL);   /* validated: digits . e + - only */
        /* NOT JS_NewFloat64, which normalises `5.0` to the int 5 -- the exact
           slip that let stringify print a parsed float back as `5`, for any
           TOML reader to re-parse as an integer. The raw constructor keeps
           the FLOAT64 tag that carries the int/double distinction to
           stringify's emit. */
        return __JS_NewFloat64(t->ctx, d);
    }
    k = 0;
    for (j = 0; j < n; j++)
        if (buf[j] != '_') clean[k++] = buf[j];
    clean[k] = 0;
    {
        int sign = 1, base = 10;
        uint64_t v = 0;
        j = 0;
        if (clean[0] == '+' || clean[0] == '-') { sign = clean[0] == '-' ? -1 : 1; j = 1; }
        if (clean[j] == '0' && j + 1 < k && (clean[j+1] | 32) == 'x') { base = 16; j += 2; }
        else if (clean[j] == '0' && j + 1 < k && (clean[j+1] | 32) == 'o') { base = 8; j += 2; }
        else if (clean[j] == '0' && j + 1 < k && (clean[j+1] | 32) == 'b') { base = 2; j += 2; }
        for (; j < k; j++) {
            int d = clean[j] <= '9' ? clean[j] - '0' : (clean[j] | 32) - 'a' + 10;
            if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) {
                return dyn_tml_err(t, "integer out of 64-bit range");
            }
            v = v * (uint64_t)base + (uint64_t)d;
        }
        /* the spec's rule, and the same error the overflow check above uses:
           an integer that does not fit is INVALID, never a silent double */
        if (sign > 0) {
            if (v <= (uint64_t)INT64_MAX)
                return JS_NewInt64(t->ctx, (int64_t)v);
            return dyn_tml_err(t, "integer out of 64-bit range");
        }
        if (v <= (uint64_t)INT64_MAX + 1)
            return JS_NewInt64(t->ctx, -(int64_t)v);
        return dyn_tml_err(t, "integer out of 64-bit range");
    }
}

/* offset date-time, validated strictly, RFC 3339 form. */
/* TOML 1.0 date-times, all forms: local-date, local-time, local-date-time
   and offset-date-time. Strict RFC 3339 grammars; the value is the raw
   string (JS has no TOML date type -- documented cut). */
static JSValue dyn_tml_datetime(dyn_tml_t *t)
{
    size_t i = t->i;
    const char *s = t->src;
    int mo = 0, dd = 0, hh = 0, mi = 0, ss = 0;

    if (dyn_tml_peek(t, 2) == ':') {
        /* local-time: HH:MM:SS(.frac)? */
        if (i + 8 > t->n || !dyn_tml_digit((unsigned char)s[i]) ||
            !dyn_tml_digit((unsigned char)s[i+1]) || s[i+2] != ':' ||
            !dyn_tml_digit((unsigned char)s[i+3]) || !dyn_tml_digit((unsigned char)s[i+4]) ||
            s[i+5] != ':' || !dyn_tml_digit((unsigned char)s[i+6]) ||
            !dyn_tml_digit((unsigned char)s[i+7]))
            goto bad;
        hh = (s[i]-'0')*10 + (s[i+1]-'0');
        mi = (s[i+3]-'0')*10 + (s[i+4]-'0');
        ss = (s[i+6]-'0')*10 + (s[i+7]-'0');
        if (hh > 23 || mi > 59 || ss > 60)
            goto bad;
        i += 8;
        if (i < t->n && s[i] == '.') {
            i++;
            if (i >= t->n || !dyn_tml_digit((unsigned char)s[i]))
                goto bad;                         /* empty fraction */
            while (i < t->n && dyn_tml_digit((unsigned char)s[i])) i++;
        }
    } else {
        /* full-date: YYYY-MM-DD */
        if (i + 10 > t->n || !dyn_tml_digit((unsigned char)s[i]) ||
            !dyn_tml_digit((unsigned char)s[i+1]) || !dyn_tml_digit((unsigned char)s[i+2]) ||
            !dyn_tml_digit((unsigned char)s[i+3]) || s[i+4] != '-' ||
            !dyn_tml_digit((unsigned char)s[i+5]) || !dyn_tml_digit((unsigned char)s[i+6]) ||
            s[i+7] != '-' || !dyn_tml_digit((unsigned char)s[i+8]) ||
            !dyn_tml_digit((unsigned char)s[i+9]))
            goto bad;
        mo = (s[i+5]-'0')*10 + (s[i+6]-'0');
        dd = (s[i+8]-'0')*10 + (s[i+9]-'0');
        if (mo < 1 || mo > 12 || dd < 1 || dd > 31)
            goto bad;
        i += 10;
        /* local-date alone, or local-date-time with a T/space separator */
        if (i < t->n && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
            i++;
            if (i + 8 > t->n || !dyn_tml_digit((unsigned char)s[i]) ||
                !dyn_tml_digit((unsigned char)s[i+1]) || s[i+2] != ':' ||
                !dyn_tml_digit((unsigned char)s[i+3]) || !dyn_tml_digit((unsigned char)s[i+4]) ||
                s[i+5] != ':' || !dyn_tml_digit((unsigned char)s[i+6]) ||
                !dyn_tml_digit((unsigned char)s[i+7]))
                goto bad;
            hh = (s[i]-'0')*10 + (s[i+1]-'0');
            mi = (s[i+3]-'0')*10 + (s[i+4]-'0');
            ss = (s[i+6]-'0')*10 + (s[i+7]-'0');
            if (hh > 23 || mi > 59 || ss > 60)
                goto bad;
            i += 8;
            if (i < t->n && s[i] == '.') {
                i++;
                if (i >= t->n || !dyn_tml_digit((unsigned char)s[i]))
                    goto bad;                     /* empty fraction */
                while (i < t->n && dyn_tml_digit((unsigned char)s[i])) i++;
            }
            /* the zone is OPTIONAL for local-date-time */
            if (i < t->n && (s[i] == 'Z' || s[i] == 'z' || s[i] == '+' || s[i] == '-')) {
                if (s[i] == 'Z' || s[i] == 'z') {
                    i++;
                } else {
                    i++;
                    if (i + 5 > t->n || !dyn_tml_digit((unsigned char)s[i]) ||
                        !dyn_tml_digit((unsigned char)s[i+1]) || s[i+2] != ':' ||
                        !dyn_tml_digit((unsigned char)s[i+3]) || !dyn_tml_digit((unsigned char)s[i+4]))
                        goto bad;
                    if ((s[i]-'0')*10 + (s[i+1]-'0') > 23 ||
                        (s[i+3]-'0')*10 + (s[i+4]-'0') > 59)
                        goto bad;
                    i += 5;
                }
            }
        }
    }
    {
        JSValue r = JS_NewStringLen(t->ctx, s + t->i, i - t->i);
        t->col += (int)(i - t->i);
        t->i = i;
        return r;
    }
bad:
    return dyn_tml_err(t, "invalid date-time");
}

/* ---- values ------------------------------------------------------------ */

static JSValue dyn_tml_value(dyn_tml_t *t, int depth);

static JSValue dyn_tml_array(dyn_tml_t *t, int depth)
{
    JSValue a = JS_NewArray(t->ctx);
    uint64_t n = 0;
    if (JS_IsException(a))
        return a;
    if (depth >= DYN_TOML_MAX_DEPTH) {
        JS_FreeValue(t->ctx, a);
        return dyn_tml_err(t, "nesting exceeds %d", DYN_TOML_MAX_DEPTH);
    }
    dyn_tml_adv(t);                              /* [ */
    dyn_tml_blank(t);
    if (dyn_tml_cur(t) == ']') { dyn_tml_adv(t); return a; }
    for (;;) {
        JSValue v = dyn_tml_value(t, depth + 1);
        if (JS_IsException(v)) { JS_FreeValue(t->ctx, a); return JS_EXCEPTION; }
        if (JS_DefinePropertyValueUint32(t->ctx, a, (uint32_t)n++, v,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(t->ctx, a);
            return JS_EXCEPTION;
        }
        dyn_tml_blank(t);
        if (dyn_tml_cur(t) == ']') { dyn_tml_adv(t); return a; }
        if (dyn_tml_cur(t) != ',') {
            dyn_tml_err(t, "expected ',' or ']' in an array");
            JS_FreeValue(t->ctx, a);
            return JS_EXCEPTION;
        }
        dyn_tml_adv(t);
        dyn_tml_blank(t);
        if (dyn_tml_cur(t) == ']') { dyn_tml_adv(t); return a; }
    }
}

static JSValue dyn_tml_inline_table(dyn_tml_t *t, int depth)
{
    JSValue o = JS_NewObject(t->ctx);
    if (JS_IsException(o))
        return o;
    if (depth >= DYN_TOML_MAX_DEPTH) {
        JS_FreeValue(t->ctx, o);
        return dyn_tml_err(t, "nesting exceeds %d", DYN_TOML_MAX_DEPTH);
    }
    /* An inline table is CLOSED the moment it is written: `[a]` after
       `a = {x = 1}` must be refused, so it joins the no-redefine set. */
    if (dyn_tml_mark(t, o) < 0) {
        JS_FreeValue(t->ctx, o);
        JS_ThrowOutOfMemory(t->ctx);
        return JS_EXCEPTION;
    }
    dyn_tml_adv(t);                              /* { */
    dyn_tml_ws(t);
    if (dyn_tml_cur(t) == '}') { dyn_tml_adv(t); return o; }
    for (;;) {
        dyn_tml_sb_t key;
        JSValue v;
        dyn_tml_ws(t);
        dyn_tml_sb_init(&key);
        {
            int c = dyn_tml_cur(t);
            size_t start = t->i;
            while (c != -1 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-')) {
                dyn_tml_adv(t);
                c = dyn_tml_cur(t);
            }
            if (t->i == start) {
                dyn_tml_err(t, "expected a key in an inline table");
                dyn_tml_sb_free(&key);
                JS_FreeValue(t->ctx, o);
                return JS_EXCEPTION;
            }
            dyn_tml_sb_put(&key, t->src + start, t->i - start);
        }
        dyn_tml_ws(t);
        if (dyn_tml_cur(t) != '=') {
            dyn_tml_err(t, "expected '=' in an inline table");
            dyn_tml_sb_free(&key);
            JS_FreeValue(t->ctx, o);
            return JS_EXCEPTION;
        }
        dyn_tml_adv(t);
        dyn_tml_ws(t);
        v = dyn_tml_value(t, depth + 1);
        if (JS_IsException(v)) {
            dyn_tml_sb_free(&key);
            JS_FreeValue(t->ctx, o);
            return JS_EXCEPTION;
        }
        if (dyn_tml_define(t, o, key.p, key.n, v, 1) < 0) {
            dyn_tml_sb_free(&key);
            JS_FreeValue(t->ctx, o);
            return JS_EXCEPTION;
        }
        dyn_tml_sb_free(&key);
        dyn_tml_ws(t);
        if (dyn_tml_cur(t) == '}') { dyn_tml_adv(t); return o; }
        if (dyn_tml_cur(t) != ',') {
            dyn_tml_err(t, "expected ',' or '}' in an inline table");
            JS_FreeValue(t->ctx, o);
            return JS_EXCEPTION;
        }
        dyn_tml_adv(t);
        dyn_tml_ws(t);
        if (dyn_tml_cur(t) == '}') { dyn_tml_adv(t); return o; }
    }
}

static JSValue dyn_tml_value(dyn_tml_t *t, int depth)
{
    int c = dyn_tml_cur(t);
    if (c == '"') {
        if (dyn_tml_peek(t, 1) == '"' && dyn_tml_peek(t, 2) == '"')
            return dyn_tml_ml_basic(t);
        return dyn_tml_basic_string(t);
    }
    if (c == '\'') {
        if (dyn_tml_peek(t, 1) == '\'' && dyn_tml_peek(t, 2) == '\'')
            return dyn_tml_ml_lit(t);
        return dyn_tml_lit_string(t);
    }
    if (c == '[')
        return dyn_tml_array(t, depth);
    if (c == '{')
        return dyn_tml_inline_table(t, depth);
    if (c == 't' && dyn_tml_peek(t, 1) == 'r' && dyn_tml_peek(t, 2) == 'u' && dyn_tml_peek(t, 3) == 'e') {
        t->i += 4; t->col += 4;
        return JS_TRUE;
    }
    if (c == 'f' && dyn_tml_peek(t, 1) == 'a' && dyn_tml_peek(t, 2) == 'l' &&
        dyn_tml_peek(t, 3) == 's' && dyn_tml_peek(t, 4) == 'e') {
        t->i += 5; t->col += 5;
        return JS_FALSE;
    }
    /* the spec's four SIGNED spellings, alongside the bare forms below */
    if ((c == '+' || c == '-') && dyn_tml_peek(t, 1) == 'i'
        && dyn_tml_peek(t, 2) == 'n' && dyn_tml_peek(t, 3) == 'f') {
        t->i += 4; t->col += 4;
        return JS_NewFloat64(t->ctx, c == '-' ? -1.0 / 0.0 : 1.0 / 0.0);
    }
    if ((c == '+' || c == '-') && dyn_tml_peek(t, 1) == 'n'
        && dyn_tml_peek(t, 2) == 'a' && dyn_tml_peek(t, 3) == 'n') {
        t->i += 4; t->col += 4;
        return JS_NewFloat64(t->ctx, 0.0 / 0.0);
    }
    if (c == 'i' && dyn_tml_peek(t, 1) == 'n' && dyn_tml_peek(t, 2) == 'f') {
        t->i += 3; t->col += 3;
        return JS_NewFloat64(t->ctx, 1.0 / 0.0);
    }
    if (c == 'n' && dyn_tml_peek(t, 1) == 'a' && dyn_tml_peek(t, 2) == 'n') {
        t->i += 3; t->col += 3;
        return JS_NewFloat64(t->ctx, 0.0 / 0.0);
    }
    /* date/time: YYYY-MM-DD or HH:MM:SS at the cursor */
    if (dyn_tml_digit(c) && dyn_tml_peek(t, 4) == '-' && dyn_tml_peek(t, 7) == '-')
        return dyn_tml_datetime(t);
    if (dyn_tml_digit(c) && dyn_tml_peek(t, 2) == ':')
        return dyn_tml_datetime(t);
    {
        /* number: strict scan + strict grammar validation */
        size_t start = t->i, j;
        int is_float = 0;
        if (c == '+' || c == '-') { dyn_tml_adv(t); c = dyn_tml_cur(t); }
        while (c != -1 && (dyn_tml_digit(c) || c == '_' || c == '.' ||
                           c == 'e' || c == 'E' || c == 'x' || c == 'X' ||
                           c == 'o' || c == 'O' || c == 'b' || c == 'B' ||
                           c == 'a' || c == 'f' || c == 'A' || c == 'F' ||
                           c == 'c' || c == 'C' || c == 'd' || c == 'D')) {
            dyn_tml_adv(t);
            c = dyn_tml_cur(t);
        }
        /* classify AFTER the scan: a 0x/0o/0b prefix is always an integer
           (hex digits like e/a/d must not mark the token as a float) */
        if (!((t->i >= start + 2 && t->src[start] == '0' &&
               (t->src[start+1] == 'x' || t->src[start+1] == 'X' ||
                t->src[start+1] == 'o' || t->src[start+1] == 'O' ||
                t->src[start+1] == 'b' || t->src[start+1] == 'B')) ||
              (t->i >= start + 3 && (t->src[start] == '+' || t->src[start] == '-') &&
               t->src[start+1] == '0' && (t->src[start+2] == 'x' || t->src[start+2] == 'X' ||
                t->src[start+2] == 'o' || t->src[start+2] == 'O' ||
                t->src[start+2] == 'b' || t->src[start+2] == 'B')))) {
            size_t j;
            for (j = start; j < t->i; j++)
                if (t->src[j] == '.' || t->src[j] == 'e' || t->src[j] == 'E')
                    is_float = 1;
        }
        if (t->i == start || (t->i == start + 1 && (t->src[start] == '+' || t->src[start] == '-')))
            return dyn_tml_err(t, "expected a value");
        /* a letter or digit glued to the token is a malformed number,
           not two tokens ("0x2g", "12abc") */
        if ((c = dyn_tml_cur(t)) != -1 &&
            ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || dyn_tml_digit(c)))
            return dyn_tml_err(t, "malformed number");
        {
            const char *tok = t->src + start;
            size_t n = t->i - start, k = 0;
            int digits = 0, seen_dot = 0, seen_exp = 0, hi = '9', lo = '0';
            if (tok[0] == '+' || tok[0] == '-') k = 1;
            if (k < n && tok[k] == '0' && k + 1 < n &&
                (tok[k+1] == 'x' || tok[k+1] == 'X')) { hi = 'f'; lo = '0'; k += 2; }
            else if (k < n && tok[k] == '0' && k + 1 < n &&
                (tok[k+1] == 'o' || tok[k+1] == 'O')) { hi = '7'; lo = '0'; k += 2; }
            else if (k < n && tok[k] == '0' && k + 1 < n &&
                (tok[k+1] == 'b' || tok[k+1] == 'B')) { hi = '1'; lo = '0'; k += 2; }
            for (j = k; j < n; j++) {
                char ch = tok[j];
                if (ch == '_') {
                    if (j == 0 || !dyn_tml_digit((unsigned char)tok[j-1]) ||
                        j + 1 >= n || !dyn_tml_digit((unsigned char)tok[j+1]))
                        return dyn_tml_err(t, "malformed number (underscore placement)");
                    continue;
                }
                if (is_float) {
                    if (dyn_tml_digit((unsigned char)ch)) { digits++; continue; }
                    if (ch == '.' && !seen_dot && !seen_exp) {
                        if (digits == 0)
                            return dyn_tml_err(t, "malformed float (no digit before the dot)");
                        seen_dot = 1;
                        /* a digit must follow the dot ("1." is invalid) */
                        if (j + 1 >= n || !dyn_tml_digit((unsigned char)tok[j+1]))
                            return dyn_tml_err(t, "malformed float (no digit after the dot)");
                        continue;
                    }
                    if ((ch == 'e' || ch == 'E') && !seen_exp && digits > 0) {
                        size_t e2 = j + 1;
                        seen_exp = 1;
                        if (e2 < n && (tok[e2] == '+' || tok[e2] == '-')) e2++;
                        /* a digit must follow the exponent ("1e" is invalid) */
                        if (e2 >= n || !dyn_tml_digit((unsigned char)tok[e2]))
                            return dyn_tml_err(t, "malformed float (no digit after the exponent)");
                        if (j + 1 < n && (tok[j+1] == '+' || tok[j+1] == '-')) j++;
                        continue;
                    }
                    return dyn_tml_err(t, "malformed float");
                }
                if (!dyn_tml_hexdig((unsigned char)ch))
                    return dyn_tml_err(t, "malformed integer");
                if ((ch | 32) > hi || (ch | 32) < lo)
                    return dyn_tml_err(t, "digit outside the number base");
                digits++;
            }
            if (digits == 0)
                return dyn_tml_err(t, "malformed number");
            /* TOML forbids leading zeros in the integer part (ints AND the
               integer part of floats); 0x/0o/0b prefixes are exempt */
            if (!(n >= 3 && (tok[1] == 'x' || tok[1] == 'X' ||
                tok[1] == 'o' || tok[1] == 'O' || tok[1] == 'b' || tok[1] == 'B'))) {
                size_t k0 = (tok[0] == '+' || tok[0] == '-') ? 1 : 0;
                if (k0 < n && tok[k0] == '0' && k0 + 1 < n &&
                    tok[k0+1] != '.' && tok[k0+1] != 'e' && tok[k0+1] != 'E')
                    return dyn_tml_err(t, "leading zero in the integer part");
            }
            return dyn_tml_number(t, tok, n, is_float);
        }
    }
}

/* ---- the document ------------------------------------------------------ */

/* Split a dotted key into segments; walk/create all but the LAST, and
   define nothing yet. Returns the owning table for the final segment in
   *parent; the final segment is [key + *pseg, key + klen). */
static int dyn_tml_walk_key(dyn_tml_t *t, JSValue root, const char *key,
                            size_t klen, JSValue *parent, size_t *pseg)
{
    JSValue cur = JS_DupValue(t->ctx, root);
    size_t seg = 0, depth = 0;
    for (;;) {
        size_t e = seg;
        int is_last = 0;
        JSAtom a;
        if (++depth > DYN_TOML_MAX_DEPTH) {
            JS_FreeValue(t->ctx, cur);
            JS_ThrowSyntaxError(t->ctx,
                "TOML.parse: line %d col %d: key nesting exceeds %d",
                t->line, t->col, DYN_TOML_MAX_DEPTH);
            return -1;
        }
        while (e < klen && key[e] != '.') e++;
        if (e >= klen)
            is_last = 1;
        a = JS_NewAtomLen(t->ctx, key + seg, e - seg);
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(t->ctx, cur);
            return -1;
        }
        if (is_last) {
            JS_FreeAtom(t->ctx, a);
            *parent = cur;
            *pseg = seg;
            return 0;
        }
        {
            JSPropertyDescriptor desc;
            JSValue nxt;
            int r = JS_GetOwnProperty(t->ctx, &desc, cur, a);
            if (r < 0) {
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (r) {
                JS_FreeValue(t->ctx, desc.getter);
                JS_FreeValue(t->ctx, desc.setter);
                nxt = desc.value;
                if (!JS_IsObject(nxt) || JS_IsArray(t->ctx, nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: a dotted key crosses a value",
                        t->line, t->col); return -1;
                }
                JS_FreeValue(t->ctx, cur);
                cur = nxt;
            } else {
                nxt = JS_NewObject(t->ctx);
                if (JS_IsException(nxt)) {
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                if (JS_DefinePropertyValue(t->ctx, cur, a, JS_DupValue(t->ctx, nxt),
                                           JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                /* a table an implicit dotted-key segment created cannot be
                   redefined by a later `[header]` either */
                if (dyn_tml_mark(t, nxt) < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowOutOfMemory(t->ctx);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                cur = nxt;
            }
        }
        JS_FreeAtom(t->ctx, a);
        seg = e + 1;
    }
}

/* table header [a.b] or [[a.b]]: walk/create ALL segments; returns the
   table (or the new array element) in *out. */
static int dyn_tml_header(dyn_tml_t *t, JSValue root, const char *key,
                          size_t klen, int is_array, JSValue *out)
{
    JSValue cur = JS_DupValue(t->ctx, root);
    size_t seg = 0;
    for (;;) {
        size_t e = seg;
        JSAtom a;
        while (e < klen && key[e] != '.') e++;
        a = JS_NewAtomLen(t->ctx, key + seg, e - seg);
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(t->ctx, cur);
            return -1;
        }
        if (e >= klen) {
            /* the last segment */
            JSPropertyDescriptor desc;
            JSValue nxt;
            int r = JS_GetOwnProperty(t->ctx, &desc, cur, a);
            if (r < 0) {
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (r) {
                JS_FreeValue(t->ctx, desc.getter);
                JS_FreeValue(t->ctx, desc.setter);
                nxt = desc.value;
                if (is_array) {
                    if (!JS_IsArray(t->ctx, nxt)) {
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        JS_FreeValue(t->ctx, cur);
                        JS_ThrowSyntaxError(t->ctx,
                            "TOML.parse: line %d col %d: a table conflicts with "
                            "an array-of-tables", t->line, t->col); return -1;
                    }
                    /* append one element */
                    {
                        JSValue el = JS_NewObject(t->ctx);
                        JSValue lv;
                        uint64_t ln = 0;
                        if (JS_IsException(el)) {
                            JS_FreeValue(t->ctx, nxt);
                            JS_FreeAtom(t->ctx, a);
                            JS_FreeValue(t->ctx, cur);
                            return -1;
                        }
                        lv = JS_GetPropertyStr(t->ctx, nxt, "length");
                        JS_ToIndex(t->ctx, &ln, lv);
                        JS_FreeValue(t->ctx, lv);
                        if (JS_DefinePropertyValueUint32(t->ctx, nxt, (uint32_t)ln,
                                                         JS_DupValue(t->ctx, el),
                                                         JS_PROP_C_W_E) < 0) {
                            JS_FreeValue(t->ctx, nxt);
                            JS_FreeAtom(t->ctx, a);
                            JS_FreeValue(t->ctx, cur);
                            return -1;
                        }
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        JS_FreeValue(t->ctx, cur);
                        *out = el;
                        return 0;
                    }
                }
                if (!JS_IsObject(nxt) || JS_IsArray(t->ctx, nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: a table conflicts with a value",
                        t->line, t->col); return -1;
                }
                /* a table defined by an EARLIER header, or closed over by an
                   inline table or a dotted key, cannot be redefined */
                if (dyn_tml_marked(t, nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: table already defined",
                        t->line, t->col);
                    return -1;
                }
                if (dyn_tml_mark(t, nxt) < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowOutOfMemory(t->ctx);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                JS_FreeAtom(t->ctx, a);
                *out = nxt;
                return 0;
            }
            if (is_array) {
                nxt = JS_NewArray(t->ctx);
                if (JS_IsException(nxt)) {
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                if (JS_DefinePropertyValue(t->ctx, cur, a, JS_DupValue(t->ctx, nxt),
                                           JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                {
                    JSValue el = JS_NewObject(t->ctx);
                    if (JS_IsException(el)) {
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        return -1;
                    }
                    if (JS_DefinePropertyValueUint32(t->ctx, nxt, 0,
                                                     JS_DupValue(t->ctx, el),
                                                     JS_PROP_C_W_E) < 0) {
                        JS_FreeValue(t->ctx, el);
                        JS_FreeValue(t->ctx, nxt);
                        JS_FreeAtom(t->ctx, a);
                        return -1;
                    }
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    *out = el;
                    return 0;
                }
            }
            nxt = JS_NewObject(t->ctx);
            if (JS_IsException(nxt)) {
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (JS_DefinePropertyValue(t->ctx, cur, a, JS_DupValue(t->ctx, nxt),
                                       JS_PROP_C_W_E) < 0) {
                JS_FreeValue(t->ctx, nxt);
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (dyn_tml_mark(t, nxt) < 0) {
                JS_FreeValue(t->ctx, nxt);
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                JS_ThrowOutOfMemory(t->ctx);
                return -1;
            }
            JS_FreeValue(t->ctx, cur);
            JS_FreeAtom(t->ctx, a);
            *out = nxt;
            return 0;
        }
        {
            JSPropertyDescriptor desc;
            JSValue nxt;
            int r = JS_GetOwnProperty(t->ctx, &desc, cur, a);
            if (r < 0) {
                JS_FreeAtom(t->ctx, a);
                JS_FreeValue(t->ctx, cur);
                return -1;
            }
            if (r) {
                JS_FreeValue(t->ctx, desc.getter);
                JS_FreeValue(t->ctx, desc.setter);
                nxt = desc.value;
                if (!JS_IsObject(nxt) || JS_IsArray(t->ctx, nxt)) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    JS_ThrowSyntaxError(t->ctx,
                        "TOML.parse: line %d col %d: a table header crosses a value",
                        t->line, t->col); return -1;
                }
                JS_FreeValue(t->ctx, cur);
                cur = nxt;
            } else {
                nxt = JS_NewObject(t->ctx);
                if (JS_IsException(nxt)) {
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                if (JS_DefinePropertyValue(t->ctx, cur, a, JS_DupValue(t->ctx, nxt),
                                           JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(t->ctx, nxt);
                    JS_FreeAtom(t->ctx, a);
                    JS_FreeValue(t->ctx, cur);
                    return -1;
                }
                JS_FreeValue(t->ctx, cur);
                cur = nxt;
            }
        }
        JS_FreeAtom(t->ctx, a);
        seg = e + 1;
    }
}

static JSValue dyn_tml_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_tml_t t;
    JSValue root, active;
    const char *s;
    size_t n;
    (void)this_val;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "TOML.parse(text): text must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    dyn_cfg_bom(&s, &n);
    memset(&t, 0, sizeof t);
    t.ctx = ctx;
    t.src = s;
    t.n = n;
    t.line = 1;
    t.col = 1;
    root = JS_NewObject(ctx);
    if (JS_IsException(root)) {
        JS_FreeCString(ctx, s);
        return root;
    }
    active = JS_DupValue(ctx, root);

    dyn_tml_blank(&t);
    while (t.i < t.n) {
        int c = dyn_tml_cur(&t);
        if (c == '[') {
            int is_array = dyn_tml_peek(&t, 1) == '[';
            dyn_tml_sb_t path;
            JSValue nact;
            dyn_tml_sb_init(&path);
            t.i += is_array ? 2 : 1;
            t.col += is_array ? 2 : 1;
            dyn_tml_ws(&t);
            {
                size_t start = t.i;
                while ((c = dyn_tml_cur(&t)) != -1 && c != ']')
                    dyn_tml_adv(&t);
                if (t.i == start) {
                    dyn_tml_err(&t, "empty table header");
                    dyn_tml_sb_free(&path);
                    JS_FreeValue(ctx, active);
                    JS_FreeCString(ctx, s);
                    JS_FreeValue(ctx, root);
                    dyn_tml_marks_free(&t);
                    return JS_EXCEPTION;
                }
                dyn_tml_sb_put(&path, t.src + start, t.i - start);
            }
            c = dyn_tml_cur(&t);
            if ((!is_array && c != ']') || (is_array && (c != ']' || dyn_tml_peek(&t, 1) != ']'))) {
                dyn_tml_err(&t, "malformed table header");
                dyn_tml_sb_free(&path);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_marks_free(&t);
                return JS_EXCEPTION;
            }
            t.i += is_array ? 2 : 1;
            t.col += is_array ? 2 : 1;
            if (dyn_tml_header(&t, root, path.p, path.n, is_array, &nact) < 0) {
                dyn_tml_sb_free(&path);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_marks_free(&t);
                return JS_EXCEPTION;
            }
            dyn_tml_sb_free(&path);
            c = dyn_tml_cur(&t);
            if (c != -1 && c != '\n' && c != '\r' && c != ' ' && c != '\t' && c != '#') {
                dyn_tml_err(&t, "junk after a table header");
                JS_FreeValue(ctx, nact);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_marks_free(&t);
                return JS_EXCEPTION;
            }
            while (c != -1 && c != '\n') { dyn_tml_adv(&t); c = dyn_tml_cur(&t); }
            JS_FreeValue(ctx, active);
            active = nact;
            dyn_tml_blank(&t);
            continue;
        }
        if (c == '#' || c == '\n' || c == '\r') {
            dyn_tml_blank(&t);
            continue;
        }
        {
            /* key = value; the key may be dotted */
            dyn_tml_sb_t key;
            JSValue v, parent;
            size_t start, e, seg;
            dyn_tml_sb_init(&key);
            start = t.i;
            while ((c = dyn_tml_cur(&t)) != -1 && c != '=' && c != '\n' && c != '\r')
                dyn_tml_adv(&t);
            if (t.i == start) {
                dyn_tml_sb_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_marks_free(&t);
                return dyn_tml_err(&t, "expected key = value");
            }
            e = t.i;
            while (e > start && (t.src[e-1] == ' ' || t.src[e-1] == '\t')) e--;
            dyn_tml_sb_put(&key, t.src + start, e - start);
            if (dyn_tml_cur(&t) != '=') {
                dyn_tml_sb_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_marks_free(&t);
                return dyn_tml_err(&t, "expected '='");
            }
            dyn_tml_adv(&t);
            dyn_tml_ws(&t);
            v = dyn_tml_value(&t, 0);
            if (JS_IsException(v)) {
                dyn_tml_sb_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_marks_free(&t);
                return v;
            }
            /* the parent chain for the dotted key starts at the ACTIVE
               table (the table header it follows, if any) */
            if (dyn_tml_walk_key(&t, active, key.p, key.n, &parent, &seg) < 0) {
                dyn_tml_sb_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_marks_free(&t);
                return JS_EXCEPTION;
            }
            if (dyn_tml_define(&t, parent, key.p + seg, key.n - seg, v, 1) < 0) {
                JS_FreeValue(ctx, parent);
                dyn_tml_sb_free(&key);
                JS_FreeValue(ctx, active);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, root);
                dyn_tml_marks_free(&t);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, parent);
            dyn_tml_sb_free(&key);
            dyn_tml_blank(&t);
            continue;
        }
    }
    JS_FreeValue(ctx, active);
    JS_FreeCString(ctx, s);
    dyn_tml_marks_free(&t);
    return root;
}

/* ---- stringify (canonical; round-trip editing cut) --------------------- */

static const uint8_t toml_esc_map[256] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  /* 0x00..0x1F (control chars) */
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* '"' at 0x22 */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,  /* '\\' at 0x5C */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,  /* 0x7F */
};

static void dyn_tml_sb_escape(dyn_tml_sb_t *b, const char *s, size_t n)
{
    size_t i = 0, start = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (!toml_esc_map[c]) {
            i++;
            continue;
        }
        if (i > start)
            dyn_tml_sb_put(b, s + start, i - start);
        switch (c) {
        case '"': dyn_tml_sb_put(b, "\\\"", 2); break;
        case '\\': dyn_tml_sb_put(b, "\\\\", 2); break;
        case '\n': dyn_tml_sb_put(b, "\\n", 2); break;
        case '\t': dyn_tml_sb_put(b, "\\t", 2); break;
        case '\r': dyn_tml_sb_put(b, "\\r", 2); break;
        case '\b': dyn_tml_sb_put(b, "\\b", 2); break;
        case '\f': dyn_tml_sb_put(b, "\\f", 2); break;
        default: {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            dyn_tml_sb_put(b, esc, 6);
            break;
        }
        }
        i++;
        start = i;
    }
    if (i > start)
        dyn_tml_sb_put(b, s + start, i - start);
}

static int dyn_tml_emit(JSContext *ctx, dyn_tml_sb_t *b, JSValueConst v, int depth);

static int dyn_tml_emit_string(JSContext *ctx, dyn_tml_sb_t *b, JSValueConst v)
{
    const char *s;
    size_t n;
    if (!JS_IsString(v))
        return -1;
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    dyn_tml_sb_putc(b, '"');
    dyn_tml_sb_escape(b, s, n);
    dyn_tml_sb_putc(b, '"');
    JS_FreeCString(ctx, s);
    return 0;
}

static int dyn_tml_emit(JSContext *ctx, dyn_tml_sb_t *b, JSValueConst v, int depth)
{
    if (depth > DYN_TOML_MAX_DEPTH)
        return -1;
    if (JS_IsString(v))
        return dyn_tml_emit_string(ctx, b, v);
    if (JS_IsBool(v)) {
        dyn_tml_sb_put(b, JS_ToBool(ctx, v) ? "true" : "false",
                       JS_ToBool(ctx, v) ? 4 : 5);
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d;
        JS_ToFloat64(ctx, &d, v);
        if (isnan(d)) { dyn_tml_sb_put(b, "nan", 3); return 0; }
        if (isinf(d)) { dyn_tml_sb_put(b, d > 0 ? "inf" : "-inf", 3); return 0; }
        {
            char num[64];
            int64_t iv;
            /* An integral FLOAT64-tagged value inside int32 range can only be
               a float the parser kept exact (`5.0`): every engine path that
               produces an integral number in int32 range tags it INT. Past
               int32 a big TOML INTEGER is itself FLOAT64-tagged, so there the
               int reading wins -- a JSValue cannot hold both facts. */
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v)) && d == floor(d)
                && d >= -2147483648.0 && d < 2147483648.0) {
                snprintf(num, sizeof num, "%.1f", d);
            } else if (JS_ToInt64(ctx, &iv, v) == 0 && (double)iv == d) {
                snprintf(num, sizeof num, "%lld", (long long)iv);
            } else {
                snprintf(num, sizeof num, "%.17g", d);
            }
            dyn_tml_sb_put(b, num, strlen(num));
            return 0;
        }
    }
    if (JS_IsArray(ctx, v)) {
        uint64_t len, i;
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        JS_ToIndex(ctx, &len, lv);
        JS_FreeValue(ctx, lv);
        dyn_tml_sb_putc(b, '[');
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            if (i) dyn_tml_sb_put(b, ", ", 2);
            if (JS_IsException(el) || dyn_tml_emit(ctx, b, el, depth + 1) < 0) {
                JS_FreeValue(ctx, el);
                return -1;
            }
            JS_FreeValue(ctx, el);
        }
        dyn_tml_sb_putc(b, ']');
        return 0;
    }
    if (JS_IsObject(v)) {
        JSPropertyEnum *tab = NULL;
        uint32_t n = 0, k;
        int is_root = (depth == 0);
        dyn_tml_sb_putc(b, is_root ? '\n' : '{');
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return -1;
        for (k = 0; k < n; k++) {
            JSValue el = JS_GetProperty(ctx, v, tab[k].atom);
            const char *ks = JS_AtomToCStringLen(ctx, NULL, tab[k].atom);
            if (!ks || JS_IsException(el)) {
                if (ks) JS_FreeCString(ctx, ks);
                JS_FreeValue(ctx, el);
                JS_FreePropertyEnum(ctx, tab, n);
                return -1;
            }
            size_t klen = strlen(ks);
            size_t m;
            int bare = klen > 0;
            for (m = 0; m < klen; m++) {
                char ch = ks[m];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
                    bare = 0;
            }
            if (k) dyn_tml_sb_put(b, is_root ? "\n" : ", ", is_root ? 1 : 2);
            if (bare)
                dyn_tml_sb_put(b, ks, klen);
            else
                dyn_tml_emit_string(ctx, b, JS_NewStringLen(ctx, ks, klen));
            dyn_tml_sb_put(b, " = ", 3);
            if (dyn_tml_emit(ctx, b, el, depth + 1) < 0) {
                JS_FreeValue(ctx, el);
                JS_FreeCString(ctx, ks);
                JS_FreePropertyEnum(ctx, tab, n);
                return -1;
            }
            JS_FreeValue(ctx, el);
        }
        JS_FreePropertyEnum(ctx, tab, n);
        if (!is_root)
            dyn_tml_sb_putc(b, '}');
        return 0;
    }
    /* TOML has no null: a null/undefined VALUE is refused rather than
       emitting text no TOML reader can parse back. */
    return -1;
}

static JSValue dyn_tml_stringify(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    dyn_tml_sb_t b;
    JSValue r;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "TOML.stringify(value): a value is required");
    /* a TOML document is key-value pairs at the top level: only a plain
       object is a valid root */
    if (!JS_IsObject(argv[0]) || JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "TOML.stringify(value): the document root must be an object");
    dyn_tml_sb_init(&b);
    if (dyn_tml_emit(ctx, &b, argv[0], 0) < 0 || b.oom) {
        dyn_tml_sb_free(&b);
        return JS_ThrowTypeError(ctx, "TOML.stringify: unsupported value");
    }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_tml_sb_free(&b);
    return r;
}

/* ------------------------------------------------------------ registration */

static const JSCFunctionListEntry dyn_ini_ns[] = {
    JS_CFUNC_DEF("parse", 1, dyn_ini_parse),
};
static const JSCFunctionListEntry dyn_env_ns[] = {
    JS_CFUNC_DEF("parse", 1, dyn_env_parse),
};
static const JSCFunctionListEntry dyn_fm_ns[] = {
    JS_CFUNC_DEF("split", 1, dyn_fm_split),
};
static const JSCFunctionListEntry dyn_toml_ns[] = {
    JS_CFUNC_DEF("parse", 1, dyn_tml_parse),
    JS_CFUNC_DEF("stringify", 1, dyn_tml_stringify),
};

static const char *const DYN_CFG_NAMES[] = { "INI", "Env", "FrontMatter", "TOML" };

static int dyn_config_init_module(JSContext *ctx, JSModuleDef *m)
{
    const JSCFunctionListEntry *tabs[4];
    int lens[4];
    size_t k;
    tabs[0] = dyn_ini_ns; lens[0] = countof(dyn_ini_ns);
    tabs[1] = dyn_env_ns; lens[1] = countof(dyn_env_ns);
    tabs[2] = dyn_fm_ns;  lens[2] = countof(dyn_fm_ns);
    tabs[3] = dyn_toml_ns; lens[3] = countof(dyn_toml_ns);
    for (k = 0; k < countof(DYN_CFG_NAMES); k++) {
        JSValue ns = JS_NewObject(ctx);
        if (JS_IsException(ns))
            return -1;
        JS_SetPropertyFunctionList(ctx, ns, tabs[k], lens[k]);
        if (JS_SetModuleExport(ctx, m, DYN_CFG_NAMES[k], ns) < 0)
            return -1;
    }
    return 0;
}

int js_nat_init_config(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:config", dyn_config_init_module);
    size_t k;
    if (!m)
        return -1;
    for (k = 0; k < countof(DYN_CFG_NAMES); k++)
        JS_AddModuleExport(ctx, m, DYN_CFG_NAMES[k]);
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_CONFIG */
