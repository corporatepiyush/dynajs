/* A Mustache-shaped template for dyna:html (design 25). It lives here because
   the thing it must get right is ESCAPING, and this module already owns that
   -- a second escaper is a second thing to keep correct. THERE ARE NO LAMBDAS:
   a template is data, and a data format that can run a function is an
   evaluator wearing a nicer name. Full API: see the dyna:* module in dyna-libc.h. */

#define TPL_MAX_SRC   (4u << 20)
#define TPL_MAX_NODES 65536u
#define TPL_MAX_DEPTH 64

enum { TN_TEXT, TN_VAR, TN_RAW, TN_SECTION, TN_INVERTED };

typedef struct {
    uint8_t  kind;
    uint32_t a, b;                      /* TEXT/name: a span into the source */
    uint32_t body, end;                 /* SECTION: the node range it covers */
} tpl_node_t;

typedef struct {
    char       *src;
    size_t      slen;
    tpl_node_t *node;
    uint32_t    n, cap;
    int         escape;                 /* {{x}} escapes unless told not to */
} tpl_t;

static void tpl_free(tpl_t *t)
{
    if (!t)
        return;
    free(t->node);
    free(t->src);
    free(t);
}

static void tpl_free_v(void *p) { tpl_free((tpl_t *)p); }

static int tpl_emit(tpl_t *t, uint8_t kind, uint32_t a, uint32_t b, uint32_t *at)
{
    if (t->n == t->cap) {
        uint32_t nc = t->cap ? t->cap * 2 : 32;
        tpl_node_t *np;
        if (nc > TPL_MAX_NODES)
            return -1;
        np = (tpl_node_t *)realloc(t->node, nc * sizeof *np);
        if (!np)
            return -1;
        t->node = np; t->cap = nc;
    }
    memset(&t->node[t->n], 0, sizeof t->node[0]);
    t->node[t->n].kind = kind;
    t->node[t->n].a = a;
    t->node[t->n].b = b;
    if (at)
        *at = t->n;
    t->n++;
    return 0;
}

static void tpl_trim(const char *s, uint32_t *a, uint32_t *b)
{
    while (*a < *b && (s[*a] == ' ' || s[*a] == '\t')) (*a)++;
    while (*b > *a && (s[*b - 1] == ' ' || s[*b - 1] == '\t')) (*b)--;
}

/* `{{name}}` `{{{name}}}` `{{&name}}` `{{#s}}` `{{^s}}` `{{/s}}` `{{! note }}`
   and nothing else: `{{> partial}}` needs a loader and `{{=<% %>=}}` changes
   the grammar mid-file, so both are refused rather than half-supported. */
