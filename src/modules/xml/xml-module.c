#include "xml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_XML)

static const char* const xml_stringify_keys[] = { "indent" };

int xml_read_opts(JSContext* ctx, JSValueConst o, xml_opts_t* out)
{
    JSValue v;
    out->trim = 1;
    out->keep_entities = 0;
    out->multiple = 0;
    if (JS_IsUndefined(o) || JS_IsNull(o))
        return 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
    v = JS_GetPropertyStr(ctx, o, "multiple");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return -1;
        out->multiple = b;
    }
    v = JS_GetPropertyStr(ctx, o, "trim");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return -1;
        out->trim = b;
    }
    v = JS_GetPropertyStr(ctx, o, "entities");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        const char* s = JS_ToCString(ctx, v);
        int keep;
        if (!s) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        keep = strcmp(s, "keep") == 0;
        if (!keep && strcmp(s, "strict") != 0) {
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "entities must be \"strict\" or \"keep\"");
            return -1;
        }
        JS_FreeCString(ctx, s);
        out->keep_entities = keep;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static JSValue xml_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    xml_ser_t w;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "XMLStringify(node, options): a node is required");
    memset(&w, 0, sizeof w);
    w.ctx = ctx;
    xb_init(&w.out);
    if (xml_atoms_init(ctx, &w.at) < 0)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], xml_stringify_keys,
                countof(xml_stringify_keys))
            < 0)
            goto fail;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "indent");
        int32_t ind = 0;
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &ind, v) < 0) {
            JS_FreeValue(ctx, v);
            goto fail;
        }
        JS_FreeValue(ctx, v);
        if (ind < 0 || ind > 16) {
            JS_ThrowRangeError(ctx, "XMLStringify: indent is 0 to 16");
            goto fail;
        }
        w.indent = ind;
    }
    if (xml_ser_node(&w, argv[0]) < 0)
        goto fail;
    xml_atoms_free(ctx, &w.at);
    if (w.out.oom) {
        xb_free(&w.out);
        return JS_ThrowOutOfMemory(ctx);
    }
    out = JS_NewStringLen(ctx, (const char*)w.out.p, w.out.n);
    xb_free(&w.out);
    return out;
fail:
    xml_atoms_free(ctx, &w.at);
    xb_free(&w.out);
    return JS_EXCEPTION;
}

JSClassID dyn_xw_class_id;

static const JSClassDef dyn_xw_class = {
    "XmlWriter",
    .finalizer = xml_writer_finalizer,
};

static const JSCFunctionListEntry dyn_xw_proto[] = {
    JS_CFUNC_DEF("open", 1, xml_writer_open),
    JS_CFUNC_DEF("text", 1, xml_writer_text),
    JS_CFUNC_DEF("close", 0, xml_writer_close),
    JS_CFUNC_DEF("toString", 0, xml_writer_to_string),
    JS_CFUNC_DEF("finish", 0, xml_writer_finish),
};

JSClassID dyn_sax_class_id;

static const JSClassDef dyn_sax_class = {
    "SAXParser",
    .finalizer = dyn_sax_finalizer,
    .gc_mark = dyn_sax_gc_mark,
};

static const JSCFunctionListEntry dyn_sax_proto[] = {
    JS_CFUNC_MAGIC_DEF("write", 1, dyn_sax_feed, 0),
    JS_CFUNC_MAGIC_DEF("end", 0, dyn_sax_feed, 1),
    JS_CFUNC_DEF("next", 0, dyn_sax_next),
};

