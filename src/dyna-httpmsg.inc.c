/* HTTP message codecs for dyna:net (design 15): ContentType, Accepts, Range,
   ETag and Cookies. Included by dyna-http.c so they register into the one
   module that already owns the HTTP surface. */

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

/* A local growable buffer: dyn_sb_t lives in dyna-url.c, a different TU. */
typedef struct { char *p; size_t n, cap; int oom; } dyn_sb_t;

static void dyn_sb_init(dyn_sb_t *b) { b->p = NULL; b->n = 0; b->cap = 0; b->oom = 0; }
static void dyn_sb_free(dyn_sb_t *b) { free(b->p); b->p = NULL; b->n = 0; b->cap = 0; }

/* Overshoot costs ABSOLUTE bytes, so the factor decays with size: doubling is
   free while small and wastes megabytes at scale. */
static size_t dyn_grow_cap(size_t cur, size_t need, size_t seed)
{
    size_t nc = cur ? cur : seed;
    while (nc < need) {
        if (nc < (1u << 16))      nc *= 2;
        else if (nc < (1u << 20)) nc += nc / 2;
        else                      nc += nc / 4;
    }
    return nc;
}

static void dyn_sb_put(dyn_sb_t *b, const char *s, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + n, 64);
        char *np;
        np = (char *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
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
            if (rb == re || q <= 0)
                continue;                          /* q=0 is an explicit refusal */
            for (k = 0; k < ncand; k++) {
                JSValue cv = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)k);
                size_t cn;
                const char *cs = JS_IsException(cv) ? NULL : JS_ToCStringLen(ctx, &cn, cv);
                int spec = 0;
                double sc = -1;
                JS_FreeValue(ctx, cv);
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
                JS_FreeCString(ctx, cs);
                if (sc > 0 && (sc > best_q || (sc == best_q && spec > best_spec))) {
                    best_q = sc; best_spec = spec; best = k;
                }
            }
        }
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

/* ------------------------------------------------------------ registration */

static const JSCFunctionListEntry dyn_httpmsg_funcs[] = {
    JS_CFUNC_DEF("ContentTypeParse", 1, dyn_hm_ctype_parse),
    JS_CFUNC_DEF("ContentTypeFormat", 1, dyn_hm_ctype_format),
    JS_CFUNC_MAGIC_DEF("Negotiate", 2, dyn_hm_negotiate, 0),
    JS_CFUNC_MAGIC_DEF("NegotiateToken", 2, dyn_hm_negotiate, 1),
    JS_CFUNC_DEF("RangeParse", 2, dyn_hm_range),
    JS_CFUNC_DEF("CookieParse", 1, dyn_hm_cookie_parse),
    JS_CFUNC_DEF("CookieSerialize", 3, dyn_hm_cookie_serialize),
    JS_CFUNC_DEF("ETagMatch", 2, dyn_hm_etag_match),
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
