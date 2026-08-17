/* dyna:xml -- one scanner, three front ends: streaming SAX, a document tree,
   and a serializer. NO DTD PROCESSING AT ALL, so XXE is unrepresentable rather
   than disabled. Full API: see the dyna:* module in dyna-libc.h. */
#include "dyna-nat.h"
#include "dyna-simd-kernels.h"   /* simd.find_first_of for the text scan */
#include "dyna-simd-kernels.h"   /* simd.find_first_of for the escape scan */

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_XML)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* Bounds are defaults ON, not options: every one of them is what stops a
   document from choosing how much memory the parser uses. */
#define XML_MAX_DEPTH   256
#define XML_MAX_ATTRS   1024
#define XML_MAX_TOKEN   (16u << 20)     /* one name, value or text run */
#define XML_MAX_INPUT   (256u << 20)    /* whole-document parse only */

/* ------------------------------------------------------------------ buffer */

typedef struct { uint8_t *p; size_t n, cap; int oom; } xb_t;

static void xb_init(xb_t *b) { b->p = NULL; b->n = 0; b->cap = 0; b->oom = 0; }
static void xb_free(xb_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* Overshoot costs ABSOLUTE bytes, so the factor decays with size. */
static size_t xb_grow(size_t cur, size_t need)
{
    size_t nc = cur ? cur : 64;
    while (nc < need) {
        if (nc < (1u << 16))      nc *= 2;
        else if (nc < (1u << 20)) nc += nc / 2;
        else                      nc += nc / 4;
    }
    return nc;
}

static void xb_write(xb_t *b, const void *p, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap) {
        size_t nc = xb_grow(b->cap, b->n + n);
        uint8_t *np = (uint8_t *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void xb_put(xb_t *b, uint8_t c) { xb_write(b, &c, 1); }

static void xb_puts(xb_t *b, const char *s) { xb_write(b, s, strlen(s)); }

/* A code point as UTF-8. */
static void xb_putc(xb_t *b, uint32_t cp)
{
    if (cp < 0x80) {
        xb_put(b, (uint8_t)cp);
    } else if (cp < 0x800) {
        xb_put(b, (uint8_t)(0xC0 | (cp >> 6)));
        xb_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        xb_put(b, (uint8_t)(0xE0 | (cp >> 12)));
        xb_put(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        xb_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else {
        xb_put(b, (uint8_t)(0xF0 | (cp >> 18)));
        xb_put(b, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
        xb_put(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        xb_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    }
}

/* ------------------------------------------------------------ name tables */

/* ONE table for both classes: bit 1 = may start a name, bit 2 = may continue.
   Two tables would drift, and a name rule that disagrees with itself accepts a
   tag the close tag then rejects. */
static const uint8_t XML_NAME[256] = {
    ['A']=3,['B']=3,['C']=3,['D']=3,['E']=3,['F']=3,['G']=3,['H']=3,['I']=3,
    ['J']=3,['K']=3,['L']=3,['M']=3,['N']=3,['O']=3,['P']=3,['Q']=3,['R']=3,
    ['S']=3,['T']=3,['U']=3,['V']=3,['W']=3,['X']=3,['Y']=3,['Z']=3,
    ['a']=3,['b']=3,['c']=3,['d']=3,['e']=3,['f']=3,['g']=3,['h']=3,['i']=3,
    ['j']=3,['k']=3,['l']=3,['m']=3,['n']=3,['o']=3,['p']=3,['q']=3,['r']=3,
    ['s']=3,['t']=3,['u']=3,['v']=3,['w']=3,['x']=3,['y']=3,['z']=3,
    ['_']=3,[':']=3,
    ['0']=2,['1']=2,['2']=2,['3']=2,['4']=2,['5']=2,['6']=2,['7']=2,['8']=2,
    ['9']=2,['-']=2,['.']=2,
};

static int xml_name_start(unsigned char c) { return (XML_NAME[c] & 1) || c >= 0x80; }
static int xml_name_char(unsigned char c) { return XML_NAME[c] != 0 || c >= 0x80; }
static int xml_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* ------------------------------------------------------------- the scanner */

enum { XEV_OPEN, XEV_CLOSE, XEV_TEXT, XEV_CDATA, XEV_COMMENT, XEV_PI, XEV_SKIP };

/* Result of one scan step. */
enum { XS_EVENT, XS_NEED, XS_END, XS_ERR };

typedef struct {
    const char *name; size_t nlen;      /* OPEN/CLOSE/PI target */
    const char *val;  size_t vlen;      /* PI data (borrowed from the input) */
    xb_t text;                          /* TEXT/CDATA/COMMENT, entities decoded */
    /* attributes: names borrowed from the input, values decoded into `abuf` */
    struct { const char *n; size_t nlen; size_t voff, vlen; } attr[XML_MAX_ATTRS];
    xb_t abuf;
    uint32_t nattr;
    uint8_t  kind, selfclose;
} xml_ev_t;

typedef struct {
    xb_t     buf;                       /* carry: the unconsumed tail */
    size_t   pos;
    uint64_t total;
    int      final, done;
    /* A token interrupted by a chunk boundary resumes where it stopped. Without
       this the scan restarts at the token's first byte on every write, which is
       quadratic in the token's length -- 27.5x on one-byte writes. */
    int      tok;                       /* 0 none, 1 text, 2 comment, 3 cdata */
    size_t   tscan;                     /* bytes of it already examined */
    int      keep_entities;             /* an unknown &foo; passes through */
    char     err[128];
    uint32_t line;
} xml_scan_t;

/* Is what we have so far a prefix of `lit`? */
static int xml_prefix(const char *p, size_t avail, const char *lit)
{
    size_t l = strlen(lit), m = avail < l ? avail : l;
    return memcmp(p, lit, m) == 0;
}

static int xml_fail(xml_scan_t *s, const char *what)
{
    if (!s->err[0])
        snprintf(s->err, sizeof s->err, "%s at byte %u", what, (unsigned)s->pos);
    return XS_ERR;
}

/* &lt; &gt; &amp; &apos; &quot; and numeric refs. Anything else is an error, or
   passes through literally under `entities: "keep"` -- never expanded, which is
   what makes an entity bomb unrepresentable rather than merely bounded. */
/* &#123; or &#x1F600;. The surrogate, NUL and >U+10FFFF cases are refused
   rather than replaced: a wire format is exact or it is wrong. */
static int xml_charref(xml_scan_t *s, const char *p, size_t n, size_t *i,
                       uint32_t *out)
{
    size_t j = *i + 2, k;
    uint32_t cp = 0;
    int hex = (j < n && (p[j] == 'x' || p[j] == 'X')), any = 0;

    for (k = hex ? j + 1 : j; k < n && p[k] != ';'; k++) {
        int d;
        if (p[k] >= '0' && p[k] <= '9') d = p[k] - '0';
        else if (hex && p[k] >= 'a' && p[k] <= 'f') d = p[k] - 'a' + 10;
        else if (hex && p[k] >= 'A' && p[k] <= 'F') d = p[k] - 'A' + 10;
        else return xml_fail(s, "bad character reference");
        cp = cp * (hex ? 16u : 10u) + (uint32_t)d;
        any = 1;
        if (cp > 0x10FFFF)
            return xml_fail(s, "character reference out of range");
    }
    if (k >= n)
        return XS_NEED;
    if (!any)
        return xml_fail(s, "empty character reference");
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return xml_fail(s, "character reference is a surrogate");
    if (cp == 0)
        return xml_fail(s, "character reference is NUL");
    *i = k + 1;
    *out = cp;
    return XS_EVENT;
}

/* The five predefined entities, or 0. They are the whole entity language. */
static char xml_predefined(const char *e, size_t len)
{
    if (len == 2 && memcmp(e, "lt", 2) == 0)   return '<';
    if (len == 2 && memcmp(e, "gt", 2) == 0)   return '>';
    if (len == 3 && memcmp(e, "amp", 3) == 0)  return '&';
    if (len == 4 && memcmp(e, "apos", 4) == 0) return '\'';
    if (len == 4 && memcmp(e, "quot", 4) == 0) return '"';
    return 0;
}

/* An entity reference. Nothing here EXPANDS: no DTD is read, so an unknown
   name cannot exist, which is what makes an entity bomb a first-reference
   error rather than a size cap. */
static int xml_entity(xml_scan_t *s, const char *p, size_t n, size_t *i, xb_t *out)
{
    size_t j = *i + 1, k;
    char c;

    if (j < n && p[j] == '#') {
        /* By pointer, not by return: XS_NEED and XS_ERR are small integers and
           a code point of 1 or 3 would be indistinguishable from them. */
        uint32_t cp;
        int r = xml_charref(s, p, n, i, &cp);
        if (r != XS_EVENT)
            return r;
        xb_putc(out, cp);
        return XS_EVENT;
    }
    for (k = j; k < n && k < j + 8 && p[k] != ';'; k++)
        ;
    if (k >= n || k >= j + 8) {
        if (k >= n && !s->final)
            return XS_NEED;
        if (s->keep_entities) {
            xb_put(out, '&');
            *i = *i + 1;
            return XS_EVENT;
        }
        return xml_fail(s, "unterminated entity");
    }
    c = xml_predefined(p + j, k - j);
    if (c) {
        xb_put(out, (uint8_t)c);
        *i = k + 1;
        return XS_EVENT;
    }
    if (s->keep_entities) {
        xb_write(out, p + *i, k + 1 - *i);
        *i = k + 1;
        return XS_EVENT;
    }
    return xml_fail(s, "unknown entity (no DTD is processed, so none can exist)");
}

/* A quoted attribute value, decoded into `out`. */
static int xml_attr_value(xml_scan_t *s, const char *p, size_t n, size_t *i,
                          xb_t *out)
{
    char quote = p[*i];
    size_t j = *i + 1;
    if (quote != '"' && quote != '\'')
        return xml_fail(s, "an attribute value must be quoted");
    while (j < n) {
        if (p[j] == quote) {
            *i = j + 1;
            return XS_EVENT;
        }
        if (p[j] == '<')
            return xml_fail(s, "< in an attribute value");
        if (p[j] == '&') {
            int r = xml_entity(s, p, n, &j, out);
            if (r != XS_EVENT)
                return r;
            continue;
        }
        /* XML normalises a literal tab/newline in an attribute value to space. */
        xb_put(out, (uint8_t)(xml_space((unsigned char)p[j]) ? ' ' : p[j]));
        j++;
        if (out->n > XML_MAX_TOKEN)
            return xml_fail(s, "attribute value exceeds the token limit");
    }
    return XS_NEED;
}

/* One `name="value"` pair. XS_END means the attribute list is finished. */
static int xml_scan_attr(xml_scan_t *s, const char *p, size_t n, size_t *j,
                         xml_ev_t *ev)
{
    size_t k = *j, an, av;
    int r;

    while (k < n && xml_space((unsigned char)p[k])) k++;
    if (k >= n)
        return XS_NEED;
    if (p[k] == '>' || p[k] == '/') { *j = k; return XS_END; }
    if (!xml_name_start((unsigned char)p[k]))
        return xml_fail(s, "expected an attribute name");
    if (ev->nattr >= XML_MAX_ATTRS)
        return xml_fail(s, "too many attributes on one element");
    an = k;
    while (k < n && xml_name_char((unsigned char)p[k])) k++;
    if (k >= n)
        return XS_NEED;
    ev->attr[ev->nattr].n = p + an;
    ev->attr[ev->nattr].nlen = k - an;
    while (k < n && xml_space((unsigned char)p[k])) k++;
    if (k >= n)
        return XS_NEED;
    if (p[k] != '=')
        return xml_fail(s, "an attribute needs a value");
    k++;
    while (k < n && xml_space((unsigned char)p[k])) k++;
    if (k >= n)
        return XS_NEED;
    av = ev->abuf.n;
    r = xml_attr_value(s, p, n, &k, &ev->abuf);
    if (r != XS_EVENT)
        return r;
    ev->attr[ev->nattr].voff = av;
    ev->attr[ev->nattr].vlen = ev->abuf.n - av;
    ev->nattr++;
    *j = k;
    return XS_EVENT;
}

static int xml_scan_open(xml_scan_t *s, const char *p, size_t n, size_t *i,
                         xml_ev_t *ev)
{
    size_t j = *i, st;

    if (!xml_name_start((unsigned char)p[j]))
        return xml_fail(s, "expected an element name");
    st = j;
    while (j < n && xml_name_char((unsigned char)p[j])) j++;
    if (j >= n)
        return XS_NEED;
    ev->name = p + st;
    ev->nlen = j - st;
    ev->nattr = 0;
    ev->selfclose = 0;
    ev->abuf.n = 0;
    for (;;) {
        int r = xml_scan_attr(s, p, n, &j, ev);
        if (r == XS_EVENT)
            continue;
        if (r != XS_END)
            return r;
        break;
    }
    if (p[j] == '>') { *i = j + 1; return XS_EVENT; }
    if (j + 1 >= n)
        return XS_NEED;
    if (p[j + 1] != '>')
        return xml_fail(s, "expected /> to close an empty element");
    ev->selfclose = 1;
    *i = j + 2;
    return XS_EVENT;
}

/* Scan one token out of the carry. XS_NEED means "more input, same position". */
/* A text run, up to the next '<'. Entities decode on the way. */
/* A clean TEXT run ends at '<', '&' or ']' (the ']]>' guard). */
static const uint8_t XML_TXT_SET[3] = { '<', '&', ']' };
#define XML_BULK_ON  32   /* consecutive clean bytes before vectorising */
#define XML_BULK_OFF 16   /* a vector run shorter than this: stop paying */

static int xml_scan_text(xml_scan_t *s, const char *p, size_t n, size_t *i,
                         xml_ev_t *ev)
{
    size_t k = *i + s->tscan;
    size_t clean = 0;
    int bulk = 0;

    ev->kind = XEV_TEXT;
    s->tok = 1;
    while (k < n && p[k] != '<') {
        /* ADAPTIVE: enter the vector path only after 32 consecutive clean bytes
           have been SEEN, and leave it when a run comes back short. Predicting
           the run from the input failed -- a 3-byte sample of `ab&amp;` lands on
           ordinary letters and measured 0.32x. History is the honest predictor. */
        if (bulk) {
            size_t run = k;
            size_t t = simd.find_first_of((const uint8_t *)p + k, n - k,
                                          XML_TXT_SET, sizeof XML_TXT_SET);
            k = (t == (size_t)-1) ? n : k + t;
            if (k > run) {
                if (ev->text.n + (k - run) > XML_MAX_TOKEN)
                    return xml_fail(s, "text run exceeds the token limit");
                xb_write(&ev->text, p + run, k - run);
            }
            if (k - run < XML_BULK_OFF)
                bulk = 0;
            clean = 0;
            continue;
        }
        if (p[k] == '&') {
            clean = 0;
            size_t at = k;
            int r = xml_entity(s, p, n, &k, &ev->text);
            if (r == XS_NEED) { s->tscan = at - *i; return XS_NEED; }
            if (r != XS_EVENT)
                return r;
            continue;
        }
        if (p[k] == ']' && k + 2 < n && p[k + 1] == ']' && p[k + 2] == '>')
            return xml_fail(s, "]]> in text must be escaped");
        xb_put(&ev->text, (uint8_t)p[k]);
        k++;
        if (++clean >= XML_BULK_ON)
            bulk = 1;
        if (ev->text.n > XML_MAX_TOKEN)
            return xml_fail(s, "text run exceeds the token limit");
    }
    if (k >= n && !s->final) { s->tscan = k - *i; return XS_NEED; }
    *i = k;
    s->tok = 0;
    s->tscan = 0;
    return ev->text.oom ? xml_fail(s, "out of memory") : XS_EVENT;
}

static int xml_scan_close(xml_scan_t *s, const char *p, size_t n, size_t *i,
                          xml_ev_t *ev)
{
    size_t j = *i + 2, st;

    if (j >= n)
        return XS_NEED;
    if (!xml_name_start((unsigned char)p[j]))
        return xml_fail(s, "expected a close-tag name");
    st = j;
    while (j < n && xml_name_char((unsigned char)p[j])) j++;
    while (j < n && xml_space((unsigned char)p[j])) j++;
    if (j >= n)
        return XS_NEED;
    if (p[j] != '>')
        return xml_fail(s, "expected > to close a close tag");
    ev->kind = XEV_CLOSE;
    ev->name = p + st;
    ev->nlen = j - st;
    while (ev->nlen && xml_space((unsigned char)ev->name[ev->nlen - 1]))
        ev->nlen--;
    *i = j + 1;
    return XS_EVENT;
}

/* A run to a fixed terminator: `-->` or `]]>`. Both resume mid-search. */
static int xml_scan_until(xml_scan_t *s, const char *p, size_t n, size_t *i,
                          xml_ev_t *ev, size_t open_len, const char *term,
                          int kind, int tok)
{
    size_t st = *i + open_len, j = st + s->tscan;

    s->tok = tok;
    while (j + 2 < n && !(p[j] == term[0] && p[j + 1] == term[1]
                          && p[j + 2] == term[2]))
        j++;
    if (j + 2 >= n) { s->tscan = j - st; return XS_NEED; }
    ev->kind = (uint8_t)kind;
    ev->text.n = 0;
    xb_write(&ev->text, p + st, j - st);
    *i = j + 3;
    s->tok = 0;
    s->tscan = 0;
    return XS_EVENT;
}

/* `<!...`: a comment, CDATA, or a DOCTYPE that is skipped and never read. */
static int xml_scan_bang(xml_scan_t *s, const char *p, size_t n, size_t *i,
                         xml_ev_t *ev)
{
    size_t j;
    int depth = 0;

    if (n - *i >= 4 && memcmp(p + *i, "<!--", 4) == 0)
        return xml_scan_until(s, p, n, i, ev, 4, "-->", XEV_COMMENT, 2);
    if (n - *i >= 9 && memcmp(p + *i, "<![CDATA[", 9) == 0)
        return xml_scan_until(s, p, n, i, ev, 9, "]]>", XEV_CDATA, 3);
    if (n - *i >= 9 && memcmp(p + *i, "<!DOCTYPE", 9) == 0) {
        for (j = *i + 9; j < n; j++) {
            if (p[j] == '[') depth++;
            else if (p[j] == ']') depth--;
            else if (p[j] == '>' && depth <= 0) break;
        }
        if (j >= n)
            return XS_NEED;
        *i = j + 1;
        ev->kind = XEV_SKIP;            /* consumed, reported to nobody */
        return XS_EVENT;
    }
    /* A chunk boundary inside `<!--` or `<![CDATA[` leaves too few bytes to
       tell which declaration this is; that is NEED, not a syntax error. */
    if (n - *i < 9 && !s->final
        && (xml_prefix(p + *i, n - *i, "<!--")
            || xml_prefix(p + *i, n - *i, "<![CDATA[")
            || xml_prefix(p + *i, n - *i, "<!DOCTYPE")))
        return XS_NEED;
    return xml_fail(s, "unsupported <! declaration");
}

static int xml_scan_pi(xml_scan_t *s, const char *p, size_t n, size_t *i,
                       xml_ev_t *ev)
{
    size_t j = *i + 2, st = j, te;

    if (j >= n)
        return XS_NEED;
    if (!xml_name_start((unsigned char)p[j]))
        return xml_fail(s, "expected a processing-instruction target");
    while (j < n && xml_name_char((unsigned char)p[j])) j++;
    te = j;
    while (j + 1 < n && !(p[j] == '?' && p[j + 1] == '>')) j++;
    if (j + 1 >= n)
        return XS_NEED;
    ev->kind = XEV_PI;
    ev->name = p + st;
    ev->nlen = te - st;
    ev->val = p + te;
    ev->vlen = j - te;
    while (ev->vlen && xml_space((unsigned char)ev->val[0])) { ev->val++; ev->vlen--; }
    *i = j + 2;
    return XS_EVENT;
}

/* Scan one token out of the carry. XS_NEED means "more input, same position". */
static int xml_step(xml_scan_t *s, xml_ev_t *ev)
{
    const char *p = (const char *)s->buf.p;
    size_t n = s->buf.n, i = s->pos;
    int r;

    if (i >= n)
        return s->final ? XS_END : XS_NEED;
    if (!s->tok)
        ev->text.n = 0;
    if (p[i] != '<') {
        r = xml_scan_text(s, p, n, &i, ev);
    } else if (i + 1 >= n) {
        return XS_NEED;
    } else if (p[i + 1] == '/') {
        r = xml_scan_close(s, p, n, &i, ev);
    } else if (p[i + 1] == '!') {
        r = xml_scan_bang(s, p, n, &i, ev);
    } else if (p[i + 1] == '?') {
        r = xml_scan_pi(s, p, n, &i, ev);
    } else {
        size_t j = i + 1;
        ev->kind = XEV_OPEN;
        r = xml_scan_open(s, p, n, &j, ev);
        i = j;
    }
    if (r == XS_EVENT)
        s->pos = i;
    return r;
}

/* Drop what has been consumed. Sliding after EVERY token is O(n^2): measured
   4 MiB/s on a 0.4 MiB feed against 38 MiB/s in 64 KiB chunks, which is one
   memmove of the whole tail paid 20k times. Slide once half is consumed, so
   each byte is moved O(1) times amortised. */
static void xml_compact(xml_scan_t *s)
{
    if (s->pos == 0 || s->pos * 2 < s->buf.n)
        return;
    memmove(s->buf.p, s->buf.p + s->pos, s->buf.n - s->pos);
    s->buf.n -= s->pos;
    s->pos = 0;
}

/* -------------------------------------------------------------- tree build */

typedef struct {
    JSContext *ctx;
    JSValue    stack[XML_MAX_DEPTH];    /* the children array of each open node */
    JSValue    names[XML_MAX_DEPTH];
    uint32_t   counts[XML_MAX_DEPTH];
    int        depth, nroots;
    int        trim;                    /* drop whitespace-only text nodes */
} xml_tree_t;

static JSValue xml_new_element(JSContext *ctx, const xml_ev_t *ev, JSValue *children)
{
    JSValue el = JS_NewObject(ctx), attrs;
    uint32_t k;
    if (JS_IsException(el))
        return el;
    attrs = JS_NewObject(ctx);
    if (JS_IsException(attrs)) {
        JS_FreeValue(ctx, el);
        return JS_EXCEPTION;
    }
    for (k = 0; k < ev->nattr; k++) {
        JSAtom a = JS_NewAtomLen(ctx, ev->attr[k].n, ev->attr[k].nlen);
        JSValue v;
        if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, attrs); JS_FreeValue(ctx, el); return JS_EXCEPTION; }
        v = JS_NewStringLen(ctx, (const char *)ev->abuf.p + ev->attr[k].voff,
                            ev->attr[k].vlen);
        /* DEFINE, not set: a document containing an attribute called __proto__
           must produce an own property, never retarget a prototype. */
        if (JS_IsException(v) || JS_DefinePropertyValue(ctx, attrs, a, v,
                                                        JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, a);
            JS_FreeValue(ctx, attrs);
            JS_FreeValue(ctx, el);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(ctx, a);
    }
    *children = JS_NewArray(ctx);
    if (JS_IsException(*children)) {
        JS_FreeValue(ctx, attrs);
        JS_FreeValue(ctx, el);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(ctx, el, "name",
                          JS_NewStringLen(ctx, ev->name, ev->nlen)) < 0
        || JS_SetPropertyStr(ctx, el, "attrs", attrs) < 0
        || JS_SetPropertyStr(ctx, el, "children", JS_DupValue(ctx, *children)) < 0) {
        JS_FreeValue(ctx, *children);
        JS_FreeValue(ctx, el);
        return JS_EXCEPTION;
    }
    return el;
}

static int xml_all_space(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!xml_space(p[i]))
            return 0;
    return 1;
}

/* ------------------------------------------------------------- the parser */

typedef struct {
    xml_scan_t scan;
    xml_ev_t   ev;
} xml_parse_t;

static void xml_parse_free(xml_parse_t *p)
{
    xb_free(&p->scan.buf);
    xb_free(&p->ev.text);
    xb_free(&p->ev.abuf);
}

/* Options shared by the parse entry points. */
typedef struct { int trim, keep_entities; } xml_opts_t;

static int xml_read_opts(JSContext *ctx, JSValueConst o, xml_opts_t *out)
{
    JSValue v;
    out->trim = 1;
    out->keep_entities = 0;
    if (JS_IsUndefined(o) || JS_IsNull(o))
        return 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
    v = JS_GetPropertyStr(ctx, o, "trim");
    if (JS_IsException(v)) return -1;
    if (!JS_IsUndefined(v)) out->trim = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "entities");
    if (JS_IsException(v)) return -1;
    if (!JS_IsUndefined(v)) {
        const char *s = JS_ToCString(ctx, v);
        int keep;
        if (!s) { JS_FreeValue(ctx, v); return -1; }
        keep = strcmp(s, "keep") == 0;
        if (!keep && strcmp(s, "strict") != 0) {
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "entities must be \"strict\" or \"keep\"");
            return -1;
        }
        JS_FreeCString(ctx, s);
        out->keep_entities = keep;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

/* Each of these returns 0, -1 with `err` filled (a malformed document), or -3
   (a JS exception is already pending). */
static int xml_tree_open(JSContext *ctx, xml_tree_t *t, const xml_ev_t *ev,
                         char *err, size_t errsz)
{
    JSValue kids, el;

    if (t->depth + 1 >= XML_MAX_DEPTH) {
        snprintf(err, errsz, "nesting exceeds %d elements", XML_MAX_DEPTH);
        return -1;
    }
    el = xml_new_element(ctx, ev, &kids);
    if (JS_IsException(el))
        return -3;
    if (t->depth == 0 && ++t->nroots > 1) {
        JS_FreeValue(ctx, kids);
        JS_FreeValue(ctx, el);
        snprintf(err, errsz, "a document has one root element");
        return -1;
    }
    if (JS_DefinePropertyValueUint32(ctx, t->stack[t->depth],
                                     t->counts[t->depth]++, el,
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, kids);
        return -3;
    }
    if (ev->selfclose) {
        JS_FreeValue(ctx, kids);
        return 0;
    }
    t->depth++;
    t->stack[t->depth] = kids;
    t->names[t->depth] = JS_NewStringLen(ctx, ev->name, ev->nlen);
    t->counts[t->depth] = 0;
    return 0;
}

static int xml_tree_close(JSContext *ctx, xml_tree_t *t, const xml_ev_t *ev,
                          char *err, size_t errsz)
{
    const char *want;
    size_t wlen;
    int rc = 0;

    if (t->depth == 0) {
        snprintf(err, errsz, "close tag with no open tag");
        return -1;
    }
    want = JS_ToCStringLen(ctx, &wlen, t->names[t->depth]);
    if (!want)
        return -3;
    if (wlen != ev->nlen || memcmp(want, ev->name, wlen) != 0) {
        snprintf(err, errsz, "</%.*s> closes <%.*s>", (int)ev->nlen, ev->name,
                 (int)wlen, want);
        rc = -1;
    }
    JS_FreeCString(ctx, want);
    if (rc)
        return rc;
    JS_FreeValue(ctx, t->stack[t->depth]);
    JS_FreeValue(ctx, t->names[t->depth]);
    t->depth--;
    return 0;
}

static int xml_tree_text(JSContext *ctx, xml_tree_t *t, const xml_ev_t *ev,
                         char *err, size_t errsz)
{
    JSValue sv;
    int blank;

    if (ev->text.n == 0)
        return 0;
    blank = ev->kind == XEV_TEXT && xml_all_space(ev->text.p, ev->text.n);
    if (blank && (t->trim || t->depth == 0))
        return 0;
    if (t->depth == 0) {
        snprintf(err, errsz, "text outside the root element");
        return -1;
    }
    sv = JS_NewStringLen(ctx, (const char *)ev->text.p, ev->text.n);
    if (JS_IsException(sv)
        || JS_DefinePropertyValueUint32(ctx, t->stack[t->depth],
                                        t->counts[t->depth]++, sv,
                                        JS_PROP_C_W_E) < 0)
        return -3;
    return 0;
}

static JSValue xml_parse(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    const char *src;
    size_t slen;
    xml_parse_t p;
    xml_tree_t t;
    JSValue root_children, result;
    xml_opts_t opts;
    int rc = 0;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "XMLParse(text, options): text must be a string");
    if (xml_read_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &opts) < 0)
        return JS_EXCEPTION;
    src = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    if (slen > XML_MAX_INPUT) {
        JS_FreeCString(ctx, src);
        return JS_ThrowRangeError(ctx, "XMLParse: input exceeds %u bytes", XML_MAX_INPUT);
    }
    memset(&p, 0, sizeof p);
    xb_init(&p.scan.buf); xb_init(&p.ev.text); xb_init(&p.ev.abuf);
    p.scan.final = 1;
    p.scan.keep_entities = opts.keep_entities;
    xb_write(&p.scan.buf, src, slen);
    JS_FreeCString(ctx, src);
    if (p.scan.buf.oom) {
        xml_parse_free(&p);
        return JS_ThrowOutOfMemory(ctx);
    }
    memset(&t, 0, sizeof t);
    t.ctx = ctx;
    t.trim = opts.trim;
    root_children = JS_NewArray(ctx);
    if (JS_IsException(root_children)) {
        xml_parse_free(&p);
        return JS_EXCEPTION;
    }
    t.stack[0] = root_children;

    for (;;) {
        int r = xml_step(&p.scan, &p.ev);
        if (r == XS_END)
            break;
        if (r == XS_NEED) { rc = -2; break; }
        if (r == XS_ERR)  { rc = -1; break; }
        switch (p.ev.kind) {
        case XEV_OPEN:
            rc = xml_tree_open(ctx, &t, &p.ev, p.scan.err, sizeof p.scan.err);
            break;
        case XEV_CLOSE:
            rc = xml_tree_close(ctx, &t, &p.ev, p.scan.err, sizeof p.scan.err);
            break;
        case XEV_TEXT:
        case XEV_CDATA:
            rc = xml_tree_text(ctx, &t, &p.ev, p.scan.err, sizeof p.scan.err);
            break;
        default:
            break;                      /* comments, PIs and a skipped DOCTYPE */
        }
        if (rc)
            break;
        xml_compact(&p.scan);
    }

    if (rc == 0 && t.depth != 0) {
        snprintf(p.scan.err, sizeof p.scan.err, "unclosed element");
        rc = -1;
    }
    if (rc == 0 && t.nroots == 0) {
        snprintf(p.scan.err, sizeof p.scan.err, "no root element");
        rc = -1;
    }
    if (rc == -2)
        snprintf(p.scan.err, sizeof p.scan.err, "unexpected end of document");
    while (t.depth > 0) {
        JS_FreeValue(ctx, t.stack[t.depth]);
        JS_FreeValue(ctx, t.names[t.depth]);
        t.depth--;
    }
    if (rc == 0)
        result = JS_GetPropertyUint32(ctx, root_children, 0);
    else if (rc == -3)
        result = JS_EXCEPTION;
    else
        result = JS_ThrowSyntaxError(ctx, "XMLParse: %s", p.scan.err);
    JS_FreeValue(ctx, root_children);
    xml_parse_free(&p);
    return result;
}

/* -------------------------------------------------------------- serializer */

/* Bytes that must be escaped in text and in an attribute value. ONE table, so
   the writer and the reader cannot disagree about what needs quoting. */
static const uint8_t XML_ESC[256] = {
    ['<']=1, ['>']=1, ['&']=1, ['"']=1,
};
static const uint8_t XML_ESC_SET[4] = { '<', '>', '&', '"' };
/* Below this the indirect call outweighs the scan (the CSV-field lesson). */
#define XML_SIMD_MIN 64

static void xml_escape(xb_t *b, const char *s, size_t n, int in_attr)
{
    size_t i = 0, run;
    while (i < n) {
        run = i;
        /* Scalar probe FIRST. The gate cannot be on the remaining input: with
           an escape every few bytes the run is short however much input is
           left, and paying the indirect call per short run measured 0.73x. */
        { size_t probe = i + XML_SIMD_MIN;
          if (probe > n) probe = n;
          while (i < probe && !XML_ESC[(unsigned char)s[i]]) i++;
          if (i == probe && n - i >= XML_SIMD_MIN) {
              size_t k = simd.find_first_of((const uint8_t *)s + i, n - i,
                                            XML_ESC_SET, sizeof XML_ESC_SET);
              i = (k == (size_t)-1) ? n : i + k;
          } }
        xb_write(b, s + run, i - run);   /* the clean span, in one copy */
        if (i >= n)
            break;
        switch (s[i]) {
        case '<': xb_puts(b, "&lt;"); break;
        case '>': xb_puts(b, "&gt;"); break;
        case '&': xb_puts(b, "&amp;"); break;
        default:  xb_puts(b, in_attr ? "&quot;" : "\""); break;
        }
        i++;
    }
}

typedef struct {
    JSContext *ctx;
    xb_t       out;
    int        indent, depth, err;
} xml_ser_t;

static int xml_ser_node(xml_ser_t *w, JSValueConst node);

static void xml_ser_nl(xml_ser_t *w)
{
    int k;
    if (!w->indent)
        return;
    xb_put(&w->out, '\n');
    for (k = 0; k < w->depth * w->indent; k++)
        xb_put(&w->out, ' ');
}

static int xml_ser_attrs(xml_ser_t *w, JSValueConst el)
{
    JSValue attrs = JS_GetPropertyStr(w->ctx, el, "attrs");
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, k;
    int rc = 0;

    if (JS_IsException(attrs))
        return -1;
    if (!JS_IsObject(attrs)) {
        JS_FreeValue(w->ctx, attrs);
        return 0;
    }
    if (JS_GetOwnPropertyNames(w->ctx, &tab, &len, attrs,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(w->ctx, attrs);
        return -1;
    }
    for (k = 0; k < len && rc == 0; k++) {
        const char *an, *av;
        size_t anl, avl;
        JSValue v = JS_GetProperty(w->ctx, attrs, tab[k].atom);
        if (JS_IsException(v)) { rc = -1; break; }
        an = JS_AtomToCStringLen(w->ctx, &anl, tab[k].atom);
        av = JS_ToCStringLen(w->ctx, &avl, v);
        JS_FreeValue(w->ctx, v);
        if (!an || !av) {
            if (an) JS_FreeCString(w->ctx, an);
            if (av) JS_FreeCString(w->ctx, av);
            rc = -1;
            break;
        }
        xb_put(&w->out, ' ');
        xb_write(&w->out, an, anl);
        xb_puts(&w->out, "=\"");
        xml_escape(&w->out, av, avl, 1);
        xb_put(&w->out, '"');
        JS_FreeCString(w->ctx, an);
        JS_FreeCString(w->ctx, av);
    }
    JS_FreePropertyEnum(w->ctx, tab, len);
    JS_FreeValue(w->ctx, attrs);
    return rc;
}

static int xml_ser_children(xml_ser_t *w, JSValueConst el, int *any_element)
{
    JSValue kids = JS_GetPropertyStr(w->ctx, el, "children");
    int64_t len = 0, i;
    int rc = 0;

    *any_element = 0;
    if (JS_IsException(kids))
        return -1;
    if (JS_IsArray(w->ctx, kids) != 1) {
        JS_FreeValue(w->ctx, kids);
        return 0;
    }
    {
        JSValue lv = JS_GetPropertyStr(w->ctx, kids, "length");
        if (JS_IsException(lv) || JS_ToInt64(w->ctx, &len, lv) < 0) {
            JS_FreeValue(w->ctx, lv);
            JS_FreeValue(w->ctx, kids);
            return -1;
        }
        JS_FreeValue(w->ctx, lv);
    }
    for (i = 0; i < len && rc == 0; i++) {
        JSValue c = JS_GetPropertyUint32(w->ctx, kids, (uint32_t)i);
        if (JS_IsException(c)) { rc = -1; break; }
        if (JS_IsObject(c)) {
            *any_element = 1;
            xml_ser_nl(w);
        }
        rc = xml_ser_node(w, c);
        JS_FreeValue(w->ctx, c);
    }
    JS_FreeValue(w->ctx, kids);
    return rc;
}

static int xml_ser_node(xml_ser_t *w, JSValueConst node)
{
    JSValue nv;
    const char *nm;
    size_t nml;
    int any = 0, rc = 0;

    if (w->depth >= XML_MAX_DEPTH) {
        JS_ThrowRangeError(w->ctx, "XMLStringify: nesting exceeds %d", XML_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(node)) {
        const char *s;
        size_t n;
        if (JS_IsUndefined(node) || JS_IsNull(node))
            return 0;
        s = JS_ToCStringLen(w->ctx, &n, node);
        if (!s)
            return -1;
        xml_escape(&w->out, s, n, 0);
        JS_FreeCString(w->ctx, s);
        return 0;
    }
    nv = JS_GetPropertyStr(w->ctx, node, "name");
    if (JS_IsException(nv))
        return -1;
    if (!JS_IsString(nv)) {
        JS_FreeValue(w->ctx, nv);
        JS_ThrowTypeError(w->ctx, "XMLStringify: a node needs a string `name`");
        return -1;
    }
    nm = JS_ToCStringLen(w->ctx, &nml, nv);
    JS_FreeValue(w->ctx, nv);
    if (!nm)
        return -1;
    if (nml == 0 || !xml_name_start((unsigned char)nm[0])) {
        JS_FreeCString(w->ctx, nm);
        JS_ThrowRangeError(w->ctx, "XMLStringify: not a valid element name");
        return -1;
    }
    xb_put(&w->out, '<');
    xb_write(&w->out, nm, nml);
    rc = xml_ser_attrs(w, node);
    if (rc == 0) {
        size_t before = w->out.n;
        xb_puts(&w->out, ">");
        w->depth++;
        rc = xml_ser_children(w, node, &any);
        w->depth--;
        if (rc == 0 && w->out.n == before + 1) {
            w->out.n = before;          /* nothing inside: the self-closing form */
            xb_puts(&w->out, "/>");
        } else if (rc == 0) {
            if (any) xml_ser_nl(w);
            xb_puts(&w->out, "</");
            xb_write(&w->out, nm, nml);
            xb_put(&w->out, '>');
        }
    }
    JS_FreeCString(w->ctx, nm);
    return rc;
}

static JSValue xml_stringify(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    xml_ser_t w;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "XMLStringify(node, options): a node is required");
    memset(&w, 0, sizeof w);
    w.ctx = ctx;
    xb_init(&w.out);
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "indent");
        int32_t ind = 0;
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &ind, v) < 0) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        if (ind < 0 || ind > 16)
            return JS_ThrowRangeError(ctx, "XMLStringify: indent is 0 to 16");
        w.indent = ind;
    }
    if (xml_ser_node(&w, argv[0]) < 0) {
        xb_free(&w.out);
        return JS_EXCEPTION;
    }
    if (w.out.oom) {
        xb_free(&w.out);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = JS_NewStringLen(ctx, (const char *)w.out.p, w.out.n);
    xb_free(&w.out);
    return out;
}

/* --------------------------------------------------------------- SAXParser */

static JSClassID dyn_sax_class_id;

typedef struct {
    xml_scan_t scan;
    xml_ev_t   ev;
    JSRuntime *rt;                      /* the dispose path has no context */
    JSValue    on[6];
    int        ended, busy;
} dyn_sax_t;

static void dyn_sax_free(JSRuntime *rt, dyn_sax_t *s)
{
    int k;
    if (!s)
        return;
    for (k = 0; k < 6; k++)
        JS_FreeValueRT(rt, s->on[k]);
    xb_free(&s->scan.buf);
    xb_free(&s->ev.text);
    xb_free(&s->ev.abuf);
    free(s);
}

static void dyn_sax_dispose(void *p)
{
    dyn_sax_t *s = (dyn_sax_t *)p;
    if (s)
        dyn_sax_free(s->rt, s);
}

static void dyn_sax_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_sax_free(rt, (dyn_sax_t *)JS_GetOpaque(val, dyn_sax_class_id));
}

