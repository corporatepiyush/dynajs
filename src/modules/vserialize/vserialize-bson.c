#include "vserialize-internal.h"
#include "vserialize-bson.h"

#if defined(CONFIG_NATIVE_MODULES) && (defined(CONFIG_NATIVE_MODULE_VSERIALIZE) || defined(CONFIG_NATIVE_MODULE_DOCDB))

#define BSONJS_FAST_KEYS 32
#define BSONJS_TPL_MAX 32

static void bsonjs_fix_surrogates(uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i + 2 < n; i++) {
        if (p[i] == 0xED && p[i + 1] >= 0xA0) {
            p[i] = 0xEF;
            p[i + 1] = 0xBF;
            p[i + 2] = 0xBD;
            i += 2;
        }
    }
}

static int bsonjs_oom(DynBsonEnc* e)
{
    if (e->w->oom) {
        JS_ThrowRangeError(e->ctx, "BSON: document exceeds the 2 GiB format limit or memory is exhausted");
        return -1;
    }
    return 0;
}

static int bsonjs_put_string(DynBsonEnc* e, const char* key, size_t klen, JSValueConst v, uint8_t type)
{
    size_t n, at;
    const char* s = JS_ToCStringLen(e->ctx, &n, v);
    if (!s)
        return -1;
    dyn_bson_w_key(e->w, type, key, klen);
    if (n >= DYN_BSON_MAX_SIZE - 8 || !dyn_bson_w_reserve(e->w, n + 5)) {
        JS_FreeCString(e->ctx, s);
        e->w->oom = 1;
        return bsonjs_oom(e);
    }
    dyn_bson_wr32(e->w->p + e->w->n, (uint32_t)n + 1);
    e->w->n += 4;
    at = e->w->n;
    memcpy(e->w->p + at, s, n);
    e->w->p[at + n] = 0;
    e->w->n += n + 1;
    JS_FreeCString(e->ctx, s);
    if (dyn_wtf8_has_surrogate((const char*)e->w->p + at, n))
        bsonjs_fix_surrogates(e->w->p + at, n);
    return 0;
}

static int bsonjs_put_int64(DynBsonEnc* e, const char* key, size_t klen, JSValueConst v)
{
    int64_t i;
    JSValue back;
    int same;
    if (JS_ToBigInt64(e->ctx, &i, v))
        return -1;
    back = JS_NewBigInt64(e->ctx, i);
    if (JS_IsException(back))
        return -1;
    same = JS_StrictEq(e->ctx, back, v);
    JS_FreeValue(e->ctx, back);
    if (!same) {
        JS_ThrowRangeError(e->ctx, "BSON: BigInt outside the int64 range");
        return -1;
    }
    dyn_bson_w_int64(e->w, key, klen, i);
    return 0;
}

static int bsonjs_regex_opts(const char* flags, size_t n, char out[8])
{
    static const char order[] = "ilmsux";
    int have[6] = { 0 }, k, o = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        const char* p = memchr(order, flags[i], 6);
        if (p)
            have[p - order] = 1;
    }
    for (k = 0; k < 6; k++)
        if (have[k])
            out[o++] = order[k];
    out[o] = 0;
    return o;
}

static int bsonjs_put_regexp(DynBsonEnc* e, const char* key, size_t klen, JSValueConst v)
{
    JSValue src = JS_GetPropertyStr(e->ctx, v, "source"), fl;
    const char *s, *f;
    size_t sn, fn;
    char opts[8];
    int olen;
    if (JS_IsException(src))
        return -1;
    fl = JS_GetPropertyStr(e->ctx, v, "flags");
    if (JS_IsException(fl)) {
        JS_FreeValue(e->ctx, src);
        return -1;
    }
    s = JS_ToCStringLen(e->ctx, &sn, src);
    f = s ? JS_ToCStringLen(e->ctx, &fn, fl) : NULL;
    JS_FreeValue(e->ctx, src);
    JS_FreeValue(e->ctx, fl);
    if (!s || !f) {
        if (s)
            JS_FreeCString(e->ctx, s);
        return -1;
    }
    if (memchr(s, 0, sn)) {
        JS_FreeCString(e->ctx, s);
        JS_FreeCString(e->ctx, f);
        JS_ThrowTypeError(e->ctx, "BSON: a RegExp source with a NUL character cannot be encoded");
        return -1;
    }
    olen = bsonjs_regex_opts(f, fn, opts);
    dyn_bson_w_regex(e->w, key, klen, s, sn, opts, (size_t)olen);
    JS_FreeCString(e->ctx, s);
    JS_FreeCString(e->ctx, f);
    return 0;
}

