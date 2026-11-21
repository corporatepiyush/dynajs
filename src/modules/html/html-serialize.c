#include "html-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTML)

static int ht_ser_children(ht_ser_t* w, JSValueConst el)
{
    JSValue kids = ht_get(w->ctx, &w->k, el, HT_K_CHILDREN), lv;
    int64_t len = 0, i;
    int rc = 0;

    if (JS_IsArray(w->ctx, kids) != 1) {
        JS_FreeValue(w->ctx, kids);
        return 0;
    }
    lv = ht_get(w->ctx, &w->k, kids, HT_K_LENGTH);
    if (ht_len(w->ctx, lv, &len))
        rc = -1;
    for (i = 0; i < len && rc == 0; i++) {
        JSValue c = JS_GetPropertyUint32(w->ctx, kids, (uint32_t)i);
        if (JS_IsException(c)) {
            rc = -1;
            break;
        }
        rc = ht_ser_node(w, c);
        JS_FreeValue(w->ctx, c);
    }
    JS_FreeValue(w->ctx, kids);
    return rc;
}

static int ht_ser_attrs(ht_ser_t* w, JSValueConst el)
{
    JSValue attrs = ht_get(w->ctx, &w->k, el, HT_K_ATTRS);
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, k;
    int rc = 0;

    if (!JS_IsObject(attrs)) {
        JS_FreeValue(w->ctx, attrs);
        return 0;
    }
    if (JS_GetOwnPropertyNames(w->ctx, &tab, &len, attrs,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
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
            if (an)
                JS_FreeCString(w->ctx, an);
            if (av)
                JS_FreeCString(w->ctx, av);
            rc = -1;
            break;
        }
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

int ht_ser_node(ht_ser_t* w, JSValueConst v)
{
    JSValue nv;
    const char* nm;
    size_t nml;
    int rc, raw;

    if (w->depth >= HT_MAX_DEPTH) {
        JS_ThrowRangeError(w->ctx, "HTMLStringify: nesting exceeds %d", HT_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(v)) {
        const char* s;
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
    nv = ht_get(w->ctx, &w->k, v, HT_K_NAME);
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
    raw = HT_IS_RAW_ESCAPELESS(nm, nml);
    if (rc == 0 && !HT_IS_VOID(nm, nml)) {
        w->depth++;
        if (raw) {
            JSValue kids = ht_get(w->ctx, &w->k, v, HT_K_CHILDREN), c;
            if (JS_IsArray(w->ctx, kids) == 1) {
                c = JS_GetPropertyUint32(w->ctx, kids, 0);
                if (JS_IsException(c)) {
                    rc = -1;
                } else if (JS_IsString(c)) {
                    size_t tn;
                    const char* ts = JS_ToCStringLen(w->ctx, &tn, c);
                    if (ts) {
                        ht_write_raw(&w->b, ts, tn);
                        JS_FreeCString(w->ctx, ts);
                    } else {
                        rc = -1;
                    }
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

#endif