/* The handlers are the cycle: a closure registered here can capture the parser.
   Nothing else this struct holds is a JS value, so this is the whole mark set. */
static void dyn_sax_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    dyn_sax_t *s = (dyn_sax_t *)JS_GetOpaque(val, dyn_sax_class_id);
    int k;
    if (!s)
        return;
    for (k = 0; k < 6; k++)
        JS_MarkValue(rt, s->on[k], mark);
}

static const JSClassDef dyn_sax_class = {
    "SAXParser", .finalizer = dyn_sax_finalizer, .gc_mark = dyn_sax_gc_mark,
};

static const char *const DYN_SAX_NAMES[6] = {
    "onOpen", "onClose", "onText", "onCData", "onComment", "onPI"
};

static JSValue dyn_sax_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    dyn_sax_t *s;
    xml_opts_t opts;
    int k;

    (void)new_target;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "new SAXParser(handlers): handlers must be an object");
    if (xml_read_opts(ctx, argv[0], &opts) < 0)
        return JS_EXCEPTION;
    s = (dyn_sax_t *)calloc(1, sizeof *s);
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->rt = JS_GetRuntime(ctx);
    for (k = 0; k < 6; k++)
        s->on[k] = JS_UNDEFINED;
    xb_init(&s->scan.buf); xb_init(&s->ev.text); xb_init(&s->ev.abuf);
    s->scan.keep_entities = opts.keep_entities;
    for (k = 0; k < 6; k++) {
        JSValue h = JS_GetPropertyStr(ctx, argv[0], DYN_SAX_NAMES[k]);
        if (JS_IsException(h)) { dyn_sax_free(JS_GetRuntime(ctx), s); return JS_EXCEPTION; }
        if (JS_IsUndefined(h) || JS_IsNull(h)) {
            JS_FreeValue(ctx, h);
            continue;
        }
        if (!JS_IsFunction(ctx, h)) {
            JS_FreeValue(ctx, h);
            dyn_sax_free(JS_GetRuntime(ctx), s);
            return JS_ThrowTypeError(ctx, "new SAXParser: %s must be a function",
                                     DYN_SAX_NAMES[k]);
        }
        s->on[k] = h;
    }
    return dyn_plain_wrap(ctx, dyn_sax_class_id, s, dyn_sax_dispose);
}

