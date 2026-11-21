#include "yaml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_YAML)

#define YML_MAX_INPUT (64u << 20)
#define YML_MAX_LINES 4000000u

static int yml_split(yml_t* p)
{
    size_t i = 0;
    uint32_t cap = 16;

    p->ln = (yml_line_t*)malloc(cap * sizeof *p->ln);
    if (!p->ln)
        return -1;
    while (i <= p->n) {
        yml_line_t L;
        size_t j = i, k, t;
        t = simd.find_u8((const uint8_t*)p->s + j, '\n', p->n - j);
        j = (t == (size_t)-1) ? p->n : j + t;
        L.raw = i;
        L.end = j;
        if (L.end > L.raw && p->s[L.end - 1] == '\r')
            L.end--;
        k = i;
        while (k < L.end && p->s[k] == ' ')
            k++;
        L.indent = (int)(k - i);
        L.start = k;
        {
            size_t kb = k, kw = i;
            int hastab = 0;
            while (kb < L.end && (p->s[kb] == ' ' || p->s[kb] == '\t'))
                kb++;
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
            yml_line_t* np;
            size_t ncap = cap + cap / 2;
            if (ncap > YML_MAX_LINES)
                ncap = YML_MAX_LINES;
            np = (yml_line_t*)realloc(p->ln, ncap * sizeof *p->ln);
            if (!np)
                return -1;
            p->ln = np;
            cap = (uint32_t)ncap;
        }
        p->ln[p->nln++] = L;
        if (j >= p->n)
            break;
        i = j + 1;
        if (i >= p->n)
            break;
    }
    return 0;
}

void yml_kv_abort(yml_t* p, yml_kv_t* b)
{
    int i;
    for (i = 0; i < b->n; i++) {
        JS_FreeAtom(p->ctx, b->atoms[i]);
        JS_FreeValue(p->ctx, b->vals[i]);
    }
    b->n = 0;
    JS_FreeValue(p->ctx, b->out);
    b->out = JS_UNDEFINED;
}

int yml_kv_add(yml_t* p, yml_kv_t* b, JSAtom key, JSValue v)
{
    int i, own;

    if (JS_IsUndefined(b->out)) {
        for (i = 0; i < b->n && b->atoms[i] != key; i++)
            ;
        if (i < b->n)
            goto dup;
        if (b->n < YML_MAP_BATCH) {
            b->atoms[b->n] = key;
            b->vals[b->n++] = v;
            return 0;
        }
        b->out = JS_NewObjectFromProps(p->ctx, NULL, b->n, b->atoms, b->vals);
        b->n = 0;
        if (JS_IsException(b->out)) {
            b->out = JS_UNDEFINED;
            goto fail;
        }
    }
    own = JS_GetOwnProperty(p->ctx, NULL, b->out, key);
    if (own > 0)
        goto dup;
    if (own < 0 || JS_DefinePropertyValue(p->ctx, b->out, key, v, JS_PROP_C_W_E) < 0) {
        JS_FreeAtom(p->ctx, key);
        return -1;
    }
    JS_FreeAtom(p->ctx, key);
    return 0;
dup:
    yml_fail(p, "duplicate mapping key");
fail:
    JS_FreeValue(p->ctx, v);
    JS_FreeAtom(p->ctx, key);
    return -1;
}

JSValue yml_kv_finish(yml_t* p, yml_kv_t* b)
{
    JSValue out = b->out;
    if (!JS_IsUndefined(out))
        return out;
    out = JS_NewObjectFromProps(p->ctx,
        p->depth >= 1 && p->depth <= YML_SHAPE_DEPTH ? &p->shapes[p->depth - 1] : NULL,
        b->n, b->atoms, b->vals);
    b->n = 0;
    return out;
}

static int yml_block_header(yml_t* p, size_t st, size_t end, int* chomp, int* ind)
{
    size_t k;

    *chomp = 0;
    *ind = -1;
    for (k = st + 1; k < end; k++) {
        if (p->s[k] == '-')
            *chomp = -1;
        else if (p->s[k] == '+')
            *chomp = 1;
        else if (p->s[k] >= '1' && p->s[k] <= '9')
            *ind = p->s[k] - '0';
        else if (p->s[k] == ' ')
            continue;
        else if (p->s[k] == '#')
            break;
        else
            return -1;
    }
    return 0;
}

