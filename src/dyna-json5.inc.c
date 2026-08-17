/* JSON5 and RFC 8785 canonical stringify for dyna:encoding.
   Both are recursive-descent over untrusted text, so both carry an explicit
   depth cap checked BEFORE descending rather than a stack that runs out. */

#define DYN_J5_MAX_DEPTH 256

/* A minimal growable byte buffer: DynBuf is engine-internal and a native module
   only gets the public API. */
typedef struct { uint8_t *p; size_t n, cap; int oom; } dyn_buf_t;

static void dyn_buf_init(dyn_buf_t *b) { b->p = NULL; b->n = 0; b->cap = 0; b->oom = 0; }
static void dyn_buf_free(dyn_buf_t *b) { free(b->p); b->p = NULL; }

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

static void dyn_buf_put(dyn_buf_t *b, uint8_t c)
{
    if (b->n == b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + 1, 64);
        uint8_t *np = (uint8_t *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    b->p[b->n++] = c;
}

/* Bytes that end a clean run in a JSON5 string literal. ONE table, so the scan
   and the switch that handles the stop byte cannot disagree. */
static const uint8_t DYN_J5_STOP[256] = {
    ['"']=1, ['\'']=1, ['\\']=1, ['\n']=1, ['\r']=1,
};

/* Bytes that must be escaped on OUTPUT. Same discipline. */
static const uint8_t DYN_SER_ESC[256] = {
    /* 0x00-0x1F are all escaped; \b \f \n \r \t live inside that range, so
       naming them again here would be a duplicate initializer. */
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    ['"']=1, ['\\']=1,
};

static void dyn_buf_write(dyn_buf_t *b, const uint8_t *p, size_t n)
{
    if (b->oom || n == 0)
        return;
    if (b->n + n > b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + n, 64);
        uint8_t *np = (uint8_t *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

typedef struct {
    JSContext *ctx;
    const char *s;
    size_t n, i;
    int depth;
} dyn_j5_t;

static JSValue dyn_j5_value(dyn_j5_t *p);

static int dyn_j5_err(dyn_j5_t *p, const char *what)
{
    JS_ThrowSyntaxError(p->ctx, "JSON5Parse: %s at offset %u", what,
                        (unsigned)p->i);
    return -1;
}

/* Whitespace plus both comment forms. JSON5 allows them anywhere JSON allows
   whitespace, so this is the one place they are consumed. */
static int dyn_j5_ws(dyn_j5_t *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
            || c == '\v') {
            p->i++;
        } else if (c == '/' && p->i + 1 < p->n && p->s[p->i + 1] == '/') {
            p->i += 2;
            while (p->i < p->n && p->s[p->i] != '\n')
                p->i++;
        } else if (c == '/' && p->i + 1 < p->n && p->s[p->i + 1] == '*') {
            p->i += 2;
            for (;;) {
                if (p->i + 1 >= p->n)
                    return dyn_j5_err(p, "unterminated block comment");
                if (p->s[p->i] == '*' && p->s[p->i + 1] == '/') {
                    p->i += 2;
                    break;
                }
                p->i++;
            }
        } else {
            break;
        }
    }
    return 0;
}

static int dyn_j5_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int dyn_j5_ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
        || c == '$' || (unsigned char)c >= 0x80;
}

static int dyn_j5_ident_part(int c)
{
    return dyn_j5_ident_start(c) || (c >= '0' && c <= '9');
}

