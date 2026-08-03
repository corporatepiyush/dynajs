/* dyna:config -- INI, .env and front-matter splitting. Four grammars would make
   a bare `parse` ambiguous, so each is a namespace object with static methods.
   Full API: docs/dynajs-guide/API.md. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CONFIG)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static const char *const DYN_CFG_NAMES[] = { "INI", "Env", "FrontMatter" };

static int dyn_config_init_module(JSContext *ctx, JSModuleDef *m)
{
    const JSCFunctionListEntry *tabs[3];
    int lens[3];
    size_t k;
    tabs[0] = dyn_ini_ns; lens[0] = countof(dyn_ini_ns);
    tabs[1] = dyn_env_ns; lens[1] = countof(dyn_env_ns);
    tabs[2] = dyn_fm_ns;  lens[2] = countof(dyn_fm_ns);
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
