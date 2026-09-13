/* HTTP message codecs for dyna:net (design 15): ContentType, Accepts, Range,
   ETag and Cookies. Included by dyna-http.c so they register into the one
   module that already owns the HTTP surface. */

#include "core/dyn-sb.h"

/* RFC 9110 tchar. ONE table, shared by every parser and serializer here: a
   serializer that quotes different characters than the parser accepts is a
   header-smuggling bug, not a cosmetic difference. */
static const uint8_t DYN_TCHAR[256] = {
    ['!']=1,['#']=1,['$']=1,['%']=1,['&']=1,['\'']=1,['*']=1,['+']=1,['-']=1,
    ['.']=1,['^']=1,['_']=1,['`']=1,['|']=1,['~']=1,
    ['0']=1,['1']=1,['2']=1,['3']=1,['4']=1,['5']=1,['6']=1,['7']=1,['8']=1,['9']=1,
    ['a']=1,['b']=1,['c']=1,['d']=1,['e']=1,['f']=1,['g']=1,['h']=1,['i']=1,
    ['j']=1,['k']=1,['l']=1,['m']=1,['n']=1,['o']=1,['p']=1,['q']=1,['r']=1,
    ['s']=1,['t']=1,['u']=1,['v']=1,['w']=1,['x']=1,['y']=1,['z']=1,
    ['A']=1,['B']=1,['C']=1,['D']=1,['E']=1,['F']=1,['G']=1,['H']=1,['I']=1,
    ['J']=1,['K']=1,['L']=1,['M']=1,['N']=1,['O']=1,['P']=1,['Q']=1,['R']=1,
    ['S']=1,['T']=1,['U']=1,['V']=1,['W']=1,['X']=1,['Y']=1,['Z']=1,
};

/* A local growable buffer over the shared core (core/dyn-sb.h): the struct
   name, the sticky-oom void-return convention and the seed (64) are this
   fragment's. */
typedef struct { char *p; size_t n, cap; int oom; } dyn_sb_t;

static void dyn_sb_init(dyn_sb_t *b) { b->p = NULL; b->n = 0; b->cap = 0; b->oom = 0; }
static void dyn_sb_free(dyn_sb_t *b) { free(b->p); b->p = NULL; b->n = 0; b->cap = 0; }

static void dyn_sb_put(dyn_sb_t *b, const char *s, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap
        && !dyn_sb_reserve((void **)&b->p, &b->cap, b->n + n, 64)) {
        b->oom = 1;
        return;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void dyn_sb_putc(dyn_sb_t *b, char c) { dyn_sb_put(b, &c, 1); }
static void dyn_sb_puts(dyn_sb_t *b, const char *s) { dyn_sb_put(b, s, strlen(s)); }

static int dyn_hm_ows(char c) { return c == ' ' || c == '\t'; }

static void dyn_hm_trim(const char *s, size_t *b, size_t *e)
{
    while (*b < *e && dyn_hm_ows(s[*b])) (*b)++;
    while (*e > *b && dyn_hm_ows(s[*e - 1])) (*e)--;
}

static void dyn_hm_lower(char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] >= 'A' && s[i] <= 'Z')
            s[i] = (char)(s[i] - 'A' + 'a');
}

/* Advance past one comma-separated element, respecting quoted-strings so a
   comma INSIDE a quoted parameter does not split the list. */
static size_t dyn_hm_next_elem(const char *s, size_t n, size_t i, size_t *end)
{
    int q = 0;
    size_t b = i;
    while (i < n) {
        char c = s[i];
        if (q) {
            if (c == '\\' && i + 1 < n) i++;
            else if (c == '"') q = 0;
        } else if (c == '"') {
            q = 1;
        } else if (c == ',') {
            break;
        }
        i++;
    }
    *end = i;
    (void)b;
    return (i < n) ? i + 1 : i;
}

/* Define one lowercased parameter name with its value. Parameter names are
   case-insensitive; values are not. */
static void dyn_hm_put_param(JSContext *ctx, JSValueConst obj, const char *k,
                             size_t kn, const char *v, size_t vn)
{
    /* Parameter names are 4-12 bytes in practice; the stack buffer removes an
       allocation per parameter of every header parsed. */
    char stack[64], *buf = stack;
    if (kn == 0)
        return;
    if (kn + 1 > sizeof stack) {
        buf = (char *)malloc(kn + 1);
        if (!buf)
            return;
    }
    memcpy(buf, k, kn);
    buf[kn] = 0;
    dyn_hm_lower(buf, kn);
    JS_DefinePropertyValueStr(ctx, obj, buf, JS_NewStringLen(ctx, v, vn),
                              JS_PROP_C_W_E);
    if (buf != stack)
        free(buf);
}

/* Parse `;name=value` parameters into `obj`, unquoting quoted-strings. */
static int dyn_hm_params(JSContext *ctx, JSValueConst obj, const char *s,
                         size_t i, size_t n)
{
    while (i < n) {
        size_t nb, ne, vb, ve;
        while (i < n && (s[i] == ';' || dyn_hm_ows(s[i]))) i++;
        if (i >= n) break;
        nb = i;
        while (i < n && s[i] != '=' && s[i] != ';') i++;
        ne = i;
        dyn_hm_trim(s, &nb, &ne);
        if (i < n && s[i] == '=') {
            i++;
            if (i < n && s[i] == '"') {          /* quoted-string value */
                dyn_sb_t v;
                dyn_sb_init(&v);
                i++;
                while (i < n && s[i] != '"') {
                    if (s[i] == '\\' && i + 1 < n) i++;   /* quoted-pair */
                    dyn_sb_putc(&v, s[i]);
                    i++;
                }
                if (i < n) i++;
                dyn_hm_put_param(ctx, obj, s + nb, ne - nb,
                                 v.p ? v.p : "", v.n);
                dyn_sb_free(&v);
                continue;
            }
            vb = i;
            while (i < n && s[i] != ';') i++;
            ve = i;
            dyn_hm_trim(s, &vb, &ve);
        } else {
            vb = ve = i;                          /* a bare parameter */
        }
        dyn_hm_put_param(ctx, obj, s + nb, ne - nb, s + vb, ve - vb);
    }
    return 0;
}

