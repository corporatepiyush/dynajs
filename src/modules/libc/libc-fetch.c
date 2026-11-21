#include "dyna-nat.h"
#include "libc-internal.h"
#include "core/dyn-urlrec.h"

#define FX_URL_MAX 65536

#if defined(CONFIG_NATIVE_MODULE_HTTP)
#define FX_NET_IMPORT "(await import('dyna:http'))"
#else
#define FX_NET_IMPORT "((() => { throw new TypeError('fetch: this build has no HTTP client: http(s) fetch needs the optional \"http\" module'); })())"
#endif

static const char dyn_fetch_loop_src[] = "(function (H) {\n"
                                         "    const REQ = Symbol.for('dyna.http.request');\n"
                                         "    return async function fetch(input, init, timeoutMs, onConnect, cookieJar, dyna) {\n"
                                         "        const req = new H.Request(input, init);\n"
                                         "        const p = H.prep(req);\n"
                                         "        const signal = p.signal;\n"
                                         "        if (signal && signal.aborted) throw signal.reason;\n"
                                         "        let { url, method, headers, body } = p;\n"
                                         "        if (p.stream !== undefined) body = await H.readAll(p.stream);\n"
                                         "        const client = new " FX_NET_IMPORT ".HTTPClient(undefined, dyna === undefined ? undefined : { dyna });\n"
                                         "        if (timeoutMs) client.setTimeout(timeoutMs);\n"
                                         "        if (typeof onConnect === 'function') client.onConnect = onConnect;\n"
                                         "        if (cookieJar !== undefined && cookieJar !== null) client.useCookies(cookieJar);\n"
                                         "        for (let hop = 0; ; hop++) {\n"
                                         "            const rp = client[REQ](method, url, body, headers, true);\n"
                                         "            let raw;\n"
                                         "            if (signal) {\n"
                                         "                rp.catch(() => {});\n"
                                         "                let l;\n"
                                         "                const ap = new Promise((_, reject) => {\n"
                                         "                    l = () => { try { client.disconnect(); } catch (e) {} reject(signal.reason); };\n"
                                         "                    signal.addEventListener('abort', l);\n"
                                         "                });\n"
                                         "                try { raw = await Promise.race([rp, ap]); }\n"
                                         "                finally {\n"
                                         "                    H.untime(signal);\n"
                                         "                    signal.removeEventListener('abort', l);\n"
                                         "                }\n"
                                         "            } else raw = await rp;\n"
                                         "            const nx = H.hop(client, p.redirect, raw, url, method, body, headers, hop);\n"
                                         "            if (nx === null) return H.responseFrom(raw, url, hop > 0);\n"
                                         "            ({ url, method, body, headers } = nx);\n"
                                         "        }\n"
                                         "    };\n"
                                         "})\n";

static JSValue js_fetch_streams_internal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    JSValue ns, internal;

    (void)this_val;
    if (argc > 0 && JS_ToBool(ctx, argv[0]) && (!ts || !ts->streams_ns_set))
        return JS_UNDEFINED;
    ns = js_streams_ns(ctx);
    if (JS_IsException(ns))
        return ns;
    internal = JS_GetPropertyStr(ctx, ns, "internal");
    JS_FreeValue(ctx, ns);
    return internal;
}

static JSClassID fx_headers_cid, fx_form_cid, fx_signal_cid, fx_ctrl_cid, fx_resp_cid, fx_req_cid, fx_hiter_cid, fx_fiter_cid;

typedef struct {
    JSAtom name;
    JSValue value;
} FxHE;

typedef struct {
    FxHE* e;
    uint32_t* ord;
    uint32_t n, cap, ord_n, ord_cap, ver, ord_ver;
    int guard;
} FxHdr;

static uint32_t fx_len(JSContext* ctx, JSValueConst arr);
static void fx_enumerable(JSContext* ctx, JSValueConst proto);

typedef struct {
    JSValue self, list;
} FxForm;

typedef struct {
    JSValue reason, onabort, timer;
    JSValue* lv;
    JSValue* deps;
    uint8_t* lf;
    uint32_t n, cap, nd, capd;
    uint8_t aborted;
} FxSignal;

static char fx_empty;

static JSValue fx_box(void* p)
{
    return (!p || p == &fx_empty) ? JS_UNDEFINED : JS_MKPTR(JS_TAG_OBJECT, p);
}

static void* fx_unbox(JSValue v)
{
    return JS_IsObject(v) ? JS_VALUE_GET_PTR(v) : &fx_empty;
}

static void fx_box_free(JSRuntime* rt, void* p)
{
    if (p && p != &fx_empty)
        JS_FreeValueRT(rt, fx_box(p));
}

static void fx_box_mark(JSRuntime* rt, void* p, JS_MarkFunc* mf)
{
    if (p && p != &fx_empty)
        JS_MarkValue(rt, fx_box(p), mf);
}

typedef struct {
    JSValue src, stream;
    uint8_t used, isnull;
} FxBody;

typedef struct {
    JSValue status_text, headers, url;
    FxBody body;
    uint16_t status;
    uint8_t type, redirected;
} FxResp;

typedef struct {
    JSValue method, url, headers, signal, extra;
    FxBody body;
    uint32_t bits;
} FxReq;

static JSValue fx_global(JSContext* ctx, const char* name)
{
    JSValue g = JS_GetGlobalObject(ctx), v = JS_GetPropertyStr(ctx, g, name);
    JS_FreeValue(ctx, g);
    return v;
}

static JSValue fx_invoke(JSContext* ctx, JSValueConst obj, const char* name, int argc, JSValueConst* argv)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue r = JS_Invoke(ctx, obj, a, argc, argv);
    JS_FreeAtom(ctx, a);
    return r;
}

static JSValue fx_new_global(JSContext* ctx, const char* name, int argc, JSValueConst* argv)
{
    JSValue c = fx_global(ctx, name), r;
    if (JS_IsException(c))
        return c;
    r = JS_CallConstructor(ctx, c, argc, argv);
    JS_FreeValue(ctx, c);
    return r;
}

static int fx_is_str(JSContext* ctx, JSValueConst v, const char* s)
{
    const char* p;
    int r;
    if (!JS_IsString(v))
        return 0;
    p = JS_ToCString(ctx, v);
    if (!p)
        return -1;
    r = strcmp(p, s) == 0;
    JS_FreeCString(ctx, p);
    return r;
}

static JSValue fx_lit(JSContext* ctx, const char* s)
{
    JSAtom a = JS_NewAtom(ctx, s);
    JSValue r = JS_AtomToString(ctx, a);
    JS_FreeAtom(ctx, a);
    return r;
}

static JSValue fx_str(JSContext* ctx, JSValueConst v)
{
    JSValue f, r;
    if (!JS_IsSymbol(v))
        return JS_ToString(ctx, v);
    f = fx_global(ctx, "String");
    r = JS_Call(ctx, f, JS_UNDEFINED, 1, &v);
    JS_FreeValue(ctx, f);
    return r;
}

static JSValue fx_throw_msg(JSContext* ctx, const char* a, JSValueConst mid, const char* b)
{
    JSValue s = JS_NewString(ctx, a), p[2], m, e;
    p[0] = fx_str(ctx, mid);
    if (JS_IsException(p[0])) {
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    p[1] = JS_NewString(ctx, b);
    m = fx_invoke(ctx, s, "concat", 2, (JSValueConst*)p);
    JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, p[0]);
    JS_FreeValue(ctx, p[1]);
    if (JS_IsException(m))
        return m;
    e = fx_new_global(ctx, "TypeError", 1, (JSValueConst*)&m);
    JS_FreeValue(ctx, m);
    if (JS_IsException(e))
        return e;
    return JS_Throw(ctx, e);
}

static int fx_tchar(unsigned char c)
{
    static const char extra[] = "!#$%&'*+-.^_`|~";
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c && strchr(extra, c));
}

static int fx_is_token(const char* p, size_t n)
{
    size_t i;
    if (!n)
        return 0;
    for (i = 0; i < n; i++)
        if (!fx_tchar((unsigned char)p[i]))
            return 0;
    return 1;
}

static JSValue fx_hname(JSContext* ctx, JSValueConst v, int strict)
{
    JSValue s = fx_str(ctx, v), r;
    const char* p;
    size_t n, i;
    char buf[256];
    if (JS_IsException(s))
        return s;
    p = JS_ToCStringLen(ctx, &n, s);
    if (!p) {
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    if (strict && !fx_is_token(p, n)) {
        JS_FreeCString(ctx, p);
        fx_throw_msg(ctx, "Headers: invalid header name \"", s, "\"");
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n && (unsigned char)p[i] < 0x80 && !(p[i] >= 'A' && p[i] <= 'Z'); i++)
        ;
    if (i == n) {
        JS_FreeCString(ctx, p);
        return s;
    }
    if (n > sizeof buf || (unsigned char)p[i] >= 0x80) {
        JS_FreeCString(ctx, p);
        r = fx_invoke(ctx, s, "toLowerCase", 0, NULL);
        JS_FreeValue(ctx, s);
        return r;
    }
    for (i = 0; i < n; i++)
        buf[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]);
    JS_FreeCString(ctx, p);
    JS_FreeValue(ctx, s);
    return JS_NewStringLen(ctx, buf, n);
}

static JSValue fx_bytestring(JSContext* ctx, JSValue v, int strict)
{
    JSValue s = fx_str(ctx, v);
    const char* p;
    size_t n, i;
    JS_FreeValue(ctx, v);
    if (JS_IsException(s) || !strict)
        return s;
    p = JS_ToCStringLen(ctx, &n, s);
    if (!p) {
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c >= 0x80) {
            if (c != 0xC2 && c != 0xC3) {
                JS_FreeCString(ctx, p);
                JS_FreeValue(ctx, s);
                return JS_ThrowTypeError(ctx, "Headers: value is not a ByteString (a character above U+00FF)");
            }
            i++;
        }
    }
    JS_FreeCString(ctx, p);
    return s;
}

static int fx_http_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static JSValue fx_hvalue(JSContext* ctx, JSValueConst v, int strict)
{
    JSValue s = fx_str(ctx, v), r, a2[2], len;
    const char* p;
    size_t n, i, a, b;
    if (JS_IsException(s))
        return s;
    p = JS_ToCStringLen(ctx, &n, s);
    if (!p) {
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    a = 0;
    b = n;
    while (a < n && fx_http_ws(p[a]))
        a++;
    while (b > a && fx_http_ws(p[b - 1]))
        b--;
    if (strict) {
        for (i = a; i < b; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c == 0 || c == '\r' || c == '\n' || (c >= 0x80 && c != 0xC2 && c != 0xC3)) {
                JS_FreeCString(ctx, p);
                fx_throw_msg(ctx, "Headers: invalid header value \"", s, "\"");
                JS_FreeValue(ctx, s);
                return JS_EXCEPTION;
            }
            if (c >= 0x80)
                i++;
        }
    }
    JS_FreeCString(ctx, p);
    if (a == 0 && b == n)
        return s;
    len = JS_GetPropertyStr(ctx, s, "length");
    {
        double L = 0;
        if (JS_ToFloat64(ctx, &L, len)) {
            JS_FreeValue(ctx, len);
            JS_FreeValue(ctx, s);
            return JS_EXCEPTION;
        }
        a2[0] = JS_NewInt64(ctx, (int64_t)a);
        a2[1] = JS_NewInt64(ctx, (int64_t)L - (int64_t)(n - b));
    }
    JS_FreeValue(ctx, len);
    r = fx_invoke(ctx, s, "substring", 2, (JSValueConst*)a2);
    JS_FreeValue(ctx, s);
    return r;
}

static JSValue fx_proto_for(JSContext* ctx, JSValueConst new_target, JSClassID cid, const char* name)
{
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Class constructor %s cannot be invoked without 'new'", name);
    (void)cid;
    return JS_GetPropertyStr(ctx, new_target, "prototype");
}

static JSValue fx_make(JSContext* ctx, JSValueConst new_target, JSClassID cid, const char* name, void* opaque)
{
    JSValue proto = JS_IsUndefined(new_target) ? JS_GetClassProto(ctx, cid) : fx_proto_for(ctx, new_target, cid, name), obj;
    if (JS_IsException(proto))
        return proto;
    obj = JS_NewObjectProtoClass(ctx, proto, cid);
    JS_FreeValue(ctx, proto);
    if (!JS_IsException(obj))
        JS_SetOpaque(obj, opaque);
    return obj;
}

static const JSAtom* fx_key_atoms(JSContext* ctx, const char* const* keys, int n);

static int fx_strict_init(JSContext* ctx, JSValueConst init, const char* const* valid, int nvalid)
{
    JSPropertyEnum* tab;
    uint32_t len, i;
    int j, bad = -1;
    if (!JS_IsObject(init) || JS_IsFunction(ctx, init))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, init, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < len && bad < 0; i++) {
        const JSAtom* va = fx_key_atoms(ctx, valid, nvalid);
        int ok = 0;
        if (va) {
            for (j = 0; j < nvalid && !ok; j++)
                ok = tab[i].atom == va[j];
        } else {
            const char* k = JS_AtomToCString(ctx, tab[i].atom);
            if (!k) {
                JS_FreePropertyEnum(ctx, tab, len);
                return -1;
            }
            for (j = 0; j < nvalid; j++)
                ok |= strcmp(k, valid[j]) == 0;
            JS_FreeCString(ctx, k);
        }
        if (!ok) {
            char list[512];
            size_t o = 0;
            JSValue ks = JS_AtomToString(ctx, tab[i].atom);
            o += (size_t)snprintf(list, sizeof list, "\" (valid: ");
            for (j = 0; j < nvalid; j++)
                if (o < sizeof list)
                    o += (size_t)snprintf(list + o, sizeof list - o, "%s%s", j ? ", " : "", valid[j]);
            if (o < sizeof list)
                snprintf(list + o, sizeof list - o, ")");
            if (!JS_IsException(ks))
                fx_throw_msg(ctx, "unknown option \"", ks, list);
            JS_FreeValue(ctx, ks);
            bad = 1;
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return bad > 0 ? -1 : 0;
}

static JSValue fx_settle(JSContext* ctx, JSValue v)
{
    JSValue p = JS_NewPromisePending(ctx);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, v);
        return p;
    }
    if (dyn_settle_value(ctx, p, v) < 0) {
        JS_FreeValue(ctx, p);
        p = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    return p;
}

static JSValue fx_si(JSContext* ctx, int only_loaded)
{
    JSValueConst a = JS_NewBool(ctx, only_loaded);
    return js_fetch_streams_internal(ctx, JS_UNDEFINED, 1, &a);
}

static JSValue fx_si_call(JSContext* ctx, const char* fn, int argc, JSValueConst* argv)
{
    JSValue si = fx_si(ctx, 0), r;
    if (JS_IsException(si))
        return si;
    r = fx_invoke(ctx, si, fn, argc, argv);
    JS_FreeValue(ctx, si);
    return r;
}

static int fx_si_flag(JSContext* ctx, const char* fn, JSValueConst v)
{
    JSValue r = fx_si_call(ctx, fn, 1, &v);
    int b;
    if (JS_IsException(r))
        return -1;
    b = JS_ToBool(ctx, r);
    JS_FreeValue(ctx, r);
    return b;
}

static int fx_is_stream(JSContext* ctx, JSValueConst b)
{
    JSValue si, r;
    int res;
    if (!JS_IsObject(b))
        return 0;
    si = fx_si(ctx, 1);
    if (JS_IsException(si))
        return -1;
    if (JS_IsUndefined(si))
        return 0;
    r = fx_invoke(ctx, si, "rsOf", 1, &b);
    JS_FreeValue(ctx, si);
    if (JS_IsException(r))
        return -1;
    res = !JS_IsUndefined(r);
    JS_FreeValue(ctx, r);
    return res;
}

static JSValue fx_decode_ex(JSContext* ctx, JSValueConst v, int strip_bom)
{
    size_t off, len, asize;
    JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, NULL), r;
    const uint8_t* base;
    if (JS_IsException(ab))
        return ab;
    base = JS_GetArrayBuffer(ctx, &asize, ab);
    if (!base && len) {
        JS_FreeValue(ctx, ab);
        return JS_EXCEPTION;
    }
    r = dyn_utf8_decode_bytes(ctx, base ? base + off : (const uint8_t*)"", len, strip_bom);
    JS_FreeValue(ctx, ab);
    return r;
}

static JSValue fx_decode(JSContext* ctx, JSValueConst v)
{
    return fx_decode_ex(ctx, v, 1);
}

static JSValue fx_decode_nobom(JSContext* ctx, JSValueConst v)
{
    return fx_decode_ex(ctx, v, 0);
}

static JSAtom fx_iter_atom;
static int fx_headers_append(JSContext* ctx, JSValueConst obj, JSValueConst name, JSValueConst value, int set, int strict);
static JSClassID fx_orig_cid;

typedef struct {
    const char* const* keys;
    JSAtom a[24];
    int n;
} FxKeySet;

typedef struct {
    JSValue aproto, values, aiproto, next;
    FxKeySet ks[4];
} FxOrig;

static void fx_orig_fin(JSRuntime* rt, JSValue v)
{
    FxOrig* o = JS_GetOpaque(v, fx_orig_cid);
    if (o) {
        JS_FreeValueRT(rt, o->aproto);
        JS_FreeValueRT(rt, o->values);
        JS_FreeValueRT(rt, o->aiproto);
        JS_FreeValueRT(rt, o->next);
        for (int s = 0; s < 4; s++)
            for (int i = 0; i < o->ks[s].n; i++)
                JS_FreeAtomRT(rt, o->ks[s].a[i]);
        js_free_rt(rt, o);
    }
}

static void fx_orig_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    FxOrig* o = JS_GetOpaque(v, fx_orig_cid);
    if (o) {
        JS_MarkValue(rt, o->aproto, mf);
        JS_MarkValue(rt, o->values, mf);
        JS_MarkValue(rt, o->aiproto, mf);
        JS_MarkValue(rt, o->next, mf);
    }
}

static JSClassDef fx_orig_class = { "FetchIntrinsics", .finalizer = fx_orig_fin, .gc_mark = fx_orig_mark };

static JSAtom fx_sym_iterator(JSContext* ctx)
{
    if (fx_iter_atom == JS_ATOM_NULL) {
        JSValue sy = fx_global(ctx, "Symbol"), key;
        if (JS_IsException(sy))
            return JS_ATOM_NULL;
        key = JS_GetPropertyStr(ctx, sy, "iterator");
        JS_FreeValue(ctx, sy);
        if (JS_IsException(key))
            return JS_ATOM_NULL;
        fx_iter_atom = JS_ValueToAtom(ctx, key);
        JS_FreeValue(ctx, key);
    }
    return fx_iter_atom;
}

typedef struct {
    JSValue k, v;
    uint32_t cnt;
} FxPair;

typedef struct {
    FxPair* p;
    uint32_t n, cap;
} FxPairs;

static int fx_pairs_add(JSContext* ctx, FxPairs* ps, JSValue k, JSValue v, uint32_t cnt)
{
    if (ps->n == ps->cap) {
        uint32_t nc = ps->cap ? ps->cap * 2 : 16;
        FxPair* np = js_realloc(ctx, ps->p, nc * sizeof(*np));
        if (!np) {
            JS_FreeValue(ctx, k);
            JS_FreeValue(ctx, v);
            return -1;
        }
        ps->p = np;
        ps->cap = nc;
    }
    ps->p[ps->n].k = k;
    ps->p[ps->n].v = v;
    ps->p[ps->n].cnt = cnt;
    ps->n++;
    return 0;
}

static void fx_pairs_free(JSContext* ctx, FxPairs* ps)
{
    uint32_t i;
    for (i = 0; i < ps->n; i++) {
        JS_FreeValue(ctx, ps->p[i].k);
        JS_FreeValue(ctx, ps->p[i].v);
    }
    js_free(ctx, ps->p);
}

static void fx_orig_install(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    FxOrig* o;
    JSValue holder, arr, it;
    JSAtom at = fx_sym_iterator(ctx);
    JS_NewClassID(&fx_orig_cid);
    if (!JS_IsRegisteredClass(rt, fx_orig_cid) && JS_NewClass(rt, fx_orig_cid, &fx_orig_class) < 0)
        return;
    o = js_mallocz(ctx, sizeof(*o));
    if (!o || at == JS_ATOM_NULL) {
        js_free(ctx, o);
        return;
    }
    holder = JS_NewObjectClass(ctx, (int)fx_orig_cid);
    if (JS_IsException(holder)) {
        js_free(ctx, o);
        return;
    }
    arr = JS_NewArray(ctx);
    o->aproto = JS_GetPrototype(ctx, arr);
    o->values = JS_GetProperty(ctx, arr, at);
    it = JS_IsException(o->values) ? JS_EXCEPTION : JS_Call(ctx, o->values, arr, 0, NULL);
    o->aiproto = JS_GetPrototype(ctx, it);
    o->next = JS_GetPropertyStr(ctx, o->aiproto, "next");
    JS_FreeValue(ctx, it);
    JS_FreeValue(ctx, arr);
    JS_SetOpaque(holder, o);
    JS_SetClassProto(ctx, fx_orig_cid, holder);
}

