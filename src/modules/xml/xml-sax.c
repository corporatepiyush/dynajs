#include "xml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_XML)

typedef struct {
    xml_scan_t scan;
    xml_ev_t ev;
    JSRuntime* rt;
    JSValue on[6];
    int ended, busy;
    int depth;
    int pend_close;
    JSObjectTemplate* ev_tpl[4];

    char* names[XML_MAX_DEPTH];
    size_t name_len[XML_MAX_DEPTH];
    int root_closed;
    int next_mode;
    int failed;
    int pull_done;
    char* pend_name;
    size_t pend_len;
    uint64_t pend_off;
} dyn_sax_t;

enum { SAX_SHAPE_OPEN,
    SAX_SHAPE_CLOSE,
    SAX_SHAPE_TEXT,
    SAX_SHAPE_PI };

static void dyn_sax_free(JSRuntime* rt, dyn_sax_t* s)
{
    int k;
    if (!s)
        return;
    for (k = 0; k < 6; k++)
        JS_FreeValueRT(rt, s->on[k]);
    while (s->depth > 0)
        free(s->names[--s->depth]);
    free(s->pend_name);
    for (k = 0; k < 4; k++)
        JS_FreeObjectTemplateRT(rt, s->ev_tpl[k]);
    xb_free(&s->scan.buf);
    xb_free(&s->ev.text);
    xb_free(&s->ev.abuf);
    free(s);
}

static void dyn_sax_dispose(void* p)
{
    dyn_sax_t* s = (dyn_sax_t*)p;
    if (s)
        dyn_sax_free(s->rt, s);
}

void dyn_sax_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_sax_free(rt, (dyn_sax_t*)JS_GetOpaque(val, dyn_sax_class_id));
}

void dyn_sax_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    dyn_sax_t* s = (dyn_sax_t*)JS_GetOpaque(val, dyn_sax_class_id);
    int k;
    if (!s)
        return;
    for (k = 0; k < 6; k++)
        JS_MarkValue(rt, s->on[k], mark);
    for (k = 0; k < 4; k++)
        JS_MarkObjectTemplate(rt, s->ev_tpl[k], mark);
}

static const char* const DYN_SAX_NAMES[6] = {
    "onOpen", "onClose", "onText", "onCData", "onComment", "onPI"
};

JSValue dyn_sax_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_sax_t* s;
    xml_opts_t opts;
    int k;

    {
        int have_handlers = argc >= 1 && JS_IsObject(argv[0]);
        JSValueConst opt_src;
        if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])
            && !JS_IsObject(argv[0]))
            return JS_ThrowTypeError(ctx,
                "new SAXParser(handlers): handlers must be an object");
        opt_src = have_handlers ? argv[0]
                                : (argc >= 2 && JS_IsObject(argv[1]) ? argv[1] : JS_UNDEFINED);
        if (xml_read_opts(ctx, opt_src, &opts) < 0)
            return JS_EXCEPTION;
        s = (dyn_sax_t*)calloc(1, sizeof *s);
        if (!s)
            return JS_ThrowOutOfMemory(ctx);
        s->rt = JS_GetRuntime(ctx);
        for (k = 0; k < 6; k++)
            s->on[k] = JS_UNDEFINED;
        xb_init(&s->scan.buf);
        xb_init(&s->ev.text);
        xb_init(&s->ev.abuf);
        s->scan.keep_entities = opts.keep_entities;
        for (k = 0; k < 6 && have_handlers; k++) {
            JSValue h = JS_GetPropertyStr(ctx, argv[0], DYN_SAX_NAMES[k]);
            if (JS_IsException(h)) {
                dyn_sax_free(JS_GetRuntime(ctx), s);
                return JS_EXCEPTION;
            }
            if (JS_IsUndefined(h) || JS_IsNull(h)) {
                JS_FreeValue(ctx, h);
                continue;
            }
            if (!JS_IsFunction(ctx, h)) {
                JS_FreeValue(ctx, h);
                dyn_sax_free(JS_GetRuntime(ctx), s);
                return JS_ThrowTypeError(ctx, "new SAXParser: %s must be a function",
                    DYN_SAX_NAMES[k]);
            }
            s->on[k] = h;
        }
        s->next_mode = !have_handlers;
    }
    return dyn_plain_wrap(ctx, new_target, dyn_sax_class_id, s, dyn_sax_dispose);
}

