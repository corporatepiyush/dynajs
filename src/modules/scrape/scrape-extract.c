#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)
typedef struct {
    char* name;
    JSValue sel;
    JSValue dflt;
    char* attr;
    unsigned all : 1;
    unsigned required : 1;
    unsigned trim : 1;
    unsigned source : 1;
    unsigned as_number : 1;
    unsigned as_url : 1;
    unsigned as_json : 1;
} ex_field_t;

typedef struct {
    ex_field_t* f;
    size_t n;
    JSValue html_text;
} ex_t;

#define EX_SOURCE_CAP (1u << 20)

typedef struct {
    char* p;
    size_t n;
} ex_chunk_t;

static const char* const sc_ex_ctor_keys[] = { "text" };

static const char* const sc_field_keys[] = { "sel", "attr", "all", "required",
    "trim", "source", "default", "as" };

static const char* const sc_run_keys[] = { "base" };

static void ex_free_ctx(JSContext* ctx, ex_t* e)
{
    size_t i;
    for (i = 0; i < e->n; i++) {
        free(e->f[i].name);
        free(e->f[i].attr);
        JS_FreeValue(ctx, e->f[i].sel);
        JS_FreeValue(ctx, e->f[i].dflt);
    }
    free(e->f);
    JS_FreeValue(ctx, e->html_text);
    free(e);
}

void dyn_ex_finalizer(JSRuntime* rt, JSValue val)
{
    ex_t* e = JS_GetOpaque(val, dyn_ex_class_id);
    size_t i;
    if (!e)
        return;
    for (i = 0; i < e->n; i++) {
        free(e->f[i].name);
        free(e->f[i].attr);
        JS_FreeValueRT(rt, e->f[i].sel);
        JS_FreeValueRT(rt, e->f[i].dflt);
    }
    free(e->f);
    JS_FreeValueRT(rt, e->html_text);
    free(e);
}

void dyn_ex_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    ex_t* e = JS_GetOpaque(val, dyn_ex_class_id);
    size_t i;
    if (!e)
        return;
    for (i = 0; i < e->n; i++) {
        JS_MarkValue(rt, e->f[i].sel, mark);
        JS_MarkValue(rt, e->f[i].dflt, mark);
    }
    JS_MarkValue(rt, e->html_text, mark);
}

static char* ex_dup_str(JSContext* ctx, JSValueConst v)
{
    const char* s = JS_ToCString(ctx, v);
    char* o;
    if (!s)
        return NULL;
    o = strdup(s);
    JS_FreeCString(ctx, s);
    return o;
}