static const JSAtom* fx_key_atoms(JSContext* ctx, const char* const* keys, int n)
{
    JSValue holder;
    FxOrig* o;
    int s, i;
    if (!fx_orig_cid || n > 24)
        return NULL;
    holder = JS_GetClassProto(ctx, fx_orig_cid);
    o = JS_IsObject(holder) ? JS_GetOpaque(holder, fx_orig_cid) : NULL;
    JS_FreeValue(ctx, holder);
    if (!o)
        return NULL;
    for (s = 0; s < 4; s++) {
        FxKeySet* k = &o->ks[s];
        if (k->keys == keys && k->n == n)
            return k->a;
        if (!k->keys) {
            for (i = 0; i < n; i++) {
                k->a[i] = JS_NewAtom(ctx, keys[i]);
                if (k->a[i] == JS_ATOM_NULL) {
                    while (--i >= 0)
                        JS_FreeAtom(ctx, k->a[i]);
                    return NULL;
                }
            }
            k->keys = keys;
            k->n = n;
            return k->a;
        }
    }
    return NULL;
}

static int fx_same_obj(JSValueConst a, JSValueConst b)
{
    return JS_IsObject(a) && JS_IsObject(b) && JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

static int fx_own_data_is(JSContext* ctx, JSValueConst obj, JSAtom at, JSValueConst want)
{
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(ctx, &d, obj, at), ok;
    if (has <= 0)
        return 0;
    ok = !(d.flags & JS_PROP_GETSET) && fx_same_obj(d.value, want);
    JS_FreeValue(ctx, d.value);
    JS_FreeValue(ctx, d.getter);
    JS_FreeValue(ctx, d.setter);
    return ok;
}

static int fx_plain_array(JSContext* ctx, const FxOrig* o, JSValueConst v, JSAtom at, JSValue** el, uint32_t* n)
{
    JSValue p;
    JSPropertyDescriptor d;
    int has, same;
    if (!JS_GetFastArray(ctx, v, el, n))
        return 0;
    p = JS_GetPrototype(ctx, v);
    same = fx_same_obj(p, o->aproto);
    JS_FreeValue(ctx, p);
    if (!same)
        return 0;
    has = JS_GetOwnProperty(ctx, &d, v, at);
    if (has > 0) {
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
    }
    return has == 0;
}

static int fx_headers_fast_seq(JSContext* ctx, JSValueConst obj, JSValueConst init)
{
    JSValue holder = fx_orig_cid ? JS_GetClassProto(ctx, fx_orig_cid) : JS_UNDEFINED;
    FxOrig* o = JS_IsObject(holder) ? JS_GetOpaque(holder, fx_orig_cid) : NULL;
    JSAtom at = fx_sym_iterator(ctx), an;
    JSValue* items;
    uint32_t n, i, m;
    int ok = 0;
    if (!o || at == JS_ATOM_NULL)
        goto out;
    an = JS_NewAtom(ctx, "next");
    ok = an != JS_ATOM_NULL && fx_own_data_is(ctx, o->aproto, at, o->values) && fx_own_data_is(ctx, o->aiproto, an, o->next);
    if (an != JS_ATOM_NULL)
        JS_FreeAtom(ctx, an);
    if (!ok || !fx_plain_array(ctx, o, init, at, &items, &n)) {
        ok = 0;
        goto out;
    }
    for (i = 0; i < n && ok; i++) {
        JSValue* kv;
        ok = fx_plain_array(ctx, o, items[i], at, &kv, &m) && m == 2 && JS_IsString(kv[0]) && JS_IsString(kv[1]);
    }
    if (!ok)
        goto out;
    {
        FxPairs ps = { NULL, 0, 0 };
        for (i = 0; i < n && ok > 0; i++) {
            JSValue* kv;
            JSValue k, v;
            JS_GetFastArray(ctx, items[i], &kv, &m);
            k = fx_bytestring(ctx, JS_DupValue(ctx, kv[0]), 1);
            v = JS_IsException(k) ? JS_EXCEPTION : fx_bytestring(ctx, JS_DupValue(ctx, kv[1]), 1);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, k);
                ok = -1;
            } else if (fx_pairs_add(ctx, &ps, k, v, 2) < 0) {
                ok = -1;
            }
        }
        for (i = 0; i < ps.n && ok > 0; i++)
            if (fx_headers_append(ctx, obj, ps.p[i].k, ps.p[i].v, 0, 1) < 0)
                ok = -1;
        fx_pairs_free(ctx, &ps);
    }
out:
    JS_FreeValue(ctx, holder);
    return ok;
}

static int fx_hdr(JSContext* ctx, JSValueConst obj, int create, FxHdr** out)
{
    void* p = JS_GetOpaque2(ctx, obj, fx_headers_cid);
    FxHdr* h;
    if (!p)
        return -1;
    if (p != &fx_empty) {
        *out = p;
        return 0;
    }
    *out = NULL;
    if (!create)
        return 0;
    h = js_mallocz(ctx, sizeof(*h));
    if (!h)
        return -1;
    JS_SetOpaque(obj, h);
    *out = h;
    return 0;
}

static int fx_hdr_writable(JSContext* ctx, FxHdr* h)
{
    if (h && h->guard) {
        JS_ThrowTypeError(ctx, "Headers: this Headers object is immutable");
        return -1;
    }
    return 0;
}

static int32_t fx_hdr_find(FxHdr* h, JSAtom a, uint32_t from)
{
    uint32_t i;
    for (i = from; i < h->n; i++)
        if (h->e[i].name == a)
            return (int32_t)i;
    return -1;
}

static void fx_hdr_drop(JSRuntime* rt, FxHdr* h, uint32_t i)
{
    JS_FreeAtomRT(rt, h->e[i].name);
    JS_FreeValueRT(rt, h->e[i].value);
    memmove(h->e + i, h->e + i + 1, (h->n - i - 1) * sizeof(*h->e));
    h->n--;
}

static JSValue fx_join2(JSContext* ctx, JSValueConst a, JSValueConst b)
{
    size_t la, lb;
    const char *pa, *pb;
    char* buf;
    JSValue r = JS_EXCEPTION;
    pa = JS_ToCStringLen(ctx, &la, a);
    if (!pa)
        return JS_EXCEPTION;
    pb = JS_ToCStringLen(ctx, &lb, b);
    if (!pb) {
        JS_FreeCString(ctx, pa);
        return JS_EXCEPTION;
    }
    buf = js_malloc(ctx, la + lb + 2);
    if (buf) {
        memcpy(buf, pa, la);
        buf[la] = ',';
        buf[la + 1] = ' ';
        memcpy(buf + la + 2, pb, lb);
        r = JS_NewStringLen(ctx, buf, la + lb + 2);
        js_free(ctx, buf);
    }
    JS_FreeCString(ctx, pa);
    JS_FreeCString(ctx, pb);
    return r;
}

static int fx_hdr_push(JSContext* ctx, FxHdr* h, JSAtom a, JSValue v)
{
    if (h->n == h->cap) {
        uint32_t nc = h->cap ? h->cap * 2 : 8;
        FxHE* ne = js_realloc(ctx, h->e, nc * sizeof(*ne));
        if (!ne)
            return -1;
        h->e = ne;
        h->cap = nc;
    }
    h->e[h->n].name = JS_DupAtom(ctx, a);
    h->e[h->n].value = v;
    h->n++;
    return 0;
}

static int fx_atom_is(JSContext* ctx, JSAtom a, const char* s)
{
    JSAtom b = JS_NewAtom(ctx, s);
    int r = b == a;
    JS_FreeAtom(ctx, b);
    return r;
}

static int fx_hdr_put(JSContext* ctx, FxHdr* h, JSValue k, JSValue v, int set)
{
    JSAtom a = JS_ValueToAtom(ctx, k);
    int32_t i;
    int rc = -1;
    JS_FreeValue(ctx, k);
    if (a == JS_ATOM_NULL)
        goto out;
    i = fx_hdr_find(h, a, 0);
    if (i >= 0 && set) {
        int32_t j;
        JS_FreeValue(ctx, h->e[i].value);
        h->e[i].value = v;
        v = JS_UNDEFINED;
        while ((j = fx_hdr_find(h, a, (uint32_t)i + 1)) >= 0)
            fx_hdr_drop(JS_GetRuntime(ctx), h, (uint32_t)j);
        rc = 0;
    } else if (i >= 0 && !fx_atom_is(ctx, a, "set-cookie")) {
        JSValue nv = fx_join2(ctx, h->e[i].value, v);
        if (!JS_IsException(nv)) {
            JS_FreeValue(ctx, h->e[i].value);
            h->e[i].value = nv;
            rc = 0;
        }
    } else if (fx_hdr_push(ctx, h, a, v) == 0) {
        v = JS_UNDEFINED;
        rc = 0;
    }
    h->ver++;
out:
    if (a != JS_ATOM_NULL)
        JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, v);
    return rc;
}

static int fx_headers_append(JSContext* ctx, JSValueConst obj, JSValueConst name, JSValueConst value, int set, int strict)
{
    JSValue k = fx_hname(ctx, name, strict), v;
    FxHdr* h;
    if (JS_IsException(k))
        return -1;
    v = fx_hvalue(ctx, value, strict);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, k);
        return -1;
    }
    if (fx_hdr(ctx, obj, 1, &h) < 0 || fx_hdr_writable(ctx, h) < 0) {
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        return -1;
    }
    return fx_hdr_put(ctx, h, k, v, set);
}

static int fx_hdr_order(JSContext* ctx, FxHdr* h)
{
    uint32_t i, j;
    if (h->ord_ver == h->ver && h->ord_n == h->n)
        return 0;
    if (h->n > h->ord_cap) {
        uint32_t* no = js_realloc(ctx, h->ord, h->n * sizeof(*no));
        if (!no)
            return -1;
        h->ord = no;
        h->ord_cap = h->n;
    }
    {
        const char** ks = js_malloc(ctx, (h->n ? h->n : 1) * sizeof(*ks));
        if (!ks)
            return -1;
        for (i = 0; i < h->n; i++) {
            ks[i] = JS_AtomToCString(ctx, h->e[i].name);
            if (!ks[i]) {
                while (i > 0)
                    JS_FreeCString(ctx, ks[--i]);
                js_free(ctx, ks);
                return -1;
            }
        }
        for (i = 0; i < h->n; i++) {
            for (j = i; j > 0 && strcmp(ks[h->ord[j - 1]], ks[i]) > 0; j--)
                h->ord[j] = h->ord[j - 1];
            h->ord[j] = i;
        }
        for (i = 0; i < h->n; i++)
            JS_FreeCString(ctx, ks[i]);
        js_free(ctx, ks);
    }
    h->ord_n = h->n;
    h->ord_ver = h->ver;
    return 0;
}

static JSValue fx_hdr_values(JSContext* ctx, FxHdr* h, JSAtom a)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t i, k = 0;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < h->n; i++)
        if (h->e[i].name == a && JS_SetPropertyUint32(ctx, arr, k++, JS_DupValue(ctx, h->e[i].value)) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}

static JSValue fx_hdr_get_atom(JSContext* ctx, FxHdr* h, JSAtom a)
{
    int32_t i = fx_hdr_find(h, a, 0), j;
    JSValue r;
    if (i < 0)
        return JS_NULL;
    r = JS_DupValue(ctx, h->e[i].value);
    while ((j = fx_hdr_find(h, a, (uint32_t)i + 1)) >= 0) {
        JSValue nv = fx_join2(ctx, r, h->e[j].value);
        JS_FreeValue(ctx, r);
        if (JS_IsException(nv))
            return nv;
        r = nv;
        i = j;
    }
    return r;
}


static JSValue fx_iter_open(JSContext* ctx, JSValueConst obj, JSValueConst method, JSValue* next)
{
    JSValue it = JS_Call(ctx, method, obj, 0, NULL);
    if (JS_IsException(it))
        return it;
    if (!JS_IsObject(it)) {
        JS_FreeValue(ctx, it);
        return JS_ThrowTypeError(ctx, "Headers: iterator is not an object");
    }
    *next = JS_GetPropertyStr(ctx, it, "next");
    if (JS_IsException(*next)) {
        JS_FreeValue(ctx, it);
        return JS_EXCEPTION;
    }
    return it;
}

static int fx_iter_step(JSContext* ctx, JSValueConst it, JSValueConst next, JSValue* out)
{
    JSValue r = JS_Call(ctx, next, it, 0, NULL), d;
    int done;
    if (JS_IsException(r))
        return -1;
    if (!JS_IsObject(r)) {
        JS_FreeValue(ctx, r);
        JS_ThrowTypeError(ctx, "Headers: iterator result is not an object");
        return -1;
    }
    d = JS_GetPropertyStr(ctx, r, "done");
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, r);
        return -1;
    }
    done = JS_ToBool(ctx, d);
    JS_FreeValue(ctx, d);
    if (done) {
        JS_FreeValue(ctx, r);
        return 0;
    }
    *out = JS_GetPropertyStr(ctx, r, "value");
    JS_FreeValue(ctx, r);
    return JS_IsException(*out) ? -1 : 1;
}

static int fx_iter_method(JSContext* ctx, JSValueConst obj, JSValue* m)
{
    JSAtom at = fx_sym_iterator(ctx);
    if (at == JS_ATOM_NULL)
        return -1;
    *m = JS_GetProperty(ctx, obj, at);
    return JS_IsException(*m) ? -1 : 0;
}

static int fx_hdr_inner(JSContext* ctx, JSValue item, JSValueConst m, int strict, FxPairs* ps)
{
    JSValue next, it, x, kv[2] = { JS_UNDEFINED, JS_UNDEFINED };
    uint32_t cnt = 0;
    int s;
    it = fx_iter_open(ctx, item, m, &next);
    JS_FreeValue(ctx, item);
    if (JS_IsException(it))
        return -1;
    while ((s = fx_iter_step(ctx, it, next, &x)) > 0) {
        JSValue b = fx_bytestring(ctx, x, strict);
        if (JS_IsException(b)) {
            s = -1;
            break;
        }
        if (cnt < 2)
            kv[cnt] = b;
        else
            JS_FreeValue(ctx, b);
        cnt++;
    }
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, it);
    if (s < 0) {
        JS_FreeValue(ctx, kv[0]);
        JS_FreeValue(ctx, kv[1]);
        return -1;
    }
    return fx_pairs_add(ctx, ps, kv[0], kv[1], cnt);
}

static int fx_hdr_copy(JSContext* ctx, FxHdr* d, const FxHdr* s)
{
    uint32_t i;
    for (i = 0; i < s->n; i++)
    {
        JSValue v = JS_DupValue(ctx, s->e[i].value);
        if (fx_hdr_push(ctx, d, s->e[i].name, v) < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
    }
    d->ver++;
    return 0;
}

static int fx_headers_init(JSContext* ctx, JSValueConst obj, JSValueConst init, int strict)
{
    JSValue itf;
    FxPairs ps = { NULL, 0, 0 };
    uint32_t i;
    int rc = 0;
    if (JS_IsUndefined(init))
        return 0;
    if (!JS_IsObject(init)) {
        if (!strict)
            return 0;
        JS_ThrowTypeError(ctx, "Headers: init must be a sequence of pairs or a record");
        return -1;
    }
    if (!strict && JS_GetOpaque(init, fx_headers_cid)) {
        FxHdr *s, *d;
        if (fx_hdr(ctx, init, 0, &s) < 0)
            return -1;
        if (!s || !s->n)
            return 0;
        if (fx_hdr(ctx, obj, 1, &d) < 0)
            return -1;
        return fx_hdr_copy(ctx, d, s);
    }
    if (strict) {
        int f = fx_headers_fast_seq(ctx, obj, init);
        if (f)
            return f < 0 ? -1 : 0;
    }
    if (fx_iter_method(ctx, init, &itf) < 0)
        return -1;
    if (!JS_IsUndefined(itf) && !JS_IsNull(itf)) {
        JSValue next, it, item;
        int s;
        if (!JS_IsFunction(ctx, itf)) {
            JS_FreeValue(ctx, itf);
            JS_ThrowTypeError(ctx, "Headers: init[Symbol.iterator] is not a function");
            return -1;
        }
        it = fx_iter_open(ctx, init, itf, &next);
        JS_FreeValue(ctx, itf);
        if (JS_IsException(it))
            return -1;
        while (rc == 0 && (s = fx_iter_step(ctx, it, next, &item)) != 0) {
            JSValue m;
            if (s < 0) {
                rc = -1;
                break;
            }
            if (!JS_IsObject(item)) {
                JS_FreeValue(ctx, item);
                JS_ThrowTypeError(ctx, "Headers: each header must be a [name, value] pair");
                rc = -1;
                break;
            }
            if (fx_iter_method(ctx, item, &m) < 0) {
                JS_FreeValue(ctx, item);
                rc = -1;
                break;
            }
            if (!JS_IsFunction(ctx, m)) {
                JS_FreeValue(ctx, m);
                JS_FreeValue(ctx, item);
                JS_ThrowTypeError(ctx, "Headers: each header must be a [name, value] pair");
                rc = -1;
                break;
            }
            rc = fx_hdr_inner(ctx, item, m, strict, &ps);
            JS_FreeValue(ctx, m);
        }
        JS_FreeValue(ctx, next);
        JS_FreeValue(ctx, it);
    } else {
        JSPropertyEnum* tab;
        uint32_t len;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, init, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK)) {
            rc = -1;
            goto done;
        }
        for (i = 0; i < len && rc == 0; i++) {
            JSPropertyDescriptor d;
            int has = JS_GetOwnProperty(ctx, &d, init, tab[i].atom), en;
            JSValue k, v;
            if (has < 0) {
                rc = -1;
                break;
            }
            if (!has)
                continue;
            en = (d.flags & JS_PROP_ENUMERABLE) != 0;
            JS_FreeValue(ctx, d.value);
            JS_FreeValue(ctx, d.getter);
            JS_FreeValue(ctx, d.setter);
            if (!en)
                continue;
            k = JS_AtomToValue(ctx, tab[i].atom);
            if (!JS_IsSymbol(k)) {
                k = fx_bytestring(ctx, k, strict);
                if (JS_IsException(k)) {
                    rc = -1;
                    break;
                }
            } else {
                JS_FreeValue(ctx, k);
                if (!strict)
                    continue;
                JS_ThrowTypeError(ctx, "Headers: a Symbol cannot be a header name");
                rc = -1;
                break;
            }
            v = JS_GetProperty(ctx, init, tab[i].atom);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, k);
                rc = -1;
                break;
            }
            if (!strict && JS_IsArray(ctx, v) > 0) {
                uint32_t q, m;
                if (dyn_js_len_u32(ctx, v, &m)) {
                    JS_FreeValue(ctx, k);
                    JS_FreeValue(ctx, v);
                    rc = -1;
                    break;
                }
                for (q = 0; q < m && rc == 0; q++) {
                    JSValue ev = JS_GetPropertyUint32(ctx, v, q);
                    rc = JS_IsException(ev) ? -1 : fx_pairs_add(ctx, &ps, JS_DupValue(ctx, k), ev, 2);
                }
                JS_FreeValue(ctx, k);
                JS_FreeValue(ctx, v);
                continue;
            }
            v = fx_bytestring(ctx, v, strict);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, k);
                rc = -1;
                break;
            }
            rc = fx_pairs_add(ctx, &ps, k, v, 2);
        }
        JS_FreePropertyEnum(ctx, tab, len);
    }
    for (i = 0; i < ps.n && rc == 0; i++) {
        if (ps.p[i].cnt != 2) {
            JS_ThrowTypeError(ctx, "Headers: each header must be a [name, value] pair");
            rc = -1;
            break;
        }
        rc = fx_headers_append(ctx, obj, ps.p[i].k, ps.p[i].v, 0, strict);
    }
done:
    fx_pairs_free(ctx, &ps);
    return rc;
}