static int dyn_xml_bytes(JSContext* ctx, JSValueConst v, const uint8_t** pp,
    size_t* pn)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    buf = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, v, &off, &len, &bpe) : JS_EXCEPTION;
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base)
            return -1;
        *pp = base;
        *pn = ab;
        return 0;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "chunk must be a string or a byte view");
        return -1;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

static JSValue dyn_sax_attrs(JSContext* ctx, const xml_ev_t* ev)
{
    JSValue attrs = JS_NewObject(ctx);
    uint32_t k;

    if (JS_IsException(attrs))
        return attrs;
    for (k = 0; k < ev->nattr; k++) {
        JSAtom a = JS_NewAtomLen(ctx, ev->attr[k].n, ev->attr[k].nlen);
        JSValue v;
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(ctx, attrs);
            return JS_EXCEPTION;
        }
        v = JS_NewStringLen(ctx, (const char*)ev->abuf.p + ev->attr[k].voff,
            ev->attr[k].vlen);
        if (JS_IsException(v)
            || JS_DefinePropertyValue(ctx, attrs, a, v, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(ctx, a);
            JS_FreeValue(ctx, attrs);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(ctx, a);
    }
    return attrs;
}

static int dyn_sax_emit(JSContext* ctx, dyn_sax_t* s, JSValueConst this_val)
{
    JSValue args[2], r;
    int nargs = 0, slot;
    const xml_ev_t* ev = &s->ev;

    switch (ev->kind) {
    case XEV_OPEN:
        slot = 0;
        break;
    case XEV_CLOSE:
        slot = 1;
        break;
    case XEV_TEXT:
        slot = 2;
        break;
    case XEV_CDATA:
        slot = 3;
        break;
    case XEV_COMMENT:
        slot = 4;
        break;
    case XEV_PI:
        slot = 5;
        break;
    default:
        return 0;
    }
    if (JS_IsUndefined(s->on[slot]))
        return 0;
    if (ev->kind == XEV_OPEN) {
        args[1] = dyn_sax_attrs(ctx, ev);
        if (JS_IsException(args[1]))
            return -1;
        args[0] = JS_NewStringLen(ctx, ev->name, ev->nlen);
        nargs = 2;
    } else if (ev->kind == XEV_PI) {
        args[0] = JS_NewStringLen(ctx, ev->name, ev->nlen);
        args[1] = JS_NewStringLen(ctx, ev->val, ev->vlen);
        nargs = 2;
    } else if (ev->kind == XEV_CLOSE) {
        args[0] = JS_NewStringLen(ctx, ev->name, ev->nlen);
        nargs = 1;
    } else {
        args[0] = JS_NewStringLen(ctx, (const char*)ev->text.p, ev->text.n);
        nargs = 1;
    }
    r = JS_Call(ctx, s->on[slot], this_val, nargs, (JSValueConst*)args);
    while (nargs--)
        JS_FreeValue(ctx, args[nargs]);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static int dyn_sax_root_check(dyn_sax_t* s, const xml_ev_t* ev)
{
    switch (ev->kind) {
    case XEV_OPEN:
        if (s->root_closed) {
            snprintf(s->scan.err, sizeof s->scan.err,
                "a document has one root element");
            return -1;
        }
        if (ev->selfclose) {
            char* copy = (char*)malloc(ev->nlen + 1);
            if (!copy) {
                snprintf(s->scan.err, sizeof s->scan.err, "out of memory");
                return -1;
            }
            memcpy(copy, ev->name, ev->nlen);
            copy[ev->nlen] = 0;
            free(s->pend_name);
            s->pend_name = copy;
            s->pend_len = ev->nlen;
            s->pend_close = 1;
            if (s->depth == 0)
                s->root_closed = 1;
        } else {
            char* copy;
            if (s->depth + 1 >= XML_MAX_DEPTH) {
                snprintf(s->scan.err, sizeof s->scan.err, "nesting exceeds 256");
                return -1;
            }
            copy = (char*)malloc(ev->nlen + 1);
            if (!copy) {
                snprintf(s->scan.err, sizeof s->scan.err, "out of memory");
                return -1;
            }
            memcpy(copy, ev->name, ev->nlen);
            copy[ev->nlen] = 0;
            s->names[s->depth] = copy;
            s->name_len[s->depth] = ev->nlen;
            s->depth++;
        }
        break;
    case XEV_CLOSE:
        if (s->depth == 0) {
            snprintf(s->scan.err, sizeof s->scan.err,
                "close tag with no open tag");
            return -1;
        }
        {
            size_t dl = (size_t)s->depth - 1;
            if (s->name_len[dl] != ev->nlen || memcmp(s->names[dl], ev->name, ev->nlen) != 0) {
                snprintf(s->scan.err, sizeof s->scan.err,
                    "</%.*s> closes <%.*s>", (int)ev->nlen, ev->name,
                    (int)s->name_len[dl], s->names[dl]);
                return -1;
            }
        }
        free(s->names[--s->depth]);
        if (s->depth == 0)
            s->root_closed = 1;
        break;
    case XEV_TEXT:
    case XEV_CDATA:
        if (s->depth == 0) {
            if (ev->kind == XEV_TEXT && xml_all_space(ev->text.p, ev->text.n))
                return 1;
            snprintf(s->scan.err, sizeof s->scan.err,
                "text outside the root element");
            return -1;
        }
        break;
    default:
        break;
    }
    return 0;
}

static int dyn_sax_append(JSContext* ctx, dyn_sax_t* s, JSValueConst chunk)
{
    const uint8_t* p = NULL;
    const char* str = NULL;
    size_t n = 0;

    if (JS_IsString(chunk)) {
        str = JS_ToCStringLen(ctx, &n, chunk);
        if (!str)
            return -1;
        p = (const uint8_t*)str;
    } else if (dyn_xml_bytes(ctx, chunk, &p, &n) < 0) {
        return -1;
    }
    xb_write(&s->scan.buf, p, n);
    if (str)
        JS_FreeCString(ctx, str);
    if (s->scan.buf.oom) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    s->scan.total += n;
    if (s->scan.total > XML_MAX_INPUT) {
        JS_ThrowRangeError(ctx, "SAXParser: input exceeds %u bytes",
            (unsigned)XML_MAX_INPUT);
        return -1;
    }
    return 0;
}

static JSObjectTemplate* dyn_sax_shape(JSContext* ctx, dyn_sax_t* s, int shape)
{
    static const char* const FIELDS[4][4] = {
        { "event", "offset", "name", "attrs" },
        { "event", "offset", "name", NULL },
        { "event", "offset", "text", NULL },
        { "event", "offset", "name", "text" },
    };
    JSAtom atoms[4];
    uint32_t n = 0, k;
    int ok = 1;

    if (s->ev_tpl[shape])
        return s->ev_tpl[shape];
    while (n < 4 && FIELDS[shape][n]) {
        atoms[n] = JS_NewAtom(ctx, FIELDS[shape][n]);
        if (atoms[n] == JS_ATOM_NULL)
            ok = 0;
        n++;
    }
    if (ok)
        s->ev_tpl[shape] = JS_NewObjectTemplate(ctx, atoms, n);
    for (k = 0; k < n; k++)
        JS_FreeAtom(ctx, atoms[k]);
    return s->ev_tpl[shape];
}

static JSValue dyn_sax_event_obj(JSContext* ctx, dyn_sax_t* s, const xml_ev_t* ev,
    uint64_t offset)
{
    static const char* const KIND_NAMES[6] = {
        "open", "close", "text", "cdata", "comment", "pi"
    };
    JSObjectTemplate* tpl;
    JSValue obj, cells[4];
    int kind, shape, bad = 0;
    uint32_t n = 0, k;

    switch (ev->kind) {
    case XEV_OPEN:
        kind = 0;
        shape = SAX_SHAPE_OPEN;
        break;
    case XEV_CLOSE:
        kind = 1;
        shape = SAX_SHAPE_CLOSE;
        break;
    case XEV_TEXT:
        kind = 2;
        shape = SAX_SHAPE_TEXT;
        break;
    case XEV_CDATA:
        kind = 3;
        shape = SAX_SHAPE_TEXT;
        break;
    case XEV_COMMENT:
        kind = 4;
        shape = SAX_SHAPE_TEXT;
        break;
    case XEV_PI:
        kind = 5;
        shape = SAX_SHAPE_PI;
        break;
    default:
        return JS_UNDEFINED;
    }
    tpl = dyn_sax_shape(ctx, s, shape);
    if (!tpl)
        return JS_ThrowOutOfMemory(ctx);
    cells[n++] = JS_NewString(ctx, KIND_NAMES[kind]);
    cells[n++] = JS_NewInt64(ctx, (int64_t)offset);
    if (shape == SAX_SHAPE_TEXT) {
        cells[n++] = JS_NewStringLen(ctx, (const char*)ev->text.p, ev->text.n);
    } else {
        cells[n++] = JS_NewStringLen(ctx, ev->name, ev->nlen);
        if (shape == SAX_SHAPE_OPEN)
            cells[n++] = dyn_sax_attrs(ctx, ev);
        else if (shape == SAX_SHAPE_PI)
            cells[n++] = JS_NewStringLen(ctx, ev->val, ev->vlen);
    }
    obj = JS_NewObjectFromTemplate(ctx, tpl);
    for (k = 0; k < n; k++)
        if (JS_IsException(cells[k]))
            bad = 1;
    if (bad || JS_IsException(obj)) {
        for (k = 0; k < n; k++)
            JS_FreeValue(ctx, cells[k]);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    for (k = 0; k < n; k++)
        JS_SetTemplateCell(ctx, obj, k, cells[k]);
    return obj;
}

static int dyn_sax_end_checks(dyn_sax_t* s)
{
    if (s->depth > 0) {
        snprintf(s->scan.err, sizeof s->scan.err, "unclosed element");
        return -1;
    }
    if (!s->root_closed) {
        snprintf(s->scan.err, sizeof s->scan.err, "no root element");
        return -1;
    }
    if (s->scan.pos < s->scan.buf.n) {
        snprintf(s->scan.err, sizeof s->scan.err, "unexpected end of document");
        return -1;
    }
    return 0;
}

static int dyn_sax_deliver(dyn_sax_t* s, JSContext* ctx, JSValueConst this_val,
    int deliver, uint64_t off, JSValue* out)
{
    if (deliver)
        return dyn_sax_emit(ctx, s, this_val) < 0 ? -2 : 1;
    *out = dyn_sax_event_obj(ctx, s, &s->ev, off);
    if (JS_IsException(*out))
        return -2;
    return 1;
}

static int dyn_sax_pump(dyn_sax_t* s, JSContext* ctx, JSValueConst this_val,
    int deliver, JSValue* out)
{
    uint64_t off;
    int r, skip;

    *out = JS_UNDEFINED;

    if (s->pend_close) {
        s->pend_close = 0;
        s->ev.kind = XEV_CLOSE;
        s->ev.selfclose = 0;
        s->ev.name = s->pend_name;
        s->ev.nlen = s->pend_len;
        return dyn_sax_deliver(s, ctx, this_val, deliver, s->pend_off, out);
    }
    if (!s->scan.bom_done && s->scan.buf.n < 3 && !s->scan.final)
        return 0;
    xml_scan_bom(&s->scan);
    for (;;) {
        off = s->scan.total - s->scan.buf.n + s->scan.pos;
        r = xml_step(&s->scan, &s->ev);
        if (r == XS_NEED || r == XS_END)
            return 0;
        if (r == XS_ERR)
            return -1;
        skip = dyn_sax_root_check(s, &s->ev);
        if (skip < 0)
            return -1;
        if (skip)
            continue;
        if (s->ev.kind == XEV_SKIP)
            continue;
        if (s->pend_close)
            s->pend_off = s->scan.total - s->scan.buf.n + s->scan.pos - 2;
        r = dyn_sax_deliver(s, ctx, this_val, deliver, off, out);
        if (r == 1)
            xml_compact(&s->scan);
        return r;
    }
}

JSValue dyn_sax_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_sax_t* s = (dyn_sax_t*)dyn_plain_get(ctx, this_val, dyn_sax_class_id);
    JSValue out;
    int r;

    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->busy)
        return JS_ThrowTypeError(ctx, "SAXParser: next() called from a handler");
    if (s->failed)
        return JS_ThrowSyntaxError(ctx, "SAXParser: %s", s->scan.err);
    if (s->pull_done)
        return JS_NULL;
    s->busy = 1;
    r = dyn_sax_pump(s, ctx, this_val, !s->next_mode, &out);
    s->busy = 0;
    if (r == 1)
        return JS_IsUndefined(out) ? JS_NULL : out;
    if (r == -2) {
        JS_FreeValue(ctx, out);
        return JS_EXCEPTION;
    }
    if (r == -1) {
        s->failed = 1;
        return JS_ThrowSyntaxError(ctx, "SAXParser: %s", s->scan.err);
    }
    if (s->scan.final) {
        if (dyn_sax_end_checks(s) < 0) {
            s->failed = 1;
            return JS_ThrowSyntaxError(ctx, "SAXParser: %s", s->scan.err);
        }
        s->pull_done = 1;
        return JS_NULL;
    }
    return JS_NULL;
}

static int dyn_sax_drive(dyn_sax_t* s, JSContext* ctx, JSValueConst this_val)
{
    for (;;) {
        JSValue out = JS_UNDEFINED;
        int r = dyn_sax_pump(s, ctx, this_val, 1, &out);
        if (r < 0) {
            JS_FreeValue(ctx, out);
            return r;
        }
        if (r == 0)
            return 0;
        JS_FreeValue(ctx, out);
    }
}

JSValue dyn_sax_feed(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dyn_sax_t* s = (dyn_sax_t*)dyn_plain_get(ctx, this_val, dyn_sax_class_id);
    int rc = 0;

    if (!s)
        return JS_EXCEPTION;
    if (s->busy)
        return JS_ThrowTypeError(ctx, "SAXParser: write() called from a handler");
    if (s->ended)
        return JS_ThrowTypeError(ctx, "SAXParser: end() has already been called");
    if (magic == 0) {
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "SAXParser.write(chunk): a chunk is required");
        if (dyn_sax_append(ctx, s, argv[0]) < 0)
            return JS_EXCEPTION;
        if (s->next_mode)
            return JS_UNDEFINED;
    } else {
        s->scan.final = 1;
        if (s->next_mode) {
            s->ended = 1;
            return JS_UNDEFINED;
        }
    }
    s->busy = 1;
    rc = dyn_sax_drive(s, ctx, this_val);
    s->busy = 0;
    if (magic == 1) {
        s->ended = 1;
        if (rc == 0 && dyn_sax_end_checks(s) < 0)
            rc = -1;
    }
    if (rc == -2)
        return JS_EXCEPTION;
    if (rc == -1) {
        s->failed = 1;
        return JS_ThrowSyntaxError(ctx, "SAXParser: %s", s->scan.err);
    }
    return JS_UNDEFINED;
}

JSValue dyn_sax_iterate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue parser, genfn, gen;
    JSValueConst cargs[2];

    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_ThrowTypeError(ctx,
            "SAXParser.iterate(source): source is required");
    cargs[0] = JS_UNDEFINED;
    cargs[1] = argc >= 2 ? argv[1] : JS_UNDEFINED;
    parser = JS_CallConstructor(ctx, this_val, 2, cargs);
    if (JS_IsException(parser))
        return JS_EXCEPTION;
    genfn = JS_GetPropertyStr(ctx, this_val, DYN_SAX_GEN_PROP);
    if (!JS_IsFunction(ctx, genfn)) {
        JS_FreeValue(ctx, genfn);
        JS_FreeValue(ctx, parser);
        return JS_ThrowTypeError(ctx,
            "SAXParser.iterate: internal generator unavailable");
    }
    {
        JSValueConst gargs[2] = { parser, argv[0] };
        gen = JS_Call(ctx, genfn, JS_UNDEFINED, 2, gargs);
    }
    JS_FreeValue(ctx, genfn);
    JS_FreeValue(ctx, parser);
    return gen;
}

#endif
