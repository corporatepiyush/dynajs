#include "structures-internal.h"
#include "core/ds/ds-patricia.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

enum { DSX_PT_EMPTY,
    DSX_PT_KEYS,
    DSX_PT_CIDR };

#define DSX_PT_V2 0x80

typedef struct {
    JSRuntime* rt;
    int bytes;
    int space;
    dyn_patricia_t* t;
    dyn_patricia_cursor cur;
} dsx_patricia_t;

typedef struct {
    JSContext* ctx;
    dsx_patricia_t* p;
    JSValue arr;
    uint32_t n;
    int failed;
} dsx_pt_collect;

static int dsx_patricia_space(JSContext* ctx, dsx_patricia_t* p, int want, int inserting)
{
    if (dyn_patricia_size(p->t) == 0) {
        if (inserting)
            p->space = want;
        return 0;
    }
    if (p->space == want)
        return 0;
    JS_ThrowTypeError(ctx, want == DSX_PT_CIDR ? "PatriciaTrie: this trie holds string/byte keys, not CIDR blocks" : "PatriciaTrie: this trie holds CIDR blocks; use addCIDR/matchIP/deleteCIDR");
    return -1;
}

static void dsx_patricia_free(void* native)
{
    dsx_patricia_t* p = (dsx_patricia_t*)native;
    if (!p)
        return;
    dyn_patricia_free(p->t);
    dyn_patricia_cursor_free(&p->cur);
    dyn_ds_free(p);
}

JSClassID dsx_patricia_class_id;

DYN_FINALIZER(dsx_patricia)

static int dsx_pt_mark_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    dsx_mark_ctx* m = (dsx_mark_ctx*)ud;
    (void)key;
    (void)bits;
    dsx_mark_value(m->rt, val, m->mark_func);
    return 0;
}

static void dsx_patricia_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    dsx_patricia_t* p = JS_GetOpaque(val, dsx_patricia_class_id);
    dsx_mark_ctx m = { rt, mark_func };
    if (p)
        dyn_patricia_walk(p->t, (const uint8_t*)"", 0, dsx_pt_mark_visit, &m);
}

const JSClassDef dsx_patricia_class = { "PatriciaTrie", .finalizer = dsx_patricia_finalizer, .gc_mark = dsx_patricia_gc_mark };

JSValue dsx_patricia_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    dsx_patricia_t* p;
    dyn_ds_ops vo;
    int bytes = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue k = JS_GetPropertyStr(ctx, argv[0], "keys");
        if (JS_IsException(k))
            return k;
        if (!JS_IsUndefined(k)) {
            const char* s = JS_ToCString(ctx, k);
            JS_FreeValue(ctx, k);
            if (!s)
                return JS_EXCEPTION;
            if (!strcmp(s, "bytes"))
                bytes = 1;
            else if (strcmp(s, "string")) {
                JS_FreeCString(ctx, s);
                return JS_ThrowTypeError(ctx, "PatriciaTrie: keys must be \"string\" or \"bytes\"");
            }
            JS_FreeCString(ctx, s);
        }
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "PatriciaTrie: the options must be an object");
    }
    p = (dsx_patricia_t*)dyn_ds_calloc(1, sizeof(*p));
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    p->rt = JS_GetRuntime(ctx);
    p->bytes = bytes;
    p->space = DSX_PT_EMPTY;
    vo = dsx_val_ops(p->rt);
    p->t = dyn_patricia_new(&vo);
    if (!p->t) {
        dyn_ds_free(p);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsx_patricia_class_id, p, dsx_patricia_free);
}

static JSValue dsx_patricia_key_value(JSContext* ctx, dsx_patricia_t* p, const uint8_t* key, uint32_t bits)
{
    if (p->bytes)
        return dyn_nat_u8_copy(ctx, key, bits / 8);
    return JS_NewStringLen(ctx, (const char*)key, bits / 8);
}