static JSValue xml_to_object(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue out, nv, sub;
    JSAtom key;

    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "XMLToObject(node): node must be an element");
    nv = JS_GetPropertyStr(ctx, argv[0], "name");
    if (JS_IsException(nv))
        return JS_EXCEPTION;
    if (!JS_IsString(nv)) {
        JS_FreeValue(ctx, nv);
        return JS_ThrowTypeError(ctx, "XMLToObject(node): node needs a string `name`");
    }
    key = JS_ValueToAtom(ctx, nv);
    JS_FreeValue(ctx, nv);
    if (key == JS_ATOM_NULL)
        return JS_EXCEPTION;
    {
        xml_atoms_t at;
        xb_t text;
        if (xml_atoms_init(ctx, &at) < 0) {
            JS_FreeAtom(ctx, key);
            return JS_EXCEPTION;
        }
        xb_init(&text);
        sub = xml_collapse(ctx, &at, &text, argv[0], 0);
        xb_free(&text);
        xml_atoms_free(ctx, &at);
    }
    if (JS_IsException(sub)) {
        JS_FreeAtom(ctx, key);
        return JS_EXCEPTION;
    }
    out = JS_NewObject(ctx);
    if (JS_IsException(out) || JS_DefinePropertyValue(ctx, out, key, sub, JS_PROP_C_W_E) < 0) {
        JS_FreeAtom(ctx, key);
        JS_FreeValue(ctx, out);
        return JS_EXCEPTION;
    }
    JS_FreeAtom(ctx, key);
    return out;
}

static const JSCFunctionListEntry dyn_xml_funcs[] = {
    JS_CFUNC_DEF("XMLParse", 1, xml_parse),
    JS_CFUNC_DEF("XMLStringify", 1, xml_stringify),
    JS_CFUNC_DEF("XMLToObject", 1, xml_to_object),
};

static const char dyn_sax_iterate_gen_js[] = "(async function* (parser, source) {\n"
                                             "    if (source == null)\n"
                                             "        throw new TypeError('SAXParser.iterate(source): source is required');\n"
                                             "    var ev;\n"
                                             "    for await (var chunk of source) {\n"
                                             "        parser.write(chunk);\n"
                                             "        while ((ev = parser.next()) !== null)\n"
                                             "            yield ev;\n"
                                             "    }\n"
                                             "    parser.end();\n"
                                             "    while ((ev = parser.next()) !== null)\n"
                                             "        yield ev;\n"
                                             "})";

static int dyn_xml_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto, ctor, genfn;
    int rc;

    if (dyn_register_plain_class(ctx, m, &dyn_sax_class_id, &dyn_sax_class,
            dyn_sax_proto, countof(dyn_sax_proto),
            dyn_sax_ctor, "SAXParser")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_xw_class_id, &dyn_xw_class,
            dyn_xw_proto, countof(dyn_xw_proto),
            xml_writer_ctor, "XmlWriter")
        < 0)
        return -1;
    proto = JS_GetClassProto(ctx, dyn_sax_class_id);
    if (JS_IsException(proto))
        return -1;
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return -1;
    }
    genfn = JS_Eval(ctx, dyn_sax_iterate_gen_js,
        strlen(dyn_sax_iterate_gen_js),
        "<dyna:xml:iterate>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsFunction(ctx, genfn)) {
        if (!JS_IsException(genfn))
            JS_ThrowTypeError(ctx,
                "dyna:xml: iterate glue did not evaluate to a function");
        JS_FreeValue(ctx, genfn);
        JS_FreeValue(ctx, ctor);
        return -1;
    }
    {
        JSValue itfn = JS_NewCFunction(ctx, dyn_sax_iterate, "iterate", 2);
        if (JS_IsException(itfn)
            || JS_DefinePropertyValueStr(ctx, ctor, "iterate", itfn, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, itfn);
            JS_FreeValue(ctx, ctor);
            return -1;
        }
    }
    if (JS_DefinePropertyValueStr(ctx, ctor, DYN_SAX_GEN_PROP, genfn,
            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)
        < 0) {
        JS_FreeValue(ctx, genfn);
        JS_FreeValue(ctx, ctor);
        return -1;
    }
    rc = JS_SetModuleExportList(ctx, m, dyn_xml_funcs, countof(dyn_xml_funcs));
    JS_FreeValue(ctx, ctor);
    return rc;
}

int js_nat_init_xml(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:xml", dyn_xml_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "SAXParser");
    JS_AddModuleExport(ctx, m, "XmlWriter");
    return JS_AddModuleExportList(ctx, m, dyn_xml_funcs, countof(dyn_xml_funcs));
}

#endif
