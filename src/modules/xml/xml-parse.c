#include "xml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_XML)

typedef struct {
    JSContext* ctx;
    int depth, nroots;
    int trim;
    int allow_multi_roots;
    JSObjectTemplate* el_tpl;
    xb_t open_names;
    size_t name_off[XML_MAX_DEPTH];
    uint32_t counts[XML_MAX_DEPTH];
    JSValue stack[XML_MAX_DEPTH];
} xml_tree_t;

#define XML_TPL_MIN_INPUT 256

typedef struct {
    xml_scan_t scan;
    xml_ev_t ev;
} xml_parse_t;

static void xml_parse_free(xml_parse_t* p)
{
    xb_free(&p->scan.buf);
    xb_free(&p->ev.text);
    xb_free(&p->ev.abuf);
}

static const char* const xml_parse_keys[] = { "multiple", "trim", "entities" };

static int xml_tree_open(JSContext* ctx, xml_tree_t* t, const xml_ev_t* ev,
    char* err, size_t errsz)
{
    JSValue kids, el;

    if (t->depth + 1 >= XML_MAX_DEPTH) {
        snprintf(err, errsz, "nesting exceeds %d elements", XML_MAX_DEPTH);
        return -1;
    }
    el = xml_new_element(ctx, ev, &kids, t->el_tpl);
    if (JS_IsException(el))
        return -3;
    if (t->depth == 0 && ++t->nroots > 1 && !t->allow_multi_roots) {
        JS_FreeValue(ctx, kids);
        JS_FreeValue(ctx, el);
        snprintf(err, errsz, "a document has one root element");
        return -1;
    }
    if (JS_DefinePropertyValueUint32(ctx, t->stack[t->depth],
            t->counts[t->depth]++, el,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, kids);
        return -3;
    }
    if (ev->selfclose) {
        JS_FreeValue(ctx, kids);
        return 0;
    }
    t->depth++;
    t->stack[t->depth] = kids;
    t->name_off[t->depth] = t->open_names.n;
    xb_write(&t->open_names, ev->name, ev->nlen);
    if (t->open_names.oom) {
        JS_FreeValue(ctx, kids);
        t->depth--;
        JS_ThrowOutOfMemory(ctx);
        return -3;
    }
    t->counts[t->depth] = 0;
    return 0;
}

static int xml_tree_close(JSContext* ctx, xml_tree_t* t, const xml_ev_t* ev,
    char* err, size_t errsz)
{
    const char* want;
    size_t wlen;

    if (t->depth == 0) {
        snprintf(err, errsz, "close tag with no open tag");
        return -1;
    }
    want = (const char*)t->open_names.p + t->name_off[t->depth];
    wlen = t->open_names.n - t->name_off[t->depth];
    if (wlen != ev->nlen || memcmp(want, ev->name, wlen) != 0) {
        snprintf(err, errsz, "</%.*s> closes <%.*s>", (int)ev->nlen, ev->name,
            (int)wlen, want);
        return -1;
    }
    JS_FreeValue(ctx, t->stack[t->depth]);
    t->open_names.n = t->name_off[t->depth];
    t->depth--;
    return 0;
}

static int xml_tree_text(JSContext* ctx, xml_tree_t* t, const xml_ev_t* ev,
    char* err, size_t errsz)
{
    JSValue sv;
    int blank;

    const uint8_t* tp = ev->text.p;
    size_t tn = ev->text.n;

    if (tn == 0)
        return 0;
    blank = ev->kind == XEV_TEXT && xml_all_space(tp, tn);
    if (blank && (t->trim || t->depth == 0))
        return 0;
    if (t->trim && ev->kind == XEV_TEXT) {
        while (tn > 0 && xml_space(tp[0])) {
            tp++;
            tn--;
        }
        while (tn > 0 && xml_space(tp[tn - 1]))
            tn--;
        if (tn == 0)
            return 0;
    }
    if (t->depth == 0) {
        snprintf(err, errsz, "text outside the root element");
        return -1;
    }
    sv = JS_NewStringLen(ctx, (const char*)tp, tn);
    if (JS_IsException(sv)
        || JS_DefinePropertyValueUint32(ctx, t->stack[t->depth],
               t->counts[t->depth]++, sv,
               JS_PROP_C_W_E)
            < 0)
        return -3;
    return 0;
}