/* ContentTypeParse(header) -> { type, subtype, parameters } or null. */
static JSValue dyn_hm_ctype_parse(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *s;
    size_t n, i = 0, tb, te, sb, se;
    JSValue o, params;
    char *buf;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "ContentTypeParse(header): argument must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    while (i < n && dyn_hm_ows(s[i])) i++;
    tb = i;
    while (i < n && DYN_TCHAR[(unsigned char)s[i]]) i++;
    te = i;
    if (te == tb || i >= n || s[i] != '/') { JS_FreeCString(ctx, s); return JS_NULL; }
    i++;
    sb = i;
    while (i < n && DYN_TCHAR[(unsigned char)s[i]]) i++;
    se = i;
    if (se == sb) { JS_FreeCString(ctx, s); return JS_NULL; }

    o = JS_NewObject(ctx);
    params = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_IsException(params)) {
        JS_FreeValue(ctx, o); JS_FreeValue(ctx, params);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    buf = (char *)malloc((te - tb > se - sb ? te - tb : se - sb) + 1);
    if (!buf) {
        JS_FreeValue(ctx, o); JS_FreeValue(ctx, params);
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(buf, s + tb, te - tb); buf[te - tb] = 0;
    dyn_hm_lower(buf, te - tb);                   /* type is case-insensitive */
    JS_DefinePropertyValueStr(ctx, o, "type", JS_NewString(ctx, buf), JS_PROP_C_W_E);
    memcpy(buf, s + sb, se - sb); buf[se - sb] = 0;
    dyn_hm_lower(buf, se - sb);
    JS_DefinePropertyValueStr(ctx, o, "subtype", JS_NewString(ctx, buf), JS_PROP_C_W_E);
    free(buf);
    dyn_hm_params(ctx, params, s, i, n);
    JS_DefinePropertyValueStr(ctx, o, "parameters", params, JS_PROP_C_W_E);
    JS_FreeCString(ctx, s);
    return o;
}

/* A parameter value needs quoting iff it is not a bare token. Derived from the
   SAME table the parser accepts, so the two cannot drift apart. */
static int dyn_hm_needs_quote(const char *s, size_t n)
{
    size_t i;
    if (n == 0)
        return 1;
    for (i = 0; i < n; i++)
        if (!DYN_TCHAR[(unsigned char)s[i]])
            return 1;
    return 0;
}

static void dyn_hm_write_value(dyn_sb_t *b, const char *s, size_t n)
{
    size_t i;
    if (!dyn_hm_needs_quote(s, n)) {
        dyn_sb_put(b, s, n);
        return;
    }
    dyn_sb_putc(b, '"');
    for (i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\')
            dyn_sb_putc(b, '\\');
        dyn_sb_putc(b, s[i]);
    }
    dyn_sb_putc(b, '"');
}

/* ContentTypeFormat({type, subtype, parameters}) -> header string. */
static JSValue dyn_hm_ctype_format(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue tv, sv, pv, ret = JS_EXCEPTION;
    const char *t = NULL, *st = NULL;
    dyn_sb_t b;
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "ContentTypeFormat(obj): argument must be an object");
    dyn_sb_init(&b);
    tv = JS_GetPropertyStr(ctx, argv[0], "type");
    sv = JS_GetPropertyStr(ctx, argv[0], "subtype");
    pv = JS_GetPropertyStr(ctx, argv[0], "parameters");
    /* A missing property must be a refusal, not the string "undefined" --
       JS_ToCString of undefined succeeds and yields exactly that. */
    t = JS_IsString(tv) ? JS_ToCString(ctx, tv) : NULL;
    st = JS_IsString(sv) ? JS_ToCString(ctx, sv) : NULL;
    JS_FreeValue(ctx, tv); JS_FreeValue(ctx, sv);
    if (!t || !st || !*t || !*st) {
        JS_ThrowTypeError(ctx, "ContentTypeFormat(obj): type and subtype are required");
        goto done;
    }
    dyn_sb_puts(&b, t);
    dyn_sb_putc(&b, '/');
    dyn_sb_puts(&b, st);
    if (JS_IsObject(pv)
        && JS_GetOwnPropertyNames(ctx, &tab, &len, pv,
                                  JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (i = 0; i < len; i++) {
            JSValue kv = JS_AtomToString(ctx, tab[i].atom);
            JSValue vv = JS_GetProperty(ctx, pv, tab[i].atom);
            const char *ks = JS_IsException(kv) ? NULL : JS_ToCString(ctx, kv);
            size_t vn = 0;
            const char *vs = JS_IsException(vv) ? NULL : JS_ToCStringLen(ctx, &vn, vv);
            JS_FreeValue(ctx, kv); JS_FreeValue(ctx, vv);
            if (ks && vs) {
                dyn_sb_puts(&b, "; ");
                dyn_sb_puts(&b, ks);
                dyn_sb_putc(&b, '=');
                dyn_hm_write_value(&b, vs, vn);
            }
            if (ks) JS_FreeCString(ctx, ks);
            if (vs) JS_FreeCString(ctx, vs);
        }
        JS_FreePropertyEnum(ctx, tab, len);
    }
    if (b.oom) { JS_ThrowOutOfMemory(ctx); goto done; }
    ret = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
 done:
    dyn_sb_free(&b);
    JS_FreeValue(ctx, pv);
    if (t) JS_FreeCString(ctx, t);
    if (st) JS_FreeCString(ctx, st);
    return ret;
}

/* ------------------------------------------------------------ negotiation */

/* Score one candidate against one media-range. -1 = no match; higher is a
   better match. Specificity ranks an exact type/subtype above a type wildcard
   above a full wildcard, and that ordering is applied before q. */
static double dyn_hm_media_score(const char *range, size_t rn,
                                 const char *cand, size_t cn, double q,
                                 int *spec)
{
    size_t rs = 0, cs = 0, i;
    for (i = 0; i < rn; i++) if (range[i] == '/') { rs = i; break; }
    for (i = 0; i < cn; i++) if (cand[i] == '/') { cs = i; break; }
    if (rn == 3 && memcmp(range, "*/*", 3) == 0) { *spec = 0; return q; }
    if (!rs || !cs)
        return -1;
    if (rs != cs || memcmp(range, cand, rs) != 0)
        return -1;                                 /* different type */
    if (rn - rs == 2 && range[rs + 1] == '*') { *spec = 1; return q; }
    if (rn - rs == cn - cs && memcmp(range + rs, cand + cs, rn - rs) == 0) {
        *spec = 2;
        return q;
    }
    return -1;
}

/* Read `;q=` off an element; defaults to 1 when absent or malformed. */
static double dyn_hm_qvalue(const char *s, size_t b, size_t e)
{
    size_t i = b;
    while (i + 1 < e) {
        if (s[i] == ';') {
            size_t j = i + 1;
            while (j < e && dyn_hm_ows(s[j])) j++;
            if (j + 1 < e && (s[j] == 'q' || s[j] == 'Q') && s[j + 1] == '=') {
                double v = 0, scale = 0.1;
                j += 2;
                if (j < e && s[j] >= '0' && s[j] <= '9') {
                    v = s[j] - '0';
                    j++;
                    if (j < e && s[j] == '.') {
                        j++;
                        while (j < e && s[j] >= '0' && s[j] <= '9') {
                            v += (s[j] - '0') * scale;
                            scale /= 10;
                            j++;
                        }
                    }
                }
                return (v > 1) ? 1 : v;
            }
        }
        i++;
    }
    return 1.0;
}

/* Negotiate(headerValue, candidates[]) -> the best candidate, or null.
   magic 0 = media types (type/subtype), 1 = plain tokens (language, encoding,
   charset), where a `-` prefix match also counts. */
static JSValue dyn_hm_negotiate(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    const char *h;
    size_t hn, i = 0;
    int64_t ncand = 0, k;
    double best_q = -1;
    int best_spec = -1;
    int64_t best = -1;
    /* Candidates converted ONCE, up front: the old loop re-coerced every
     * candidate for every header element -- O(elems x candidates) JS->C
     * string conversions of values that never change. */
    typedef struct { const char *s; size_t n; } hm_cand_t;
    hm_cand_t *cands = NULL;

    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx,
            "Negotiate(header, candidates): a string and an array are required");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_ToInt64(ctx, &ncand, lv)) { JS_FreeValue(ctx, lv); return JS_EXCEPTION; }
        JS_FreeValue(ctx, lv);
    }
    h = JS_ToCStringLen(ctx, &hn, argv[0]);
    if (!h)
        return JS_EXCEPTION;
    /* An absent or empty header accepts anything: the first candidate wins. */
    {
        size_t t0 = 0, t1 = hn;
        dyn_hm_trim(h, &t0, &t1);
        if (t0 == t1) {
            JS_FreeCString(ctx, h);
            return ncand > 0 ? JS_GetPropertyUint32(ctx, argv[1], 0) : JS_NULL;
        }
    }
    if (ncand > 0) {
        cands = (hm_cand_t *)malloc((size_t)ncand * sizeof(*cands));
        if (cands) {
            for (k = 0; k < ncand; k++) {
                JSValue cv = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)k);
                cands[k].s = JS_IsException(cv)
                             ? NULL : JS_ToCStringLen(ctx, &cands[k].n, cv);
                JS_FreeValue(ctx, cv);
            }
        }
    }
    while (i < hn) {
        size_t eb = i, ee;
        double q;
        i = dyn_hm_next_elem(h, hn, i, &ee);
        {
            size_t rb = eb, re = ee, semi;
            for (semi = rb; semi < re && h[semi] != ';'; semi++)
                ;
            q = dyn_hm_qvalue(h, rb, re);
            re = semi;
            dyn_hm_trim(h, &rb, &re);
            if (rb == re || q <= 0 || !cands)
                continue;                          /* q=0 is an explicit refusal */
            for (k = 0; k < ncand; k++) {
                const char *cs = cands[k].s;
                size_t cn = cands[k].n;
                int spec = 0;
                double sc = -1;

                if (!cs)
                    continue;
                if (magic == 0) {
                    sc = dyn_hm_media_score(h + rb, re - rb, cs, cn, q, &spec);
                } else if (re - rb == 1 && h[rb] == '*') {
                    sc = q; spec = 0;
                } else if (re - rb == cn
                           && strncasecmp(h + rb, cs, cn) == 0) {
                    sc = q; spec = 2;
                } else if (cn > re - rb && cs[re - rb] == '-'
                           && strncasecmp(h + rb, cs, re - rb) == 0) {
                    sc = q; spec = 1;              /* `en` matches `en-GB` */
                }
                if (sc > 0 && (sc > best_q || (sc == best_q && spec > best_spec))) {
                    best_q = sc; best_spec = spec; best = k;
                }
            }
        }
    }
    if (cands) {
        for (k = 0; k < ncand; k++)
            if (cands[k].s)
                JS_FreeCString(ctx, cands[k].s);
        free(cands);
    }
    JS_FreeCString(ctx, h);
    if (best < 0)
        return JS_NULL;
    return JS_GetPropertyUint32(ctx, argv[1], (uint32_t)best);
}