JSValue yml_block_scalar(yml_t* p, int parent_indent, size_t st, size_t end)
{
    int literal = p->s[st] == '|', chomp, ind;
    yb_t b;
    JSValue v;

    if (yml_block_header(p, st, end, &chomp, &ind) < 0)
        return yml_fail(p, "unknown block scalar indicator");
    p->li++;
    yb_init(&b);
    while (p->li < p->nln) {
        const yml_line_t* L = &p->ln[p->li];
        size_t kc = L->start;
        int content;
        while (kc < L->end && (p->s[kc] == ' ' || p->s[kc] == '\t'))
            kc++;
        content = kc < L->end;
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
                b.n--;
            yb_put(&b, (uint8_t)'\n');
        } else {
            size_t from = L->raw + (size_t)(ind < 0 ? 0 : ind);
            if (from > L->end)
                from = L->end;
            yb_write(&b, p->s + from, L->end - from);
            yb_put(&b, (uint8_t)(literal ? '\n' : ' '));
        }
        p->li++;
    }
    if (!literal && b.n && b.p[b.n - 1] == ' ')
        b.p[b.n - 1] = '\n';
    if (chomp < 0) {
        while (b.n && b.p[b.n - 1] == '\n')
            b.n--;
    } else if (chomp == 0) {
        while (b.n > 1 && b.p[b.n - 1] == '\n' && b.p[b.n - 2] == '\n')
            b.n--;
    }
    v = JS_NewStringLen(p->ctx, (const char*)b.p, b.n);
    yb_free(&b);
    return v;
}

static JSAtom yml_map_key(yml_t* p, size_t st, size_t end, size_t* pcolon)
{
    size_t colon = yml_key_colon(p->s, st, end), ke;
    const char* bad;
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
    while (ke > st && (p->s[ke - 1] == ' ' || p->s[ke - 1] == '\t'))
        ke--;
    bad = yml_unsupported(p->s + st, ke - st);
    if (bad) {
        snprintf(p->err, sizeof p->err, "%s at line %u", bad, (unsigned)(p->li + 1));
        return JS_ATOM_NULL;
    }
    if (ke > st && (p->s[st] == '"' || p->s[st] == '\'')) {
        size_t q = st;
        kv = yml_quoted(p, &q, ke, 0);
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

JSValue yml_map(yml_t* p, int indent)
{
    yml_kv_t kv;

    kv.n = 0;
    kv.out = JS_UNDEFINED;
    for (;;) {
        size_t st, end, colon = 0, vs;
        JSValue v;
        JSAtom key;

        yml_skip_blank(p);
        if (p->li >= p->nln || p->ln[p->li].indent < indent)
            break;
        if (yml_tab_refuse(p, p->li))
            goto fail;
        st = p->ln[p->li].start;
        end = p->ln[p->li].end;
        if (p->s[st] == '-' && (st + 1 >= end || p->s[st + 1] == ' '))
            break;
        if (yml_marker_at(p, p->li, "---") || yml_marker_at(p, p->li, "..."))
            break;
        if (p->ln[p->li].indent > indent) {
            yml_fail(p, "unexpected indentation");
            goto fail;
        }
        key = yml_map_key(p, st, end, &colon);
        if (key == JS_ATOM_NULL)
            goto fail;
        vs = colon + 1;
        while (vs < end && (p->s[vs] == ' ' || p->s[vs] == '\t'))
            vs++;
        if (vs >= end || p->s[vs] == '#') {
            p->li++;
            v = yml_node(p, indent + 1);
        } else {
            v = yml_inline_value(p, indent, vs, end);
        }
        if (JS_IsException(v)) {
            JS_FreeAtom(p->ctx, key);
            goto fail;
        }
        if (yml_kv_add(p, &kv, key, v) < 0)
            goto fail;
    }
    return yml_kv_finish(p, &kv);
fail:
    yml_kv_abort(p, &kv);
    return JS_EXCEPTION;
}

static int yml_start(JSContext* ctx, yml_t* p, JSValueConst arg, char** out_buf)
{
    const char* src;
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

static const char* const yml_parse_keys[] = { "maxDepth", "schema", "bigint" };

static int yml_read_opts(JSContext* ctx, JSValueConst o, int* max_depth,
    int* bigint, const char* pfx)
{
    JSValue v;

    *max_depth = YML_MAX_DEPTH;
    *bigint = 0;
    if (JS_IsUndefined(o) || JS_IsNull(o))
        return 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "%s(text, options): options must be an object",
            pfx);
        return -1;
    }
    if (dyn_opts_strict(ctx, o, yml_parse_keys,
            countof(yml_parse_keys))
        < 0)
        return -1;
    if (dyn_opt_bool(ctx, o, "bigint", pfx, bigint) < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, o, "maxDepth");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        double d;
        if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) || !(d >= 1 && d <= (double)YML_MAX_DEPTH) || d != (double)(int64_t)d) {
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
        const char* s = JS_ToCString(ctx, v);
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
                "this parser resolves the core schema only",
                pfx);
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