static int bsonjs_encode_array(DynBsonEnc* e, JSValueConst v)
{
    JSValue* arr;
    uint32_t count, i;
    char kb[12];
    dyn_bson_w_doc_begin(e->w);
    if (JS_GetFastArray(e->ctx, v, &arr, &count)) {
        for (i = 0; i < count; i++) {
            int kl = (int)dyn_bson_u32_key(kb, i);
            if (!JS_GetFastArray(e->ctx, v, &arr, &count) || i >= count)
                break;
            if (dyn_bsonjs_encode_elem(e, kb, (size_t)kl, JS_IsUndefined(arr[i]) ? JS_NULL : arr[i]) < 0)
                return -1;
        }
    } else {
        JSValue lv = JS_GetPropertyStr(e->ctx, v, "length");
        uint32_t len;
        if (JS_IsException(lv) || JS_ToUint32(e->ctx, &len, lv)) {
            JS_FreeValue(e->ctx, lv);
            return -1;
        }
        JS_FreeValue(e->ctx, lv);
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetPropertyUint32(e->ctx, v, i);
            int kl = (int)dyn_bson_u32_key(kb, i), rc;
            if (JS_IsException(el))
                return -1;
            rc = dyn_bsonjs_encode_elem(e, kb, (size_t)kl, JS_IsUndefined(el) ? JS_NULL : el);
            JS_FreeValue(e->ctx, el);
            if (rc < 0)
                return -1;
        }
    }
    dyn_bson_w_doc_end(e->w);
    return bsonjs_oom(e);
}

static int bsonjs_key_ok(DynBsonEnc* e, const char* k, size_t n)
{
    if (memchr(k, 0, n)) {
        JS_ThrowTypeError(e->ctx, "BSON: a key with a NUL character cannot be encoded");
        return 0;
    }
    return 1;
}