/* ----------------------------------------------------------------- Range */

/* One `first-last` element against `size`. 0 with [*start,*end] filled, or -1
   when the element is malformed or selects nothing. Bounds are INCLUSIVE and an
   end past the resource is clamped rather than rejected. */
static int dyn_hm_one_range(const char *h, size_t b, size_t e, int64_t size,
                            int64_t *start, int64_t *end)
{
    size_t d, p;
    int have_start = 0, have_end = 0;
    *start = -1; *end = -1;
    for (d = b; d < e && h[d] != '-'; d++)
        ;
    if (d >= e)
        return -1;                            /* no dash: not a range */
    if (d > b) {
        *start = 0; have_start = 1;
        for (p = b; p < d; p++) {
            if (h[p] < '0' || h[p] > '9') { have_start = 0; break; }
            *start = *start * 10 + (h[p] - '0');
            if (*start > size) *start = size;  /* clamp before it can overflow */
        }
    }
    if (d + 1 < e) {
        *end = 0; have_end = 1;
        for (p = d + 1; p < e; p++) {
            if (h[p] < '0' || h[p] > '9') { have_end = 0; break; }
            *end = *end * 10 + (h[p] - '0');
            if (*end > size) *end = size;
        }
    }
    if (!have_start && !have_end)
        return -1;
    if (!have_start) {                         /* suffix: the last `end` bytes */
        if (*end == 0)
            return -1;                         /* `-0` selects nothing */
        *start = (*end >= size) ? 0 : size - *end;
        *end = size - 1;
    } else if (!have_end || *end >= size) {
        *end = size - 1;
    }
    if (size == 0 || *start >= size || *start > *end)
        return -1;
    return 0;
}

/* RangeParse(header, size) -> [{start,end}] | "unsatisfiable" | null.
   Ranges are INCLUSIVE, and a suffix range (`-500`) counts from the end. */
static JSValue dyn_hm_range(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    const char *h;
    size_t hn, i = 0;
    int64_t size = 0;
    JSValue arr;
    uint32_t out = 0;
    int any_sat = 0;

    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "RangeParse(header, size): header must be a string");
    if (JS_ToInt64(ctx, &size, argv[1]))
        return JS_EXCEPTION;
    if (size < 0)
        return JS_ThrowRangeError(ctx, "RangeParse(header, size): size must be >= 0");
    h = JS_ToCStringLen(ctx, &hn, argv[0]);
    if (!h)
        return JS_EXCEPTION;
    if (hn < 6 || strncasecmp(h, "bytes=", 6) != 0) {
        JS_FreeCString(ctx, h);
        return JS_NULL;                            /* not a byte range: ignore */
    }
    i = 6;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) { JS_FreeCString(ctx, h); return JS_EXCEPTION; }
    while (i < hn) {
        size_t eb = i, ee;
        int64_t start, end;
        i = dyn_hm_next_elem(h, hn, i, &ee);
        dyn_hm_trim(h, &eb, &ee);
        if (dyn_hm_one_range(h, eb, ee, size, &start, &end) < 0)
            continue;
        any_sat = 1;
        {
            JSValue o = JS_NewObject(ctx);
            if (JS_IsException(o)) { JS_FreeValue(ctx, arr); JS_FreeCString(ctx, h); return JS_EXCEPTION; }
            JS_DefinePropertyValueStr(ctx, o, "start", JS_NewInt64(ctx, start), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, o, "end", JS_NewInt64(ctx, end), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, arr, out++, o, JS_PROP_C_W_E);
        }
    }
    JS_FreeCString(ctx, h);
    if (!any_sat) {
        JS_FreeValue(ctx, arr);
        /* A Range header that parsed but selects nothing is a 416, which is a
           different answer from "there was no Range header". */
        return JS_NewString(ctx, "unsatisfiable");
    }
    return arr;
}

/* ---------------------------------------------------------------- cookies */

/* CookieParse(header) -> { name: value }. The Cookie request header only:
   `name=value` pairs separated by `; `, no attributes. */
static JSValue dyn_hm_cookie_parse(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *s;
    size_t n, i = 0;
    JSValue obj;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "CookieParse(header): argument must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
    while (i < n) {
        size_t nb, ne, vb, ve;
        while (i < n && (s[i] == ';' || dyn_hm_ows(s[i]))) i++;
        if (i >= n) break;
        nb = i;
        while (i < n && s[i] != '=' && s[i] != ';') i++;
        ne = i;
        if (i < n && s[i] == '=') {
            i++;
            vb = i;
            while (i < n && s[i] != ';') i++;
            ve = i;
        } else {
            vb = ve = i;
        }
        dyn_hm_trim(s, &nb, &ne);
        dyn_hm_trim(s, &vb, &ve);
        /* A quoted cookie value keeps its quotes off, per RFC 6265 sec.4.1.1. */
        if (ve - vb >= 2 && s[vb] == '"' && s[ve - 1] == '"') { vb++; ve--; }
        if (ne > nb) {
            JSAtom a = JS_NewAtomLen(ctx, s + nb, ne - nb);
            if (a != JS_ATOM_NULL) {
                /* Define, not Set: a cookie named __proto__ must not retarget
                   the prototype -- it arrives from the network. */
                JS_DefinePropertyValue(ctx, obj, a,
                    JS_NewStringLen(ctx, s + vb, ve - vb), JS_PROP_C_W_E);
                JS_FreeAtom(ctx, a);
            }
        }
    }
    JS_FreeCString(ctx, s);
    return obj;
}