static size_t ex_trimmed(const char* s, size_t n, size_t* ofs)
{
    size_t a = 0, e = n;
    while (a < n && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        a++;
    while (e > a && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        e--;
    *ofs = a;
    return e - a;
}

static int ex_concat_kids(JSContext* ctx, JSValueConst node, int depth,
    ex_chunk_t** chunks, size_t* n_ch, size_t* cap_ch,
    size_t* total)
{
    JSValue kids;
    uint32_t n = 0, i;

    if (depth > 64)
        return 0;
    if (!JS_IsObject(node))
        return 0;
    kids = JS_GetPropertyStr(ctx, node, "children");
    if (JS_IsException(kids))
        return -1;
    if (!JS_IsArray(ctx, kids)) {
        JS_FreeValue(ctx, kids);
        return 0;
    }
    if (dyn_js_len_u32(ctx, kids, &n)) {
        JS_FreeValue(ctx, kids);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, kids, i);
        if (JS_IsException(c)) {
            JS_FreeValue(ctx, kids);
            return -1;
        }
        if (JS_IsString(c)) {
            const char* t;
            size_t tn;
            t = JS_ToCStringLen(ctx, &tn, c);
            if (!t) {
                JS_FreeValue(ctx, c);
                JS_FreeValue(ctx, kids);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            if (*n_ch == *cap_ch) {
                size_t nc = *cap_ch ? *cap_ch * 2 : 8;
                ex_chunk_t* na = (ex_chunk_t*)
                    realloc(*chunks, nc * sizeof(ex_chunk_t));
                if (!na) {
                    JS_FreeCString(ctx, t);
                    JS_FreeValue(ctx, c);
                    JS_FreeValue(ctx, kids);
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                *chunks = na;
                *cap_ch = nc;
            }
            {
                ex_chunk_t* ch = &(*chunks)[(*n_ch)++];
                ch->p = (char*)malloc(tn + 1);
                if (!ch->p) {
                    (*n_ch)--;
                    JS_FreeCString(ctx, t);
                    JS_FreeValue(ctx, c);
                    JS_FreeValue(ctx, kids);
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                memcpy(ch->p, t, tn);
                ch->p[tn] = 0;
                ch->n = tn;
            }
            *total += tn;
            JS_FreeCString(ctx, t);
            if (*total > EX_SOURCE_CAP) {
                JS_FreeValue(ctx, c);
                JS_FreeValue(ctx, kids);
                return 0;
            }
        } else if (JS_IsObject(c)) {
            int rc = ex_concat_kids(ctx, c, depth + 1,
                chunks, n_ch, cap_ch, total);
            if (rc < 0) {
                JS_FreeValue(ctx, c);
                JS_FreeValue(ctx, kids);
                return -1;
            }
        }
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, kids);
    return 0;
}

static JSValue ex_node_source(JSContext* ctx, JSValueConst node)
{
    ex_chunk_t* chunks = NULL;
    size_t n_ch = 0, cap_ch = 0, total = 0, i, o = 0;
    char* joined;
    JSValue out;

    if (ex_concat_kids(ctx, node, 0, &chunks, &n_ch, &cap_ch, &total) < 0) {
        size_t z;
        for (z = 0; z < n_ch; z++)
            free(chunks[z].p);
        free(chunks);
        return JS_EXCEPTION;
    }
    joined = (char*)malloc(total + 1);
    if (!joined) {
        for (i = 0; i < n_ch; i++)
            free(chunks[i].p);
        free(chunks);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n_ch; i++) {
        memcpy(joined + o, chunks[i].p, chunks[i].n);
        o += chunks[i].n;
        free(chunks[i].p);
    }
    free(chunks);
    joined[o] = 0;
    out = JS_NewStringLen(ctx, joined, o);
    free(joined);
    return out;
}

JSValue dyn_ex_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    ex_t* e;
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    JSValue obj, proto, ht = JS_UNDEFINED;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Extractor(spec) needs an object");
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return JS_EXCEPTION;
    e = (ex_t*)calloc(1, sizeof(*e));
    if (!e) {
        JS_FreePropertyEnum(ctx, tab, len);
        return JS_ThrowOutOfMemory(ctx);
    }
    e->html_text = JS_UNDEFINED;
    e->f = (ex_field_t*)calloc(len ? len : 1, sizeof(ex_field_t));
    if (!e->f) {
        free(e);
        JS_FreePropertyEnum(ctx, tab, len);
        return JS_ThrowOutOfMemory(ctx);
    }

    for (i = 0; i < len; i++) {
        JSValue fv = JS_GetProperty(ctx, argv[0], tab[i].atom), v;
        ex_field_t* f = &e->f[e->n];
        const char* nm;
        int b;
        memset(f, 0, sizeof(*f));
        e->n++;
        f->dflt = JS_UNDEFINED;
        if (JS_IsException(fv))
            goto fail;
        if (!JS_IsObject(fv)) {
            JS_FreeValue(ctx, fv);
            JS_ThrowTypeError(ctx, "Extractor: each field must be an object");
            goto fail;
        }
        nm = JS_AtomToCString(ctx, tab[i].atom);
        f->name = nm ? strdup(nm) : NULL;
        if (nm)
            JS_FreeCString(ctx, nm);
        if (dyn_opts_strict(ctx, fv, sc_field_keys, 8)) {
            JS_FreeValue(ctx, fv);
            goto fail;
        }
        f->sel = JS_GetPropertyStr(ctx, fv, "sel");
        if (JS_IsException(f->sel)) {
            f->sel = JS_UNDEFINED;
            JS_FreeValue(ctx, fv);
            goto fail;
        }
        if (!JS_IsObject(f->sel)) {
            JS_FreeValue(ctx, fv);
            JS_ThrowTypeError(ctx, "Extractor: field `%s` needs a Selector as `sel`",
                f->name ? f->name : "?");
            goto fail;
        }
        v = JS_GetPropertyStr(ctx, fv, "attr");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, fv);
            goto fail;
        }
        if (JS_IsString(v))
            f->attr = ex_dup_str(ctx, v);
        JS_FreeValue(ctx, v);
        b = dyn_get_truthy(ctx, fv, "all");
        if (b < 0) {
            JS_FreeValue(ctx, fv);
            goto fail;
        }
        f->all = b ? 1u : 0u;
        b = dyn_get_truthy(ctx, fv, "required");
        if (b < 0) {
            JS_FreeValue(ctx, fv);
            goto fail;
        }
        f->required = b ? 1u : 0u;
        b = dyn_get_truthy(ctx, fv, "trim");
        if (b < 0) {
            JS_FreeValue(ctx, fv);
            goto fail;
        }
        f->trim = b ? 1u : 0u;
        b = dyn_get_truthy(ctx, fv, "source");
        if (b < 0) {
            JS_FreeValue(ctx, fv);
            goto fail;
        }
        f->source = b ? 1u : 0u;
        v = JS_GetPropertyStr(ctx, fv, "default");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, fv);
            goto fail;
        }
        if (!JS_IsUndefined(v))
            f->dflt = v;
        else
            JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, fv, "as");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, fv);
            goto fail;
        }
        if (JS_IsString(v)) {
            const char* as = JS_ToCString(ctx, v);
            if (as) {
                if (!strcmp(as, "number"))
                    f->as_number = 1u;
                else if (!strcmp(as, "url"))
                    f->as_url = 1u;
                else if (!strcmp(as, "json"))
                    f->as_json = 1u;
                else {
                    char bad[40];
                    snprintf(bad, sizeof bad, "%.39s", as);
                    JS_FreeCString(ctx, as);
                    JS_FreeValue(ctx, v);
                    JS_FreeValue(ctx, fv);
                    JS_ThrowTypeError(ctx,
                        "Extractor: field `%s` as:\"%s\" is not supported "
                        "(supported: number, url, json)",
                        f->name ? f->name : "?", bad);
                    goto fail;
                }
                JS_FreeCString(ctx, as);
            }
        }
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, fv);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    tab = NULL;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], sc_ex_ctor_keys, 1)) {
            ex_free_ctx(ctx, e);
            return JS_EXCEPTION;
        }
        ht = JS_GetPropertyStr(ctx, argv[1], "text");
        if (JS_IsException(ht)) {
            ex_free_ctx(ctx, e);
            return JS_EXCEPTION;
        }
        if (JS_IsFunction(ctx, ht))
            e->html_text = ht;
        else
            JS_FreeValue(ctx, ht);
    }

    proto = dyn_ctor_proto(ctx, new_target, dyn_ex_class_id);
    if (JS_IsException(proto)) {
        ex_free_ctx(ctx, e);
        return proto;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, (int)dyn_ex_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) {
        ex_free_ctx(ctx, e);
        return obj;
    }
    JS_SetOpaque(obj, e);
    return obj;
