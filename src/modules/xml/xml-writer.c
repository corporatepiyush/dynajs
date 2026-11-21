#include "xml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_XML)

#define XML_WRITER_MAX_DEPTH 65536

typedef struct {
    xb_t out;
    xb_t names;
    size_t* stack;
    size_t n, cap;
    int indent;
    char* ind;
    size_t nind, ind_cap;
} xml_writer_t;

static void xml_writer_free(xml_writer_t* w)
{
    size_t i;
    if (!w)
        return;
    (void)i;
    free(w->stack);
    free(w->ind);
    xb_free(&w->names);
    xb_free(&w->out);
    free(w);
}

static void xml_writer_dispose(void* p) { xml_writer_free((xml_writer_t*)p); }

void xml_writer_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    xml_writer_free((xml_writer_t*)JS_GetOpaque(val, dyn_xw_class_id));
}

static const char* const xml_writer_keys[] = { "indent" };

static int xml_writer_valid_name(const char* s, size_t n)
{
    size_t i;
    if (n == 0 || !xml_name_start((unsigned char)s[0]))
        return 0;
    for (i = 1; i < n; i++)
        if (!xml_name_char((unsigned char)s[i]))
            return 0;
    return 1;
}

static void xml_writer_nl(xml_writer_t* w)
{
    if (!w->indent)
        return;
    xb_put(&w->out, '\n');
    xb_write(&w->out, w->ind, w->nind);
}

static void xml_writer_end_tag(xml_writer_t* w)
{
    size_t off = w->stack[--w->n];
    if (w->nind >= (size_t)w->indent)
        w->nind -= (size_t)w->indent;
    if (w->indent)
        xml_writer_nl(w);
    xb_write(&w->out, "</", 2);
    xb_write(&w->out, w->names.p + off, w->names.n - off);
    xb_put(&w->out, '>');
    w->names.n = off;
}

JSValue xml_writer_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    xml_writer_t* w;
    int32_t ind = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        if (dyn_opts_strict(ctx, argv[0], xml_writer_keys,
                countof(xml_writer_keys))
            < 0)
            return JS_EXCEPTION;
        {
            JSValue v = JS_GetPropertyStr(ctx, argv[0], "indent");
            if (JS_IsException(v))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                double d;
                if (!JS_IsNumber(v)) {
                    JS_FreeValue(ctx, v);
                    return JS_ThrowTypeError(ctx,
                        "XmlWriter: indent must be an integer from 0 to 16");
                }
                if (JS_ToFloat64(ctx, &d, v) < 0) {
                    JS_FreeValue(ctx, v);
                    return JS_EXCEPTION;
                }
                JS_FreeValue(ctx, v);
                if (!(d >= -2147483648.0 && d <= 2147483647.0) || d != (double)(int)d) {
                    return JS_ThrowTypeError(ctx,
                        "XmlWriter: indent must be an integer from 0 to 16");
                }
                ind = (int)d;
            } else {
                JS_FreeValue(ctx, v);
            }
        }
        if (ind < 0 || ind > 16)
            return JS_ThrowRangeError(ctx, "XmlWriter: indent is 0 to 16");
    } else if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        return JS_ThrowTypeError(ctx, "XmlWriter(options): options must be an object");
    }
    w = (xml_writer_t*)calloc(1, sizeof *w);
    if (!w)
        return JS_ThrowOutOfMemory(ctx);
    xb_init(&w->out);
    xb_init(&w->names);
    w->indent = ind;
    return dyn_plain_wrap(ctx, new_target, dyn_xw_class_id, w, xml_writer_dispose);
}