/* CookieSerialize(name, value, opts?) -> a Set-Cookie value. */
static JSValue dyn_hm_cookie_serialize(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const char *nm, *val;
    size_t nn, vn;
    dyn_sb_t b;
    JSValue ret = JS_EXCEPTION, v;

    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "CookieSerialize(name, value, opts): name must be a string");
    nm = JS_ToCStringLen(ctx, &nn, argv[0]);
    if (!nm)
        return JS_EXCEPTION;
    val = JS_ToCStringLen(ctx, &vn, argv[1]);
    if (!val) { JS_FreeCString(ctx, nm); return JS_EXCEPTION; }
    /* A name that is not a token, or a value carrying a delimiter, would let a
       caller inject an attribute or a second cookie. Refuse rather than escape. */
    if (dyn_hm_needs_quote(nm, nn)) {
        JS_ThrowTypeError(ctx, "CookieSerialize(name, value): name must be a token");
        goto done0;
    }
    {
        size_t k;
        for (k = 0; k < vn; k++) {
            unsigned char c = (unsigned char)val[k];
            if (c < 0x21 || c == ';' || c == ',' || c == '\\' || c == '"' || c == 0x7F) {
                JS_ThrowTypeError(ctx,
                    "CookieSerialize(name, value): value must not contain \"%c\"", c);
                goto done0;
            }
        }
    }
    dyn_sb_init(&b);
    dyn_sb_put(&b, nm, nn);
    dyn_sb_putc(&b, '=');
    dyn_sb_put(&b, val, vn);
    if (argc > 2 && JS_IsObject(argv[2])) {
        int32_t age;
        const char *cs;
        v = JS_GetPropertyStr(ctx, argv[2], "maxAge");
        if (!JS_IsUndefined(v) && !JS_ToInt32(ctx, &age, v)) {
            char num[32];
            snprintf(num, sizeof num, "; Max-Age=%d", age);
            dyn_sb_puts(&b, num);
        }
        JS_FreeValue(ctx, v);
        for (age = 0; age < 3; age++) {
            static const char *const K[3] = { "domain", "path", "sameSite" };
            static const char *const A[3] = { "; Domain=", "; Path=", "; SameSite=" };
            v = JS_GetPropertyStr(ctx, argv[2], K[age]);
            if (JS_IsString(v) && (cs = JS_ToCString(ctx, v)) != NULL) {
                size_t cl = strlen(cs);
                int bad = 0;
                size_t ci;
                for (ci = 0; ci < cl; ci++) {
                    unsigned char ch = (unsigned char)cs[ci];
                    if (ch < 0x21 || ch == ';' || ch == ',' || ch == 0x7F) { bad = 1; break; }
                }
                if (bad) {
                    JS_FreeCString(ctx, cs);
                    JS_FreeValue(ctx, v);
                    dyn_sb_free(&b);
                    JS_ThrowTypeError(ctx,
                        "CookieSerialize: %s must not contain controls, ';' or ','", K[age]);
                    goto done0;
                }
                if (age == 2 && cl > 0) { /* sameSite value check */
                    if (strcasecmp(cs, "Strict") != 0 && strcasecmp(cs, "Lax") != 0 &&
                        strcasecmp(cs, "None") != 0) {
                        JS_FreeCString(ctx, cs);
                        JS_FreeValue(ctx, v);
                        dyn_sb_free(&b);
                        JS_ThrowTypeError(ctx,
                            "CookieSerialize: sameSite must be Strict, Lax or None");
                        goto done0;
                    }
                }
                dyn_sb_puts(&b, A[age]);
                dyn_sb_puts(&b, cs);
                JS_FreeCString(ctx, cs);
            }
            JS_FreeValue(ctx, v);
        }
        v = JS_GetPropertyStr(ctx, argv[2], "secure");
        if (JS_ToBool(ctx, v)) dyn_sb_puts(&b, "; Secure");
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "httpOnly");
        if (JS_ToBool(ctx, v)) dyn_sb_puts(&b, "; HttpOnly");
        JS_FreeValue(ctx, v);
    }
    if (b.oom) { JS_ThrowOutOfMemory(ctx); dyn_sb_free(&b); goto done0; }
    ret = JS_NewStringLen(ctx, b.p, b.n);
    dyn_sb_free(&b);
 done0:
    JS_FreeCString(ctx, nm);
    JS_FreeCString(ctx, val);
    return ret;
}

/* ------------------------------------------------------------------ ETag */

/* ETagMatch(ifNoneMatch, etag) -> boolean. `*` matches anything; comparison is
   WEAK (the W/ prefix is ignored), which is what If-None-Match specifies. */
static JSValue dyn_hm_etag_match(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *h, *e_owned, *e;
    size_t hn, en, i = 0;
    int hit = 0;

    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "ETagMatch(header, etag): two strings required");
    h = JS_ToCStringLen(ctx, &hn, argv[0]);
    if (!h) return JS_EXCEPTION;
    e_owned = JS_ToCStringLen(ctx, &en, argv[1]);
    if (!e_owned) { JS_FreeCString(ctx, h); return JS_EXCEPTION; }
    /* Skip the weak marker with a CURSOR: JS_FreeCString must get the exact
       pointer it returned, or it frees by the wrong address. */
    e = e_owned;
    if (en > 2 && e[0] == 'W' && e[1] == '/') { e += 2; en -= 2; }
    while (i < hn && !hit) {
        size_t eb = i, ee;
        i = dyn_hm_next_elem(h, hn, i, &ee);
        dyn_hm_trim(h, &eb, &ee);
        if (ee - eb == 1 && h[eb] == '*') { hit = 1; break; }
        if (ee - eb > 2 && h[eb] == 'W' && h[eb + 1] == '/') eb += 2;
        if (ee - eb == en && memcmp(h + eb, e, en) == 0)
            hit = 1;
    }
    JS_FreeCString(ctx, h);
    JS_FreeCString(ctx, e_owned);
    return JS_NewBool(ctx, hit);
}

/* -------------------------------------------------------------- multipart */

/* RFC 7578 multipart/form-data parse + encode, on the untrusted frontier (a
   server body). Every structural fault is a REFUSAL with its own guard and
   its own test; no two refusals share one guard. */
#define DYN_MP_MAX_PARTS      1024u
#define DYN_MP_MAX_PART_BYTES (16u << 20)  /* 16 MiB per part body */
#define DYN_MP_MAX_HDR_LINE   4096u
#define DYN_MP_MAX_HDR_BLOCK  65536u
#define DYN_MP_MAX_BOUNDARY   70u          /* RFC 2046 5.1.1 */

static int dyn_mp_bchar(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z'))
        return 1;
    switch (c) {
    case '\'': case '+': case '_': case ',': case '-':
    case '.': case '/': case ':': case '=': case '?':
        return 1;
    default:
        return 0;
    }
}

/* RFC 2046 bcharsnospace: 1..70 chars, none CTL or space. */
static int dyn_mp_boundary_ok(const char *b, size_t n)
{
    size_t i;
    if (n == 0 || n > DYN_MP_MAX_BOUNDARY)
        return 0;
    for (i = 0; i < n; i++)
        if (!dyn_mp_bchar((unsigned char)b[i]))
            return 0;
    return 1;
}