static JSValue dsx_patricia_op(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    dsx_patricia_t* p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    dsx_bytes kb;
    JSValue out = JS_UNDEFINED;
    if (!p)
        return JS_EXCEPTION;
    if (dsx_key_bytes(ctx, argv[0], p->bytes, &kb))
        return JS_EXCEPTION;
    p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    if (!p) {
        dsx_key_bytes_free(ctx, &kb);
        return JS_EXCEPTION;
    }
    if (kb.n > 0x1FFFFFFF) {
        dsx_key_bytes_free(ctx, &kb);
        return JS_ThrowRangeError(ctx, "PatriciaTrie: key too long");
    }
    if (dsx_patricia_space(ctx, p, DSX_PT_KEYS, magic == 0) < 0) {
        dsx_key_bytes_free(ctx, &kb);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0: {
        JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
        if (dyn_patricia_put(p->t, kb.p, (uint32_t)kb.n * 8, &v) < 0)
            out = JS_ThrowOutOfMemory(ctx);
        else
            out = JS_DupValue(ctx, this_val);
    } break;
    case 1: {
        void* v = dyn_patricia_get(p->t, kb.p, (uint32_t)kb.n * 8);
        out = v ? JS_DupValue(ctx, dsx_val_get(v)) : JS_UNDEFINED;
    } break;
    case 2:
        out = JS_NewBool(ctx, dyn_patricia_get(p->t, kb.p, (uint32_t)kb.n * 8) != NULL);
        break;
    case 3:
        out = JS_NewBool(ctx, dyn_patricia_remove(p->t, kb.p, (uint32_t)kb.n * 8));
        break;
    default: {
        uint32_t matched = 0;
        void* v = dyn_patricia_longest_prefix(p->t, kb.p, (uint32_t)kb.n * 8, &matched);
        if (v) {
            JSValue pair[2];
            pair[0] = dsx_patricia_key_value(ctx, p, kb.p, matched);
            pair[1] = JS_DupValue(ctx, dsx_val_get(v));
            out = JS_NewArrayFrom(ctx, 2, pair);
        }
    } break;
    }
    dsx_key_bytes_free(ctx, &kb);
    return out;
}

static int dsx_pt_collect_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    dsx_pt_collect* c = (dsx_pt_collect*)ud;
    JSValue pair[2], e;
    pair[0] = dsx_patricia_key_value(c->ctx, c->p, key, bits);
    pair[1] = JS_DupValue(c->ctx, dsx_val_get(val));
    e = JS_NewArrayFrom(c->ctx, 2, pair);
    if (JS_IsException(e) || JS_DefinePropertyValueUint32(c->ctx, c->arr, c->n++, e, JS_PROP_C_W_E) < 0) {
        c->failed = 1;
        return 1;
    }
    return 0;
}

static JSValue dsx_patricia_with_prefix(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_patricia_t* p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    dsx_bytes kb;
    dsx_pt_collect c;
    if (!p)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (dsx_key_bytes(ctx, argv[0], p->bytes, &kb))
            return JS_EXCEPTION;
    } else {
        kb.p = (const uint8_t*)"";
        kb.n = 0;
        kb.cstr = NULL;
        kb.buf = JS_UNDEFINED;
    }
    p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    if (!p || kb.n > 0x1FFFFFFF) {
        dsx_key_bytes_free(ctx, &kb);
        return p ? JS_ThrowRangeError(ctx, "PatriciaTrie: prefix too long") : JS_EXCEPTION;
    }
    if (dsx_patricia_space(ctx, p, DSX_PT_KEYS, 0) < 0) {
        dsx_key_bytes_free(ctx, &kb);
        return JS_EXCEPTION;
    }
    c.ctx = ctx;
    c.p = p;
    c.n = 0;
    c.failed = 0;
    c.arr = JS_NewArray(ctx);
    if (JS_IsException(c.arr)) {
        dsx_key_bytes_free(ctx, &kb);
        return c.arr;
    }
    dyn_patricia_walk(p->t, kb.p, (uint32_t)kb.n * 8, dsx_pt_collect_visit, &c);
    dsx_key_bytes_free(ctx, &kb);
    if (c.failed) {
        JS_FreeValue(ctx, c.arr);
        return JS_EXCEPTION;
    }
    return c.arr;
}

