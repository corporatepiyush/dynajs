#include "html-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTML)

#define HT_ATTR_BATCH 32

static JSAtom ht_attr_atom(ht_t* p, uint32_t k)
{
    char anstack[128];
    char* an = anstack;
    size_t al = p->attr[k].nlen, q;
    JSAtom a;

    anstack[0] = 0;
    if (al > sizeof anstack) {
        an = (char*)malloc(al);
        if (!an) {
            JS_ThrowOutOfMemory(p->ctx);
            return JS_ATOM_NULL;
        }
    }
    for (q = 0; q < al; q++)
        an[q] = (char)ht_lower((unsigned char)p->attr[k].n[q]);
    a = JS_NewAtomLen(p->ctx, an, al);
    if (an != anstack)
        free(an);
    return a;
}

static JSValue ht_attr_value(ht_t* p, uint32_t k)
{
    return JS_NewStringLen(p->ctx, (const char*)p->abuf.p + p->attr[k].voff,
        p->attr[k].vlen);
}

static JSValue ht_new_attrs_batch(ht_t* p)
{
    JSAtom atoms[HT_ATTR_BATCH];
    JSValue vals[HT_ATTR_BATCH];
    uint32_t k, n = p->nattr;

    for (k = 0; k < n; k++) {
        atoms[k] = ht_attr_atom(p, k);
        vals[k] = atoms[k] == JS_ATOM_NULL ? JS_EXCEPTION : ht_attr_value(p, k);
        if (JS_IsException(vals[k])) {
            uint32_t j;
            if (atoms[k] != JS_ATOM_NULL)
                JS_FreeAtom(p->ctx, atoms[k]);
            for (j = 0; j < k; j++) {
                JS_FreeAtom(p->ctx, atoms[j]);
                JS_FreeValue(p->ctx, vals[j]);
            }
            return JS_EXCEPTION;
        }
    }
    return JS_NewObjectFromProps(p->ctx, &p->at_shape, (int)n, atoms, vals);
}

JSValue ht_new_attrs(ht_t* p)
{
    JSValue attrs;
    uint32_t k;

    if (p->nattr <= HT_ATTR_BATCH)
        return p->nattr ? ht_new_attrs_batch(p) : JS_NewObject(p->ctx);
    attrs = JS_NewObject(p->ctx);
    if (JS_IsException(attrs))
        return attrs;
    for (k = 0; k < p->nattr; k++) {
        JSAtom a = ht_attr_atom(p, k);
        JSValue v;
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(p->ctx, attrs);
            return JS_EXCEPTION;
        }
        v = ht_attr_value(p, k);
        if (JS_IsException(v)
            || JS_DefinePropertyValue(p->ctx, attrs, a, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(p->ctx, a);
            JS_FreeValue(p->ctx, attrs);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(p->ctx, a);
    }
    return attrs;
}

static JSValue dyn_html_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ht_ser_t w;
    JSValue out;
    int rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "HTMLStringify(node): a node is required");
    memset(&w, 0, sizeof w);
    w.ctx = ctx;
    hb_init(&w.b);
    if (ht_keys_init(ctx, &w.k) < 0) {
        ht_keys_free(ctx, &w.k);
        return JS_EXCEPTION;
    }
    if (JS_IsArray(ctx, argv[0]) == 1) {
        int64_t len = 0, i;
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        rc = JS_IsException(lv) ? -1 : ht_len(ctx, lv, &len);
        for (i = 0; i < len && rc == 0; i++) {
            JSValue c = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            rc = JS_IsException(c) ? -1 : ht_ser_node(&w, c);
            JS_FreeValue(ctx, c);
        }
    } else {
        rc = ht_ser_node(&w, argv[0]);
    }
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

JSClassID dyn_sel_class_id;

static const JSClassDef dyn_sel_class = {
    "Selector",
    .finalizer = dyn_sel_finalizer,
};

static const JSCFunctionListEntry dyn_sel_proto[] = {
    JS_CFUNC_MAGIC_DEF("all", 1, dyn_sel_query, 0),
    JS_CFUNC_MAGIC_DEF("first", 1, dyn_sel_query, 1),
    JS_CFUNC_MAGIC_DEF("matches", 1, dyn_sel_query, 2),
};

JSClassID dyn_san_class_id;

static const JSClassDef dyn_san_class = {
    "Sanitizer",
    .finalizer = dyn_san_finalizer,
};

static const JSCFunctionListEntry dyn_san_proto[] = {
    JS_CFUNC_DEF("clean", 1, dyn_san_clean),
};

static int ht_text_skip(const char* s, size_t n)
{
    return ht_ieq(s, n, "script") || ht_ieq(s, n, "style")
        || ht_ieq(s, n, "template") || ht_ieq(s, n, "noscript");
}