JSValue xml_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* src;
    size_t slen;
    xml_parse_t p;
    xml_tree_t t;
    JSValue root_children, result;
    xml_opts_t opts;
    int rc = 0;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "XMLParse(text, options): text must be a string");
    if (dyn_opts_strict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
            xml_parse_keys, countof(xml_parse_keys))
        < 0)
        return JS_EXCEPTION;
    if (xml_read_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &opts) < 0)
        return JS_EXCEPTION;
    src = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    if (slen > XML_MAX_INPUT) {
        JS_FreeCString(ctx, src);
        return JS_ThrowRangeError(ctx, "XMLParse: input exceeds %u bytes", XML_MAX_INPUT);
    }
    memset(&p.scan, 0, sizeof p.scan);
    memset(&p.ev, 0, offsetof(xml_ev_t, attr));
    p.ev.nattr = 0;
    p.ev.kind = 0;
    p.ev.selfclose = 0;
    xb_init(&p.scan.buf);
    xb_init(&p.ev.text);
    xb_init(&p.ev.abuf);
    if (slen > 8192) {
        xb_reserve(&p.ev.text, 8192);
        xb_reserve(&p.ev.abuf, 4096);
    }
    p.scan.final = 1;
    p.scan.keep_entities = opts.keep_entities;
    xb_reserve(&p.scan.buf, slen);
    xb_write(&p.scan.buf, src, slen);
    JS_FreeCString(ctx, src);
    if (p.scan.buf.oom) {
        xml_parse_free(&p);
        return JS_ThrowOutOfMemory(ctx);
    }
    xml_scan_bom(&p.scan);
    t.ctx = ctx;
    t.depth = 0;
    t.nroots = 0;
    t.trim = opts.trim;
    t.allow_multi_roots = opts.multiple;
    t.counts[0] = 0;
    t.name_off[0] = 0;
    xb_init(&t.open_names);
    t.el_tpl = slen >= XML_TPL_MIN_INPUT ? xml_element_template(ctx) : NULL;
    root_children = JS_NewArray(ctx);
    if (JS_IsException(root_children)) {
        JS_FreeObjectTemplate(ctx, t.el_tpl);
        xml_parse_free(&p);
        return JS_EXCEPTION;
    }
    t.stack[0] = root_children;

    for (;;) {
        int r = xml_step(&p.scan, &p.ev);
        if (r == XS_END)
            break;
        if (r == XS_NEED) {
            rc = -2;
            break;
        }
        if (r == XS_ERR) {
            rc = -1;
            break;
        }
        switch (p.ev.kind) {
        case XEV_OPEN:
            rc = xml_tree_open(ctx, &t, &p.ev, p.scan.err, sizeof p.scan.err);
            break;
        case XEV_CLOSE:
            rc = xml_tree_close(ctx, &t, &p.ev, p.scan.err, sizeof p.scan.err);
            break;
        case XEV_TEXT:
        case XEV_CDATA:
            rc = xml_tree_text(ctx, &t, &p.ev, p.scan.err, sizeof p.scan.err);
            break;
        default:
            break;
        }
        if (rc)
            break;
        xml_compact(&p.scan);
    }

    if (rc == 0 && t.depth != 0) {
        snprintf(p.scan.err, sizeof p.scan.err, "unclosed element");
        rc = -1;
    }
    if (rc == 0 && t.nroots == 0) {
        snprintf(p.scan.err, sizeof p.scan.err, "no root element");
        rc = -1;
    }
    if (rc == -2)
        snprintf(p.scan.err, sizeof p.scan.err, "unexpected end of document");
    while (t.depth > 0) {
        JS_FreeValue(ctx, t.stack[t.depth]);
        t.depth--;
    }
    xb_free(&t.open_names);
    JS_FreeObjectTemplate(ctx, t.el_tpl);
    if (rc == 0) {
        if (opts.multiple) {
            result = root_children;
            root_children = JS_UNDEFINED;
        } else
            result = JS_GetPropertyUint32(ctx, root_children, 0);
    } else if (rc == -3)
        result = JS_EXCEPTION;
    else
        result = JS_ThrowSyntaxError(ctx, "XMLParse: %s", p.scan.err);
    JS_FreeValue(ctx, root_children);
    xml_parse_free(&p);
    return result;
}

#endif
