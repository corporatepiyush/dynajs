#include "html-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTML)

typedef struct {
    JSContext* ctx;
    JSValue fn;
    int count;
    int failed;
    JSAtom a_name, a_attrs, a_children, a_length;
} ht_rw_t;

static int ht_rewrite_node(ht_rw_t* rw, JSValueConst node, int depth);

static int ht_rewrite_srcset(ht_rw_t* rw, JSValue attrs, JSAtom atom,
    const char* tag, size_t tagn,
    const char* attrname,
    const char* val, size_t vn)
{
    hb_t b;
    size_t i = 0;
    int first = 1, local = 0;
    hb_init(&b);
    while (i < vn) {
        size_t us, ue, url_end, de;
        int comma_ended = 0;
        while (i < vn && (val[i] == ' ' || val[i] == '\t' || val[i] == '\n' || val[i] == '\r' || val[i] == ','))
            i++;
        if (i >= vn)
            break;
        us = i;
        while (i < vn && val[i] != ' ' && val[i] != '\t' && val[i] != '\n' && val[i] != '\r')
            i++;
        ue = i;
        url_end = ue;
        while (url_end > us && val[url_end - 1] == ',') {
            url_end--;
            comma_ended = 1;
        }
        de = ue;
        if (!comma_ended) {
            int depth = 0;
            size_t j = ue;
            while (j < vn) {
                char ch = val[j];
                if (ch == '(')
                    depth++;
                else if (ch == ')') {
                    if (depth > 0)
                        depth--;
                } else if (ch == ',' && depth == 0)
                    break;
                j++;
            }
            de = j;
            i = (j < vn) ? j + 1 : vn;
        }
        if (us >= url_end)
            continue;
        {
            JSValue arg, tagv, attrv, r;
            JSValueConst a[3];
            const char* nurl;
            size_t nn;
            arg = JS_NewStringLen(rw->ctx, val + us, url_end - us);
            tagv = JS_NewStringLen(rw->ctx, tag, tagn);
            attrv = JS_NewStringLen(rw->ctx, attrname, strlen(attrname));
            a[0] = arg;
            a[1] = tagv;
            a[2] = attrv;
            r = JS_Call(rw->ctx, rw->fn, JS_UNDEFINED, 3, a);
            JS_FreeValue(rw->ctx, arg);
            JS_FreeValue(rw->ctx, tagv);
            JS_FreeValue(rw->ctx, attrv);
            if (JS_IsException(r)) {
                hb_free(&b);
                rw->failed = 1;
                return -1;
            }
            if (JS_IsUndefined(r) || JS_IsNull(r)) {
                JS_FreeValue(rw->ctx, r);
                nurl = val + us;
                nn = url_end - us;
            } else {
                JSValue cs2 = JS_ToString(rw->ctx, r);
                JS_FreeValue(rw->ctx, r);
                if (JS_IsException(cs2)) {
                    hb_free(&b);
                    rw->failed = 1;
                    return -1;
                }
                nurl = JS_ToCStringLen(rw->ctx, &nn, cs2);
                JS_FreeValue(rw->ctx, cs2);
                if (!nurl) {
                    hb_free(&b);
                    rw->failed = 1;
                    return -1;
                }
                local++;
            }
            if (!first)
                hb_puts(&b, ", ");
            first = 0;
            hb_write(&b, nurl, nn);
            if (de > ue)
                hb_write(&b, val + ue, de - ue);
            if (nurl != val + us)
                JS_FreeCString(rw->ctx, nurl);
        }
    }
    if (rw->failed) {
        hb_free(&b);
        return -1;
    }
    if (local > 0) {
        JSValue nv = JS_NewStringLen(rw->ctx, (const char*)b.p, b.n);
        hb_free(&b);
        if (JS_IsException(nv))
            return -1;
        if (JS_DefinePropertyValue(rw->ctx, attrs, atom, nv, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            return -1;
        rw->count += local;
    } else {
        hb_free(&b);
    }
    return 0;
}

static int ht_rewrite_attrs(ht_rw_t* rw, JSValueConst el,
    const char* tag, size_t tagn)
{
    JSValue attrs = JS_GetProperty(rw->ctx, el, rw->a_attrs);
    JSValue tagv = JS_UNDEFINED;
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, k;
    int rc = 0;
    if (JS_IsException(attrs))
        return -1;
    if (!JS_IsObject(attrs)) {
        JS_FreeValue(rw->ctx, attrs);
        return 0;
    }
    if (JS_GetOwnPropertyNames(rw->ctx, &tab, &len, attrs,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(rw->ctx, attrs);
        return -1;
    }
    for (k = 0; k < len && rc == 0; k++) {
        const char* an;
        const char* owned = NULL;
        size_t anl;
        JSValue av;
        an = JS_AtomBorrowASCII(rw->ctx, &anl, tab[k].atom);
        if (!an) {
            an = owned = JS_AtomToCStringLen(rw->ctx, &anl, tab[k].atom);
            if (!an) {
                rc = -1;
                break;
            }
        }
        if (!ht_is_single_url_attr(an, anl) && !ht_is_srcset_attr(an, anl)) {
            if (owned)
                JS_FreeCString(rw->ctx, owned);
            continue;
        }
        av = JS_GetProperty(rw->ctx, attrs, tab[k].atom);
        if (JS_IsException(av)) {
            if (owned)
                JS_FreeCString(rw->ctx, owned);
            rc = -1;
            break;
        }
        if (!JS_IsString(av)) {
            JS_FreeValue(rw->ctx, av);
            if (owned)
                JS_FreeCString(rw->ctx, owned);
            continue;
        }
        if (ht_is_srcset_attr(an, anl)) {
            char attrbuf[16];
            size_t cn = anl < sizeof attrbuf - 1 ? anl : sizeof attrbuf - 1;
            size_t ci, avl;
            const char* as = JS_ToCStringLen(rw->ctx, &avl, av);
            JS_FreeValue(rw->ctx, av);
            if (!as) {
                if (owned)
                    JS_FreeCString(rw->ctx, owned);
                rc = -1;
                break;
            }
            for (ci = 0; ci < cn; ci++) {
                unsigned char c = (unsigned char)an[ci];
                attrbuf[ci] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            }
            attrbuf[cn] = '\0';
            rc = ht_rewrite_srcset(rw, attrs, tab[k].atom, tag, tagn,
                attrbuf, as, avl);
            JS_FreeCString(rw->ctx, as);
            if (owned)
                JS_FreeCString(rw->ctx, owned);
            if (rc < 0)
                break;
            continue;
        }
        {
            JSValue attrv;
            char attrbuf[64];
            size_t cn = anl < sizeof attrbuf - 1 ? anl : sizeof attrbuf - 1;
            size_t ci;
            JSValue r;
            JSValueConst a[3];
            if (JS_IsUndefined(tagv)) {
                tagv = JS_NewStringLen(rw->ctx, tag, tagn);
                if (JS_IsException(tagv)) {
                    JS_FreeValue(rw->ctx, av);
                    if (owned)
                        JS_FreeCString(rw->ctx, owned);
                    rc = -1;
                    break;
                }
            }
            for (ci = 0; ci < cn; ci++) {
                unsigned char c = (unsigned char)an[ci];
                attrbuf[ci] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            }
            attrbuf[cn] = '\0';
            if (owned)
                JS_FreeCString(rw->ctx, owned);
            attrv = JS_NewStringLen(rw->ctx, attrbuf, cn);
            a[0] = av;
            a[1] = tagv;
            a[2] = attrv;
            r = JS_Call(rw->ctx, rw->fn, JS_UNDEFINED, 3, a);
            JS_FreeValue(rw->ctx, av);
            JS_FreeValue(rw->ctx, attrv);
            if (JS_IsException(r)) {
                rw->failed = 1;
                rc = -1;
                break;
            }
            if (JS_IsUndefined(r) || JS_IsNull(r)) {
                JS_FreeValue(rw->ctx, r);
                continue;
            }
            {
                JSValue cs = JS_ToString(rw->ctx, r);
                JS_FreeValue(rw->ctx, r);
                if (JS_IsException(cs)) {
                    rw->failed = 1;
                    rc = -1;
                    break;
                }
                if (JS_DefinePropertyValue(rw->ctx, attrs, tab[k].atom, cs, JS_PROP_C_W_E | JS_PROP_THROW) < 0) {
                    rw->failed = 1;
                    rc = -1;
                    break;
                }
                rw->count++;
            }
        }
    }
    JS_FreeValue(rw->ctx, tagv);
    JS_FreePropertyEnum(rw->ctx, tab, len);
    JS_FreeValue(rw->ctx, attrs);
    return rc;
}

static int ht_rewrite_node(ht_rw_t* rw, JSValueConst node, int depth)
{
    JSValue nm, kids, lv;
    const char* tag;
    size_t tagn;
    int64_t len = 0, i;
    int rc = 0;
    if (depth > HT_MAX_DEPTH) {
        JS_ThrowRangeError(rw->ctx, "rewriteLinks: the tree is nested deeper than %d levels", HT_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(node))
        return 0;
    if (JS_IsArray(rw->ctx, node) == 1) {
        lv = JS_GetProperty(rw->ctx, node, rw->a_length);
        if (JS_IsException(lv) || ht_len(rw->ctx, lv, &len))
            return -1;
        for (i = 0; i < len && rc == 0; i++) {
            JSValue c = JS_GetPropertyUint32(rw->ctx, node, (uint32_t)i);
            if (JS_IsException(c)) {
                rc = -1;
                break;
            }
            rc = ht_rewrite_node(rw, c, depth + 1);
            JS_FreeValue(rw->ctx, c);
        }
        return rc;
    }
    nm = JS_GetProperty(rw->ctx, node, rw->a_name);
    if (JS_IsException(nm))
        return -1;
    if (!JS_IsString(nm)) {
        JS_FreeValue(rw->ctx, nm);
        return 0;
    }
    tag = JS_ToCStringLen(rw->ctx, &tagn, nm);
    JS_FreeValue(rw->ctx, nm);
    if (!tag)
        return -1;
    {
        char sbuf[64];
        char* lc = tagn < sizeof sbuf ? sbuf : (char*)malloc(tagn + 1);
        size_t k;
        if (!lc) {
            JS_FreeCString(rw->ctx, tag);
            return -1;
        }
        for (k = 0; k < tagn; k++) {
            unsigned char ch = (unsigned char)tag[k];
            lc[k] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        }
        lc[tagn] = '\0';
        JS_FreeCString(rw->ctx, tag);
        rc = ht_rewrite_attrs(rw, node, lc, tagn);
        if (lc != sbuf)
            free(lc);
    }
    if (rc < 0)
        return rc;
    kids = JS_GetProperty(rw->ctx, node, rw->a_children);
    if (JS_IsException(kids))
        return -1;
    if (JS_IsArray(rw->ctx, kids) == 1) {
        lv = JS_GetProperty(rw->ctx, kids, rw->a_length);
        if (JS_IsException(lv) || ht_len(rw->ctx, lv, &len))
            rc = -1;
        for (i = 0; i < len && rc == 0; i++) {
            JSValue c = JS_GetPropertyUint32(rw->ctx, kids, (uint32_t)i);
            if (JS_IsException(c)) {
                rc = -1;
                break;
            }
            if (JS_IsObject(c))
                rc = ht_rewrite_node(rw, c, depth + 1);
            JS_FreeValue(rw->ctx, c);
        }
    }
    JS_FreeValue(rw->ctx, kids);
    return rc;
}

JSValue dyn_html_rewrite_links(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ht_rw_t rw;
    int rc;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "rewriteLinks(doc, fn): doc and fn are required");
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "rewriteLinks(doc, fn): fn must be a function");
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "rewriteLinks(doc, fn): doc must be a node or node array");
    rw.ctx = ctx;
    rw.fn = argv[1];
    rw.count = 0;
    rw.failed = 0;
    rw.a_name = JS_NewAtom(ctx, "name");
    rw.a_attrs = JS_NewAtom(ctx, "attrs");
    rw.a_children = JS_NewAtom(ctx, "children");
    rw.a_length = JS_NewAtom(ctx, "length");
    if (rw.a_name == JS_ATOM_NULL || rw.a_attrs == JS_ATOM_NULL
        || rw.a_children == JS_ATOM_NULL || rw.a_length == JS_ATOM_NULL)
        rc = -1;
    else
        rc = ht_rewrite_node(&rw, argv[0], 0);
    JS_FreeAtom(ctx, rw.a_name);
    JS_FreeAtom(ctx, rw.a_attrs);
    JS_FreeAtom(ctx, rw.a_children);
    JS_FreeAtom(ctx, rw.a_length);
    if (rc < 0)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, rw.count);
}

#endif