static int dsx_parse_cidr(JSContext* ctx, JSValueConst v, int need_len, uint8_t* key, uint32_t* bits)
{
    const char* s = JS_ToCString(ctx, v);
    char buf[64];
    const char* slash;
    int family, plen, maxlen;
    unsigned char addr[16];
    size_t alen;
    if (!s)
        return -1;
    slash = strchr(s, '/');
    alen = slash ? (size_t)(slash - s) : strlen(s);
    if (alen >= sizeof(buf)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "not an IP address or CIDR block");
        return -1;
    }
    memcpy(buf, s, alen);
    buf[alen] = 0;
    if (inet_pton(AF_INET, buf, addr) == 1) {
        family = 4;
        maxlen = 32;
    } else if (inet_pton(AF_INET6, buf, addr) == 1) {
        family = 6;
        maxlen = 128;
    } else {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "not an IP address or CIDR block");
        return -1;
    }
    plen = maxlen;
    if (slash) {
        char* end;
        long l = strtol(slash + 1, &end, 10);
        if (!slash[1] || *end || l < 0 || l > maxlen) {
            JS_FreeCString(ctx, s);
            JS_ThrowRangeError(ctx, "the prefix length must be 0..%d", maxlen);
            return -1;
        }
        plen = (int)l;
    } else if (need_len) {
        plen = maxlen;
    }
    JS_FreeCString(ctx, s);
    key[0] = (uint8_t)family;
    memcpy(key + 1, addr, (size_t)maxlen / 8);
    *bits = 8 + (uint32_t)plen;
    return 0;
}

static JSValue dsx_patricia_cidr(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    uint8_t key[17];
    uint32_t bits;
    dsx_patricia_t* p;
    if (dsx_parse_cidr(ctx, argv[0], magic == 0, key, &bits))
        return JS_EXCEPTION;
    p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    if (!p || dsx_patricia_space(ctx, p, DSX_PT_CIDR, magic == 0) < 0)
        return JS_EXCEPTION;
    if (magic == 0) {
        JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
        if (dyn_patricia_put(p->t, key, bits, &v) < 0)
            return JS_ThrowOutOfMemory(ctx);
        return JS_DupValue(ctx, this_val);
    }
    if (magic == 1) {
        void* v = dyn_patricia_longest_prefix(p->t, key, bits, NULL);
        return v ? JS_DupValue(ctx, dsx_val_get(v)) : JS_UNDEFINED;
    }
    return JS_NewBool(ctx, dyn_patricia_remove(p->t, key, bits));
}

static JSValue dsx_patricia_size(JSContext* ctx, JSValueConst this_val)
{
    dsx_patricia_t* p = dyn_plain_get(ctx, this_val, dsx_patricia_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_patricia_size(p->t));
}

const JSCFunctionListEntry dsx_patricia_proto[] = {
    JS_CFUNC_MAGIC_DEF("set", 2, dsx_patricia_op, 0),
    JS_CFUNC_MAGIC_DEF("get", 1, dsx_patricia_op, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, dsx_patricia_op, 2),
    JS_CFUNC_MAGIC_DEF("delete", 1, dsx_patricia_op, 3),
    JS_CFUNC_MAGIC_DEF("longestPrefix", 1, dsx_patricia_op, 4),
    JS_CFUNC_DEF("withPrefix", 1, dsx_patricia_with_prefix),
    JS_CFUNC_MAGIC_DEF("addCIDR", 2, dsx_patricia_cidr, 0),
    JS_CFUNC_MAGIC_DEF("matchIP", 1, dsx_patricia_cidr, 1),
    JS_CFUNC_MAGIC_DEF("deleteCIDR", 1, dsx_patricia_cidr, 2),
    JS_CGETSET_DEF("size", dsx_patricia_size, NULL),
};