static int bsonjs_encode_props(DynBsonEnc* e, JSValueConst v)
{
    JSPropertyEnum stackbuf[BSONJS_FAST_KEYS], *tab = stackbuf;
    uint32_t len = 0, k;
    int rc, owns = 0;
    rc = JS_GetOwnFastProps(e->ctx, v, stackbuf, BSONJS_FAST_KEYS, &len);
    if (rc == -2) {
        tab = malloc(len * sizeof *tab);
        if (!tab) {
            JS_ThrowOutOfMemory(e->ctx);
            return -1;
        }
        owns = 2;
        rc = JS_GetOwnFastProps(e->ctx, v, tab, len, &len);
    }
    if (rc < 0) {
        if (owns == 2)
            free(tab);
        tab = NULL;
        owns = 1;
        if (JS_GetOwnPropertyNames(e->ctx, &tab, &len, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return -1;
    }
    rc = 0;
    for (k = 0; k < len && rc == 0; k++) {
        size_t klen;
        const char* owned = NULL;
        const char* ks = JS_AtomBorrowASCII(e->ctx, &klen, tab[k].atom);
        JSValue val;
        if (!ks) {
            owned = JS_AtomToCStringLen(e->ctx, &klen, tab[k].atom);
            ks = owned;
            if (!ks) {
                rc = -1;
                break;
            }
        }
        val = JS_GetProperty(e->ctx, v, tab[k].atom);
        if (JS_IsException(val))
            rc = -1;
        else if (!JS_IsUndefined(val)) {
            if (!bsonjs_key_ok(e, ks, klen))
                rc = -1;
            else if (owned && dyn_wtf8_has_surrogate(ks, klen)) {
                char* fixed = malloc(klen ? klen : 1);
                if (!fixed) {
                    JS_ThrowOutOfMemory(e->ctx);
                    rc = -1;
                } else {
                    memcpy(fixed, ks, klen);
                    bsonjs_fix_surrogates((uint8_t*)fixed, klen);
                    rc = dyn_bsonjs_encode_elem(e, fixed, klen, val);
                    free(fixed);
                }
            } else {
                rc = dyn_bsonjs_encode_elem(e, ks, klen, val);
            }
        }
        JS_FreeValue(e->ctx, val);
        if (owned)
            JS_FreeCString(e->ctx, owned);
    }
    if (owns == 1) {
        JS_FreePropertyEnum(e->ctx, tab, len);
    } else {
        for (k = 0; k < len; k++)
            JS_FreeAtom(e->ctx, tab[k].atom);
        if (owns == 2)
            free(tab);
    }
    return rc;
}

int dyn_bsonjs_encode_doc(DynBsonEnc* e, JSValueConst obj)
{
    if (e->depth >= DYN_BSON_MAX_DEPTH) {
        JS_ThrowRangeError(e->ctx, "BSON: nesting exceeds %d levels", DYN_BSON_MAX_DEPTH);
        return -1;
    }
    e->depth++;
    dyn_bson_w_doc_begin(e->w);
    if (bsonjs_encode_props(e, obj) < 0) {
        e->depth--;
        return -1;
    }
    dyn_bson_w_doc_end(e->w);
    e->depth--;
    return bsonjs_oom(e);
}

static int bsonjs_special(DynBsonEnc* e, const char* key, size_t klen, JSValueConst v, JSClassID cid)
{
    const uint8_t* p;
    size_t n;
    uint8_t sub = 0;
    const char* s2 = NULL;
    int which;
    for (which = 0; which < DYN_BSONJS_NCLASSES; which++)
        if (cid == e->k->cls[which])
            break;
    if (which == DYN_BSONJS_NCLASSES || !dyn_bsonjs_payload(v, which, &p, &n, &sub, &s2))
        return 0;
    switch (which) {
    case DYN_BSONJS_OBJECTID:
        dyn_bson_w_oid(e->w, key, klen, p);
        break;
    case DYN_BSONJS_TIMESTAMP:
        dyn_bson_w_timestamp(e->w, key, klen, dyn_bson_rd32(p + 4), dyn_bson_rd32(p));
        break;
    case DYN_BSONJS_DECIMAL128:
        dyn_bson_w_dec128(e->w, key, klen, p);
        break;
    case DYN_BSONJS_BINARY:
        dyn_bson_w_binary(e->w, key, klen, sub, p, n);
        break;
    case DYN_BSONJS_REGEXP:
        dyn_bson_w_regex(e->w, key, klen, (const char*)p, n, s2, strlen(s2));
        break;
    case DYN_BSONJS_MINKEY:
        dyn_bson_w_key(e->w, DYN_BSON_MINKEY, key, klen);
        break;
    case DYN_BSONJS_MAXKEY:
        dyn_bson_w_key(e->w, DYN_BSON_MAXKEY, key, klen);
        break;
    case DYN_BSONJS_CODE:
        dyn_bson_w_key(e->w, DYN_BSON_CODE, key, klen);
        if (dyn_bson_w_reserve(e->w, n + 5)) {
            dyn_bson_wr32(e->w->p + e->w->n, (uint32_t)n + 1);
            e->w->n += 4;
            dyn_bson_w_bytes(e->w, p, n + 1);
        }
        break;
    }
    return bsonjs_oom(e) < 0 ? -1 : 1;
}

int dyn_bsonjs_encode_elem(DynBsonEnc* e, const char* key, size_t klen, JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT) {
        dyn_bson_w_int32(e->w, key, klen, JS_VALUE_GET_INT(v));
        return bsonjs_oom(e);
    }
    if (JS_TAG_IS_FLOAT64(tag)) {
        double d = JS_VALUE_GET_FLOAT64(v);
        if (d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d && !(d == 0 && signbit(d)))
            dyn_bson_w_int32(e->w, key, klen, (int32_t)d);
        else
            dyn_bson_w_double(e->w, key, klen, d);
        return bsonjs_oom(e);
    }
    if (JS_IsBool(v)) {
        dyn_bson_w_bool(e->w, key, klen, JS_VALUE_GET_BOOL(v));
        return bsonjs_oom(e);
    }
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        dyn_bson_w_null(e->w, key, klen);
        return bsonjs_oom(e);
    }
    if (JS_IsString(v))
        return bsonjs_put_string(e, key, klen, v, DYN_BSON_STRING) < 0 ? -1 : bsonjs_oom(e);
    if (JS_IsBigInt(e->ctx, v))
        return bsonjs_put_int64(e, key, klen, v) < 0 ? -1 : bsonjs_oom(e);
    if (JS_IsObject(v)) {
        JSClassID cid = JS_GetClassID(v);
        int rc;
        const uint8_t* p;
        size_t n;
        JSValue hold;
        if (JS_IsFunction(e->ctx, v)) {
            JS_ThrowTypeError(e->ctx, "BSON: a function cannot be encoded (key \"%.*s\")", (int)(klen > 64 ? 64 : klen), key);
            return -1;
        }
        rc = bsonjs_special(e, key, klen, v, cid);
        if (rc)
            return rc < 0 ? -1 : 0;
        if (cid == e->k->date_cls) {
            double d;
            if (JS_ToFloat64(e->ctx, &d, v))
                return -1;
            if (!isfinite(d)) {
                JS_ThrowRangeError(e->ctx, "BSON: an invalid Date cannot be encoded");
                return -1;
            }
            dyn_bson_w_date(e->w, key, klen, (int64_t)d);
            return bsonjs_oom(e);
        }
        if (cid == e->k->regexp_cls)
            return bsonjs_put_regexp(e, key, klen, v) < 0 ? -1 : bsonjs_oom(e);
        if (JS_GetBufferKind(v) != JS_BUFFER_KIND_NONE) {
            size_t off, len, bpe, ab;
            uint8_t* base;
            if (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER) {
                hold = JS_UNDEFINED;
                base = JS_GetArrayBuffer(e->ctx, &ab, v);
                if (!base)
                    return -1;
                p = base;
                n = ab;
            } else {
                hold = JS_GetArrayBufferView(e->ctx, v, &off, &len, &bpe);
                if (JS_IsException(hold))
                    return -1;
                base = JS_GetArrayBuffer(e->ctx, &ab, hold);
                if (!base || off > ab || len > ab - off) {
                    JS_FreeValue(e->ctx, hold);
                    if (base)
                        JS_ThrowRangeError(e->ctx, "BSON: byte view out of bounds");
                    return -1;
                }
                p = base + off;
                n = len;
            }
            dyn_bson_w_binary(e->w, key, klen, 0, p, n);
            JS_FreeValue(e->ctx, hold);
            return bsonjs_oom(e);
        }
        if (e->depth >= DYN_BSON_MAX_DEPTH) {
            JS_ThrowRangeError(e->ctx, "BSON: nesting exceeds %d levels", DYN_BSON_MAX_DEPTH);
            return -1;
        }
        rc = JS_IsArray(e->ctx, v);
        if (rc < 0)
            return -1;
        e->depth++;
        dyn_bson_w_key(e->w, rc ? DYN_BSON_ARRAY : DYN_BSON_DOC, key, klen);
        if (rc) {
            rc = bsonjs_encode_array(e, v);
        } else {
            dyn_bson_w_doc_begin(e->w);
            rc = bsonjs_encode_props(e, v);
            if (rc == 0) {
                dyn_bson_w_doc_end(e->w);
                rc = bsonjs_oom(e);
            }
        }
        e->depth--;
        return rc;
    }
    JS_ThrowTypeError(e->ctx, "BSON: a %s cannot be encoded (key \"%.*s\")", JS_IsSymbol(v) ? "symbol" : "value of this type",
        (int)(klen > 64 ? 64 : klen), key);
    return -1;
}

