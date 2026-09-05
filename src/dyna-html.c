/* dyna:html -- an HTML5 tokenizer, a lenient tree, compiled CSS selectors and
   a sanitizer. The tree shape is dyna:xml's, on purpose: one node shape for
   both markup languages. Full API and the stated scope line: see the module header. */
#include "dyna-nat.h"
#include "dyna-simd-kernels.h"   /* simd.find_first_of for the escape scan */

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTML)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define HT_MAX_DEPTH  256
#define HT_MAX_ATTRS  512
#define HT_MAX_INPUT  (64u << 20)
#define HT_MAX_SEL    4096

typedef struct { uint8_t *p; size_t n, cap; int oom; } hb_t;

static void hb_init(hb_t *b) { b->p = NULL; b->n = b->cap = 0; b->oom = 0; }
static void hb_free(hb_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void hb_write(hb_t *b, const void *p, size_t n)
{
    if (b->oom || !n)
        return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        uint8_t *np;
        while (nc < b->n + n) {
            if (nc < (1u << 20)) nc *= 2;
            else                 nc += nc / 4;
        }
        np = (uint8_t *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void hb_reserve(hb_t *b, size_t need)
{
    if (b->oom || need <= b->cap)
        return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < need) {
        if (nc < (1u << 20)) nc *= 2;
        else                 nc += nc / 4;
    }
    uint8_t *np = (uint8_t *)realloc(b->p, nc);
    if (!np) { b->oom = 1; return; }
    b->p = np; b->cap = nc;
}

static void hb_put(hb_t *b, uint8_t c) { hb_write(b, &c, 1); }
static void hb_puts(hb_t *b, const char *s) { hb_write(b, s, strlen(s)); }

static void hb_putc(hb_t *b, uint32_t cp)
{
    if (cp < 0x80) {
        hb_put(b, (uint8_t)cp);
    } else if (cp < 0x800) {
        hb_put(b, (uint8_t)(0xC0 | (cp >> 6)));
        hb_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        hb_put(b, (uint8_t)(0xE0 | (cp >> 12)));
        hb_put(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        hb_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else {
        hb_put(b, (uint8_t)(0xF0 | (cp >> 18)));
        hb_put(b, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
        hb_put(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        hb_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    }
}

/* ----------------------------------------------------------- element facts */

static int ht_lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int ht_ieq(const char *a, size_t an, const char *b)
{
    size_t i, bn = strlen(b);
    if (an != bn)
        return 0;
    for (i = 0; i < an; i++)
        if (ht_lower((unsigned char)a[i]) != b[i])
            return 0;
    return 1;
}

/* Elements that never have a close tag. A tree builder that does not know
   these nests everything after an <img> inside it. */
static const char *const HT_VOID[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "param", "source", "track", "wbr",
};

/* Elements whose content is TEXT, not markup: a `<` inside them is a
   character, and treating it as a tag is the classic mXSS opening. */
static const char *const HT_RAW[] = { "script", "style", "textarea", "title" };

/* An open <p> or <li> is closed by these, which is what "implied end tag"
   means in practice and why real pages parse at all. */
static const char *const HT_CLOSES_P[] = {
    "address", "article", "aside", "blockquote", "div", "dl", "fieldset",
    "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header",
    "hr", "main", "nav", "ol", "p", "pre", "section", "table", "ul",
};

static int ht_in(const char *const *set, size_t n, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (ht_ieq(s, len, set[i]))
            return 1;
    return 0;
}

#define HT_IS_VOID(s, n)     ht_in(HT_VOID, countof(HT_VOID), (s), (n))
#define HT_IS_RAW(s, n)      ht_in(HT_RAW, countof(HT_RAW), (s), (n))
#define HT_CLOSES(s, n)      ht_in(HT_CLOSES_P, countof(HT_CLOSES_P), (s), (n))

/* ---------------------------------------------------------------- entities */

/* The named references a real page uses. The full HTML5 table is 2231 entries
   and a generated file; this is the working set, and an unknown `&name;` is
   left as literal text -- which is what a browser does for an unknown name. */
static const struct { const char *name; uint32_t cp; } HT_ENT[] = {
    {"amp",38},{"lt",60},{"gt",62},{"quot",34},{"apos",39},{"nbsp",160},
    {"copy",169},{"reg",174},{"trade",8482},{"hellip",8230},{"mdash",8212},
    {"ndash",8211},{"lsquo",8216},{"rsquo",8217},{"ldquo",8220},{"rdquo",8221},
    {"bull",8226},{"dagger",8224},{"permil",8240},{"lsaquo",8249},
    {"rsaquo",8250},{"euro",8364},{"pound",163},{"yen",165},{"cent",162},
    {"sect",167},{"para",182},{"middot",183},{"deg",176},{"plusmn",177},
    {"times",215},{"divide",247},{"frac12",189},{"frac14",188},{"frac34",190},
    {"laquo",171},{"raquo",187},{"iexcl",161},{"iquest",191},{"szlig",223},
    {"agrave",224},{"aacute",225},{"eacute",233},{"egrave",232},{"ccedil",231},
    {"ntilde",241},{"ouml",246},{"uuml",252},{"auml",228},{"alpha",945},
    {"beta",946},{"gamma",947},{"pi",960},{"micro",181},{"larr",8592},
    {"rarr",8594},{"harr",8596},{"crarr",8629},{"infin",8734},{"ne",8800},
    {"le",8804},{"ge",8805},{"asymp",8776},{"sum",8721},{"radic",8730},
    {"star",9733},{"hearts",9829},{"check",10003},{"cross",10007},
};

/* A character reference, or 0 with *pi untouched when it is not one. */
static int ht_entity(const char *s, size_t n, size_t *pi, hb_t *out)
{
    size_t i = *pi + 1, j;
    uint32_t cp = 0;

    if (i < n && s[i] == '#') {
        int hex = (i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')), any = 0;
        j = hex ? i + 2 : i + 1;
        for (; j < n; j++) {
            int d;
            if (s[j] >= '0' && s[j] <= '9') d = s[j] - '0';
            else if (hex && s[j] >= 'a' && s[j] <= 'f') d = s[j] - 'a' + 10;
            else if (hex && s[j] >= 'A' && s[j] <= 'F') d = s[j] - 'A' + 10;
            else break;
            cp = cp * (hex ? 16u : 10u) + (uint32_t)d;
            any = 1;
            if (cp > 0x10FFFF)
                return 0;
        }
        if (!any)
            return 0;
        if (j < n && s[j] == ';')
            j++;
        /* A surrogate or an out-of-range code point becomes U+FFFD, which is
           what the spec says and what stops a lone half reaching the output. */
        if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            cp = 0xFFFD;
        hb_putc(out, cp);
        *pi = j;
        return 1;
    }
    for (j = i; j < n && j < i + 32; j++)
        if (s[j] == ';')
            break;
    if (j >= n || j >= i + 32 || j == i)
        return 0;
    {
        size_t k, len = j - i;
        for (k = 0; k < countof(HT_ENT); k++)
            if (ht_ieq(s + i, len, HT_ENT[k].name)) {
                hb_putc(out, HT_ENT[k].cp);
                *pi = j + 1;
                return 1;
            }
    }
    return 0;                           /* unknown: the text stays literal */
}

static void ht_decode(const char *s, size_t n, hb_t *out)
{
    size_t i = 0;

    if (out->cap == 0 && n > 1024)
        hb_reserve(out, n);
    while (i < n) {
        size_t start = i;
        size_t probe = i + 64;
        size_t j = i;

        if (probe > n) probe = n;
        while (j < probe && s[j] != '&') j++;
        if (j == probe && n - j >= 64) {
            size_t t = simd.find_first_of((const uint8_t *)s + j, n - j,
                                          (const uint8_t *)"&", 1);
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

/* ------------------------------------------------------------- the parser */

typedef struct {
    const char *n;
    size_t      nlen;
    size_t      voff, vlen;             /* into the decoded attribute buffer */
} ht_attr_t;

typedef struct {
    JSContext  *ctx;
    const char *s;
    size_t      n, i;
    hb_t        text, abuf;
    ht_attr_t   attr[HT_MAX_ATTRS];
    uint32_t    nattr;
    const char *tag;
    size_t      taglen;
    int         selfclose;
    JSValue     stack[HT_MAX_DEPTH];    /* the children array of each open node */
    char        names[HT_MAX_DEPTH][64];
    uint32_t    counts[HT_MAX_DEPTH];
    int         depth;
    char        err[160];
} ht_t;

static int ht_tag_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int ht_name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':' || c >= 0x80;
}

static int ht_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Whether an attribute with this name was already scanned for the current
   tag. Comparison is ASCII-case-insensitive: the names are lower-cased when
   the element is built, so `HREF` and `href` are the same attribute. */
static int ht_attr_dup(const ht_t *p, const char *n, size_t nl)
{
    uint32_t k;
    size_t i;
    for (k = 0; k < p->nattr; k++) {
        if (p->attr[k].nlen != nl)
            continue;
        for (i = 0; i < nl; i++)
            if (ht_lower((unsigned char)p->attr[k].n[i])
                != ht_lower((unsigned char)n[i]))
                break;
        if (i == nl)
            return 1;
    }
    return 0;
}

/* One attribute, or 0 when the tag's attribute list is finished. */
static int ht_scan_attr(ht_t *p)
{
    size_t st;

    while (p->i < p->n && ht_space((unsigned char)p->s[p->i])) p->i++;
    if (p->i >= p->n || p->s[p->i] == '>' )
        return 0;
    if (p->s[p->i] == '/') {
        p->i++;
        p->selfclose = 1;
        return 0;
    }
    st = p->i;
    while (p->i < p->n && !ht_space((unsigned char)p->s[p->i])
           && p->s[p->i] != '=' && p->s[p->i] != '>' && p->s[p->i] != '/')
        p->i++;
    if (p->i == st) {                   /* a stray character: skip it */
        p->i++;
        return 1;
    }
    if (p->nattr >= HT_MAX_ATTRS)
        return 0;
    p->attr[p->nattr].n = p->s + st;
    p->attr[p->nattr].nlen = p->i - st;
    p->attr[p->nattr].voff = p->abuf.n;
    p->attr[p->nattr].vlen = 0;
    while (p->i < p->n && ht_space((unsigned char)p->s[p->i])) p->i++;
    if (p->i < p->n && p->s[p->i] == '=') {
        size_t vs;
        p->i++;
        while (p->i < p->n && ht_space((unsigned char)p->s[p->i])) p->i++;
        if (p->i < p->n && (p->s[p->i] == '"' || p->s[p->i] == '\'')) {
            char q = p->s[p->i++];
            vs = p->i;
            while (p->i < p->n && p->s[p->i] != q) p->i++;
            ht_decode(p->s + vs, p->i - vs, &p->abuf);
            if (p->i < p->n) p->i++;
        } else {
            vs = p->i;
            while (p->i < p->n && !ht_space((unsigned char)p->s[p->i])
                   && p->s[p->i] != '>')
                p->i++;
            ht_decode(p->s + vs, p->i - vs, &p->abuf);
        }
        p->attr[p->nattr].vlen = p->abuf.n - p->attr[p->nattr].voff;
    }
    /* A duplicated attribute keeps the FIRST occurrence, as a browser does:
       the later one is parsed (its value bytes are dropped from the decode
       buffer) but not stored. */
    if (ht_attr_dup(p, p->attr[p->nattr].n, p->attr[p->nattr].nlen)) {
        p->abuf.n = p->attr[p->nattr].voff;
        return 1;
    }
    p->nattr++;
    return 1;
}

/* Build the element object; `children` is handed back so the caller can push. */
static JSValue ht_new_element(ht_t *p, JSValue *children)
{
    JSValue el = JS_NewObject(p->ctx), attrs;
    uint32_t k;
    char lower[64];
    size_t ln = p->taglen > 63 ? 63 : p->taglen, m;

    if (JS_IsException(el))
        return el;
    for (m = 0; m < ln; m++)
        lower[m] = (char)ht_lower((unsigned char)p->tag[m]);
    attrs = JS_NewObject(p->ctx);
    if (JS_IsException(attrs)) {
        JS_FreeValue(p->ctx, el);
        return JS_EXCEPTION;
    }
    for (k = 0; k < p->nattr; k++) {
        char an[128];
        size_t al = p->attr[k].nlen > 127 ? 127 : p->attr[k].nlen, q;
        JSAtom a;
        JSValue v;
        for (q = 0; q < al; q++)
            an[q] = (char)ht_lower((unsigned char)p->attr[k].n[q]);
        a = JS_NewAtomLen(p->ctx, an, al);
        if (a == JS_ATOM_NULL) { JS_FreeValue(p->ctx, attrs); JS_FreeValue(p->ctx, el); return JS_EXCEPTION; }
        v = JS_NewStringLen(p->ctx, (const char *)p->abuf.p + p->attr[k].voff,
                            p->attr[k].vlen);
        /* DEFINE: an attribute called __proto__ is an own property. */
        if (JS_IsException(v)
            || JS_DefinePropertyValue(p->ctx, attrs, a, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(p->ctx, a);
            JS_FreeValue(p->ctx, attrs);
            JS_FreeValue(p->ctx, el);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(p->ctx, a);
    }
    *children = JS_NewArray(p->ctx);
    if (JS_IsException(*children)) {
        JS_FreeValue(p->ctx, attrs);
        JS_FreeValue(p->ctx, el);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(p->ctx, el, "name", JS_NewStringLen(p->ctx, lower, ln)) < 0
        || JS_SetPropertyStr(p->ctx, el, "attrs", attrs) < 0
        || JS_SetPropertyStr(p->ctx, el, "children",
                             JS_DupValue(p->ctx, *children)) < 0) {
        JS_FreeValue(p->ctx, *children);
        JS_FreeValue(p->ctx, el);
        return JS_EXCEPTION;
    }
    return el;
}

static int ht_push_text(ht_t *p)
{
    JSValue sv;

    if (p->text.n == 0)
        return 0;
    sv = JS_NewStringLen(p->ctx, (const char *)p->text.p, p->text.n);
    p->text.n = 0;
    if (JS_IsException(sv))
        return -1;
    return JS_DefinePropertyValueUint32(p->ctx, p->stack[p->depth],
                                        p->counts[p->depth]++, sv,
                                        JS_PROP_C_W_E) < 0 ? -1 : 0;
}

/* Close the innermost open element whose name matches, if any. An unmatched
   close tag is IGNORED, which is what a browser does and what stops a stray
   </div> from unwinding the whole document. */
static void ht_close(ht_t *p, const char *name, size_t nlen)
{
    int d;

    for (d = p->depth; d > 0; d--)
        if (ht_ieq(name, nlen, p->names[d]))
            break;
    if (d == 0)
        return;
    while (p->depth >= d) {
        JS_FreeValue(p->ctx, p->stack[p->depth]);
        p->depth--;
    }
}

static int ht_open(ht_t *p)
{
    JSValue kids, el;
    size_t ln;

    /* An open <p> or <li> ends where the next block-level element starts. */
    if (p->depth > 0 && (ht_ieq(p->names[p->depth], strlen(p->names[p->depth]), "p")
                         && HT_CLOSES(p->tag, p->taglen)))
        ht_close(p, "p", 1);
    if (p->depth > 0 && ht_ieq(p->names[p->depth], strlen(p->names[p->depth]), "li")
        && ht_ieq(p->tag, p->taglen, "li"))
        ht_close(p, "li", 2);
    if (p->depth + 1 >= HT_MAX_DEPTH) {
        snprintf(p->err, sizeof p->err, "nesting exceeds %d elements", HT_MAX_DEPTH);
        return -1;
    }
    el = ht_new_element(p, &kids);
    if (JS_IsException(el))
        return -1;
    if (JS_DefinePropertyValueUint32(p->ctx, p->stack[p->depth],
                                     p->counts[p->depth]++, el,
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(p->ctx, kids);
        return -1;
    }
    if (p->selfclose || HT_IS_VOID(p->tag, p->taglen)) {
        JS_FreeValue(p->ctx, kids);
        return 0;
    }
    p->depth++;
    p->stack[p->depth] = kids;
    ln = p->taglen > 62 ? 62 : p->taglen;
    {
        size_t m;
        for (m = 0; m < ln; m++)
            p->names[p->depth][m] = (char)ht_lower((unsigned char)p->tag[m]);
        p->names[p->depth][ln] = 0;
    }
    p->counts[p->depth] = 0;
    return 0;
}

/* The body of a raw-text element is TEXT up to its own close tag. Scanning it
   as markup is the mXSS opening this exists to close. */
static void ht_raw_text(ht_t *p, const char *tag, size_t tlen)
{
    size_t st = p->i;
    const size_t n = p->n;
    const char *s = p->s;

    /* Portfolio: scalar for small (<256B), SIMD jump for large. A 1MB
       <script> without '<' inside would otherwise pay per-byte. */
    if (n - p->i >= 256) {
        while (p->i < n) {
            size_t t = simd.find_first_of((const uint8_t *)s + p->i, n - p->i,
                                          (const uint8_t *)"<", 1);
            if (t == (size_t)-1) { p->i = n; break; }
            p->i += t;
            if (p->i + 1 < n && s[p->i + 1] == '/'
                && p->i + 2 + tlen <= n
                && ht_ieq(s + p->i + 2, tlen, p->names[p->depth])) {
                break;
            }
            p->i++;
        }
    } else {
        while (p->i < n) {
            if (s[p->i] == '<' && p->i + 1 < n && s[p->i + 1] == '/'
                && p->i + 2 + tlen <= n
                && ht_ieq(s + p->i + 2, tlen, p->names[p->depth])) {
                break;
            }
            p->i++;
        }
    }
    (void)tag;
    hb_write(&p->text, s + st, p->i - st);
}

/* `<!`: a comment, a doctype or a CDATA section. None reach the tree. */
static void ht_skip_bang(ht_t *p)
{
    if (p->n - p->i >= 4 && memcmp(p->s + p->i, "<!--", 4) == 0) {
        size_t j = p->i + 4;
        if (p->n - j >= 256) {
            while (j + 2 < p->n) {
                size_t t = simd.find_first_of((const uint8_t *)p->s + j, p->n - j,
                                              (const uint8_t *)"-", 1);
                if (t == (size_t)-1) { j = p->n; break; }
                j += t;
                if (j + 2 < p->n && p->s[j] == '-' && p->s[j+1] == '-' && p->s[j+2] == '>')
                    break;
                j++;
            }
        } else {
            while (j + 2 < p->n && !(p->s[j] == '-' && p->s[j+1] == '-' && p->s[j+2] == '>'))
                j++;
        }
        p->i = j + 2 < p->n ? j + 3 : p->n;
        return;
    }
    if (p->n - p->i >= 256) {
        size_t t = simd.find_first_of((const uint8_t *)p->s + p->i, p->n - p->i,
                                      (const uint8_t *)">", 1);
        p->i = (t == (size_t)-1) ? p->n : p->i + t + 1;
        return;
    }
    while (p->i < p->n && p->s[p->i] != '>') p->i++;
    if (p->i < p->n) p->i++;
}

/* `</name>`. An unmatched close tag is IGNORED, which is what a browser does
   and what stops a stray </div> from unwinding the whole document. */
static int ht_step_close(ht_t *p)
{
    size_t j = p->i + 2, st = j;

    if (j < p->n && ht_tag_start((unsigned char)p->s[j]))
        while (j < p->n && ht_name_char((unsigned char)p->s[j])) j++;
    if (j == st) {                      /* `</` with no name is text */
        hb_put(&p->text, '<');
        p->i++;
        return 0;
    }
    if (ht_push_text(p) < 0)
        return -1;
    ht_close(p, p->s + st, j - st);
    while (j < p->n && p->s[j] != '>') j++;
    p->i = j < p->n ? j + 1 : p->n;
    return 0;
}

static int ht_step_open(ht_t *p)
{
    size_t st = p->i + 1;
    int raw;
    const char *tag;
    size_t tlen;

    p->i = st;
    while (p->i < p->n && ht_name_char((unsigned char)p->s[p->i])) p->i++;
    p->tag = p->s + st;
    p->taglen = p->i - st;
    p->nattr = 0;
    p->abuf.n = 0;
    p->selfclose = 0;
    while (ht_scan_attr(p))
        ;
    if (p->i < p->n && p->s[p->i] == '>')
        p->i++;
    if (ht_push_text(p) < 0)
        return -1;
    raw = HT_IS_RAW(p->tag, p->taglen);
    tag = p->tag;
    tlen = p->taglen;
    if (ht_open(p) < 0)
        return -1;
    if (raw && !p->selfclose && p->depth > 0) {
        ht_raw_text(p, tag, tlen);
        if (ht_push_text(p) < 0)
            return -1;
    }
    return 0;
}

static int ht_step(ht_t *p)
{
    size_t st;

    if (p->s[p->i] != '<') {
        st = p->i;
        while (p->i < p->n && p->s[p->i] != '<') p->i++;
        ht_decode(p->s + st, p->i - st, &p->text);
        return 0;
    }
    if (p->i + 1 >= p->n) {             /* a trailing '<' is text */
        hb_put(&p->text, '<');
        p->i++;
        return 0;
    }
    if (p->s[p->i + 1] == '!') {
        ht_skip_bang(p);
        return 0;
    }
    if (p->s[p->i + 1] == '/')
        return ht_step_close(p);
    if (!ht_tag_start((unsigned char)p->s[p->i + 1])) {
        hb_put(&p->text, '<');          /* `<3` is text, not a tag */
        p->i++;
        return 0;
    }
    return ht_step_open(p);
}

static JSValue ht_parse_into(JSContext *ctx, const char *s, size_t n)
{
    ht_t p;
    JSValue root;
    int rc = 0;

    memset(&p, 0, sizeof p);
    p.ctx = ctx;
    p.s = s;
    p.n = n;
    hb_init(&p.text);
    hb_init(&p.abuf);
    if (n > 4096) {
        size_t ec = n > HT_MAX_INPUT ? HT_MAX_INPUT : n;
        hb_reserve(&p.text, ec < 8192 ? ec : 8192);
        hb_reserve(&p.abuf, 4096);
    }
    root = JS_NewArray(ctx);
    if (JS_IsException(root)) {
        hb_free(&p.text);
        hb_free(&p.abuf);
        return root;
    }
    p.stack[0] = root;
    p.names[0][0] = 0;
    while (p.i < p.n && rc == 0)
        rc = ht_step(&p);
    if (rc == 0)
        rc = ht_push_text(&p);
    while (p.depth > 0) {
        JS_FreeValue(ctx, p.stack[p.depth]);
        p.depth--;
    }
    hb_free(&p.text);
    hb_free(&p.abuf);
    if (rc < 0) {
        JS_FreeValue(ctx, root);
        return p.err[0] ? JS_ThrowRangeError(ctx, "HTMLParse: %s", p.err)
                        : JS_EXCEPTION;
    }
    return root;
}

static JSValue dyn_html_parse(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    JSValue out;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "HTMLParse(text): text must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > HT_MAX_INPUT) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "HTMLParse: input exceeds %u bytes",
                                  HT_MAX_INPUT);
    }
    out = ht_parse_into(ctx, s, n);
    JS_FreeCString(ctx, s);
    return out;
}

/* --------------------------------------------------------------- serialize */

static const uint8_t HT_ESC[256] = { ['<']=1, ['>']=1, ['&']=1, ['"']=1 };
static const uint8_t HT_ESC_SET[4] = { '<', '>', '&', '"' };
/* Below this the indirect call costs more than the scan -- the same trap that
   made this bind a 15-20% REGRESSION in the CSV field scanner. */
#define HT_SIMD_MIN 64

static void ht_escape(hb_t *b, const char *s, size_t n, int attr)
{
    size_t i = 0, run;

    while (i < n) {
        run = i;
        /* Scalar probe FIRST. The gate cannot be on the remaining input: with
           an escape every few bytes the run is short however much input is
           left, and paying the indirect call per short run measured 0.73x. */
        { size_t probe = i + HT_SIMD_MIN;
          if (probe > n) probe = n;
          while (i < probe && !HT_ESC[(unsigned char)s[i]]) i++;
          if (i == probe && n - i >= HT_SIMD_MIN) {
              size_t k = simd.find_first_of((const uint8_t *)s + i, n - i,
                                            HT_ESC_SET, sizeof HT_ESC_SET);
              i = (k == (size_t)-1) ? n : i + k;
          } }
        hb_write(b, s + run, i - run);
        if (i >= n)
            break;
        switch (s[i]) {
        case '<': hb_puts(b, "&lt;"); break;
        case '>': hb_puts(b, "&gt;"); break;
        case '&': hb_puts(b, "&amp;"); break;
        default:  hb_puts(b, attr ? "&quot;" : "\""); break;
        }
        i++;
    }
}

typedef struct { JSContext *ctx; hb_t b; int depth; } ht_ser_t;

static int ht_ser_node(ht_ser_t *w, JSValueConst v);

static int ht_ser_children(ht_ser_t *w, JSValueConst el)
{
    JSValue kids = JS_GetPropertyStr(w->ctx, el, "children"), lv;
    int64_t len = 0, i;
    int rc = 0;

    if (JS_IsArray(w->ctx, kids) != 1) {
        JS_FreeValue(w->ctx, kids);
        return 0;
    }
    lv = JS_GetPropertyStr(w->ctx, kids, "length");
    if (JS_IsException(lv) || JS_ToInt64(w->ctx, &len, lv) < 0)
        rc = -1;
    JS_FreeValue(w->ctx, lv);
    for (i = 0; i < len && rc == 0; i++) {
        JSValue c = JS_GetPropertyUint32(w->ctx, kids, (uint32_t)i);
        if (JS_IsException(c)) { rc = -1; break; }
        rc = ht_ser_node(w, c);
        JS_FreeValue(w->ctx, c);
    }
    JS_FreeValue(w->ctx, kids);
    return rc;
}

static int ht_ser_attrs(ht_ser_t *w, JSValueConst el)
{
    JSValue attrs = JS_GetPropertyStr(w->ctx, el, "attrs");
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, k;
    int rc = 0;

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
        JSValue v = JS_GetProperty(w->ctx, attrs, tab[k].atom);
        const char *an, *av;
        size_t anl, avl;
        an = JS_AtomToCStringLen(w->ctx, &anl, tab[k].atom);
        av = JS_IsException(v) ? NULL : JS_ToCStringLen(w->ctx, &avl, v);
        JS_FreeValue(w->ctx, v);
        if (!an || !av) {
            if (an) JS_FreeCString(w->ctx, an);
            if (av) JS_FreeCString(w->ctx, av);
            rc = -1;
            break;
        }
        /* hand-built trees are the injection vector: the parser can never
           produce `x" onload=` as a name, so neither may the serializer */
        if (anl == 0 || !ht_tag_start((unsigned char)an[0])) {
            JS_ThrowTypeError(w->ctx, "HTMLStringify: invalid attribute name");
            JS_FreeCString(w->ctx, an);
            JS_FreeCString(w->ctx, av);
            JS_FreePropertyEnum(w->ctx, tab, len);
            JS_FreeValue(w->ctx, attrs);
            return -1;
        }
        {
            size_t ci;
            for (ci = 1; ci < anl; ci++)
                if (!ht_name_char((unsigned char)an[ci])) {
                    JS_ThrowTypeError(w->ctx, "HTMLStringify: invalid attribute name");
                    JS_FreeCString(w->ctx, an);
                    JS_FreeCString(w->ctx, av);
                    JS_FreePropertyEnum(w->ctx, tab, len);
                    JS_FreeValue(w->ctx, attrs);
                    return -1;
                }
        }
        hb_put(&w->b, ' ');
        hb_write(&w->b, an, anl);
        hb_puts(&w->b, "=\"");
        ht_escape(&w->b, av, avl, 1);
        hb_put(&w->b, '"');
        JS_FreeCString(w->ctx, an);
        JS_FreeCString(w->ctx, av);
    }
    JS_FreePropertyEnum(w->ctx, tab, len);
    JS_FreeValue(w->ctx, attrs);
    return rc;
}

static int ht_ser_node(ht_ser_t *w, JSValueConst v)
{
    JSValue nv;
    const char *nm;
    size_t nml;
    int rc, raw;

    if (w->depth >= HT_MAX_DEPTH) {
        JS_ThrowRangeError(w->ctx, "HTMLStringify: nesting exceeds %d", HT_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(v)) {
        const char *s;
        size_t n;
        if (JS_IsUndefined(v) || JS_IsNull(v))
            return 0;
        s = JS_ToCStringLen(w->ctx, &n, v);
        if (!s)
            return -1;
        ht_escape(&w->b, s, n, 0);
        JS_FreeCString(w->ctx, s);
        return 0;
    }
    nv = JS_GetPropertyStr(w->ctx, v, "name");
    if (JS_IsException(nv))
        return -1;
    if (!JS_IsString(nv)) {
        JS_FreeValue(w->ctx, nv);
        JS_ThrowTypeError(w->ctx, "HTMLStringify: a node needs a string `name`");
        return -1;
    }
    nm = JS_ToCStringLen(w->ctx, &nml, nv);
    JS_FreeValue(w->ctx, nv);
    if (!nm)
        return -1;
    if (nml == 0 || !ht_tag_start((unsigned char)nm[0])) {
        JS_FreeCString(w->ctx, nm);
        JS_ThrowTypeError(w->ctx, "HTMLStringify: not a valid element name");
        return -1;
    }
    {
        size_t ci;
        for (ci = 1; ci < nml; ci++)
            if (!ht_name_char((unsigned char)nm[ci])) {
                JS_FreeCString(w->ctx, nm);
                JS_ThrowTypeError(w->ctx, "HTMLStringify: not a valid element name");
                return -1;
            }
    }
    hb_put(&w->b, '<');
    hb_write(&w->b, nm, nml);
    rc = ht_ser_attrs(w, v);
    hb_put(&w->b, '>');
    raw = HT_IS_RAW(nm, nml);
    if (rc == 0 && !HT_IS_VOID(nm, nml)) {
        w->depth++;
        if (raw) {
            /* A raw-text element's content is NOT escaped, because it is not
               markup -- escaping it would corrupt every script on the page. */
            JSValue kids = JS_GetPropertyStr(w->ctx, v, "children"), c;
            if (JS_IsArray(w->ctx, kids) == 1) {
                c = JS_GetPropertyUint32(w->ctx, kids, 0);
                if (JS_IsString(c)) {
                    size_t tn;
                    const char *ts = JS_ToCStringLen(w->ctx, &tn, c);
                    if (ts) { hb_write(&w->b, ts, tn); JS_FreeCString(w->ctx, ts); }
                }
                JS_FreeValue(w->ctx, c);
            }
            JS_FreeValue(w->ctx, kids);
        } else {
            rc = ht_ser_children(w, v);
        }
        w->depth--;
        if (rc == 0) {
            hb_puts(&w->b, "</");
            hb_write(&w->b, nm, nml);
            hb_put(&w->b, '>');
        }
    }
    JS_FreeCString(w->ctx, nm);
    return rc;
}

static JSValue dyn_html_stringify(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    ht_ser_t w;
    JSValue out;
    int rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "HTMLStringify(node): a node is required");
    memset(&w, 0, sizeof w);
    w.ctx = ctx;
    hb_init(&w.b);
    if (JS_IsArray(ctx, argv[0]) == 1) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        int64_t len = 0, i;
        rc = (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) ? -1 : 0;
        JS_FreeValue(ctx, lv);
        for (i = 0; i < len && rc == 0; i++) {
            JSValue c = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            rc = JS_IsException(c) ? -1 : ht_ser_node(&w, c);
            JS_FreeValue(ctx, c);
        }
    } else {
        rc = ht_ser_node(&w, argv[0]);
    }
    if (rc < 0 || w.b.oom) {
        if (w.b.oom) JS_ThrowOutOfMemory(ctx);
        hb_free(&w.b);
        return JS_EXCEPTION;
    }
    out = JS_NewStringLen(ctx, (const char *)w.b.p, w.b.n);
    hb_free(&w.b);
    return out;
}

/* ------------------------------------------------------------- selectors */

enum { SP_TAG, SP_ID, SP_CLASS, SP_ATTR, SP_FIRST, SP_LAST, SP_ANY };
enum { AO_HAS, AO_EQ, AO_PREFIX, AO_SUFFIX, AO_CONTAINS, AO_WORD };
enum { CB_DESC, CB_CHILD };             /* the combinator BEFORE a compound */

typedef struct {
    uint8_t  kind, op;
    char    *a, *b;                     /* name and value, both owned */
    uint32_t sub;                       /* spare; the removed :not() support
                                           used it for the inner part index */
} sel_part_t;

typedef struct {
    uint8_t  comb;
    uint32_t first, n;                  /* range into parts */
} sel_unit_t;

typedef struct {
    sel_unit_t *units; uint32_t nunit, cunit;
    sel_part_t *parts; uint32_t npart, cpart;
    uint32_t   *groups; uint32_t ngroup, cgroup;   /* unit index each group starts at */
} sel_t;

static void sel_free(sel_t *s)
{
    uint32_t i;
    if (!s)
        return;
    for (i = 0; i < s->npart; i++) { free(s->parts[i].a); free(s->parts[i].b); }
    free(s->parts); free(s->units); free(s->groups);
    free(s);
}

static void sel_free_v(void *p) { sel_free((sel_t *)p); }

static char *sel_dup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

static int sel_add_part(sel_t *s, uint8_t kind, uint8_t op, char *a, char *b)
{
    if (s->npart == s->cpart) {
        uint32_t nc = s->cpart ? s->cpart * 2 : 8;
        sel_part_t *np = (sel_part_t *)realloc(s->parts, nc * sizeof *np);
        if (!np) { free(a); free(b); return -1; }
        s->parts = np; s->cpart = nc;
    }
    memset(&s->parts[s->npart], 0, sizeof s->parts[0]);
    s->parts[s->npart].kind = kind;
    s->parts[s->npart].op = op;
    s->parts[s->npart].a = a;
    s->parts[s->npart].b = b;
    s->npart++;
    return 0;
}

static int sel_add_unit(sel_t *s, uint8_t comb, uint32_t first, uint32_t n)
{
    if (s->nunit == s->cunit) {
        uint32_t nc = s->cunit ? s->cunit * 2 : 8;
        sel_unit_t *np = (sel_unit_t *)realloc(s->units, nc * sizeof *np);
        if (!np) return -1;
        s->units = np; s->cunit = nc;
    }
    s->units[s->nunit].comb = comb;
    s->units[s->nunit].first = first;
    s->units[s->nunit].n = n;
    s->nunit++;
    return 0;
}

/* A selector identifier: letters, digits, `-` and `_`. NOT ':' or '.', which
   start a pseudo-class and a class -- ht_name_char accepts both because an
   element name may contain them. */
static int sel_ident_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80;
}

static int sel_ident(const char *s, size_t n, size_t *i, char **out)
{
    size_t st = *i;
    while (*i < n && sel_ident_char((unsigned char)s[*i]))
        (*i)++;
    if (*i == st)
        return -1;
    *out = sel_dup(s + st, *i - st);
    return *out ? 0 : -1;
}

/* One compound: tag, #id, .class, [attr op "v"], :first-child, :last-child. */
static int sel_compound(sel_t *sel, const char *s, size_t n, size_t *i,
                        uint32_t *count, char *err, size_t errsz)
{
    *count = 0;
    for (;;) {
        char *a = NULL, *b = NULL;
        if (*i >= n)
            break;
        if (s[*i] == '*') {
            /* The universal selector must still COUNT as a compound, or the
               parser reports "unexpected character" at the end of the input. */
            (*i)++;
            if (sel_add_part(sel, SP_ANY, 0, NULL, NULL) < 0) return -1;
            (*count)++;
            continue;
        }
        if (s[*i] == '#' || s[*i] == '.') {
            uint8_t k = s[*i] == '#' ? SP_ID : SP_CLASS;
            (*i)++;
            if (sel_ident(s, n, i, &a) < 0) {
                snprintf(err, errsz, "expected a name after %c", s[*i - 1]);
                return -1;
            }
            if (sel_add_part(sel, k, 0, a, NULL) < 0) return -1;
            (*count)++;
            continue;
        }
        if (s[*i] == '[') {
            size_t st;
            uint8_t op = AO_HAS;
            (*i)++;
            st = *i;
            while (*i < n && s[*i] != ']' && s[*i] != '=' && s[*i] != '^'
                   && s[*i] != '$' && s[*i] != '*' && s[*i] != '~')
                (*i)++;
            if (*i == st) { snprintf(err, errsz, "empty attribute selector"); return -1; }
            a = sel_dup(s + st, *i - st);
            if (!a) return -1;
            {
                /* The parser lower-cases attribute names, so the selector's
                   name is lower-cased to meet it: [CLASS=x] matches <p class=x>
                   and hand-built trees alike. */
                size_t k, l = strlen(a);
                for (k = 0; k < l; k++) a[k] = (char)ht_lower((unsigned char)a[k]);
            }
            if (*i < n && s[*i] != ']') {
                switch (s[*i]) {
                case '^': op = AO_PREFIX; (*i)++; break;
                case '$': op = AO_SUFFIX; (*i)++; break;
                case '*': op = AO_CONTAINS; (*i)++; break;
                case '~': op = AO_WORD; (*i)++; break;
                default:  op = AO_EQ; break;
                }
                if (*i >= n || s[*i] != '=') {
                    free(a);
                    snprintf(err, errsz, "expected = in an attribute selector");
                    return -1;
                }
                (*i)++;
                if (op == AO_HAS) op = AO_EQ;
                if (*i < n && (s[*i] == '"' || s[*i] == '\'')) {
                    char q = s[(*i)++];
                    st = *i;
                    while (*i < n && s[*i] != q) (*i)++;
                    b = sel_dup(s + st, *i - st);
                    if (*i < n) (*i)++;
                } else {
                    st = *i;
                    while (*i < n && s[*i] != ']') (*i)++;
                    b = sel_dup(s + st, *i - st);
                }
                if (!b) { free(a); return -1; }
            }
            if (*i >= n || s[*i] != ']') {
                free(a); free(b);
                snprintf(err, errsz, "unterminated attribute selector");
                return -1;
            }
            (*i)++;
            if (sel_add_part(sel, SP_ATTR, op, a, b) < 0) return -1;
            (*count)++;
            continue;
        }
        if (s[*i] == ':') {
            (*i)++;
            if (n - *i >= 11 && memcmp(s + *i, "first-child", 11) == 0) {
                *i += 11;
                if (sel_add_part(sel, SP_FIRST, 0, NULL, NULL) < 0) return -1;
            } else if (n - *i >= 10 && memcmp(s + *i, "last-child", 10) == 0) {
                *i += 10;
                if (sel_add_part(sel, SP_LAST, 0, NULL, NULL) < 0) return -1;
            } else {
                snprintf(err, errsz, "the pseudo-classes here are :first-child "
                                     "and :last-child");
                return -1;
            }
            (*count)++;
            continue;
        }
        if (sel_ident_char((unsigned char)s[*i])) {
            if (sel_ident(s, n, i, &a) < 0) return -1;
            {
                size_t k, l = strlen(a);
                for (k = 0; k < l; k++) a[k] = (char)ht_lower((unsigned char)a[k]);
            }
            if (sel_add_part(sel, SP_TAG, 0, a, NULL) < 0) return -1;
            (*count)++;
            continue;
        }
        break;
    }
    return 0;
}

static sel_t *sel_compile(const char *s, size_t n, char *err, size_t errsz)
{
    sel_t *sel = (sel_t *)calloc(1, sizeof *sel);
    size_t i = 0;
    uint8_t comb = CB_DESC;
    int started = 0;

    if (!sel)
        return NULL;
    for (;;) {
        uint32_t first = sel->npart, count = 0;
        while (i < n && ht_space((unsigned char)s[i])) i++;
        if (i >= n)
            break;
        if (s[i] == ',') {
            if (!started) { snprintf(err, errsz, "an empty selector in the group"); goto fail; }
            i++;
            comb = CB_DESC;
            started = 0;
            continue;
        }
        if (s[i] == '>') {
            if (!started) { snprintf(err, errsz, "a combinator with nothing before it"); goto fail; }
            comb = CB_CHILD;
            i++;
            continue;
        }
        if (!started) {
            if (sel->ngroup == sel->cgroup) {
                uint32_t nc = sel->cgroup ? sel->cgroup * 2 : 4;
                uint32_t *np = (uint32_t *)realloc(sel->groups, nc * sizeof *np);
                if (!np) goto fail;
                sel->groups = np; sel->cgroup = nc;
            }
            sel->groups[sel->ngroup++] = sel->nunit;
        }
        if (sel_compound(sel, s, n, &i, &count, err, errsz) < 0)
            goto fail;
        if (count == 0) {
            snprintf(err, errsz, "unexpected character '%c'", s[i]);
            goto fail;
        }
        if (sel_add_unit(sel, comb, first, count) < 0)
            goto fail;
        comb = CB_DESC;
        started = 1;
    }
    if (!started) {
        snprintf(err, errsz, "an empty selector matches nothing and is a typo");
        goto fail;
    }
    if (comb != CB_DESC) {
        /* A trailing combinator has nothing on its right, and accepting it
           silently makes "div >" mean "div". */
        snprintf(err, errsz, "a combinator with nothing after it");
        goto fail;
    }
    return sel;
fail:
    sel_free(sel);
    return NULL;
}

/* --------------------------------------------------------- selector match */

typedef struct {
    JSContext *ctx;
    JSValue    el[HT_MAX_DEPTH];        /* the ancestor chain, root first */
    int32_t    idx[HT_MAX_DEPTH];       /* each one's index in its parent */
    uint8_t    last[HT_MAX_DEPTH];      /* and whether it is the last child */
    int        depth;
} sel_ctx_t;

static int sel_attr_eq(JSContext *ctx, JSValueConst el, const char *name,
                       const char *want, int op)
{
    JSValue attrs = JS_GetPropertyStr(ctx, el, "attrs"), v;
    const char *s;
    size_t sn, wn;
    int r = 0;

    if (!JS_IsObject(attrs)) { JS_FreeValue(ctx, attrs); return 0; }
    v = JS_GetPropertyStr(ctx, attrs, name);
    JS_FreeValue(ctx, attrs);
    if (JS_IsException(v) || JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (op == AO_HAS) { JS_FreeValue(ctx, v); return 1; }
    s = JS_ToCStringLen(ctx, &sn, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return 0;
    wn = strlen(want);
    switch (op) {
    case AO_EQ:       r = sn == wn && memcmp(s, want, wn) == 0; break;
    case AO_PREFIX:   r = wn && sn >= wn && memcmp(s, want, wn) == 0; break;
    case AO_SUFFIX:   r = wn && sn >= wn && memcmp(s + sn - wn, want, wn) == 0; break;
    case AO_CONTAINS: r = wn && sn >= wn && strstr(s, want) != NULL; break;
    default: {        /* AO_WORD: one space-separated token, which is how
                         class-like attributes are actually matched */
        size_t i = 0;
        while (i < sn && !r) {
            size_t st;
            while (i < sn && ht_space((unsigned char)s[i])) i++;
            st = i;
            while (i < sn && !ht_space((unsigned char)s[i])) i++;
            r = (i - st) == wn && memcmp(s + st, want, wn) == 0;
        }
        break;
    }
    }
    JS_FreeCString(ctx, s);
    return r;
}

static int sel_has_class(JSContext *ctx, JSValueConst el, const char *want)
{
    return sel_attr_eq(ctx, el, "class", want, AO_WORD);
}

static int sel_name_is(JSContext *ctx, JSValueConst el, const char *want)
{
    JSValue nv = JS_GetPropertyStr(ctx, el, "name");
    const char *s;
    size_t n;
    int r;

    if (!JS_IsString(nv)) { JS_FreeValue(ctx, nv); return 0; }
    s = JS_ToCStringLen(ctx, &n, nv);
    JS_FreeValue(ctx, nv);
    if (!s)
        return 0;
    r = ht_ieq(s, n, want);
    JS_FreeCString(ctx, s);
    return r;
}

/* Does this element match one compound (all of its parts)? */
static int sel_unit_match(sel_ctx_t *c, const sel_t *sel, const sel_unit_t *u,
                          int depth)
{
    uint32_t k;
    JSValueConst el = c->el[depth];

    for (k = 0; k < u->n; k++) {
        const sel_part_t *p = &sel->parts[u->first + k];
        int ok;
        switch (p->kind) {
        case SP_TAG:   ok = sel_name_is(c->ctx, el, p->a); break;
        case SP_ID:    ok = sel_attr_eq(c->ctx, el, "id", p->a, AO_EQ); break;
        case SP_CLASS: ok = sel_has_class(c->ctx, el, p->a); break;
        case SP_ATTR:  ok = sel_attr_eq(c->ctx, el, p->a, p->b ? p->b : "", p->op); break;
        case SP_FIRST: ok = c->idx[depth] == 0; break;
        case SP_ANY:   ok = 1; break;
        default:       ok = c->last[depth]; break;      /* SP_LAST */
        }
        if (!ok)
            return 0;
    }
    return 1;
}

/* Walk the compiled units right to left over the ancestor chain. */
static int sel_chain_match(sel_ctx_t *c, const sel_t *sel, uint32_t gstart,
                           uint32_t gend, int depth)
{
    int32_t d = depth;
    uint32_t u = gend;

    if (!sel_unit_match(c, sel, &sel->units[gend - 1], d))
        return 0;
    for (u = gend - 1; u > gstart; u--) {
        const sel_unit_t *prev = &sel->units[u - 1];
        uint8_t comb = sel->units[u].comb;
        if (comb == CB_CHILD) {
            d--;
            if (d < 0 || !sel_unit_match(c, sel, prev, d))
                return 0;
        } else {
            int found = 0;
            for (d--; d >= 0; d--)
                if (sel_unit_match(c, sel, prev, d)) { found = 1; break; }
            if (!found)
                return 0;
        }
    }
    return 1;
}

static int sel_matches_here(sel_ctx_t *c, const sel_t *sel)
{
    uint32_t g;

    for (g = 0; g < sel->ngroup; g++) {
        uint32_t start = sel->groups[g];
        uint32_t end = g + 1 < sel->ngroup ? sel->groups[g + 1] : sel->nunit;
        if (end > start && sel_chain_match(c, sel, start, end, c->depth))
            return 1;
    }
    return 0;
}

/* magic 0 = all, 1 = first */
typedef struct { JSValue out; uint32_t n; int want_first; int done; } sel_sink_t;

static int sel_visit(sel_ctx_t *c, const sel_t *sel, JSValueConst kids,
                     sel_sink_t *sink)
{
    JSValue lv = JS_GetPropertyStr(c->ctx, kids, "length");
    int64_t len = 0, i;
    int rc = 0;

    if (JS_IsException(lv) || JS_ToInt64(c->ctx, &len, lv) < 0) {
        JS_FreeValue(c->ctx, lv);
        return -1;
    }
    JS_FreeValue(c->ctx, lv);
    for (i = 0; i < len && rc == 0 && !sink->done; i++) {
        JSValue el = JS_GetPropertyUint32(c->ctx, kids, (uint32_t)i), sub;
        if (JS_IsException(el))
            return -1;
        if (!JS_IsObject(el)) { JS_FreeValue(c->ctx, el); continue; }
        if (c->depth + 1 >= HT_MAX_DEPTH) { JS_FreeValue(c->ctx, el); continue; }
        c->depth++;
        c->el[c->depth] = el;
        c->idx[c->depth] = (int32_t)i;
        c->last[c->depth] = (uint8_t)(i + 1 == len);
        if (sel_matches_here(c, sel)) {
            if (sink->want_first) {
                sink->out = JS_DupValue(c->ctx, el);
                sink->done = 1;
            } else if (JS_DefinePropertyValueUint32(c->ctx, sink->out, sink->n++,
                                                    JS_DupValue(c->ctx, el),
                                                    JS_PROP_C_W_E) < 0) {
                rc = -1;
            }
        }
        sub = JS_GetPropertyStr(c->ctx, el, "children");
        if (rc == 0 && !sink->done && JS_IsArray(c->ctx, sub) == 1)
            rc = sel_visit(c, sel, sub, sink);
        JS_FreeValue(c->ctx, sub);
        c->depth--;
        JS_FreeValue(c->ctx, el);
    }
    return rc;
}

/* --------------------------------------------------------- the Selector */

static JSClassID dyn_sel_class_id;

static void dyn_sel_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    sel_free((sel_t *)JS_GetOpaque(val, dyn_sel_class_id));
}

static const JSClassDef dyn_sel_class = {
    "Selector", .finalizer = dyn_sel_finalizer,
};

static JSValue dyn_sel_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    char err[160] = { 0 };
    sel_t *sel;

    (void)new_target;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "new Selector(text): text must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > HT_MAX_SEL) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "new Selector: exceeds %u bytes", HT_MAX_SEL);
    }
    sel = sel_compile(s, n, err, sizeof err);
    JS_FreeCString(ctx, s);
    if (!sel)
        return JS_ThrowSyntaxError(ctx, "new Selector: %s",
                                   err[0] ? err : "out of memory");
    return dyn_plain_wrap(ctx, dyn_sel_class_id, sel, sel_free_v);
}

/* magic 0 = all, 1 = first, 2 = matches */
static JSValue dyn_sel_query(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    sel_t *sel = (sel_t *)dyn_plain_get(ctx, this_val, dyn_sel_class_id);
    sel_ctx_t c;
    sel_sink_t sink;
    JSValue kids;
    int rc;

    if (!sel)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "a document or node is required");
    memset(&c, 0, sizeof c);
    c.ctx = ctx;
    c.depth = 0;
    if (magic == 2) {
        /* `matches` tests ONE node with no ancestors in scope, so a descendant
           combinator in the selector cannot be satisfied -- say so rather than
           returning a quiet false. */
        if (sel->nunit > sel->ngroup)
            return JS_ThrowTypeError(ctx, "Selector.matches: this selector has a "
                "combinator, and matches() sees one node with no ancestors");
        if (!JS_IsObject(argv[0]))
            return JS_NewBool(ctx, 0);
        c.depth = 0;
        c.el[0] = argv[0];
        c.idx[0] = 0;
        c.last[0] = 1;
        return JS_NewBool(ctx, sel_matches_here(&c, sel));
    }
    memset(&sink, 0, sizeof sink);
    sink.want_first = magic == 1;
    sink.out = magic == 1 ? JS_NULL : JS_NewArray(ctx);
    if (magic == 0 && JS_IsException(sink.out))
        return sink.out;
    if (JS_IsArray(ctx, argv[0]) == 1) {
        kids = JS_DupValue(ctx, argv[0]);
    } else {
        kids = JS_NewArray(ctx);
        if (!JS_IsException(kids))
            JS_DefinePropertyValueUint32(ctx, kids, 0, JS_DupValue(ctx, argv[0]),
                                         JS_PROP_C_W_E);
    }
    if (JS_IsException(kids)) {
        JS_FreeValue(ctx, sink.out);
        return kids;
    }
    c.depth = -1;                       /* the frontier is the caller's list */
    rc = sel_visit(&c, sel, kids, &sink);
    JS_FreeValue(ctx, kids);
    if (rc < 0) {
        JS_FreeValue(ctx, sink.out);
        return JS_EXCEPTION;
    }
    return sink.out;
}

static const JSCFunctionListEntry dyn_sel_proto[] = {
    JS_CFUNC_MAGIC_DEF("all", 1, dyn_sel_query, 0),
    JS_CFUNC_MAGIC_DEF("first", 1, dyn_sel_query, 1),
    JS_CFUNC_MAGIC_DEF("matches", 1, dyn_sel_query, 2),
};

/* -------------------------------------------------------- the Sanitizer */

typedef struct {
    char   *tag;
    char  **attrs;
    uint32_t nattr;
} san_rule_t;

typedef struct {
    san_rule_t *rules;
    char      **protos;                 /* "tag.attr=scheme" triples, flattened */
    uint32_t    n;
    uint32_t    nproto;
} san_t;

_Static_assert(sizeof(san_t) == 2 * sizeof(void *) + 8,
               "san_t regained padding: keep the two counts adjacent");

static void san_free(san_t *s)
{
    uint32_t i, k;
    if (!s)
        return;
    for (i = 0; i < s->n; i++) {
        for (k = 0; k < s->rules[i].nattr; k++)
            free(s->rules[i].attrs[k]);
        free(s->rules[i].attrs);
        free(s->rules[i].tag);
    }
    for (i = 0; i < s->nproto; i++)
        free(s->protos[i]);
    free(s->protos);
    free(s->rules);
    free(s);
}

static void san_free_v(void *p) { san_free((san_t *)p); }

static const san_rule_t *san_find(const san_t *s, const char *tag, size_t n)
{
    uint32_t i;
    for (i = 0; i < s->n; i++)
        if (ht_ieq(tag, n, s->rules[i].tag))
            return &s->rules[i];
    return NULL;
}

static int san_attr_ok(const san_rule_t *r, const char *a, size_t n)
{
    uint32_t k;
    for (k = 0; k < r->nattr; k++)
        if (ht_ieq(a, n, r->attrs[k]))
            return 1;
    return 0;
}

/* The attributes a browser resolves as a URL against the document's base. A
   value on one of these is a navigation or resource sink, so an ALLOW-list
   that names one WITHOUT an explicit `protocols` rule must still reject
   `javascript:` & co: there is no safe default that lets them through.
   `srcset` holds candidate URLs, `style` is a sink for css url() tokens. */
static int san_is_url_attr(const char *a, size_t n)
{
    static const char *const URL_ATTRS[] = {
        "href", "src", "action", "formaction", "xlink:href", "poster", "cite",
        "data", "srcset", "background", "ping", "style",
    };
    return ht_in(URL_ATTRS, countof(URL_ATTRS), a, n);
}

/* The schemes a URL attribute may carry when the allow-list names it without
   an explicit `protocols` rule. These resolve against the base URL; the rest
   (`javascript:`, `vbscript:`, `data:`, `file:`, `blob:`, and anything
   unknown) are omitted rather than emitted verbatim. */
static int san_safe_scheme(const char *s)
{
    static const char *const SAFE[] = { "http", "https", "mailto", "ftp" };
    uint32_t i;
    for (i = 0; i < countof(SAFE); i++)
        if (strcmp(s, SAFE[i]) == 0)
            return 1;
    return 0;
}

/* Split the scheme off a URL value. Returns 1 when a ':' scheme was found
   (copied lower-cased into `out`, a `cap`-byte NUL-terminated buffer), and 0
   when the value is relative -- a '/', '?' or '#' precedes any ':', or the
   string holds no scheme at all. Leading space/control bytes and control
   bytes inside the scheme are skipped: `java\tscript:` is the oldest trick in
   the book. A scheme too long to fit `out` still counts as a scheme (return 1)
   so the caller treats it as non-relative; the truncated copy cannot equal a
   real scheme name, so it is refused. */
static int san_scheme_of(const char *val, size_t vn, char *out, size_t cap)
{
    size_t k = 0, sn = 0;
    while (k < vn && (unsigned char)val[k] <= 0x20)
        k++;                                        /* leading space/control */
    while (k < vn) {
        unsigned char c = (unsigned char)val[k];
        if (c == ':')
            break;
        if (c == '/' || c == '?' || c == '#')
            return 0;                               /* relative: no scheme */
        if (c <= 0x20) { k++; continue; }           /* browsers ignore these */
        if (sn + 1 < cap)
            out[sn++] = (char)ht_lower(c);          /* else: too long, see above */
        k++;
    }
    if (k >= vn)
        return 0;                                   /* no colon at all */
    out[sn] = 0;
    return 1;
}

/* A URL is allowed only when its scheme is on the list for this tag.attr. An
   explicit `protocols` rule is an override; when none exists but the attribute
   is a URL sink, a fixed safe set is applied instead of the old fail-open
   `return 1`. Relative values (no scheme) pass either way. */
static int san_proto_ok(const san_t *s, const char *tag, size_t tn,
                        const char *attr, size_t an, const char *val, size_t vn)
{
    char key[128];
    uint32_t i;
    size_t j = 0, k;
    int have_rule = 0;
    char scheme[32];

    if ((int)tn + (int)an + 2 > (int)sizeof key)
        return 0;
    for (k = 0; k < tn; k++) key[j++] = (char)ht_lower((unsigned char)tag[k]);
    key[j++] = '.';
    for (k = 0; k < an; k++) key[j++] = (char)ht_lower((unsigned char)attr[k]);
    key[j] = 0;
    for (i = 0; i < s->nproto; i += 2)
        if (strcmp(s->protos[i], key) == 0) { have_rule = 1; break; }

    /* A non-URL attribute with no rule is free (title, alt, ...). A URL
       attribute with no rule falls through to the safe-scheme check. */
    if (!have_rule && !san_is_url_attr(attr, an))
        return 1;

    if (!san_scheme_of(val, vn, scheme, sizeof scheme))
        return 1;                       /* relative URL: no scheme to check */

    if (have_rule) {
        for (i = 0; i < s->nproto; i += 2)
            if (strcmp(s->protos[i], key) == 0 && strcmp(s->protos[i + 1], scheme) == 0)
                return 1;
        return 0;
    }
    return san_safe_scheme(scheme);     /* no rule: allow only safe schemes */
}

static JSClassID dyn_san_class_id;

static void dyn_san_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    san_free((san_t *)JS_GetOpaque(val, dyn_san_class_id));
}

static const JSClassDef dyn_san_class = {
    "Sanitizer", .finalizer = dyn_san_finalizer,
};

/* A lower-cased copy, because tag and attribute names are matched that way. */
static char *san_lower_dup(const char *s)
{
    size_t i, n = strlen(s);
    char *d = sel_dup(s, n);
    if (d)
        for (i = 0; i < n; i++)
            d[i] = (char)ht_lower((unsigned char)d[i]);
    return d;
}

/* The attribute names allowed on one tag. */
static int san_read_attrs(JSContext *ctx, JSValueConst list, san_rule_t *r)
{
    JSValue lv;
    int64_t an = 0, k;

    if (JS_IsArray(ctx, list) == 1) {
        lv = JS_GetPropertyStr(ctx, list, "length");
        if (!JS_IsException(lv))
            JS_ToInt64(ctx, &an, lv);
        JS_FreeValue(ctx, lv);
    }
    r->attrs = (char **)calloc(an ? (size_t)an : 1, sizeof(char *));
    if (!r->attrs)
        return -1;
    for (k = 0; k < an; k++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, (uint32_t)k);
        const char *as = JS_IsException(e) ? NULL : JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!as)
            return -1;
        r->attrs[r->nattr] = san_lower_dup(as);
        JS_FreeCString(ctx, as);
        if (!r->attrs[r->nattr])
            return -1;
        r->nattr++;
    }
    return 0;
}

static int san_read_allow(JSContext *ctx, JSValueConst o, san_t *s)
{
    JSValue allow = JS_GetPropertyStr(ctx, o, "allow");
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    int rc = 0;

    if (!JS_IsObject(allow)) {
        JS_FreeValue(ctx, allow);
        JS_ThrowTypeError(ctx, "new Sanitizer: `allow` maps a tag to its "
                               "permitted attributes");
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, allow,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, allow);
        return -1;
    }
    s->rules = (san_rule_t *)calloc(len ? len : 1, sizeof *s->rules);
    if (!s->rules)
        rc = -1;
    for (i = 0; i < len && rc == 0; i++) {
        JSValue list = JS_GetProperty(ctx, allow, tab[i].atom);
        const char *tn = JS_AtomToCString(ctx, tab[i].atom);
        if (!tn || JS_IsException(list)) {
            rc = -1;
        } else {
            s->rules[s->n].tag = san_lower_dup(tn);
            rc = s->rules[s->n].tag ? san_read_attrs(ctx, list, &s->rules[s->n])
                                    : -1;
            s->n++;
        }
        if (tn) JS_FreeCString(ctx, tn);
        JS_FreeValue(ctx, list);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    JS_FreeValue(ctx, allow);
    return rc;
}

static int san_read_protocols(JSContext *ctx, JSValueConst o, san_t *s)
{
    JSValue pr = JS_GetPropertyStr(ctx, o, "protocols");
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i, cap;
    int rc = 0;

    if (!JS_IsObject(pr)) {
        JS_FreeValue(ctx, pr);
        return 0;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, pr,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, pr);
        return -1;
    }
    cap = len * 8 + 2;
    s->protos = (char **)calloc(cap, sizeof(char *));
    if (!s->protos) { rc = -1; goto done; }
    for (i = 0; i < len && rc == 0; i++) {
        JSValue list = JS_GetProperty(ctx, pr, tab[i].atom), lv;
        const char *key = JS_AtomToCString(ctx, tab[i].atom);
        int64_t an = 0, k;
        if (!key || JS_IsException(list)) { rc = -1; JS_FreeValue(ctx, list); break; }
        if (JS_IsArray(ctx, list) == 1) {
            lv = JS_GetPropertyStr(ctx, list, "length");
            if (!JS_IsException(lv))
                JS_ToInt64(ctx, &an, lv);
            JS_FreeValue(ctx, lv);
        }
        size_t klen = strlen(key);   /* invariant across the protocol list */
        for (k = 0; k < an && rc == 0; k++) {
            JSValue e = JS_GetPropertyUint32(ctx, list, (uint32_t)k);
            const char *ps = JS_IsException(e) ? NULL : JS_ToCString(ctx, e);
            JS_FreeValue(ctx, e);
            if (!ps || s->nproto + 2 > cap) { if (ps) JS_FreeCString(ctx, ps); rc = -1; break; }
            s->protos[s->nproto] = sel_dup(key, klen);
            s->protos[s->nproto + 1] = sel_dup(ps, strlen(ps));
            JS_FreeCString(ctx, ps);
            if (!s->protos[s->nproto] || !s->protos[s->nproto + 1]) { rc = -1; break; }
            s->nproto += 2;
        }
        JS_FreeCString(ctx, key);
        JS_FreeValue(ctx, list);
    }
done:
    JS_FreePropertyEnum(ctx, tab, len);
    JS_FreeValue(ctx, pr);
    return rc;
}

static JSValue dyn_san_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    san_t *s;

    (void)new_target;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Sanitizer({allow, protocols}): an allow-list is required -- "
            "there is no default policy, because a default is a policy nobody read");
    s = (san_t *)calloc(1, sizeof *s);
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    if (san_read_allow(ctx, argv[0], s) < 0 || san_read_protocols(ctx, argv[0], s) < 0) {
        san_free(s);
        return JS_EXCEPTION;
    }
    return dyn_plain_wrap(ctx, dyn_san_class_id, s, san_free_v);
}