/* Append one code point as UTF-8. */
static void dyn_buf_putc(dyn_buf_t *b, uint32_t cp)
{
    if (cp < 0x80) {
        dyn_buf_put(b, (uint8_t)cp);
    } else if (cp < 0x800) {
        dyn_buf_put(b, (uint8_t)(0xC0 | (cp >> 6)));
        dyn_buf_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        dyn_buf_put(b, (uint8_t)(0xE0 | (cp >> 12)));
        dyn_buf_put(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        dyn_buf_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else {
        dyn_buf_put(b, (uint8_t)(0xF0 | (cp >> 18)));
        dyn_buf_put(b, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
        dyn_buf_put(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        dyn_buf_put(b, (uint8_t)(0x80 | (cp & 0x3F)));
    }
}

/* Read `want` hex digits; -1 (having thrown) if any is not a hex digit. */
static int dyn_j5_hexn(dyn_j5_t *p, int want, int *out)
{
    int k, cp = 0;
    for (k = 0; k < want; k++) {
        int h = (p->i < p->n) ? dyn_j5_hex((unsigned char)p->s[p->i]) : -1;
        if (h < 0)
            return dyn_j5_err(p, "bad hex escape");
        cp = (cp << 4) | h;
        p->i++;
    }
    *out = cp;
    return 0;
}

/* A \x or \u escape, recombining a surrogate PAIR. A lone surrogate is
   written through unchanged so the round trip stays lossless. */
static int dyn_j5_hex_escape(dyn_j5_t *p, dyn_buf_t *b, int want)
{
    int cp;
    if (dyn_j5_hexn(p, want, &cp) < 0)
        return -1;
    if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 1 < p->n
        && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
        size_t save = p->i;
        int lo;
        p->i += 2;
        if (dyn_j5_hexn(p, 4, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        else
            p->i = save;
    }
    dyn_buf_putc(b, (uint32_t)cp);
    return 0;
}

/* A JSON5 string: either quote, escapes including \x, \u, and a backslash
   before a real newline (line continuation). Emits UTF-8 into a StringBuffer-
   free growable buffer; the caller turns it into a JS string. */
static JSValue dyn_j5_string(dyn_j5_t *p, char quote)
{
    JSContext *ctx = p->ctx;
    dyn_buf_t b;
    JSValue ret;
    dyn_buf_init(&b);
    p->i++;                                    /* the opening quote */
    while (p->i < p->n && p->s[p->i] != quote) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (!DYN_J5_STOP[c]) {                 /* bulk-copy the clean run */
            size_t run = p->i;
            while (p->i < p->n && !DYN_J5_STOP[(unsigned char)p->s[p->i]])
                p->i++;
            dyn_buf_write(&b, (const uint8_t *)p->s + run, p->i - run);
            continue;
        }
        if (c == '\n' || c == '\r') {
            dyn_buf_free(&b);
            dyn_j5_err(p, "unescaped newline in a string");
            return JS_EXCEPTION;
        }
        if (c != '\\') {
            dyn_buf_put(&b, c);
            p->i++;
            continue;
        }
        p->i++;
        if (p->i >= p->n)
            break;
        c = (unsigned char)p->s[p->i++];
        switch (c) {
        case 'n': dyn_buf_put(&b, '\n'); break;
        case 't': dyn_buf_put(&b, '\t'); break;
        case 'r': dyn_buf_put(&b, '\r'); break;
        case 'b': dyn_buf_put(&b, '\b'); break;
        case 'f': dyn_buf_put(&b, '\f'); break;
        case 'v': dyn_buf_put(&b, '\v'); break;
        case '0': dyn_buf_put(&b, '\0'); break;
        case '\r':                             /* line continuation */
            if (p->i < p->n && p->s[p->i] == '\n') p->i++;
            break;
        case '\n': break;
        case 'x': case 'u':
            if (dyn_j5_hex_escape(p, &b, c == 'x' ? 2 : 4) < 0) {
                dyn_buf_free(&b);
                return JS_EXCEPTION;
            }
            break;
        default: dyn_buf_put(&b, c); break;       /* \\ \" \' \/ and the rest */
        }
    }
    if (p->i >= p->n) {
        dyn_buf_free(&b);
        dyn_j5_err(p, "unterminated string");
        return JS_EXCEPTION;
    }
    p->i++;                                     /* the closing quote */
    if (b.oom) { dyn_buf_free(&b); return JS_ThrowOutOfMemory(ctx); }
    ret = JS_NewStringLen(ctx, (const char *)b.p, b.n);
    dyn_buf_free(&b);
    return ret;
}

/* 0x-prefixed integer. JSON5 has no hex FLOAT, so this is exact up to 2^53. */
static JSValue dyn_j5_hexnum(dyn_j5_t *p, int sign)
{
    uint64_t v = 0;
    int any = 0;
    p->i += 2;
    while (p->i < p->n) {
        int h = dyn_j5_hex((unsigned char)p->s[p->i]);
        if (h < 0)
            break;
        v = v * 16 + (uint64_t)h;
        any = 1;
        p->i++;
    }
    if (!any) {
        dyn_j5_err(p, "bad hex number");
        return JS_EXCEPTION;
    }
    return JS_NewFloat64(p->ctx, sign * (double)v);
}

/* Consume a decimal literal: digits, an optional '.', an optional exponent.
   An 'e' with no digits after it is not part of the number and is rewound. */
static void dyn_j5_decimal(dyn_j5_t *p)
{
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        size_t save = p->i;
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        if (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
            while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        else
            p->i = save;
    }
}

/* JSON5 numbers: hex, leading/trailing '.', a leading '+', Infinity and NaN.
   Hand-rolled rather than strtod, which reads LC_NUMERIC for the radix. */
static JSValue dyn_j5_number(dyn_j5_t *p)
{
    size_t start = p->i;
    int sign = 1;
    char buf[64];
    size_t len;

    if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) {
        if (p->s[p->i] == '-') sign = -1;
        p->i++;
    }
    if (p->n - p->i >= 8 && memcmp(p->s + p->i, "Infinity", 8) == 0) {
        p->i += 8;
        return JS_NewFloat64(p->ctx, sign > 0 ? INFINITY : -INFINITY);
    }
    if (p->n - p->i >= 3 && memcmp(p->s + p->i, "NaN", 3) == 0) {
        p->i += 3;
        return JS_NewFloat64(p->ctx, NAN);
    }
    if (p->n - p->i >= 2 && p->s[p->i] == '0'
        && (p->s[p->i + 1] == 'x' || p->s[p->i + 1] == 'X'))
        return dyn_j5_hexnum(p, sign);
    dyn_j5_decimal(p);
    len = p->i - start;
    if (len == 0 || len >= sizeof buf) {
        dyn_j5_err(p, "bad number");
        return JS_EXCEPTION;
    }
    memcpy(buf, p->s + start, len);
    buf[len] = 0;
    {
        /* ToNumber over a string is the engine's own correctly-rounded, locale-
           independent parser; strtod would read LC_NUMERIC for the radix. */
        JSValue sv = JS_NewStringLen(p->ctx, buf, len);
        double d;
        int r;
        if (JS_IsException(sv))
            return JS_EXCEPTION;
        r = JS_ToFloat64(p->ctx, &d, sv);
        JS_FreeValue(p->ctx, sv);
        if (r)
            return JS_EXCEPTION;
        return JS_NewFloat64(p->ctx, d);
    }
}

/* A key is a string, or a bare ECMAScript identifier. */
static JSAtom dyn_j5_key(dyn_j5_t *p)
{
    if (p->i < p->n && (p->s[p->i] == '"' || p->s[p->i] == '\'')) {
        JSValue k = dyn_j5_string(p, p->s[p->i]);
        JSAtom a;
        if (JS_IsException(k))
            return JS_ATOM_NULL;
        a = JS_ValueToAtom(p->ctx, k);
        JS_FreeValue(p->ctx, k);
        return a;
    }
    if (p->i < p->n && dyn_j5_ident_start((unsigned char)p->s[p->i])) {
        size_t b = p->i;
        while (p->i < p->n && dyn_j5_ident_part((unsigned char)p->s[p->i]))
            p->i++;
        return JS_NewAtomLen(p->ctx, p->s + b, p->i - b);
    }
    dyn_j5_err(p, "expected a key");
    return JS_ATOM_NULL;
}

static JSValue dyn_j5_object(dyn_j5_t *p)
{
    JSValue obj = JS_NewObject(p->ctx);
    if (JS_IsException(obj))
        return obj;
    p->i++;                                     /* '{' */
    for (;;) {
        JSAtom k;
        JSValue v;
        if (dyn_j5_ws(p) < 0) goto fail;
        if (p->i >= p->n) { dyn_j5_err(p, "unterminated object"); goto fail; }
        if (p->s[p->i] == '}') { p->i++; break; }
        k = dyn_j5_key(p);
        if (k == JS_ATOM_NULL) goto fail;
        if (dyn_j5_ws(p) < 0) { JS_FreeAtom(p->ctx, k); goto fail; }
        if (p->i >= p->n || p->s[p->i] != ':') {
            JS_FreeAtom(p->ctx, k);
            dyn_j5_err(p, "expected ':'");
            goto fail;
        }
        p->i++;
        v = dyn_j5_value(p);
        if (JS_IsException(v)) { JS_FreeAtom(p->ctx, k); goto fail; }
        /* Define, never Set: a document with a `__proto__` key must produce an
           own property rather than retarget the object's prototype. */
        if (JS_DefinePropertyValue(p->ctx, obj, k, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(p->ctx, k);
            goto fail;
        }
        JS_FreeAtom(p->ctx, k);
        if (dyn_j5_ws(p) < 0) goto fail;
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
        dyn_j5_err(p, "expected ',' or '}'");
        goto fail;
    }
    return obj;
 fail:
    JS_FreeValue(p->ctx, obj);
    return JS_EXCEPTION;
}

static JSValue dyn_j5_array(dyn_j5_t *p)
{
    JSValue arr = JS_NewArray(p->ctx);
    uint32_t idx = 0;
    if (JS_IsException(arr))
        return arr;
    p->i++;                                     /* '[' */
    for (;;) {
        JSValue v;
        if (dyn_j5_ws(p) < 0) goto fail;
        if (p->i >= p->n) { dyn_j5_err(p, "unterminated array"); goto fail; }
        if (p->s[p->i] == ']') { p->i++; break; }
        v = dyn_j5_value(p);
        if (JS_IsException(v)) goto fail;
        if (JS_DefinePropertyValueUint32(p->ctx, arr, idx++, v, JS_PROP_C_W_E) < 0)
            goto fail;
        if (dyn_j5_ws(p) < 0) goto fail;
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
        dyn_j5_err(p, "expected ',' or ']'");
        goto fail;
    }
    return arr;
 fail:
    JS_FreeValue(p->ctx, arr);
    return JS_EXCEPTION;
}

static JSValue dyn_j5_value(dyn_j5_t *p)
{
    JSValue v;
    char c;
    if (dyn_j5_ws(p) < 0)
        return JS_EXCEPTION;
    if (p->i >= p->n) {
        dyn_j5_err(p, "unexpected end of input");
        return JS_EXCEPTION;
    }
    /* The depth cap is checked BEFORE descending, so a nest bomb is refused
       rather than discovered when the C stack is gone. */
    if (++p->depth > DYN_J5_MAX_DEPTH) {
        p->depth--;
        JS_ThrowRangeError(p->ctx, "JSON5Parse: nesting deeper than %d",
                           DYN_J5_MAX_DEPTH);
        return JS_EXCEPTION;
    }
    c = p->s[p->i];
    if (c == '{')            v = dyn_j5_object(p);
    else if (c == '[')       v = dyn_j5_array(p);
    else if (c == '"' || c == '\'') v = dyn_j5_string(p, c);
    else if (p->n - p->i >= 4 && memcmp(p->s + p->i, "true", 4) == 0) {
        p->i += 4; v = JS_TRUE;
    } else if (p->n - p->i >= 5 && memcmp(p->s + p->i, "false", 5) == 0) {
        p->i += 5; v = JS_FALSE;
    } else if (p->n - p->i >= 4 && memcmp(p->s + p->i, "null", 4) == 0) {
        p->i += 4; v = JS_NULL;
    } else if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')
               || c == 'I' || c == 'N') {
        v = dyn_j5_number(p);
    } else {
        dyn_j5_err(p, "unexpected token");
        v = JS_EXCEPTION;
    }
    p->depth--;
    return v;
}

/* JSON5Parse(text) -> value. The JSON5 dialect: comments, trailing commas,
   unquoted keys, single quotes, hex, +/-Infinity, NaN, line continuations. */
static JSValue dyn_json5_parse(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_j5_t p;
    const char *src;
    size_t n;
    JSValue v;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "JSON5Parse(text): argument must be a string");
    src = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    p.ctx = ctx; p.s = src; p.n = n; p.i = 0; p.depth = 0;
    v = dyn_j5_value(&p);
    if (!JS_IsException(v)) {
        if (dyn_j5_ws(&p) < 0 || p.i != n) {
            JS_FreeValue(ctx, v);
            if (p.i != n)
                JS_ThrowSyntaxError(ctx, "JSON5Parse: trailing content at offset %u",
                                    (unsigned)p.i);
            v = JS_EXCEPTION;
        }
    }
    JS_FreeCString(ctx, src);
    return v;
}

/* ------------------------------------------------------- stringify (both) */

/* One walker serves JSON5Stringify and StableStringify; `stable` switches on
   RFC 8785 rules (sorted keys, no whitespace, no unquoted keys, reject the
   non-finite numbers JSON5 is happy to print). */
typedef struct {
    JSContext *ctx;
    dyn_buf_t b;
    int stable, indent, depth;
    JSValue *seen;          /* the ancestor chain, for cycle detection */
    int n_seen, cap_seen;
} dyn_ser_t;

static int dyn_ser_value(dyn_ser_t *w, JSValueConst v);

static void dyn_ser_puts(dyn_ser_t *w, const char *s)
{
    while (*s)
        dyn_buf_put(&w->b, (uint8_t)*s++);
}

static void dyn_ser_nl(dyn_ser_t *w, int depth)
{
    int k;
    if (w->stable || w->indent <= 0)
        return;
    dyn_buf_put(&w->b, '\n');
    for (k = 0; k < depth * w->indent; k++)
        dyn_buf_put(&w->b, ' ');
}

/* A JSON string literal. Control characters take the short escape where one
   exists and \u00XX otherwise -- RFC 8785 pins exactly this set. */
static int dyn_ser_string(dyn_ser_t *w, JSValueConst v)
{
    size_t n, i;
    const char *s = JS_ToCStringLen(w->ctx, &n, v);
    if (!s)
        return -1;
    dyn_buf_put(&w->b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!DYN_SER_ESC[c]) {                 /* bulk-copy the clean run */
            size_t run = i;
            while (i < n && !DYN_SER_ESC[(unsigned char)s[i]])
                i++;
            dyn_buf_write(&w->b, (const uint8_t *)s + run, i - run);
            if (i >= n) break;
            c = (unsigned char)s[i];
        }
        switch (c) {
        case '"':  dyn_ser_puts(w, "\\\""); break;
        case '\\': dyn_ser_puts(w, "\\\\"); break;
        case '\b': dyn_ser_puts(w, "\\b");  break;
        case '\f': dyn_ser_puts(w, "\\f");  break;
        case '\n': dyn_ser_puts(w, "\\n");  break;
        case '\r': dyn_ser_puts(w, "\\r");  break;
        case '\t': dyn_ser_puts(w, "\\t");  break;
        default:
            if (c < 0x20) {
                static const char HEX[] = "0123456789abcdef";
                dyn_ser_puts(w, "\\u00");
                dyn_buf_put(&w->b, (uint8_t)HEX[(c >> 4) & 0xF]);
                dyn_buf_put(&w->b, (uint8_t)HEX[c & 0xF]);
            } else {
                dyn_buf_put(&w->b, c);
            }
        }
    }
    dyn_buf_put(&w->b, '"');
    JS_FreeCString(w->ctx, s);
    return 0;
}

/* A number, via the engine's own ToString -- that IS the ECMAScript shortest
   round-trip form RFC 8785 requires, so no separate formatter is needed. */
static int dyn_ser_number(dyn_ser_t *w, JSValueConst v)
{
    double d;
    JSValue sv;
    const char *s;
    if (JS_ToFloat64(w->ctx, &d, v))
        return -1;
    if (!isfinite(d)) {
        if (w->stable) {
            JS_ThrowTypeError(w->ctx,
                "StableStringify: %s has no canonical form (RFC 8785)",
                (d != d) ? "NaN" : "Infinity");
            return -1;
        }
        dyn_ser_puts(w, (d != d) ? "NaN" : (d > 0 ? "Infinity" : "-Infinity"));
        return 0;
    }
    if (d == 0)                     /* RFC 8785: -0 serialises as 0 */
        d = 0;
    sv = JS_NewFloat64(w->ctx, d);
    s = JS_ToCString(w->ctx, sv);
    JS_FreeValue(w->ctx, sv);
    if (!s)
        return -1;
    dyn_ser_puts(w, s);
    JS_FreeCString(w->ctx, s);
    return 0;
}

/* First UTF-16 code unit of a code point. RFC 8785 sorts by UTF-16 units, and
   that is NOT code point order: an astral character encodes as a surrogate
   (0xD800..0xDBFF) and therefore sorts BELOW U+E000..U+FFFF. */
static uint32_t dyn_u16_first(uint32_t cp)
{
    return (cp < 0x10000) ? cp : (0xD800u + ((cp - 0x10000u) >> 10));
}

/* Next code point from UTF-8; advances *i. Malformed bytes decode as
   themselves, which keeps the comparison total rather than throwing. */
static uint32_t dyn_u8_next(const char *s, size_t n, size_t *i)
{
    unsigned char c = (unsigned char)s[(*i)++];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && *i < n)
        return (uint32_t)((c & 0x1F) << 6) | ((unsigned char)s[(*i)++] & 0x3F);
    if ((c & 0xF0) == 0xE0 && *i + 1 < n) {
        uint32_t v = (uint32_t)(c & 0x0F) << 12;
        v |= (uint32_t)((unsigned char)s[(*i)++] & 0x3F) << 6;
        v |= (unsigned char)s[(*i)++] & 0x3F;
        return v;
    }
    if ((c & 0xF8) == 0xF0 && *i + 2 < n) {
        uint32_t v = (uint32_t)(c & 0x07) << 18;
        v |= (uint32_t)((unsigned char)s[(*i)++] & 0x3F) << 12;
        v |= (uint32_t)((unsigned char)s[(*i)++] & 0x3F) << 6;
        v |= (unsigned char)s[(*i)++] & 0x3F;
        return v;
    }
    return c;
}