static JSValue fx_headers_make(JSContext* ctx, JSValueConst new_target, JSValueConst init, int strict, int guard)
{
    JSValue obj = fx_make(ctx, new_target, fx_headers_cid, "Headers", &fx_empty);
    FxHdr* h;
    if (JS_IsException(obj))
        return obj;
    if (fx_headers_init(ctx, obj, init, strict) < 0)
        goto fail;
    if (guard) {
        if (fx_hdr(ctx, obj, 1, &h) < 0)
            goto fail;
        h->guard = guard;
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue fx_headers_new(JSContext* ctx, JSValueConst new_target, JSValueConst init)
{
    return fx_headers_make(ctx, new_target, init, 1, 0);
}

static JSValue fx_headers_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Class constructor Headers cannot be invoked without 'new'");
    return fx_headers_new(ctx, new_target, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue fx_headers_method(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FxHdr* h;
    JSValue k, r = JS_UNDEFINED;
    JSAtom a;
    if (fx_hdr(ctx, this_val, 0, &h) < 0)
        return JS_EXCEPTION;
    if (magic >= 3) {
        if (fx_headers_append(ctx, this_val, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, magic == 4, 1) < 0)
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    k = fx_hname(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 1);
    if (JS_IsException(k))
        return k;
    if (magic == 0 && fx_hdr_writable(ctx, h) < 0) {
        JS_FreeValue(ctx, k);
        return JS_EXCEPTION;
    }
    if (!h) {
        JS_FreeValue(ctx, k);
        return magic == 1 ? JS_NULL : magic == 2 ? JS_FALSE : JS_UNDEFINED;
    }
    a = JS_ValueToAtom(ctx, k);
    JS_FreeValue(ctx, k);
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    if (magic == 0) {
        int32_t i;
        while ((i = fx_hdr_find(h, a, 0)) >= 0)
            fx_hdr_drop(JS_GetRuntime(ctx), h, (uint32_t)i);
        h->ver++;
    } else if (magic == 1) {
        r = fx_hdr_get_atom(ctx, h, a);
    } else {
        r = JS_NewBool(ctx, fx_hdr_find(h, a, 0) >= 0);
    }
    JS_FreeAtom(ctx, a);
    return r;
}

static JSValue fx_headers_set_cookie(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FxHdr* h;
    JSAtom a;
    JSValue r;
    (void)argc;
    (void)argv;
    if (fx_hdr(ctx, this_val, 0, &h) < 0)
        return JS_EXCEPTION;
    if (!h)
        return JS_NewArray(ctx);
    a = JS_NewAtom(ctx, "set-cookie");
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = fx_hdr_values(ctx, h, a);
    JS_FreeAtom(ctx, a);
    return r;
}

typedef struct {
    JSValue target;
    uint32_t index;
    int kind;
} FxIt;

static JSValue fx_iter_make(JSContext* ctx, JSValueConst target, int kind, int form)
{
    JSClassID cid = form ? fx_fiter_cid : fx_hiter_cid;
    FxIt* it = js_mallocz(ctx, sizeof(*it));
    JSValue obj;
    if (!it)
        return JS_EXCEPTION;
    obj = JS_NewObjectClass(ctx, (int)cid);
    if (JS_IsException(obj)) {
        js_free(ctx, it);
        return obj;
    }
    it->target = JS_DupValue(ctx, target);
    it->kind = kind;
    JS_SetOpaque(obj, it);
    return obj;
}

static int fx_pair_at(JSContext* ctx, JSValueConst target, int form, uint32_t i, JSValue* k, JSValue* v)
{
    if (form) {
        JSValue pairs = JS_DupValue(ctx, fx_box(JS_GetOpaque(target, fx_form_cid))), e;
        if (JS_IsUndefined(pairs) || i >= fx_len(ctx, pairs)) {
            JS_FreeValue(ctx, pairs);
            return 0;
        }
        e = JS_GetPropertyUint32(ctx, pairs, i);
        JS_FreeValue(ctx, pairs);
        *k = JS_GetPropertyUint32(ctx, e, 0);
        *v = JS_GetPropertyUint32(ctx, e, 1);
        JS_FreeValue(ctx, e);
        return 1;
    } else {
        FxHdr* h;
        const FxHE* e;
        if (fx_hdr(ctx, target, 0, &h) < 0)
            return -1;
        if (!h || i >= h->n)
            return 0;
        if (fx_hdr_order(ctx, h) < 0)
            return -1;
        e = &h->e[h->ord[i]];
        *k = JS_AtomToString(ctx, e->name);
        *v = JS_DupValue(ctx, e->value);
        return 1;
    }
}

static JSValue fx_iter_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int* pdone, int magic)
{
    FxIt* it = JS_GetOpaque2(ctx, this_val, magic ? fx_fiter_cid : fx_hiter_cid);
    JSValue k, v;
    int s;
    (void)argc;
    (void)argv;
    *pdone = 0;
    if (!it)
        return JS_EXCEPTION;
    s = fx_pair_at(ctx, it->target, magic, it->index, &k, &v);
    if (s < 0)
        return JS_EXCEPTION;
    if (!s) {
        *pdone = 1;
        return JS_UNDEFINED;
    }
    it->index++;
    if (it->kind == 2) {
        JSValue pair = JS_NewArray(ctx);
        if (JS_IsException(pair)) {
            JS_FreeValue(ctx, k);
            JS_FreeValue(ctx, v);
            return pair;
        }
        JS_SetPropertyUint32(ctx, pair, 0, k);
        JS_SetPropertyUint32(ctx, pair, 1, v);
        return pair;
    }
    JS_FreeValue(ctx, it->kind ? k : v);
    return it->kind ? v : k;
}

static JSValue fx_headers_iter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FxHdr* h;
    (void)argc;
    (void)argv;
    if (fx_hdr(ctx, this_val, 0, &h) < 0)
        return JS_EXCEPTION;
    return fx_iter_make(ctx, this_val, magic, 0);
}

static JSValue fx_pairs_foreach(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int form)
{
    JSValueConst cb = argc > 0 ? argv[0] : JS_UNDEFINED, self = argc > 1 ? argv[1] : JS_UNDEFINED;
    uint32_t i;
    if (!JS_IsFunction(ctx, cb))
        return JS_ThrowTypeError(ctx, "forEach: callback is not a function");
    for (i = 0;; i++) {
        JSValue a[3], r;
        int s = fx_pair_at(ctx, this_val, form, i, &a[1], &a[0]);
        if (s < 0)
            return JS_EXCEPTION;
        if (!s)
            break;
        a[2] = JS_DupValue(ctx, this_val);
        r = JS_Call(ctx, cb, self, 3, (JSValueConst*)a);
        JS_FreeValue(ctx, a[0]);
        JS_FreeValue(ctx, a[1]);
        JS_FreeValue(ctx, a[2]);
        if (JS_IsException(r))
            return r;
        JS_FreeValue(ctx, r);
    }
    return JS_UNDEFINED;
}

static JSValue fx_headers_foreach(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FxHdr* h;
    if (fx_hdr(ctx, this_val, 0, &h) < 0)
        return JS_EXCEPTION;
    return fx_pairs_foreach(ctx, this_val, argc, argv, 0);
}

static FxForm* fx_form(JSContext* ctx, JSValueConst v, FxForm* f)
{
    void* p = JS_GetOpaque2(ctx, v, fx_form_cid);
    if (!p)
        return NULL;
    f->self = v;
    f->list = fx_box(p);
    return f;
}

static JSValue fx_form_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValue list, obj;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Class constructor FormData cannot be invoked without 'new'");
    if (argc > 0 && !JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "FormData: the form argument must be an HTMLFormElement, which this runtime does not have");
    list = JS_NewArray(ctx);
    if (JS_IsException(list))
        return list;
    obj = fx_make(ctx, new_target, fx_form_cid, "FormData", fx_unbox(list));
    if (JS_IsException(obj))
        JS_FreeValue(ctx, list);
    return obj;
}

static uint32_t fx_len(JSContext* ctx, JSValueConst arr)
{
    JSValue l = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = !JS_IsException(l) && JS_IsNumber(l) ? (uint32_t)dyn_num_value(l) : 0;
    JS_FreeValue(ctx, l);
    return n;
}

static JSValue fx_usv(JSContext* ctx, JSValueConst v)
{
    JSValue s = fx_str(ctx, v), r;
    if (JS_IsException(s))
        return s;
    r = fx_invoke(ctx, s, "toWellFormed", 0, NULL);
    JS_FreeValue(ctx, s);
    return r;
}

static JSValue fx_form_entry(JSContext* ctx, JSValueConst name, int argc, JSValueConst* argv)
{
    JSValueConst value = argc > 1 ? argv[1] : JS_UNDEFINED;
    int has_fn = argc > 2 && !JS_IsUndefined(argv[2]);
    JSValue v, fn = JS_UNDEFINED, t;
    if (js_blob_is(value)) {
        v = js_blob_form_entry(ctx, value, has_fn ? argv[2] : JS_UNDEFINED, has_fn);
        if (JS_IsException(v))
            return v;
    } else if (JS_GetBufferKind(value) != JS_BUFFER_KIND_NONE) {
        v = JS_DupValue(ctx, value);
        if (has_fn) {
            fn = fx_usv(ctx, argv[2]);
            if (JS_IsException(fn)) {
                JS_FreeValue(ctx, v);
                return fn;
            }
        }
    } else if (has_fn) {
        return JS_ThrowTypeError(ctx, "FormData: a filename is only allowed with a Blob, File or byte-buffer value");
    } else {
        v = fx_usv(ctx, value);
        if (JS_IsException(v))
            return v;
    }
    t = JS_NewArray(ctx);
    if (JS_IsException(t)) {
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, fn);
        return t;
    }
    JS_SetPropertyUint32(ctx, t, 0, JS_DupValue(ctx, name));
    JS_SetPropertyUint32(ctx, t, 1, v);
    JS_SetPropertyUint32(ctx, t, 2, fn);
    return t;
}

static JSValue fx_form_method(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FxForm fv, *f = fx_form(ctx, this_val, &fv);
    JSValue name, out = JS_UNDEFINED, keep, ent = JS_UNDEFINED;
    uint32_t n, i;
    int replaced = 0;
    if (!f)
        return JS_EXCEPTION;
    if ((magic == 0 || magic == 5) && argc < 2)
        return JS_ThrowTypeError(ctx, "FormData: 2 arguments required");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "FormData: 1 argument required");
    name = fx_usv(ctx, argv[0]);
    if (JS_IsException(name))
        return name;
    if (magic == 0 || magic == 5) {
        ent = fx_form_entry(ctx, name, argc, argv);
        if (JS_IsException(ent)) {
            JS_FreeValue(ctx, name);
            return ent;
        }
    }
    if (magic == 0) {
        JS_SetPropertyUint32(ctx, f->list, fx_len(ctx, f->list), ent);
        JS_FreeValue(ctx, name);
        return JS_UNDEFINED;
    }
    n = fx_len(ctx, f->list);
    keep = JS_NewArray(ctx);
    if (magic == 3)
        out = JS_NewArray(ctx);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, f->list, i), k;
        int same;
        if (JS_IsException(e))
            goto fm_fail;
        k = JS_GetPropertyUint32(ctx, e, 0);
        if (JS_IsException(k)) {
            JS_FreeValue(ctx, e);
            goto fm_fail;
        }
        same = JS_StrictEq(ctx, k, name);
        JS_FreeValue(ctx, k);
        if (magic == 1) {
            if (!same)
                JS_SetPropertyUint32(ctx, keep, fx_len(ctx, keep), JS_DupValue(ctx, e));
        } else if (magic == 2 || magic == 4) {
            if (same && JS_IsUndefined(out)) {
                out = magic == 2 ? JS_GetPropertyUint32(ctx, e, 1) : JS_TRUE;
                JS_FreeValue(ctx, e);
                if (JS_IsException(out))
                    goto fm_fail;
                break;
            }
        } else if (magic == 3) {
            if (same) {
                JSValue ev = JS_GetPropertyUint32(ctx, e, 1);
                if (JS_IsException(ev)) {
                    JS_FreeValue(ctx, e);
                    goto fm_fail;
                }
                JS_SetPropertyUint32(ctx, out, fx_len(ctx, out), ev);
            }
        } else if (magic == 5) {
            if (same) {
                if (!replaced) {
                    replaced = 1;
                    JS_SetPropertyUint32(ctx, keep, fx_len(ctx, keep), JS_DupValue(ctx, ent));
                }
            } else {
                JS_SetPropertyUint32(ctx, keep, fx_len(ctx, keep), JS_DupValue(ctx, e));
            }
        }
        JS_FreeValue(ctx, e);
    }
    if (magic == 5 && !replaced)
        JS_SetPropertyUint32(ctx, keep, fx_len(ctx, keep), JS_DupValue(ctx, ent));
    JS_FreeValue(ctx, ent);
    JS_FreeValue(ctx, name);
    if (magic == 1 || magic == 5) {
        JS_SetOpaque(f->self, fx_unbox(keep));
        JS_FreeValue(ctx, f->list);
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, keep);
    if (magic == 2)
        return JS_IsUndefined(out) ? JS_NULL : out;
    if (magic == 4)
        return JS_IsUndefined(out) ? JS_FALSE : out;
    return out;
fm_fail:
    JS_FreeValue(ctx, keep);
    JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, ent);
    JS_FreeValue(ctx, name);
    return JS_EXCEPTION;
}

static JSValue fx_form_iter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FxForm fv;
    (void)argc;
    (void)argv;
    if (!fx_form(ctx, this_val, &fv))
        return JS_EXCEPTION;
    return fx_iter_make(ctx, this_val, magic, 1);
}

static JSValue fx_form_foreach(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FxForm fv;
    if (!fx_form(ctx, this_val, &fv))
        return JS_EXCEPTION;
    return fx_pairs_foreach(ctx, this_val, argc, argv, 1);
}

static FxSignal* fx_signal(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, fx_signal_cid);
}

static JSValue fx_signal_new(JSContext* ctx, JSValueConst new_target)
{
    FxSignal* s = js_mallocz(ctx, sizeof(*s));
    JSValue obj;
    if (!s)
        return JS_EXCEPTION;
    s->reason = JS_UNDEFINED;
    s->onabort = JS_NULL;
    s->timer = JS_UNDEFINED;
    obj = fx_make(ctx, new_target, fx_signal_cid, "AbortSignal", s);
    if (JS_IsException(obj))
        js_free(ctx, s);
    return obj;
}

static JSValue fx_signal_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor: use new AbortController().signal, AbortSignal.abort(), AbortSignal.timeout() or AbortSignal.any()");
}

static int fx_sig_push(JSContext* ctx, FxSignal* s, JSValue v, uint8_t flags)
{
    if (s->n == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 2;
        JSValue* lv = js_realloc(ctx, s->lv, nc * sizeof(*lv));
        uint8_t* lf;
        if (!lv) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        s->lv = lv;
        lf = js_realloc(ctx, s->lf, nc);
        if (!lf) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        s->lf = lf;
        s->cap = nc;
    }
    s->lv[s->n] = v;
    s->lf[s->n++] = flags;
    return 0;
}

static int fx_sig_dep(JSContext* ctx, FxSignal* s, JSValueConst dep)
{
    if (s->nd == s->capd) {
        uint32_t nc = s->capd ? s->capd * 2 : 2;
        JSValue* d = js_realloc(ctx, s->deps, nc * sizeof(*d));
        if (!d)
            return -1;
        s->deps = d;
        s->capd = nc;
    }
    s->deps[s->nd++] = JS_DupValue(ctx, dep);
    return 0;
}

static void fx_sig_remove(JSContext* ctx, FxSignal* s, uint32_t i)
{
    JS_FreeValue(ctx, s->lv[i]);
    memmove(s->lv + i, s->lv + i + 1, (s->n - i - 1) * sizeof(*s->lv));
    memmove(s->lf + i, s->lf + i + 1, s->n - i - 1);
    s->n--;
}

enum { FXL_ONCE = 1, FXL_CAPTURE = 2, FXL_HANDLER = 4 };

static JSValue fx_signal_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    FxSignal* s = fx_signal(ctx, this_val);
    if (!s)
        return JS_EXCEPTION;
    if (magic == 2)
        return JS_DupValue(ctx, s->onabort);
    return magic == 0 ? JS_NewBool(ctx, s->aborted) : JS_DupValue(ctx, s->reason);
}

static JSValue fx_signal_set_onabort(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    FxSignal* s = fx_signal(ctx, this_val);
    uint32_t i;
    (void)magic;
    if (!s)
        return JS_EXCEPTION;
    JS_FreeValue(ctx, s->onabort);
    s->onabort = JS_IsObject(v) ? JS_DupValue(ctx, v) : JS_NULL;
    for (i = 0; i < s->n; i++)
        if (s->lf[i] & FXL_HANDLER)
            break;
    if (JS_IsNull(s->onabort)) {
        if (i < s->n)
            fx_sig_remove(ctx, s, i);
    } else if (i == s->n && fx_sig_push(ctx, s, JS_NULL, FXL_HANDLER) < 0) {
        return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static JSValue fx_abort_event(JSContext* ctx, JSValueConst target)
{
    JSValue evt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, evt, "type", fx_lit(ctx, "abort"));
    JS_SetPropertyStr(ctx, evt, "target", JS_DupValue(ctx, target));
    JS_SetPropertyStr(ctx, evt, "currentTarget", JS_DupValue(ctx, target));
    return evt;
}

static JSValue fx_rethrow(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    return JS_Throw(ctx, JS_DupValue(ctx, data[0]));
}

static void fx_microtask(JSContext* ctx, JSValue fn)
{
    JSValue q = fx_global(ctx, "queueMicrotask"), r;
    r = JS_Call(ctx, q, JS_UNDEFINED, 1, (JSValueConst*)&fn);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, q);
    JS_FreeValue(ctx, fn);
}

static void fx_report_later(JSContext* ctx)
{
    JSValue e;
    if (JS_HasUncatchableException(ctx))
        return;
    e = JS_GetException(ctx);
    fx_microtask(ctx, JS_NewCFunctionData(ctx, fx_rethrow, 0, 0, 1, &e));
    JS_FreeValue(ctx, e);
}

static int fx_listen_opts(JSContext* ctx, JSValueConst o, int add, uint8_t* flags, JSValue* sig)
{
    JSValue v;
    *flags = 0;
    *sig = JS_UNDEFINED;
    if (!JS_IsObject(o)) {
        if (JS_ToBool(ctx, o))
            *flags |= FXL_CAPTURE;
        return 0;
    }
    v = JS_GetPropertyStr(ctx, o, "capture");
    if (JS_IsException(v))
        return -1;
    if (JS_ToBool(ctx, v))
        *flags |= FXL_CAPTURE;
    JS_FreeValue(ctx, v);
    if (!add)
        return 0;
    v = JS_GetPropertyStr(ctx, o, "once");
    if (JS_IsException(v))
        return -1;
    if (JS_ToBool(ctx, v))
        *flags |= FXL_ONCE;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "passive");
    if (JS_IsException(v))
        return -1;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "signal");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !(JS_IsObject(v) && JS_GetOpaque(v, fx_signal_cid))) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "addEventListener: options.signal must be an AbortSignal");
        return -1;
    }
    *sig = v;
    return 0;
}

static int fx_listen_find(JSContext* ctx, FxSignal* s, JSValueConst cb, uint8_t capture)
{
    uint32_t i;
    for (i = 0; i < s->n; i++)
        if (!(s->lf[i] & FXL_HANDLER) && (s->lf[i] & FXL_CAPTURE) == capture && JS_StrictEq(ctx, s->lv[i], cb))
            return (int)i;
    return -1;
}

static JSValue fx_unlisten(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    FxSignal* s = JS_GetOpaque(data[0], fx_signal_cid);
    int i;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (s && (i = fx_listen_find(ctx, s, data[1], (uint8_t)magic)) >= 0)
        fx_sig_remove(ctx, s, (uint32_t)i);
    return JS_UNDEFINED;
}

static JSValue fx_signal_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FxSignal* s = fx_signal(ctx, this_val);
    JSValueConst fn = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue sig;
    uint8_t flags;
    int is_abort;
    if (!s)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%s: 2 arguments required", magic ? "removeEventListener" : "addEventListener");
    if (!JS_IsNull(fn) && !JS_IsUndefined(fn) && !JS_IsObject(fn))
        return JS_ThrowTypeError(ctx, "%s: the listener must be an object or null", magic ? "removeEventListener" : "addEventListener");
    {
        JSValue t = fx_str(ctx, argv[0]);
        if (JS_IsException(t))
            return t;
        is_abort = fx_is_str(ctx, t, "abort");
        JS_FreeValue(ctx, t);
    }
    if (fx_listen_opts(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, !magic, &flags, &sig) < 0)
        return JS_EXCEPTION;
    if (is_abort <= 0 || !JS_IsObject(fn)) {
        JS_FreeValue(ctx, sig);
        return is_abort < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    if (magic) {
        int i = fx_listen_find(ctx, s, fn, flags & FXL_CAPTURE);
        if (i >= 0)
            fx_sig_remove(ctx, s, (uint32_t)i);
        return JS_UNDEFINED;
    }
    if (!JS_IsUndefined(sig)) {
        FxSignal* o = JS_GetOpaque(sig, fx_signal_cid);
        if (o->aborted) {
            JS_FreeValue(ctx, sig);
            return JS_UNDEFINED;
        }
    }
    if (fx_listen_find(ctx, s, fn, flags & FXL_CAPTURE) >= 0) {
        JS_FreeValue(ctx, sig);
        return JS_UNDEFINED;
    }
    if (fx_sig_push(ctx, s, JS_DupValue(ctx, fn), flags) < 0) {
        JS_FreeValue(ctx, sig);
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(sig)) {
        JSValue d[2], un, a[2], r;
        d[0] = JS_DupValue(ctx, this_val);
        d[1] = JS_DupValue(ctx, fn);
        un = JS_NewCFunctionData(ctx, fx_unlisten, 0, flags & FXL_CAPTURE, 2, (JSValueConst*)d);
        JS_FreeValue(ctx, d[0]);
        JS_FreeValue(ctx, d[1]);
        a[0] = fx_lit(ctx, "abort");
        a[1] = un;
        r = fx_signal_listen(ctx, sig, 2, (JSValueConst*)a, 0);
        JS_FreeValue(ctx, a[0]);
        JS_FreeValue(ctx, un);
        JS_FreeValue(ctx, sig);
        if (JS_IsException(r))
            return r;
    }
    return JS_UNDEFINED;
}