static int tpl_compile(JSContext *ctx, tpl_t *t, char *err, size_t errsz)
{
    size_t i = 0;
    uint32_t stack[TPL_MAX_DEPTH];
    int sp = 0;

    while (i < t->slen) {
        size_t open = i, close;
        uint32_t a, b;
        char sigil;
        int raw = 0;

        while (open + 1 < t->slen && !(t->src[open] == '{' && t->src[open + 1] == '{'))
            open++;
        if (open + 1 >= t->slen) {
            if (tpl_emit(t, TN_TEXT, (uint32_t)i, (uint32_t)t->slen, NULL) < 0)
                goto oom;
            break;
        }
        if (open > i && tpl_emit(t, TN_TEXT, (uint32_t)i, (uint32_t)open, NULL) < 0)
            goto oom;
        a = (uint32_t)(open + 2);
        if (a < t->slen && t->src[a] == '{') { raw = 1; a++; }
        /* `{{{x}}}` closes on THREE braces: scanning for two finds the first
           of them and reads the tag one character short. */
        close = a;
        if (raw) {
            while (close + 2 < t->slen
                   && !(t->src[close] == '}' && t->src[close + 1] == '}'
                        && t->src[close + 2] == '}'))
                close++;
            if (close + 2 >= t->slen) {
                snprintf(err, errsz, "unclosed {{{ at offset %u", (unsigned)open);
                return -1;
            }
            b = (uint32_t)close;
            i = close + 3;
        } else {
            while (close + 1 < t->slen
                   && !(t->src[close] == '}' && t->src[close + 1] == '}'))
                close++;
            if (close + 1 >= t->slen) {
                snprintf(err, errsz, "unclosed {{ at offset %u", (unsigned)open);
                return -1;
            }
            b = (uint32_t)close;
            i = close + 2;
        }
        sigil = a < b ? t->src[a] : 0;
        if (sigil == '!')
            continue;                   /* a comment emits nothing */
        if (sigil == '>' || sigil == '=') {
            snprintf(err, errsz, sigil == '>'
                     ? "{{> partial}} needs a loader this template has no way to call"
                     : "{{=delimiters=}} changes the grammar mid-file");
            return -1;
        }
        if (sigil == '&' || sigil == '#' || sigil == '^' || sigil == '/')
            a++;
        tpl_trim(t->src, &a, &b);
        if (a >= b && sigil != '/') {
            snprintf(err, errsz, "an empty tag at offset %u", (unsigned)open);
            return -1;
        }
        if (sigil == '#' || sigil == '^') {
            uint32_t at;
            if (sp >= TPL_MAX_DEPTH) {
                snprintf(err, errsz, "sections nested deeper than %d", TPL_MAX_DEPTH);
                return -1;
            }
            if (tpl_emit(t, sigil == '#' ? TN_SECTION : TN_INVERTED, a, b, &at) < 0)
                goto oom;
            t->node[at].body = t->n;
            stack[sp++] = at;
            continue;
        }
        if (sigil == '/') {
            uint32_t at;
            if (sp == 0) {
                snprintf(err, errsz, "{{/%.*s}} closes a section that is not open",
                         (int)(b - a), t->src + a);
                return -1;
            }
            at = stack[--sp];
            /* The names must match: a mismatched close is a typo that would
               otherwise silently re-parent everything after it. */
            if (b - a != t->node[at].b - t->node[at].a
                || memcmp(t->src + a, t->src + t->node[at].a, b - a) != 0) {
                snprintf(err, errsz, "{{/%.*s}} closes {{#%.*s}}",
                         (int)(b - a), t->src + a,
                         (int)(t->node[at].b - t->node[at].a),
                         t->src + t->node[at].a);
                return -1;
            }
            t->node[at].end = t->n;
            continue;
        }
        if (tpl_emit(t, raw || sigil == '&' ? TN_RAW : TN_VAR, a, b, NULL) < 0)
            goto oom;
    }
    if (sp > 0) {
        uint32_t at = stack[sp - 1];
        snprintf(err, errsz, "{{#%.*s}} is never closed",
                 (int)(t->node[at].b - t->node[at].a), t->src + t->node[at].a);
        return -1;
    }
    return 0;
oom:
    JS_ThrowOutOfMemory(ctx);
    snprintf(err, errsz, "out of memory");
    return -1;
}

/* A template does NOT know whether its value lands in text, a double-quoted
   attribute or a single-quoted one, so it escapes all five characters that
   could end any of them. `ht_escape` is the context-KNOWN escaper and stays
   as it is; this is a different contract, not a copy of it. */
static const uint8_t TPL_ESC[256] = {
    ['<']=1, ['>']=1, ['&']=1, ['"']=1, ['\'']=1,
};

static void tpl_escape(hb_t *b, const char *s, size_t n)
{
    size_t i = 0, run;

    while (i < n) {
        run = i;
        while (i < n && !TPL_ESC[(unsigned char)s[i]]) i++;
        hb_write(b, s + run, i - run);
        if (i >= n)
            break;
        switch (s[i]) {
        case '<':  hb_puts(b, "&lt;"); break;
        case '>':  hb_puts(b, "&gt;"); break;
        case '&':  hb_puts(b, "&amp;"); break;
        case '"':  hb_puts(b, "&quot;"); break;
        default:   hb_puts(b, "&#39;"); break;
        }
        i++;
    }
}

/* --------------------------------------------------------------- rendering */

typedef struct {
    JSContext *ctx;
    tpl_t     *t;
    hb_t       out;
    JSValue    scope[TPL_MAX_DEPTH];    /* the context stack, outermost first */
    int        depth;
} tpl_r_t;

