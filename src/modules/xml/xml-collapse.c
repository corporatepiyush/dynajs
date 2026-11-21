#include "xml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_XML)

static int xml_own(JSContext* ctx, JSValueConst obj, JSAtom k, JSValue* out)
{
    JSPropertyDescriptor d;
    int r = JS_GetOwnProperty(ctx, &d, obj, k);
    if (r <= 0)
        return r;
    JS_FreeValue(ctx, d.getter);
    JS_FreeValue(ctx, d.setter);
    *out = d.value;
    return 1;
}

static int xml_collapse_add(JSContext* ctx, const xml_atoms_t* at, JSValue obj, JSAtom key, JSValue v)
{
    JSValue cur;
    int r = xml_own(ctx, obj, key, &cur);
    if (r < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    if (r == 0)
        return JS_DefinePropertyValue(ctx, obj, key, v, JS_PROP_C_W_E) < 0 ? -1 : 0;
    if (JS_IsArray(ctx, cur) == 1) {
        JSValue lv = JS_GetProperty(ctx, cur, at->length);
        int64_t len = 0;
        if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, cur);
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_FreeValue(ctx, lv);
        r = JS_DefinePropertyValueUint32(ctx, cur, (uint32_t)len, v, JS_PROP_C_W_E);
        JS_FreeValue(ctx, cur);
        return r < 0 ? -1 : 0;
    }
    {
        JSValue arr = JS_NewArray(ctx);
        if (JS_IsException(arr)) {
            JS_FreeValue(ctx, cur);
            JS_FreeValue(ctx, v);
            return -1;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, 0, cur, JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueUint32(ctx, arr, 1, v, JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValue(ctx, obj, key, arr, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return -1;
        }
        return 0;
    }
}

static int xml_collapse_attrs(JSContext* ctx, const xml_atoms_t* at, JSValueConst el, JSValue out)
{
    JSValue attrs = JS_GetProperty(ctx, el, at->attrs);
    JSPropertyEnum* tab = NULL;
    uint32_t alen = 0, k;
    int rc = 0;

    if (JS_IsException(attrs))
        return -1;
    if (!JS_IsObject(attrs)) {
        JS_FreeValue(ctx, attrs);
        return 0;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &alen, attrs,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(ctx, attrs);
        return -1;
    }
    for (k = 0; k < alen && rc == 0; k++) {
        JSValue v = JS_GetProperty(ctx, attrs, tab[k].atom);
        size_t anl = 0;
        const char* an = JS_AtomToCStringLen(ctx, &anl, tab[k].atom);
        char kbuf[128];
        char* key;
        JSAtom ka;
        if (JS_IsException(v) || !an) {
            JS_FreeValue(ctx, v);
            if (an)
                JS_FreeCString(ctx, an);
            rc = -1;
            break;
        }
        key = anl + 2 <= sizeof kbuf ? kbuf : (char*)malloc(anl + 2);
        if (!key) {
            JS_FreeCString(ctx, an);
            JS_FreeValue(ctx, v);
            rc = -1;
            break;
        }
        key[0] = '@';
        memcpy(key + 1, an, anl);
        key[anl + 1] = '\0';
        JS_FreeCString(ctx, an);
        ka = JS_NewAtomLen(ctx, key, anl + 1);
        if (key != kbuf)
            free(key);
        if (ka == JS_ATOM_NULL
            || JS_DefinePropertyValue(ctx, out, ka, v, JS_PROP_C_W_E) < 0)
            rc = -1;
        if (ka != JS_ATOM_NULL)
            JS_FreeAtom(ctx, ka);
    }
    JS_FreePropertyEnum(ctx, tab, alen);
    JS_FreeValue(ctx, attrs);
    return rc < 0 ? -1 : (int)alen;
}

static int xml_collapse_children(JSContext* ctx, const xml_atoms_t* at, JSValueConst el, JSValue out,
    xb_t* text, int depth)
{
    JSValue kids = JS_GetProperty(ctx, el, at->children), lv;
    int64_t len = 0, i;
    int nelem = 0, rc = 0;

    if (JS_IsException(kids))
        return -1;
    if (JS_IsArray(ctx, kids) != 1) {
        JS_FreeValue(ctx, kids);
        return 0;
    }
    lv = JS_GetProperty(ctx, kids, at->length);
    if (JS_IsException(lv) || JS_ToInt64(ctx, &len, lv) < 0)
        rc = -1;
    JS_FreeValue(ctx, lv);
    for (i = 0; i < len && rc == 0; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, kids, (uint32_t)i);
        if (JS_IsException(c)) {
            rc = -1;
            break;
        }
        if (JS_IsString(c)) {
            size_t sn;
            const char* sp = JS_ToCStringLen(ctx, &sn, c);
            if (sp) {
                xb_write(text, sp, sn);
                JS_FreeCString(ctx, sp);
            }
        } else if (JS_IsObject(c)) {
            JSValue nv = JS_GetProperty(ctx, c, at->name);
            JSValue sub = xml_collapse(ctx, at, text, c, depth + 1);
            JSAtom ka = JS_IsException(nv) ? JS_ATOM_NULL : JS_ValueToAtom(ctx, nv);
            JS_FreeValue(ctx, nv);
            if (JS_IsException(sub) || ka == JS_ATOM_NULL
                || xml_collapse_add(ctx, at, out, ka, sub) < 0) {
                if (ka == JS_ATOM_NULL && !JS_IsException(sub))
                    JS_FreeValue(ctx, sub);
                if (ka != JS_ATOM_NULL)
                    JS_FreeAtom(ctx, ka);
                rc = -1;
            } else {
                JS_FreeAtom(ctx, ka);
                nelem++;
            }
        }
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, kids);
    return rc < 0 ? -1 : nelem;
}

JSValue xml_collapse(JSContext* ctx, const xml_atoms_t* at, xb_t* text, JSValueConst el, int depth)
{
    JSValue out, sv;
    size_t base = text->n;
    int nattr, nelem;

    if (depth >= XML_MAX_DEPTH)
        return JS_ThrowRangeError(ctx, "XMLToObject: nesting exceeds %d", XML_MAX_DEPTH);
    out = JS_NewObject(ctx);
    if (JS_IsException(out))
        return out;
    nattr = xml_collapse_attrs(ctx, at, el, out);
    nelem = nattr < 0 ? -1 : xml_collapse_children(ctx, at, el, out, text, depth);
    if (nattr < 0 || nelem < 0) {
        JS_FreeValue(ctx, out);
        text->n = base;
        return JS_EXCEPTION;
    }
    if (text->n > base && !nelem && !nattr) {
        sv = JS_NewStringLen(ctx, (const char*)text->p + base, text->n - base);
        JS_FreeValue(ctx, out);
        text->n = base;
        return sv;
    }
    if (text->n > base) {
        sv = JS_NewStringLen(ctx, (const char*)text->p + base, text->n - base);
        text->n = base;
        if (JS_IsException(sv) || JS_DefinePropertyValue(ctx, out, at->text, sv, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    return out;
}

#endif