static JSValue fx_signal_throw_if(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FxSignal* s = fx_signal(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (s->aborted)
        return JS_Throw(ctx, JS_DupValue(ctx, s->reason));
    return JS_UNDEFINED;
}

static void fx_signal_fire(JSContext* ctx, JSValueConst sig, FxSignal* s)
{
    JSValue evt = fx_abort_event(ctx, sig), *snap;
    uint32_t n = s->n, i;
    snap = n ? js_malloc(ctx, n * sizeof(*snap)) : NULL;
    if (n && !snap) {
        JS_FreeValue(ctx, evt);
        return;
    }
    for (i = 0; i < n; i++)
        snap[i] = (s->lf[i] & FXL_HANDLER) ? JS_NULL : JS_DupValue(ctx, s->lv[i]);
    for (i = 0; i < n; i++) {
        JSValue cb, r, thisv;
        int k = -1;
        uint32_t j;
        if (JS_HasUncatchableException(ctx))
            break;
        for (j = 0; j < s->n; j++) {
            if (JS_IsNull(snap[i]) ? (s->lf[j] & FXL_HANDLER) != 0 : (!(s->lf[j] & FXL_HANDLER) && JS_StrictEq(ctx, s->lv[j], snap[i]))) {
                k = (int)j;
                break;
            }
        }
        if (k < 0)
            continue;
        if (JS_IsNull(snap[i])) {
            cb = JS_DupValue(ctx, s->onabort);
            thisv = JS_DupValue(ctx, sig);
        } else {
            if (s->lf[k] & FXL_ONCE)
                fx_sig_remove(ctx, s, (uint32_t)k);
            if (JS_IsFunction(ctx, snap[i])) {
                cb = JS_DupValue(ctx, snap[i]);
                thisv = JS_DupValue(ctx, sig);
            } else {
                cb = JS_GetPropertyStr(ctx, snap[i], "handleEvent");
                thisv = JS_DupValue(ctx, snap[i]);
                if (JS_IsException(cb)) {
                    fx_report_later(ctx);
                    JS_FreeValue(ctx, thisv);
                    continue;
                }
                if (!JS_IsFunction(ctx, cb)) {
                    JS_FreeValue(ctx, cb);
                    JS_FreeValue(ctx, thisv);
                    JS_ThrowTypeError(ctx, "abort listener: handleEvent is not a function");
                    fx_report_later(ctx);
                    continue;
                }
            }
        }
        if (JS_IsFunction(ctx, cb)) {
            r = JS_Call(ctx, cb, thisv, 1, (JSValueConst*)&evt);
            if (JS_IsException(r))
                fx_report_later(ctx);
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, cb);
        JS_FreeValue(ctx, thisv);
    }
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, snap[i]);
    js_free(ctx, snap);
    JS_FreeValue(ctx, evt);
}

static int fx_signal_abort(JSContext* ctx, JSValueConst sig, JSValueConst reason)
{
    FxSignal* s = fx_signal(ctx, sig);
    JSValue* deps;
    uint32_t nd, i;
    if (!s)
        return -1;
    if (s->aborted)
        return 0;
    if (JS_IsUndefined(reason)) {
        s->reason = js_dom_exception(ctx, "AbortError", "signal is aborted without reason");
        if (JS_IsException(s->reason)) {
            s->reason = JS_UNDEFINED;
            return -1;
        }
    } else {
        s->reason = JS_DupValue(ctx, reason);
    }
    s->aborted = 1;
    deps = s->deps;
    nd = s->nd;
    s->deps = NULL;
    s->nd = s->capd = 0;
    for (i = 0; i < nd; i++) {
        FxSignal* d = JS_GetOpaque(deps[i], fx_signal_cid);
        if (d->aborted) {
            JS_FreeValue(ctx, deps[i]);
            deps[i] = JS_UNDEFINED;
        } else {
            d->aborted = 1;
            d->reason = JS_DupValue(ctx, s->reason);
        }
    }
    fx_signal_fire(ctx, sig, s);
    for (i = 0; i < nd; i++) {
        if (!JS_IsUndefined(deps[i])) {
            fx_signal_fire(ctx, deps[i], JS_GetOpaque(deps[i], fx_signal_cid));
            JS_FreeValue(ctx, deps[i]);
        }
    }
    js_free(ctx, deps);
    return JS_HasUncatchableException(ctx) ? -1 : 0;
}

static JSValue fx_signal_s_abort(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue s = fx_signal_new(ctx, JS_UNDEFINED);
    (void)this_val;
    if (JS_IsException(s))
        return s;
    if (fx_signal_abort(ctx, s, argc > 0 ? argv[0] : JS_UNDEFINED) < 0) {
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    return s;
}

static JSValue fx_signal_any_of(JSContext* ctx, int n, JSValueConst* srcs)
{
    JSValue sig = fx_signal_new(ctx, JS_UNDEFINED);
    int i;
    if (JS_IsException(sig))
        return sig;
    for (i = 0; i < n; i++) {
        FxSignal* s = JS_GetOpaque(srcs[i], fx_signal_cid);
        if (s->aborted) {
            FxSignal* d = JS_GetOpaque(sig, fx_signal_cid);
            d->aborted = 1;
            d->reason = JS_DupValue(ctx, s->reason);
            return sig;
        }
    }
    for (i = 0; i < n; i++) {
        if (fx_sig_dep(ctx, JS_GetOpaque(srcs[i], fx_signal_cid), sig) < 0) {
            JS_FreeValue(ctx, sig);
            return JS_EXCEPTION;
        }
    }
    return sig;
}

static JSValue fx_signal_s_any(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue arr, a, r;
    uint32_t n, i;
    JSValue* v;
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "AbortSignal.any: argument must be an iterable of AbortSignal");
    a = fx_global(ctx, "Array");
    arr = fx_invoke(ctx, a, "from", 1, argv);
    JS_FreeValue(ctx, a);
    if (JS_IsException(arr))
        return arr;
    if (dyn_js_len_u32(ctx, arr, &n)) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    v = js_mallocz(ctx, (n ? n : 1) * sizeof(*v));
    if (!v) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        v[i] = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(v[i])) {
            n = i + 1;
            r = JS_EXCEPTION;
            goto done;
        }
        if (!(JS_IsObject(v[i]) && JS_GetOpaque(v[i], fx_signal_cid))) {
            n = i + 1;
            r = JS_ThrowTypeError(ctx, "AbortSignal.any: every element must be an AbortSignal");
            goto done;
        }
    }
    r = fx_signal_any_of(ctx, (int)n, (JSValueConst*)v);
done:
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, v[i]);
    js_free(ctx, v);
    JS_FreeValue(ctx, arr);
    return r;
}

static JSValue fx_ctrl_new(JSContext* ctx, JSValueConst new_target)
{
    JSValue sig = fx_signal_new(ctx, JS_UNDEFINED), obj;
    if (JS_IsException(sig))
        return sig;
    obj = fx_make(ctx, new_target, fx_ctrl_cid, "AbortController", fx_unbox(sig));
    if (JS_IsException(obj))
        JS_FreeValue(ctx, sig);
    return obj;
}

static JSValue fx_ctrl_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Class constructor AbortController cannot be invoked without 'new'");
    return fx_ctrl_new(ctx, new_target);
}

static JSValue fx_ctrl_signal(JSContext* ctx, JSValueConst this_val)
{
    void* c = JS_GetOpaque2(ctx, this_val, fx_ctrl_cid);
    return c ? JS_DupValue(ctx, fx_box(c)) : JS_EXCEPTION;
}

static JSValue fx_ctrl_abort(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    void* c = JS_GetOpaque2(ctx, this_val, fx_ctrl_cid);
    if (!c)
        return JS_EXCEPTION;
    if (fx_signal_abort(ctx, fx_box(c), argc > 0 ? argv[0] : JS_UNDEFINED) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue fx_timeout_fire(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    FxSignal* s = JS_GetOpaque(data[0], fx_signal_cid);
    JSValue e;
    int r;
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)magic;
    if (!s)
        return JS_UNDEFINED;
    JS_FreeValue(ctx, s->timer);
    s->timer = JS_UNDEFINED;
    e = js_dom_exception(ctx, "TimeoutError", "signal timed out");
    if (JS_IsException(e))
        return e;
    r = fx_signal_abort(ctx, data[0], e);
    JS_FreeValue(ctx, e);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static void fx_signal_untime(JSContext* ctx, FxSignal* s)
{
    if (!JS_IsUndefined(s->timer)) {
        JSValue m = fx_global(ctx, "clearTimeout"), r;
        r = JS_Call(ctx, m, JS_UNDEFINED, 1, (JSValueConst*)&s->timer);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, m);
        JS_FreeValue(ctx, s->timer);
        s->timer = JS_UNDEFINED;
    }
}

static JSValue fx_signal_s_timeout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue sig, st, fire, a[2];
    FxSignal* s;
    double ms;
    (void)this_val;
    if (argc < 1 || JS_ToFloat64(ctx, &ms, argv[0]))
        return argc < 1 ? JS_ThrowTypeError(ctx, "AbortSignal.timeout: 1 argument required") : JS_EXCEPTION;
    if (!isfinite(ms) || ms < 0 || ms > 9007199254740991.0)
        return JS_ThrowTypeError(ctx, "AbortSignal.timeout: milliseconds must be a finite non-negative integer");
    sig = fx_signal_new(ctx, JS_UNDEFINED);
    if (JS_IsException(sig))
        return sig;
    s = JS_GetOpaque(sig, fx_signal_cid);
    fire = JS_NewCFunctionData(ctx, fx_timeout_fire, 0, 0, 1, (JSValueConst*)&sig);
    st = fx_global(ctx, "setTimeout");
    a[0] = fire;
    a[1] = JS_NewFloat64(ctx, trunc(ms));
    s->timer = JS_Call(ctx, st, JS_UNDEFINED, 2, (JSValueConst*)a);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, fire);
    if (JS_IsException(s->timer)) {
        s->timer = JS_UNDEFINED;
        JS_FreeValue(ctx, sig);
        return JS_EXCEPTION;
    }
    return sig;
}

static JSValue fx_h_untime(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FxSignal* s = argc > 0 && JS_IsObject(argv[0]) ? JS_GetOpaque(argv[0], fx_signal_cid) : NULL;
    (void)this_val;
    if (s)
        fx_signal_untime(ctx, s);
    return JS_UNDEFINED;
}

static void fx_body_init(FxBody* b)
{
    b->src = JS_NULL;
    b->stream = JS_NULL;
    b->used = 0;
    b->isnull = 1;
}

static void fx_body_free(JSRuntime* rt, FxBody* b)
{
    JS_FreeValueRT(rt, b->src);
    JS_FreeValueRT(rt, b->stream);
}

static void fx_body_mark(JSRuntime* rt, FxBody* b, JS_MarkFunc* mf)
{
    JS_MarkValue(rt, b->src, mf);
    JS_MarkValue(rt, b->stream, mf);
}

static JSValue fx_u8_view(JSContext* ctx, JSValue ab)
{
    JSValue a[3], r;
    if (JS_IsException(ab))
        return ab;
    a[0] = ab;
    a[1] = JS_UNDEFINED;
    a[2] = JS_UNDEFINED;
    r = JS_NewTypedArray(ctx, 3, a, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return r;
}

static JSValue fx_bytes_new(JSContext* ctx, const uint8_t* p, size_t n)
{
    return fx_u8_view(ctx, JS_NewArrayBufferCopy(ctx, p ? p : (const uint8_t*)"", n));
}

static int fx_buffer_copy(JSContext* ctx, JSValueConst v, JSValue* out)
{
    int k = JS_GetBufferKind(v);
    size_t n = 0, off = 0, len = 0;
    uint8_t* p;
    JSValue buf;
    if (k == JS_BUFFER_KIND_NONE)
        return 0;
    if (k == JS_BUFFER_KIND_BUFFER) {
        p = JS_PeekArrayBuffer(v, &n);
        *out = fx_bytes_new(ctx, p, p ? n : 0);
        return JS_IsException(*out) ? -1 : 1;
    }
    buf = JS_GetArrayBufferView(ctx, v, &off, &len, NULL);
    if (JS_IsException(buf))
        return -1;
    p = JS_PeekArrayBuffer(buf, &n);
    JS_FreeValue(ctx, buf);
    if (!p || off > n || len > n - off)
        len = 0;
    *out = fx_bytes_new(ctx, p ? p + off : NULL, len);
    return JS_IsException(*out) ? -1 : 1;
}

static JSValue fx_utf8_bytes(JSContext* ctx, JSValueConst s)
{
    size_t n;
    const char* p = JS_ToCStringLenUTF8(ctx, &n, s);
    JSValue r;
    if (!p)
        return JS_EXCEPTION;
    r = fx_bytes_new(ctx, (const uint8_t*)p, n);
    JS_FreeCString(ctx, p);
    return r;
}

static JSValue fx_form_parts(JSContext* ctx, JSValueConst form)
{
    JSValue flist = fx_box(JS_GetOpaque(form, fx_form_cid)), parts = JS_NewArray(ctx);
    uint32_t n = fx_len(ctx, flist), i;
    if (JS_IsException(parts))
        return parts;
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, flist, i), part, name, val, fn;
        if (JS_IsException(e)) {
            JS_FreeValue(ctx, parts);
            return JS_EXCEPTION;
        }
        name = JS_GetPropertyUint32(ctx, e, 0);
        val = JS_GetPropertyUint32(ctx, e, 1);
        fn = JS_GetPropertyUint32(ctx, e, 2);
        JS_FreeValue(ctx, e);
        part = JS_NewObject(ctx);
        if (JS_IsException(name) || JS_IsException(val) || JS_IsException(fn) || JS_IsException(part)) {
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, val);
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, part);
            JS_FreeValue(ctx, parts);
            return JS_EXCEPTION;
        }
        JS_SetPropertyStr(ctx, part, "name", name);
        if (js_blob_is(val)) {
            JSValue bb = js_blob_bytes(ctx, val), bt = js_blob_type(ctx, val);
            if (!JS_ToBool(ctx, fn)) {
                JS_FreeValue(ctx, fn);
                fn = js_blob_file_name(ctx, val);
            }
            JS_FreeValue(ctx, val);
            if (JS_IsException(bb) || JS_IsException(bt) || JS_IsException(fn)) {
                JS_FreeValue(ctx, bb);
                JS_FreeValue(ctx, bt);
                JS_FreeValue(ctx, fn);
                JS_FreeValue(ctx, part);
                JS_FreeValue(ctx, parts);
                return JS_EXCEPTION;
            }
            JS_SetPropertyStr(ctx, part, "body", bb);
            if (!JS_IsString(bt) || !JS_ToBool(ctx, bt)) {
                JS_FreeValue(ctx, bt);
                bt = fx_lit(ctx, "application/octet-stream");
            }
            JS_SetPropertyStr(ctx, part, "contentType", bt);
        } else if (JS_GetBufferKind(val) != JS_BUFFER_KIND_NONE) {
            JS_SetPropertyStr(ctx, part, "body", val);
        } else {
            JSValue sv = fx_str(ctx, val);
            JS_FreeValue(ctx, val);
            if (JS_IsException(sv)) {
                JS_FreeValue(ctx, fn);
                JS_FreeValue(ctx, part);
                JS_FreeValue(ctx, parts);
                return sv;
            }
            JS_SetPropertyStr(ctx, part, "value", sv);
        }
        if (!JS_ToBool(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            fn = JS_UNDEFINED;
        }
        JS_SetPropertyStr(ctx, part, "filename", fn);
        JS_SetPropertyUint32(ctx, parts, i, part);
    }
    return parts;
}

static int fx_body_extract(JSContext* ctx, FxBody* b, JSValueConst v, JSValue* ct)
{
    int k;
    *ct = JS_UNDEFINED;
    fx_body_init(b);
    if (JS_IsNull(v) || JS_IsUndefined(v))
        return 0;
    b->isnull = 0;
    if (js_blob_is(v)) {
        JSValue t = js_blob_type(ctx, v);
        if (JS_IsException(t))
            return -1;
        b->src = js_blob_bytes(ctx, v);
        if (JS_IsException(b->src)) {
            JS_FreeValue(ctx, t);
            b->src = JS_NULL;
            return -1;
        }
        if (JS_IsString(t) && JS_ToBool(ctx, t))
            *ct = t;
        else
            JS_FreeValue(ctx, t);
        return 0;
    }
    k = JS_IsSharedArrayBuffer(v) ? 0 : fx_buffer_copy(ctx, v, &b->src);
    if (k < 0) {
        b->src = JS_NULL;
        return -1;
    }
    if (k)
        return 0;
    if (JS_IsObject(v) && JS_GetOpaque(v, fx_form_cid)) {
        JSValue parts = fx_form_parts(ctx, v), fmt;
        if (JS_IsException(parts))
            return -1;
        fmt = dyn_http_multipart_format(ctx, parts);
        JS_FreeValue(ctx, parts);
        if (JS_IsException(fmt))
            return -1;
        b->src = JS_GetPropertyStr(ctx, fmt, "body");
        *ct = JS_GetPropertyStr(ctx, fmt, "contentType");
        JS_FreeValue(ctx, fmt);
        return 0;
    }
    if (JS_IsObject(v) && JS_GetOpaque(v, dyn_sp_class_id)) {
        JSValue s = fx_str(ctx, v);
        if (JS_IsException(s))
            return -1;
        b->src = fx_utf8_bytes(ctx, s);
        JS_FreeValue(ctx, s);
        *ct = fx_lit(ctx, "application/x-www-form-urlencoded;charset=UTF-8");
        return JS_IsException(b->src) ? (b->src = JS_NULL, -1) : 0;
    }
    k = fx_is_stream(ctx, v);
    if (k < 0)
        return -1;
    if (k) {
        int l = fx_si_flag(ctx, "isLocked", v), d = l ? l : fx_si_flag(ctx, "isDisturbed", v);
        if (l < 0 || d < 0)
            return -1;
        if (l || d) {
            JS_ThrowTypeError(ctx, "body: the ReadableStream is locked or disturbed");
            return -1;
        }
        b->stream = JS_DupValue(ctx, v);
        return 0;
    }
    {
        JSValue s = fx_str(ctx, v);
        if (JS_IsException(s))
            return -1;
        b->src = s;
    }
    *ct = fx_lit(ctx, "text/plain;charset=UTF-8");
    return JS_IsException(b->src) ? (b->src = JS_NULL, -1) : 0;
}

static int fx_body_bytes(JSContext* ctx, FxBody* b)
{
    JSValue bytes;
    if (!JS_IsString(b->src))
        return 0;
    bytes = fx_utf8_bytes(ctx, b->src);
    if (JS_IsException(bytes))
        return -1;
    JS_FreeValue(ctx, b->src);
    b->src = bytes;
    return 0;
}

static JSValue fx_text_of(JSContext* ctx, JSValueConst str, int json)
{
    size_t n;
    const char* p;
    JSValue r;
    if (!json && JS_GetNarrowStringBytes(ctx, str, &n))
        return JS_DupValue(ctx, str);
    p = JS_ToCStringLenUTF8(ctx, &n, str);
    if (!p)
        return JS_EXCEPTION;
    {
        const char* q = p;
        size_t m = n;
        if (m >= 3 && (uint8_t)q[0] == 0xEF && (uint8_t)q[1] == 0xBB && (uint8_t)q[2] == 0xBF) {
            q += 3;
            m -= 3;
        }
        r = json ? JS_ParseJSON(ctx, q, m, "<body>") : JS_NewStringLen(ctx, q, m);
    }
    JS_FreeCString(ctx, p);
    return r;
}

static int fx_body_used(JSContext* ctx, FxBody* b)
{
    if (b->isnull)
        return 0;
    if (b->used)
        return 1;
    if (JS_IsNull(b->stream))
        return 0;
    return fx_si_flag(ctx, "isDisturbed", b->stream);
}

