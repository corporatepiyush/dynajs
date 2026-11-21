#include "html-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTML)

typedef struct {
    JSContext* ctx;
    JSValue el[HT_MAX_DEPTH];
    int32_t idx[HT_MAX_DEPTH];
    uint8_t last[HT_MAX_DEPTH];
    int32_t ord[HT_MAX_DEPTH];
    int depth;
    uint8_t* memo;
    size_t memocap;
    uint32_t nunit;
    uint32_t ordn;
    uint64_t work;
    int exhausted;
    ht_keys_t k;
    JSAtom a_class, a_id;
    JSAtom* patom;
} sel_ctx_t;

#define SEL_MEMO_MIN_UNITS 2
#define SEL_MEMO_MAX_BYTES (64u << 20)
#define SEL_WORK_MAX ((uint64_t)1 << 24)

typedef struct {
    JSValue out;
    uint32_t n;
    int want_first;
    int done;
} sel_sink_t;

static int sel_attr_eq(sel_ctx_t* c, JSValueConst el, JSAtom name_atom,
    const char* want, int op)
{
    JSContext* ctx = c->ctx;
    JSValue attrs = ht_get(ctx, &c->k, el, HT_K_ATTRS), v;
    const char* s;
    size_t sn, wn;
    int r = 0;

    if (!JS_IsObject(attrs)) {
        JS_FreeValue(ctx, attrs);
        return 0;
    }
    {
        JSPropertyDescriptor desc;
        int own;
        own = JS_GetOwnProperty(ctx, &desc, attrs, name_atom);
        if (own < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        if (own > 0) {
            v = JS_DupValue(ctx, desc.value);
            JS_FreeValue(ctx, desc.value);
            JS_FreeValue(ctx, desc.getter);
            JS_FreeValue(ctx, desc.setter);
        } else {
            v = JS_UNDEFINED;
        }
    }
    JS_FreeValue(ctx, attrs);
    if (JS_IsException(v) || JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (op == AO_HAS) {
        JS_FreeValue(ctx, v);
        return 1;
    }
    s = JS_ToCStringLen(ctx, &sn, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return 0;
    wn = strlen(want);
    switch (op) {
    case AO_EQ:
        r = sn == wn && memcmp(s, want, wn) == 0;
        break;
    case AO_PREFIX:
        r = wn && sn >= wn && memcmp(s, want, wn) == 0;
        break;
    case AO_SUFFIX:
        r = wn && sn >= wn && memcmp(s + sn - wn, want, wn) == 0;
        break;
    case AO_CONTAINS:
        r = wn && sn >= wn && strstr(s, want) != NULL;
        break;
    default: {
        size_t i = 0;
        while (i < sn && !r) {
            size_t st;
            while (i < sn && ht_space((unsigned char)s[i]))
                i++;
            st = i;
            while (i < sn && !ht_space((unsigned char)s[i]))
                i++;
            r = (i - st) == wn && memcmp(s + st, want, wn) == 0;
        }
        break;
    }
    }
    JS_FreeCString(ctx, s);
    return r;
}

static int sel_name_is(sel_ctx_t* c, JSValueConst el, const char* want)
{
    JSContext* ctx = c->ctx;
    JSValue nv = ht_get(ctx, &c->k, el, HT_K_NAME);
    const uint8_t* b;
    const char* s;
    size_t n, i;
    int r;

    if (!JS_IsString(nv)) {
        JS_FreeValue(ctx, nv);
        return 0;
    }
    b = JS_GetNarrowStringBytes(ctx, nv, &n);
    if (b) {
        for (i = 0; i < n && b[i] < 0x80; i++)
            ;
        if (i == n) {
            r = ht_ieq((const char*)b, n, want);
            JS_FreeValue(ctx, nv);
            return r;
        }
    }
    s = JS_ToCStringLen(ctx, &n, nv);
    JS_FreeValue(ctx, nv);
    if (!s)
        return 0;
    r = ht_ieq(s, n, want);
    JS_FreeCString(ctx, s);
    return r;
}

static int sel_unit_match(sel_ctx_t* c, const sel_t* sel, const sel_unit_t* u,
    int depth)
{
    uint32_t k;
    JSValueConst el = c->el[depth];

    for (k = 0; k < u->n; k++) {
        const sel_part_t* p = &sel->parts[u->first + k];
        int ok;
        switch (p->kind) {
        case SP_TAG:
            ok = sel_name_is(c, el, p->a);
            break;
        case SP_ID:
            ok = sel_attr_eq(c, el, c->a_id, p->a, AO_EQ);
            break;
        case SP_CLASS:
            ok = sel_attr_eq(c, el, c->a_class, p->a, AO_WORD);
            break;
        case SP_ATTR:
            ok = sel_attr_eq(c, el, c->patom[u->first + k], p->b ? p->b : "", p->op);
            break;
        case SP_FIRST:
            ok = c->idx[depth] == 0;
            break;
        case SP_ANY:
            ok = 1;
            break;
        default:
            ok = c->last[depth];
            break;
        }
        if (!ok)
            return 0;
    }
    return 1;
}

static int sel_chain_at(sel_ctx_t* c, const sel_t* sel, uint32_t gstart,
    uint32_t u, int depth)
{
    uint8_t* slot = NULL;
    int r;

    if (c->memo && c->ord[depth] >= 0)
        slot = &c->memo[(size_t)c->ord[depth] * c->nunit + u];
    if (slot && *slot)
        return *slot == 2;
    if (c->work >= SEL_WORK_MAX) {
        c->exhausted = 1;
        return 0;
    }
    c->work++;
    if (!sel_unit_match(c, sel, &sel->units[u], depth))
        r = 0;
    else if (u == gstart)
        r = 1;
    else if (sel->units[u].comb == CB_CHILD)
        r = depth > 0 && sel_chain_at(c, sel, gstart, u - 1, depth - 1);
    else {
        int32_t d;
        r = 0;
        for (d = depth - 1; d >= 0; d--)
            if (sel_chain_at(c, sel, gstart, u - 1, d)) {
                r = 1;
                break;
            }
    }
    if (slot)
        *slot = r ? 2 : 1;
    return r;
}

static int sel_chain_match(sel_ctx_t* c, const sel_t* sel, uint32_t gstart,
    uint32_t gend, int depth)
{
    return sel_chain_at(c, sel, gstart, gend - 1, depth);
}

static int sel_matches_here(sel_ctx_t* c, const sel_t* sel)
{
    uint32_t g;

    for (g = 0; g < sel->ngroup; g++) {
        uint32_t start = sel->groups[g];
        uint32_t end = g + 1 < sel->ngroup ? sel->groups[g + 1] : sel->nunit;
        if (end > start && sel_chain_match(c, sel, start, end, c->depth))
            return 1;
    }
    return 0;
}

static int sel_visit(sel_ctx_t* c, const sel_t* sel, JSValueConst kids,
    sel_sink_t* sink)
{
    int64_t len = 0, i;
    int rc = 0;

    if (ht_len(c->ctx, ht_get(c->ctx, &c->k, kids, HT_K_LENGTH), &len))
        return -1;
    {
        int64_t nelem = -1, epos = 0;
        if (sel->pos_used) {
            int64_t q;
            nelem = 0;
            for (q = 0; q < len; q++) {
                JSValue probe = JS_GetPropertyUint32(c->ctx, kids, (uint32_t)q);
                if (JS_IsException(probe))
                    return -1;
                if (JS_IsObject(probe))
                    nelem++;
                JS_FreeValue(c->ctx, probe);
            }
        }
        for (i = 0; i < len && rc == 0 && !sink->done; i++) {
            JSValue el = JS_GetPropertyUint32(c->ctx, kids, (uint32_t)i), sub;
            if (JS_IsException(el))
                return -1;
            if (!JS_IsObject(el)) {
                JS_FreeValue(c->ctx, el);
                continue;
            }
            if (c->depth + 1 >= HT_MAX_DEPTH) {
                JS_FreeValue(c->ctx, el);
                JS_ThrowRangeError(c->ctx, "Selector: the tree is nested deeper than %d levels", HT_MAX_DEPTH);
                return -1;
            }
            c->depth++;
            c->el[c->depth] = el;
            c->idx[c->depth] = (int32_t)(nelem >= 0 ? epos : i);
            c->last[c->depth] = (uint8_t)(nelem >= 0 ? (epos + 1 == nelem) : (i + 1 == len));
            epos++;
            c->ord[c->depth] = (int32_t)c->ordn;
            c->ordn++;
            if (c->memo) {
                size_t need = (size_t)c->ordn * c->nunit;
                if (need > c->memocap) {
                    JSRuntime* rt = JS_GetRuntime(c->ctx);
                    size_t nc = c->memocap ? c->memocap : 4096;
                    uint8_t* np;
                    while (nc < need && nc < SEL_MEMO_MAX_BYTES)
                        nc *= 2;
                    if (nc < need) {
                        js_free_rt(rt, c->memo);
                        c->memo = NULL;
                    } else {
                        np = (uint8_t*)js_realloc_rt(rt, c->memo, nc);
                        if (!np) {
                            js_free_rt(rt, c->memo);
                            c->memo = NULL;
                        } else {
                            memset(np + c->memocap, 0, nc - c->memocap);
                            c->memo = np;
                            c->memocap = nc;
                        }
                    }
                }
            }
            if (sel_matches_here(c, sel)) {
                if (sink->want_first) {
                    sink->out = JS_DupValue(c->ctx, el);
                    sink->done = 1;
                } else if (JS_DefinePropertyValueUint32(c->ctx, sink->out, sink->n++,
                               JS_DupValue(c->ctx, el),
                               JS_PROP_C_W_E)
                    < 0) {
                    rc = -1;
                }
            }
            sub = ht_get(c->ctx, &c->k, el, HT_K_CHILDREN);
            if (rc == 0 && !sink->done && JS_IsArray(c->ctx, sub) == 1)
                rc = sel_visit(c, sel, sub, sink);
            JS_FreeValue(c->ctx, sub);
            c->depth--;
            JS_FreeValue(c->ctx, el);
        }
    }
    return rc;
}

void dyn_sel_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sel_free((sel_t*)JS_GetOpaque(val, dyn_sel_class_id));
}

JSValue dyn_sel_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    char err[160] = { 0 };
    sel_t* sel;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "new Selector(text): text must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n > HT_MAX_SEL) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "new Selector: exceeds %u bytes", HT_MAX_SEL);
    }
    sel = sel_compile(s, n, err, sizeof err);
    JS_FreeCString(ctx, s);
    if (!sel)
        return JS_ThrowSyntaxError(ctx, "new Selector: %s",
            err[0] ? err : "out of memory");
    return dyn_plain_wrap(ctx, new_target, dyn_sel_class_id, sel, sel_free_v);
}