static int ht_text_node(JSContext* ctx, ht_keys_t* k, hb_t* b, JSValueConst node, int depth)
{
    JSValue kids, nm;
    const char* tag;
    const uint8_t* nb;
    size_t nn, z;
    int64_t len = 0, i;
    int rc = 0, skip = 0;

    if (depth > HT_MAX_DEPTH) {
        JS_ThrowRangeError(ctx, "HTMLText: nesting exceeds %d", HT_MAX_DEPTH);
        return -1;
    }
    if (JS_IsString(node)) {
        size_t n;
        const char* t = JS_ToCStringLen(ctx, &n, node);
        if (!t)
            return -1;
        hb_write(b, t, n);
        JS_FreeCString(ctx, t);
        return 0;
    }
    if (!JS_IsObject(node))
        return 0;
    nm = ht_get(ctx, k, node, HT_K_NAME);
    if (JS_IsException(nm))
        return -1;
    nb = JS_IsString(nm) ? JS_GetNarrowStringBytes(ctx, nm, &nn) : NULL;
    if (nb) {
        for (z = 0; z < nn && nb[z] && nb[z] < 0x80; z++)
            ;
        if (z == nn || !nb[z])
            skip = ht_text_skip((const char*)nb, z);
        else
            nb = NULL;
    }
    if (!nb) {
        tag = JS_IsString(nm) ? JS_ToCString(ctx, nm) : NULL;
        if (tag) {
            skip = ht_tag_is(tag, "script") || ht_tag_is(tag, "style")
                || ht_tag_is(tag, "template") || ht_tag_is(tag, "noscript");
            JS_FreeCString(ctx, tag);
        }
    }
    JS_FreeValue(ctx, nm);
    if (skip)
        return 0;
    kids = ht_get(ctx, k, node, HT_K_CHILDREN);
    if (JS_IsException(kids))
        return -1;
    if (JS_IsArray(ctx, kids) == 1) {
        rc = ht_len(ctx, ht_get(ctx, k, kids, HT_K_LENGTH), &len);
        for (i = 0; i < len && rc == 0; i++) {
            JSValue c = JS_GetPropertyUint32(ctx, kids, (uint32_t)i);
            rc = JS_IsException(c) ? -1 : ht_text_node(ctx, k, b, c, depth + 1);
            JS_FreeValue(ctx, c);
        }
    }
    JS_FreeValue(ctx, kids);
    return rc;
}

static JSValue dyn_html_text(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    hb_t b;
    ht_keys_t k;
    JSValue out;
    int rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "HTMLText(node): a node is required");
    if (ht_keys_init(ctx, &k) < 0) {
        ht_keys_free(ctx, &k);
        return JS_EXCEPTION;
    }
    hb_init(&b);
    if (JS_IsArray(ctx, argv[0]) == 1) {
        int64_t len = 0, i;
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        rc = JS_IsException(lv) ? -1 : ht_len(ctx, lv, &len);
        for (i = 0; i < len && rc == 0; i++) {
            JSValue c = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            rc = JS_IsException(c) ? -1 : ht_text_node(ctx, &k, &b, c, 0);
            JS_FreeValue(ctx, c);
        }
    } else {
        rc = ht_text_node(ctx, &k, &b, argv[0], 0);
    }
    ht_keys_free(ctx, &k);
    if (rc < 0) {
        hb_free(&b);
        return JS_EXCEPTION;
    }
    out = JS_NewStringLen(ctx, b.p ? (const char*)b.p : "", b.n);
    hb_free(&b);
    return out;
}

static const JSCFunctionListEntry dyn_html_funcs[] = {
    JS_CFUNC_DEF("HTMLParse", 1, dyn_html_parse),
    JS_CFUNC_DEF("HTMLStringify", 1, dyn_html_stringify),
    JS_CFUNC_DEF("HTMLText", 1, dyn_html_text),
    JS_CFUNC_DEF("MarkdownToHTML", 1, dyn_md_render),
    JS_CFUNC_DEF("rewriteLinks", 2, dyn_html_rewrite_links),
};

static int dyn_html_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_sel_class_id, &dyn_sel_class,
            dyn_sel_proto, countof(dyn_sel_proto),
            dyn_sel_ctor, "Selector")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_san_class_id, &dyn_san_class,
            dyn_san_proto, countof(dyn_san_proto),
            dyn_san_ctor, "Sanitizer")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_tpl_class_id, &dyn_tpl_class,
            dyn_tpl_proto, countof(dyn_tpl_proto),
            dyn_tpl_ctor, "Template")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_html_funcs, countof(dyn_html_funcs));
}

int js_nat_init_html(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:html", dyn_html_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Selector");
    JS_AddModuleExport(ctx, m, "Sanitizer");
    JS_AddModuleExport(ctx, m, "Template");
    return JS_AddModuleExportList(ctx, m, dyn_html_funcs, countof(dyn_html_funcs));
}

#endif
