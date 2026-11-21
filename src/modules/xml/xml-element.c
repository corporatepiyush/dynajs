#include "xml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_XML)

int xml_atoms_init(JSContext* ctx, xml_atoms_t* a)
{
    a->name = JS_NewAtom(ctx, "name");
    a->attrs = JS_NewAtom(ctx, "attrs");
    a->children = JS_NewAtom(ctx, "children");
    a->length = JS_NewAtom(ctx, "length");
    a->text = JS_NewAtom(ctx, "#text");
    if (a->name == JS_ATOM_NULL || a->attrs == JS_ATOM_NULL || a->children == JS_ATOM_NULL
        || a->length == JS_ATOM_NULL || a->text == JS_ATOM_NULL) {
        xml_atoms_free(ctx, a);
        return -1;
    }
    return 0;
}

void xml_atoms_free(JSContext* ctx, xml_atoms_t* a)
{
    JS_FreeAtom(ctx, a->name);
    JS_FreeAtom(ctx, a->attrs);
    JS_FreeAtom(ctx, a->children);
    JS_FreeAtom(ctx, a->length);
    JS_FreeAtom(ctx, a->text);
    a->name = a->attrs = a->children = a->length = a->text = JS_ATOM_NULL;
}

JSObjectTemplate* xml_element_template(JSContext* ctx)
{
    JSAtom fields[3];
    JSObjectTemplate* tpl = NULL;

    fields[0] = JS_NewAtom(ctx, "name");
    fields[1] = JS_NewAtom(ctx, "attrs");
    fields[2] = JS_NewAtom(ctx, "children");
    if (fields[0] != JS_ATOM_NULL && fields[1] != JS_ATOM_NULL && fields[2] != JS_ATOM_NULL)
        tpl = JS_NewObjectTemplate(ctx, fields, 3);
    JS_FreeAtom(ctx, fields[0]);
    JS_FreeAtom(ctx, fields[1]);
    JS_FreeAtom(ctx, fields[2]);
    return tpl;
}

JSValue xml_new_element(JSContext* ctx, const xml_ev_t* ev, JSValue* children,
    JSObjectTemplate* tpl)
{
    JSValue el = tpl ? JS_NewObjectFromTemplate(ctx, tpl) : JS_NewObject(ctx), attrs;
    uint32_t k;
    if (JS_IsException(el))
        return el;
    attrs = JS_NewObject(ctx);
    if (JS_IsException(attrs)) {
        JS_FreeValue(ctx, el);
        return JS_EXCEPTION;
    }
    for (k = 0; k < ev->nattr; k++) {
        JSAtom a = JS_NewAtomLen(ctx, ev->attr[k].n, ev->attr[k].nlen);
        JSValue v;
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(ctx, attrs);
            JS_FreeValue(ctx, el);
            return JS_EXCEPTION;
        }
        v = JS_NewStringLen(ctx, (const char*)ev->abuf.p + ev->attr[k].voff,
            ev->attr[k].vlen);
        if (JS_IsException(v) || JS_DefinePropertyValue(ctx, attrs, a, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, a);
            JS_FreeValue(ctx, attrs);
            JS_FreeValue(ctx, el);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(ctx, a);
    }
    *children = JS_NewArray(ctx);
    if (JS_IsException(*children)) {
        JS_FreeValue(ctx, attrs);
        JS_FreeValue(ctx, el);
        return JS_EXCEPTION;
    }
    if (tpl) {
        JSValue name = JS_NewStringLen(ctx, ev->name, ev->nlen);
        if (JS_IsException(name)) {
            JS_FreeValue(ctx, attrs);
            JS_FreeValue(ctx, *children);
            JS_FreeValue(ctx, el);
            return JS_EXCEPTION;
        }
        JS_SetTemplateCell(ctx, el, 0, name);
        JS_SetTemplateCell(ctx, el, 1, attrs);
        JS_SetTemplateCell(ctx, el, 2, JS_DupValue(ctx, *children));
        return el;
    }
    if (JS_DefinePropertyValueStr(ctx, el, "name",
            JS_NewStringLen(ctx, ev->name, ev->nlen), JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, el, "attrs", attrs, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, el, "children", JS_DupValue(ctx, *children), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, *children);
        JS_FreeValue(ctx, el);
        return JS_EXCEPTION;
    }
    return el;
}

#endif