/* A byte view, an ArrayBuffer or nothing. The view is tried first because that
   is what callers pass; JS_GetArrayBuffer throws for a typed array. */
static int dyn_xml_bytes(JSContext *ctx, JSValueConst v, const uint8_t **pp,
                         size_t *pn)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base)
            return -1;
        *pp = base;
        *pn = ab;
        return 0;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "chunk must be a string or a byte view");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

/* One event to its handler. A handler that throws stops the parse. */
/* The attribute object handed to onOpen. Define, not set, so an attribute
   called __proto__ is an own property. */
static JSValue dyn_sax_attrs(JSContext *ctx, const xml_ev_t *ev)
{
    JSValue attrs = JS_NewObject(ctx);
    uint32_t k;

    if (JS_IsException(attrs))
        return attrs;
    for (k = 0; k < ev->nattr; k++) {
        JSAtom a = JS_NewAtomLen(ctx, ev->attr[k].n, ev->attr[k].nlen);
        JSValue v;
        if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, attrs); return JS_EXCEPTION; }
        v = JS_NewStringLen(ctx, (const char *)ev->abuf.p + ev->attr[k].voff,
                            ev->attr[k].vlen);
        if (JS_IsException(v)
            || JS_DefinePropertyValue(ctx, attrs, a, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, a);
            JS_FreeValue(ctx, attrs);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(ctx, a);
    }
    return attrs;
}