typedef struct { JSContext *ctx; const san_t *s; hb_t b; int depth; } san_w_t;

static int san_node(san_w_t *w, JSValueConst v);

static int san_element(san_w_t *w, JSValueConst v, const char *nm, size_t nml)
{
    const san_rule_t *r = san_find(w->s, nm, nml);
    JSValue kids;
    int rc = 0;

    if (r) {
        JSValue attrs = JS_GetPropertyStr(w->ctx, v, "attrs");
        JSPropertyEnum *tab = NULL;
        uint32_t len = 0, k;
        hb_put(&w->b, '<');
        hb_write(&w->b, nm, nml);
        if (JS_IsObject(attrs)
            && JS_GetOwnPropertyNames(w->ctx, &tab, &len, attrs,
                                      JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (k = 0; k < len; k++) {
                JSValue av = JS_GetProperty(w->ctx, attrs, tab[k].atom);
                const char *an, *as;
                size_t anl, asl;
                an = JS_AtomToCStringLen(w->ctx, &anl, tab[k].atom);
                as = JS_IsException(av) ? NULL : JS_ToCStringLen(w->ctx, &asl, av);
                JS_FreeValue(w->ctx, av);
                if (an && as && san_attr_ok(r, an, anl)
                    && san_proto_ok(w->s, nm, nml, an, anl, as, asl)) {
                    hb_put(&w->b, ' ');
                    hb_write(&w->b, an, anl);
                    hb_puts(&w->b, "=\"");
                    ht_escape(&w->b, as, asl, 1);
                    hb_put(&w->b, '"');
                }
                if (an) JS_FreeCString(w->ctx, an);
                if (as) JS_FreeCString(w->ctx, as);
            }
            JS_FreePropertyEnum(w->ctx, tab, len);
        }
        JS_FreeValue(w->ctx, attrs);
        hb_put(&w->b, '>');
    }
    /* A DISALLOWED element loses its tag but keeps its children: dropping the
       subtree silently deletes content, and the tag was the dangerous part. A
       raw-text element is the exception -- its content IS script. */
    if (!HT_IS_VOID(nm, nml) && !(!r && HT_IS_RAW(nm, nml))) {
        kids = JS_GetPropertyStr(w->ctx, v, "children");
        if (JS_IsArray(w->ctx, kids) == 1) {
            JSValue lv = JS_GetPropertyStr(w->ctx, kids, "length");
            int64_t len = 0, i;
            if (!JS_IsException(lv))
                JS_ToInt64(w->ctx, &len, lv);
            JS_FreeValue(w->ctx, lv);
            w->depth++;
            for (i = 0; i < len && rc == 0; i++) {
                JSValue c = JS_GetPropertyUint32(w->ctx, kids, (uint32_t)i);
                rc = JS_IsException(c) ? -1 : san_node(w, c);
                JS_FreeValue(w->ctx, c);
            }
            w->depth--;
        }
        JS_FreeValue(w->ctx, kids);
    }
    if (r && !HT_IS_VOID(nm, nml)) {
        hb_puts(&w->b, "</");
        hb_write(&w->b, nm, nml);
        hb_put(&w->b, '>');
    }
    return rc;
}

static int san_node(san_w_t *w, JSValueConst v)
{
    JSValue nv;
    const char *nm;
    size_t nml;
    int rc;

    if (w->depth >= HT_MAX_DEPTH)
        return 0;
    if (!JS_IsObject(v)) {
        const char *s;
        size_t n;
        if (!JS_IsString(v))
            return 0;
        s = JS_ToCStringLen(w->ctx, &n, v);
        if (!s)
            return -1;
        ht_escape(&w->b, s, n, 0);
        JS_FreeCString(w->ctx, s);
        return 0;
    }
    nv = JS_GetPropertyStr(w->ctx, v, "name");
    nm = JS_IsString(nv) ? JS_ToCStringLen(w->ctx, &nml, nv) : NULL;
    JS_FreeValue(w->ctx, nv);
    if (!nm)
        return 0;
    rc = san_element(w, v, nm, nml);
    JS_FreeCString(w->ctx, nm);
    return rc;
}

static JSValue dyn_san_clean(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    san_t *s = (san_t *)dyn_plain_get(ctx, this_val, dyn_san_class_id);
    san_w_t w;
    JSValue tree, lv, out;
    const char *src;
    size_t n;
    int64_t len = 0, i;
    int rc = 0;

    if (!s)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Sanitizer.clean(html): html must be a string");
    src = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    /* the sanitizer is the MOST hostile entry point into the parser; it gets
       the same 64 MiB cap HTMLParse enforces, not a bypass of it */
    if (n > HT_MAX_INPUT) {
        JS_FreeCString(ctx, src);
        return JS_ThrowRangeError(ctx,
                                  "Sanitizer.clean: input exceeds %u bytes",
                                  (unsigned)HT_MAX_INPUT);
    }
    tree = ht_parse_into(ctx, src, n);
    JS_FreeCString(ctx, src);
    if (JS_IsException(tree))
        return tree;
    memset(&w, 0, sizeof w);
    w.ctx = ctx;
    w.s = s;
    hb_init(&w.b);
    lv = JS_GetPropertyStr(ctx, tree, "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0)
        rc = -1;
    JS_FreeValue(ctx, lv);
    for (i = 0; i < len && rc == 0; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, tree, (uint32_t)i);
        rc = JS_IsException(c) ? -1 : san_node(&w, c);
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, tree);
    if (rc < 0 || w.b.oom) {
        if (w.b.oom) JS_ThrowOutOfMemory(ctx);
        hb_free(&w.b);
        return JS_EXCEPTION;
    }
    out = JS_NewStringLen(ctx, (const char *)w.b.p, w.b.n);
    hb_free(&w.b);
    return out;
}

static const JSCFunctionListEntry dyn_san_proto[] = {
    JS_CFUNC_DEF("clean", 1, dyn_san_clean),
};

/* Markdown renders THROUGH this module's escaper, which is the point. */
#include "dyna-markdown.inc.c"

/* And a template, which renders through the same escaper. */
#include "dyna-template.inc.c"

/* ------------------------------------------------------------ registration */

/* Tag names come from the parser lowercased, but a node can also be built by
   hand in JS, so compare without case. */
static int ht_tag_is(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        if (ca != *b) return 0;
    }
    return *a == 0 && *b == 0;
}