int dyn_bsonjs_encode_value_doc(DynBsonEnc* e, JSValueConst v)
{
    JSClassID cid;
    int arr;
    if (!JS_IsObject(v) || JS_IsFunction(e->ctx, v)) {
        JS_ThrowTypeError(e->ctx, "BSON: the top-level value must be a document (an object)");
        return -1;
    }
    cid = JS_GetClassID(v);
    arr = JS_IsArray(e->ctx, v);
    if (arr < 0)
        return -1;
    if (arr || cid == e->k->date_cls || cid == e->k->regexp_cls || JS_GetBufferKind(v) != JS_BUFFER_KIND_NONE) {
        JS_ThrowTypeError(e->ctx, "BSON: the top-level value must be a document, not an array, Date, RegExp or bytes");
        return -1;
    }
    {
        int which;
        for (which = 0; which < DYN_BSONJS_NCLASSES; which++)
            if (cid == e->k->cls[which]) {
                JS_ThrowTypeError(e->ctx, "BSON: the top-level value must be a document, not a %s", "BSON scalar");
                return -1;
            }
    }
    return dyn_bsonjs_encode_doc(e, v);
}

enum { BSONJS_KC_DEPTH = 8,
    BSONJS_KC_KEYS = 16,
    BSONJS_KC_KLEN = 23 };