/* A borrowable body: a JS string is COPIED (owned, JS_FreeCString); a byte
   view / ArrayBuffer is borrowed and lives as long as the argument. */
typedef struct { const uint8_t *p; size_t n; char *owned; } dyn_mp_body_t;

static void dyn_mp_body_free(JSContext *ctx, dyn_mp_body_t *v)
{
    if (v->owned) {
        JS_FreeCString(ctx, v->owned);
        v->owned = NULL;
    }
}

/* Resolve a string or byte view (Uint8Array/Int8Array/Uint8ClampedArray/
   DataView/ArrayBuffer) to (ptr,len). View first, so the common Uint8Array
   never pays the throw the ArrayBuffer-first order costs (dyna-bytes.c
   measured 11.5x). Types are checked before any coercion: a plain Array is
   refused, never stringified. Returns JS_UNDEFINED on success, JS_EXCEPTION
   on failure (the exception is set). */
static JSValue dyn_mp_body_view(JSContext *ctx, JSValueConst v,
                                dyn_mp_body_t *out)
{
    JSValue buf;
    size_t off, len, bpe, ab;
    uint8_t *base;

    out->p = NULL; out->n = 0; out->owned = NULL;
    if (JS_IsString(v)) {
        out->p = (const uint8_t *)JS_ToCStringLen(ctx, &out->n, v);
        if (!out->p)
            return JS_EXCEPTION;
        out->owned = (char *)out->p;
        return JS_UNDEFINED;
    }
    if (!JS_IsObject(v))
        return JS_ThrowTypeError(ctx,
            "MultipartParse: body must be a string or a byte view");
    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (!JS_IsException(buf)) {
        if (bpe != 1) {
            JS_FreeValue(ctx, buf);
            return JS_ThrowTypeError(ctx,
                "MultipartParse: body must be a byte view (Uint8Array)");
        }
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        JS_FreeValue(ctx, buf);
        if (!base)
            return JS_EXCEPTION;          /* detached mid-resolve */
        if (off > ab || len > ab - off)
            return JS_ThrowRangeError(ctx,
                "MultipartParse: typed array out of bounds");
        out->p = base + off;
        out->n = len;
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));  /* not a view: try ArrayBuffer */
    base = JS_GetArrayBuffer(ctx, &ab, v);
    if (base) {
        out->p = base;
        out->n = ab;
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_ThrowTypeError(ctx,
        "MultipartParse: body must be a string or a byte view");
}

/* Any CTL byte (RFC 9110: 0x00-0x1F, 0x7F)? These would break out of a
   header line; every encode-side field is checked before emission. */
static int dyn_mp_ctl(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F)
            return 1;
    }
    return 0;
}

/* Emit a quoted-string with quoted-pair escaping of " and \. CTL was already
   refused, so nothing else can break the header. */
static void dyn_mp_escape(dyn_sb_t *b, const char *s, size_t n)
{
    size_t i;
    dyn_sb_putc(b, '"');
    for (i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\')
            dyn_sb_putc(b, '\\');
        dyn_sb_putc(b, s[i]);
    }
    dyn_sb_putc(b, '"');
}

/* Find the next boundary delimiter at or after `start` in b[0..n). A
   delimiter is "--"+bnd at the scan origin or preceded by CRLF, and MUST be
   followed by CRLF or "--" (RFC 2046 5.1.1); a boundary that is a PREFIX of a
   longer token is not a delimiter and is skipped (the longest-prefix attack).
   Returns the offset of the delimiter TOKEN (its leading '-'), SIZE_MAX when
   absent. *final is set when the delimiter is closed by "--". */
static size_t dyn_mp_find_delim(const uint8_t *b, size_t n, size_t start,
                                const char *bnd, size_t blen, int *final)
{
    size_t i, end;
    *final = 0;
    if (n < start + blen + 2)
        return SIZE_MAX;
    if (b[start] == '-' && b[start + 1] == '-'
        && memcmp(b + start + 2, bnd, blen) == 0) {
        end = start + blen + 2;
        if (end + 2 <= n && b[end] == '-' && b[end + 1] == '-') {
            *final = 1;
            return start;
        }
        if (end + 1 < n && b[end] == '\r' && b[end + 1] == '\n')
            return start;
        /* a longer token only begins here: scan for a real delimiter */
    }
    for (i = start; i + blen + 4 <= n; i++) {
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '-'
            && b[i + 3] == '-' && b[i + 4] == (unsigned char)bnd[0]
            && memcmp(b + i + 4, bnd, blen) == 0) {
            end = i + blen + 4;
            if (end + 2 <= n && b[end] == '-' && b[end + 1] == '-') {
                *final = 1;
                return i + 2;
            }
            if (end + 1 < n && b[end] == '\r' && b[end + 1] == '\n')
                return i + 2;
            /* prefix false positive: keep scanning */
        }
    }
    return SIZE_MAX;
}

/* Advance past one `; name=value` parameter of a Content-Disposition value.
   Names must be tokens; values are tokens or quoted-strings (quoted-pair
   aware). Fills the name span [*pb,*pe) and value span [*vb,*ve) borrowed
   into v, sets *quoted for quoted-strings. Returns the next scan position or
   SIZE_MAX on malformed input. */
static size_t dyn_mp_param(const char *v, size_t vn, size_t i,
                           size_t *pb, size_t *pe, size_t *vb, size_t *ve,
                           int *quoted)
{
    size_t k;
    *pb = i;
    while (i < vn && v[i] != '=' && v[i] != ';') i++;
    *pe = i;
    dyn_hm_trim(v, pb, pe);
    if (*pe == *pb)
        return SIZE_MAX;
    for (k = *pb; k < *pe; k++)
        if (!DYN_TCHAR[(unsigned char)v[k]])
            return SIZE_MAX;
    if (i >= vn || v[i] != '=')
        return SIZE_MAX;                 /* a bare parameter needs '=' */
    i++;
    if (i < vn && v[i] == '"') {
        *quoted = 1;
        i++;
        *vb = i;
        while (i < vn && v[i] != '"') {
            if (v[i] == '\\' && i + 1 < vn) {
                i++;                                 /* quoted-pair: any byte */
            } else if ((v[i] < 0x20 && v[i] != '\t') || v[i] == 0x7f) {
                return SIZE_MAX;                     /* CTL outside a quoted-pair */
            }
            i++;
        }
        if (i >= vn)
            return SIZE_MAX;             /* unterminated quoted-string */
        *ve = i;
        i++;
        while (i < vn && dyn_hm_ows(v[i])) i++;
        if (i < vn && v[i] != ';')
            return SIZE_MAX;             /* junk after the quoted-string */
        return i;
    }
    *quoted = 0;
    *vb = i;
    while (i < vn && v[i] != ';') i++;
    *ve = i;
    dyn_hm_trim(v, vb, ve);
    for (k = *vb; k < *ve; k++)
        if (!DYN_TCHAR[(unsigned char)v[k]])
            return SIZE_MAX;
    return i;
}

typedef struct {
    dyn_sb_t name, fname;
    int have_name, have_fname;
} dyn_mp_disp_t;

/* Parse one Content-Disposition value: disposition type "form-data" plus
   `name` and optional `filename` (RFC 7578 4.2), unescaping quoted-pairs.
   CR/LF cannot reach the parsed values: the header walk refused any bare
   CR/LF in the line, and the CRLF split isolates each value span. */