/* ---- HTMLText: the text of a subtree, which every scraper needs first ----
 * Walks children and concatenates string nodes. <script>/<style> contents are
 * NOT text and are skipped -- returning JS source as page text is the classic
 * scraping bug. Same depth bound as the serializer. */
static int ht_text_node(JSContext *ctx, hb_t *b, JSValueConst node, int depth)
{
    JSValue kids, nm;
    const char *tag;
    int64_t len = 0, i;
    int rc = 0, skip = 0;

    if (depth > HT_MAX_DEPTH) {
        JS_ThrowRangeError(ctx, "HTMLText: nesting exceeds %d", HT_MAX_DEPTH);
        return -1;
    }
    if (JS_IsString(node)) {
        size_t n;
        const char *t = JS_ToCStringLen(ctx, &n, node);
        if (!t) return -1;
        hb_write(b, t, n);
        JS_FreeCString(ctx, t);
        return 0;
    }
    if (!JS_IsObject(node))
        return 0;                      /* comments/doctypes carry no text */
    nm = JS_GetPropertyStr(ctx, node, "name");
    if (JS_IsException(nm)) return -1;
    tag = JS_IsString(nm) ? JS_ToCString(ctx, nm) : NULL;
    if (tag) {
        /* template/noscript hold text that is NOT page text: their content
           leaked into HTMLText output the same way script/style once did */
        skip = ht_tag_is(tag, "script") || ht_tag_is(tag, "style")
            || ht_tag_is(tag, "template") || ht_tag_is(tag, "noscript");
        JS_FreeCString(ctx, tag);
    }
    JS_FreeValue(ctx, nm);
    if (skip)
        return 0;
    kids = JS_GetPropertyStr(ctx, node, "children");
    if (JS_IsException(kids)) return -1;
    if (JS_IsArray(ctx, kids) == 1) {
        JSValue lv = JS_GetPropertyStr(ctx, kids, "length");
        rc = (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) ? -1 : 0;
        JS_FreeValue(ctx, lv);
        for (i = 0; i < len && rc == 0; i++) {
            JSValue c = JS_GetPropertyUint32(ctx, kids, (uint32_t)i);
            rc = JS_IsException(c) ? -1 : ht_text_node(ctx, b, c, depth + 1);
            JS_FreeValue(ctx, c);
        }
    }
    JS_FreeValue(ctx, kids);
    return rc;
}

