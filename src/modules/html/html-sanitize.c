#include "html-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTML)

typedef struct {
    JSContext* ctx;
    const san_t* s;
    hb_t b;
    int depth;
    ht_keys_t k;
} san_w_t;

void dyn_san_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    san_free((san_t*)JS_GetOpaque(val, dyn_san_class_id));
}

static int san_read_attrs(JSContext* ctx, JSValueConst list, san_rule_t* r)
{
    JSValue lv;
    int64_t an = 0, k;

    if (JS_IsArray(ctx, list) == 1) {
        lv = JS_GetPropertyStr(ctx, list, "length");
        if (JS_IsException(lv) || ht_len(ctx, lv, &an))
            return -1;
    }
    r->attrs = (char**)calloc(an ? (size_t)an : 1, sizeof(char*));
    if (!r->attrs)
        return -1;
    for (k = 0; k < an; k++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, (uint32_t)k);
        const char* as = JS_IsException(e) ? NULL : JS_ToCString(ctx, e);
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

static int san_read_allow(JSContext* ctx, JSValueConst o, san_t* s)
{
    JSValue allow = JS_GetPropertyStr(ctx, o, "allow");
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    int rc = 0;

    if (JS_IsException(allow))
        return -1;
    if (!JS_IsObject(allow)) {
        JS_FreeValue(ctx, allow);
        JS_ThrowTypeError(ctx, "new Sanitizer: `allow` maps a tag to its "
                               "permitted attributes");
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, allow,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(ctx, allow);
        return -1;
    }
    s->rules = (san_rule_t*)calloc(len ? len : 1, sizeof *s->rules);
    if (!s->rules)
        rc = -1;
    for (i = 0; i < len && rc == 0; i++) {
        JSValue list = JS_GetProperty(ctx, allow, tab[i].atom);
        const char* tn = JS_AtomToCString(ctx, tab[i].atom);
        if (!tn || JS_IsException(list)) {
            rc = -1;
        } else {
            s->rules[s->n].tag = san_lower_dup(tn);
            rc = s->rules[s->n].tag ? san_read_attrs(ctx, list, &s->rules[s->n])
                                    : -1;
            s->n++;
        }
        if (tn)
            JS_FreeCString(ctx, tn);
        JS_FreeValue(ctx, list);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    JS_FreeValue(ctx, allow);
    return rc;
}

static int san_read_protocols(JSContext* ctx, JSValueConst o, san_t* s)
{
    JSValue pr = JS_GetPropertyStr(ctx, o, "protocols");
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i, cap;
    int rc = 0;

    if (JS_IsException(pr))
        return -1;
    if (!JS_IsObject(pr)) {
        JS_FreeValue(ctx, pr);
        return 0;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, pr,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(ctx, pr);
        return -1;
    }
    cap = len * 8 + 2;
    s->protos = (char**)calloc(cap, sizeof(char*));
    if (!s->protos) {
        rc = -1;
        goto done;
    }
    for (i = 0; i < len && rc == 0; i++) {
        JSValue list = JS_GetProperty(ctx, pr, tab[i].atom), lv;
        const char* key = JS_AtomToCString(ctx, tab[i].atom);
        int64_t an = 0, k;
        if (!key || JS_IsException(list)) {
            rc = -1;
            JS_FreeValue(ctx, list);
            break;
        }
        if (JS_IsArray(ctx, list) == 1) {
            lv = JS_GetPropertyStr(ctx, list, "length");
            if (JS_IsException(lv) || ht_len(ctx, lv, &an)) {
                rc = -1;
                JS_FreeCString(ctx, key);
                JS_FreeValue(ctx, list);
                break;
            }
        }
        for (k = 0; k < an && rc == 0; k++) {
            JSValue e = JS_GetPropertyUint32(ctx, list, (uint32_t)k);
            const char* ps = JS_IsException(e) ? NULL : JS_ToCString(ctx, e);
            JS_FreeValue(ctx, e);
            if (ps && s->nproto + 2 > cap) {
                size_t ncap = cap * 2 + 8;
                char** grown = (char**)realloc(s->protos, ncap * sizeof(char*));
                if (!grown) {
                    JS_FreeCString(ctx, ps);
                    JS_ThrowOutOfMemory(ctx);
                    rc = -1;
                    break;
                }
                memset(grown + cap, 0, (ncap - cap) * sizeof(char*));
                s->protos = grown;
                cap = ncap;
            }
            if (!ps) {
                rc = -1;
                break;
            }
            s->protos[s->nproto] = san_lower_dup(key);
            s->protos[s->nproto + 1] = san_lower_dup(ps);
            JS_FreeCString(ctx, ps);
            if (!s->protos[s->nproto] || !s->protos[s->nproto + 1]) {
                rc = -1;
                break;
            }
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

JSValue dyn_san_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    san_t* s;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "new Sanitizer({allow, protocols}): an allow-list is required -- "
            "there is no default policy, because a default is a policy nobody read");
    s = (san_t*)calloc(1, sizeof *s);
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    if (san_read_allow(ctx, argv[0], s) < 0 || san_read_protocols(ctx, argv[0], s) < 0) {
        san_free(s);
        return JS_EXCEPTION;
    }
    return dyn_plain_wrap(ctx, new_target, dyn_san_class_id, s, san_free_v);
}

static int san_node(san_w_t* w, JSValueConst v);

static int san_element(san_w_t* w, JSValueConst v, const char* nm, size_t nml)
{
    const san_rule_t* r = san_find(w->s, nm, nml);
    JSValue kids;
    int rc = 0;

    if (!san_name_ok(nm, nml))
        return 0;
    if (r) {
        JSValue attrs = ht_get(w->ctx, &w->k, v, HT_K_ATTRS);
        JSPropertyEnum* tab = NULL;
        uint32_t len = 0, k;
        hb_put(&w->b, '<');
        hb_write(&w->b, nm, nml);
        if (JS_IsObject(attrs)
            && JS_GetOwnPropertyNames(w->ctx, &tab, &len, attrs,
                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
                == 0) {
            for (k = 0; k < len; k++) {
                JSValue av = JS_GetProperty(w->ctx, attrs, tab[k].atom);
                const char *an, *as;
                size_t anl, asl;
                an = JS_AtomToCStringLen(w->ctx, &anl, tab[k].atom);
                as = JS_IsException(av) ? NULL : JS_ToCStringLen(w->ctx, &asl, av);
                JS_FreeValue(w->ctx, av);
                if (an && as && san_name_ok(an, anl) && san_attr_ok(r, an, anl)
                    && san_proto_ok(w->s, nm, nml, an, anl, as, asl)) {
                    hb_put(&w->b, ' ');
                    hb_write(&w->b, an, anl);
                    hb_puts(&w->b, "=\"");
                    ht_escape(&w->b, as, asl, 1);
                    hb_put(&w->b, '"');
                }
                if (an)
                    JS_FreeCString(w->ctx, an);
                if (as)
                    JS_FreeCString(w->ctx, as);
            }
            JS_FreePropertyEnum(w->ctx, tab, len);
        }
        JS_FreeValue(w->ctx, attrs);
        hb_put(&w->b, '>');
    }
    if (!HT_IS_VOID(nm, nml) && !(!r && HT_IS_RAW(nm, nml))) {
        kids = ht_get(w->ctx, &w->k, v, HT_K_CHILDREN);
        if (JS_IsArray(w->ctx, kids) == 1) {
            int64_t len = 0, i;
            if (ht_len(w->ctx, ht_get(w->ctx, &w->k, kids, HT_K_LENGTH), &len)) {
                JS_FreeValue(w->ctx, kids);
                return -1;
            }
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

static int san_node(san_w_t* w, JSValueConst v)
{
    JSValue nv;
    const char* nm;
    size_t nml;
    int rc;

    if (w->depth >= HT_MAX_DEPTH) {
        JS_ThrowRangeError(w->ctx, "Sanitizer: the tree is nested deeper than %d levels", HT_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(v)) {
        const char* s;
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
    nv = ht_get(w->ctx, &w->k, v, HT_K_NAME);
    nm = JS_IsString(nv) ? JS_ToCStringLen(w->ctx, &nml, nv) : NULL;
    JS_FreeValue(w->ctx, nv);
    if (!nm)
        return 0;
    rc = san_element(w, v, nm, nml);
    JS_FreeCString(w->ctx, nm);
    return rc;
}

JSValue dyn_san_clean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    san_t* s = (san_t*)dyn_plain_get(ctx, this_val, dyn_san_class_id);
    san_w_t w;
    JSValue tree, lv, out;
    const char* src;
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
    if (ht_keys_init(ctx, &w.k) < 0) {
        ht_keys_free(ctx, &w.k);
        JS_FreeValue(ctx, tree);
        return JS_EXCEPTION;
    }
    lv = JS_GetPropertyStr(ctx, tree, "length");
    if (JS_IsException(lv) || ht_len(ctx, lv, &len))
        rc = -1;
    for (i = 0; i < len && rc == 0; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, tree, (uint32_t)i);
        rc = JS_IsException(c) ? -1 : san_node(&w, c);
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, tree);
    ht_keys_free(ctx, &w.k);
    if (rc < 0 || w.b.oom) {
        if (w.b.oom)
            JS_ThrowOutOfMemory(ctx);
        hb_free(&w.b);
        return JS_EXCEPTION;
    }
    out = JS_NewStringLen(ctx, (const char*)w.b.p, w.b.n);
    hb_free(&w.b);
    return out;
}

#endif