static int fx_body_unusable(JSContext* ctx, FxBody* b)
{
    int u = fx_body_used(ctx, b);
    if (u || b->isnull || JS_IsNull(b->stream))
        return u;
    return fx_si_flag(ctx, "isLocked", b->stream);
}

static JSValue fx_body_stream(JSContext* ctx, FxBody* b)
{
    JSValue bytes, a[2];
    if (b->isnull)
        return JS_NULL;
    if (!JS_IsNull(b->stream))
        return JS_DupValue(ctx, b->stream);
    if (fx_body_bytes(ctx, b) < 0)
        return JS_EXCEPTION;
    bytes = JS_IsNull(b->src) ? fx_bytes_new(ctx, NULL, 0) : JS_DupValue(ctx, b->src);
    if (JS_IsException(bytes))
        return bytes;
    a[0] = bytes;
    a[1] = JS_NewBool(ctx, b->used);
    b->stream = fx_si_call(ctx, "bodyStream", 2, (JSValueConst*)a);
    JS_FreeValue(ctx, bytes);
    if (JS_IsException(b->stream)) {
        b->stream = JS_NULL;
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, b->src);
    b->src = JS_NULL;
    return JS_DupValue(ctx, b->stream);
}

static JSValue fx_finish(JSContext* ctx, JSValueConst bytes, int kind, int full_ok)
{
    JSValue t, r, buf, a[2];
    size_t off, len, blen;
    switch (kind) {
    case 0:
        return fx_decode(ctx, bytes);
    case 1: {
        const char* p;
        size_t n;
        t = fx_decode(ctx, bytes);
        if (JS_IsException(t))
            return t;
        p = JS_ToCStringLen(ctx, &n, t);
        JS_FreeValue(ctx, t);
        if (!p)
            return JS_EXCEPTION;
        r = JS_ParseJSON(ctx, p, n, "<body>");
        JS_FreeCString(ctx, p);
        return r;
    }
    case 2:
        return JS_DupValue(ctx, bytes);
    default:
        buf = JS_GetTypedArrayBuffer(ctx, bytes, &off, &len, NULL);
        if (JS_IsException(buf))
            return buf;
        if (full_ok && off == 0 && JS_GetArrayBuffer(ctx, &blen, buf) && blen == len)
            return buf;
        a[0] = JS_NewInt64(ctx, (int64_t)off);
        a[1] = JS_NewInt64(ctx, (int64_t)(off + len));
        r = fx_invoke(ctx, buf, "slice", 2, (JSValueConst*)a);
        JS_FreeValue(ctx, buf);
        return r;
    }
}

static JSValue fx_stream_done(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    (void)this_val;
    (void)data;
    return fx_finish(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, magic & 3, 1);
}

static JSValue fx_body_consume(JSContext* ctx, FxBody* b, int kind)
{
    int u;
    JSValue bytes, r;
    if (b->isnull) {
        bytes = fx_bytes_new(ctx, NULL, 0);
        if (JS_IsException(bytes))
            return fx_settle(ctx, bytes);
        r = fx_finish(ctx, bytes, kind, 1);
        JS_FreeValue(ctx, bytes);
        return fx_settle(ctx, r);
    }
    u = fx_body_unusable(ctx, b);
    if (u < 0)
        return fx_settle(ctx, JS_EXCEPTION);
    if (u)
        return fx_settle(ctx, JS_ThrowTypeError(ctx, "Body is unusable: it has already been read or its stream is locked"));
    b->used = 1;
    if (!JS_IsNull(b->stream)) {
        JSValue p = fx_si_call(ctx, "bodyReadAll", 1, (JSValueConst*)&b->stream), cb;
        if (JS_IsException(p))
            return fx_settle(ctx, p);
        if (kind == 2)
            return p;
        cb = JS_NewCFunctionData(ctx, fx_stream_done, 1, kind, 0, NULL);
        r = fx_invoke(ctx, p, "then", 1, (JSValueConst*)&cb);
        JS_FreeValue(ctx, cb);
        JS_FreeValue(ctx, p);
        return r;
    }
    if (JS_IsString(b->src) && kind < 2) {
        r = fx_text_of(ctx, b->src, kind);
        JS_FreeValue(ctx, b->src);
        b->src = JS_NULL;
        return fx_settle(ctx, r);
    }
    if (fx_body_bytes(ctx, b) < 0)
        return fx_settle(ctx, JS_EXCEPTION);
    bytes = b->src;
    b->src = JS_NULL;
    r = fx_finish(ctx, bytes, kind, 1);
    JS_FreeValue(ctx, bytes);
    return fx_settle(ctx, r);
}

static int fx_body_clone(JSContext* ctx, FxBody* from, FxBody* to)
{
    int u = fx_body_unusable(ctx, from);
    fx_body_init(to);
    if (u < 0)
        return -1;
    if (u) {
        JS_ThrowTypeError(ctx, "Cannot clone a consumed response/request (its body was read or its stream is locked)");
        return -1;
    }
    to->isnull = from->isnull;
    if (!JS_IsNull(from->stream)) {
        JSValue pair = fx_si_call(ctx, "tee", 1, (JSValueConst*)&from->stream);
        if (JS_IsException(pair))
            return -1;
        JS_FreeValue(ctx, from->stream);
        from->stream = JS_GetPropertyUint32(ctx, pair, 0);
        to->stream = JS_GetPropertyUint32(ctx, pair, 1);
        JS_FreeValue(ctx, pair);
        return 0;
    }
    if (JS_IsNull(from->src))
        return 0;
    if (JS_IsString(from->src)) {
        to->src = JS_DupValue(ctx, from->src);
        return 0;
    }
    return fx_buffer_copy(ctx, from->src, &to->src) < 0 ? (to->src = JS_NULL, -1) : 0;
}

static void fx_body_move(FxBody* from, FxBody* to)
{
    *to = *from;
    from->src = JS_NULL;
    from->stream = JS_NULL;
    from->used = 1;
}

static int fx_hdr_has(JSContext* ctx, JSValueConst headers, const char* lname)
{
    FxHdr* h;
    JSAtom a;
    int r;
    if (fx_hdr(ctx, headers, 0, &h) < 0)
        return -1;
    if (!h)
        return 0;
    a = JS_NewAtom(ctx, lname);
    if (a == JS_ATOM_NULL)
        return -1;
    r = fx_hdr_find(h, a, 0) >= 0;
    JS_FreeAtom(ctx, a);
    return r;
}

static int fx_hdr_add_raw(JSContext* ctx, JSValueConst headers, const char* lname, JSValueConst v)
{
    FxHdr* h;
    JSValue s;
    JSAtom a;
    int r;
    if (fx_hdr(ctx, headers, 1, &h) < 0 || fx_hdr_writable(ctx, h) < 0)
        return -1;
    s = fx_hvalue(ctx, v, 0);
    if (JS_IsException(s))
        return -1;
    a = JS_NewAtom(ctx, lname);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, s);
        return -1;
    }
    r = fx_hdr_put(ctx, h, JS_AtomToString(ctx, a), s, 0);
    JS_FreeAtom(ctx, a);
    return r;
}

static int fx_add_ct(JSContext* ctx, JSValueConst headers, JSValue ct)
{
    int r = 0;
    if (!JS_IsUndefined(ct)) {
        r = fx_hdr_has(ctx, headers, "content-type");
        if (r == 0)
            r = fx_hdr_add_raw(ctx, headers, "content-type", ct);
        r = r < 0 ? -1 : 0;
    }
    JS_FreeValue(ctx, ct);
    return r;
}

static dyn_url_t* fx_core_parse(JSContext* ctx, JSValueConst input, const dyn_url_t* base, int* oom)
{
    size_t n;
    const char* s = JS_ToCStringLenUTF8(ctx, &n, input);
    char* cs;
    dyn_url_t* u = NULL;
    *oom = 0;
    if (!s) {
        *oom = 1;
        return NULL;
    }
    cs = dyn_strip_tabnl(s, n, &n);
    if (n <= FX_URL_MAX) {
        u = (dyn_url_t*)calloc(1, sizeof *u);
        if (!u)
            *oom = 1;
        else if (dyn_url_parse(u, cs ? cs : s, n, base) < 0) {
            dyn_url_free(u);
            u = NULL;
        }
    }
    free(cs);
    JS_FreeCString(ctx, s);
    return u;
}

static JSValue fx_core_href(JSContext* ctx, const dyn_url_t* u)
{
    dyn_sb_t b;
    JSValue r;
    dyn_sb_init(&b);
    dyn_url_serialize(&b, u);
    if (b.oom) {
        dyn_sb_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

static dyn_url_t* fx_parse_url(JSContext* ctx, JSValueConst s, const char* what)
{
    JSValue loc, href = JS_UNDEFINED;
    dyn_url_t *u, *base = NULL;
    int oom;
    u = fx_core_parse(ctx, s, NULL, &oom);
    if (u || oom)
        return u;
    loc = fx_global(ctx, "location");
    if (JS_IsException(loc))
        return NULL;
    if (JS_IsObject(loc))
        href = JS_GetPropertyStr(ctx, loc, "href");
    JS_FreeValue(ctx, loc);
    if (JS_IsException(href))
        return NULL;
    if (JS_IsString(href))
        base = fx_core_parse(ctx, href, NULL, &oom);
    JS_FreeValue(ctx, href);
    if (base) {
        u = fx_core_parse(ctx, s, base, &oom);
        dyn_url_free(base);
    }
    if (!u && !oom)
        fx_throw_msg(ctx, what, s, "\" is not a valid URL");
    return u;
}

static JSValue fx_url_href(JSContext* ctx, JSValueConst input, const char* what, int no_cred)
{
    JSValue s = fx_str(ctx, input), r;
    dyn_url_t* u;
    if (JS_IsException(s))
        return s;
    u = fx_parse_url(ctx, s, what);
    JS_FreeValue(ctx, s);
    if (!u)
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
    r = fx_core_href(ctx, u);
    if (!JS_IsException(r) && no_cred && ((u->username && *u->username) || (u->password && *u->password))) {
        fx_throw_msg(ctx, "Request: URL \"", r, "\" includes credentials");
        JS_FreeValue(ctx, r);
        r = JS_EXCEPTION;
    }
    dyn_url_free(u);
    return r;
}

static int fx_null_body_status(int s)
{
    return s == 101 || s == 103 || s == 204 || s == 205 || s == 304;
}

static int fx_reason_phrase(JSContext* ctx, JSValueConst s)
{
    size_t n, i;
    const char* p = JS_ToCStringLen(ctx, &n, s);
    int ok = 1;
    if (!p)
        return -1;
    for (i = 0; i < n && ok; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x80)
            ok = c == 0x09 || (c >= 0x20 && c <= 0x7E);
        else if (c == 0xC2 || c == 0xC3)
            i++;
        else
            ok = 0;
    }
    JS_FreeCString(ctx, p);
    return ok;
}

static int fx_init_get(JSContext* ctx, JSValueConst init, const char* const* keys, int n, JSValue* out)
{
    int i;
    for (i = 0; i < n; i++)
        out[i] = JS_UNDEFINED;
    if (JS_IsUndefined(init) || JS_IsNull(init))
        return 0;
    if (!JS_IsObject(init)) {
        JS_ThrowTypeError(ctx, "init must be an object");
        return -1;
    }
    {
        const JSAtom* ka = fx_key_atoms(ctx, keys, n);
        for (i = 0; i < n; i++) {
            out[i] = ka ? JS_GetProperty(ctx, init, ka[i]) : JS_GetPropertyStr(ctx, init, keys[i]);
        if (JS_IsException(out[i])) {
            while (--i >= 0)
                JS_FreeValue(ctx, out[i]);
            out[0] = JS_UNDEFINED;
            return -1;
        }
        }
    }
    return 0;
}

static void fx_init_free(JSContext* ctx, JSValue* v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, v[i]);
}

static int fx_enum(JSContext* ctx, JSValueConst v, const char* const* list, int n, const char* what)
{
    JSValue s = fx_str(ctx, v);
    const char* p;
    int i, r = -1;
    if (JS_IsException(s))
        return -1;
    p = JS_ToCString(ctx, s);
    if (!p) {
        JS_FreeValue(ctx, s);
        return -1;
    }
    for (i = 0; i < n && r < 0; i++)
        if (strcmp(p, list[i]) == 0)
            r = i;
    JS_FreeCString(ctx, p);
    if (r < 0)
        fx_throw_msg(ctx, what, s, "\" is not a valid enum value");
    JS_FreeValue(ctx, s);
    return r;
}

enum { RS_HEADERS, RS_STATUS, RS_TEXT, RS_URL, RS_N };
static const char* const fx_response_keys[RS_N] = { "headers", "status", "statusText", "url" };
static const char* const fx_resp_types[] = { "default", "basic", "error", "opaqueredirect" };

static FxResp* fx_resp(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, fx_resp_cid);
}

static JSValue fx_resp_alloc(JSContext* ctx, JSValueConst new_target, FxResp** out)
{
    FxResp* r = js_mallocz(ctx, sizeof(*r));
    JSValue obj;
    if (!r)
        return JS_EXCEPTION;
    r->status_text = JS_UNDEFINED;
    r->headers = JS_UNDEFINED;
    r->url = JS_UNDEFINED;
    fx_body_init(&r->body);
    r->status = 200;
    obj = fx_make(ctx, new_target, fx_resp_cid, "Response", r);
    if (JS_IsException(obj)) {
        js_free(ctx, r);
        return obj;
    }
    *out = r;
    return obj;
}

static JSValue fx_resp_init(JSContext* ctx, JSValueConst new_target, JSValueConst init, JSValueConst body, JSValue fixed_ct)
{
    JSValue iv[RS_N], obj, ct = JS_UNDEFINED;
    FxResp* r = NULL;
    uint16_t st = 200;
    int bad;
    if (fx_strict_init(ctx, init, fx_response_keys, RS_N) < 0 || fx_init_get(ctx, init, fx_response_keys, RS_N, iv) < 0) {
        JS_FreeValue(ctx, fixed_ct);
        return JS_EXCEPTION;
    }
    obj = JS_UNDEFINED;
    if (!JS_IsUndefined(iv[RS_STATUS])) {
        double d;
        if (JS_ToFloat64(ctx, &d, iv[RS_STATUS]))
            goto fail;
        d = isfinite(d) ? fmod(trunc(d), 65536.0) : 0;
        st = (uint16_t)(d < 0 ? d + 65536.0 : d);
    }
    if (st < 200 || st > 599) {
        JS_ThrowRangeError(ctx, "Response: status %u is outside the range [200, 599]", st);
        goto fail;
    }
    if (!JS_IsUndefined(iv[RS_TEXT])) {
        JSValue s = fx_str(ctx, iv[RS_TEXT]);
        JS_FreeValue(ctx, iv[RS_TEXT]);
        iv[RS_TEXT] = s;
        if (JS_IsException(s))
            goto fail;
        bad = fx_reason_phrase(ctx, s);
        if (bad < 0)
            goto fail;
        if (!bad) {
            fx_throw_msg(ctx, "Response: statusText \"", s, "\" is not a valid reason-phrase");
            goto fail;
        }
    }
    obj = fx_resp_alloc(ctx, new_target, &r);
    if (JS_IsException(obj))
        goto fail;
    r->status = st;
    r->status_text = JS_IsUndefined(iv[RS_TEXT]) ? fx_lit(ctx, "") : JS_DupValue(ctx, iv[RS_TEXT]);
    r->url = JS_IsUndefined(iv[RS_URL]) ? fx_lit(ctx, "") : fx_str(ctx, iv[RS_URL]);
    if (JS_IsException(r->url)) {
        r->url = JS_UNDEFINED;
        goto fail;
    }
    r->headers = fx_headers_make(ctx, JS_UNDEFINED, iv[RS_HEADERS], 1, 0);
    if (JS_IsException(r->headers)) {
        r->headers = JS_UNDEFINED;
        goto fail;
    }
    if (!JS_IsNull(body) && !JS_IsUndefined(body)) {
        if (fx_null_body_status(st)) {
            JS_ThrowTypeError(ctx, "Response: a null body status (101, 103, 204, 205, 304) cannot have a body");
            goto fail;
        }
        if (fx_body_extract(ctx, &r->body, body, &ct) < 0)
            goto fail;
        if (!JS_IsUndefined(fixed_ct)) {
            JS_FreeValue(ctx, ct);
            ct = fixed_ct;
            fixed_ct = JS_UNDEFINED;
        }
        if (fx_add_ct(ctx, r->headers, ct) < 0)
            goto fail;
    }
    JS_FreeValue(ctx, fixed_ct);
    fx_init_free(ctx, iv, RS_N);
    return obj;
fail:
    JS_FreeValue(ctx, fixed_ct);
    fx_init_free(ctx, iv, RS_N);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue fx_resp_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Class constructor Response cannot be invoked without 'new'");
    return fx_resp_init(ctx, new_target, argc > 1 ? argv[1] : JS_UNDEFINED, argc > 0 ? argv[0] : JS_NULL, JS_UNDEFINED);
}

static JSValue fx_resp_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    FxResp* r = fx_resp(ctx, this_val);
    int u;
    if (!r)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return fx_body_stream(ctx, &r->body);
    case 1:
        return JS_NewInt32(ctx, r->status);
    case 2:
        return JS_DupValue(ctx, r->status_text);
    case 3:
        return JS_NewBool(ctx, r->status >= 200 && r->status < 300);
    case 4:
        return JS_DupValue(ctx, r->headers);
    case 5:
        return JS_DupValue(ctx, r->url);
    case 6:
        u = fx_body_used(ctx, &r->body);
        return u < 0 ? JS_EXCEPTION : JS_NewBool(ctx, u);
    case 7:
        return fx_lit(ctx, fx_resp_types[r->type & 3]);
    default:
        return JS_NewBool(ctx, r->redirected);
    }
}

static JSValue fx_resp_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FxResp* r = fx_resp(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!r)
        return fx_settle(ctx, JS_EXCEPTION);
    return fx_body_consume(ctx, &r->body, magic);
}

static int fx_ct_essence_is(const char* s, size_t n, const char* want)
{
    size_t b = 0, e, w = strlen(want);
    while (b < n && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        b++;
    for (e = b; e < n && s[e] != ';'; e++)
        ;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        e--;
    return e - b == w && !strncasecmp(s + b, want, w);
}

static int fx_form_push(JSContext* ctx, JSValueConst list, uint32_t* at, JSValue name, JSValue v, JSValue fn)
{
    JSValue t[3], arr;
    if (JS_IsException(name) || JS_IsException(v) || JS_IsException(fn))
        goto fail;
    if (!JS_IsUndefined(fn)) {
        JSValue f = js_blob_form_entry(ctx, v, fn, 1);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, fn);
        fn = JS_UNDEFINED;
        v = f;
        if (JS_IsException(v))
            goto fail;
    }
    t[0] = name;
    t[1] = v;
    t[2] = JS_UNDEFINED;
    arr = JS_NewArrayFrom(ctx, 3, t);
    if (JS_IsException(arr))
        return -1;
    return JS_DefinePropertyValueUint32(ctx, list, (*at)++, arr, JS_PROP_C_W_E) < 0 ? -1 : 0;
fail:
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, fn);
    return -1;
}

static int fx_form_multipart(JSContext* ctx, JSValueConst list, JSValueConst bytes, JSValueConst ct)
{
    JSValue parts = dyn_http_multipart_parse(ctx, ct, bytes);
    uint32_t n, i, at = 0;
    int rc = 0;
    if (JS_IsException(parts))
        return -1;
    n = fx_len(ctx, parts);
    for (i = 0; i < n && !rc; i++) {
        JSValue p = JS_GetPropertyUint32(ctx, parts, i);
        JSValue name, fn, body, v;
        if (JS_IsException(p)) {
            rc = -1;
            break;
        }
        name = JS_GetPropertyStr(ctx, p, "name");
        fn = JS_GetPropertyStr(ctx, p, "filename");
        body = JS_GetPropertyStr(ctx, p, "body");
        if (JS_IsException(name) || JS_IsException(fn) || JS_IsException(body)) {
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, body);
            JS_FreeValue(ctx, p);
            rc = -1;
            break;
        }
        if (JS_IsUndefined(fn)) {
            v = fx_decode_nobom(ctx, body);
        } else {
            JSValue t = JS_GetPropertyStr(ctx, p, "contentType");
            if (JS_IsException(t)) {
                v = JS_EXCEPTION;
            } else {
                if (!JS_IsString(t)) {
                    JS_FreeValue(ctx, t);
                    t = fx_lit(ctx, "text/plain");
                }
                v = js_blob_from_bytes(ctx, body, t);
                JS_FreeValue(ctx, t);
            }
        }
        JS_FreeValue(ctx, body);
        JS_FreeValue(ctx, p);
        rc = fx_form_push(ctx, list, &at, name, v, fn);
    }
    JS_FreeValue(ctx, parts);
    return rc;
}