static void sel_keys_free(sel_ctx_t* c, const sel_t* sel)
{
    uint32_t i;
    ht_keys_free(c->ctx, &c->k);
    if (c->a_class != JS_ATOM_NULL)
        JS_FreeAtom(c->ctx, c->a_class);
    if (c->a_id != JS_ATOM_NULL)
        JS_FreeAtom(c->ctx, c->a_id);
    if (c->patom) {
        for (i = 0; i < sel->npart; i++)
            if (c->patom[i] != JS_ATOM_NULL)
                JS_FreeAtom(c->ctx, c->patom[i]);
        js_free(c->ctx, c->patom);
    }
}

static int sel_keys_init(sel_ctx_t* c, const sel_t* sel)
{
    uint32_t i;
    c->a_class = JS_ATOM_NULL;
    c->a_id = JS_ATOM_NULL;
    c->patom = NULL;
    if (ht_keys_init(c->ctx, &c->k) < 0)
        return -1;
    c->a_class = JS_NewAtom(c->ctx, "class");
    if (c->a_class == JS_ATOM_NULL)
        return -1;
    c->a_id = JS_NewAtom(c->ctx, "id");
    if (c->a_id == JS_ATOM_NULL)
        return -1;
    if (!sel->npart)
        return 0;
    c->patom = (JSAtom*)js_mallocz(c->ctx, sel->npart * sizeof *c->patom);
    if (!c->patom)
        return -1;
    for (i = 0; i < sel->npart; i++)
        if (sel->parts[i].kind == SP_ATTR) {
            c->patom[i] = JS_NewAtom(c->ctx, sel->parts[i].a);
            if (c->patom[i] == JS_ATOM_NULL)
                return -1;
        }
    return 0;
}