static int dyn_mp_disposition(JSContext *ctx, const char *v, size_t vn,
                              dyn_mp_disp_t *d)
{
    size_t i = 0;
    d->have_name = 0;
    d->have_fname = 0;
    while (i < vn && dyn_hm_ows(v[i])) i++;
    if (vn - i < 9 || strncasecmp(v + i, "form-data", 9) != 0) {
        JS_ThrowTypeError(ctx,
            "MultipartParse: Content-Disposition type must be form-data");
        return -1;
    }
    i += 9;
    while (i < vn) {
        size_t pb, pe, vb, ve, k;
        int quoted;
        dyn_sb_t *dst;
        while (i < vn && (dyn_hm_ows(v[i]) || v[i] == ';')) i++;
        if (i >= vn)
            break;                       /* no more parameters */
        i = dyn_mp_param(v, vn, i, &pb, &pe, &vb, &ve, &quoted);
        if (i == SIZE_MAX) {
            JS_ThrowTypeError(ctx,
                "MultipartParse: malformed Content-Disposition parameter");
            return -1;
        }
        if (pe - pb == 4 && strncasecmp(v + pb, "name", 4) == 0) {
            if (d->have_name) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: duplicate name parameter");
                return -1;
            }
            dst = &d->name;
            d->have_name = 1;
        } else if (pe - pb == 8 && strncasecmp(v + pb, "filename", 8) == 0) {
            if (d->have_fname) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: duplicate filename parameter");
                return -1;
            }
            dst = &d->fname;
            d->have_fname = 1;
        } else {
            dst = NULL;                  /* other params are ignored */
        }
        if (dst) {
            if (quoted) {
                for (k = vb; k < ve; k++) {
                    if (v[k] == '\\' && k + 1 < ve)
                        k++;             /* quoted-pair */
                    dyn_sb_putc(dst, v[k]);
                }
            } else {
                dyn_sb_put(dst, v + vb, ve - vb);
            }
            if (dst->oom) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        }
        if (i == vn)
            break;
    }
    return 0;
}

/* Build one parsed part object: { name, filename?, contentType?, body }. */
static JSValue dyn_mp_part_obj(JSContext *ctx, const dyn_mp_disp_t *d,
                               const uint8_t *b, size_t cs, size_t ce,
                               size_t body_start, size_t body_end)
{
    static const uint8_t zero = 0;
    JSValue o, ab, ta;
    JSValueConst ta3[3];

    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, o, "name",
        JS_NewStringLen(ctx, d->name.p ? d->name.p : "", d->name.n),
        JS_PROP_C_W_E);
    if (d->have_fname)
        JS_DefinePropertyValueStr(ctx, o, "filename",
            JS_NewStringLen(ctx, d->fname.p ? d->fname.p : "", d->fname.n),
            JS_PROP_C_W_E);
    if (cs != SIZE_MAX)
        JS_DefinePropertyValueStr(ctx, o, "contentType",
            JS_NewStringLen(ctx, (const char *)b + cs, ce - cs),
            JS_PROP_C_W_E);
    ab = JS_NewArrayBufferCopy(ctx,
        body_end > body_start ? b + body_start : &zero,
        body_end - body_start);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    ta3[0] = ab; ta3[1] = JS_UNDEFINED; ta3[2] = JS_UNDEFINED;
    ta = JS_NewTypedArray(ctx, 3, ta3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(ta)) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, o, "body", ta, JS_PROP_C_W_E);
    return o;
}

/* MultipartParse(contentType, body) -> [{name, filename?, contentType?,
   body: Uint8Array}] or refuse. The body is scanned in one pass; every
   structural fault names its own guard. */