static int fx_form_urlencoded(JSContext* ctx, JSValueConst list, JSValueConst bytes)
{
    size_t off, len, asize, i;
    JSValue ab = JS_GetArrayBufferView(ctx, bytes, &off, &len, NULL);
    const uint8_t* base;
    dyn_sp_list_t l;
    uint32_t at = 0;
    int rc = 0;
    if (JS_IsException(ab))
        return -1;
    base = JS_GetArrayBuffer(ctx, &asize, ab);
    dyn_sp_list_init(&l);
    if (base && len)
        dyn_sp_parse((const char*)base + off, len, &l);
    JS_FreeValue(ctx, ab);
    for (i = 0; i < l.n && !rc; i++)
        rc = fx_form_push(ctx, list, &at, JS_NewStringLen(ctx, l.a[i].k, l.a[i].kn), JS_NewStringLen(ctx, l.a[i].v, l.a[i].vn), JS_UNDEFINED);
    dyn_sp_list_free(&l);
    return rc;
}

static JSValue fx_form_from_bytes(JSContext* ctx, JSValueConst bytes, JSValueConst ct)
{
    JSValue list, obj;
    const char* s;
    size_t n;
    int kind = 0, rc;
    if (JS_IsString(ct)) {
        s = JS_ToCStringLen(ctx, &n, ct);
        if (!s)
            return JS_EXCEPTION;
        kind = fx_ct_essence_is(s, n, "multipart/form-data") ? 1 : fx_ct_essence_is(s, n, "application/x-www-form-urlencoded") ? 2 : 0;
        JS_FreeCString(ctx, s);
    }
    if (!kind)
        return JS_ThrowTypeError(ctx, "formData: the body's Content-Type is neither multipart/form-data nor application/x-www-form-urlencoded");
    list = JS_NewArray(ctx);
    if (JS_IsException(list))
        return list;
    rc = kind == 1 ? fx_form_multipart(ctx, list, bytes, ct) : fx_form_urlencoded(ctx, list, bytes);
    if (rc < 0) {
        JS_FreeValue(ctx, list);
        return JS_EXCEPTION;
    }
    obj = fx_make(ctx, JS_UNDEFINED, fx_form_cid, "FormData", fx_unbox(list));
    if (JS_IsException(obj))
        JS_FreeValue(ctx, list);
    return obj;
}

static JSValue fx_blob_done(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    JSValue out = JS_GetPropertyUint32(ctx, data, 0), ct, v;
    int rc;
    if (JS_IsException(out))
        return JS_EXCEPTION;
    if (is_reject) {
        rc = dyn_settle(ctx, out, arg, 1);
        JS_FreeValue(ctx, out);
        return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    ct = JS_GetPropertyUint32(ctx, data, 1);
    v = JS_IsException(ct) ? JS_EXCEPTION : magic ? fx_form_from_bytes(ctx, arg, ct) : js_blob_from_bytes(ctx, arg, ct);
    JS_FreeValue(ctx, ct);
    rc = dyn_settle_value(ctx, out, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, out);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue fx_body_blob(JSContext* ctx, FxBody* b, JSValueConst headers, int form)
{
    FxHdr* h = NULL;
    JSValue ct = JS_NULL, p, out, data, re;
    if (fx_hdr(ctx, headers, 0, &h) < 0)
        return fx_settle(ctx, JS_EXCEPTION);
    if (h) {
        JSAtom a = JS_NewAtom(ctx, "content-type");
        if (a == JS_ATOM_NULL)
            return fx_settle(ctx, JS_EXCEPTION);
        ct = fx_hdr_get_atom(ctx, h, a);
        JS_FreeAtom(ctx, a);
        if (JS_IsException(ct))
            return fx_settle(ctx, ct);
    }
    p = fx_body_consume(ctx, b, 2);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, ct);
        return p;
    }
    out = JS_NewPromisePending(ctx);
    data = JS_NewArray(ctx);
    if (JS_IsException(out) || JS_IsException(data) || JS_SetPropertyUint32(ctx, data, 0, JS_DupValue(ctx, out)) < 0) {
        JS_FreeValue(ctx, ct);
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, data);
        JS_FreeValue(ctx, out);
        return fx_settle(ctx, JS_EXCEPTION);
    }
    if (JS_SetPropertyUint32(ctx, data, 1, ct) < 0)
        re = JS_EXCEPTION;
    else
        re = JS_NewPromiseReaction(ctx, fx_blob_done, data, form);
    JS_FreeValue(ctx, data);
    if (JS_IsException(re) || JS_PromiseReact(ctx, p, re) < 0) {
        JS_FreeValue(ctx, re);
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, out);
        return fx_settle(ctx, JS_EXCEPTION);
    }
    JS_FreeValue(ctx, re);
    JS_FreeValue(ctx, p);
    return out;
}

static JSValue fx_resp_blob(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FxResp* r = fx_resp(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!r)
        return fx_settle(ctx, JS_EXCEPTION);
    return fx_body_blob(ctx, &r->body, r->headers, magic);
}

static JSValue fx_resp_clone(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FxResp* r = fx_resp(ctx, this_val), *c;
    FxHdr* h;
    JSValue obj;
    (void)argc;
    (void)argv;
    if (!r)
        return JS_EXCEPTION;
    obj = fx_resp_alloc(ctx, JS_UNDEFINED, &c);
    if (JS_IsException(obj))
        return obj;
    if (fx_body_clone(ctx, &r->body, &c->body) < 0)
        goto fail;
    if (fx_hdr(ctx, r->headers, 0, &h) < 0)
        goto fail;
    c->headers = fx_headers_make(ctx, JS_UNDEFINED, r->headers, 0, h ? h->guard : 0);
    if (JS_IsException(c->headers)) {
        c->headers = JS_UNDEFINED;
        goto fail;
    }
    c->status = r->status;
    c->status_text = JS_DupValue(ctx, r->status_text);
    c->url = JS_DupValue(ctx, r->url);
    c->type = r->type;
    c->redirected = r->redirected;
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue fx_resp_s_error(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FxResp* r;
    JSValue obj = fx_resp_alloc(ctx, JS_UNDEFINED, &r);
    (void)this_val;
    (void)argc;
    (void)argv;
    if (JS_IsException(obj))
        return obj;
    r->status = 0;
    r->type = 2;
    r->status_text = fx_lit(ctx, "");
    r->url = fx_lit(ctx, "");
    r->headers = fx_headers_make(ctx, JS_UNDEFINED, JS_UNDEFINED, 0, 1);
    if (JS_IsException(r->headers)) {
        r->headers = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue fx_resp_s_redirect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue href, obj;
    FxResp* r;
    FxHdr* h;
    uint32_t st = 302;
    (void)this_val;
    href = fx_url_href(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "Response.redirect: \"", 0);
    if (JS_IsException(href))
        return href;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[1])) {
            JS_FreeValue(ctx, href);
            return JS_EXCEPTION;
        }
        d = isfinite(d) ? fmod(trunc(d), 65536.0) : 0;
        st = (uint32_t)(d < 0 ? d + 65536.0 : d);
    }
    if (st != 301 && st != 302 && st != 303 && st != 307 && st != 308) {
        JS_FreeValue(ctx, href);
        return JS_ThrowRangeError(ctx, "Response.redirect: status %u is not a redirect status (301, 302, 303, 307, 308)", st);
    }
    obj = fx_resp_alloc(ctx, JS_UNDEFINED, &r);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, href);
        return obj;
    }
    r->status = (uint16_t)st;
    r->status_text = fx_lit(ctx, "");
    r->url = fx_lit(ctx, "");
    r->headers = fx_headers_make(ctx, JS_UNDEFINED, JS_UNDEFINED, 0, 0);
    if (JS_IsException(r->headers) || fx_hdr_add_raw(ctx, r->headers, "location", href) < 0 || fx_hdr(ctx, r->headers, 1, &h) < 0) {
        if (JS_IsException(r->headers))
            r->headers = JS_UNDEFINED;
        JS_FreeValue(ctx, href);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    h->guard = 1;
    JS_FreeValue(ctx, href);
    return obj;
}

static JSValue fx_resp_s_json(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue s, bytes, r;
    (void)this_val;
    s = JS_JSONStringify(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s))
        return s;
    if (!JS_IsString(s)) {
        JS_FreeValue(ctx, s);
        return JS_ThrowTypeError(ctx, "Response.json: data is not JSON-serializable");
    }
    bytes = fx_utf8_bytes(ctx, s);
    JS_FreeValue(ctx, s);
    if (JS_IsException(bytes))
        return bytes;
    r = fx_resp_init(ctx, JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, bytes, fx_lit(ctx, "application/json"));
    JS_FreeValue(ctx, bytes);
    return r;
}

enum {
    RI_BODY,
    RI_CACHE,
    RI_CRED,
    RI_DUPLEX,
    RI_HEADERS,
    RI_INTEGRITY,
    RI_KEEPALIVE,
    RI_METHOD,
    RI_MODE,
    RI_PRIORITY,
    RI_REDIRECT,
    RI_REFERRER,
    RI_RPOLICY,
    RI_SIGNAL,
    RI_WINDOW,
    RI_N
};
static const char* const fx_request_keys[RI_N] = { "body", "cache", "credentials", "duplex", "headers", "integrity", "keepalive", "method", "mode", "priority", "redirect", "referrer", "referrerPolicy", "signal", "window" };
static const char* const fx_fetch_keys[RI_N + 4] = { "body", "cache", "credentials", "duplex", "headers", "integrity", "keepalive", "method", "mode", "priority", "redirect", "referrer", "referrerPolicy", "signal", "window", "timeout", "onConnect", "cookieJar", "dyna" };
static const char* const fx_e_cache[] = { "default", "no-store", "reload", "no-cache", "force-cache", "only-if-cached" };
static const char* const fx_e_cred[] = { "same-origin", "omit", "include" };
static const char* const fx_e_mode[] = { "cors", "same-origin", "no-cors", "navigate" };
static const char* const fx_e_prio[] = { "auto", "high", "low" };
static const char* const fx_e_redirect[] = { "follow", "error", "manual" };
static const char* const fx_e_rpolicy[] = { "", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url" };

#define FXQ_GET(q, sh, w) (((q)->bits >> (sh)) & ((1u << (w)) - 1))
#define FXQ_SET(q, sh, w, v) ((q)->bits = ((q)->bits & ~(((1u << (w)) - 1) << (sh))) | ((uint32_t)(v) << (sh)))
enum { QS_CACHE = 0, QS_CRED = 3, QS_MODE = 5, QS_PRIO = 7, QS_REDIR = 9, QS_RPOL = 11, QS_KEEP = 15, QS_OWNSIG = 16 };

static FxReq* fx_req(JSContext* ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, fx_req_cid);
}

static JSValue fx_method(JSContext* ctx, JSValueConst v)
{
    static const char* const norm[] = { "DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT" };
    JSValue s = fx_str(ctx, v);
    const char* p;
    size_t n, i;
    if (JS_IsException(s))
        return s;
    p = JS_ToCStringLen(ctx, &n, s);
    if (!p) {
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    if (!fx_is_token(p, n)) {
        JS_FreeCString(ctx, p);
        fx_throw_msg(ctx, "Request: method \"", s, "\" is not a valid HTTP token");
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    if (!strcasecmp(p, "CONNECT") || !strcasecmp(p, "TRACE") || !strcasecmp(p, "TRACK")) {
        JS_FreeCString(ctx, p);
        fx_throw_msg(ctx, "Request: method \"", s, "\" is forbidden");
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    for (i = 0; i < sizeof norm / sizeof norm[0]; i++) {
        if (!strcasecmp(p, norm[i])) {
            JS_FreeCString(ctx, p);
            JS_FreeValue(ctx, s);
            return fx_lit(ctx, norm[i]);
        }
    }
    JS_FreeCString(ctx, p);
    return s;
}

static JSValue fx_req_extra(JSContext* ctx, FxReq* q, int i)
{
    if (JS_IsUndefined(q->extra))
        return fx_lit(ctx, i == 0 ? "about:client" : "");
    return JS_GetPropertyUint32(ctx, q->extra, (uint32_t)i);
}

static int fx_req_set_extra(JSContext* ctx, FxReq* q, int i, JSValue v)
{
    if (JS_IsUndefined(q->extra)) {
        q->extra = JS_NewArray(ctx);
        if (JS_IsException(q->extra)) {
            q->extra = JS_UNDEFINED;
            JS_FreeValue(ctx, v);
            return -1;
        }
        JS_SetPropertyUint32(ctx, q->extra, 0, fx_lit(ctx, "about:client"));
        JS_SetPropertyUint32(ctx, q->extra, 1, fx_lit(ctx, ""));
    }
    return JS_SetPropertyUint32(ctx, q->extra, (uint32_t)i, v);
}

static JSValue fx_req_alloc(JSContext* ctx, JSValueConst new_target, FxReq** out)
{
    FxReq* q = js_mallocz(ctx, sizeof(*q));
    JSValue obj;
    if (!q)
        return JS_EXCEPTION;
    q->method = JS_UNDEFINED;
    q->url = JS_UNDEFINED;
    q->headers = JS_UNDEFINED;
    q->signal = JS_NULL;
    q->extra = JS_UNDEFINED;
    fx_body_init(&q->body);
    obj = fx_make(ctx, new_target, fx_req_cid, "Request", q);
    if (JS_IsException(obj)) {
        js_free(ctx, q);
        return obj;
    }
    *out = q;
    return obj;
}

static int fx_init_nonempty(JSValue* iv)
{
    int i;
    for (i = 0; i < RI_N; i++)
        if (!JS_IsUndefined(iv[i]))
            return 1;
    return 0;
}

static JSValue fx_req_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst input = argc > 0 ? argv[0] : JS_UNDEFINED, init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue iv[RI_N], obj = JS_UNDEFINED, ct = JS_UNDEFINED;
    FxReq* q = NULL;
    FxReq* src = NULL;
    FxBody initb;
    int e, get, has_init_body;
    fx_body_init(&initb);
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Class constructor Request cannot be invoked without 'new'");
    if (fx_strict_init(ctx, init, fx_request_keys, RI_N) < 0 || fx_init_get(ctx, init, fx_request_keys, RI_N, iv) < 0)
        return JS_EXCEPTION;
    if (JS_IsObject(input))
        src = JS_GetOpaque(input, fx_req_cid);
    obj = fx_req_alloc(ctx, new_target, &q);
    if (JS_IsException(obj))
        goto fail;
    if (src) {
        q->url = JS_DupValue(ctx, src->url);
        q->method = JS_DupValue(ctx, src->method);
        q->bits = src->bits & ~(1u << QS_OWNSIG);
        q->signal = JS_DupValue(ctx, src->signal);
        q->extra = JS_DupValue(ctx, src->extra);
        if (!JS_IsUndefined(q->extra)) {
            JS_FreeValue(ctx, q->extra);
            q->extra = JS_UNDEFINED;
            if (fx_req_set_extra(ctx, q, 0, fx_req_extra(ctx, src, 0)) < 0 || fx_req_set_extra(ctx, q, 1, fx_req_extra(ctx, src, 1)) < 0)
                goto fail;
        }
    } else {
        q->url = fx_url_href(ctx, input, "Request: \"", 1);
        if (JS_IsException(q->url)) {
            q->url = JS_UNDEFINED;
            goto fail;
        }
        q->method = fx_lit(ctx, "GET");
    }
    if (!JS_IsUndefined(iv[RI_WINDOW]) && !JS_IsNull(iv[RI_WINDOW])) {
        JS_ThrowTypeError(ctx, "Request: init.window must be null");
        goto fail;
    }
    if (fx_init_nonempty(iv)) {
        if (FXQ_GET(q, QS_MODE, 2) == 3)
            FXQ_SET(q, QS_MODE, 2, 1);
        if (!JS_IsUndefined(q->extra) && (fx_req_set_extra(ctx, q, 0, fx_lit(ctx, "about:client")) < 0))
            goto fail;
        FXQ_SET(q, QS_RPOL, 4, 0);
    }
    if (!JS_IsUndefined(iv[RI_REFERRER])) {
        JSValue rs = fx_str(ctx, iv[RI_REFERRER]);
        if (JS_IsException(rs))
            goto fail;
        if (!JS_ToBool(ctx, rs)) {
            JS_FreeValue(ctx, rs);
            if (fx_req_set_extra(ctx, q, 0, fx_lit(ctx, "")) < 0)
                goto fail;
        } else {
            JSValue href = fx_url_href(ctx, rs, "Request: referrer \"", 0);
            JS_FreeValue(ctx, rs);
            if (JS_IsException(href))
                goto fail;
            if (fx_is_str(ctx, href, "about:client")) {
                JS_FreeValue(ctx, href);
                href = fx_lit(ctx, "about:client");
            }
            if (fx_req_set_extra(ctx, q, 0, href) < 0)
                goto fail;
        }
    }
    if (!JS_IsUndefined(iv[RI_RPOLICY])) {
        if ((e = fx_enum(ctx, iv[RI_RPOLICY], fx_e_rpolicy, 9, "Request: referrerPolicy \"")) < 0)
            goto fail;
        FXQ_SET(q, QS_RPOL, 4, e);
    }
    if (!JS_IsUndefined(iv[RI_MODE])) {
        if ((e = fx_enum(ctx, iv[RI_MODE], fx_e_mode, 4, "Request: mode \"")) < 0)
            goto fail;
        if (e == 3) {
            JS_ThrowTypeError(ctx, "Request: mode \"navigate\" cannot be set from script");
            goto fail;
        }
        FXQ_SET(q, QS_MODE, 2, e);
    }
    if (!JS_IsUndefined(iv[RI_CRED])) {
        if ((e = fx_enum(ctx, iv[RI_CRED], fx_e_cred, 3, "Request: credentials \"")) < 0)
            goto fail;
        FXQ_SET(q, QS_CRED, 2, e);
    }
    if (!JS_IsUndefined(iv[RI_CACHE])) {
        if ((e = fx_enum(ctx, iv[RI_CACHE], fx_e_cache, 6, "Request: cache \"")) < 0)
            goto fail;
        FXQ_SET(q, QS_CACHE, 3, e);
    }
    if (FXQ_GET(q, QS_CACHE, 3) == 5 && FXQ_GET(q, QS_MODE, 2) != 1) {
        JS_ThrowTypeError(ctx, "Request: cache \"only-if-cached\" requires mode \"same-origin\"");
        goto fail;
    }
    if (!JS_IsUndefined(iv[RI_REDIRECT])) {
        if ((e = fx_enum(ctx, iv[RI_REDIRECT], fx_e_redirect, 3, "Request: redirect \"")) < 0)
            goto fail;
        FXQ_SET(q, QS_REDIR, 2, e);
    }
    if (!JS_IsUndefined(iv[RI_INTEGRITY])) {
        JSValue s = fx_str(ctx, iv[RI_INTEGRITY]);
        if (JS_IsException(s) || fx_req_set_extra(ctx, q, 1, s) < 0)
            goto fail;
    }
    if (!JS_IsUndefined(iv[RI_KEEPALIVE]))
        FXQ_SET(q, QS_KEEP, 1, JS_ToBool(ctx, iv[RI_KEEPALIVE]));
    if (!JS_IsUndefined(iv[RI_PRIORITY])) {
        if ((e = fx_enum(ctx, iv[RI_PRIORITY], fx_e_prio, 3, "Request: priority \"")) < 0)
            goto fail;
        FXQ_SET(q, QS_PRIO, 2, e);
    }
    if (!JS_IsUndefined(iv[RI_METHOD])) {
        JSValue m = fx_method(ctx, iv[RI_METHOD]);
        if (JS_IsException(m))
            goto fail;
        JS_FreeValue(ctx, q->method);
        q->method = m;
    }
    if (!JS_IsUndefined(iv[RI_SIGNAL])) {
        if (!JS_IsNull(iv[RI_SIGNAL]) && !(JS_IsObject(iv[RI_SIGNAL]) && JS_GetOpaque(iv[RI_SIGNAL], fx_signal_cid))) {
            JS_ThrowTypeError(ctx, "Request: init.signal must be an AbortSignal or null");
            goto fail;
        }
        JS_FreeValue(ctx, q->signal);
        q->signal = JS_DupValue(ctx, iv[RI_SIGNAL]);
    }
    if (!JS_IsUndefined(iv[RI_DUPLEX]) && fx_enum(ctx, iv[RI_DUPLEX], (const char* const[]) { "half" }, 1, "Request: duplex \"") < 0)
        goto fail;
    q->headers = fx_headers_make(ctx, JS_UNDEFINED, !JS_IsUndefined(iv[RI_HEADERS]) ? (JSValueConst)iv[RI_HEADERS] : src ? (JSValueConst)src->headers : JS_UNDEFINED, !JS_IsUndefined(iv[RI_HEADERS]), 0);
    if (JS_IsException(q->headers)) {
        q->headers = JS_UNDEFINED;
        goto fail;
    }
    if (FXQ_GET(q, QS_MODE, 2) == 2 && !fx_is_str(ctx, q->method, "GET") && !fx_is_str(ctx, q->method, "HEAD") && !fx_is_str(ctx, q->method, "POST")) {
        JS_ThrowTypeError(ctx, "Request: mode \"no-cors\" allows only GET, HEAD and POST");
        goto fail;
    }
    has_init_body = !JS_IsUndefined(iv[RI_BODY]) && !JS_IsNull(iv[RI_BODY]);
    get = fx_is_str(ctx, q->method, "GET") || fx_is_str(ctx, q->method, "HEAD");
    if (get && (has_init_body || (src && !src->body.isnull))) {
        JS_ThrowTypeError(ctx, "Request: a GET or HEAD request cannot have a body");
        goto fail;
    }
    if (has_init_body) {
        if (fx_body_extract(ctx, &initb, iv[RI_BODY], &ct) < 0)
            goto fail;
        if (FXQ_GET(q, QS_KEEP, 1) && !JS_IsNull(initb.stream)) {
            JS_FreeValue(ctx, ct);
            JS_ThrowTypeError(ctx, "Request: keepalive cannot be used with a ReadableStream body");
            goto fail;
        }
        if (fx_add_ct(ctx, q->headers, ct) < 0)
            goto fail;
        if (!JS_IsNull(initb.stream) && JS_IsUndefined(iv[RI_DUPLEX])) {
            JS_ThrowTypeError(ctx, "Request: a ReadableStream body requires init.duplex = \"half\"");
            goto fail;
        }
        q->body = initb;
        fx_body_init(&initb);
    } else if (src && !src->body.isnull) {
        int u = fx_body_unusable(ctx, &src->body);
        if (u) {
            if (u > 0)
                JS_ThrowTypeError(ctx, "Request: the input Request's body is unusable (already read or locked)");
            goto fail;
        }
        fx_body_move(&src->body, &q->body);
    }
    if (!q->body.isnull && !JS_IsNull(q->body.stream) && FXQ_GET(q, QS_MODE, 2) >= 2) {
        JS_ThrowTypeError(ctx, "Request: a ReadableStream body requires mode \"cors\" or \"same-origin\"");
        goto fail;
    }
    fx_init_free(ctx, iv, RI_N);
    return obj;
fail:
    fx_body_free(JS_GetRuntime(ctx), &initb);
    fx_init_free(ctx, iv, RI_N);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue fx_signal_follow(JSContext* ctx, JSValueConst source)
{
    if (JS_IsNull(source))
        return fx_signal_new(ctx, JS_UNDEFINED);
    return fx_signal_any_of(ctx, 1, &source);
}

static JSValue fx_req_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    FxReq* q = fx_req(ctx, this_val);
    int u;
    if (!q)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_DupValue(ctx, q->method);
    case 1:
        return JS_DupValue(ctx, q->url);
    case 2:
        return JS_DupValue(ctx, q->headers);
    case 3:
        if (!FXQ_GET(q, QS_OWNSIG, 1)) {
            JSValue sig = fx_signal_follow(ctx, q->signal);
            if (JS_IsException(sig))
                return sig;
            JS_FreeValue(ctx, q->signal);
            q->signal = sig;
            FXQ_SET(q, QS_OWNSIG, 1, 1);
        }
        return JS_DupValue(ctx, q->signal);
    case 4:
        return fx_lit(ctx, fx_e_redirect[FXQ_GET(q, QS_REDIR, 2) % 3]);
    case 5:
        return fx_body_stream(ctx, &q->body);
    case 6:
        u = fx_body_used(ctx, &q->body);
        return u < 0 ? JS_EXCEPTION : JS_NewBool(ctx, u);
    case 7:
        return fx_lit(ctx, fx_e_cache[FXQ_GET(q, QS_CACHE, 3) % 6]);
    case 8:
        return fx_lit(ctx, fx_e_cred[FXQ_GET(q, QS_CRED, 2) % 3]);
    case 9:
        return fx_lit(ctx, fx_e_mode[FXQ_GET(q, QS_MODE, 2)]);
    case 10:
        return fx_lit(ctx, "");
    case 11:
        return fx_req_extra(ctx, q, 0);
    case 12:
        return fx_lit(ctx, fx_e_rpolicy[FXQ_GET(q, QS_RPOL, 4) % 9]);
    case 13:
        return fx_req_extra(ctx, q, 1);
    case 14:
        return JS_NewBool(ctx, FXQ_GET(q, QS_KEEP, 1));
    case 15:
    case 16:
        return JS_FALSE;
    default:
        return fx_lit(ctx, "half");
    }
}

static JSValue fx_req_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FxReq* q = fx_req(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!q)
        return fx_settle(ctx, JS_EXCEPTION);
    return fx_body_consume(ctx, &q->body, magic);
}