typedef struct {
    JSAtom a;
    uint8_t n;
    char k[BSONJS_KC_KLEN];
} bsonjs_kc_ent;

struct DynBsonKc {
    bsonjs_kc_ent e[BSONJS_KC_DEPTH][BSONJS_KC_KEYS];
};

void dyn_bsonjs_dec_init(DynBsonDec* d, JSContext* ctx, const DynBsonJsKinds* k, int flags)
{
    memset(d, 0, sizeof *d);
    d->ctx = ctx;
    d->k = k;
    d->flags = flags;
    if (flags & DYN_BSONJS_KEYCACHE)
        d->kc = calloc(1, sizeof *d->kc);
}

static JSAtom bsonjs_key_atom(DynBsonDec* d, int depth, int ki, const char* k, uint32_t kn)
{
    bsonjs_kc_ent* e;
    JSAtom a;
    if (!d->kc || depth >= BSONJS_KC_DEPTH || ki >= BSONJS_KC_KEYS || kn > BSONJS_KC_KLEN)
        return JS_NewAtomLen(d->ctx, k, kn);
    e = &d->kc->e[depth][ki];
    if (e->a != JS_ATOM_NULL && e->n == kn && memcmp(e->k, k, kn) == 0)
        return JS_DupAtom(d->ctx, e->a);
    a = JS_NewAtomLen(d->ctx, k, kn);
    if (a != JS_ATOM_NULL) {
        if (e->a != JS_ATOM_NULL)
            JS_FreeAtom(d->ctx, e->a);
        e->a = JS_DupAtom(d->ctx, a);
        e->n = (uint8_t)kn;
        memcpy(e->k, k, kn);
    }
    return a;
}

void dyn_bsonjs_dec_free(DynBsonDec* d)
{
    int i;
    for (i = 0; i < 8; i++)
        JS_FreeShapeCache(JS_GetRuntime(d->ctx), &d->tpl[i]);
    if (d->kc) {
        int j;
        for (i = 0; i < BSONJS_KC_DEPTH; i++)
            for (j = 0; j < BSONJS_KC_KEYS; j++)
                if (d->kc->e[i][j].a != JS_ATOM_NULL)
                    JS_FreeAtom(d->ctx, d->kc->e[i][j].a);
        free(d->kc);
        d->kc = NULL;
    }
}

static JSValue bsonjs_regexp_value(DynBsonDec* d, const char* pat, const char* opts)
{
    JSValue g, ctor, args[2], r;
    size_t i;
    char flags[8];
    int fn = 0;
    for (i = 0; opts[i]; i++) {
        char c = opts[i];
        if (c == 'i' || c == 'm' || c == 's' || c == 'u') {
            if (fn < 7)
                flags[fn++] = c;
        } else {
            return dyn_bsonjs_make(d->ctx, DYN_BSONJS_REGEXP, (const uint8_t*)pat, strlen(pat), 0, opts);
        }
    }
    flags[fn] = 0;
    g = JS_GetGlobalObject(d->ctx);
    ctor = JS_GetPropertyStr(d->ctx, g, "RegExp");
    JS_FreeValue(d->ctx, g);
    if (JS_IsException(ctor))
        return ctor;
    args[0] = JS_NewString(d->ctx, pat);
    args[1] = JS_NewString(d->ctx, flags);
    r = JS_CallConstructor(d->ctx, ctor, 2, (JSValueConst*)args);
    JS_FreeValue(d->ctx, args[0]);
    JS_FreeValue(d->ctx, args[1]);
    JS_FreeValue(d->ctx, ctor);
    if (JS_IsException(r)) {
        JS_FreeValue(d->ctx, JS_GetException(d->ctx));
        return dyn_bsonjs_make(d->ctx, DYN_BSONJS_REGEXP, (const uint8_t*)pat, strlen(pat), 0, opts);
    }
    return r;
}