static int dyn_key_less(JSContext *ctx, JSAtom ka, JSAtom kb)
{
    JSValue va = JS_AtomToString(ctx, ka), vb = JS_AtomToString(ctx, kb);
    const char *sa = NULL, *sb = NULL;
    size_t na = 0, nb = 0, ia = 0, ib = 0;
    int r = 0;

    if (!JS_IsException(va)) sa = JS_ToCStringLen(ctx, &na, va);
    if (!JS_IsException(vb)) sb = JS_ToCStringLen(ctx, &nb, vb);
    if (sa && sb) {
        while (ia < na && ib < nb) {
            /* RFC 8785 orders by UTF-16 code UNITS: an astral code point is a
               surrogate PAIR, so after the high surrogates tie, the LOW one
               decides before the next code point is even looked at. */
            uint32_t ca = dyn_u8_next(sa, na, &ia);
            uint32_t cb = dyn_u8_next(sb, nb, &ib);
            uint32_t ua = dyn_u16_first(ca), ub = dyn_u16_first(cb);
            if (ua != ub) { r = ua < ub; goto out; }
            if (ca >= 0x10000 && cb >= 0x10000) {
                uint32_t la = 0xDC00u + (ca & 0x3FF);
                uint32_t lb = 0xDC00u + (cb & 0x3FF);
                if (la != lb) { r = la < lb; goto out; }
            } else if (ca != cb) {
                /* Same first unit, one astral and one a lone surrogate in
                   D800..DFFF. The BMP key is NOT necessarily complete: its
                   NEXT unit decides against the astral key's low surrogate.
                   Assuming it ended here ordered "\uD800￿" before
                   "\u{10000}" (FFFF vs DC00), which is the wrong canonical
                   byte order -- and JCS output is what gets signed. */
                uint32_t la, lb;
                size_t pk;
                if (ca >= 0x10000) {
                    la = 0xDC00u + (ca & 0x3FF);
                    pk = ib;
                    lb = ib < nb ? dyn_u16_first(dyn_u8_next(sb, nb, &pk)) : 0;
                } else {
                    pk = ia;
                    la = ia < na ? dyn_u16_first(dyn_u8_next(sa, na, &pk)) : 0;
                    lb = 0xDC00u + (cb & 0x3FF);
                }
                if (la != lb) { r = la < lb; goto out; }
                /* both continue identically: fall through to the next unit */
            }
        }
        r = (ia >= na) && (ib < nb);          /* the shorter key sorts first */
    }
 out:
    if (sa) JS_FreeCString(ctx, sa);
    if (sb) JS_FreeCString(ctx, sb);
    JS_FreeValue(ctx, va);
    JS_FreeValue(ctx, vb);
    return r;
}