JSValue yml_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    yml_t p;
    char* buf;
    JSValue out;
    int max_depth, bigint;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(text): text is required",
            magic ? "ParseAll" : "Parse");
    if (yml_read_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &max_depth, &bigint, magic ? "ParseAll" : "Parse")
        < 0)
        return JS_EXCEPTION;
    if (yml_start(ctx, &p, argv[0], &buf) < 0)
        return JS_EXCEPTION;
    p.max_depth = max_depth;
    p.bigint = bigint;
    if (magic == 0) {
        out = yml_one(&p);
        if (!JS_IsException(out)) {
            int ended = 0;
            yml_skip_blank(&p);
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
                yml_fail(&p, ended || yml_at_doc_start(&p) ? "the input holds more than one document -- use ParseAll" : "unexpected content after the document");
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
    yml_shapes_free(JS_GetRuntime(ctx), &p);
    free(p.ln);
    js_free(ctx, buf);
    return out;
}

static int yml_prop_cmp(const void* a, const void* b)
{
    return strcmp(((const yml_prop_t*)a)->name, ((const yml_prop_t*)b)->name);
}

int yml_props(JSContext* ctx, JSValueConst v, int sort,
    yml_prop_t** out, uint32_t* outn)
{
    JSPropertyEnum* tab = NULL;
    uint32_t n = 0, ntab = 0, k;
    yml_prop_t* t;

    if (JS_GetOwnPropertyNames(ctx, &tab, &ntab, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    n = ntab;
    t = (yml_prop_t*)malloc(n ? n * sizeof *t : sizeof *t);
    if (!t) {
        JS_FreePropertyEnum(ctx, tab, n);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (k = 0; k < n; k++) {
        size_t l = 0;
        const char* s = JS_AtomToCStringLen(ctx, &l, tab[k].atom);
        t[k].e = tab[k];
        t[k].e.atom = JS_DupAtom(ctx, tab[k].atom);
        t[k].name = NULL;
        if (!s) {
            n = k + 1;
            goto fail;
        }
        t[k].name = (char*)malloc(l + 1);
        if (!t[k].name) {
            JS_FreeCString(ctx, s);
            n = k + 1;
            goto fail;
        }
        memcpy(t[k].name, s, l + 1);
        JS_FreeCString(ctx, s);
    }
    JS_FreePropertyEnum(ctx, tab, ntab);
    if (sort)
        qsort(t, n, sizeof *t, yml_prop_cmp);
    *out = t;
    *outn = n;
    return 0;
fail:
    for (k = 0; k < n; k++) {
        JS_FreeAtom(ctx, t[k].e.atom);
        free(t[k].name);
    }
    free(t);
    if (tab)
        JS_FreePropertyEnum(ctx, tab, ntab);
    return -1;
}

void yml_props_free(JSContext* ctx, yml_prop_t* t, uint32_t n)
{
    uint32_t k;
    if (!t)
        return;
    for (k = 0; k < n; k++) {
        JS_FreeAtom(ctx, t[k].e.atom);
        free(t[k].name);
    }
    free(t);
}

int yml_is_block(JSContext* ctx, JSValueConst v, int64_t* plen)
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
        JSPropertyEnum* tab = NULL;
        uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            return -1;
        JS_FreePropertyEnum(ctx, tab, n);
        *plen = n;
        return n > 0 ? 1 : 0;
    }
}

JSValue yml_parse_stream(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ystream_t* st;
    JSRuntime* rt;
    JSValue proto, obj;
    int max_depth, bigint;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ParseStream(text): text is required");
    if (yml_read_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            &max_depth, &bigint, "ParseStream")
        < 0)
        return JS_EXCEPTION;
    st = (ystream_t*)calloc(1, sizeof *st);
    if (!st)
        return JS_ThrowOutOfMemory(ctx);
    st->ctx = ctx;
    st->err_val = JS_UNDEFINED;
    if (yml_start(ctx, &st->p, argv[0], &st->buf) < 0) {
        ystream_free(JS_GetRuntime(ctx), st);
        return JS_EXCEPTION;
    }
    st->p.max_depth = max_depth;
    st->p.bigint = bigint;
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

#endif