static JSValue dyn_mp_parse(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    const char *ct = NULL, *bnd = NULL;
    size_t ctn = 0, blen = 0;
    dyn_mp_body_t body;
    JSValue ctv = JS_UNDEFINED, params = JS_UNDEFINED, bndv = JS_UNDEFINED;
    JSValue arr = JS_UNDEFINED;
    JSValue ret = JS_EXCEPTION;
    uint32_t out = 0;
    const uint8_t *b;
    size_t q, E, D2, hd;
    int final;

    body.p = NULL; body.n = 0; body.owned = NULL;
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "MultipartParse(contentType, body): contentType must be a string");
    ct = JS_ToCStringLen(ctx, &ctn, argv[0]);
    if (!ct)
        return JS_EXCEPTION;
    if (JS_IsException(dyn_mp_body_view(ctx, argv[1], &body))) {
        JS_FreeCString(ctx, ct);
        return JS_EXCEPTION;
    }
    b = body.p;
    ctv = dyn_hm_ctype_parse(ctx, JS_UNDEFINED, 1, argv);
    if (JS_IsException(ctv))
        goto done;
    if (JS_IsNull(ctv)) {
        JS_ThrowTypeError(ctx,
            "MultipartParse: Content-Type must be multipart/form-data");
        goto done;
    }
    {
        JSValue tv = JS_GetPropertyStr(ctx, ctv, "type");
        JSValue sv = JS_GetPropertyStr(ctx, ctv, "subtype");
        const char *ts, *ss;
        size_t tn, sn;
        int bad = 1;
        if (JS_IsString(tv) && JS_IsString(sv)) {
            ts = JS_ToCStringLen(ctx, &tn, tv);
            ss = JS_ToCStringLen(ctx, &sn, sv);
            if (ts && ss && tn == 9 && sn == 9
                && memcmp(ts, "multipart", 9) == 0
                && memcmp(ss, "form-data", 9) == 0)
                bad = 0;
            if (ts) JS_FreeCString(ctx, ts);
            if (ss) JS_FreeCString(ctx, ss);
        }
        JS_FreeValue(ctx, tv);
        JS_FreeValue(ctx, sv);
        if (bad) {
            JS_ThrowTypeError(ctx,
                "MultipartParse: Content-Type must be multipart/form-data");
            goto done;
        }
    }
    params = JS_GetPropertyStr(ctx, ctv, "parameters");
    if (JS_IsObject(params)) {
        bndv = JS_GetPropertyStr(ctx, params, "boundary");
        if (JS_IsString(bndv))
            bnd = JS_ToCStringLen(ctx, &blen, bndv);
    }
    if (!bnd || !dyn_mp_boundary_ok(bnd, blen)) {
        JS_ThrowTypeError(ctx,
            "MultipartParse: boundary parameter is missing or invalid (1-70 bcharsnospace)");
        goto done;
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        goto done;
    D2 = dyn_mp_find_delim(b, body.n, 0, bnd, blen, &final);
    if (D2 == SIZE_MAX) {
        JS_ThrowTypeError(ctx, "MultipartParse: no opening boundary");
        goto done;
    }
    if (final) {
        ret = arr;                        /* --bnd-- : zero parts */
        goto done;
    }
    q = D2 + blen + 4;
    for (;;) {
        size_t r, ds = SIZE_MAX, de = 0, cs = SIZE_MAX, ce = 0;
        size_t body_start, body_end, plen;
        int disp_lines = 0;
        dyn_mp_disp_t d;
        JSValue part;

        if (out >= DYN_MP_MAX_PARTS) {
            JS_ThrowTypeError(ctx, "MultipartParse: more than 1024 parts");
            goto done;
        }
        /* the blank line that terminates the header block */
        {
            size_t k;
            E = SIZE_MAX;
            for (k = q; k + 3 < body.n; k++)
                if (b[k] == '\r' && b[k + 1] == '\n'
                    && b[k + 2] == '\r' && b[k + 3] == '\n') {
                    E = k;
                    break;
                }
            if (E == SIZE_MAX) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: part headers not terminated by a blank line");
                goto done;
            }
            if (E - q > DYN_MP_MAX_HDR_BLOCK) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: part header block exceeds 65536 bytes");
                goto done;
            }
        }
        /* a delimiter before the blank line means the headers never ended */
        hd = dyn_mp_find_delim(b, E, q, bnd, blen, &final);
        if (hd != SIZE_MAX) {
            JS_ThrowTypeError(ctx,
                "MultipartParse: boundary delimiter inside part headers");
            goto done;
        }
        /* header lines */
        r = q;
        while (r < E) {
            size_t le = r, colon, hb, ve;
            size_t k;
            /* the header's own CRLF sits AT E,E+1 (E was found by the
               CRLFCRLF search), so le must be allowed to reach E */
            while (le <= E && !(b[le] == '\r' && b[le + 1] == '\n')) le++;
            if (le > E)
                break;
            if (le - r > DYN_MP_MAX_HDR_LINE) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: part header line exceeds 4096 bytes");
                goto done;
            }
            for (k = r; k < le; k++)
                if (b[k] == '\r' || b[k] == '\n') {
                    JS_ThrowTypeError(ctx,
                        "MultipartParse: bare CR or LF in a header line");
                    goto done;
                }
            for (colon = r; colon < le && b[colon] != ':'; colon++) ;
            if (colon == le || colon == r) {
                JS_ThrowTypeError(ctx,
                    "MultipartParse: malformed part header line");
                goto done;
            }
            for (k = r; k < colon; k++)
                if (!DYN_TCHAR[(unsigned char)b[k]]) {
                    JS_ThrowTypeError(ctx,
                        "MultipartParse: malformed part header name");
                    goto done;
                }
            hb = colon + 1;
            while (hb < le && dyn_hm_ows(b[hb])) hb++;
            ve = le;
            while (ve > hb && dyn_hm_ows(b[ve - 1])) ve--;
            if (colon - r == 19
                && strncasecmp((const char *)b + r, "Content-Disposition",
                               19) == 0) {
                if (disp_lines) {
                    JS_ThrowTypeError(ctx,
                        "MultipartParse: more than one Content-Disposition header");
                    goto done;
                }
                disp_lines = 1;
                ds = hb;
                de = ve;
            } else if (colon - r == 12
                && strncasecmp((const char *)b + r, "Content-Type", 12) == 0) {
                cs = hb;
                ce = ve;
            }
            r = le + 2;
        }
        if (!disp_lines) {
            JS_ThrowTypeError(ctx,
                "MultipartParse: part has no Content-Disposition header");
            goto done;
        }
        dyn_sb_init(&d.name);
        dyn_sb_init(&d.fname);
        if (dyn_mp_disposition(ctx, (const char *)b + ds, de - ds, &d) < 0) {
            dyn_sb_free(&d.name);
            dyn_sb_free(&d.fname);
            goto done;
        }
        if (!d.have_name) {
            dyn_sb_free(&d.name);
            dyn_sb_free(&d.fname);
            JS_ThrowTypeError(ctx,
                "MultipartParse: Content-Disposition lacks a name parameter");
            goto done;
        }
        /* the part body runs to the next delimiter (or the closing one) */
        body_start = E + 4;
        D2 = dyn_mp_find_delim(b, body.n, body_start, bnd, blen, &final);
        if (D2 == SIZE_MAX) {
            dyn_sb_free(&d.name);
            dyn_sb_free(&d.fname);
            JS_ThrowTypeError(ctx,
                "MultipartParse: missing closing boundary");
            goto done;
        }
        body_end = (D2 == body_start) ? D2 : D2 - 2;
        plen = body_end - body_start;
        if (plen > DYN_MP_MAX_PART_BYTES) {
            dyn_sb_free(&d.name);
            dyn_sb_free(&d.fname);
            JS_ThrowTypeError(ctx,
                "MultipartParse: part body exceeds 16 MiB");
            goto done;
        }
        part = dyn_mp_part_obj(ctx, &d, b, cs, ce, body_start, body_end);
        dyn_sb_free(&d.name);
        dyn_sb_free(&d.fname);
        if (JS_IsException(part))
            goto done;
        JS_DefinePropertyValueUint32(ctx, arr, out++, part, JS_PROP_C_W_E);
        if (final) {
            ret = arr;
            goto done;
        }
        q = D2 + blen + 4;
    }
 done:
    if (bnd) JS_FreeCString(ctx, bnd);
    JS_FreeValue(ctx, bndv);
    JS_FreeValue(ctx, params);
    JS_FreeValue(ctx, ctv);
    dyn_mp_body_free(ctx, &body);
    JS_FreeCString(ctx, ct);
    if (JS_IsException(ret))
        JS_FreeValue(ctx, arr);
    return ret;
}

/* MultipartFormat(parts, boundary?) -> { contentType, boundary, body }.
   Each part is { name, value|body, filename?, contentType? }. CTL bytes in
   name/filename/contentType are refused (header injection); quotes and
   backslashes in name/filename are quoted-pair escaped. A caller-supplied
   boundary must pass the same bcharsnospace rule the parser enforces. */