/* One dotted path, resolved against the innermost scope that HAS its head --
   which is what makes `{{name}}` inside a section see the outer object too.
   Own data properties only: a getter would run code. */
static JSValue tpl_lookup(tpl_r_t *r, const char *path, size_t n)
{
    int d;

    if (n == 1 && path[0] == '.')
        return JS_DupValue(r->ctx, r->scope[r->depth]);
    for (d = r->depth; d >= 0; d--) {
        JSValue cur = JS_DupValue(r->ctx, r->scope[d]);
        size_t i = 0;
        int ok = 1;
        while (i < n && ok) {
            size_t st = i;
            JSAtom key;
            JSPropertyDescriptor desc;
            int got;
            while (i < n && path[i] != '.') i++;
            if (!JS_IsObject(cur)) { ok = 0; break; }
            key = JS_NewAtomLen(r->ctx, path + st, i - st);
            if (key == JS_ATOM_NULL) { ok = 0; break; }
            got = JS_GetOwnProperty(r->ctx, &desc, cur, key);
            JS_FreeAtom(r->ctx, key);
            if (got <= 0 || (desc.flags & JS_PROP_GETSET)) {
                if (got > 0) {
                    JS_FreeValue(r->ctx, desc.getter);
                    JS_FreeValue(r->ctx, desc.setter);
                    JS_FreeValue(r->ctx, desc.value);
                }
                ok = 0;
                break;
            }
            JS_FreeValue(r->ctx, desc.getter);
            JS_FreeValue(r->ctx, desc.setter);
            JS_FreeValue(r->ctx, cur);
            cur = desc.value;
            if (i < n) i++;
        }
        if (ok)
            return cur;
        JS_FreeValue(r->ctx, cur);
    }
    return JS_UNDEFINED;                /* a missing name renders as nothing */
}

/* Mustache falsiness: undefined, null, false, 0, "" and an EMPTY ARRAY. The
   empty array is the one a plain truthiness test gets wrong. */
static int tpl_truthy(JSContext *ctx, JSValueConst v, int64_t *len)
{
    *len = -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (JS_IsArray(ctx, v) == 1) {
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        int64_t n = 0;
        if (!JS_IsException(lv))
            JS_ToInt64(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        *len = n;
        return n > 0;
    }
    return JS_ToBool(ctx, v);
}

static int tpl_run(tpl_r_t *r, uint32_t from, uint32_t to);

static int tpl_section(tpl_r_t *r, const tpl_node_t *nd)
{
    JSValue v = tpl_lookup(r, r->t->src + nd->a, nd->b - nd->a);
    int64_t len;
    int truthy = tpl_truthy(r->ctx, v, &len), rc = 0;

    if (nd->kind == TN_INVERTED) {
        rc = truthy ? 0 : tpl_run(r, nd->body, nd->end);
        JS_FreeValue(r->ctx, v);
        return rc;
    }
    if (!truthy) {
        JS_FreeValue(r->ctx, v);
        return 0;
    }
    if (r->depth + 1 >= TPL_MAX_DEPTH) {
        JS_FreeValue(r->ctx, v);
        JS_ThrowRangeError(r->ctx, "Template: nesting exceeds %d", TPL_MAX_DEPTH);
        return -1;
    }
    if (len >= 0) {                     /* an array renders its body per item */
        int64_t i;
        for (i = 0; i < len && rc == 0; i++) {
            JSValue el = JS_GetPropertyUint32(r->ctx, v, (uint32_t)i);
            if (JS_IsException(el)) { rc = -1; break; }
            r->depth++;
            r->scope[r->depth] = el;
            rc = tpl_run(r, nd->body, nd->end);
            r->depth--;
            JS_FreeValue(r->ctx, el);
        }
    } else {
        r->depth++;
        r->scope[r->depth] = v;
        rc = tpl_run(r, nd->body, nd->end);
        r->depth--;
    }
    JS_FreeValue(r->ctx, v);
    return rc;
}

static int tpl_value(tpl_r_t *r, const tpl_node_t *nd)
{
    JSValue v = tpl_lookup(r, r->t->src + nd->a, nd->b - nd->a);
    const char *s;
    size_t n;

    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(r->ctx, v);
        return 0;                       /* a missing value renders as nothing */
    }
    if (JS_IsFunction(r->ctx, v)) {
        JS_FreeValue(r->ctx, v);
        JS_ThrowTypeError(r->ctx, "Template: %.*s is a function, and a template "
                                   "does not call one",
                          (int)(nd->b - nd->a), r->t->src + nd->a);
        return -1;
    }
    s = JS_ToCStringLen(r->ctx, &n, v);
    JS_FreeValue(r->ctx, v);
    if (!s)
        return -1;
    if (nd->kind == TN_RAW || !r->t->escape)
        hb_write(&r->out, s, n);
    else
        tpl_escape(&r->out, s, n);
    JS_FreeCString(r->ctx, s);
    return 0;
}