static JSValue fx_req_blob(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    FxReq* q = fx_req(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!q)
        return fx_settle(ctx, JS_EXCEPTION);
    return fx_body_blob(ctx, &q->body, q->headers, magic);
}

static JSValue fx_req_clone(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FxReq* q = fx_req(ctx, this_val), *c;
    JSValue obj, sig;
    (void)argc;
    (void)argv;
    if (!q)
        return JS_EXCEPTION;
    sig = fx_req_get(ctx, this_val, 3);
    if (JS_IsException(sig))
        return sig;
    obj = fx_req_alloc(ctx, JS_UNDEFINED, &c);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, sig);
        return obj;
    }
    c->signal = sig;
    if (fx_body_clone(ctx, &q->body, &c->body) < 0)
        goto fail;
    c->url = JS_DupValue(ctx, q->url);
    c->method = JS_DupValue(ctx, q->method);
    c->bits = q->bits & ~(1u << QS_OWNSIG);
    c->extra = JS_UNDEFINED;
    if (!JS_IsUndefined(q->extra) && (fx_req_set_extra(ctx, c, 0, fx_req_extra(ctx, q, 0)) < 0 || fx_req_set_extra(ctx, c, 1, fx_req_extra(ctx, q, 1)) < 0))
        goto fail;
    c->headers = fx_headers_make(ctx, JS_UNDEFINED, q->headers, 0, 0);
    if (JS_IsException(c->headers)) {
        c->headers = JS_UNDEFINED;
        goto fail;
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue fx_headers_obj(JSContext* ctx, FxHdr* h)
{
    JSValue o = JS_NewObject(ctx);
    uint32_t i;
    if (!h || JS_IsException(o))
        return o;
    for (i = 0; i < h->n; i++) {
        JSValue v;
        if (fx_hdr_find(h, h->e[i].name, 0) != (int32_t)i)
            continue;
        v = fx_hdr_get_atom(ctx, h, h->e[i].name);
        if (JS_IsException(v) || JS_SetProperty(ctx, o, h->e[i].name, v) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
    }
    return o;
}

static JSValue fx_h_prep(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    FxReq* q = fx_req(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    FxHdr* h;
    JSValue out, hobj;
    (void)this_val;
    if (!q || fx_hdr(ctx, q->headers, 0, &h) < 0)
        return JS_EXCEPTION;
    hobj = fx_headers_obj(ctx, h);
    if (JS_IsException(hobj))
        return hobj;
    out = JS_NewObject(ctx);
    if (JS_IsException(out)) {
        JS_FreeValue(ctx, hobj);
        return out;
    }
    JS_SetPropertyStr(ctx, out, "url", JS_DupValue(ctx, q->url));
    JS_SetPropertyStr(ctx, out, "method", JS_DupValue(ctx, q->method));
    JS_SetPropertyStr(ctx, out, "headers", hobj);
    JS_SetPropertyStr(ctx, out, "signal", JS_DupValue(ctx, q->signal));
    JS_SetPropertyStr(ctx, out, "redirect", fx_lit(ctx, fx_e_redirect[FXQ_GET(q, QS_REDIR, 2) % 3]));
    if (!q->body.isnull) {
        if (!JS_IsNull(q->body.stream)) {
            JS_SetPropertyStr(ctx, out, "stream", JS_DupValue(ctx, q->body.stream));
            q->body.used = 1;
        } else if (!JS_IsNull(q->body.src)) {
            if (fx_body_bytes(ctx, &q->body) < 0) {
                JS_FreeValue(ctx, out);
                return JS_EXCEPTION;
            }
            JS_SetPropertyStr(ctx, out, "body", JS_DupValue(ctx, q->body.src));
        }
    }
    return out;
}

static JSValue fx_h_read_all(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    return fx_si_call(ctx, "bodyReadAll", argc > 0 ? 1 : 0, argv);
}

static int fx_is_redirect_status(JSContext* ctx, JSValueConst raw)
{
    JSValue s = JS_GetPropertyStr(ctx, raw, "status");
    double d;
    int r = 0;
    if (JS_IsException(s))
        return -1;
    if (JS_IsNumber(s)) {
        JS_ToFloat64(ctx, &d, s);
        r = d == 301 || d == 302 || d == 303 || d == 307 || d == 308;
    }
    JS_FreeValue(ctx, s);
    return r;
}

static JSValue fx_location(JSContext* ctx, JSValueConst raw)
{
    JSValue h = JS_GetPropertyStr(ctx, raw, "headers"), v = JS_NULL;
    JSPropertyEnum* tab;
    uint32_t len, i;
    if (JS_IsException(h))
        return h;
    if (JS_IsObject(h)) {
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, h, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
            JS_FreeValue(ctx, h);
            return JS_EXCEPTION;
        }
        for (i = 0; i < len; i++) {
            const char* k = JS_AtomToCString(ctx, tab[i].atom);
            int hit = k && strcasecmp(k, "location") == 0 && strlen(k) == 8;
            JS_FreeCString(ctx, k);
            if (hit) {
                v = JS_GetProperty(ctx, h, tab[i].atom);
                break;
            }
        }
        JS_FreePropertyEnum(ctx, tab, len);
    } else if (JS_IsNull(h) || JS_IsUndefined(h)) {
        JS_FreeValue(ctx, h);
        return JS_ThrowTypeError(ctx, "cannot convert to object");
    }
    JS_FreeValue(ctx, h);
    return v;
}

static int fx_loc_empty(JSContext* ctx, JSValueConst loc)
{
    if (JS_IsNull(loc))
        return 1;
    return fx_is_str(ctx, loc, "");
}

static JSValue fx_resolve(JSContext* ctx, JSValueConst loc, JSValueConst url)
{
    dyn_url_t *base, *u;
    JSValue r;
    int oom;
    base = fx_core_parse(ctx, url, NULL, &oom);
    if (!base)
        return oom ? JS_ThrowOutOfMemory(ctx) : JS_ThrowTypeError(ctx, "invalid base URL");
    u = fx_core_parse(ctx, loc, base, &oom);
    dyn_url_free(base);
    if (!u)
        return oom ? JS_ThrowOutOfMemory(ctx) : JS_ThrowTypeError(ctx, "invalid Location");
    r = fx_core_href(ctx, u);
    dyn_url_free(u);
    return r;
}

static JSValue fx_h_hop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const drop_body[] = { "content-type", "content-encoding", "content-language", "content-location" };
    static const char* const drop_cred[] = { "authorization", "cookie", "cookie2", "proxy-authorization" };
    JSValueConst client, mode, raw, url, method, body, headers, hop;
    JSValue loc, next, sym, fn, plan, status, a[5], out, nh, flag;
    JSPropertyEnum* tab;
    uint32_t len, i;
    int m, rs, e, err;
    (void)this_val;
    if (argc < 8)
        return JS_ThrowTypeError(ctx, "hop: missing arguments");
    client = argv[0];
    mode = argv[1];
    raw = argv[2];
    url = argv[3];
    method = argv[4];
    body = argv[5];
    headers = argv[6];
    hop = argv[7];
    m = fx_is_str(ctx, mode, "manual");
    if (m)
        return m < 0 ? JS_EXCEPTION : JS_NULL;
    rs = fx_is_redirect_status(ctx, raw);
    if (rs <= 0)
        return rs < 0 ? JS_EXCEPTION : JS_NULL;
    loc = fx_location(ctx, raw);
    if (JS_IsException(loc))
        return loc;
    e = fx_loc_empty(ctx, loc);
    if (e) {
        JS_FreeValue(ctx, loc);
        return e < 0 ? JS_EXCEPTION : JS_NULL;
    }
    next = fx_resolve(ctx, loc, url);
    if (JS_IsException(next)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        fx_throw_msg(ctx, "fetch: invalid Location in redirect: ", loc, "");
        JS_FreeValue(ctx, loc);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, loc);
    {
        JSValue sy = fx_global(ctx, "Symbol"), key = JS_NewString(ctx, "dyna.http.redirect");
        sym = fx_invoke(ctx, sy, "for", 1, (JSValueConst*)&key);
        JS_FreeValue(ctx, sy);
        JS_FreeValue(ctx, key);
    }
    if (JS_IsException(sym)) {
        JS_FreeValue(ctx, next);
        return sym;
    }
    {
        JSAtom at = JS_ValueToAtom(ctx, sym);
        fn = JS_GetProperty(ctx, client, at);
        JS_FreeAtom(ctx, at);
    }
    JS_FreeValue(ctx, sym);
    if (JS_IsException(fn)) {
        JS_FreeValue(ctx, next);
        return JS_EXCEPTION;
    }
    status = JS_GetPropertyStr(ctx, raw, "status");
    if (JS_IsException(status)) {
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, next);
        return JS_EXCEPTION;
    }
    a[0] = status;
    a[1] = JS_DupValue(ctx, method);
    a[2] = JS_DupValue(ctx, url);
    a[3] = JS_DupValue(ctx, next);
    a[4] = JS_DupValue(ctx, hop);
    plan = JS_Call(ctx, fn, client, 5, (JSValueConst*)a);
    for (i = 0; i < 5; i++)
        JS_FreeValue(ctx, a[i]);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(plan) || JS_IsNull(plan)) {
        JS_FreeValue(ctx, next);
        return plan;
    }
    nh = JS_NewObject(ctx);
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, headers, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
        JS_FreeValue(ctx, nh);
        JS_FreeValue(ctx, plan);
        JS_FreeValue(ctx, next);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        JSValue hv = JS_GetProperty(ctx, headers, tab[i].atom);
        if (JS_IsException(hv) || JS_SetProperty(ctx, nh, tab[i].atom, hv) < 0) {
            JS_FreePropertyEnum(ctx, tab, len);
            JS_FreeValue(ctx, nh);
            JS_FreeValue(ctx, plan);
            JS_FreeValue(ctx, next);
            return JS_EXCEPTION;
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    out = JS_NewObject(ctx);
    flag = JS_GetPropertyStr(ctx, plan, "dropBody");
    if (JS_IsException(flag))
        goto hop_fail;
    if (JS_ToBool(ctx, flag)) {
        for (i = 0; i < 4; i++) {
            JSAtom at = JS_NewAtom(ctx, drop_body[i]);
            JS_DeleteProperty(ctx, nh, at, 0);
            JS_FreeAtom(ctx, at);
        }
        JS_SetPropertyStr(ctx, out, "body", JS_UNDEFINED);
    } else {
        JS_SetPropertyStr(ctx, out, "body", JS_DupValue(ctx, body));
    }
    JS_FreeValue(ctx, flag);
    flag = JS_GetPropertyStr(ctx, plan, "stripCredentials");
    if (JS_IsException(flag))
        goto hop_fail;
    if (JS_ToBool(ctx, flag)) {
        for (i = 0; i < 4; i++) {
            JSAtom at = JS_NewAtom(ctx, drop_cred[i]);
            JS_DeleteProperty(ctx, nh, at, 0);
            JS_FreeAtom(ctx, at);
        }
    }
    JS_FreeValue(ctx, flag);
    err = fx_is_str(ctx, mode, "error");
    if (err) {
        if (err > 0)
            fx_throw_msg(ctx, "fetch: redirect to ", next, " refused by redirect: \"error\"");
        JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, nh);
        JS_FreeValue(ctx, plan);
        JS_FreeValue(ctx, next);
        return JS_EXCEPTION;
    }
    {
        JSValue mv = JS_GetPropertyStr(ctx, plan, "method");
        if (JS_IsException(mv))
            goto hop_fail;
        JS_SetPropertyStr(ctx, out, "url", next);
        JS_SetPropertyStr(ctx, out, "method", mv);
    }
    JS_SetPropertyStr(ctx, out, "headers", nh);
    JS_FreeValue(ctx, plan);
    return out;
hop_fail:
    JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, nh);
    JS_FreeValue(ctx, plan);
    JS_FreeValue(ctx, next);
    return JS_EXCEPTION;
}

static JSValue fx_h_response_from(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst raw = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue obj, v;
    FxResp* r;
    int32_t st = 0;
    (void)this_val;
    obj = fx_resp_alloc(ctx, JS_UNDEFINED, &r);
    if (JS_IsException(obj))
        return obj;
    v = JS_GetPropertyStr(ctx, raw, "status");
    if (JS_IsException(v) || JS_ToInt32(ctx, &st, v)) {
        JS_FreeValue(ctx, v);
        goto fail;
    }
    JS_FreeValue(ctx, v);
    r->status = (uint16_t)st;
    r->type = 1;
    r->redirected = argc > 2 && JS_ToBool(ctx, argv[2]);
    r->url = argc > 1 && JS_IsString(argv[1]) ? JS_DupValue(ctx, argv[1]) : fx_lit(ctx, "");
    v = JS_GetPropertyStr(ctx, raw, "statusText");
    if (JS_IsException(v))
        goto fail;
    r->status_text = JS_IsString(v) ? v : (JS_FreeValue(ctx, v), fx_lit(ctx, ""));
    v = JS_GetPropertyStr(ctx, raw, "headers");
    if (JS_IsException(v))
        goto fail;
    r->headers = fx_headers_make(ctx, JS_UNDEFINED, v, 0, 1);
    JS_FreeValue(ctx, v);
    if (JS_IsException(r->headers)) {
        r->headers = JS_UNDEFINED;
        goto fail;
    }
    if (!fx_null_body_status(st)) {
        r->body.isnull = 0;
        v = JS_GetPropertyStr(ctx, raw, "bodyBytes");
        if (JS_IsException(v))
            goto fail;
        if (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER) {
            r->body.src = fx_u8_view(ctx, v);
            if (JS_IsException(r->body.src)) {
                r->body.src = JS_NULL;
                goto fail;
            }
        } else {
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, raw, "body");
            if (JS_IsException(v))
                goto fail;
            r->body.src = JS_IsString(v) ? fx_utf8_bytes(ctx, v) : fx_bytes_new(ctx, NULL, 0);
            JS_FreeValue(ctx, v);
            if (JS_IsException(r->body.src)) {
                r->body.src = JS_NULL;
                goto fail;
            }
        }
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue fx_fetch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    JSValueConst input = argc > 0 ? argv[0] : JS_UNDEFINED, init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue loop, bag = JS_DupValue(ctx, init), a[6], r;
    JSValue tmo = JS_UNDEFINED, oc = JS_UNDEFINED, jar = JS_UNDEFINED, dy = JS_UNDEFINED;
    int i;
    (void)this_val;
    (void)magic;
    if (fx_strict_init(ctx, init, fx_fetch_keys, RI_N + 4) < 0) {
        JS_FreeValue(ctx, bag);
        return JS_EXCEPTION;
    }
    if (JS_IsObject(init)) {
        tmo = JS_GetPropertyStr(ctx, init, "timeout");
        oc = JS_IsException(tmo) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, init, "onConnect");
        jar = JS_IsException(tmo) || JS_IsException(oc) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, init, "cookieJar");
        dy = JS_IsException(tmo) || JS_IsException(oc) || JS_IsException(jar) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, init, "dyna");
        if (JS_IsException(tmo) || JS_IsException(oc) || JS_IsException(jar) || JS_IsException(dy)) {
            JS_FreeValue(ctx, tmo);
            JS_FreeValue(ctx, oc);
            JS_FreeValue(ctx, jar);
            JS_FreeValue(ctx, dy);
            JS_FreeValue(ctx, bag);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(tmo) || !JS_IsUndefined(oc) || !JS_IsUndefined(jar) || !JS_IsUndefined(dy)) {
            static const char* const drop[] = { "timeout", "onConnect", "cookieJar", "dyna" };
            JSValue obj = fx_global(ctx, "Object"), args[2], copy;
            args[0] = JS_NewObject(ctx);
            args[1] = JS_DupValue(ctx, init);
            copy = fx_invoke(ctx, obj, "assign", 2, (JSValueConst*)args);
            JS_FreeValue(ctx, obj);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
            JS_FreeValue(ctx, bag);
            if (JS_IsException(copy)) {
                JS_FreeValue(ctx, tmo);
                JS_FreeValue(ctx, oc);
                JS_FreeValue(ctx, jar);
                JS_FreeValue(ctx, dy);
                return copy;
            }
            for (i = 0; i < 4; i++) {
                JSAtom at = JS_NewAtom(ctx, drop[i]);
                JS_DeleteProperty(ctx, copy, at, 0);
                JS_FreeAtom(ctx, at);
            }
            bag = copy;
        } else {
            JS_FreeValue(ctx, tmo);
            JS_FreeValue(ctx, oc);
            JS_FreeValue(ctx, jar);
            JS_FreeValue(ctx, dy);
            tmo = oc = jar = dy = JS_UNDEFINED;
        }
    }
    loop = JS_GetPropertyStr(ctx, data[0], "loop");
    if (JS_IsUndefined(loop)) {
        JSValue factory = JS_Eval(ctx, dyn_fetch_loop_src, strlen(dyn_fetch_loop_src), "<fetch>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(factory)) {
            JS_FreeValue(ctx, bag);
            return factory;
        }
        loop = JS_Call(ctx, factory, JS_UNDEFINED, 1, (JSValueConst*)&data[0]);
        JS_FreeValue(ctx, factory);
        if (JS_IsException(loop)) {
            JS_FreeValue(ctx, bag);
            return loop;
        }
        JS_SetPropertyStr(ctx, data[0], "loop", JS_DupValue(ctx, loop));
    }
    a[0] = JS_DupValue(ctx, input);
    a[1] = bag;
    a[2] = tmo;
    a[3] = oc;
    a[4] = jar;
    a[5] = dy;
    r = JS_Call(ctx, loop, JS_UNDEFINED, 6, (JSValueConst*)a);
    for (i = 0; i < 6; i++)
        JS_FreeValue(ctx, a[i]);
    JS_FreeValue(ctx, loop);
    return r;
}

