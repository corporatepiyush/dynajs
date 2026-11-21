#include "xml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_XML)

static void xml_ser_nl(xml_ser_t* w)
{
    int k;
    if (!w->indent)
        return;
    xb_put(&w->out, '\n');
    for (k = 0; k < w->depth * w->indent; k++)
        xb_put(&w->out, ' ');
}

static int xml_ser_attrs(xml_ser_t* w, JSValueConst el)
{
    JSValue attrs = JS_GetProperty(w->ctx, el, w->at.attrs);
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, k;
    int rc = 0;

    if (JS_IsException(attrs))
        return -1;
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
        const char *an, *av;
        size_t anl, avl;
        JSValue v = JS_GetProperty(w->ctx, attrs, tab[k].atom);
        if (JS_IsException(v)) {
            rc = -1;
            break;
        }
        an = JS_AtomToCStringLen(w->ctx, &anl, tab[k].atom);
        av = JS_ToCStringLen(w->ctx, &avl, v);
        JS_FreeValue(w->ctx, v);
        if (!an || !av) {
            if (an)
                JS_FreeCString(w->ctx, an);
            if (av)
                JS_FreeCString(w->ctx, av);
            rc = -1;
            break;
        }
        if (anl == 0 || !xml_name_start((unsigned char)an[0])) {
            JS_ThrowTypeError(w->ctx,
                "XMLStringify: invalid attribute name");
            JS_FreeCString(w->ctx, an);
            JS_FreeCString(w->ctx, av);
            JS_FreePropertyEnum(w->ctx, tab, len);
            JS_FreeValue(w->ctx, attrs);
            return -1;
        }
        {
            size_t ci;
            for (ci = 1; ci < anl; ci++)
                if (!xml_name_char((unsigned char)an[ci])) {
                    JS_ThrowTypeError(w->ctx,
                        "XMLStringify: invalid attribute name");
                    JS_FreeCString(w->ctx, an);
                    JS_FreeCString(w->ctx, av);
                    JS_FreePropertyEnum(w->ctx, tab, len);
                    JS_FreeValue(w->ctx, attrs);
                    return -1;
                }
        }
        xb_put(&w->out, ' ');
        xb_write(&w->out, an, anl);
        xb_puts(&w->out, "=\"");
        xml_escape(&w->out, av, avl, 1);
        xb_put(&w->out, '"');
        JS_FreeCString(w->ctx, an);
        JS_FreeCString(w->ctx, av);
    }
    JS_FreePropertyEnum(w->ctx, tab, len);
    JS_FreeValue(w->ctx, attrs);
    return rc;
}

static int xml_ser_children(xml_ser_t* w, JSValueConst el, int* any_element)
{
    JSValue kids = JS_GetProperty(w->ctx, el, w->at.children);
    int64_t len = 0, i;
    int rc = 0;

    *any_element = 0;
    if (JS_IsException(kids))
        return -1;
    if (JS_IsArray(w->ctx, kids) != 1) {
        JS_FreeValue(w->ctx, kids);
        return 0;
    }
    {
        JSValue lv = JS_GetProperty(w->ctx, kids, w->at.length);
        if (JS_IsException(lv) || JS_ToInt64(w->ctx, &len, lv) < 0) {
            JS_FreeValue(w->ctx, lv);
            JS_FreeValue(w->ctx, kids);
            return -1;
        }
        JS_FreeValue(w->ctx, lv);
    }
    for (i = 0; i < len && rc == 0; i++) {
        JSValue c = JS_GetPropertyUint32(w->ctx, kids, (uint32_t)i);
        if (JS_IsException(c)) {
            rc = -1;
            break;
        }
        if (JS_IsObject(c)) {
            *any_element = 1;
            xml_ser_nl(w);
        }
        rc = xml_ser_node(w, c);
        JS_FreeValue(w->ctx, c);
    }
    JS_FreeValue(w->ctx, kids);
    return rc;
}

int xml_ser_node(xml_ser_t* w, JSValueConst node)
{
    JSValue nv;
    const char* nm;
    size_t nml;
    int any = 0, rc = 0;

    if (w->depth >= XML_MAX_DEPTH) {
        JS_ThrowRangeError(w->ctx, "XMLStringify: nesting exceeds %d", XML_MAX_DEPTH);
        return -1;
    }
    if (!JS_IsObject(node)) {
        const char* s;
        size_t n;
        if (JS_IsUndefined(node) || JS_IsNull(node))
            return 0;
        s = JS_ToCStringLen(w->ctx, &n, node);
        if (!s)
            return -1;
        xml_escape(&w->out, s, n, 0);
        JS_FreeCString(w->ctx, s);
        return 0;
    }
    nv = JS_GetProperty(w->ctx, node, w->at.name);
    if (JS_IsException(nv))
        return -1;
    if (!JS_IsString(nv)) {
        JS_FreeValue(w->ctx, nv);
        JS_ThrowTypeError(w->ctx, "XMLStringify: a node needs a string `name`");
        return -1;
    }
    nm = JS_ToCStringLen(w->ctx, &nml, nv);
    JS_FreeValue(w->ctx, nv);
    if (!nm)
        return -1;
    if (nml == 0 || !xml_name_start((unsigned char)nm[0])) {
        JS_FreeCString(w->ctx, nm);
        JS_ThrowRangeError(w->ctx, "XMLStringify: not a valid element name");
        return -1;
    }
    {
        size_t ci;
        for (ci = 1; ci < nml; ci++)
            if (!xml_name_char((unsigned char)nm[ci])) {
                JS_FreeCString(w->ctx, nm);
                JS_ThrowRangeError(w->ctx, "XMLStringify: not a valid element name");
                return -1;
            }
    }
    xb_put(&w->out, '<');
    xb_write(&w->out, nm, nml);
    rc = xml_ser_attrs(w, node);
    if (rc == 0) {
        size_t before = w->out.n;
        xb_puts(&w->out, ">");
        w->depth++;
        rc = xml_ser_children(w, node, &any);
        w->depth--;
        if (rc == 0 && w->out.n == before + 1) {
            w->out.n = before;
            xb_puts(&w->out, "/>");
        } else if (rc == 0) {
            if (any)
                xml_ser_nl(w);
            xb_puts(&w->out, "</");
            xb_write(&w->out, nm, nml);
            xb_put(&w->out, '>');
        }
    }
    JS_FreeCString(w->ctx, nm);
    return rc;
}

#endif