static int tpl_run(tpl_r_t *r, uint32_t from, uint32_t to)
{
    uint32_t i = from;

    while (i < to) {
        const tpl_node_t *nd = &r->t->node[i];
        int rc = 0;
        switch (nd->kind) {
        case TN_TEXT:
            hb_write(&r->out, r->t->src + nd->a, nd->b - nd->a);
            break;
        case TN_SECTION:
        case TN_INVERTED:
            rc = tpl_section(r, nd);
            i = nd->end;                /* the body was rendered, or skipped */
            if (rc < 0)
                return -1;
            continue;
        default:
            rc = tpl_value(r, nd);
            break;
        }
        if (rc < 0)
            return -1;
        i++;
    }
    return 0;
}

/* ----------------------------------------------------------- the class */

static JSClassID dyn_tpl_class_id;

static void dyn_tpl_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    tpl_free((tpl_t *)JS_GetOpaque(val, dyn_tpl_class_id));
}

static const JSClassDef dyn_tpl_class = {
    "Template", .finalizer = dyn_tpl_finalizer,
};

static JSValue dyn_tpl_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    tpl_t *t;
    const char *s;
    size_t n;
    char err[160] = { 0 };

    (void)new_target;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "new Template(source): source must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > TPL_MAX_SRC) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "new Template: source exceeds %u bytes",
                                  TPL_MAX_SRC);
    }
    t = (tpl_t *)calloc(1, sizeof *t);
    if (!t) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    t->escape = 1;
    t->src = (char *)malloc(n + 1);
    if (!t->src) {
        JS_FreeCString(ctx, s);
        tpl_free(t);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(t->src, s, n);
    t->src[n] = 0;
    t->slen = n;
    JS_FreeCString(ctx, s);
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "escape");
        if (JS_IsException(v)) { tpl_free(t); return JS_EXCEPTION; }
        if (!JS_IsUndefined(v))
            t->escape = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (tpl_compile(ctx, t, err, sizeof err) < 0) {
        tpl_free(t);
        return JS_ThrowSyntaxError(ctx, "new Template: %s",
                                   err[0] ? err : "out of memory");
    }
    return dyn_plain_wrap(ctx, dyn_tpl_class_id, t, tpl_free_v);
}

static JSValue dyn_tpl_render(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    tpl_t *t = (tpl_t *)dyn_plain_get(ctx, this_val, dyn_tpl_class_id);
    tpl_r_t r;
    JSValue out;

    if (!t)
        return JS_EXCEPTION;
    memset(&r, 0, sizeof r);
    r.ctx = ctx;
    r.t = t;
    hb_init(&r.out);
    r.depth = 0;
    r.scope[0] = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (tpl_run(&r, 0, t->n) < 0 || r.out.oom) {
        if (r.out.oom)
            JS_ThrowOutOfMemory(ctx);
        hb_free(&r.out);
        return JS_EXCEPTION;
    }
    out = JS_NewStringLen(ctx, (const char *)r.out.p, r.out.n);
    hb_free(&r.out);
    return out;
}

static const JSCFunctionListEntry dyn_tpl_proto[] = {
    JS_CFUNC_DEF("render", 0, dyn_tpl_render),
};