JSValue dyn_bsonjs_decode_val(DynBsonDec* d, dyn_bson_val v)
{
    JSContext* ctx = d->ctx;
    switch (v.type) {
    case DYN_BSON_DOUBLE:
        return JS_NewFloat64(ctx, dyn_bson_rdf64(v.val));
    case DYN_BSON_STRING:
    case DYN_BSON_SYMBOL:
        return JS_NewStringLen(ctx, (const char*)v.val + 4, dyn_bson_rd32(v.val) - 1);
    case DYN_BSON_DOC:
        return dyn_bsonjs_decode_doc(d, v.val, 0);
    case DYN_BSON_ARRAY:
        return dyn_bsonjs_decode_doc(d, v.val, 1);
    case DYN_BSON_BINARY: {
        uint32_t l = dyn_bson_rd32(v.val);
        uint8_t sub = v.val[4];
        if (sub == 0)
            return dyn_nat_u8_copy(ctx, v.val + 5, l);
        if (sub == 2)
            return dyn_bsonjs_make(ctx, DYN_BSONJS_BINARY, v.val + 9, l - 4, sub, NULL);
        return dyn_bsonjs_make(ctx, DYN_BSONJS_BINARY, v.val + 5, l, sub, NULL);
    }
    case DYN_BSON_UNDEFINED:
        return JS_UNDEFINED;
    case DYN_BSON_OID:
        return dyn_bsonjs_make(ctx, DYN_BSONJS_OBJECTID, v.val, 12, 0, NULL);
    case DYN_BSON_BOOL:
        return JS_NewBool(ctx, v.val[0] != 0);
    case DYN_BSON_DATE:
        return JS_NewDate(ctx, (double)(int64_t)dyn_bson_rd64(v.val));
    case DYN_BSON_NULL:
        return JS_NULL;
    case DYN_BSON_REGEX: {
        const char* pat = (const char*)v.val;
        return bsonjs_regexp_value(d, pat, pat + strlen(pat) + 1);
    }
    case DYN_BSON_CODE:
        return dyn_bsonjs_make(ctx, DYN_BSONJS_CODE, v.val + 4, dyn_bson_rd32(v.val) - 1, 0, NULL);
    case DYN_BSON_INT32:
        return JS_NewInt32(ctx, (int32_t)dyn_bson_rd32(v.val));
    case DYN_BSON_TIMESTAMP:
        return dyn_bsonjs_make(ctx, DYN_BSONJS_TIMESTAMP, v.val, 8, 0, NULL);
    case DYN_BSON_INT64: {
        int64_t i = (int64_t)dyn_bson_rd64(v.val);
        if ((d->flags & DYN_BSONJS_PROMOTE_LONGS) && i >= -9007199254740991LL && i <= 9007199254740991LL)
            return JS_NewInt64(ctx, i);
        return JS_NewBigInt64(ctx, i);
    }
    case DYN_BSON_DEC128:
        return dyn_bsonjs_make(ctx, DYN_BSONJS_DECIMAL128, v.val, 16, 0, NULL);
    case DYN_BSON_MINKEY:
        return dyn_bsonjs_make(ctx, DYN_BSONJS_MINKEY, NULL, 0, 0, NULL);
    case DYN_BSON_MAXKEY:
        return dyn_bsonjs_make(ctx, DYN_BSONJS_MAXKEY, NULL, 0, 0, NULL);
    case DYN_BSON_DBPOINTER:
        return JS_ThrowTypeError(ctx, "BSON: the deprecated DBPointer type (0x0C) is not decoded");
    case DYN_BSON_CODE_W_SCOPE:
        return JS_ThrowTypeError(ctx, "BSON: the deprecated code-with-scope type (0x0F) is not decoded");
    }
    return JS_ThrowTypeError(ctx, "BSON: unknown element type 0x%02x", v.type);
}

