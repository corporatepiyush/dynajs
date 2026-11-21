#include "vserialize-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && (defined(CONFIG_NATIVE_MODULE_VSERIALIZE) || defined(CONFIG_NATIVE_MODULE_SCHEMA))

__attribute__((noinline)) int vb_grow(vb_t* b, size_t n)
{
    if (b->oom)
        return 0;
    if (!dyn_sb_reserve((void**)&b->p, &b->cap, b->n + n, 128)) {
        b->oom = 1;
        return 0;
    }
    return 1;
}

static inline int vb_room(vb_t* b, size_t n)
{
    return likely(b->n + n <= b->cap && !b->oom) || vb_grow(b, n);
}

static inline void vb_write(vb_t* b, const void* p, size_t n)
{
    if (!n || !vb_room(b, n))
        return;
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static inline void vb_put(vb_t* b, uint8_t c)
{
    if (vb_room(b, 1))
        b->p[b->n++] = c;
}

static inline void vb_be(vb_t* b, uint64_t v, int bytes)
{
    int i;
    if (!vb_room(b, (size_t)bytes))
        return;
    for (i = 0; i < bytes; i++)
        b->p[b->n + (size_t)i] = (uint8_t)(v >> ((bytes - 1 - i) * 8));
    b->n += (size_t)bytes;
}

typedef struct {
    JSAtom atom;
    const char* s;
    size_t len;
    uint32_t idx;
    int owned;
} venc_key_t;

#define VENC_FAST_KEYS 128

static void cbor_head(vb_t* b, int major, uint64_t n)
{
    int m = major << 5;
    if (n < 24)
        vb_put(b, (uint8_t)(m | n));
    else if (n <= 0xFF) {
        vb_put(b, (uint8_t)(m | 24));
        vb_be(b, n, 1);
    } else if (n <= 0xFFFF) {
        vb_put(b, (uint8_t)(m | 25));
        vb_be(b, n, 2);
    } else if (n <= 0xFFFFFFFF) {
        vb_put(b, (uint8_t)(m | 26));
        vb_be(b, n, 4);
    } else {
        vb_put(b, (uint8_t)(m | 27));
        vb_be(b, n, 8);
    }
}

static void venc_int(venc_t* e, int64_t v)
{
    if (e->fmt == FMT_CBOR) {
        if (v < 0)
            cbor_head(&e->b, 1, (uint64_t)(-(v + 1)));
        else
            cbor_head(&e->b, 0, (uint64_t)v);
        return;
    }
    if (v >= 0 && v < 128) {
        vb_put(&e->b, (uint8_t)v);
        return;
    }
    if (v < 0 && v >= -32) {
        vb_put(&e->b, (uint8_t)(0xE0 | (v + 32)));
        return;
    }
    if (v >= 0) {
        if (v <= 0xFF) {
            vb_put(&e->b, 0xCC);
            vb_be(&e->b, (uint64_t)v, 1);
        } else if (v <= 0xFFFF) {
            vb_put(&e->b, 0xCD);
            vb_be(&e->b, (uint64_t)v, 2);
        } else if (v <= 0xFFFFFFFF) {
            vb_put(&e->b, 0xCE);
            vb_be(&e->b, (uint64_t)v, 4);
        } else {
            vb_put(&e->b, 0xCF);
            vb_be(&e->b, (uint64_t)v, 8);
        }
        return;
    }
    if (v >= -128) {
        vb_put(&e->b, 0xD0);
        vb_be(&e->b, (uint64_t)(uint8_t)v, 1);
    } else if (v >= -32768) {
        vb_put(&e->b, 0xD1);
        vb_be(&e->b, (uint64_t)(uint16_t)v, 2);
    } else if (v >= -2147483648LL) {
        vb_put(&e->b, 0xD2);
        vb_be(&e->b, (uint64_t)(uint32_t)v, 4);
    } else {
        vb_put(&e->b, 0xD3);
        vb_be(&e->b, (uint64_t)v, 8);
    }
}

static void venc_len(venc_t* e, int kind, uint64_t n)
{
    if (e->fmt == FMT_CBOR) {
        static const int MAJ[4] = { 3, 2, 4, 5 };
        cbor_head(&e->b, MAJ[kind], n);
        return;
    }
    switch (kind) {
    case 0:
        if (n < 32)
            vb_put(&e->b, (uint8_t)(0xA0 | n));
        else if (n <= 0xFF) {
            vb_put(&e->b, 0xD9);
            vb_be(&e->b, n, 1);
        } else if (n <= 0xFFFF) {
            vb_put(&e->b, 0xDA);
            vb_be(&e->b, n, 2);
        } else {
            vb_put(&e->b, 0xDB);
            vb_be(&e->b, n, 4);
        }
        break;
    case 1:
        if (n <= 0xFF) {
            vb_put(&e->b, 0xC4);
            vb_be(&e->b, n, 1);
        } else if (n <= 0xFFFF) {
            vb_put(&e->b, 0xC5);
            vb_be(&e->b, n, 2);
        } else {
            vb_put(&e->b, 0xC6);
            vb_be(&e->b, n, 4);
        }
        break;
    case 2:
        if (n < 16)
            vb_put(&e->b, (uint8_t)(0x90 | n));
        else if (n <= 0xFFFF) {
            vb_put(&e->b, 0xDC);
            vb_be(&e->b, n, 2);
        } else {
            vb_put(&e->b, 0xDD);
            vb_be(&e->b, n, 4);
        }
        break;
    default:
        if (n < 16)
            vb_put(&e->b, (uint8_t)(0x80 | n));
        else if (n <= 0xFFFF) {
            vb_put(&e->b, 0xDE);
            vb_be(&e->b, n, 2);
        } else {
            vb_put(&e->b, 0xDF);
            vb_be(&e->b, n, 4);
        }
        break;
    }
}

static int venc_push(venc_t* e, JSValueConst v)
{
    int i;
    for (i = 0; i < e->nseen; i++)
        if (JS_VALUE_GET_PTR(e->seen[i]) == JS_VALUE_GET_PTR(v)) {
            JS_ThrowTypeError(e->ctx, "encode: the value contains a cycle, and "
                                      "neither wire format can express one");
            return -1;
        }
    if (e->nseen == e->cseen) {
        int nc = e->cseen ? e->cseen * 2 : 16;
        JSValue* np = (JSValue*)realloc(e->seen, (size_t)nc * sizeof *np);
        if (!np) {
            JS_ThrowOutOfMemory(e->ctx);
            return -1;
        }
        e->seen = np;
        e->cseen = nc;
    }
    e->seen[e->nseen++] = (JSValue)v;
    return 0;
}

static int venc_bytes_of(JSContext* ctx, JSValueConst v, const uint8_t** pp,
    size_t* pn, JSValue* hold)
{
    size_t off, len, bpe, ab;
    uint8_t* base;
    JSValue buf;

    if (JS_GetBufferKind(v) != JS_BUFFER_KIND_VIEW)
        return 0;
    buf = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buf);
        return 0;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    if (!base || off > ab || len > ab - off) {
        JS_FreeValue(ctx, buf);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    *hold = buf;
    *pp = base + off;
    *pn = len;
    return 1;
}

static int venc_string(venc_t* e, JSValueConst v)
{
    size_t n;
    const char* s = JS_ToCStringLenUTF8(e->ctx, &n, v);
    if (!s)
        return -1;
    venc_len(e, 0, n);
    vb_write(&e->b, s, n);
    JS_FreeCString(e->ctx, s);
    return 0;
}

static int venc_array(venc_t* e, JSValueConst v)
{
    JSValue lv = JS_GetPropertyStr(e->ctx, v, "length");
    int64_t len = 0, i;

    if (JS_IsException(lv) || JS_ToInt64(e->ctx, &len, lv) < 0) {
        JS_FreeValue(e->ctx, lv);
        return -1;
    }
    JS_FreeValue(e->ctx, lv);
    if (len < 0 || len > VS_MAX_ITEMS) {
        JS_ThrowRangeError(e->ctx, "encode: array too long");
        return -1;
    }
    venc_len(e, 2, (uint64_t)len);
    for (i = 0; i < len; i++) {
        JSValue el = JS_GetPropertyUint32(e->ctx, v, (uint32_t)i);
        int rc;
        if (JS_IsException(el))
            return -1;
        rc = venc_value(e, el);
        JS_FreeValue(e->ctx, el);
        if (rc < 0)
            return -1;
    }
    return 0;
}

static int venc_key_cmp(const void* pa, const void* pb)
{
    const venc_key_t* a = (const venc_key_t*)pa;
    const venc_key_t* b = (const venc_key_t*)pb;
    int c;

    if (a->len != b->len)
        return a->len < b->len ? -1 : 1;
    c = a->len ? memcmp(a->s, b->s, a->len) : 0;
    if (c != 0)
        return c;
    return a->idx < b->idx ? -1 : (a->idx > b->idx);
}

static int venc_key_bytes(JSContext* ctx, JSAtom atom, uint32_t idx,
    venc_key_t* k)
{
    k->atom = atom;
    k->idx = idx;
    k->owned = 0;
    k->len = 0;
    k->s = JS_AtomBorrowASCII(ctx, &k->len, atom);
    if (k->s)
        return 0;
    k->s = JS_AtomToCStringLen(ctx, &k->len, atom);
    if (!k->s)
        return -1;
    k->owned = 1;
    return 0;
}

static void venc_keys_free(JSContext* ctx, venc_key_t* ks, uint32_t built)
{
    uint32_t i;
    for (i = 0; i < built; i++)
        if (ks[i].owned)
            JS_FreeCString(ctx, ks[i].s);
    free(ks);
}

static int venc_kv(venc_t* e, JSValueConst v, const char* ks, size_t klen,
    JSAtom atom)
{
    JSValue val;
    int rc;

    venc_len(e, 0, klen);
    vb_write(&e->b, ks, klen);
    val = JS_GetProperty(e->ctx, v, atom);
    if (JS_IsException(val))
        return -1;
    rc = venc_value(e, val);
    JS_FreeValue(e->ctx, val);
    return rc;
}

static int venc_map_sorted(venc_t* e, JSValueConst v, JSPropertyEnum* tab,
    uint32_t len)
{
    venc_key_t* keys;
    uint32_t built = 0, k;
    int rc = 0;

    keys = (venc_key_t*)malloc((len ? len : 1) * sizeof *keys);
    if (!keys) {
        JS_ThrowOutOfMemory(e->ctx);
        return -1;
    }
    for (built = 0; built < len; built++)
        if (venc_key_bytes(e->ctx, tab[built].atom, built, &keys[built]) < 0) {
            rc = -1;
            break;
        }
    if (rc == 0) {
        qsort(keys, len, sizeof *keys, venc_key_cmp);
        venc_len(e, 3, len);
        for (k = 0; k < len && rc == 0; k++)
            rc = venc_kv(e, v, keys[k].s, keys[k].len, keys[k].atom);
    }
    venc_keys_free(e->ctx, keys, built);
    return rc;
}

static int venc_map_plain(venc_t* e, JSValueConst v, JSPropertyEnum* tab,
    uint32_t len)
{
    uint32_t k;
    int rc = 0;

    venc_len(e, 3, len);
    for (k = 0; k < len && rc == 0; k++) {
        size_t klen;
        const char* owned = NULL;
        const char* ks = JS_AtomBorrowASCII(e->ctx, &klen, tab[k].atom);
        if (!ks) {
            owned = JS_AtomToCStringLen(e->ctx, &klen, tab[k].atom);
            ks = owned;
        }
        if (!ks)
            return -1;
        rc = venc_kv(e, v, ks, klen, tab[k].atom);
        if (owned)
            JS_FreeCString(e->ctx, owned);
    }
    return rc;
}

static int venc_object(venc_t* e, JSValueConst v)
{
    JSPropertyEnum stackbuf[VENC_FAST_KEYS], *tab = stackbuf;
    uint32_t len = 0, k;
    int rc = 0, owns_tab = 0;

    rc = JS_GetOwnFastProps(e->ctx, v, stackbuf, VENC_FAST_KEYS, &len);
    if (rc == -2) {
        tab = (JSPropertyEnum*)malloc(len * sizeof *tab);
        if (!tab)
            return -1;
        owns_tab = 2;
        rc = JS_GetOwnFastProps(e->ctx, v, tab, len, &len);
    }
    if (rc < 0) {
        if (owns_tab == 2)
            free(tab);
        tab = NULL;
        owns_tab = 1;
        if (JS_GetOwnPropertyNames(e->ctx, &tab, &len, v,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            return -1;
    }
    rc = e->sorted ? venc_map_sorted(e, v, tab, len)
                   : venc_map_plain(e, v, tab, len);
    if (owns_tab == 1) {
        JS_FreePropertyEnum(e->ctx, tab, len);
    } else {
        for (k = 0; k < len; k++)
            JS_FreeAtom(e->ctx, tab[k].atom);
        if (owns_tab == 2)
            free(tab);
    }
    return rc;
}

static int venc_hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static int venc_cbor_bignum(venc_t* e, JSValueConst v)
{
    JSValue fn, radix, sv;
    const char* s;
    size_t n, i, k, nb;
    int neg, h, rc = -1;
    uint8_t* m;
    fn = JS_GetPropertyStr(e->ctx, v, "toString");
    if (JS_IsException(fn))
        return -1;
    radix = JS_NewInt32(e->ctx, 16);
    sv = JS_Call(e->ctx, fn, v, 1, (JSValueConst*)&radix);
    JS_FreeValue(e->ctx, fn);
    if (JS_IsException(sv))
        return -1;
    s = JS_ToCStringLen(e->ctx, &n, sv);
    JS_FreeValue(e->ctx, sv);
    if (!s)
        return -1;
    neg = n && s[0] == '-';
    i = (size_t)neg;
    nb = (n - i + 1) / 2;
    if (nb > VS_BIGNUM_MAX_BYTES) {
        JS_FreeCString(e->ctx, s);
        JS_ThrowRangeError(e->ctx, "encode: BigInt over %d bytes", VS_BIGNUM_MAX_BYTES);
        return -1;
    }
    m = (uint8_t*)js_malloc(e->ctx, nb ? nb : 1);
    if (!m) {
        JS_FreeCString(e->ctx, s);
        return -1;
    }
    memset(m, 0, nb ? nb : 1);
    for (k = 0; i < n; i++, k++) {
        size_t pos = (n - 1 - i);
        h = venc_hexval((unsigned char)s[i]);
        if (h < 0)
            goto out;
        m[nb - 1 - pos / 2] |= (uint8_t)(pos & 1 ? h << 4 : h);
    }
    if (neg) {
        for (k = nb; k-- > 0;) {
            if (m[k]--)
                break;
        }
    }
    for (k = 0; k < nb && !m[k]; k++)
        ;
    if (nb - k <= 8) {
        uint64_t u = 0;
        for (; k < nb; k++)
            u = (u << 8) | m[k];
        cbor_head(&e->b, neg, u);
    } else {
        cbor_head(&e->b, 6, neg ? 3 : 2);
        cbor_head(&e->b, 2, (uint64_t)(nb - k));
        vb_write(&e->b, m + k, nb - k);
    }
    rc = 0;
out:
    if (rc < 0)
        JS_ThrowTypeError(e->ctx, "encode: BigInt did not stringify to hex");
    js_free(e->ctx, m);
    JS_FreeCString(e->ctx, s);
    return rc;
}

static int venc_bigint(venc_t* e, JSValueConst v)
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
    if (same) {
        venc_int(e, i);
        return 0;
    }
    if (e->fmt == FMT_CBOR)
        return venc_cbor_bignum(e, v);
    back = JS_NewBigUint64(e->ctx, (uint64_t)i);
    if (JS_IsException(back))
        return -1;
    same = JS_StrictEq(e->ctx, back, v);
    JS_FreeValue(e->ctx, back);
    if (!same) {
        JS_ThrowRangeError(e->ctx, "encode: BigInt outside the int64/uint64 range");
        return -1;
    }
    vb_put(&e->b, 0xCF);
    vb_be(&e->b, (uint64_t)i, 8);
    return 0;
}

int venc_value(venc_t* e, JSValueConst v)
{
    if (e->depth >= VS_MAX_DEPTH) {
        JS_ThrowRangeError(e->ctx, "encode: nesting exceeds %d", VS_MAX_DEPTH);
        return -1;
    }
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        vb_put(&e->b, e->fmt == FMT_CBOR ? (uint8_t)(JS_IsNull(v) ? 0xF6 : 0xF7) : 0xC0);
        return 0;
    }
    if (JS_IsBool(v)) {
        int t = JS_ToBool(e->ctx, v);
        vb_put(&e->b, e->fmt == FMT_CBOR ? (uint8_t)(t ? 0xF5 : 0xF4) : (uint8_t)(t ? 0xC3 : 0xC2));
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d;
        int64_t iv;
        if (JS_ToFloat64(e->ctx, &d, v) < 0)
            return -1;
        if (d == floor(d) && !isinf(d) && !(d == 0 && signbit(d))) {
            double lim = e->bigint ? 9007199254740992.0 : 9223372036854775808.0;
            if (d >= -lim + (e->bigint ? 1.0 : 0.0) && d < lim) {
                iv = (int64_t)d;
                venc_int(e, iv);
                return 0;
            }
            if (!e->bigint && d >= 9223372036854775808.0 && d < 18446744073709551616.0) {
                uint64_t u = (uint64_t)d;
                if (e->fmt == FMT_CBOR) {
                    cbor_head(&e->b, 0, u);
                } else {
                    vb_put(&e->b, 0xCF);
                    vb_be(&e->b, u, 8);
                }
                return 0;
            }
        }
        {
            uint64_t bits;
            memcpy(&bits, &d, 8);
            vb_put(&e->b, e->fmt == FMT_CBOR ? 0xFB : 0xCB);
            vb_be(&e->b, bits, 8);
        }
        return 0;
    }
    if (JS_IsString(v))
        return venc_string(e, v);
    if (JS_IsBigInt(e->ctx, v))
        return venc_bigint(e, v);
    if (!JS_IsObject(v) || JS_IsFunction(e->ctx, v)) {
        JS_ThrowTypeError(e->ctx, "encode: a symbol or function has no encoding");
        return -1;
    }
    {
        const uint8_t* p;
        size_t n;
        JSValue hold;
        if (venc_bytes_of(e->ctx, v, &p, &n, &hold)) {
            venc_len(e, 1, n);
            vb_write(&e->b, p, n);
            JS_FreeValue(e->ctx, hold);
            return 0;
        }
    }
    if (venc_push(e, v) < 0)
        return -1;
    e->depth++;
    {
        int rc = JS_IsArray(e->ctx, v) == 1 ? venc_array(e, v) : venc_object(e, v);
        e->depth--;
        e->nseen--;
        return rc;
    }
}

int dyn_value_hash64(JSContext* ctx, JSValueConst v, uint64_t* out)
{
    venc_t e;
    memset(&e, 0, sizeof e);
    e.ctx = ctx;
    e.fmt = FMT_CBOR;
    e.sorted = 1;
    if (venc_value(&e, v) < 0 || e.b.oom) {
        if (e.b.oom)
            JS_ThrowOutOfMemory(ctx);
        free(e.b.p);
        free(e.seen);
        return -1;
    }
    free(e.seen);
    *out = dyn_xxh64(e.b.p, e.b.n, 0);
    free(e.b.p);
    return 0;
}

#endif