static int dyn_ser_object(dyn_ser_t *w, JSValueConst v)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i, j;
    int rc = -1, wrote = 0;

    if (JS_GetOwnPropertyNames(w->ctx, &tab, &len, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;
    if (w->stable) {                       /* insertion sort: key sets are small */
        for (i = 1; i < len; i++) {
            JSPropertyEnum t = tab[i];
            j = i;
            while (j > 0 && dyn_key_less(w->ctx, t.atom, tab[j - 1].atom)) {
                tab[j] = tab[j - 1];
                j--;
            }
            tab[j] = t;
        }
    }
    dyn_buf_put(&w->b, '{');
    w->depth++;
    for (i = 0; i < len; i++) {
        JSValue pv = JS_GetProperty(w->ctx, v, tab[i].atom);
        JSValue kv;
        if (JS_IsException(pv))
            goto done;
        if (JS_IsUndefined(pv)) {          /* omitted, as JSON.stringify does */
            JS_FreeValue(w->ctx, pv);
            continue;
        }
        if (wrote)
            dyn_buf_put(&w->b, ',');
        dyn_ser_nl(w, w->depth);
        wrote = 1;
        kv = JS_AtomToString(w->ctx, tab[i].atom);
        if (JS_IsException(kv) || dyn_ser_string(w, kv) < 0) {
            JS_FreeValue(w->ctx, kv);
            JS_FreeValue(w->ctx, pv);
            goto done;
        }
        JS_FreeValue(w->ctx, kv);
        dyn_buf_put(&w->b, ':');
        if (!w->stable && w->indent > 0)
            dyn_buf_put(&w->b, ' ');
        if (dyn_ser_value(w, pv) < 0) {
            JS_FreeValue(w->ctx, pv);
            goto done;
        }
        JS_FreeValue(w->ctx, pv);
    }
    w->depth--;
    if (wrote)
        dyn_ser_nl(w, w->depth);
    dyn_buf_put(&w->b, '}');
    rc = 0;
 done:
    JS_FreePropertyEnum(w->ctx, tab, len);
    return rc;
}

static int dyn_ser_array(dyn_ser_t *w, JSValueConst v)
{
    int64_t len = 0, i;
    JSValue lv = JS_GetPropertyStr(w->ctx, v, "length");
    if (JS_ToInt64(w->ctx, &len, lv)) { JS_FreeValue(w->ctx, lv); return -1; }
    JS_FreeValue(w->ctx, lv);
    dyn_buf_put(&w->b, '[');
    w->depth++;
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(w->ctx, v, (uint32_t)i);
        if (JS_IsException(e))
            return -1;
        if (i)
            dyn_buf_put(&w->b, ',');
        dyn_ser_nl(w, w->depth);
        /* A hole or undefined is null in an array -- JSON.stringify's rule. */
        if (JS_IsUndefined(e) || JS_IsFunction(w->ctx, e)) {
            dyn_ser_puts(w, "null");
        } else if (dyn_ser_value(w, e) < 0) {
            JS_FreeValue(w->ctx, e);
            return -1;
        }
        JS_FreeValue(w->ctx, e);
    }
    w->depth--;
    if (len)
        dyn_ser_nl(w, w->depth);
    dyn_buf_put(&w->b, ']');
    return 0;
}