typedef struct {
    dsx_flat f;
    dyn_ser_t* w;
} dsx_pt_ser2;

static int dsx_pt_ser_visit(const uint8_t* key, uint32_t bits, void* val, void* ud)
{
    dsx_pt_ser2* s = (dsx_pt_ser2*)ud;
    if (dyn_ser_u32(s->w, bits) < 0 || dyn_ser_raw(s->w, key, (bits + 7) / 8) < 0) {
        s->f.failed = 1;
        return 1;
    }
    dsx_flat_push(&s->f, JS_DupValue(s->f.ctx, dsx_val_get(val)));
    return s->f.failed;
}

int dsx_patricia_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_patricia_t* p = JS_GetOpaque(obj, dsx_patricia_class_id);
    dsx_pt_ser2 s;
    size_t n;
    if (!p)
        return -1;
    n = dyn_patricia_size(p->t);
    if (n > UINT32_MAX || dyn_ser_u8(w, (uint8_t)(p->bytes | DSX_PT_V2)) < 0 || dyn_ser_u8(w, (uint8_t)(n ? p->space : DSX_PT_EMPTY)) < 0 || dyn_ser_u32(w, (uint32_t)n) < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (dsx_flat_open(ctx, &s.f) < 0)
        return -1;
    s.w = w;
    dyn_patricia_walk(p->t, (const uint8_t*)"", 0, dsx_pt_ser_visit, &s);
    if (s.f.failed && !JS_HasException(ctx))
        JS_ThrowOutOfMemory(ctx);
    return dsx_flat_close(&s.f, w);
}

static int dsx_pt_key_ok(int space, const uint8_t* k, size_t len, uint32_t bits)
{
    if (len > 0x1FFFFFFF)
        return 0;
    if (space == DSX_PT_CIDR) {
        uint32_t maxbits;
        if (len < 1 || (k[0] != 4 && k[0] != 6))
            return 0;
        maxbits = 8 + (k[0] == 4 ? 32u : 128u);
        return bits >= 8 && bits <= maxbits && len == ((size_t)bits + 7) / 8;
    }
    return (uint64_t)bits == (uint64_t)len * 8;
}

static int dsx_pt_insert(dsx_patricia_t* p, const uint8_t* kp, size_t len, uint32_t bits, const JSValue* v)
{
    uint8_t key[17];
    if (p->space == DSX_PT_CIDR) {
        memset(key, 0, sizeof(key));
        memcpy(key, kp, len);
        kp = key;
    }
    return dyn_patricia_put_cursor(p->t, &p->cur, kp, bits, v);
}

static JSValue dsx_patricia_restore(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_patricia_t* p = JS_GetOpaque(this_val, dsx_patricia_class_id);
    size_t off, len, bpe;
    JSValue ab;
    uint8_t* base;
    size_t ablen;
    double dbits;
    uint32_t bits;
    int ok;
    (void)argc;
    if (!p)
        return JS_EXCEPTION;
    if (!JS_IsNumber(argv[1]) || JS_ToFloat64(ctx, &dbits, argv[1]))
        return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key length is not a number");
    ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
    if (JS_IsException(ab))
        return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key is not bytes");
    base = JS_GetArrayBuffer(ctx, &ablen, ab);
    ok = dbits >= 0 && dbits <= 4294967295.0 && dbits == (double)(uint32_t)dbits && (base || !len) && dsx_pt_key_ok(p->space, base ? base + off : (const uint8_t*)"", len, (uint32_t)dbits);
    if (!ok) {
        JS_FreeValue(ctx, ab);
        return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key does not fit its key space");
    }
    bits = (uint32_t)dbits;
    {
        int rc = dsx_pt_insert(p, base ? base + off : (const uint8_t*)"", len, bits, &argv[2]);
        JS_FreeValue(ctx, ab);
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
    }
    return JS_UNDEFINED;
}