/* One event to its handler. A handler that throws stops the parse. */
static int dyn_sax_emit(JSContext *ctx, dyn_sax_t *s, JSValueConst this_val)
{
    JSValue args[2], r;
    int nargs = 0, slot;
    const xml_ev_t *ev = &s->ev;

    switch (ev->kind) {
    case XEV_OPEN:    slot = 0; break;
    case XEV_CLOSE:   slot = 1; break;
    case XEV_TEXT:    slot = 2; break;
    case XEV_CDATA:   slot = 3; break;
    case XEV_COMMENT: slot = 4; break;
    case XEV_PI:      slot = 5; break;
    default:          return 0;         /* XEV_SKIP: a consumed DOCTYPE */
    }
    if (JS_IsUndefined(s->on[slot]))
        return 0;
    if (ev->kind == XEV_OPEN) {
        args[1] = dyn_sax_attrs(ctx, ev);
        if (JS_IsException(args[1]))
            return -1;
        args[0] = JS_NewStringLen(ctx, ev->name, ev->nlen);
        nargs = 2;
    } else if (ev->kind == XEV_PI) {
        args[0] = JS_NewStringLen(ctx, ev->name, ev->nlen);
        args[1] = JS_NewStringLen(ctx, ev->val, ev->vlen);
        nargs = 2;
    } else if (ev->kind == XEV_CLOSE) {
        args[0] = JS_NewStringLen(ctx, ev->name, ev->nlen);
        nargs = 1;
    } else {
        args[0] = JS_NewStringLen(ctx, (const char *)ev->text.p, ev->text.n);
        nargs = 1;
    }
    r = JS_Call(ctx, s->on[slot], this_val, nargs, (JSValueConst *)args);
    while (nargs--)
        JS_FreeValue(ctx, args[nargs]);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

/* magic 0 = write(chunk), 1 = end() */
/* Append one chunk to the carry. A string or any byte view. */
static int dyn_sax_append(JSContext *ctx, dyn_sax_t *s, JSValueConst chunk)
{
    const uint8_t *p = NULL;
    const char *str = NULL;
    size_t n = 0;

    if (JS_IsString(chunk)) {
        str = JS_ToCStringLen(ctx, &n, chunk);
        if (!str)
            return -1;
        p = (const uint8_t *)str;
    } else if (dyn_xml_bytes(ctx, chunk, &p, &n) < 0) {
        return -1;
    }
    xb_write(&s->scan.buf, p, n);
    if (str)
        JS_FreeCString(ctx, str);
    if (s->scan.buf.oom) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    s->scan.total += n;
    return 0;
}

/* magic 0 = write(chunk), 1 = end() */
static JSValue dyn_sax_feed(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    dyn_sax_t *s = (dyn_sax_t *)dyn_plain_get(ctx, this_val, dyn_sax_class_id);
    int rc = 0;

    if (!s)
        return JS_EXCEPTION;
    /* A handler can call write() again; the carry buffer is not reentrant. */
    if (s->busy)
        return JS_ThrowTypeError(ctx, "SAXParser: write() called from a handler");
    if (s->ended)
        return JS_ThrowTypeError(ctx, "SAXParser: end() has already been called");
    if (magic == 0) {
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "SAXParser.write(chunk): a chunk is required");
        if (dyn_sax_append(ctx, s, argv[0]) < 0)
            return JS_EXCEPTION;
    } else {
        s->scan.final = 1;
    }
    s->busy = 1;
    for (;;) {
        int r = xml_step(&s->scan, &s->ev);
        if (r == XS_NEED || r == XS_END)
            break;
        if (r == XS_ERR) { rc = -1; break; }
        if (dyn_sax_emit(ctx, s, this_val) < 0) { rc = -2; break; }
        xml_compact(&s->scan);
    }
    s->busy = 0;
    if (magic == 1) {
        s->ended = 1;
        if (rc == 0 && s->scan.pos < s->scan.buf.n) {
            snprintf(s->scan.err, sizeof s->scan.err, "unexpected end of document");
            rc = -1;
        }
    }
    if (rc == -2)
        return JS_EXCEPTION;
    if (rc == -1)
        return JS_ThrowSyntaxError(ctx, "SAXParser: %s", s->scan.err);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry dyn_sax_proto[] = {
    JS_CFUNC_MAGIC_DEF("write", 1, dyn_sax_feed, 0),
    JS_CFUNC_MAGIC_DEF("end", 0, dyn_sax_feed, 1),
};

/* ------------------------------------------------------------- toObject */

/* An OWN property, or 0. JS_GetProperty walks the prototype, so a document
   whose element is called __proto__ would find Object.prototype and be merged
   into an array with it. */
static int xml_own(JSContext *ctx, JSValueConst obj, JSAtom k, JSValue *out)
{
    JSPropertyDescriptor d;
    int r = JS_GetOwnProperty(ctx, &d, obj, k);
    if (r <= 0)
        return r;
    JS_FreeValue(ctx, d.getter);
    JS_FreeValue(ctx, d.setter);
    *out = d.value;
    return 1;
}

static int xml_collapse_add(JSContext *ctx, JSValue obj, JSAtom key, JSValue v)
{
    JSValue cur;
    int r = xml_own(ctx, obj, key, &cur);
    if (r < 0) { JS_FreeValue(ctx, v); return -1; }
    if (r == 0)
        return JS_DefinePropertyValue(ctx, obj, key, v, JS_PROP_C_W_E) < 0 ? -1 : 0;
    if (JS_IsArray(ctx, cur) == 1) {
        JSValue lv = JS_GetPropertyStr(ctx, cur, "length");
        int64_t len = 0;
        if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) {
            JS_FreeValue(ctx, lv); JS_FreeValue(ctx, cur); JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, lv);
        r = JS_DefinePropertyValueUint32(ctx, cur, (uint32_t)len, v, JS_PROP_C_W_E);
        JS_FreeValue(ctx, cur);
        return r < 0 ? -1 : 0;
    }
    {   /* a second child of the same name turns the slot into an array */
        JSValue arr = JS_NewArray(ctx);
        if (JS_IsException(arr)) { JS_FreeValue(ctx, cur); JS_FreeValue(ctx, v); return -1; }
        if (JS_DefinePropertyValueUint32(ctx, arr, 0, cur, JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueUint32(ctx, arr, 1, v, JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValue(ctx, obj, key, arr, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
        return 0;
    }
}

static JSValue xml_collapse(JSContext *ctx, JSValueConst el, int depth);

/* The xml2js-style collapse. LOSSY on purpose: element order and mixed content
   do not survive it, which is why the tree is the primary shape. */
/* Attributes become @-prefixed keys. Returns the count, or -1. */
static int xml_collapse_attrs(JSContext *ctx, JSValueConst el, JSValue out)
{
    JSValue attrs = JS_GetPropertyStr(ctx, el, "attrs");
    JSPropertyEnum *tab = NULL;
    uint32_t alen = 0, k;
    int rc = 0;

    if (!JS_IsObject(attrs)) {
        JS_FreeValue(ctx, attrs);
        return 0;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &alen, attrs,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, attrs);
        return -1;
    }
    for (k = 0; k < alen && rc == 0; k++) {
        JSValue v = JS_GetProperty(ctx, attrs, tab[k].atom);
        const char *an = JS_AtomToCString(ctx, tab[k].atom);
        char key[256];
        JSAtom ka;
        if (JS_IsException(v) || !an) {
            JS_FreeValue(ctx, v);
            if (an) JS_FreeCString(ctx, an);
            rc = -1;
            break;
        }
        snprintf(key, sizeof key, "@%s", an);
        JS_FreeCString(ctx, an);
        ka = JS_NewAtom(ctx, key);
        if (ka == JS_ATOM_NULL
            || JS_DefinePropertyValue(ctx, out, ka, v, JS_PROP_C_W_E) < 0)
            rc = -1;
        if (ka != JS_ATOM_NULL)
            JS_FreeAtom(ctx, ka);
    }
    JS_FreePropertyEnum(ctx, tab, alen);
    JS_FreeValue(ctx, attrs);
    return rc < 0 ? -1 : (int)alen;
}

/* Text accumulates into `text`; each child element collapses into `out`.
   Returns the number of element children, or -1. */
static int xml_collapse_children(JSContext *ctx, JSValueConst el, JSValue out,
                                 xb_t *text, int depth)
{
    JSValue kids = JS_GetPropertyStr(ctx, el, "children"), lv;
    int64_t len = 0, i;
    int nelem = 0, rc = 0;

    if (JS_IsArray(ctx, kids) != 1) {
        JS_FreeValue(ctx, kids);
        return 0;
    }
    lv = JS_GetPropertyStr(ctx, kids, "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0)
        rc = -1;
    JS_FreeValue(ctx, lv);
    for (i = 0; i < len && rc == 0; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, kids, (uint32_t)i);
        if (JS_IsException(c)) { rc = -1; break; }
        if (JS_IsString(c)) {
            size_t sn;
            const char *sp = JS_ToCStringLen(ctx, &sn, c);
            if (sp) { xb_write(text, sp, sn); JS_FreeCString(ctx, sp); }
        } else if (JS_IsObject(c)) {
            JSValue nv = JS_GetPropertyStr(ctx, c, "name");
            JSValue sub = xml_collapse(ctx, c, depth + 1);
            JSAtom ka = JS_IsException(nv) ? JS_ATOM_NULL : JS_ValueToAtom(ctx, nv);
            JS_FreeValue(ctx, nv);
            if (JS_IsException(sub) || ka == JS_ATOM_NULL
                || xml_collapse_add(ctx, out, ka, sub) < 0) {
                if (ka == JS_ATOM_NULL && !JS_IsException(sub))
                    JS_FreeValue(ctx, sub);
                if (ka != JS_ATOM_NULL)
                    JS_FreeAtom(ctx, ka);
                rc = -1;
            } else {
                JS_FreeAtom(ctx, ka);
                nelem++;
            }
        }
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, kids);
    return rc < 0 ? -1 : nelem;
}

static JSValue xml_collapse(JSContext *ctx, JSValueConst el, int depth)
{
    JSValue out, sv;
    xb_t text;
    int nattr, nelem;

    if (depth >= XML_MAX_DEPTH)
        return JS_ThrowRangeError(ctx, "XMLToObject: nesting exceeds %d", XML_MAX_DEPTH);
    out = JS_NewObject(ctx);
    if (JS_IsException(out))
        return out;
    xb_init(&text);
    nattr = xml_collapse_attrs(ctx, el, out);
    nelem = nattr < 0 ? -1 : xml_collapse_children(ctx, el, out, &text, depth);
    if (nattr < 0 || nelem < 0) {
        JS_FreeValue(ctx, out);
        xb_free(&text);
        return JS_EXCEPTION;
    }
    if (text.n && !nelem && !nattr) {   /* text only: the value IS the string */
        sv = JS_NewStringLen(ctx, (const char *)text.p, text.n);
        JS_FreeValue(ctx, out);
        xb_free(&text);
        return sv;
    }
    if (text.n) {
        sv = JS_NewStringLen(ctx, (const char *)text.p, text.n);
        if (JS_IsException(sv) || JS_SetPropertyStr(ctx, out, "#text", sv) < 0) {
            JS_FreeValue(ctx, out);
            xb_free(&text);
            return JS_EXCEPTION;
        }
    }
    xb_free(&text);
    return out;
}


static JSValue xml_to_object(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValue out, nv, sub;
    JSAtom key;

    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "XMLToObject(node): node must be an element");
    nv = JS_GetPropertyStr(ctx, argv[0], "name");
    if (JS_IsException(nv))
        return JS_EXCEPTION;
    if (!JS_IsString(nv)) {
        JS_FreeValue(ctx, nv);
        return JS_ThrowTypeError(ctx, "XMLToObject(node): node needs a string `name`");
    }
    key = JS_ValueToAtom(ctx, nv);
    JS_FreeValue(ctx, nv);
    if (key == JS_ATOM_NULL)
        return JS_EXCEPTION;
    sub = xml_collapse(ctx, argv[0], 0);
    if (JS_IsException(sub)) {
        JS_FreeAtom(ctx, key);
        return JS_EXCEPTION;
    }
    out = JS_NewObject(ctx);
    if (JS_IsException(out) || JS_DefinePropertyValue(ctx, out, key, sub,
                                                      JS_PROP_C_W_E) < 0) {
        JS_FreeAtom(ctx, key);
        JS_FreeValue(ctx, out);
        return JS_EXCEPTION;
    }
    JS_FreeAtom(ctx, key);
    return out;
}

/* ------------------------------------------------------------ registration */

static const JSCFunctionListEntry dyn_xml_funcs[] = {
    JS_CFUNC_DEF("XMLParse", 1, xml_parse),
    JS_CFUNC_DEF("XMLStringify", 1, xml_stringify),
    JS_CFUNC_DEF("XMLToObject", 1, xml_to_object),
};

static int dyn_xml_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_sax_class_id, &dyn_sax_class,
                                 dyn_sax_proto, countof(dyn_sax_proto),
                                 dyn_sax_ctor, "SAXParser") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_xml_funcs, countof(dyn_xml_funcs));
}

int js_nat_init_xml(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:xml", dyn_xml_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "SAXParser");
    return JS_AddModuleExportList(ctx, m, dyn_xml_funcs, countof(dyn_xml_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_XML */