fail:
    if (tab)
        JS_FreePropertyEnum(ctx, tab, len);
    ex_free_ctx(ctx, e);
    return JS_EXCEPTION;
}

JSValue dyn_ex_run(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ex_t* e = JS_GetOpaque2(ctx, this_val, dyn_ex_class_id);
    JSValue out, val, missing, base = JS_UNDEFINED;
    const char* base_s = NULL;
    size_t i;
    int ok = 1;
    uint32_t nmiss = 0;

    if (!e)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "run(doc[, { base }])");
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], sc_run_keys, 1))
            return JS_EXCEPTION;
        base = JS_GetPropertyStr(ctx, argv[1], "base");
        if (JS_IsException(base))
            return JS_EXCEPTION;
        if (JS_IsString(base)) {
            base_s = JS_ToCString(ctx, base);
            if (!base_s) {
                JS_FreeValue(ctx, base);
                return JS_EXCEPTION;
            }
        }
    }

    out = JS_NewObject(ctx);
    val = JS_NewObject(ctx);
    missing = JS_NewArray(ctx);
    if (JS_IsException(out) || JS_IsException(val) || JS_IsException(missing))
        goto fail;

    for (i = 0; i < e->n; i++) {
        ex_field_t* f = &e->f[i];
        JSValue m, nodes, res;
        uint32_t cnt = 0, j;

        m = JS_GetPropertyStr(ctx, f->sel, "all");
        if (JS_IsException(m))
            goto fail;
        if (!JS_IsFunction(ctx, m)) {
            JS_FreeValue(ctx, m);
            JS_ThrowTypeError(ctx, "Extractor: `%s`.sel is not a Selector",
                f->name ? f->name : "?");
            goto fail;
        }
        nodes = JS_Call(ctx, m, f->sel, 1, (JSValueConst*)&argv[0]);
        JS_FreeValue(ctx, m);
        if (JS_IsException(nodes))
            goto fail;
        if (dyn_js_len_u32(ctx, nodes, &cnt)) {
            JS_FreeValue(ctx, nodes);
            goto fail;
        }

        res = f->all ? JS_NewArray(ctx) : JS_UNDEFINED;
        if (JS_IsException(res)) {
            JS_FreeValue(ctx, nodes);
            goto fail;
        }
        for (j = 0; j < cnt; j++) {
            JSValue node = JS_GetPropertyUint32(ctx, nodes, j), piece;
            if (f->attr) {
                JSValue at = JS_GetPropertyStr(ctx, node, "attrs");
                piece = JS_IsException(at) ? JS_EXCEPTION
                    : JS_IsObject(at)      ? JS_GetPropertyStr(ctx, at, f->attr)
                                           : JS_UNDEFINED;
                JS_FreeValue(ctx, at);
            } else if (f->source) {
                piece = ex_node_source(ctx, node);
            } else if (JS_IsFunction(ctx, e->html_text)) {
                JSValueConst a1[1] = { node };
                piece = JS_Call(ctx, e->html_text, JS_UNDEFINED, 1, a1);
            } else {
                JS_FreeValue(ctx, node);
                JS_FreeValue(ctx, res);
                JS_FreeValue(ctx, nodes);
                JS_ThrowTypeError(ctx,
                    "Extractor: field `%s` wants text; pass { text: HTMLText } "
                    "to the constructor, or take attr/source instead",
                    f->name ? f->name : "?");
                goto fail;
            }
            JS_FreeValue(ctx, node);
            if (JS_IsException(piece)) {
                JS_FreeValue(ctx, res);
                JS_FreeValue(ctx, nodes);
                goto fail;
            }
            if (f->trim && !JS_IsUndefined(piece)) {
                const char* ts;
                size_t tn2, ofs;
                if (JS_IsString(piece)) {
                    ts = JS_ToCStringLen(ctx, &tn2, piece);
                    if (ts) {
                        size_t tn3 = ex_trimmed(ts, tn2, &ofs);
                        JSValue nt = JS_NewStringLen(ctx, ts + ofs, tn3);
                        JS_FreeCString(ctx, ts);
                        JS_FreeValue(ctx, piece);
                        piece = nt;
                    }
                }
            }
            if (f->as_number && !JS_IsUndefined(piece)) {
                double d;
                if (JS_ToFloat64(ctx, &d, piece) < 0) {
                    JS_FreeValue(ctx, piece);
                    piece = JS_EXCEPTION;
                } else if (d != d) {
                    JS_FreeValue(ctx, piece);
                    piece = JS_UNDEFINED;
                } else {
                    JS_FreeValue(ctx, piece);
                    piece = JS_NewFloat64(ctx, d);
                }
            }
            if (f->as_json && !JS_IsUndefined(piece)) {
                if (JS_IsString(piece)) {
                    const char* js;
                    size_t jl;
                    js = JS_ToCStringLen(ctx, &jl, piece);
                    if (js) {
                        JSValue jv = JS_ParseJSON(ctx, js, jl, "<extractor>");
                        JS_FreeCString(ctx, js);
                        if (JS_IsException(jv)) {
                            JS_FreeValue(ctx, piece);
                            piece = dyn_take_syntax_error(ctx) ? JS_UNDEFINED : JS_EXCEPTION;
                        } else {
                            JS_FreeValue(ctx, piece);
                            piece = jv;
                        }
                    }
                } else {
                    JS_FreeValue(ctx, piece);
                    piece = JS_UNDEFINED;
                }
            }
            if (f->as_url && !JS_IsUndefined(piece) && JS_IsString(piece)) {
                const char* us;
                size_t un;
                us = JS_ToCStringLen(ctx, &un, piece);
                if (!us || (!strncasecmp(us, "http://", 7) || !strncasecmp(us, "https://", 8))) {
                    if (us)
                        JS_FreeCString(ctx, us);
                } else {
                    char rbuf[1024];
                    long rl = base_s ? fe_resolve(base_s, us, rbuf,
                                           sizeof rbuf)
                                     : -1;
                    JS_FreeCString(ctx, us);
                    if (rl > 0) {
                        JSValue nv = JS_NewStringLen(ctx, rbuf, (size_t)rl);
                        JS_FreeValue(ctx, piece);
                        piece = nv;
                    } else {
                        JS_FreeValue(ctx, piece);
                        piece = JS_UNDEFINED;
                    }
                }
            }
            if (JS_IsException(piece) || JS_HasException(ctx)) {
                JS_FreeValue(ctx, piece);
                JS_FreeValue(ctx, res);
                JS_FreeValue(ctx, nodes);
                goto fail;
            }
            if (f->all) {
                if (JS_DefinePropertyValueUint32(ctx, res, j, piece, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, res);
                    JS_FreeValue(ctx, nodes);
                    goto fail;
                }
            } else {
                res = piece;
                break;
            }
        }
        JS_FreeValue(ctx, nodes);
        if (!f->all && cnt == 0)
            res = JS_UNDEFINED;

        if (!f->all && JS_IsUndefined(res) && !JS_IsUndefined(f->dflt))
            res = JS_DupValue(ctx, f->dflt);
        else if (f->all && cnt == 0 && !JS_IsUndefined(f->dflt)) {
            JS_FreeValue(ctx, res);
            res = JS_DupValue(ctx, f->dflt);
        }

        if (JS_IsUndefined(f->dflt) && (cnt == 0 || (!f->all && JS_IsUndefined(res))) && f->required) {
            ok = 0;
            if (JS_DefinePropertyValueUint32(ctx, missing, nmiss++,
                    JS_NewString(ctx, f->name ? f->name : "?"), JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, res);
                goto fail;
            }
        }
        if (JS_DefinePropertyValueStr(ctx, val, f->name ? f->name : "?", res, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    JS_FreeCString(ctx, base_s);
    JS_FreeValue(ctx, base);
    {
        int r1 = JS_DefinePropertyValueStr(ctx, out, "ok", JS_NewBool(ctx, ok), JS_PROP_C_W_E);
        int r2 = JS_DefinePropertyValueStr(ctx, out, "value", val, JS_PROP_C_W_E);
        int r3 = JS_DefinePropertyValueStr(ctx, out, "missing", missing, JS_PROP_C_W_E);
        if (r1 < 0 || r2 < 0 || r3 < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    return out;
fail:
    JS_FreeCString(ctx, base_s);
    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, missing);
    return JS_EXCEPTION;
}

static int fe_link_rel_canonical(const char* p, size_t n, size_t eq)
{
    size_t j = eq + 1;
    while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == '"' || p[j] == '\''))
        j++;
    while (j < n) {
        size_t k = j;
        while (k < n && p[k] != ',' && p[k] != ';' && p[k] != ' ' && p[k] != '\t' && p[k] != '"' && p[k] != '\'')
            k++;
        if (k - j == 9 && !strncasecmp(p + j, "canonical", 9))
            return 1;
        j = k;
        while (j < n && (p[j] == ' ' || p[j] == '\t' || p[j] == ',' || p[j] == '"' || p[j] == '\''))
            j++;
        if (j >= n || p[j] == ';')
            break;
    }
    return 0;
}

void fe_link_canonical(JSContext* ctx, JSValueConst res,
    const char* base, JSValue out)
{
    char* all = fe_headers_all(ctx, res, "link");
    size_t i = 0;
    char* url = NULL;
    if (!all)
        return;
    while (all[i]) {
        if (all[i] == '<') {
            size_t e = i + 1, p, pend, k;
            while (all[e] && all[e] != '>' && all[e] != '\n')
                e++;
            if (all[e] != '>') {
                i++;
                continue;
            }
            p = e + 1;
            pend = p;
            while (all[pend] && all[pend] != ',' && all[pend] != '\n')
                pend++;
            for (k = p; k + 4 <= pend; k++)
                if ((all[k] == 'r' || all[k] == 'R') && (all[k + 1] == 'e' || all[k + 1] == 'E') && (all[k + 2] == 'l' || all[k + 2] == 'L') && all[k + 3] == '=' && fe_link_rel_canonical(all + p, pend - p, (k + 3) - p)) {
                    size_t u0 = i + 1, u1 = e;
                    while (u0 < u1 && (all[u0] == ' ' || all[u0] == '\t'))
                        u0++;
                    while (u1 > u0 && (all[u1 - 1] == ' ' || all[u1 - 1] == '\t'))
                        u1--;
                    url = (char*)malloc(u1 - u0 + 1);
                    if (url) {
                        memcpy(url, all + u0, u1 - u0);
                        url[u1 - u0] = 0;
                    }
                    break;
                }
            i = pend;
            if (url)
                break;
            continue;
        }
        i++;
    }
    free(all);
    if (!url)
        return;
    {
        char rbuf[1024];
        long rl = fe_resolve(base, url, rbuf, sizeof rbuf);
        free(url);
        if (rl > 0 && (!strncmp(rbuf, "http://", 7) || !strncmp(rbuf, "https://", 8)))
            JS_DefinePropertyValueStr(ctx, out, "canonicalUrl",
                JS_NewStringLen(ctx, rbuf, (size_t)rl),
                JS_PROP_C_W_E);
    }
}
#endif