static JSValue dsx_patricia_read_v2(JSContext* ctx, dyn_de_t* r, JSValue obj, dsx_patricia_t* p)
{
    uint32_t n = 0, i, nv;
    const uint8_t** keys = NULL;
    uint32_t* bits = NULL;
    JSValue arr;
    if (dyn_de_count(r, &n, 4) < 0) {
        JS_FreeValue(ctx, obj);
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    }
    if ((n == 0) != (p->space == DSX_PT_EMPTY)) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key space does not match its entries");
    }
    keys = (const uint8_t**)dyn_ds_malloc(sizeof(*keys) * ((size_t)n + 1));
    bits = (uint32_t*)dyn_ds_malloc(sizeof(*bits) * ((size_t)n + 1));
    if (!keys || !bits) {
        dyn_ds_free(keys);
        dyn_ds_free(bits);
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) {
        size_t len;
        bits[i] = dyn_de_u32(r);
        len = ((size_t)bits[i] + 7) / 8;
        keys[i] = dyn_de_ok(r) ? dyn_de_raw(r, len) : NULL;
        if (!dyn_de_ok(r) || (len && !keys[i]))
            goto trunc;
        if (!dsx_pt_key_ok(p->space, keys[i] ? keys[i] : (const uint8_t*)"", len, bits[i]))
            goto bad;
    }
    arr = dsx_flat_read(ctx, r, 1, &nv);
    if (JS_IsException(arr)) {
        dyn_ds_free(keys);
        dyn_ds_free(bits);
        JS_FreeValue(ctx, obj);
        return arr;
    }
    if (nv != n) {
        JS_FreeValue(ctx, arr);
        goto trunc;
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        int vex = JS_IsException(v);
        int rc = vex ? -1 : dsx_pt_insert(p, keys[i] ? keys[i] : (const uint8_t*)"", ((size_t)bits[i] + 7) / 8, bits[i], &v);
        JS_FreeValue(ctx, v);
        if (rc < 0) {
            JS_FreeValue(ctx, arr);
            dyn_ds_free(keys);
            dyn_ds_free(bits);
            JS_FreeValue(ctx, obj);
            return vex ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
        }
    }
    JS_FreeValue(ctx, arr);
    dyn_ds_free(keys);
    dyn_ds_free(bits);
    dyn_patricia_cursor_free(&p->cur);
    return obj;
trunc:
    dyn_ds_free(keys);
    dyn_ds_free(bits);
    JS_FreeValue(ctx, obj);
    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
bad:
    dyn_ds_free(keys);
    dyn_ds_free(bits);
    JS_FreeValue(ctx, obj);
    return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key does not fit its key space");
}

JSValue dsx_patricia_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint8_t bytes = dyn_de_u8(r), space = dyn_de_u8(r);
    uint32_t n;
    JSValue arr, obj;
    dsx_patricia_t* p;
    (void)opts;
    if (!dyn_de_ok(r) || (bytes & ~DSX_PT_V2) > 1 || space > DSX_PT_CIDR)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    if (bytes & DSX_PT_V2) {
        obj = dsx_patricia_ctor(ctx, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(obj))
            return obj;
        p = JS_GetOpaque(obj, dsx_patricia_class_id);
        p->bytes = bytes & 1;
        p->space = space;
        return dsx_patricia_read_v2(ctx, r, obj, p);
    }
    arr = dsx_flat_read(ctx, r, 3, &n);
    if (JS_IsException(arr))
        return arr;
    if ((n == 0) != (space == DSX_PT_EMPTY)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx, "malformed DYNS record: PatriciaTrie key space does not match its entries");
    }
    obj = dsx_patricia_ctor(ctx, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, arr);
        return obj;
    }
    p = JS_GetOpaque(obj, dsx_patricia_class_id);
    p->bytes = bytes;
    p->space = space;
    obj = dsx_replay(ctx, obj, arr, n, 3, dsx_patricia_restore);
    if (!JS_IsException(obj))
        dyn_patricia_cursor_free(&p->cur);
    return obj;
}

#endif