static void fx_headers_fin(JSRuntime* rt, JSValue v)
{
    FxHdr* h = JS_GetOpaque(v, fx_headers_cid);
    if (h && (void*)h != (void*)&fx_empty) {
        while (h->n)
            fx_hdr_drop(rt, h, h->n - 1);
        js_free_rt(rt, h->e);
        js_free_rt(rt, h->ord);
        js_free_rt(rt, h);
    }
}

static void fx_headers_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    FxHdr* h = JS_GetOpaque(v, fx_headers_cid);
    if (h && (void*)h != (void*)&fx_empty) {
        uint32_t i;
        for (i = 0; i < h->n; i++)
            JS_MarkValue(rt, h->e[i].value, mf);
    }
}

static void fx_iter_fin(JSRuntime* rt, JSValue v)
{
    FxIt* it = JS_GetOpaque(v, JS_GetClassID(v));
    if (it) {
        JS_FreeValueRT(rt, it->target);
        js_free_rt(rt, it);
    }
}

static void fx_iter_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    FxIt* it = JS_GetOpaque(v, JS_GetClassID(v));
    if (it)
        JS_MarkValue(rt, it->target, mf);
}

static void fx_form_fin(JSRuntime* rt, JSValue v)
{
    fx_box_free(rt, JS_GetOpaque(v, fx_form_cid));
}

static void fx_form_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    fx_box_mark(rt, JS_GetOpaque(v, fx_form_cid), mf);
}

static void fx_signal_fin(JSRuntime* rt, JSValue v)
{
    FxSignal* s = JS_GetOpaque(v, fx_signal_cid);
    uint32_t i;
    if (s) {
        JS_FreeValueRT(rt, s->reason);
        JS_FreeValueRT(rt, s->onabort);
        JS_FreeValueRT(rt, s->timer);
        for (i = 0; i < s->n; i++)
            JS_FreeValueRT(rt, s->lv[i]);
        for (i = 0; i < s->nd; i++)
            JS_FreeValueRT(rt, s->deps[i]);
        js_free_rt(rt, s->lv);
        js_free_rt(rt, s->lf);
        js_free_rt(rt, s->deps);
        js_free_rt(rt, s);
    }
}

static void fx_signal_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    FxSignal* s = JS_GetOpaque(v, fx_signal_cid);
    uint32_t i;
    if (s) {
        JS_MarkValue(rt, s->reason, mf);
        JS_MarkValue(rt, s->onabort, mf);
        JS_MarkValue(rt, s->timer, mf);
        for (i = 0; i < s->n; i++)
            JS_MarkValue(rt, s->lv[i], mf);
        for (i = 0; i < s->nd; i++)
            JS_MarkValue(rt, s->deps[i], mf);
    }
}

static void fx_ctrl_fin(JSRuntime* rt, JSValue v)
{
    fx_box_free(rt, JS_GetOpaque(v, fx_ctrl_cid));
}

static void fx_ctrl_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    fx_box_mark(rt, JS_GetOpaque(v, fx_ctrl_cid), mf);
}

static void fx_resp_fin(JSRuntime* rt, JSValue v)
{
    FxResp* r = JS_GetOpaque(v, fx_resp_cid);
    if (r) {
        JS_FreeValueRT(rt, r->status_text);
        JS_FreeValueRT(rt, r->headers);
        JS_FreeValueRT(rt, r->url);
        fx_body_free(rt, &r->body);
        js_free_rt(rt, r);
    }
}

static void fx_resp_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    FxResp* r = JS_GetOpaque(v, fx_resp_cid);
    if (r) {
        JS_MarkValue(rt, r->status_text, mf);
        JS_MarkValue(rt, r->headers, mf);
        JS_MarkValue(rt, r->url, mf);
        fx_body_mark(rt, &r->body, mf);
    }
}

static void fx_req_fin(JSRuntime* rt, JSValue v)
{
    FxReq* q = JS_GetOpaque(v, fx_req_cid);
    if (q) {
        JS_FreeValueRT(rt, q->method);
        JS_FreeValueRT(rt, q->url);
        JS_FreeValueRT(rt, q->headers);
        JS_FreeValueRT(rt, q->signal);
        JS_FreeValueRT(rt, q->extra);
        fx_body_free(rt, &q->body);
        js_free_rt(rt, q);
    }
}

static void fx_req_mark(JSRuntime* rt, JSValueConst v, JS_MarkFunc* mf)
{
    FxReq* q = JS_GetOpaque(v, fx_req_cid);
    if (q) {
        JS_MarkValue(rt, q->method, mf);
        JS_MarkValue(rt, q->url, mf);
        JS_MarkValue(rt, q->headers, mf);
        JS_MarkValue(rt, q->signal, mf);
        JS_MarkValue(rt, q->extra, mf);
        fx_body_mark(rt, &q->body, mf);
    }
}

static const JSClassDef fx_classes[6] = {
    { .class_name = "Headers", .finalizer = fx_headers_fin, .gc_mark = fx_headers_mark },
    { .class_name = "FormData", .finalizer = fx_form_fin, .gc_mark = fx_form_mark },
    { .class_name = "AbortSignal", .finalizer = fx_signal_fin, .gc_mark = fx_signal_mark },
    { .class_name = "AbortController", .finalizer = fx_ctrl_fin, .gc_mark = fx_ctrl_mark },
    { .class_name = "Response", .finalizer = fx_resp_fin, .gc_mark = fx_resp_mark },
    { .class_name = "Request", .finalizer = fx_req_fin, .gc_mark = fx_req_mark },
};

static const JSCFunctionListEntry fx_headers_funcs[] = {
    JS_CFUNC_MAGIC_DEF("append", 2, fx_headers_method, 3),
    JS_CFUNC_MAGIC_DEF("delete", 1, fx_headers_method, 0),
    JS_CFUNC_MAGIC_DEF("get", 1, fx_headers_method, 1),
    JS_CFUNC_DEF("getSetCookie", 0, fx_headers_set_cookie),
    JS_CFUNC_MAGIC_DEF("has", 1, fx_headers_method, 2),
    JS_CFUNC_MAGIC_DEF("set", 2, fx_headers_method, 4),
    JS_CFUNC_DEF("forEach", 2, fx_headers_foreach),
    JS_CFUNC_MAGIC_DEF("keys", 0, fx_headers_iter, 0),
    JS_CFUNC_MAGIC_DEF("values", 0, fx_headers_iter, 1),
    JS_CFUNC_MAGIC_DEF("entries", 0, fx_headers_iter, 2),
};

static const JSCFunctionListEntry fx_form_funcs[] = {
    JS_CFUNC_MAGIC_DEF("append", 3, fx_form_method, 0),
    JS_CFUNC_MAGIC_DEF("delete", 1, fx_form_method, 1),
    JS_CFUNC_MAGIC_DEF("get", 1, fx_form_method, 2),
    JS_CFUNC_MAGIC_DEF("getAll", 1, fx_form_method, 3),
    JS_CFUNC_MAGIC_DEF("has", 1, fx_form_method, 4),
    JS_CFUNC_MAGIC_DEF("set", 3, fx_form_method, 5),
    JS_CFUNC_DEF("forEach", 2, fx_form_foreach),
    JS_CFUNC_MAGIC_DEF("keys", 0, fx_form_iter, 0),
    JS_CFUNC_MAGIC_DEF("values", 0, fx_form_iter, 1),
    JS_CFUNC_MAGIC_DEF("entries", 0, fx_form_iter, 2),
};

static const JSCFunctionListEntry fx_signal_funcs[] = {
    JS_CGETSET_MAGIC_DEF("aborted", fx_signal_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("reason", fx_signal_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("onabort", fx_signal_get, fx_signal_set_onabort, 2),
    JS_CFUNC_MAGIC_DEF("addEventListener", 2, fx_signal_listen, 0),
    JS_CFUNC_MAGIC_DEF("removeEventListener", 2, fx_signal_listen, 1),
    JS_CFUNC_DEF("throwIfAborted", 0, fx_signal_throw_if),
};

static const JSCFunctionListEntry fx_signal_static[] = {
    JS_CFUNC_DEF("abort", 0, fx_signal_s_abort),
    JS_CFUNC_DEF("timeout", 1, fx_signal_s_timeout),
    JS_CFUNC_DEF("any", 1, fx_signal_s_any),
};

static const JSCFunctionListEntry fx_ctrl_funcs[] = {
    JS_CGETSET_DEF("signal", fx_ctrl_signal, NULL),
    JS_CFUNC_DEF("abort", 1, fx_ctrl_abort),
};

static const JSCFunctionListEntry fx_resp_funcs[] = {
    JS_CGETSET_MAGIC_DEF("type", fx_resp_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("url", fx_resp_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("redirected", fx_resp_get, NULL, 8),
    JS_CGETSET_MAGIC_DEF("status", fx_resp_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("ok", fx_resp_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("statusText", fx_resp_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("headers", fx_resp_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("body", fx_resp_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("bodyUsed", fx_resp_get, NULL, 6),
    JS_CFUNC_DEF("clone", 0, fx_resp_clone),
    JS_CFUNC_MAGIC_DEF("arrayBuffer", 0, fx_resp_read, 3),
    JS_CFUNC_MAGIC_DEF("blob", 0, fx_resp_blob, 0),
    JS_CFUNC_MAGIC_DEF("formData", 0, fx_resp_blob, 1),
    JS_CFUNC_MAGIC_DEF("bytes", 0, fx_resp_read, 2),
    JS_CFUNC_MAGIC_DEF("json", 0, fx_resp_read, 1),
    JS_CFUNC_MAGIC_DEF("text", 0, fx_resp_read, 0),
};

static const JSCFunctionListEntry fx_resp_static[] = {
    JS_CFUNC_DEF("error", 0, fx_resp_s_error),
    JS_CFUNC_DEF("redirect", 1, fx_resp_s_redirect),
    JS_CFUNC_DEF("json", 1, fx_resp_s_json),
};

static const JSCFunctionListEntry fx_req_funcs[] = {
    JS_CGETSET_MAGIC_DEF("method", fx_req_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("url", fx_req_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("headers", fx_req_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("destination", fx_req_get, NULL, 10),
    JS_CGETSET_MAGIC_DEF("referrer", fx_req_get, NULL, 11),
    JS_CGETSET_MAGIC_DEF("referrerPolicy", fx_req_get, NULL, 12),
    JS_CGETSET_MAGIC_DEF("mode", fx_req_get, NULL, 9),
    JS_CGETSET_MAGIC_DEF("credentials", fx_req_get, NULL, 8),
    JS_CGETSET_MAGIC_DEF("cache", fx_req_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("redirect", fx_req_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("integrity", fx_req_get, NULL, 13),
    JS_CGETSET_MAGIC_DEF("keepalive", fx_req_get, NULL, 14),
    JS_CGETSET_MAGIC_DEF("isReloadNavigation", fx_req_get, NULL, 15),
    JS_CGETSET_MAGIC_DEF("isHistoryNavigation", fx_req_get, NULL, 16),
    JS_CGETSET_MAGIC_DEF("signal", fx_req_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("duplex", fx_req_get, NULL, 17),
    JS_CGETSET_MAGIC_DEF("body", fx_req_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("bodyUsed", fx_req_get, NULL, 6),
    JS_CFUNC_DEF("clone", 0, fx_req_clone),
    JS_CFUNC_MAGIC_DEF("arrayBuffer", 0, fx_req_read, 3),
    JS_CFUNC_MAGIC_DEF("blob", 0, fx_req_blob, 0),
    JS_CFUNC_MAGIC_DEF("formData", 0, fx_req_blob, 1),
    JS_CFUNC_MAGIC_DEF("bytes", 0, fx_req_read, 2),
    JS_CFUNC_MAGIC_DEF("json", 0, fx_req_read, 1),
    JS_CFUNC_MAGIC_DEF("text", 0, fx_req_read, 0),
};

static const JSCFunctionListEntry fx_helper_funcs[] = {
    JS_CFUNC_DEF("prep", 1, fx_h_prep),
    JS_CFUNC_DEF("readAll", 1, fx_h_read_all),
    JS_CFUNC_DEF("hop", 8, fx_h_hop),
    JS_CFUNC_DEF("responseFrom", 2, fx_h_response_from),
    JS_CFUNC_DEF("untime", 1, fx_h_untime),
};

static const JSClassDef fx_iter_classes[2] = {
    { .class_name = "Headers Iterator", .finalizer = fx_iter_fin, .gc_mark = fx_iter_mark },
    { .class_name = "FormData Iterator", .finalizer = fx_iter_fin, .gc_mark = fx_iter_mark },
};

static const JSCFunctionListEntry fx_iter_funcs[2][1] = {
    { JS_ITERATOR_NEXT_DEF("next", 0, fx_iter_next, 0) },
    { JS_ITERATOR_NEXT_DEF("next", 0, fx_iter_next, 1) },
};

static void fx_tag(JSContext* ctx, JSValueConst obj, const char* tag)
{
    JSValue sy = fx_global(ctx, "Symbol");
    JSValue t = JS_IsException(sy) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, sy, "toStringTag");
    JSAtom at = JS_IsException(t) ? JS_ATOM_NULL : JS_ValueToAtom(ctx, t);
    if (at != JS_ATOM_NULL)
        JS_DefinePropertyValue(ctx, obj, at, JS_NewString(ctx, tag), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, sy);
}

static void fx_install_iter(JSContext* ctx, int i, JSClassID* cid)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue arr, it, p1, p2, proto;
    JS_NewClassID(cid);
    if (!JS_IsRegisteredClass(rt, *cid))
        JS_NewClass(rt, *cid, &fx_iter_classes[i]);
    arr = JS_NewArray(ctx);
    it = fx_invoke(ctx, arr, "values", 0, NULL);
    p1 = JS_GetPrototype(ctx, it);
    p2 = JS_GetPrototype(ctx, p1);
    proto = JS_NewObjectProto(ctx, p2);
    JS_SetPropertyFunctionList(ctx, proto, fx_iter_funcs[i], 1);
    fx_enumerable(ctx, proto);
    fx_tag(ctx, proto, fx_iter_classes[i].class_name);
    JS_SetClassProto(ctx, *cid, proto);
    JS_FreeValue(ctx, p2);
    JS_FreeValue(ctx, p1);
    JS_FreeValue(ctx, it);
    JS_FreeValue(ctx, arr);
}

static void fx_enumerable(JSContext* ctx, JSValueConst proto)
{
    JSPropertyEnum* tab;
    uint32_t len, i;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, proto, JS_GPN_STRING_MASK))
        return;
    for (i = 0; i < len; i++)
        JS_DefineProperty(ctx, proto, tab[i].atom, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_PROP_HAS_ENUMERABLE | JS_PROP_ENUMERABLE);
    JS_FreePropertyEnum(ctx, tab, len);
}

static JSValue fx_install_class(JSContext* ctx, JSValueConst global_obj, int i, JSClassID* cid,
    JSCFunction* ctor_fn, int length, const JSCFunctionListEntry* funcs, int nfuncs, int iterable)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;
    JS_NewClassID(cid);
    if (!JS_IsRegisteredClass(rt, *cid))
        JS_NewClass(rt, *cid, &fx_classes[i]);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, funcs, nfuncs);
    fx_enumerable(ctx, proto);
    if (iterable) {
        JSValue sy = fx_global(ctx, "Symbol");
        JSValue it = JS_IsException(sy) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, sy, "iterator");
        JSAtom at = JS_IsException(it) ? JS_ATOM_NULL : JS_ValueToAtom(ctx, it);
        JSValue ent = at == JS_ATOM_NULL ? JS_EXCEPTION : JS_GetPropertyStr(ctx, proto, "entries");
        if (!JS_IsException(ent))
            JS_DefinePropertyValue(ctx, proto, at, ent, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
        JS_FreeAtom(ctx, at);
        JS_FreeValue(ctx, it);
        JS_FreeValue(ctx, sy);
    }
    ctor = JS_NewCFunction2(ctx, ctor_fn, fx_classes[i].class_name, length, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    fx_tag(ctx, proto, fx_classes[i].class_name);
    JS_SetClassProto(ctx, *cid, proto);
    JS_DefinePropertyValueStr(ctx, global_obj, fx_classes[i].class_name, JS_DupValue(ctx, ctor), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return ctor;
}

void js_fetch_install(JSContext* ctx, JSValueConst global_obj)
{
    JSValue c, helper, fetch;
    if (fx_req_cid && JS_IsRegisteredClass(JS_GetRuntime(ctx), fx_req_cid)) {
        JSValue p = JS_GetClassProto(ctx, fx_req_cid);
        int have = JS_IsObject(p);
        JS_FreeValue(ctx, p);
        if (have)
            return;
    }
    js_blob_install(ctx, global_obj);
    js_domex_install(ctx, global_obj);
    fx_orig_install(ctx);
    fx_install_iter(ctx, 0, &fx_hiter_cid);
    fx_install_iter(ctx, 1, &fx_fiter_cid);
    JS_FreeValue(ctx, fx_install_class(ctx, global_obj, 0, &fx_headers_cid, fx_headers_ctor, 1, fx_headers_funcs, countof(fx_headers_funcs), 1));
    JS_FreeValue(ctx, fx_install_class(ctx, global_obj, 1, &fx_form_cid, fx_form_ctor, 0, fx_form_funcs, countof(fx_form_funcs), 1));
    c = fx_install_class(ctx, global_obj, 2, &fx_signal_cid, fx_signal_ctor, 0, fx_signal_funcs, countof(fx_signal_funcs), 0);
    JS_SetPropertyFunctionList(ctx, c, fx_signal_static, countof(fx_signal_static));
    JS_FreeValue(ctx, c);
    JS_FreeValue(ctx, fx_install_class(ctx, global_obj, 3, &fx_ctrl_cid, fx_ctrl_ctor, 0, fx_ctrl_funcs, countof(fx_ctrl_funcs), 0));
    c = fx_install_class(ctx, global_obj, 4, &fx_resp_cid, fx_resp_ctor, 0, fx_resp_funcs, countof(fx_resp_funcs), 0);
    JS_SetPropertyFunctionList(ctx, c, fx_resp_static, countof(fx_resp_static));
    JS_FreeValue(ctx, c);
    c = fx_install_class(ctx, global_obj, 5, &fx_req_cid, fx_req_ctor, 1, fx_req_funcs, countof(fx_req_funcs), 0);
    helper = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, helper, fx_helper_funcs, countof(fx_helper_funcs));
    JS_SetPropertyStr(ctx, helper, "Request", c);
    fetch = JS_NewCFunctionData(ctx, fx_fetch, 2, 0, 1, &helper);
    JS_FreeValue(ctx, helper);
    {
        JSAtom at = JS_NewAtom(ctx, "name");
        JS_DefinePropertyValue(ctx, fetch, at, JS_NewString(ctx, "fetch"), JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, at);
    }
    JS_DefinePropertyValueStr(ctx, global_obj, "fetch", fetch, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}