JSValue dyn_sel_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sel_t* sel = (sel_t*)dyn_plain_get(ctx, this_val, dyn_sel_class_id);
    sel_ctx_t c;
    sel_sink_t sink;
    JSValue kids, res;
    int rc;

    if (!sel)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "a document or node is required");
    c.ctx = ctx;
    c.depth = 0;
    c.memo = NULL;
    c.memocap = 0;
    c.nunit = 0;
    c.ordn = 0;
    c.work = 0;
    c.exhausted = 0;
    if (sel_keys_init(&c, sel) < 0) {
        sel_keys_free(&c, sel);
        return JS_EXCEPTION;
    }
    if (magic == 2) {
        if (sel->nunit > sel->ngroup) {
            sel_keys_free(&c, sel);
            return JS_ThrowTypeError(ctx, "Selector.matches: this selector has a "
                                          "combinator, and matches() sees one node with no ancestors");
        }
        if (!JS_IsObject(argv[0])) {
            sel_keys_free(&c, sel);
            return JS_NewBool(ctx, 0);
        }
        c.depth = 0;
        c.el[0] = argv[0];
        c.idx[0] = 0;
        c.last[0] = 1;
        c.ord[0] = 0;
        c.ordn = 1;
        res = JS_NewBool(ctx, sel_matches_here(&c, sel));
        sel_keys_free(&c, sel);
        return res;
    }
    if (sel->nunit >= SEL_MEMO_MIN_UNITS) {
        c.nunit = sel->nunit;
        c.memo = (uint8_t*)js_malloc_rt(JS_GetRuntime(ctx), 4096);
        if (c.memo) {
            memset(c.memo, 0, 4096);
            c.memocap = 4096;
        }
    }
    memset(&sink, 0, sizeof sink);
    sink.want_first = magic == 1;
    sink.out = magic == 1 ? JS_UNDEFINED : JS_NewArray(ctx);
    if (magic == 0 && JS_IsException(sink.out)) {
        js_free_rt(JS_GetRuntime(ctx), c.memo);
        sel_keys_free(&c, sel);
        return sink.out;
    }
    if (JS_IsArray(ctx, argv[0]) == 1) {
        kids = JS_DupValue(ctx, argv[0]);
    } else {
        kids = JS_NewArray(ctx);
        if (!JS_IsException(kids))
            JS_DefinePropertyValueUint32(ctx, kids, 0, JS_DupValue(ctx, argv[0]),
                JS_PROP_C_W_E);
    }
    if (JS_IsException(kids)) {
        JS_FreeValue(ctx, sink.out);
        js_free_rt(JS_GetRuntime(ctx), c.memo);
        sel_keys_free(&c, sel);
        return kids;
    }
    c.depth = -1;
    rc = sel_visit(&c, sel, kids, &sink);
    JS_FreeValue(ctx, kids);
    js_free_rt(JS_GetRuntime(ctx), c.memo);
    sel_keys_free(&c, sel);
    if (rc < 0) {
        JS_FreeValue(ctx, sink.out);
        return JS_EXCEPTION;
    }
    if (c.exhausted) {
        JS_FreeValue(ctx, sink.out);
        return JS_ThrowRangeError(ctx, "Selector: match work budget exceeded "
                                       "(selector too complex for this document)");
    }
    return sink.out;
}

#endif