static JSValue dyn_mp_format(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_sb_t out;
    char bnd[71];
    size_t blen;
    int64_t nparts = 0, k;
    JSValue ret = JS_EXCEPTION;
    JSValue el = JS_UNDEFINED, nm = JS_UNDEFINED, val = JS_UNDEFINED;
    JSValue bd = JS_UNDEFINED, fn = JS_UNDEFINED, ct = JS_UNDEFINED;
    const char *ns = NULL;
    size_t nn = 0;
    dyn_mp_body_t payload;

    payload.p = NULL; payload.n = 0; payload.owned = NULL;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "MultipartFormat(parts, boundary?): parts must be an array");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_ToInt64(ctx, &nparts, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        const char *s;
        size_t sn;
        if (!JS_IsString(argv[1]))
            return JS_ThrowTypeError(ctx,
                "MultipartFormat(parts, boundary?): boundary must be a string");
        s = JS_ToCStringLen(ctx, &sn, argv[1]);
        if (!s)
            return JS_EXCEPTION;
        if (!dyn_mp_boundary_ok(s, sn)) {
            JS_FreeCString(ctx, s);
            return JS_ThrowTypeError(ctx,
                "MultipartFormat: boundary must be 1-70 bcharsnospace characters");
        }
        memcpy(bnd, s, sn);               /* sn <= 70, buffer is 71 */
        bnd[sn] = 0;
        blen = sn;
        JS_FreeCString(ctx, s);
    } else {
        dyn_prng_t r;
        size_t i;
        static const char B62[] =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        if (dyn_prng_seed_random(&r) < 0)
            return JS_ThrowInternalError(ctx,
                "MultipartFormat: OS entropy unavailable");
        memcpy(bnd, "------------------------", 24);
        for (i = 24; i < 48; i++)
            bnd[i] = B62[dyn_prng_next_bounded(&r, 62)];
        bnd[48] = 0;
        blen = 48;
    }
    dyn_sb_init(&out);
    for (k = 0; k < nparts; k++) {
        int have_payload = 0;

        el = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)k);
        if (JS_IsException(el))
            goto done;
        if (!JS_IsObject(el)) {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: each part must be an object");
            goto err;
        }
        nm = JS_GetPropertyStr(ctx, el, "name");
        if (!JS_IsString(nm)) {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: each part needs a string name");
            goto err;
        }
        ns = JS_ToCStringLen(ctx, &nn, nm);
        if (!ns)
            goto err;
        if (nn == 0 || dyn_mp_ctl(ns, nn)) {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: name must be a non-empty string with no control characters");
            goto err;
        }
        val = JS_GetPropertyStr(ctx, el, "value");
        bd = JS_GetPropertyStr(ctx, el, "body");
        fn = JS_GetPropertyStr(ctx, el, "filename");
        ct = JS_GetPropertyStr(ctx, el, "contentType");
        if (!JS_IsUndefined(val) && !JS_IsUndefined(bd)) {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: a part must have value OR body, not both");
            goto err;
        }
        if (!JS_IsUndefined(bd)) {
            if (JS_IsException(dyn_mp_body_view(ctx, bd, &payload)))
                goto err;
            have_payload = 1;
        } else if (!JS_IsUndefined(val)) {
            if (!JS_IsString(val)) {
                JS_ThrowTypeError(ctx, "MultipartFormat: value must be a string");
                goto err;
            }
            payload.p = (const uint8_t *)JS_ToCStringLen(ctx, &payload.n, val);
            if (!payload.p)
                goto err;
            payload.owned = (char *)payload.p;
            have_payload = 1;
        } else {
            JS_ThrowTypeError(ctx,
                "MultipartFormat: a part needs value or body");
            goto err;
        }
        dyn_sb_putc(&out, '-');
        dyn_sb_putc(&out, '-');
        dyn_sb_put(&out, bnd, blen);
        dyn_sb_puts(&out, "\r\nContent-Disposition: form-data; name=");
        dyn_mp_escape(&out, ns, nn);
        if (!JS_IsUndefined(fn)) {
            const char *fs;
            size_t fnl = 0;
            if (!JS_IsString(fn)) {
                JS_ThrowTypeError(ctx,
                    "MultipartFormat: filename must be a string");
                goto err;
            }
            fs = JS_ToCStringLen(ctx, &fnl, fn);
            if (!fs)
                goto err;
            if (dyn_mp_ctl(fs, fnl)) {
                JS_FreeCString(ctx, fs);
                JS_ThrowTypeError(ctx,
                    "MultipartFormat: filename must not contain control characters");
                goto err;
            }
            dyn_sb_puts(&out, "; filename=");
            dyn_mp_escape(&out, fs, fnl);
            JS_FreeCString(ctx, fs);
        }
        dyn_sb_puts(&out, "\r\n");
        if (!JS_IsUndefined(ct)) {
            const char *cts;
            size_t ctn2 = 0;
            if (!JS_IsString(ct)) {
                JS_ThrowTypeError(ctx,
                    "MultipartFormat: contentType must be a string");
                goto err;
            }
            cts = JS_ToCStringLen(ctx, &ctn2, ct);
            if (!cts)
                goto err;
            if (dyn_mp_ctl(cts, ctn2)) {
                JS_FreeCString(ctx, cts);
                JS_ThrowTypeError(ctx,
                    "MultipartFormat: contentType must not contain control characters");
                goto err;
            }
            dyn_sb_puts(&out, "Content-Type: ");
            dyn_sb_put(&out, cts, ctn2);
            dyn_sb_puts(&out, "\r\n");
            JS_FreeCString(ctx, cts);
        }
        dyn_sb_puts(&out, "\r\n");
        if (have_payload)
            dyn_sb_put(&out, (const char *)payload.p, payload.n);
        dyn_sb_puts(&out, "\r\n");
        if (out.oom) {
            JS_ThrowOutOfMemory(ctx);
            goto err;
        }
        dyn_mp_body_free(ctx, &payload);
        payload.p = NULL; payload.n = 0;
        JS_FreeCString(ctx, ns);
        ns = NULL; nn = 0;
        JS_FreeValue(ctx, el); el = JS_UNDEFINED;
        JS_FreeValue(ctx, nm); nm = JS_UNDEFINED;
        JS_FreeValue(ctx, val); val = JS_UNDEFINED;
        JS_FreeValue(ctx, bd); bd = JS_UNDEFINED;
        JS_FreeValue(ctx, fn); fn = JS_UNDEFINED;
        JS_FreeValue(ctx, ct); ct = JS_UNDEFINED;
    }
    dyn_sb_put(&out, "--", 2);
    dyn_sb_put(&out, bnd, blen);
    dyn_sb_puts(&out, "--\r\n");
    if (out.oom) {
        JS_ThrowOutOfMemory(ctx);
        goto err;
    }
    {
        dyn_sb_t hdrb;
        JSValue o, ab, ta;
        JSValueConst ta3[3];
        dyn_sb_init(&hdrb);
        dyn_sb_puts(&hdrb, "multipart/form-data; boundary=");
        dyn_sb_put(&hdrb, bnd, blen);
        if (hdrb.oom) {
            dyn_sb_free(&hdrb);
            JS_ThrowOutOfMemory(ctx);
            goto err;
        }
        o = JS_NewObject(ctx);
        if (JS_IsException(o)) {
            dyn_sb_free(&hdrb);
            goto err;
        }
        JS_DefinePropertyValueStr(ctx, o, "contentType",
            JS_NewStringLen(ctx, hdrb.p ? hdrb.p : "", hdrb.n), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, o, "boundary",
            JS_NewStringLen(ctx, bnd, blen), JS_PROP_C_W_E);
        dyn_sb_free(&hdrb);
        ab = JS_NewArrayBufferCopy(ctx,
            (const uint8_t *)(out.p ? out.p : ""), out.n);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, o);
            goto err;
        }
        ta3[0] = ab; ta3[1] = JS_UNDEFINED; ta3[2] = JS_UNDEFINED;
        ta = JS_NewTypedArray(ctx, 3, ta3, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, ab);
        if (JS_IsException(ta)) {
            JS_FreeValue(ctx, o);
            goto err;
        }
        JS_DefinePropertyValueStr(ctx, o, "body", ta, JS_PROP_C_W_E);
        ret = o;
    }
 done:
    dyn_sb_free(&out);
    return ret;
 err:
    dyn_mp_body_free(ctx, &payload);
    if (ns) JS_FreeCString(ctx, ns);
    JS_FreeValue(ctx, el);
    JS_FreeValue(ctx, nm);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, bd);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, ct);
    dyn_sb_free(&out);
    return JS_EXCEPTION;
}

/* ------------------------------------------------------------ registration */

static const JSCFunctionListEntry dyn_httpmsg_funcs[] = {
    JS_CFUNC_DEF("ContentTypeParse", 1, dyn_hm_ctype_parse),
    JS_CFUNC_DEF("ContentTypeFormat", 1, dyn_hm_ctype_format),
    JS_CFUNC_MAGIC_DEF("Negotiate", 2, dyn_hm_negotiate, 0),
    JS_CFUNC_MAGIC_DEF("NegotiateToken", 2, dyn_hm_negotiate, 1),
    JS_CFUNC_DEF("RangeParse", 2, dyn_hm_range),
    JS_CFUNC_DEF("CookieParse", 1, dyn_hm_cookie_parse),
    JS_CFUNC_DEF("CookieSerialize", 2, dyn_hm_cookie_serialize),
    JS_CFUNC_DEF("ETagMatch", 2, dyn_hm_etag_match),
    JS_CFUNC_DEF("MultipartParse", 2, dyn_mp_parse),
    JS_CFUNC_DEF("MultipartFormat", 1, dyn_mp_format),
};

static int dyn_httpmsg_register(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_httpmsg_funcs,
                                  countof(dyn_httpmsg_funcs));
}

static void dyn_httpmsg_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExportList(ctx, m, dyn_httpmsg_funcs, countof(dyn_httpmsg_funcs));
}