static JSValue bsonjs_decode_array(DynBsonDec* d, const uint8_t* doc)
{
    dyn_bson_iter it;
    JSValue stackv[32], *vals = stackv, out;
    uint32_t n = 0, cap = 32, i;
    int rc;
    dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
    while ((rc = dyn_bson_iter_next(&it)) > 0) {
        JSValue v;
        if (n == cap) {
            JSValue* nv;
            cap *= 2;
            nv = vals == stackv ? malloc(cap * sizeof *nv) : realloc(vals, cap * sizeof *nv);
            if (!nv) {
                JS_ThrowOutOfMemory(d->ctx);
                goto fail;
            }
            if (vals == stackv)
                memcpy(nv, stackv, sizeof stackv);
            vals = nv;
        }
        v = dyn_bsonjs_decode_val(d, dyn_bson_iter_val(&it));
        if (JS_IsException(v))
            goto fail;
        vals[n++] = v;
    }
    if (rc < 0) {
        JS_ThrowTypeError(d->ctx, "BSON: %s", dyn_bson_strerror(rc));
        goto fail;
    }
    out = JS_NewArrayFrom(d->ctx, (int)n, vals);
    if (vals != stackv)
        free(vals);
    return out;
fail:
    for (i = 0; i < n; i++)
        JS_FreeValue(d->ctx, vals[i]);
    if (vals != stackv)
        free(vals);
    return JS_EXCEPTION;
}

JSValue dyn_bsonjs_decode_doc(DynBsonDec* d, const uint8_t* doc, int is_array)
{
    dyn_bson_iter it;
    JSAtom atoms[BSONJS_TPL_MAX];
    JSValue vals[BSONJS_TPL_MAX], out = JS_UNDEFINED;
    int n = 0, rc, depth = d->depth, i, ki = 0;
    if (d->depth >= DYN_BSON_MAX_DEPTH + 2)
        return JS_ThrowRangeError(d->ctx, "BSON: nesting exceeds %d levels", DYN_BSON_MAX_DEPTH);
    d->depth++;
    if (is_array) {
        out = bsonjs_decode_array(d, doc);
        d->depth--;
        return out;
    }
    dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
    while ((rc = dyn_bson_iter_next(&it)) > 0) {
        JSAtom a;
        JSValue v;
        if (n == BSONJS_TPL_MAX && JS_IsUndefined(out)) {
            out = JS_NewObject(d->ctx);
            if (JS_IsException(out))
                goto fail;
            for (i = 0; i < n; i++)
                if (JS_DefinePropertyValue(d->ctx, out, atoms[i], vals[i], JS_PROP_C_W_E) < 0) {
                    for (; i < n; i++)
                        JS_FreeAtom(d->ctx, atoms[i]);
                    n = 0;
                    goto fail;
                } else {
                    JS_FreeAtom(d->ctx, atoms[i]);
                }
            n = 0;
        }
        a = bsonjs_key_atom(d, depth, ki++, it.key, it.klen);
        if (a == JS_ATOM_NULL)
            goto fail;
        v = dyn_bsonjs_decode_val(d, dyn_bson_iter_val(&it));
        if (JS_IsException(v)) {
            JS_FreeAtom(d->ctx, a);
            goto fail;
        }
        if (!JS_IsUndefined(out)) {
            int r = JS_DefinePropertyValue(d->ctx, out, a, v, JS_PROP_C_W_E);
            JS_FreeAtom(d->ctx, a);
            if (r < 0)
                goto fail;
        } else {
            atoms[n] = a;
            vals[n] = v;
            n++;
        }
    }
    if (rc < 0) {
        JS_ThrowTypeError(d->ctx, "BSON: %s", dyn_bson_strerror(rc));
        goto fail;
    }
    d->depth--;
    if (!JS_IsUndefined(out))
        return out;
    return JS_NewObjectFromProps(d->ctx, depth < 8 ? &d->tpl[depth] : NULL, n, atoms, vals);
fail:
    d->depth--;
    for (i = 0; i < n; i++) {
        JS_FreeAtom(d->ctx, atoms[i]);
        JS_FreeValue(d->ctx, vals[i]);
    }
    JS_FreeValue(d->ctx, out);
    return JS_EXCEPTION;
}