JSValue xml_writer_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* nm;
    size_t nml;
    JSValue attrs = JS_UNDEFINED;
    xml_writer_t* w;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "XmlWriter.open(name, attrs?): name must be a string");
    nm = JS_ToCStringLen(ctx, &nml, argv[0]);
    if (!nm)
        return JS_EXCEPTION;
    if (!xml_writer_valid_name(nm, nml)) {
        JS_FreeCString(ctx, nm);
        return JS_ThrowRangeError(ctx, "XmlWriter.open: not a valid element name");
    }
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (!JS_IsObject(argv[1])) {
            JS_FreeCString(ctx, nm);
            return JS_ThrowTypeError(ctx, "XmlWriter.open: attrs must be an object");
        }
        attrs = argv[1];
    }
    {
        xb_t ab;
        xb_init(&ab);
        if (JS_IsObject(attrs)) {
            JSPropertyEnum* tab = NULL;
            uint32_t len = 0, k;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, attrs,
                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
                < 0) {
                xb_free(&ab);
                JS_FreeCString(ctx, nm);
                return JS_EXCEPTION;
            }
            for (k = 0; k < len; k++) {
                JSValue v = JS_GetProperty(ctx, attrs, tab[k].atom);
                const char *an, *av;
                size_t anl, avl;
                if (JS_IsException(v)) {
                    JS_FreePropertyEnum(ctx, tab, len);
                    xb_free(&ab);
                    JS_FreeCString(ctx, nm);
                    return JS_EXCEPTION;
                }
                an = JS_AtomToCStringLen(ctx, &anl, tab[k].atom);
                av = JS_ToCStringLen(ctx, &avl, v);
                JS_FreeValue(ctx, v);
                if (!an || !av) {
                    if (an)
                        JS_FreeCString(ctx, an);
                    if (av)
                        JS_FreeCString(ctx, av);
                    JS_FreePropertyEnum(ctx, tab, len);
                    xb_free(&ab);
                    JS_FreeCString(ctx, nm);
                    return JS_EXCEPTION;
                }
                if (!xml_writer_valid_name(an, anl)) {
                    JS_FreeCString(ctx, an);
                    JS_FreeCString(ctx, av);
                    JS_FreePropertyEnum(ctx, tab, len);
                    xb_free(&ab);
                    JS_FreeCString(ctx, nm);
                    return JS_ThrowTypeError(ctx, "XmlWriter.open: invalid attribute name");
                }
                if (!xml_ok_chars(av, avl)) {
                    JS_FreeCString(ctx, an);
                    JS_FreeCString(ctx, av);
                    JS_FreePropertyEnum(ctx, tab, len);
                    xb_free(&ab);
                    JS_FreeCString(ctx, nm);
                    return JS_ThrowRangeError(ctx,
                        "XmlWriter.open: attribute value contains a character "
                        "not legal in XML 1.0");
                }
                xb_put(&ab, ' ');
                xb_write(&ab, an, anl);
                xb_puts(&ab, "=\"");
                xml_escape(&ab, av, avl, 1);
                xb_put(&ab, '"');
                JS_FreeCString(ctx, an);
                JS_FreeCString(ctx, av);
                if (ab.oom) {
                    JS_FreePropertyEnum(ctx, tab, len);
                    xb_free(&ab);
                    JS_FreeCString(ctx, nm);
                    return JS_ThrowOutOfMemory(ctx);
                }
            }
            JS_FreePropertyEnum(ctx, tab, len);
        }
        w = (xml_writer_t*)dyn_plain_get(ctx, this_val, dyn_xw_class_id);
        if (!w) {
            xb_free(&ab);
            JS_FreeCString(ctx, nm);
            return JS_EXCEPTION;
        }
        if (w->n >= XML_WRITER_MAX_DEPTH) {
            xb_free(&ab);
            JS_FreeCString(ctx, nm);
            return JS_ThrowRangeError(ctx, "XmlWriter: nesting exceeds %d",
                XML_WRITER_MAX_DEPTH);
        }
        {
            size_t mark = w->out.n;
            int was_oom = w->out.oom;
            if (w->n == w->cap) {
                size_t nc = w->cap ? w->cap * 2 : 8;
                size_t* ns = (size_t*)realloc(w->stack, nc * sizeof *ns);
                if (!ns)
                    goto fail_oom;
                w->stack = ns;
                w->cap = nc;
            }
            if (w->indent && w->nind + (size_t)w->indent > w->ind_cap) {
                size_t nc = (w->nind + (size_t)w->indent) * 2;
                char* ni = (char*)realloc(w->ind, nc);
                if (!ni)
                    goto fail_oom;
                memset(ni + w->ind_cap, ' ', nc - w->ind_cap);
                w->ind = ni;
                w->ind_cap = nc;
            }
            if (w->out.n > 0 && w->indent) {
                xb_put(&w->out, '\n');
                xb_write(&w->out, w->ind, w->nind);
            }
            xb_put(&w->out, '<');
            xb_write(&w->out, nm, nml);
            xb_write(&w->out, ab.p, ab.n);
            xb_put(&w->out, '>');
            if (ab.oom || w->out.oom) {
                w->out.n = mark;
                w->out.oom = was_oom;
                goto fail_oom;
            }
            w->stack[w->n++] = w->names.n;
            xb_write(&w->names, nm, nml);
            if (w->names.oom) {
                w->n--;
                w->names.n = w->stack[w->n];
                w->names.oom = 0;
                w->out.n = mark;
                goto fail_oom;
            }
            if (w->indent)
                w->nind += (size_t)w->indent;
            xb_free(&ab);
            JS_FreeCString(ctx, nm);
            return JS_UNDEFINED;
        fail_oom:
            xb_free(&ab);
            JS_FreeCString(ctx, nm);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
}

JSValue xml_writer_text(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    xml_writer_t* w = (xml_writer_t*)dyn_plain_get(ctx, this_val, dyn_xw_class_id);
    const char* s;
    size_t n;
    if (!w)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "XmlWriter.text(s): s must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (!xml_ok_chars(s, n)) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx,
            "XmlWriter.text: text contains a character not legal in XML 1.0");
    }
    xml_escape(&w->out, s, n, 0);
    JS_FreeCString(ctx, s);
    if (w->out.oom)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

JSValue xml_writer_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    xml_writer_t* w = (xml_writer_t*)dyn_plain_get(ctx, this_val, dyn_xw_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    if (w->n == 0)
        return JS_ThrowRangeError(ctx, "XmlWriter.close: no open element");
    xml_writer_end_tag(w);
    if (w->out.oom)
        return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}

JSValue xml_writer_to_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    xml_writer_t* w = (xml_writer_t*)dyn_plain_get(ctx, this_val, dyn_xw_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    if (w->out.oom)
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewStringLen(ctx, (const char*)w->out.p, w->out.n);
}

JSValue xml_writer_finish(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    xml_writer_t* w = (xml_writer_t*)dyn_plain_get(ctx, this_val, dyn_xw_class_id);
    JSValue out;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    while (w->n > 0) {
        xml_writer_end_tag(w);
        if (w->out.oom)
            return JS_ThrowOutOfMemory(ctx);
    }
    out = JS_NewStringLen(ctx, (const char*)w->out.p, w->out.n);
    return out;
}

#endif