static int dyn_ser_value(dyn_ser_t *w, JSValueConst v)
{
    int i;
    if (JS_IsNull(v))    { dyn_ser_puts(w, "null");  return 0; }
    if (JS_IsBool(v))    { dyn_ser_puts(w, JS_ToBool(w->ctx, v) ? "true" : "false"); return 0; }
    if (JS_IsNumber(v))  return dyn_ser_number(w, v);
    if (JS_IsString(v))  return dyn_ser_string(w, v);
    if (JS_IsUndefined(v)) { dyn_ser_puts(w, "null"); return 0; }
    if (!JS_IsObject(v)) { dyn_ser_puts(w, "null"); return 0; }

    /* Cycles are detected against the ANCESTOR chain, not everything seen: a
       DAG that repeats a node is legal and must serialise, not throw. */
    for (i = 0; i < w->n_seen; i++)
        if (JS_VALUE_GET_PTR(w->seen[i]) == JS_VALUE_GET_PTR(v)) {
            JS_ThrowTypeError(w->ctx, "%s: cyclic value",
                              w->stable ? "StableStringify" : "JSON5Stringify");
            return -1;
        }
    if (w->n_seen == w->cap_seen) {
        int nc = w->cap_seen ? w->cap_seen * 2 : 16;
        JSValue *ns;
        if (nc > DYN_J5_MAX_DEPTH) {
            JS_ThrowRangeError(w->ctx, "stringify: nesting deeper than %d",
                               DYN_J5_MAX_DEPTH);
            return -1;
        }
        ns = (JSValue *)realloc(w->seen, (size_t)nc * sizeof(JSValue));
        if (!ns) { JS_ThrowOutOfMemory(w->ctx); return -1; }
        w->seen = ns; w->cap_seen = nc;
    }
    w->seen[w->n_seen++] = (JSValue)v;
    {
        int r = JS_IsArray(w->ctx, v) ? dyn_ser_array(w, v) : dyn_ser_object(w, v);
        w->n_seen--;
        return r;
    }
}

/* magic 0 = JSON5Stringify (indent honoured), 1 = StableStringify (RFC 8785). */
static JSValue dyn_stringify(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    dyn_ser_t w;
    JSValue ret;
    int32_t indent = 0;

    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue iv = JS_GetPropertyStr(ctx, argv[1], "indent");
        if (JS_IsException(iv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(iv) && JS_ToInt32(ctx, &indent, iv)) {
            JS_FreeValue(ctx, iv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, iv);
        if (indent < 0) indent = 0;
        if (indent > 10) indent = 10;
    }
    w.ctx = ctx; w.stable = magic; w.indent = indent; w.depth = 0;
    w.seen = NULL; w.n_seen = 0; w.cap_seen = 0;
    dyn_buf_init(&w.b);
    if (dyn_ser_value(&w, argc > 0 ? argv[0] : JS_UNDEFINED) < 0) {
        dyn_buf_free(&w.b);
        free(w.seen);
        return JS_EXCEPTION;
    }
    free(w.seen);
    if (w.b.oom) { dyn_buf_free(&w.b); return JS_ThrowOutOfMemory(ctx); }
    ret = JS_NewStringLen(ctx, (const char *)w.b.p, w.b.n);
    dyn_buf_free(&w.b);
    return ret;
}