static JSValue dyn_html_text(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    hb_t b;
    JSValue out;
    int rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "HTMLText(node): a node is required");
    hb_init(&b);
    if (JS_IsArray(ctx, argv[0]) == 1) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        int64_t len = 0, i;
        rc = (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) ? -1 : 0;
        JS_FreeValue(ctx, lv);
        for (i = 0; i < len && rc == 0; i++) {
            JSValue c = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            rc = JS_IsException(c) ? -1 : ht_text_node(ctx, &b, c, 0);
            JS_FreeValue(ctx, c);
        }
    } else {
        rc = ht_text_node(ctx, &b, argv[0], 0);
    }
    if (rc < 0) { hb_free(&b); return JS_EXCEPTION; }
    out = JS_NewStringLen(ctx, b.p ? (const char *)b.p : "", b.n);
    hb_free(&b);
    return out;
}

static const JSCFunctionListEntry dyn_html_funcs[] = {
    JS_CFUNC_DEF("HTMLParse", 1, dyn_html_parse),
    JS_CFUNC_DEF("HTMLStringify", 1, dyn_html_stringify),
    JS_CFUNC_DEF("HTMLText", 1, dyn_html_text),
    JS_CFUNC_DEF("MarkdownToHTML", 1, dyn_md_render),
};

static int dyn_html_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_sel_class_id, &dyn_sel_class,
                                 dyn_sel_proto, countof(dyn_sel_proto),
                                 dyn_sel_ctor, "Selector") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_san_class_id, &dyn_san_class,
                                 dyn_san_proto, countof(dyn_san_proto),
                                 dyn_san_ctor, "Sanitizer") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_tpl_class_id, &dyn_tpl_class,
                                 dyn_tpl_proto, countof(dyn_tpl_proto),
                                 dyn_tpl_ctor, "Template") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_html_funcs, countof(dyn_html_funcs));
}

int js_nat_init_html(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:html", dyn_html_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Selector");
    JS_AddModuleExport(ctx, m, "Sanitizer");
    JS_AddModuleExport(ctx, m, "Template");
    return JS_AddModuleExportList(ctx, m, dyn_html_funcs, countof(dyn_html_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_HTML */