static int bsonjs_bytes_arg(JSContext* ctx, JSValueConst v, const uint8_t** pp, size_t* pn, JSValue* hold)
{
    size_t off, len, bpe, ab;
    uint8_t* base;
    int kind = JS_GetBufferKind(v);
    *hold = JS_UNDEFINED;
    if (kind == JS_BUFFER_KIND_BUFFER) {
        base = JS_GetArrayBuffer(ctx, &ab, v);
        if (!base)
            return -1;
        *pp = base;
        *pn = ab;
        return 0;
    }
    if (kind != JS_BUFFER_KIND_VIEW) {
        JS_ThrowTypeError(ctx, "BSONDecode(bytes): expected a Uint8Array, ArrayBuffer or byte view");
        return -1;
    }
    *hold = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(*hold))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, *hold);
    if (!base || off > ab || len > ab - off) {
        JS_FreeValue(ctx, *hold);
        if (base)
            JS_ThrowRangeError(ctx, "BSONDecode: byte view out of bounds");
        return -1;
    }
    *pp = base + off;
    *pn = len;
    return 0;
}

JSValue dyn_bsonjs_encode_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_bson_w w;
    DynBsonEnc e;
    JSValue out;
    const DynBsonJsKinds* k;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "BSONEncode(document): a document is required");
    if (dyn_bsonjs_ensure(ctx, &k) < 0)
        return JS_EXCEPTION;
    dyn_bson_w_init(&w);
    memset(&e, 0, sizeof e);
    e.ctx = ctx;
    e.w = &w;
    e.k = k;
    if (dyn_bsonjs_encode_value_doc(&e, argv[0]) < 0) {
        dyn_bson_w_free(&w);
        return JS_EXCEPTION;
    }
    out = dyn_nat_u8_copy(ctx, w.p, w.n);
    dyn_bson_w_free(&w);
    return out;
}

JSValue dyn_bsonjs_decode_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const uint8_t* p;
    size_t n;
    JSValue hold, out;
    dyn_bson_err err;
    DynBsonDec d;
    const DynBsonJsKinds* k;
    uint8_t* copy;
    int rc;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "BSONDecode(bytes): bytes are required");
    if (dyn_bsonjs_ensure(ctx, &k) < 0)
        return JS_EXCEPTION;
    if (bsonjs_bytes_arg(ctx, argv[0], &p, &n, &hold) < 0)
        return JS_EXCEPTION;
    if (n > DYN_BSON_MAX_SIZE) {
        JS_FreeValue(ctx, hold);
        return JS_ThrowRangeError(ctx, "BSONDecode: input exceeds the 2 GiB format limit");
    }
    rc = dyn_bson_validate(p, n, DYN_BSON_MAX_DEPTH, &err);
    if (rc < 0) {
        JS_FreeValue(ctx, hold);
        if (rc == DYN_BSON_E_DEPTH)
            return JS_ThrowRangeError(ctx, "BSONDecode: nesting exceeds %d levels", DYN_BSON_MAX_DEPTH);
        return JS_ThrowTypeError(ctx, "BSONDecode: %s at byte %u", dyn_bson_strerror(err.code), (unsigned)err.offset);
    }
    copy = malloc(n);
    if (!copy) {
        JS_FreeValue(ctx, hold);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(copy, p, n);
    JS_FreeValue(ctx, hold);
    dyn_bsonjs_dec_init(&d, ctx, k, 0);
    out = dyn_bsonjs_decode_doc(&d, copy, 0);
    dyn_bsonjs_dec_free(&d);
    free(copy);
    return out;
}

#endif
