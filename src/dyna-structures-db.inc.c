#include "core/dyn-hash.h"

#define SDB_FIXED_SEED 0x5DB1E995C3A5C85CULL
#define SDB_MAX_ITEMS (1u << 28)

static _Atomic JSClassID sdb_ta_cid[JS_TYPED_ARRAY_FLOAT64 + 1];

static void sdb_capture_typed_cids(JSContext* ctx)
{
    int t;
    for (t = 0; t <= JS_TYPED_ARRAY_FLOAT64; t++) {
        JSValueConst args[1];
        JSValue ta;
        args[0] = JS_NewInt32(ctx, 0);
        ta = JS_NewTypedArray(ctx, 1, args, (JSTypedArrayEnum)t);
        if (JS_IsException(ta)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        atomic_store_explicit(&sdb_ta_cid[t], JS_GetClassID(ta), memory_order_relaxed);
        JS_FreeValue(ctx, ta);
    }
}

static int sdb_typed_kind(JSValueConst v)
{
    JSClassID cid;
    int t;
    if (!JS_IsObject(v))
        return -1;
    cid = JS_GetClassID(v);
    for (t = 0; t <= JS_TYPED_ARRAY_FLOAT64; t++)
        if (cid == atomic_load_explicit(&sdb_ta_cid[t], memory_order_relaxed))
            return t;
    return -1;
}

static const uint8_t* sdb_typed_bytes(JSContext* ctx, JSValueConst v, size_t* plen)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return NULL;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base) {
        JS_ThrowTypeError(ctx, "typed array buffer is detached");
        return NULL;
    }
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return NULL;
    }
    *plen = len;
    return base + off;
}

static double sdb_typed_at(const uint8_t* p, int kind, size_t i)
{
    switch (kind) {
    case JS_TYPED_ARRAY_INT8: {
        int8_t x;
        memcpy(&x, p + i, 1);
        return x;
    }
    case JS_TYPED_ARRAY_UINT8:
    case JS_TYPED_ARRAY_UINT8C:
        return p[i];
    case JS_TYPED_ARRAY_INT16: {
        int16_t x;
        memcpy(&x, p + i * 2, 2);
        return x;
    }
    case JS_TYPED_ARRAY_UINT16: {
        uint16_t x;
        memcpy(&x, p + i * 2, 2);
        return x;
    }
    case JS_TYPED_ARRAY_INT32: {
        int32_t x;
        memcpy(&x, p + i * 4, 4);
        return x;
    }
    case JS_TYPED_ARRAY_UINT32: {
        uint32_t x;
        memcpy(&x, p + i * 4, 4);
        return x;
    }
    case JS_TYPED_ARRAY_FLOAT32: {
        float x;
        memcpy(&x, p + i * 4, 4);
        return (double)x;
    }
    case JS_TYPED_ARRAY_FLOAT64: {
        double x;
        memcpy(&x, p + i * 8, 8);
        return x;
    }
    default:
        return (double)NAN;
    }
}

static size_t sdb_typed_bpe(int kind)
{
    switch (kind) {
    case JS_TYPED_ARRAY_INT8:
    case JS_TYPED_ARRAY_UINT8:
    case JS_TYPED_ARRAY_UINT8C:
        return 1;
    case JS_TYPED_ARRAY_INT16:
    case JS_TYPED_ARRAY_UINT16:
    case JS_TYPED_ARRAY_FLOAT16:
        return 2;
    case JS_TYPED_ARRAY_FLOAT64:
    case JS_TYPED_ARRAY_BIG_INT64:
    case JS_TYPED_ARRAY_BIG_UINT64:
        return 8;
    default:
        return 4;
    }
}

static int sdb_f64_list(JSContext* ctx, JSValueConst v, const char* what,
    double** out, uint32_t* pn)
{
    int kind = sdb_typed_kind(v);
    double* d;
    uint32_t n, i;

    *out = NULL;
    *pn = 0;
    if (kind >= 0) {
        size_t len, bpe = sdb_typed_bpe(kind);
        const uint8_t* p;
        if (kind == JS_TYPED_ARRAY_BIG_INT64 || kind == JS_TYPED_ARRAY_BIG_UINT64 || kind == JS_TYPED_ARRAY_FLOAT16) {
            JS_ThrowTypeError(ctx, "%s: BigInt and Float16 arrays are not accepted", what);
            return -1;
        }
        p = sdb_typed_bytes(ctx, v, &len);
        if (!p)
            return -1;
        if (len / bpe > SDB_MAX_ITEMS) {
            JS_ThrowRangeError(ctx, "%s: more than %u values", what, SDB_MAX_ITEMS);
            return -1;
        }
        n = (uint32_t)(len / bpe);
        d = (double*)dyn_nat_malloc(sizeof(double) * ((size_t)n + 1));
        if (!d) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (i = 0; i < n; i++)
            d[i] = sdb_typed_at(p, kind, i);
        *out = d;
        *pn = n;
        return 0;
    }
    if (JS_IsArray(ctx, v) > 0) {
        JSValue* arr;
        uint32_t cnt;
        if (JS_GetFastArray(ctx, v, &arr, &cnt)) {
            if (cnt > SDB_MAX_ITEMS) {
                JS_ThrowRangeError(ctx, "%s: more than %u values", what, SDB_MAX_ITEMS);
                return -1;
            }
            d = (double*)dyn_nat_malloc(sizeof(double) * ((size_t)cnt + 1));
            if (!d) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            for (i = 0; i < cnt; i++) {
                int tag = JS_VALUE_GET_TAG(arr[i]);
                if (tag == JS_TAG_INT)
                    d[i] = JS_VALUE_GET_INT(arr[i]);
                else if (JS_TAG_IS_FLOAT64(tag))
                    d[i] = JS_VALUE_GET_FLOAT64(arr[i]);
                else {
                    dyn_nat_free(d);
                    JS_ThrowTypeError(ctx, "%s: element %u is not a number", what, (unsigned)i);
                    return -1;
                }
            }
            *out = d;
            *pn = cnt;
            return 0;
        } else {
            JSValue lv = JS_GetPropertyStr(ctx, v, "length");
            uint32_t len;
            if (JS_ToUint32(ctx, &len, lv)) {
                JS_FreeValue(ctx, lv);
                return -1;
            }
            JS_FreeValue(ctx, lv);
            if (len > SDB_MAX_ITEMS) {
                JS_ThrowRangeError(ctx, "%s: more than %u values", what, SDB_MAX_ITEMS);
                return -1;
            }
            d = (double*)dyn_nat_malloc(sizeof(double) * 16);
            if (!d) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            n = 0;
            for (i = 0; i < len; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, v, i);
                if (JS_IsException(e)) {
                    dyn_nat_free(d);
                    return -1;
                }
                if (!JS_IsNumber(e)) {
                    JS_FreeValue(ctx, e);
                    dyn_nat_free(d);
                    JS_ThrowTypeError(ctx, "%s: element %u is not a number", what, (unsigned)i);
                    return -1;
                }
                if ((n & (n - 1)) == 0 && n >= 16) {
                    double* nd = (double*)dyn_nat_realloc(d, sizeof(double) * (size_t)n * 2);
                    if (!nd) {
                        dyn_nat_free(d);
                        JS_ThrowOutOfMemory(ctx);
                        return -1;
                    }
                    d = nd;
                }
                JS_ToFloat64(ctx, &d[n++], e);
            }
            *out = d;
            *pn = n;
            return 0;
        }
    }
    JS_ThrowTypeError(ctx, "%s: expected an array or a typed array of numbers", what);
    return -1;
}

static int sdb_u32_list(JSContext* ctx, JSValueConst v, const char* what,
    uint32_t** out, uint32_t* pn)
{
    int kind = sdb_typed_kind(v);
    double* d;
    uint32_t* u;
    uint32_t n, i;

    if (kind == JS_TYPED_ARRAY_UINT32 || kind == JS_TYPED_ARRAY_INT32) {
        size_t len;
        const uint8_t* p = sdb_typed_bytes(ctx, v, &len);
        if (!p)
            return -1;
        if (len / 4 > SDB_MAX_ITEMS) {
            JS_ThrowRangeError(ctx, "%s: more than %u values", what, SDB_MAX_ITEMS);
            return -1;
        }
        n = (uint32_t)(len / 4);
        u = (uint32_t*)dyn_nat_malloc(sizeof(uint32_t) * ((size_t)n + 1));
        if (!u) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(u, p, (size_t)n * 4);
        if (kind == JS_TYPED_ARRAY_INT32)
            for (i = 0; i < n; i++)
                if ((int32_t)u[i] < 0) {
                    dyn_nat_free(u);
                    JS_ThrowRangeError(ctx, "%s: element %u is negative", what, (unsigned)i);
                    return -1;
                }
        *out = u;
        *pn = n;
        return 0;
    }
    if (sdb_f64_list(ctx, v, what, &d, &n))
        return -1;
    u = (uint32_t*)d;
    for (i = 0; i < n; i++) {
        double x = d[i];
        if (!(x >= 0 && x <= 4294967295.0) || x != (double)(uint32_t)x) {
            dyn_nat_free(d);
            JS_ThrowRangeError(ctx, "%s: element %u is not an integer in 0..2^32-1", what, (unsigned)i);
            return -1;
        }
        u[i] = (uint32_t)x;
    }
    *out = u;
    *pn = n;
    return 0;
}

static int sdb_u32_arg(JSContext* ctx, JSValueConst v, const char* what, uint32_t* out)
{
    double x;
    if (JS_ToFloat64(ctx, &x, v))
        return -1;
    if (!(x >= 0 && x <= 4294967295.0) || x != (double)(uint32_t)x) {
        JS_ThrowRangeError(ctx, "%s must be an integer in 0..2^32-1", what);
        return -1;
    }
    *out = (uint32_t)x;
    return 0;
}

static int sdb_num_arg(JSContext* ctx, JSValueConst v, const char* what, double* out)
{
    if (JS_ToFloat64(ctx, out, v))
        return -1;
    if (*out != *out) {
        JS_ThrowTypeError(ctx, "%s must not be NaN", what);
        return -1;
    }
    return 0;
}

static int sdb_finite_arg(JSContext* ctx, JSValueConst v, const char* what, double* out)
{
    if (JS_ToFloat64(ctx, out, v))
        return -1;
    if (!isfinite(*out)) {
        JS_ThrowRangeError(ctx, "%s must be a finite number", what);
        return -1;
    }
    return 0;
}

static int sdb_opt_get(JSContext* ctx, JSValueConst opts, const char* name, JSValue* out)
{
    *out = JS_UNDEFINED;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
    *out = JS_GetPropertyStr(ctx, opts, name);
    return JS_IsException(*out) ? -1 : 0;
}

static int sdb_opt_bool(JSContext* ctx, JSValueConst opts, const char* name, int* out)
{
    JSValue v;
    if (sdb_opt_get(ctx, opts, name, &v))
        return -1;
    if (!JS_IsUndefined(v))
        *out = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return 0;
}

static int sdb_opt_u32(JSContext* ctx, JSValueConst opts, const char* name, uint32_t* out)
{
    JSValue v;
    int rc = 0;
    if (sdb_opt_get(ctx, opts, name, &v))
        return -1;
    if (!JS_IsUndefined(v)) {
        double x;
        if (JS_ToFloat64(ctx, &x, v))
            rc = -1;
        else if (x == HUGE_VAL)
            *out = UINT32_MAX;
        else if (!(x >= 0 && x <= 4294967295.0) || x != (double)(uint32_t)x) {
            JS_ThrowRangeError(ctx, "%s must be a non-negative integer", name);
            rc = -1;
        } else
            *out = (uint32_t)x;
    }
    JS_FreeValue(ctx, v);
    return rc;
}

static int sdb_opt_f64(JSContext* ctx, JSValueConst opts, const char* name, double* out)
{
    JSValue v;
    int rc = 0;
    if (sdb_opt_get(ctx, opts, name, &v))
        return -1;
    if (!JS_IsUndefined(v) && JS_ToFloat64(ctx, out, v))
        rc = -1;
    JS_FreeValue(ctx, v);
    return rc;
}

static JSValue sdb_typed_out(JSContext* ctx, const void* data, size_t n, JSTypedArrayEnum t)
{
    static const uint64_t zero = 0;
    JSValue ab, out;
    JSValueConst a[3];
    size_t bpe = sdb_typed_bpe((int)t);
    ab = JS_NewArrayBufferCopy(ctx, n ? (const uint8_t*)data : (const uint8_t*)&zero, n * bpe);
    if (JS_IsException(ab))
        return ab;
    a[0] = ab;
    a[1] = JS_NewInt32(ctx, 0);
    a[2] = JS_NewInt64(ctx, (int64_t)n);
    out = JS_NewTypedArray(ctx, 3, a, t);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue sdb_pair(JSContext* ctx, JSValue a, JSValue b)
{
    JSValue p = JS_NewArray(ctx);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return p;
    }
    if (JS_DefinePropertyValueUint32(ctx, p, 0, a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueUint32(ctx, p, 1, b, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, p);
        return JS_EXCEPTION;
    }
    return p;
}

static int sdb_push(JSContext* ctx, JSValueConst arr, uint32_t* i, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueUint32(ctx, arr, (*i)++, v, JS_PROP_C_W_E) < 0 ? -1 : 0;
}

static uint64_t sdb_hash(const char* p, size_t n)
{
    return dyn_xxh64((const uint8_t*)p, n, dyn_str_seed());
}

static uint64_t sdb_hash_fixed(const char* p, size_t n, uint64_t salt)
{
    return dyn_xxh64((const uint8_t*)p, n, SDB_FIXED_SEED ^ salt);
}

static uint64_t sdb_mix(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

typedef struct {
    uint64_t h;
    const char* k;
    uint32_t n;
    uint32_t used;
    uintptr_t v;
} sdb_sent;

typedef struct {
    sdb_sent* e;
    uint32_t cap, n;
} sdb_smap;

static void sdb_smap_free(sdb_smap* m)
{
    dyn_nat_free(m->e);
    m->e = NULL;
    m->cap = m->n = 0;
}

static int sdb_smap_grow(sdb_smap* m)
{
    uint32_t ncap = m->cap ? m->cap * 2 : 16, i;
    sdb_sent* ne;
    if (ncap > (1u << 30))
        return -1;
    ne = (sdb_sent*)dyn_nat_calloc(ncap, sizeof(*ne));
    if (!ne)
        return -1;
    for (i = 0; i < m->cap; i++) {
        sdb_sent* s = &m->e[i];
        uint32_t j;
        if (!s->used)
            continue;
        j = (uint32_t)s->h & (ncap - 1);
        while (ne[j].used)
            j = (j + 1) & (ncap - 1);
        ne[j] = *s;
    }
    dyn_nat_free(m->e);
    m->e = ne;
    m->cap = ncap;
    return 0;
}

static sdb_sent* sdb_smap_find(const sdb_smap* m, const char* k, size_t n, uint64_t h)
{
    uint32_t j;
    if (!m->cap)
        return NULL;
    j = (uint32_t)h & (m->cap - 1);
    while (m->e[j].used) {
        sdb_sent* s = &m->e[j];
        if (s->h == h && s->n == n && (n == 0 || memcmp(s->k, k, n) == 0))
            return s;
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

static sdb_sent* sdb_smap_insert(sdb_smap* m, const char* k, size_t n, uint64_t h, uintptr_t v)
{
    uint32_t j;
    if (n > UINT32_MAX)
        return NULL;
    if ((m->n + 1) * 4 > m->cap * 3 && sdb_smap_grow(m) < 0)
        return NULL;
    j = (uint32_t)h & (m->cap - 1);
    while (m->e[j].used)
        j = (j + 1) & (m->cap - 1);
    m->e[j].h = h;
    m->e[j].k = k;
    m->e[j].n = (uint32_t)n;
    m->e[j].used = 1;
    m->e[j].v = v;
    m->n++;
    return &m->e[j];
}

static void sdb_smap_remove(sdb_smap* m, sdb_sent* s)
{
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)(s - m->e), j = i;
    m->e[i].used = 0;
    m->n--;
    for (;;) {
        uint32_t home;
        j = (j + 1) & mask;
        if (!m->e[j].used)
            break;
        home = (uint32_t)m->e[j].h & mask;
        if ((j > i && (home <= i || home > j)) || (j < i && (home <= i && home > j))) {
            m->e[i] = m->e[j];
            m->e[j].used = 0;
            i = j;
        }
    }
}

typedef struct {
    uint32_t k;
    uint32_t used;
    uint32_t v;
} sdb_ient;

typedef struct {
    sdb_ient* e;
    uint32_t cap, n;
    uint64_t seed;
} sdb_imap;

static uint32_t sdb_ihash(const sdb_imap* m, uint32_t k)
{
    return (uint32_t)sdb_mix((uint64_t)k ^ m->seed);
}

static void sdb_imap_init(sdb_imap* m)
{
    m->e = NULL;
    m->cap = m->n = 0;
    m->seed = dyn_str_seed();
}

static void sdb_imap_free(sdb_imap* m)
{
    dyn_nat_free(m->e);
    m->e = NULL;
    m->cap = m->n = 0;
}

static int sdb_imap_grow(sdb_imap* m)
{
    uint32_t ncap = m->cap ? m->cap * 2 : 16, i;
    sdb_ient* ne;
    if (ncap > (1u << 30))
        return -1;
    ne = (sdb_ient*)dyn_nat_calloc(ncap, sizeof(*ne));
    if (!ne)
        return -1;
    for (i = 0; i < m->cap; i++) {
        uint32_t j;
        if (!m->e[i].used)
            continue;
        j = sdb_ihash(m, m->e[i].k) & (ncap - 1);
        while (ne[j].used)
            j = (j + 1) & (ncap - 1);
        ne[j] = m->e[i];
    }
    dyn_nat_free(m->e);
    m->e = ne;
    m->cap = ncap;
    return 0;
}

static sdb_ient* sdb_imap_find(const sdb_imap* m, uint32_t k)
{
    uint32_t j;
    if (!m->cap)
        return NULL;
    j = sdb_ihash(m, k) & (m->cap - 1);
    while (m->e[j].used) {
        if (m->e[j].k == k)
            return &m->e[j];
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

static sdb_ient* sdb_imap_put(sdb_imap* m, uint32_t k, uint32_t v)
{
    sdb_ient* f = sdb_imap_find(m, k);
    uint32_t j;
    if (f) {
        f->v = v;
        return f;
    }
    if ((m->n + 1) * 4 > m->cap * 3 && sdb_imap_grow(m) < 0)
        return NULL;
    j = sdb_ihash(m, k) & (m->cap - 1);
    while (m->e[j].used)
        j = (j + 1) & (m->cap - 1);
    m->e[j].k = k;
    m->e[j].v = v;
    m->e[j].used = 1;
    m->n++;
    return &m->e[j];
}

static void sdb_imap_remove(sdb_imap* m, sdb_ient* s)
{
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)(s - m->e), j = i;
    m->e[i].used = 0;
    m->n--;
    for (;;) {
        uint32_t home;
        j = (j + 1) & mask;
        if (!m->e[j].used)
            break;
        home = sdb_ihash(m, m->e[j].k) & mask;
        if ((j > i && (home <= i || home > j)) || (j < i && (home <= i && home > j))) {
            m->e[i] = m->e[j];
            m->e[j].used = 0;
            i = j;
        }
    }
}

static uint8_t* sdb_bytes_dup(const char* p, size_t n)
{
    uint8_t* d = (uint8_t*)dyn_nat_malloc(n + 1);
    if (d) {
        if (n)
            memcpy(d, p, n);
        d[n] = 0;
    }
    return d;
}

static int sdb_index_arg(JSContext* ctx, JSValueConst v, const char* what, int64_t* out)
{
    double x;
    if (JS_ToFloat64(ctx, &x, v))
        return -1;
    if (x != x || x != trunc(x)) {
        JS_ThrowRangeError(ctx, "%s must be an integer", what);
        return -1;
    }
    if (x > 9007199254740992.0)
        x = 9007199254740992.0;
    if (x < -9007199254740992.0)
        x = -9007199254740992.0;
    *out = (int64_t)x;
    return 0;
}

#define SDB_SL_MAX 32

typedef struct sdb_sln sdb_sln;

typedef struct {
    sdb_sln* fwd;
    uint32_t span;
} sdb_sllv;

struct sdb_sln {
    double score;
    JSValue val;
    sdb_sln* back;
    char* key;
    uint32_t klen;
    uint32_t lvl;
    sdb_sllv lv[];
};

typedef struct {
    JSRuntime* rt;
    sdb_sln* head;
    sdb_sln* tail;
    uint32_t len;
    int level;
    uint64_t rng;
    sdb_smap idx;
} sdb_sl;

static int sdb_sl_cmp(const sdb_sln* x, double s, const char* k, uint32_t n)
{
    uint32_t m;
    int c;
    if (x->score < s)
        return -1;
    if (x->score > s)
        return 1;
    m = x->klen < n ? x->klen : n;
    c = m ? memcmp(x->key, k, m) : 0;
    if (c)
        return c;
    return (x->klen > n) - (x->klen < n);
}

static sdb_sln* sdb_sl_node(sdb_sl* s, double score, const char* k, size_t n)
{
    uint64_t r = dyn_splitmix64(&s->rng);
    uint32_t lvl = 1;
    sdb_sln* x;
    while ((r & 3) == 0 && lvl < SDB_SL_MAX) {
        lvl++;
        r >>= 2;
    }
    if (n > UINT32_MAX - 1)
        return NULL;
    x = (sdb_sln*)dyn_nat_malloc(sizeof(sdb_sln) + lvl * sizeof(sdb_sllv) + n + 1);
    if (!x)
        return NULL;
    x->score = score;
    x->val = JS_UNDEFINED;
    x->back = NULL;
    x->lvl = lvl;
    x->klen = (uint32_t)n;
    x->key = (char*)&x->lv[lvl];
    if (n)
        memcpy(x->key, k, n);
    x->key[n] = 0;
    return x;
}

static int sdb_sl_init(sdb_sl* s, JSRuntime* rt)
{
    int i;
    memset(s, 0, sizeof(*s));
    s->rt = rt;
    s->level = 1;
    s->rng = dyn_str_seed() ^ (uint64_t)(uintptr_t)s;
    s->head = (sdb_sln*)dyn_nat_malloc(sizeof(sdb_sln) + SDB_SL_MAX * sizeof(sdb_sllv) + 1);
    if (!s->head)
        return -1;
    memset(s->head, 0, sizeof(sdb_sln));
    s->head->val = JS_UNDEFINED;
    s->head->lvl = SDB_SL_MAX;
    s->head->key = (char*)&s->head->lv[SDB_SL_MAX];
    s->head->key[0] = 0;
    for (i = 0; i < SDB_SL_MAX; i++) {
        s->head->lv[i].fwd = NULL;
        s->head->lv[i].span = 0;
    }
    return 0;
}

static void sdb_sl_link(sdb_sl* s, sdb_sln* x)
{
    sdb_sln* update[SDB_SL_MAX];
    uint32_t rank[SDB_SL_MAX];
    sdb_sln* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        rank[i] = i == s->level - 1 ? 0 : rank[i + 1];
        while (p->lv[i].fwd && sdb_sl_cmp(p->lv[i].fwd, x->score, x->key, x->klen) < 0) {
            rank[i] += p->lv[i].span;
            p = p->lv[i].fwd;
        }
        update[i] = p;
    }
    if ((int)x->lvl > s->level) {
        for (i = s->level; i < (int)x->lvl; i++) {
            rank[i] = 0;
            update[i] = s->head;
            update[i]->lv[i].span = s->len;
        }
        s->level = (int)x->lvl;
    }
    for (i = 0; i < (int)x->lvl; i++) {
        x->lv[i].fwd = update[i]->lv[i].fwd;
        update[i]->lv[i].fwd = x;
        x->lv[i].span = update[i]->lv[i].span - (rank[0] - rank[i]);
        update[i]->lv[i].span = (rank[0] - rank[i]) + 1;
    }
    for (i = (int)x->lvl; i < s->level; i++)
        update[i]->lv[i].span++;
    x->back = update[0] == s->head ? NULL : update[0];
    if (x->lv[0].fwd)
        x->lv[0].fwd->back = x;
    else
        s->tail = x;
    s->len++;
}

static void sdb_sl_unlink(sdb_sl* s, sdb_sln* x)
{
    sdb_sln* update[SDB_SL_MAX];
    sdb_sln* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && p->lv[i].fwd != x && sdb_sl_cmp(p->lv[i].fwd, x->score, x->key, x->klen) < 0)
            p = p->lv[i].fwd;
        update[i] = p;
    }
    for (i = 0; i < s->level; i++) {
        if (update[i]->lv[i].fwd == x) {
            update[i]->lv[i].span += x->lv[i].span - 1;
            update[i]->lv[i].fwd = x->lv[i].fwd;
        } else {
            update[i]->lv[i].span -= 1;
        }
    }
    if (x->lv[0].fwd)
        x->lv[0].fwd->back = x->back;
    else
        s->tail = x->back;
    while (s->level > 1 && s->head->lv[s->level - 1].fwd == NULL)
        s->level--;
    s->len--;
}

static uint32_t sdb_sl_rank(const sdb_sl* s, const sdb_sln* x)
{
    uint32_t r = 0;
    sdb_sln* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && sdb_sl_cmp(p->lv[i].fwd, x->score, x->key, x->klen) <= 0) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
        if (p == x)
            return r;
    }
    return 0;
}

static sdb_sln* sdb_sl_at(const sdb_sl* s, uint32_t rank)
{
    uint32_t t = 0;
    sdb_sln* p = s->head;
    int i;
    if (rank == 0 || rank > s->len)
        return NULL;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && t + p->lv[i].span <= rank) {
            t += p->lv[i].span;
            p = p->lv[i].fwd;
        }
        if (t == rank)
            return p;
    }
    return NULL;
}

static sdb_sln* sdb_sl_lt(const sdb_sl* s, double sc, const char* k, uint32_t n, int le, uint32_t* prank)
{
    uint32_t r = 0;
    sdb_sln* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && sdb_sl_cmp(p->lv[i].fwd, sc, k, n) < le) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
    }
    if (prank)
        *prank = r;
    return p == s->head ? NULL : p;
}

static sdb_sln* sdb_sl_first_ge_score(const sdb_sl* s, double sc, uint32_t* prank)
{
    uint32_t r = 0;
    sdb_sln* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && p->lv[i].fwd->score < sc) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
    }
    if (prank)
        *prank = r + 1;
    return p->lv[0].fwd;
}

static sdb_sln* sdb_sl_last_le_score(const sdb_sl* s, double sc, uint32_t* prank)
{
    uint32_t r = 0;
    sdb_sln* p = s->head;
    int i;
    for (i = s->level - 1; i >= 0; i--) {
        while (p->lv[i].fwd && p->lv[i].fwd->score <= sc) {
            r += p->lv[i].span;
            p = p->lv[i].fwd;
        }
    }
    if (prank)
        *prank = r;
    return p == s->head ? NULL : p;
}

static sdb_sln* sdb_sl_find(const sdb_sl* s, const char* k, size_t n)
{
    sdb_sent* e = sdb_smap_find(&s->idx, k, n, sdb_hash(k, n));
    return e ? (sdb_sln*)e->v : NULL;
}

static void sdb_sl_drop(sdb_sl* s, sdb_sln* x)
{
    sdb_sent* e = sdb_smap_find(&s->idx, x->key, x->klen, sdb_hash(x->key, x->klen));
    if (e)
        sdb_smap_remove(&s->idx, e);
    sdb_sl_unlink(s, x);
    JS_FreeValueRT(s->rt, x->val);
    dyn_nat_free(x);
}

static int sdb_sl_insert(sdb_sl* s, double score, const char* k, size_t n, JSValue val, sdb_sln** out)
{
    sdb_sln* x = sdb_sl_node(s, score, k, n);
    if (!x)
        return -1;
    if (!sdb_smap_insert(&s->idx, x->key, x->klen, sdb_hash(x->key, x->klen), (uintptr_t)x)) {
        dyn_nat_free(x);
        return -1;
    }
    x->val = val;
    sdb_sl_link(s, x);
    if (out)
        *out = x;
    return 0;
}

static void sdb_sl_clear(sdb_sl* s)
{
    sdb_sln* x = s->head->lv[0].fwd;
    int i;
    while (x) {
        sdb_sln* nx = x->lv[0].fwd;
        JS_FreeValueRT(s->rt, x->val);
        dyn_nat_free(x);
        x = nx;
    }
    for (i = 0; i < SDB_SL_MAX; i++) {
        s->head->lv[i].fwd = NULL;
        s->head->lv[i].span = 0;
    }
    s->tail = NULL;
    s->len = 0;
    s->level = 1;
    sdb_smap_free(&s->idx);
}

static void sdb_sl_destroy(sdb_sl* s)
{
    if (!s->head)
        return;
    sdb_sl_clear(s);
    dyn_nat_free(s->head);
    s->head = NULL;
}

static void sdb_sl_mark(JSRuntime* rt, const sdb_sl* s, JS_MarkFunc* mark_func)
{
    sdb_sln* x;
    if (!s->head)
        return;
    for (x = s->head->lv[0].fwd; x; x = x->lv[0].fwd)
        JS_MarkValue(rt, x->val, mark_func);
}

static JSValue sdb_sl_keystr(JSContext* ctx, const sdb_sln* x)
{
    return JS_NewStringLen(ctx, x->key, x->klen);
}

static int sdb_norm_range(int64_t start, int64_t stop, uint32_t len, uint32_t* a, uint32_t* b)
{
    if (start < 0)
        start += len;
    if (stop < 0)
        stop += len;
    if (start < 0)
        start = 0;
    if (start > stop || start >= (int64_t)len)
        return 0;
    if (stop >= (int64_t)len)
        stop = (int64_t)len - 1;
    *a = (uint32_t)start;
    *b = (uint32_t)stop;
    return 1;
}

typedef struct {
    sdb_sl sl;
} sdb_zset_w;

static JSClassID sdb_zset_class_id;

static void sdb_zset_free(void* native)
{
    sdb_zset_w* w = (sdb_zset_w*)native;
    if (!w)
        return;
    sdb_sl_destroy(&w->sl);
    dyn_nat_free(w);
}

static void sdb_zset_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_zset_free(JS_GetOpaque(val, sdb_zset_class_id));
}

static const JSClassDef sdb_zset_class = {
    "ZSet",
    .finalizer = sdb_zset_finalizer,
};

static sdb_zset_w* sdb_zset_new(JSContext* ctx)
{
    sdb_zset_w* w = (sdb_zset_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return NULL;
    if (sdb_sl_init(&w->sl, JS_GetRuntime(ctx)) < 0) {
        dyn_nat_free(w);
        return NULL;
    }
    return w;
}

static JSValue sdb_zset_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w;
    (void)argc;
    (void)argv;
    w = sdb_zset_new(ctx);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_zset_class_id, w, sdb_zset_free);
}

static int sdb_zset_put(sdb_zset_w* w, const char* k, size_t n, double score, int incr, double* result, int* added)
{
    sdb_sln* x = sdb_sl_find(&w->sl, k, n);
    *added = 0;
    if (x) {
        double ns = incr ? x->score + score : score;
        if (ns != ns)
            return -2;
        *result = ns;
        if (ns == x->score)
            return 0;
        sdb_sl_unlink(&w->sl, x);
        x->score = ns;
        sdb_sl_link(&w->sl, x);
        return 0;
    }
    *result = score;
    if (sdb_sl_insert(&w->sl, score, k, n, JS_UNDEFINED, NULL) < 0)
        return -1;
    *added = 1;
    return 0;
}

static JSValue sdb_zset_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    ds2_str k;
    double sc, res;
    int added, rc;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    if (sdb_num_arg(ctx, argv[1], magic ? "delta" : "score", &sc)) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    rc = sdb_zset_put(w, k.p, k.n, sc, magic, &res, &added);
    ds2_str_release(ctx, &k);
    if (rc == -2)
        return JS_ThrowRangeError(ctx, "incrBy: resulting score is NaN");
    if (rc < 0)
        return ds2_oom(ctx);
    return magic ? JS_NewFloat64(ctx, res) : JS_NewBool(ctx, added);
}

static JSValue sdb_zset_member_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    ds2_str k;
    sdb_sln* x;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    x = sdb_sl_find(&w->sl, k.p, k.n);
    ds2_str_release(ctx, &k);
    switch (magic) {
    case 0:
        return x ? JS_NewFloat64(ctx, x->score) : JS_UNDEFINED;
    case 1:
        return JS_NewBool(ctx, x != NULL);
    case 2:
        if (!x)
            return JS_FALSE;
        sdb_sl_drop(&w->sl, x);
        return JS_TRUE;
    case 3:
        return x ? JS_NewInt64(ctx, (int64_t)sdb_sl_rank(&w->sl, x) - 1) : JS_UNDEFINED;
    default:
        return x ? JS_NewInt64(ctx, (int64_t)w->sl.len - (int64_t)sdb_sl_rank(&w->sl, x)) : JS_UNDEFINED;
    }
}

static JSValue sdb_zset_entry(JSContext* ctx, const sdb_sln* x)
{
    return sdb_pair(ctx, sdb_sl_keystr(ctx, x), JS_NewFloat64(ctx, x->score));
}

static JSValue sdb_zset_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w;
    int64_t i;
    sdb_sln* x;
    (void)argc;
    if (sdb_index_arg(ctx, argv[0], "index", &i))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (i < 0)
        i += w->sl.len;
    if (i < 0 || i >= (int64_t)w->sl.len)
        return JS_UNDEFINED;
    x = sdb_sl_at(&w->sl, (uint32_t)i + 1);
    return x ? sdb_zset_entry(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_zset_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    int64_t start = 0, stop = -1;
    int reverse = 0;
    uint32_t a, b, i, out = 0;
    sdb_sln* x;
    JSValue arr;
    if (!magic) {
        if (sdb_index_arg(ctx, argv[0], "start", &start) || sdb_index_arg(ctx, argv[1], "stop", &stop))
            return JS_EXCEPTION;
        if (argc > 2 && sdb_opt_bool(ctx, argv[2], "reverse", &reverse))
            return JS_EXCEPTION;
    }
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || !sdb_norm_range(start, stop, w->sl.len, &a, &b))
        return arr;
    x = reverse ? sdb_sl_at(&w->sl, w->sl.len - a) : sdb_sl_at(&w->sl, a + 1);
    for (i = a; x && i <= b; i++) {
        if (sdb_push(ctx, arr, &out, sdb_zset_entry(ctx, x)) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        x = reverse ? x->back : x->lv[0].fwd;
    }
    return arr;
}

static JSValue sdb_zset_by_score(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    double lo, hi;
    uint32_t offset = 0, limit = UINT32_MAX, r1, r2, out = 0, n;
    int reverse = 0;
    sdb_sln* x;
    JSValue arr;
    if (sdb_num_arg(ctx, argv[0], "min", &lo) || sdb_num_arg(ctx, argv[1], "max", &hi))
        return JS_EXCEPTION;
    if (magic == 0 && argc > 2 && (sdb_opt_u32(ctx, argv[2], "offset", &offset) || sdb_opt_u32(ctx, argv[2], "limit", &limit) || sdb_opt_bool(ctx, argv[2], "reverse", &reverse)))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    sdb_sl_first_ge_score(&w->sl, lo, &r1);
    sdb_sl_last_le_score(&w->sl, hi, &r2);
    n = r2 >= r1 ? r2 - r1 + 1 : 0;
    if (magic == 1)
        return JS_NewInt64(ctx, n);
    if (magic == 2) {
        uint32_t k;
        x = n ? sdb_sl_at(&w->sl, r1) : NULL;
        for (k = 0; k < n && x; k++) {
            sdb_sln* nx = x->lv[0].fwd;
            sdb_sl_drop(&w->sl, x);
            x = nx;
        }
        return JS_NewInt64(ctx, k);
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || offset >= n)
        return arr;
    x = reverse ? sdb_sl_at(&w->sl, r2 - offset) : sdb_sl_at(&w->sl, r1 + offset);
    n -= offset;
    while (x && n-- && out < limit) {
        if (sdb_push(ctx, arr, &out, sdb_zset_entry(ctx, x)) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        x = reverse ? x->back : x->lv[0].fwd;
    }
    return arr;
}

static JSValue sdb_zset_remove_rank(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w;
    int64_t start, stop;
    uint32_t a, b, k = 0;
    sdb_sln* x;
    (void)argc;
    if (sdb_index_arg(ctx, argv[0], "start", &start) || sdb_index_arg(ctx, argv[1], "stop", &stop))
        return JS_EXCEPTION;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!sdb_norm_range(start, stop, w->sl.len, &a, &b))
        return JS_NewInt32(ctx, 0);
    x = sdb_sl_at(&w->sl, a + 1);
    while (x && k < b - a + 1) {
        sdb_sln* nx = x->lv[0].fwd;
        sdb_sl_drop(&w->sl, x);
        x = nx;
        k++;
    }
    return JS_NewInt64(ctx, k);
}

static JSValue sdb_zset_pop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_zset_w* w;
    sdb_sln* x;
    JSValue r;
    (void)argc;
    (void)argv;
    w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    x = magic ? w->sl.tail : w->sl.head->lv[0].fwd;
    if (!x)
        return JS_UNDEFINED;
    r = sdb_zset_entry(ctx, x);
    if (JS_IsException(r))
        return r;
    sdb_sl_drop(&w->sl, x);
    return r;
}

static JSValue sdb_zset_size(JSContext* ctx, JSValueConst this_val)
{
    sdb_zset_w* w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, w->sl.len);
}

static JSValue sdb_zset_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_zset_w* w = (sdb_zset_w*)dyn_plain_get(ctx, this_val, sdb_zset_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    sdb_sl_clear(&w->sl);
    return JS_UNDEFINED;
}

DS2_ITERATOR(sdb_zset, "entries")

static const JSCFunctionListEntry sdb_zset_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 2, sdb_zset_add, 0),
    JS_CFUNC_MAGIC_DEF("incrBy", 2, sdb_zset_add, 1),
    JS_CFUNC_MAGIC_DEF("score", 1, sdb_zset_member_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_zset_member_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_zset_member_op, 2),
    JS_CFUNC_MAGIC_DEF("rank", 1, sdb_zset_member_op, 3),
    JS_CFUNC_MAGIC_DEF("revRank", 1, sdb_zset_member_op, 4),
    JS_CFUNC_DEF("at", 1, sdb_zset_at),
    JS_CFUNC_MAGIC_DEF("range", 2, sdb_zset_range, 0),
    JS_CFUNC_MAGIC_DEF("entries", 0, sdb_zset_range, 1),
    JS_CFUNC_MAGIC_DEF("rangeByScore", 2, sdb_zset_by_score, 0),
    JS_CFUNC_MAGIC_DEF("countByScore", 2, sdb_zset_by_score, 1),
    JS_CFUNC_MAGIC_DEF("removeRangeByScore", 2, sdb_zset_by_score, 2),
    JS_CFUNC_DEF("removeRangeByRank", 2, sdb_zset_remove_rank),
    JS_CFUNC_MAGIC_DEF("popMin", 0, sdb_zset_pop, 0),
    JS_CFUNC_MAGIC_DEF("popMax", 0, sdb_zset_pop, 1),
    JS_CFUNC_DEF("clear", 0, sdb_zset_clear),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, sdb_zset_iterator),
    JS_CGETSET_DEF("size", sdb_zset_size, NULL),
};

typedef struct {
    sdb_sl sl;
} sdb_omap_w;

static JSClassID sdb_omap_class_id;

static void sdb_omap_free(void* native)
{
    sdb_omap_w* w = (sdb_omap_w*)native;
    if (!w)
        return;
    sdb_sl_destroy(&w->sl);
    dyn_nat_free(w);
}

static void sdb_omap_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_omap_free(JS_GetOpaque(val, sdb_omap_class_id));
}

static void sdb_omap_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    sdb_omap_w* w = JS_GetOpaque(val, sdb_omap_class_id);
    if (w)
        sdb_sl_mark(rt, &w->sl, mark_func);
}

static const JSClassDef sdb_omap_class = {
    "OrderedMap",
    .finalizer = sdb_omap_finalizer,
    .gc_mark = sdb_omap_gc_mark,
};

static JSValue sdb_omap_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    (void)argc;
    (void)argv;
    w = (sdb_omap_w*)dyn_nat_malloc(sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    if (sdb_sl_init(&w->sl, JS_GetRuntime(ctx)) < 0) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, sdb_omap_class_id, w, sdb_omap_free);
}

static int sdb_omap_put(JSContext* ctx, sdb_omap_w* w, const char* k, size_t n, JSValueConst v)
{
    sdb_sln* x = sdb_sl_find(&w->sl, k, n);
    if (x) {
        JSValue old = x->val;
        x->val = JS_DupValue(ctx, v);
        JS_FreeValue(ctx, old);
        return 0;
    }
    {
        JSValue d = JS_DupValue(ctx, v);
        if (sdb_sl_insert(&w->sl, 0, k, n, d, NULL) < 0) {
            JS_FreeValue(ctx, d);
            return -1;
        }
    }
    return 0;
}

static JSValue sdb_omap_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    ds2_str k;
    int rc;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    rc = sdb_omap_put(ctx, w, k.p, k.n, argv[1]);
    ds2_str_release(ctx, &k);
    if (rc < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_omap_entry(JSContext* ctx, const sdb_sln* x)
{
    return sdb_pair(ctx, sdb_sl_keystr(ctx, x), JS_DupValue(ctx, x->val));
}

static JSValue sdb_omap_key_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_omap_w* w;
    ds2_str k;
    sdb_sln* x = NULL;
    uint32_t r = 0;
    JSValue res = JS_UNDEFINED;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0:
        x = sdb_sl_find(&w->sl, k.p, k.n);
        res = x ? JS_DupValue(ctx, x->val) : JS_UNDEFINED;
        break;
    case 1:
        res = JS_NewBool(ctx, sdb_sl_find(&w->sl, k.p, k.n) != NULL);
        break;
    case 2:
        x = sdb_sl_find(&w->sl, k.p, k.n);
        if (x)
            sdb_sl_drop(&w->sl, x);
        res = JS_NewBool(ctx, x != NULL);
        break;
    case 3:
        x = sdb_sl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 1, NULL);
        res = x ? sdb_sl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    case 4:
        x = sdb_sl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 0, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
        res = x ? sdb_sl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    case 5:
        x = sdb_sl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 0, NULL);
        res = x ? sdb_sl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    case 6:
        x = sdb_sl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 1, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
        res = x ? sdb_sl_keystr(ctx, x) : JS_UNDEFINED;
        break;
    default:
        sdb_sl_lt(&w->sl, 0, k.p, (uint32_t)k.n, 0, &r);
        res = JS_NewInt64(ctx, r);
        break;
    }
    ds2_str_release(ctx, &k);
    return res;
}

static JSValue sdb_omap_end(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_omap_w* w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    sdb_sln* x;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    x = magic ? w->sl.tail : w->sl.head->lv[0].fwd;
    return x ? sdb_sl_keystr(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_omap_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_omap_w* w;
    ds2_str lo = { 0 }, hi = { 0 };
    uint32_t limit = UINT32_MAX, out = 0;
    int reverse = 0, keys_only = magic == 3;
    sdb_sln* x;
    JSValue arr;
    JSValueConst opts = JS_UNDEFINED;

    if (magic == 0 || magic == 1) {
        if (ds2_str_get(ctx, &lo, argv[0]))
            return JS_EXCEPTION;
    }
    if (magic == 0) {
        if (ds2_str_get(ctx, &hi, argv[1])) {
            ds2_str_release(ctx, &lo);
            return JS_EXCEPTION;
        }
        if (argc > 2)
            opts = argv[2];
    } else if (magic == 1 && argc > 1)
        opts = argv[1];
    if (sdb_opt_u32(ctx, opts, "limit", &limit) || (magic == 0 && sdb_opt_bool(ctx, opts, "reverse", &reverse))) {
        ds2_str_release(ctx, &lo);
        ds2_str_release(ctx, &hi);
        return JS_EXCEPTION;
    }
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    arr = w ? JS_NewArray(ctx) : JS_EXCEPTION;
    if (JS_IsException(arr)) {
        ds2_str_release(ctx, &lo);
        ds2_str_release(ctx, &hi);
        return arr;
    }
    if (magic >= 2) {
        x = w->sl.head->lv[0].fwd;
    } else if (reverse) {
        x = sdb_sl_lt(&w->sl, 0, hi.p, (uint32_t)hi.n, 1, NULL);
    } else {
        x = sdb_sl_lt(&w->sl, 0, lo.p, (uint32_t)lo.n, 0, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
    }
    while (x && out < limit) {
        JSValue item;
        if (magic == 0) {
            if (reverse ? sdb_sl_cmp(x, 0, lo.p, (uint32_t)lo.n) < 0 : sdb_sl_cmp(x, 0, hi.p, (uint32_t)hi.n) > 0)
                break;
        } else if (magic == 1) {
            if (x->klen < lo.n || (lo.n && memcmp(x->key, lo.p, lo.n) != 0))
                break;
        }
        item = keys_only ? sdb_sl_keystr(ctx, x) : sdb_omap_entry(ctx, x);
        if (sdb_push(ctx, arr, &out, item) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
        x = reverse ? x->back : x->lv[0].fwd;
    }
    ds2_str_release(ctx, &lo);
    ds2_str_release(ctx, &hi);
    return arr;
}

static JSValue sdb_omap_delete_range(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    ds2_str lo, hi;
    sdb_sln* x;
    uint32_t k = 0;
    (void)argc;
    if (ds2_str_get(ctx, &lo, argv[0]))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &hi, argv[1])) {
        ds2_str_release(ctx, &lo);
        return JS_EXCEPTION;
    }
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (w) {
        x = sdb_sl_lt(&w->sl, 0, lo.p, (uint32_t)lo.n, 0, NULL);
        x = x ? x->lv[0].fwd : w->sl.head->lv[0].fwd;
        while (x && sdb_sl_cmp(x, 0, hi.p, (uint32_t)hi.n) <= 0) {
            sdb_sln* nx = x->lv[0].fwd;
            sdb_sl_drop(&w->sl, x);
            x = nx;
            k++;
        }
    }
    ds2_str_release(ctx, &lo);
    ds2_str_release(ctx, &hi);
    return w ? JS_NewInt64(ctx, k) : JS_EXCEPTION;
}

static JSValue sdb_omap_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w;
    int64_t i;
    sdb_sln* x;
    (void)argc;
    if (sdb_index_arg(ctx, argv[0], "index", &i))
        return JS_EXCEPTION;
    w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (i < 0)
        i += w->sl.len;
    if (i < 0 || i >= (int64_t)w->sl.len)
        return JS_UNDEFINED;
    x = sdb_sl_at(&w->sl, (uint32_t)i + 1);
    return x ? sdb_omap_entry(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_omap_size(JSContext* ctx, JSValueConst this_val)
{
    sdb_omap_w* w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, w->sl.len);
}

static JSValue sdb_omap_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_omap_w* w = (sdb_omap_w*)dyn_plain_get(ctx, this_val, sdb_omap_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    sdb_sl_clear(&w->sl);
    return JS_UNDEFINED;
}

DS2_ITERATOR(sdb_omap, "entries")

static const JSCFunctionListEntry sdb_omap_proto[] = {
    JS_CFUNC_DEF("set", 2, sdb_omap_set),
    JS_CFUNC_MAGIC_DEF("get", 1, sdb_omap_key_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_omap_key_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_omap_key_op, 2),
    JS_CFUNC_MAGIC_DEF("floorKey", 1, sdb_omap_key_op, 3),
    JS_CFUNC_MAGIC_DEF("ceilKey", 1, sdb_omap_key_op, 4),
    JS_CFUNC_MAGIC_DEF("lowerKey", 1, sdb_omap_key_op, 5),
    JS_CFUNC_MAGIC_DEF("higherKey", 1, sdb_omap_key_op, 6),
    JS_CFUNC_MAGIC_DEF("rank", 1, sdb_omap_key_op, 7),
    JS_CFUNC_MAGIC_DEF("firstKey", 0, sdb_omap_end, 0),
    JS_CFUNC_MAGIC_DEF("lastKey", 0, sdb_omap_end, 1),
    JS_CFUNC_MAGIC_DEF("rangeQuery", 2, sdb_omap_range, 0),
    JS_CFUNC_MAGIC_DEF("prefix", 1, sdb_omap_range, 1),
    JS_CFUNC_MAGIC_DEF("entries", 0, sdb_omap_range, 2),
    JS_CFUNC_MAGIC_DEF("keys", 0, sdb_omap_range, 3),
    JS_CFUNC_DEF("deleteRange", 2, sdb_omap_delete_range),
    JS_CFUNC_DEF("at", 1, sdb_omap_at),
    JS_CFUNC_DEF("clear", 0, sdb_omap_clear),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, sdb_omap_iterator),
    JS_CGETSET_DEF("size", sdb_omap_size, NULL),
};

#define RB_ARR 1
#define RB_BMP 2
#define RB_FULL 3
#define RB_ARR_MAX 4096u
#define RB_WORDS 1024
#define RB_AND 0
#define RB_OR 1
#define RB_XOR 2
#define RB_ANDNOT 3

typedef struct {
    uint16_t key;
    uint8_t type;
    uint32_t card;
    uint32_t cap;
    void* data;
} rb_ct;

typedef struct {
    rb_ct* c;
    uint32_t n, cap;
} rb_t;

static const uint64_t rb_ones[RB_WORDS] = { [0 ... RB_WORDS - 1] = ~0ULL };

static const uint64_t* rb_words(const rb_ct* c)
{
    return c->type == RB_FULL ? rb_ones : (const uint64_t*)c->data;
}

static void rb_ct_free(rb_ct* c)
{
    if (c->type != RB_FULL)
        dyn_nat_free(c->data);
    c->data = NULL;
    c->type = 0;
    c->card = 0;
}

static void rb_clear(rb_t* r)
{
    uint32_t i;
    for (i = 0; i < r->n; i++)
        rb_ct_free(&r->c[i]);
    dyn_nat_free(r->c);
    r->c = NULL;
    r->n = r->cap = 0;
}

static int rb_find(const rb_t* r, uint16_t key, uint32_t* pos)
{
    uint32_t lo = 0, hi = r->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (r->c[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    *pos = lo;
    return lo < r->n && r->c[lo].key == key;
}

static int rb_reserve(rb_t* r, uint32_t need)
{
    uint32_t ncap;
    rb_ct* nc;
    if (need <= r->cap)
        return 0;
    ncap = r->cap ? r->cap * 2 : 4;
    while (ncap < need)
        ncap *= 2;
    nc = (rb_ct*)dyn_nat_realloc(r->c, sizeof(rb_ct) * ncap);
    if (!nc)
        return -1;
    r->c = nc;
    r->cap = ncap;
    return 0;
}

static rb_ct* rb_insert_at(rb_t* r, uint32_t pos, uint16_t key)
{
    if (rb_reserve(r, r->n + 1) < 0)
        return NULL;
    memmove(&r->c[pos + 1], &r->c[pos], sizeof(rb_ct) * (r->n - pos));
    r->n++;
    memset(&r->c[pos], 0, sizeof(rb_ct));
    r->c[pos].key = key;
    return &r->c[pos];
}

static void rb_remove_at(rb_t* r, uint32_t pos)
{
    rb_ct_free(&r->c[pos]);
    memmove(&r->c[pos], &r->c[pos + 1], sizeof(rb_ct) * (r->n - pos - 1));
    r->n--;
}

static uint32_t rb_popcnt(const uint64_t* w)
{
    uint32_t i, c = 0;
    for (i = 0; i < RB_WORDS; i++)
        c += (uint32_t)__builtin_popcountll(w[i]);
    return c;
}

static void rb_arr_to_words(const rb_ct* c, uint64_t* w)
{
    const uint16_t* a = (const uint16_t*)c->data;
    uint32_t i;
    memset(w, 0, sizeof(uint64_t) * RB_WORDS);
    for (i = 0; i < c->card; i++)
        w[a[i] >> 6] |= 1ULL << (a[i] & 63);
}

static const uint64_t* rb_words_of(const rb_ct* c, uint64_t* tmp)
{
    if (!c) {
        memset(tmp, 0, sizeof(uint64_t) * RB_WORDS);
        return tmp;
    }
    if (c->type == RB_ARR) {
        rb_arr_to_words(c, tmp);
        return tmp;
    }
    return rb_words(c);
}

static int rb_adopt_words(rb_ct* out, uint64_t* w, uint32_t card)
{
    out->card = card;
    if (card == 0) {
        dyn_nat_free(w);
        out->type = 0;
        out->data = NULL;
        return 0;
    }
    if (card == 65536) {
        dyn_nat_free(w);
        out->type = RB_FULL;
        out->data = NULL;
        return 0;
    }
    if (card <= RB_ARR_MAX) {
        uint16_t* a = (uint16_t*)dyn_nat_malloc(sizeof(uint16_t) * card);
        uint32_t i, k = 0;
        if (!a) {
            dyn_nat_free(w);
            return -1;
        }
        for (i = 0; i < RB_WORDS; i++) {
            uint64_t x = w[i];
            while (x) {
                a[k++] = (uint16_t)(i * 64 + __builtin_ctzll(x));
                x &= x - 1;
            }
        }
        dyn_nat_free(w);
        out->type = RB_ARR;
        out->data = a;
        out->cap = card;
        return 0;
    }
    out->type = RB_BMP;
    out->data = w;
    out->cap = 0;
    return 0;
}

static int rb_ct_copy(rb_ct* out, const rb_ct* src)
{
    out->key = src->key;
    out->type = src->type;
    out->card = src->card;
    out->cap = 0;
    out->data = NULL;
    if (src->type == RB_ARR) {
        out->data = dyn_nat_malloc(sizeof(uint16_t) * src->card);
        if (!out->data)
            return -1;
        memcpy(out->data, src->data, sizeof(uint16_t) * src->card);
        out->cap = src->card;
    } else if (src->type == RB_BMP) {
        out->data = dyn_nat_malloc(sizeof(uint64_t) * RB_WORDS);
        if (!out->data)
            return -1;
        memcpy(out->data, src->data, sizeof(uint64_t) * RB_WORDS);
    }
    return 0;
}

static int rb_arr_has(const rb_ct* c, uint16_t lo, uint32_t* pos)
{
    const uint16_t* a = (const uint16_t*)c->data;
    uint32_t l = 0, h = c->card;
    while (l < h) {
        uint32_t m = (l + h) >> 1;
        if (a[m] < lo)
            l = m + 1;
        else
            h = m;
    }
    if (pos)
        *pos = l;
    return l < c->card && a[l] == lo;
}

static int rb_ct_has(const rb_ct* c, uint16_t lo)
{
    if (c->type == RB_ARR)
        return rb_arr_has(c, lo, NULL);
    return (rb_words(c)[lo >> 6] >> (lo & 63)) & 1;
}

static int rb_add(rb_t* r, uint32_t x)
{
    uint16_t hi = (uint16_t)(x >> 16), lo = (uint16_t)x;
    uint32_t pos;
    rb_ct* c;
    if (!rb_find(r, hi, &pos)) {
        c = rb_insert_at(r, pos, hi);
        if (!c)
            return -1;
        c->data = dyn_nat_malloc(sizeof(uint16_t) * 4);
        if (!c->data) {
            memmove(&r->c[pos], &r->c[pos + 1], sizeof(rb_ct) * (r->n - pos - 1));
            r->n--;
            return -1;
        }
        c->type = RB_ARR;
        c->cap = 4;
        c->card = 1;
        ((uint16_t*)c->data)[0] = lo;
        return 1;
    }
    c = &r->c[pos];
    if (c->type == RB_FULL)
        return 0;
    if (c->type == RB_BMP) {
        uint64_t* w = (uint64_t*)c->data;
        uint64_t b = 1ULL << (lo & 63);
        if (w[lo >> 6] & b)
            return 0;
        w[lo >> 6] |= b;
        if (++c->card == 65536) {
            dyn_nat_free(c->data);
            c->data = NULL;
            c->type = RB_FULL;
        }
        return 1;
    }
    {
        uint16_t* a = (uint16_t*)c->data;
        uint32_t p;
        if (c->card && a[c->card - 1] < lo)
            p = c->card;
        else if (rb_arr_has(c, lo, &p))
            return 0;
        if (c->card == RB_ARR_MAX) {
            uint64_t* w = (uint64_t*)dyn_nat_malloc(sizeof(uint64_t) * RB_WORDS);
            if (!w)
                return -1;
            rb_arr_to_words(c, w);
            w[lo >> 6] |= 1ULL << (lo & 63);
            dyn_nat_free(c->data);
            c->data = w;
            c->type = RB_BMP;
            c->card++;
            return 1;
        }
        if (c->card == c->cap) {
            uint32_t ncap = c->cap * 2 > RB_ARR_MAX ? RB_ARR_MAX : c->cap * 2;
            uint16_t* na = (uint16_t*)dyn_nat_realloc(a, sizeof(uint16_t) * ncap);
            if (!na)
                return -1;
            c->data = a = na;
            c->cap = ncap;
        }
        memmove(&a[p + 1], &a[p], sizeof(uint16_t) * (c->card - p));
        a[p] = lo;
        c->card++;
        return 1;
    }
}

static int rb_remove(rb_t* r, uint32_t x)
{
    uint16_t hi = (uint16_t)(x >> 16), lo = (uint16_t)x;
    uint32_t pos, p;
    rb_ct* c;
    if (!rb_find(r, hi, &pos))
        return 0;
    c = &r->c[pos];
    if (c->type == RB_ARR) {
        uint16_t* a = (uint16_t*)c->data;
        if (!rb_arr_has(c, lo, &p))
            return 0;
        memmove(&a[p], &a[p + 1], sizeof(uint16_t) * (c->card - p - 1));
        if (--c->card == 0)
            rb_remove_at(r, pos);
        return 1;
    }
    if (c->type == RB_FULL) {
        uint64_t* w = (uint64_t*)dyn_nat_malloc(sizeof(uint64_t) * RB_WORDS);
        if (!w)
            return -1;
        memcpy(w, rb_ones, sizeof(rb_ones));
        c->data = w;
        c->type = RB_BMP;
    }
    {
        uint64_t* w = (uint64_t*)c->data;
        uint64_t b = 1ULL << (lo & 63);
        if (!(w[lo >> 6] & b))
            return 0;
        w[lo >> 6] &= ~b;
        c->card--;
        if (c->card <= RB_ARR_MAX) {
            rb_ct t = *c;
            c->data = NULL;
            if (rb_adopt_words(&t, w, c->card) < 0) {
                c->data = NULL;
                rb_remove_at(r, pos);
                return -1;
            }
            *c = t;
        }
        return 1;
    }
}

static int rb_has(const rb_t* r, uint32_t x)
{
    uint32_t pos;
    if (!rb_find(r, (uint16_t)(x >> 16), &pos))
        return 0;
    return rb_ct_has(&r->c[pos], (uint16_t)x);
}

static int rb_range(rb_t* r, uint64_t lo, uint64_t hi, int add)
{
    uint64_t k;
    if (lo >= hi)
        return 0;
    for (k = lo >> 16; k <= (hi - 1) >> 16; k++) {
        uint32_t a = k == (lo >> 16) ? (uint32_t)(lo & 0xFFFF) : 0;
        uint32_t b = k == ((hi - 1) >> 16) ? (uint32_t)((hi - 1) & 0xFFFF) : 0xFFFF;
        uint32_t pos;
        int found = rb_find(r, (uint16_t)k, &pos);
        rb_ct* c;
        uint64_t* w;
        uint32_t i;
        if (a == 0 && b == 0xFFFF) {
            if (!add) {
                if (found)
                    rb_remove_at(r, pos);
                continue;
            }
            if (!found && !rb_insert_at(r, pos, (uint16_t)k))
                return -1;
            rb_ct_free(&r->c[pos]);
            r->c[pos].type = RB_FULL;
            r->c[pos].card = 65536;
            continue;
        }
        if (!found && !add)
            continue;
        w = (uint64_t*)dyn_nat_malloc(sizeof(uint64_t) * RB_WORDS);
        if (!w)
            return -1;
        if (found) {
            const uint64_t* src = rb_words_of(&r->c[pos], w);
            if (src != w)
                memcpy(w, src, sizeof(uint64_t) * RB_WORDS);
        } else {
            memset(w, 0, sizeof(uint64_t) * RB_WORDS);
        }
        for (i = a; i <= b;) {
            if ((i & 63) == 0 && i + 63 <= b) {
                w[i >> 6] = add ? ~0ULL : 0;
                i += 64;
            } else {
                if (add)
                    w[i >> 6] |= 1ULL << (i & 63);
                else
                    w[i >> 6] &= ~(1ULL << (i & 63));
                i++;
            }
        }
        if (!found) {
            c = rb_insert_at(r, pos, (uint16_t)k);
            if (!c) {
                dyn_nat_free(w);
                return -1;
            }
        } else {
            c = &r->c[pos];
            rb_ct_free(c);
        }
        if (rb_adopt_words(c, w, rb_popcnt(w)) < 0) {
            rb_remove_at(r, pos);
            return -1;
        }
        if (c->card == 0)
            rb_remove_at(r, pos);
    }
    return 0;
}

static uint64_t rb_card(const rb_t* r)
{
    uint64_t s = 0;
    uint32_t i;
    for (i = 0; i < r->n; i++)
        s += r->c[i].card;
    return s;
}

static int rb_ct_op(const rb_ct* a, const rb_ct* b, int op, rb_ct* out, uint64_t* cardp)
{
    uint64_t ta[RB_WORDS], tb[RB_WORDS];
    const uint64_t *wa, *wb;
    uint32_t i, card = 0;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->key = a ? a->key : b->key;
    }
    if (!a || !b) {
        const rb_ct* s = NULL;
        if (op == RB_AND || (op == RB_ANDNOT && !a))
            s = NULL;
        else
            s = a ? a : b;
        if (!s) {
            *cardp = 0;
            return 0;
        }
        *cardp = s->card;
        return out ? rb_ct_copy(out, s) : 0;
    }
    if (a->type == RB_ARR && b->type == RB_ARR) {
        const uint16_t *x = (const uint16_t*)a->data, *y = (const uint16_t*)b->data;
        uint32_t p = 0, q = 0, cap = op == RB_AND ? (a->card < b->card ? a->card : b->card) : op == RB_ANDNOT ? a->card
                                                                                                              : a->card + b->card;
        uint16_t* o = NULL;
        if (out && cap) {
            o = (uint16_t*)dyn_nat_malloc(sizeof(uint16_t) * cap);
            if (!o)
                return -1;
        }
        while (p < a->card || q < b->card) {
            int take;
            uint16_t v;
            if (q >= b->card || (p < a->card && x[p] < y[q])) {
                v = x[p++];
                take = op != RB_AND;
            } else if (p >= a->card || y[q] < x[p]) {
                v = y[q++];
                take = op == RB_OR || op == RB_XOR;
            } else {
                v = x[p++];
                q++;
                take = op == RB_AND || op == RB_OR;
            }
            if (take) {
                if (o)
                    o[card] = v;
                card++;
            }
        }
        *cardp = card;
        if (!out)
            return 0;
        if (card == 0) {
            dyn_nat_free(o);
            return 0;
        }
        if (card <= RB_ARR_MAX) {
            out->type = RB_ARR;
            out->data = o;
            out->cap = cap;
            out->card = card;
            return 0;
        }
        {
            uint64_t* w = (uint64_t*)dyn_nat_calloc(RB_WORDS, sizeof(uint64_t));
            if (!w) {
                dyn_nat_free(o);
                return -1;
            }
            for (i = 0; i < card; i++)
                w[o[i] >> 6] |= 1ULL << (o[i] & 63);
            dyn_nat_free(o);
            return rb_adopt_words(out, w, card);
        }
    }
    if ((op == RB_AND || op == RB_ANDNOT) && a->type == RB_ARR) {
        const uint16_t* x = (const uint16_t*)a->data;
        const uint64_t* yw = rb_words(b);
        uint16_t* o = NULL;
        if (out) {
            o = (uint16_t*)dyn_nat_malloc(sizeof(uint16_t) * a->card);
            if (!o)
                return -1;
        }
        for (i = 0; i < a->card; i++) {
            int in = (int)((yw[x[i] >> 6] >> (x[i] & 63)) & 1);
            if (in == (op == RB_AND)) {
                if (o)
                    o[card] = x[i];
                card++;
            }
        }
        *cardp = card;
        if (!out)
            return 0;
        if (!card) {
            dyn_nat_free(o);
            return 0;
        }
        out->type = RB_ARR;
        out->data = o;
        out->cap = a->card;
        out->card = card;
        return 0;
    }
    if (op == RB_AND && b->type == RB_ARR)
        return rb_ct_op(b, a, op, out, cardp);
    wa = rb_words_of(a, ta);
    wb = rb_words_of(b, tb);
    if (!out) {
        for (i = 0; i < RB_WORDS; i++) {
            uint64_t v = op == RB_AND ? wa[i] & wb[i] : op == RB_OR ? wa[i] | wb[i]
                : op == RB_XOR                                      ? wa[i] ^ wb[i]
                                                                    : wa[i] & ~wb[i];
            card += (uint32_t)__builtin_popcountll(v);
        }
        *cardp = card;
        return 0;
    }
    {
        uint64_t* w = (uint64_t*)dyn_nat_malloc(sizeof(uint64_t) * RB_WORDS);
        if (!w)
            return -1;
        for (i = 0; i < RB_WORDS; i++) {
            uint64_t v = op == RB_AND ? wa[i] & wb[i] : op == RB_OR ? wa[i] | wb[i]
                : op == RB_XOR                                      ? wa[i] ^ wb[i]
                                                                    : wa[i] & ~wb[i];
            w[i] = v;
            card += (uint32_t)__builtin_popcountll(v);
        }
        *cardp = card;
        return rb_adopt_words(out, w, card);
    }
}

static int rb_op(rb_t* a, const rb_t* b, int op)
{
    rb_t nr = { NULL, 0, 0 };
    uint32_t i = 0, j = 0;
    if (a == b) {
        if (op == RB_XOR || op == RB_ANDNOT)
            rb_clear(a);
        return 0;
    }
    while (i < a->n || j < b->n) {
        uint32_t ka = i < a->n ? a->c[i].key : 0x10000u, kb = j < b->n ? b->c[j].key : 0x10000u;
        rb_ct out;
        uint64_t card;
        if (op == RB_AND && (i >= a->n || j >= b->n))
            break;
        if (op == RB_ANDNOT && i >= a->n)
            break;
        if (ka < kb && op != RB_AND) {
            if (rb_reserve(&nr, nr.n + 1) < 0)
                goto oom;
            nr.c[nr.n++] = a->c[i];
            a->c[i].data = NULL;
            a->c[i].type = 0;
            i++;
            continue;
        }
        if (ka < kb) {
            i++;
            continue;
        }
        if (kb < ka && (op == RB_AND || op == RB_ANDNOT)) {
            j++;
            continue;
        }
        if (rb_ct_op(ka == kb ? &a->c[i] : NULL, &b->c[j], op, &out, &card) < 0)
            goto oom;
        if (ka == kb)
            i++;
        j++;
        if (card) {
            if (rb_reserve(&nr, nr.n + 1) < 0) {
                rb_ct_free(&out);
                goto oom;
            }
            nr.c[nr.n++] = out;
        }
    }
    rb_clear(a);
    *a = nr;
    return 0;
oom:
    rb_clear(&nr);
    return -1;
}

static uint64_t rb_op_card(const rb_t* a, const rb_t* b, int op)
{
    uint32_t i = 0, j = 0;
    uint64_t s = 0;
    while (i < a->n || j < b->n) {
        uint32_t ka = i < a->n ? a->c[i].key : 0x10000u, kb = j < b->n ? b->c[j].key : 0x10000u;
        uint64_t card = 0;
        if (ka == kb) {
            rb_ct_op(&a->c[i], &b->c[j], op, NULL, &card);
            i++;
            j++;
        } else if (ka < kb) {
            rb_ct_op(&a->c[i], NULL, op, NULL, &card);
            i++;
        } else {
            rb_ct_op(NULL, &b->c[j], op, NULL, &card);
            j++;
        }
        s += card;
    }
    return s;
}

static uint64_t rb_rank(const rb_t* r, uint32_t x)
{
    uint16_t hi = (uint16_t)(x >> 16), lo = (uint16_t)x;
    uint64_t s = 0;
    uint32_t i;
    for (i = 0; i < r->n && r->c[i].key < hi; i++)
        s += r->c[i].card;
    if (i < r->n && r->c[i].key == hi) {
        const rb_ct* c = &r->c[i];
        if (c->type == RB_FULL)
            s += (uint32_t)lo + 1;
        else if (c->type == RB_ARR) {
            uint32_t p;
            s += rb_arr_has(c, lo, &p) ? p + 1 : p;
        } else {
            const uint64_t* w = (const uint64_t*)c->data;
            uint32_t k;
            for (k = 0; k < (uint32_t)(lo >> 6); k++)
                s += (uint64_t)__builtin_popcountll(w[k]);
            s += (uint64_t)__builtin_popcountll(w[lo >> 6] & (((lo & 63) == 63) ? ~0ULL : ((2ULL << (lo & 63)) - 1)));
        }
    }
    return s;
}

static int rb_select(const rb_t* r, uint64_t i, uint32_t* out)
{
    uint32_t k;
    for (k = 0; k < r->n; k++) {
        const rb_ct* c = &r->c[k];
        if (i >= c->card) {
            i -= c->card;
            continue;
        }
        if (c->type == RB_FULL)
            *out = ((uint32_t)c->key << 16) | (uint32_t)i;
        else if (c->type == RB_ARR)
            *out = ((uint32_t)c->key << 16) | ((const uint16_t*)c->data)[i];
        else {
            const uint64_t* w = (const uint64_t*)c->data;
            uint32_t j;
            for (j = 0; j < RB_WORDS; j++) {
                uint32_t pc = (uint32_t)__builtin_popcountll(w[j]);
                if (i < pc) {
                    uint64_t x = w[j];
                    while (i--)
                        x &= x - 1;
                    *out = ((uint32_t)c->key << 16) | (j * 64 + (uint32_t)__builtin_ctzll(x));
                    return 1;
                }
                i -= pc;
            }
            return 0;
        }
        return 1;
    }
    return 0;
}

static void rb_fill(const rb_t* r, uint32_t* out)
{
    uint32_t k, n = 0;
    for (k = 0; k < r->n; k++) {
        const rb_ct* c = &r->c[k];
        uint32_t base = (uint32_t)c->key << 16, i;
        if (c->type == RB_ARR) {
            const uint16_t* a = (const uint16_t*)c->data;
            for (i = 0; i < c->card; i++)
                out[n++] = base | a[i];
        } else if (c->type == RB_FULL) {
            for (i = 0; i < 65536; i++)
                out[n++] = base | i;
        } else {
            const uint64_t* w = (const uint64_t*)c->data;
            for (i = 0; i < RB_WORDS; i++) {
                uint64_t x = w[i];
                while (x) {
                    out[n++] = base | (i * 64 + (uint32_t)__builtin_ctzll(x));
                    x &= x - 1;
                }
            }
        }
    }
}

static int rb_clone(rb_t* dst, const rb_t* src)
{
    uint32_t i;
    dst->c = NULL;
    dst->n = dst->cap = 0;
    if (rb_reserve(dst, src->n) < 0)
        return -1;
    for (i = 0; i < src->n; i++) {
        if (rb_ct_copy(&dst->c[i], &src->c[i]) < 0) {
            dst->n = i;
            rb_clear(dst);
            return -1;
        }
        dst->n = i + 1;
    }
    return 0;
}

static int rb_u32_cmp(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

typedef struct {
    rb_t r;
} sdb_rb_w;

static JSClassID sdb_rb_class_id;

static void sdb_rb_free(void* native)
{
    sdb_rb_w* w = (sdb_rb_w*)native;
    if (!w)
        return;
    rb_clear(&w->r);
    dyn_nat_free(w);
}

static void sdb_rb_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_rb_free(JS_GetOpaque(val, sdb_rb_class_id));
}

static const JSClassDef sdb_rb_class = {
    "RoaringBitmap",
    .finalizer = sdb_rb_finalizer,
};

static int sdb_rb_add_list(JSContext* ctx, sdb_rb_w* w, uint32_t* v, uint32_t n)
{
    uint32_t i;
    qsort(v, n, sizeof(uint32_t), rb_u32_cmp);
    for (i = 0; i < n; i++)
        if ((i == 0 || v[i] != v[i - 1]) && rb_add(&w->r, v[i]) < 0) {
            ds2_oom(ctx);
            return -1;
        }
    return 0;
}

static JSValue sdb_rb_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_rb_w* w;
    uint32_t* v = NULL;
    uint32_t n = 0;
    JSValue obj;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && sdb_u32_list(ctx, argv[0], "RoaringBitmap", &v, &n))
        return JS_EXCEPTION;
    w = (sdb_rb_w*)dyn_nat_calloc(1, sizeof(*w));
    if (!w) {
        dyn_nat_free(v);
        return ds2_oom(ctx);
    }
    if (n && sdb_rb_add_list(ctx, w, v, n) < 0) {
        dyn_nat_free(v);
        sdb_rb_free(w);
        return JS_EXCEPTION;
    }
    dyn_nat_free(v);
    obj = dyn_plain_wrap(ctx, new_target, sdb_rb_class_id, w, sdb_rb_free);
    return obj;
}

static sdb_rb_w* sdb_rb_get(JSContext* ctx, JSValueConst v)
{
    return (sdb_rb_w*)dyn_plain_get(ctx, v, sdb_rb_class_id);
}

static JSValue sdb_rb_elem(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w* w;
    uint32_t x;
    int rc;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "value", &x))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (rb_add(&w->r, x) < 0)
            return ds2_oom(ctx);
        return JS_DupValue(ctx, this_val);
    case 1:
        rc = rb_remove(&w->r, x);
        if (rc < 0)
            return ds2_oom(ctx);
        return JS_NewBool(ctx, rc);
    case 2:
        return JS_NewBool(ctx, rb_has(&w->r, x));
    case 3:
        return JS_NewInt64(ctx, (int64_t)rb_rank(&w->r, x));
    default:
        rc = rb_add(&w->r, x);
        if (rc < 0)
            return ds2_oom(ctx);
        return JS_NewBool(ctx, rc);
    }
}

static JSValue sdb_rb_add_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_rb_w* w;
    uint32_t *v, n;
    (void)argc;
    if (sdb_u32_list(ctx, argv[0], "addMany", &v, &n))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w || sdb_rb_add_list(ctx, w, v, n) < 0) {
        dyn_nat_free(v);
        return JS_EXCEPTION;
    }
    dyn_nat_free(v);
    return JS_DupValue(ctx, this_val);
}

static int sdb_rb_bounds(JSContext* ctx, JSValueConst a, JSValueConst b, uint64_t* lo, uint64_t* hi)
{
    double x, y;
    if (JS_ToFloat64(ctx, &x, a) || JS_ToFloat64(ctx, &y, b))
        return -1;
    if (!(x >= 0 && x <= 4294967296.0) || x != trunc(x) || !(y >= 0 && y <= 4294967296.0) || y != trunc(y)) {
        JS_ThrowRangeError(ctx, "range bounds must be integers in 0..2^32");
        return -1;
    }
    *lo = (uint64_t)x;
    *hi = (uint64_t)y;
    return 0;
}

static JSValue sdb_rb_range_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w* w;
    uint64_t lo, hi;
    (void)argc;
    if (sdb_rb_bounds(ctx, argv[0], argv[1], &lo, &hi))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 2) {
        uint64_t a = lo ? rb_rank(&w->r, (uint32_t)(lo - 1)) : 0;
        uint64_t b = hi ? rb_rank(&w->r, (uint32_t)(hi - 1)) : 0;
        return JS_NewInt64(ctx, hi > lo ? (int64_t)(b - a) : 0);
    }
    if (rb_range(&w->r, lo, hi, magic == 0) < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_rb_setop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w *w, *o;
    (void)argc;
    o = sdb_rb_get(ctx, argv[0]);
    if (!o)
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (magic < 4) {
        if (rb_op(&w->r, &o->r, magic) < 0)
            return ds2_oom(ctx);
        return JS_DupValue(ctx, this_val);
    }
    if (magic < 8)
        return JS_NewInt64(ctx, (int64_t)rb_op_card(&w->r, &o->r, magic - 4));
    {
        uint64_t inter = rb_op_card(&w->r, &o->r, RB_AND);
        uint64_t ca = rb_card(&w->r), cb = rb_card(&o->r);
        switch (magic) {
        case 8:
            return JS_NewBool(ctx, inter != 0);
        case 9:
            return JS_NewBool(ctx, inter == ca);
        case 10:
            return JS_NewBool(ctx, inter == ca && ca == cb);
        default:
            return JS_NewFloat64(ctx, ca + cb - inter ? (double)inter / (double)(ca + cb - inter) : 1.0);
        }
    }
}

static JSValue sdb_rb_misc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_rb_w* w = sdb_rb_get(ctx, this_val);
    uint32_t x;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (!w->r.n)
            return JS_UNDEFINED;
        return rb_select(&w->r, 0, &x) ? JS_NewInt64(ctx, x) : JS_UNDEFINED;
    case 1:
        if (!w->r.n)
            return JS_UNDEFINED;
        return rb_select(&w->r, rb_card(&w->r) - 1, &x) ? JS_NewInt64(ctx, x) : JS_UNDEFINED;
    case 2: {
        uint64_t n = rb_card(&w->r);
        uint32_t* buf;
        JSValue out;
        if (n > SDB_MAX_ITEMS)
            return JS_ThrowRangeError(ctx, "toArray: %llu values exceed the %u materialisation limit", (unsigned long long)n, SDB_MAX_ITEMS);
        buf = (uint32_t*)dyn_nat_malloc(sizeof(uint32_t) * (size_t)(n + 1));
        if (!buf)
            return ds2_oom(ctx);
        rb_fill(&w->r, buf);
        out = sdb_typed_out(ctx, buf, (size_t)n, JS_TYPED_ARRAY_UINT32);
        dyn_nat_free(buf);
        return out;
    }
    case 3: {
        sdb_rb_w* c = (sdb_rb_w*)dyn_nat_calloc(1, sizeof(*c));
        if (!c || rb_clone(&c->r, &w->r) < 0) {
            dyn_nat_free(c);
            return ds2_oom(ctx);
        }
        return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_rb_class_id, c, sdb_rb_free);
    }
    case 4:
        rb_clear(&w->r);
        return JS_UNDEFINED;
    default:
        return JS_NewBool(ctx, w->r.n == 0);
    }
}

static JSValue sdb_rb_select_m(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_rb_w* w;
    double d;
    uint32_t x;
    (void)argc;
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    w = sdb_rb_get(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (!(d >= 0) || d != trunc(d) || d >= 4294967296.0)
        return JS_UNDEFINED;
    return rb_select(&w->r, (uint64_t)d, &x) ? JS_NewInt64(ctx, x) : JS_UNDEFINED;
}

static JSValue sdb_rb_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_rb_w* w = sdb_rb_get(ctx, this_val);
    uint64_t b = 0;
    uint32_t i;
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, (int64_t)rb_card(&w->r));
    b = sizeof(rb_ct) * (uint64_t)w->r.cap;
    for (i = 0; i < w->r.n; i++) {
        const rb_ct* c = &w->r.c[i];
        b += c->type == RB_ARR ? 2u * c->cap : c->type == RB_BMP ? 8u * RB_WORDS
                                                                 : 0;
    }
    return JS_NewInt64(ctx, (int64_t)b);
}

DS2_ITERATOR(sdb_rb, "toArray")

static const JSCFunctionListEntry sdb_rb_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, sdb_rb_elem, 0),
    JS_CFUNC_MAGIC_DEF("tryAdd", 1, sdb_rb_elem, 4),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_rb_elem, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_rb_elem, 2),
    JS_CFUNC_MAGIC_DEF("rank", 1, sdb_rb_elem, 3),
    JS_CFUNC_DEF("addMany", 1, sdb_rb_add_many),
    JS_CFUNC_MAGIC_DEF("addRange", 2, sdb_rb_range_op, 0),
    JS_CFUNC_MAGIC_DEF("removeRange", 2, sdb_rb_range_op, 1),
    JS_CFUNC_MAGIC_DEF("rangeCardinality", 2, sdb_rb_range_op, 2),
    JS_CFUNC_MAGIC_DEF("and", 1, sdb_rb_setop, RB_AND),
    JS_CFUNC_MAGIC_DEF("or", 1, sdb_rb_setop, RB_OR),
    JS_CFUNC_MAGIC_DEF("xor", 1, sdb_rb_setop, RB_XOR),
    JS_CFUNC_MAGIC_DEF("andNot", 1, sdb_rb_setop, RB_ANDNOT),
    JS_CFUNC_MAGIC_DEF("andCardinality", 1, sdb_rb_setop, 4 + RB_AND),
    JS_CFUNC_MAGIC_DEF("orCardinality", 1, sdb_rb_setop, 4 + RB_OR),
    JS_CFUNC_MAGIC_DEF("xorCardinality", 1, sdb_rb_setop, 4 + RB_XOR),
    JS_CFUNC_MAGIC_DEF("andNotCardinality", 1, sdb_rb_setop, 4 + RB_ANDNOT),
    JS_CFUNC_MAGIC_DEF("intersects", 1, sdb_rb_setop, 8),
    JS_CFUNC_MAGIC_DEF("isSubsetOf", 1, sdb_rb_setop, 9),
    JS_CFUNC_MAGIC_DEF("equals", 1, sdb_rb_setop, 10),
    JS_CFUNC_MAGIC_DEF("jaccard", 1, sdb_rb_setop, 11),
    JS_CFUNC_DEF("select", 1, sdb_rb_select_m),
    JS_CFUNC_MAGIC_DEF("min", 0, sdb_rb_misc, 0),
    JS_CFUNC_MAGIC_DEF("max", 0, sdb_rb_misc, 1),
    JS_CFUNC_MAGIC_DEF("toArray", 0, sdb_rb_misc, 2),
    JS_CFUNC_MAGIC_DEF("clone", 0, sdb_rb_misc, 3),
    JS_CFUNC_MAGIC_DEF("clear", 0, sdb_rb_misc, 4),
    JS_CFUNC_MAGIC_DEF("isEmpty", 0, sdb_rb_misc, 5),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, sdb_rb_iterator),
    JS_CGETSET_MAGIC_DEF("size", sdb_rb_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("bytes", sdb_rb_getter, NULL, 2),
};

typedef struct {
    double m, w;
} td_cent;

typedef struct {
    double delta;
    td_cent* c;
    uint32_t n, nbuf, cap;
    double total, min, max, sum;
} td_t;

#define TD_MIN_DELTA 10.0
#define TD_MAX_DELTA 10000.0

static int td_init(td_t* t, double delta)
{
    memset(t, 0, sizeof(*t));
    t->delta = delta;
    t->cap = (uint32_t)(delta * 6) + 64;
    t->c = (td_cent*)dyn_nat_malloc(sizeof(td_cent) * t->cap);
    t->min = HUGE_VAL;
    t->max = -HUGE_VAL;
    return t->c ? 0 : -1;
}

static void td_free(td_t* t)
{
    dyn_nat_free(t->c);
    t->c = NULL;
}

static int td_cent_cmp(const void* a, const void* b)
{
    double x = ((const td_cent*)a)->m, y = ((const td_cent*)b)->m;
    return (x > y) - (x < y);
}

static double td_k(double q, double d)
{
    return d / (2 * M_PI) * asin(2 * q - 1);
}

static double td_kinv(double k, double d)
{
    if (k <= -d / 4)
        return 0;
    if (k >= d / 4)
        return 1;
    return (sin(k * 2 * M_PI / d) + 1) / 2;
}

static void td_compress(td_t* t)
{
    uint32_t n = t->n + t->nbuf, i, out = 0;
    double total = 0, so_far = 0, qlimit;
    td_cent cur;
    if (t->nbuf == 0)
        return;
    qsort(t->c, n, sizeof(td_cent), td_cent_cmp);
    for (i = 0; i < n; i++)
        total += t->c[i].w;
    cur = t->c[0];
    qlimit = td_kinv(td_k(0, t->delta) + 1, t->delta);
    for (i = 1; i < n; i++) {
        td_cent nx = t->c[i];
        double q = (so_far + cur.w + nx.w) / total;
        if (q <= qlimit) {
            double w = cur.w + nx.w;
            cur.m += (nx.m - cur.m) * nx.w / w;
            cur.w = w;
        } else {
            t->c[out++] = cur;
            so_far += cur.w;
            qlimit = td_kinv(td_k(so_far / total, t->delta) + 1, t->delta);
            cur = nx;
        }
    }
    t->c[out++] = cur;
    t->n = out;
    t->nbuf = 0;
    t->total = total;
}

static void td_add(td_t* t, double x, double w)
{
    if (t->n + t->nbuf >= t->cap)
        td_compress(t);
    if (t->n + t->nbuf >= t->cap)
        return;
    t->c[t->n + t->nbuf].m = x;
    t->c[t->n + t->nbuf].w = w;
    t->nbuf++;
    t->total += w;
    t->sum += x * w;
    if (x < t->min)
        t->min = x;
    if (x > t->max)
        t->max = x;
}

static double td_wavg(double x1, double w1, double x2, double w2)
{
    double lo = x1 < x2 ? x1 : x2, hi = x1 < x2 ? x2 : x1;
    double r;
    if (!(w1 + w2 > 0))
        return (x1 + x2) / 2;
    r = (x1 * w1 + x2 * w2) / (w1 + w2);
    return r < lo ? lo : r > hi ? hi
                                : r;
}

static double td_quantile(td_t* t, double q)
{
    double W, index, so_far;
    uint32_t i, n;
    td_cent* c;
    td_compress(t);
    n = t->n;
    c = t->c;
    if (n == 0)
        return (double)NAN;
    if (n == 1)
        return c[0].m;
    W = t->total;
    index = q * W;
    if (index < 1)
        return t->min;
    if (c[0].w > 1 && index < c[0].w / 2)
        return t->min + (index - 1) / (c[0].w / 2 - 1) * (c[0].m - t->min);
    if (index > W - 1)
        return t->max;
    if (c[n - 1].w > 1 && W - index <= c[n - 1].w / 2)
        return t->max - (W - index - 1) / (c[n - 1].w / 2 - 1) * (t->max - c[n - 1].m);
    so_far = c[0].w / 2;
    for (i = 0; i + 1 < n; i++) {
        double dw = (c[i].w + c[i + 1].w) / 2;
        if (so_far + dw > index) {
            double lu = 0, ru = 0, z1, z2;
            if (c[i].w == 1) {
                if (index - so_far < 0.5)
                    return c[i].m;
                lu = 0.5;
            }
            if (c[i + 1].w == 1) {
                if (so_far + dw - index <= 0.5)
                    return c[i + 1].m;
                ru = 0.5;
            }
            z1 = index - so_far - lu;
            z2 = so_far + dw - index - ru;
            return td_wavg(c[i].m, z2, c[i + 1].m, z1);
        }
        so_far += dw;
    }
    {
        double z1 = index - so_far, z2 = c[n - 1].w / 2 - z1;
        if (z1 < 0)
            z1 = 0;
        if (z2 < 0)
            z2 = 0;
        if (z1 + z2 == 0)
            return c[n - 1].m;
        return td_wavg(c[n - 1].m, z2, t->max, z1);
    }
}

static double td_cdf(td_t* t, double x)
{
    uint32_t i, n;
    td_cent* c;
    double cum = 0, px, py;
    td_compress(t);
    n = t->n;
    c = t->c;
    if (n == 0)
        return (double)NAN;
    if (x < t->min)
        return 0;
    if (x >= t->max)
        return 1;
    if (n == 1)
        return t->max - t->min > 0 ? (x - t->min) / (t->max - t->min) : 0.5;
    px = t->min;
    py = 0;
    for (i = 0; i < n; i++) {
        double kx = c[i].m, ky = cum + c[i].w / 2;
        if (x < kx) {
            if (kx <= px)
                return ky / t->total;
            return (py + (ky - py) * (x - px) / (kx - px)) / t->total;
        }
        px = kx;
        py = ky;
        cum += c[i].w;
    }
    if (t->max <= px)
        return 1;
    return (py + (t->total - py) * (x - px) / (t->max - px)) / t->total;
}

typedef struct {
    td_t t;
} sdb_td_w;

static JSClassID sdb_td_class_id;

static void sdb_td_free(void* native)
{
    sdb_td_w* w = (sdb_td_w*)native;
    if (!w)
        return;
    td_free(&w->t);
    dyn_nat_free(w);
}

static void sdb_td_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_td_free(JS_GetOpaque(val, sdb_td_class_id));
}

static const JSClassDef sdb_td_class = {
    "TDigest",
    .finalizer = sdb_td_finalizer,
};

static JSValue sdb_td_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_td_w* w;
    double d = 100;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (!(d >= TD_MIN_DELTA && d <= TD_MAX_DELTA))
            return JS_ThrowRangeError(ctx, "compression must be in %g..%g", TD_MIN_DELTA, TD_MAX_DELTA);
    }
    w = (sdb_td_w*)dyn_nat_calloc(1, sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    if (td_init(&w->t, d) < 0) {
        dyn_nat_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, sdb_td_class_id, w, sdb_td_free);
}

static JSValue sdb_td_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_td_w* w;
    double x, wt = 1;
    if (sdb_finite_arg(ctx, argv[0], "value", &x))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (JS_ToFloat64(ctx, &wt, argv[1]))
            return JS_EXCEPTION;
        if (!(wt > 0) || !isfinite(wt))
            return JS_ThrowRangeError(ctx, "weight must be a positive finite number");
    }
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w)
        return JS_EXCEPTION;
    td_add(&w->t, x, wt);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_td_add_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_td_w* w;
    double* v;
    uint32_t n, i;
    (void)argc;
    if (sdb_f64_list(ctx, argv[0], "addMany", &v, &n))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        if (!isfinite(v[i])) {
            dyn_nat_free(v);
            return JS_ThrowRangeError(ctx, "addMany: element %u is not finite", (unsigned)i);
        }
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w) {
        dyn_nat_free(v);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++)
        td_add(&w->t, v[i], 1);
    dyn_nat_free(v);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_td_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_td_w* w;
    double x;
    (void)argc;
    if (sdb_num_arg(ctx, argv[0], magic ? "value" : "q", &x))
        return JS_EXCEPTION;
    if (!magic && !(x >= 0 && x <= 1))
        return JS_ThrowRangeError(ctx, "q must be in 0..1");
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, magic ? td_cdf(&w->t, x) : td_quantile(&w->t, x));
}

static JSValue sdb_td_quantiles(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_td_w* w;
    double* q;
    uint32_t n, i;
    JSValue out;
    (void)argc;
    if (sdb_f64_list(ctx, argv[0], "quantiles", &q, &n))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        if (!(q[i] >= 0 && q[i] <= 1)) {
            dyn_nat_free(q);
            return JS_ThrowRangeError(ctx, "quantiles: element %u is not in 0..1", (unsigned)i);
        }
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w) {
        dyn_nat_free(q);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++)
        q[i] = td_quantile(&w->t, q[i]);
    out = sdb_typed_out(ctx, q, n, JS_TYPED_ARRAY_FLOAT64);
    dyn_nat_free(q);
    return out;
}

static JSValue sdb_td_merge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_td_w *w, *o;
    uint32_t i, n;
    td_cent* tmp;
    (void)argc;
    o = (sdb_td_w*)dyn_plain_get(ctx, argv[0], sdb_td_class_id);
    if (!o)
        return JS_EXCEPTION;
    w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w)
        return JS_EXCEPTION;
    td_compress(&o->t);
    n = o->t.n;
    tmp = (td_cent*)dyn_nat_malloc(sizeof(td_cent) * ((size_t)n + 1));
    if (!tmp)
        return ds2_oom(ctx);
    memcpy(tmp, o->t.c, sizeof(td_cent) * n);
    {
        double omin = o->t.min, omax = o->t.max, osum = o->t.sum;
        for (i = 0; i < n; i++)
            td_add(&w->t, tmp[i].m, tmp[i].w);
        if (n) {
            w->t.sum += osum;
            for (i = 0; i < n; i++)
                w->t.sum -= tmp[i].m * tmp[i].w;
            if (omin < w->t.min)
                w->t.min = omin;
            if (omax > w->t.max)
                w->t.max = omax;
        }
    }
    dyn_nat_free(tmp);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_td_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_td_w* w = (sdb_td_w*)dyn_plain_get(ctx, this_val, sdb_td_class_id);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewFloat64(ctx, w->t.total);
    case 1:
        return w->t.total > 0 ? JS_NewFloat64(ctx, w->t.min) : JS_UNDEFINED;
    case 2:
        return w->t.total > 0 ? JS_NewFloat64(ctx, w->t.max) : JS_UNDEFINED;
    case 3:
        return w->t.total > 0 ? JS_NewFloat64(ctx, w->t.sum / w->t.total) : JS_UNDEFINED;
    case 4:
        td_compress(&w->t);
        return JS_NewInt64(ctx, w->t.n);
    default:
        return JS_NewFloat64(ctx, w->t.delta);
    }
}

static const JSCFunctionListEntry sdb_td_proto[] = {
    JS_CFUNC_DEF("add", 1, sdb_td_add),
    JS_CFUNC_DEF("addMany", 1, sdb_td_add_many),
    JS_CFUNC_MAGIC_DEF("quantile", 1, sdb_td_query, 0),
    JS_CFUNC_MAGIC_DEF("cdf", 1, sdb_td_query, 1),
    JS_CFUNC_DEF("quantiles", 1, sdb_td_quantiles),
    JS_CFUNC_DEF("merge", 1, sdb_td_merge),
    JS_CGETSET_MAGIC_DEF("count", sdb_td_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("min", sdb_td_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("max", sdb_td_getter, NULL, 2),
    JS_CGETSET_MAGIC_DEF("mean", sdb_td_getter, NULL, 3),
    JS_CGETSET_MAGIC_DEF("centroids", sdb_td_getter, NULL, 4),
    JS_CGETSET_MAGIC_DEF("compression", sdb_td_getter, NULL, 5),
};

typedef struct {
    char* key;
    uint32_t klen;
    uint32_t hpos;
    double count;
    double err;
} tk_slot;

typedef struct {
    uint32_t k, n;
    tk_slot* s;
    uint32_t* heap;
    sdb_smap idx;
    double total;
} tk_t;

#define TK_MAX_K (1u << 20)

static void tk_swap(tk_t* t, uint32_t a, uint32_t b)
{
    uint32_t x = t->heap[a];
    t->heap[a] = t->heap[b];
    t->heap[b] = x;
    t->s[t->heap[a]].hpos = a;
    t->s[t->heap[b]].hpos = b;
}

static int tk_less(const tk_t* t, uint32_t a, uint32_t b)
{
    return t->s[t->heap[a]].count < t->s[t->heap[b]].count;
}

static void tk_down(tk_t* t, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < t->n && tk_less(t, l, m))
            m = l;
        if (r < t->n && tk_less(t, r, m))
            m = r;
        if (m == i)
            return;
        tk_swap(t, i, m);
        i = m;
    }
}

static void tk_up(tk_t* t, uint32_t i)
{
    while (i) {
        uint32_t p = (i - 1) / 2;
        if (!tk_less(t, i, p))
            return;
        tk_swap(t, i, p);
        i = p;
    }
}

static void tk_free(tk_t* t)
{
    uint32_t i;
    for (i = 0; i < t->n; i++)
        dyn_nat_free(t->s[i].key);
    dyn_nat_free(t->s);
    dyn_nat_free(t->heap);
    sdb_smap_free(&t->idx);
}

static int tk_add(tk_t* t, const char* k, size_t n, double c, char** evicted, size_t* evn)
{
    uint64_t h = sdb_hash(k, n);
    sdb_sent* e = sdb_smap_find(&t->idx, k, n, h);
    *evicted = NULL;
    t->total += c;
    if (e) {
        tk_slot* s = &t->s[e->v];
        s->count += c;
        tk_down(t, s->hpos);
        return 0;
    }
    if (n > UINT32_MAX - 1)
        return -1;
    if (t->n < t->k) {
        uint32_t i = t->n;
        tk_slot* s = &t->s[i];
        s->key = (char*)sdb_bytes_dup(k, n);
        if (!s->key)
            return -1;
        s->klen = (uint32_t)n;
        s->count = c;
        s->err = 0;
        if (!sdb_smap_insert(&t->idx, s->key, n, h, i)) {
            dyn_nat_free(s->key);
            return -1;
        }
        t->heap[i] = i;
        s->hpos = i;
        t->n++;
        tk_up(t, i);
        return 0;
    }
    {
        uint32_t si = t->heap[0];
        tk_slot* s = &t->s[si];
        char* nk = (char*)sdb_bytes_dup(k, n);
        sdb_sent* old;
        if (!nk)
            return -1;
        old = sdb_smap_find(&t->idx, s->key, s->klen, sdb_hash(s->key, s->klen));
        if (old)
            sdb_smap_remove(&t->idx, old);
        if (!sdb_smap_insert(&t->idx, nk, n, h, si)) {
            sdb_smap_insert(&t->idx, s->key, s->klen, sdb_hash(s->key, s->klen), si);
            dyn_nat_free(nk);
            return -1;
        }
        *evicted = s->key;
        *evn = s->klen;
        s->key = nk;
        s->klen = (uint32_t)n;
        s->err = s->count;
        s->count += c;
        tk_down(t, 0);
        return 0;
    }
}

typedef struct {
    tk_t t;
} sdb_tk_w;

static JSClassID sdb_tk_class_id;

static void sdb_tk_free(void* native)
{
    sdb_tk_w* w = (sdb_tk_w*)native;
    if (!w)
        return;
    tk_free(&w->t);
    dyn_nat_free(w);
}

static void sdb_tk_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_tk_free(JS_GetOpaque(val, sdb_tk_class_id));
}

static const JSClassDef sdb_tk_class = {
    "TopK",
    .finalizer = sdb_tk_finalizer,
};

static sdb_tk_w* sdb_tk_new(uint32_t k)
{
    sdb_tk_w* w = (sdb_tk_w*)dyn_nat_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->t.k = k;
    w->t.s = (tk_slot*)dyn_nat_calloc(k, sizeof(tk_slot));
    w->t.heap = (uint32_t*)dyn_nat_calloc(k, sizeof(uint32_t));
    if (!w->t.s || !w->t.heap) {
        sdb_tk_free(w);
        return NULL;
    }
    return w;
}

static JSValue sdb_tk_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_tk_w* w;
    uint32_t k;
    (void)argc;
    if (dyn_size_arg(ctx, argv[0], TK_MAX_K, &k))
        return JS_EXCEPTION;
    if (k == 0)
        return JS_ThrowRangeError(ctx, "k must be at least 1");
    w = sdb_tk_new(k);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_tk_class_id, w, sdb_tk_free);
}

static JSValue sdb_tk_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_tk_w* w;
    ds2_str k;
    double c = 1;
    char* ev;
    size_t evn = 0;
    JSValue r;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (JS_ToFloat64(ctx, &c, argv[1]))
            return JS_EXCEPTION;
        if (!(c > 0) || !isfinite(c))
            return JS_ThrowRangeError(ctx, "count must be a positive finite number");
    }
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (tk_add(&w->t, k.p, k.n, c, &ev, &evn) < 0) {
        ds2_str_release(ctx, &k);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &k);
    if (!ev)
        return JS_UNDEFINED;
    r = JS_NewStringLen(ctx, ev, evn);
    dyn_nat_free(ev);
    return r;
}

static JSValue sdb_tk_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_tk_w* w;
    ds2_str k;
    sdb_sent* e;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    e = sdb_smap_find(&w->t.idx, k.p, k.n, sdb_hash(k.p, k.n));
    ds2_str_release(ctx, &k);
    if (magic == 1)
        return JS_NewBool(ctx, e != NULL);
    if (magic == 2)
        return e ? JS_NewFloat64(ctx, w->t.s[e->v].err) : JS_UNDEFINED;
    return JS_NewFloat64(ctx, e ? w->t.s[e->v].count : 0);
}

static _Thread_local const tk_t* tk_sort_ctx;

static int tk_cmp_desc(const void* a, const void* b)
{
    const tk_slot* x = &tk_sort_ctx->s[*(const uint32_t*)a];
    const tk_slot* y = &tk_sort_ctx->s[*(const uint32_t*)b];
    uint32_t m;
    int c;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    m = x->klen < y->klen ? x->klen : y->klen;
    c = m ? memcmp(x->key, y->key, m) : 0;
    return c ? c : (x->klen > y->klen) - (x->klen < y->klen);
}

static void tk_sorted(const tk_t* t, uint32_t* ord)
{
    uint32_t i;
    for (i = 0; i < t->n; i++)
        ord[i] = i;
    tk_sort_ctx = t;
    qsort(ord, t->n, sizeof(uint32_t), tk_cmp_desc);
    tk_sort_ctx = NULL;
}

static JSValue sdb_tk_list(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_tk_w* w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    uint32_t *ord, i, out = 0;
    JSValue arr;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    ord = (uint32_t*)dyn_nat_malloc(sizeof(uint32_t) * ((size_t)w->t.n + 1));
    if (!ord)
        return ds2_oom(ctx);
    tk_sorted(&w->t, ord);
    arr = JS_NewArray(ctx);
    for (i = 0; i < w->t.n && !JS_IsException(arr); i++) {
        const tk_slot* s = &w->t.s[ord[i]];
        JSValue item = JS_NewArray(ctx);
        uint32_t j = 0;
        if (sdb_push(ctx, item, &j, JS_NewStringLen(ctx, s->key, s->klen)) < 0
            || sdb_push(ctx, item, &j, JS_NewFloat64(ctx, s->count)) < 0
            || sdb_push(ctx, item, &j, JS_NewFloat64(ctx, s->err)) < 0
            || sdb_push(ctx, arr, &out, item) < 0) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_nat_free(ord);
    return arr;
}

static JSValue sdb_tk_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_tk_w* w = (sdb_tk_w*)dyn_plain_get(ctx, this_val, sdb_tk_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, w->t.k);
    if (magic == 1)
        return JS_NewInt64(ctx, w->t.n);
    return JS_NewFloat64(ctx, w->t.total);
}

static const JSCFunctionListEntry sdb_tk_proto[] = {
    JS_CFUNC_DEF("add", 1, sdb_tk_add),
    JS_CFUNC_MAGIC_DEF("count", 1, sdb_tk_query, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_tk_query, 1),
    JS_CFUNC_MAGIC_DEF("error", 1, sdb_tk_query, 2),
    JS_CFUNC_DEF("list", 0, sdb_tk_list),
    JS_CGETSET_MAGIC_DEF("k", sdb_tk_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("size", sdb_tk_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("total", sdb_tk_getter, NULL, 2),
};

#define CF_SLOTS 4
#define CF_MAX_KICKS 500
#define CF_MAX_CAPACITY (1u << 26)

typedef struct {
    uint16_t* t;
    uint32_t nb, mask;
    uint32_t count;
    uint16_t victim_fp;
    uint32_t victim_i;
    int has_victim;
    uint64_t rng;
} cf_t;

static uint32_t cf_alt(const cf_t* c, uint32_t i, uint16_t fp)
{
    return (i ^ (uint32_t)(((uint64_t)fp * 0x5bd1e995u) >> 0)) & c->mask;
}

static void cf_hash(const cf_t* c, const char* k, size_t n, uint32_t* i, uint16_t* fp)
{
    uint64_t h = sdb_hash_fixed(k, n, 0xCF);
    uint16_t f = (uint16_t)(h >> 48);
    if (!f)
        f = 1;
    *fp = f;
    *i = (uint32_t)h & c->mask;
}

static int cf_put(cf_t* c, uint32_t i, uint16_t fp)
{
    uint32_t s;
    for (s = 0; s < CF_SLOTS; s++)
        if (!c->t[i * CF_SLOTS + s]) {
            c->t[i * CF_SLOTS + s] = fp;
            return 1;
        }
    return 0;
}

static int cf_has_in(const cf_t* c, uint32_t i, uint16_t fp)
{
    uint32_t s;
    for (s = 0; s < CF_SLOTS; s++)
        if (c->t[i * CF_SLOTS + s] == fp)
            return 1;
    return 0;
}

static int cf_del_in(cf_t* c, uint32_t i, uint16_t fp)
{
    uint32_t s;
    for (s = 0; s < CF_SLOTS; s++)
        if (c->t[i * CF_SLOTS + s] == fp) {
            c->t[i * CF_SLOTS + s] = 0;
            return 1;
        }
    return 0;
}

static int cf_insert(cf_t* c, uint32_t i, uint16_t fp)
{
    uint32_t i2 = cf_alt(c, i, fp), k;
    if (c->has_victim)
        return 0;
    if (cf_put(c, i, fp) || cf_put(c, i2, fp)) {
        c->count++;
        return 1;
    }
    i = (dyn_splitmix64(&c->rng) & 1) ? i : i2;
    for (k = 0; k < CF_MAX_KICKS; k++) {
        uint32_t s = (uint32_t)(dyn_splitmix64(&c->rng) % CF_SLOTS);
        uint16_t old = c->t[i * CF_SLOTS + s];
        c->t[i * CF_SLOTS + s] = fp;
        fp = old;
        i = cf_alt(c, i, fp);
        if (cf_put(c, i, fp)) {
            c->count++;
            return 1;
        }
    }
    c->victim_fp = fp;
    c->victim_i = i;
    c->has_victim = 1;
    c->count++;
    return 1;
}

static int cf_contains(const cf_t* c, uint32_t i, uint16_t fp)
{
    uint32_t i2 = cf_alt(c, i, fp);
    if (cf_has_in(c, i, fp) || cf_has_in(c, i2, fp))
        return 1;
    return c->has_victim && c->victim_fp == fp && (c->victim_i == i || c->victim_i == i2);
}

static int cf_delete(cf_t* c, uint32_t i, uint16_t fp)
{
    uint32_t i2 = cf_alt(c, i, fp);
    if (cf_del_in(c, i, fp) || cf_del_in(c, i2, fp)) {
        c->count--;
        if (c->has_victim) {
            uint16_t vf = c->victim_fp;
            uint32_t vi = c->victim_i;
            c->has_victim = 0;
            c->count--;
            cf_insert(c, vi, vf);
        }
        return 1;
    }
    if (c->has_victim && c->victim_fp == fp && (c->victim_i == i || c->victim_i == i2)) {
        c->has_victim = 0;
        c->count--;
        return 1;
    }
    return 0;
}

typedef struct {
    cf_t c;
} sdb_cf_w;

static JSClassID sdb_cf_class_id;

static void sdb_cf_free(void* native)
{
    sdb_cf_w* w = (sdb_cf_w*)native;
    if (!w)
        return;
    dyn_nat_free(w->c.t);
    dyn_nat_free(w);
}

static void sdb_cf_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_cf_free(JS_GetOpaque(val, sdb_cf_class_id));
}

static const JSClassDef sdb_cf_class = {
    "CuckooFilter",
    .finalizer = sdb_cf_finalizer,
};

static sdb_cf_w* sdb_cf_new(uint32_t nb)
{
    sdb_cf_w* w = (sdb_cf_w*)dyn_nat_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->c.nb = nb;
    w->c.mask = nb - 1;
    w->c.rng = SDB_FIXED_SEED;
    w->c.t = (uint16_t*)dyn_nat_calloc((size_t)nb * CF_SLOTS, sizeof(uint16_t));
    if (!w->c.t) {
        dyn_nat_free(w);
        return NULL;
    }
    return w;
}

static JSValue sdb_cf_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_cf_w* w;
    uint32_t cap, nb = 1;
    (void)argc;
    if (dyn_size_arg(ctx, argv[0], CF_MAX_CAPACITY, &cap))
        return JS_EXCEPTION;
    if (cap == 0)
        return JS_ThrowRangeError(ctx, "capacity must be at least 1");
    while ((uint64_t)nb * CF_SLOTS * 95 < (uint64_t)cap * 100)
        nb <<= 1;
    w = sdb_cf_new(nb);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_cf_class_id, w, sdb_cf_free);
}

static JSValue sdb_cf_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_cf_w* w;
    ds2_str k;
    uint32_t i;
    uint16_t fp;
    int r;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_cf_w*)dyn_plain_get(ctx, this_val, sdb_cf_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    cf_hash(&w->c, k.p, k.n, &i, &fp);
    ds2_str_release(ctx, &k);
    r = magic == 0 ? cf_insert(&w->c, i, fp) : magic == 1 ? cf_contains(&w->c, i, fp)
                                                          : cf_delete(&w->c, i, fp);
    return JS_NewBool(ctx, r);
}

static JSValue sdb_cf_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_cf_w* w = (sdb_cf_w*)dyn_plain_get(ctx, this_val, sdb_cf_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, w->c.count);
    if (magic == 1)
        return JS_NewInt64(ctx, (int64_t)w->c.nb * CF_SLOTS);
    return JS_NewFloat64(ctx, (double)w->c.count / ((double)w->c.nb * CF_SLOTS));
}

static const JSCFunctionListEntry sdb_cf_proto[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, sdb_cf_op, 0),
    JS_CFUNC_MAGIC_DEF("mayContain", 1, sdb_cf_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_cf_op, 2),
    JS_CGETSET_MAGIC_DEF("size", sdb_cf_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("capacity", sdb_cf_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("loadFactor", sdb_cf_getter, NULL, 2),
};

typedef struct {
    double p;
    uint32_t id;
} ih_ent;

typedef struct {
    ih_ent* h;
    uint32_t n, cap;
    int max;
    sdb_imap pos;
} ih_t;

static int ih_before(const ih_t* q, uint32_t a, uint32_t b)
{
    const ih_ent *x = &q->h[a], *y = &q->h[b];
    if (x->p != y->p)
        return q->max ? x->p > y->p : x->p < y->p;
    return x->id < y->id;
}

static void ih_swap(ih_t* q, uint32_t a, uint32_t b)
{
    ih_ent t = q->h[a];
    sdb_ient* s;
    q->h[a] = q->h[b];
    q->h[b] = t;
    s = sdb_imap_find(&q->pos, q->h[a].id);
    if (s)
        s->v = a;
    s = sdb_imap_find(&q->pos, q->h[b].id);
    if (s)
        s->v = b;
}

static uint32_t ih_up(ih_t* q, uint32_t i)
{
    while (i) {
        uint32_t p = (i - 1) / 2;
        if (!ih_before(q, i, p))
            break;
        ih_swap(q, i, p);
        i = p;
    }
    return i;
}

static void ih_down(ih_t* q, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < q->n && ih_before(q, l, m))
            m = l;
        if (r < q->n && ih_before(q, r, m))
            m = r;
        if (m == i)
            return;
        ih_swap(q, i, m);
        i = m;
    }
}

static int ih_push(ih_t* q, uint32_t id, double p)
{
    sdb_ient* s = sdb_imap_find(&q->pos, id);
    if (s) {
        uint32_t i = s->v;
        if (q->h[i].p == p)
            return 0;
        q->h[i].p = p;
        ih_down(q, ih_up(q, i));
        return 0;
    }
    if (q->n == q->cap) {
        uint32_t nc = q->cap ? q->cap * 2 : 16;
        ih_ent* nh;
        if (nc > SDB_MAX_ITEMS)
            return -1;
        nh = (ih_ent*)dyn_nat_realloc(q->h, sizeof(ih_ent) * nc);
        if (!nh)
            return -1;
        q->h = nh;
        q->cap = nc;
    }
    if (!sdb_imap_put(&q->pos, id, q->n))
        return -1;
    q->h[q->n].p = p;
    q->h[q->n].id = id;
    q->n++;
    ih_up(q, q->n - 1);
    return 0;
}

static void ih_remove_at(ih_t* q, uint32_t i)
{
    sdb_ient* s = sdb_imap_find(&q->pos, q->h[i].id);
    uint32_t last = q->n - 1;
    if (s)
        sdb_imap_remove(&q->pos, s);
    q->n--;
    if (i == last)
        return;
    q->h[i] = q->h[last];
    s = sdb_imap_find(&q->pos, q->h[i].id);
    if (s)
        s->v = i;
    ih_down(q, ih_up(q, i));
}

typedef struct {
    ih_t q;
} sdb_ih_w;

static JSClassID sdb_ih_class_id;

static void sdb_ih_free(void* native)
{
    sdb_ih_w* w = (sdb_ih_w*)native;
    if (!w)
        return;
    dyn_nat_free(w->q.h);
    sdb_imap_free(&w->q.pos);
    dyn_nat_free(w);
}

static void sdb_ih_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_ih_free(JS_GetOpaque(val, sdb_ih_class_id));
}

static const JSClassDef sdb_ih_class = {
    "IndexedHeap",
    .finalizer = sdb_ih_finalizer,
};

static sdb_ih_w* sdb_ih_new(int max)
{
    sdb_ih_w* w = (sdb_ih_w*)dyn_nat_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->q.max = max;
    sdb_imap_init(&w->q.pos);
    return w;
}

static JSValue sdb_ih_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w;
    int max = 0;
    if (argc >= 1 && sdb_opt_bool(ctx, argv[0], "max", &max))
        return JS_EXCEPTION;
    w = sdb_ih_new(max);
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_ih_class_id, w, sdb_ih_free);
}

static JSValue sdb_ih_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w;
    uint32_t id;
    double p;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id) || sdb_num_arg(ctx, argv[1], "priority", &p))
        return JS_EXCEPTION;
    w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (ih_push(&w->q, id, p) < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_ih_push_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w;
    uint32_t *ids, n, np, i;
    double* ps;
    (void)argc;
    if (sdb_u32_list(ctx, argv[0], "pushMany ids", &ids, &n))
        return JS_EXCEPTION;
    if (sdb_f64_list(ctx, argv[1], "pushMany priorities", &ps, &np)) {
        dyn_nat_free(ids);
        return JS_EXCEPTION;
    }
    if (n != np) {
        dyn_nat_free(ids);
        dyn_nat_free(ps);
        return JS_ThrowRangeError(ctx, "pushMany: %u ids but %u priorities", (unsigned)n, (unsigned)np);
    }
    for (i = 0; i < n; i++)
        if (ps[i] != ps[i]) {
            dyn_nat_free(ids);
            dyn_nat_free(ps);
            return JS_ThrowTypeError(ctx, "pushMany: priority %u is NaN", (unsigned)i);
        }
    w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    for (i = 0; w && i < n; i++)
        if (ih_push(&w->q, ids[i], ps[i]) < 0) {
            dyn_nat_free(ids);
            dyn_nat_free(ps);
            return ds2_oom(ctx);
        }
    dyn_nat_free(ids);
    dyn_nat_free(ps);
    return w ? JS_DupValue(ctx, this_val) : JS_EXCEPTION;
}

static JSValue sdb_ih_id_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_ih_w* w;
    uint32_t id;
    sdb_ient* s;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id))
        return JS_EXCEPTION;
    w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    if (!w)
        return JS_EXCEPTION;
    s = sdb_imap_find(&w->q.pos, id);
    if (magic == 0)
        return s ? JS_NewFloat64(ctx, w->q.h[s->v].p) : JS_UNDEFINED;
    if (magic == 1)
        return JS_NewBool(ctx, s != NULL);
    if (!s)
        return JS_FALSE;
    ih_remove_at(&w->q, s->v);
    return JS_TRUE;
}

static JSValue sdb_ih_top(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_ih_w* w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    ih_ent e;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    if (!w->q.n)
        return JS_UNDEFINED;
    e = w->q.h[0];
    if (magic == 1)
        return JS_NewInt64(ctx, e.id);
    if (magic == 2)
        return JS_NewFloat64(ctx, e.p);
    ih_remove_at(&w->q, 0);
    if (magic == 3)
        return sdb_pair(ctx, JS_NewInt64(ctx, e.id), JS_NewFloat64(ctx, e.p));
    return JS_NewInt64(ctx, e.id);
}

static JSValue sdb_ih_drain(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w;
    double lim;
    uint32_t cap = UINT32_MAX, n = 0, *buf = NULL, bcap = 0;
    JSValue out;
    if (sdb_num_arg(ctx, argv[0], "limit", &lim))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && sdb_u32_arg(ctx, argv[1], "max", &cap))
        return JS_EXCEPTION;
    w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    if (!w)
        return JS_EXCEPTION;
    while (w->q.n && n < cap && (w->q.max ? w->q.h[0].p >= lim : w->q.h[0].p <= lim)) {
        if (n == bcap) {
            uint32_t nc = bcap ? bcap * 2 : 16;
            uint32_t* nb = (uint32_t*)dyn_nat_realloc(buf, sizeof(uint32_t) * nc);
            if (!nb) {
                dyn_nat_free(buf);
                return ds2_oom(ctx);
            }
            buf = nb;
            bcap = nc;
        }
        buf[n++] = w->q.h[0].id;
        ih_remove_at(&w->q, 0);
    }
    out = sdb_typed_out(ctx, buf, n, JS_TYPED_ARRAY_UINT32);
    dyn_nat_free(buf);
    return out;
}

static JSValue sdb_ih_clear(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ih_w* w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    w->q.n = 0;
    sdb_imap_free(&w->q.pos);
    return JS_UNDEFINED;
}

static JSValue sdb_ih_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_ih_w* w = (sdb_ih_w*)dyn_plain_get(ctx, this_val, sdb_ih_class_id);
    if (!w)
        return JS_EXCEPTION;
    return magic ? JS_NewBool(ctx, w->q.max) : JS_NewInt64(ctx, w->q.n);
}

static const JSCFunctionListEntry sdb_ih_proto[] = {
    JS_CFUNC_DEF("push", 2, sdb_ih_push),
    JS_CFUNC_DEF("pushMany", 2, sdb_ih_push_many),
    JS_CFUNC_MAGIC_DEF("priority", 1, sdb_ih_id_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_ih_id_op, 1),
    JS_CFUNC_MAGIC_DEF("delete", 1, sdb_ih_id_op, 2),
    JS_CFUNC_MAGIC_DEF("pop", 0, sdb_ih_top, 0),
    JS_CFUNC_MAGIC_DEF("peek", 0, sdb_ih_top, 1),
    JS_CFUNC_MAGIC_DEF("peekPriority", 0, sdb_ih_top, 2),
    JS_CFUNC_MAGIC_DEF("popEntry", 0, sdb_ih_top, 3),
    JS_CFUNC_DEF("popUntil", 1, sdb_ih_drain),
    JS_CFUNC_DEF("clear", 0, sdb_ih_clear),
    JS_CGETSET_MAGIC_DEF("size", sdb_ih_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("isMax", sdb_ih_getter, NULL, 1),
};

#define KD_LEAF 12
#define KD_MAX_DIMS 64

typedef struct {
    uint32_t lo, hi;
    int32_t l, r;
    double split;
    uint32_t dim;
} kd_node;

typedef struct {
    uint32_t d, n;
    double* pts;
    uint32_t* perm;
    kd_node* nodes;
    uint32_t nn, ncap;
    uint64_t rng;
} kd_t;

static double kd_coord(const kd_t* t, uint32_t idx, uint32_t dim)
{
    return t->pts[(size_t)idx * t->d + dim];
}

static void kd_select(kd_t* t, uint32_t lo, uint32_t hi, uint32_t k, uint32_t dim)
{
    uint32_t* p = t->perm;
    while (hi - lo > 1) {
        uint32_t piv = lo + (uint32_t)(dyn_splitmix64(&t->rng) % (hi - lo));
        double pv = kd_coord(t, p[piv], dim);
        uint32_t lt = lo, i = lo, gt = hi;
        while (i < gt) {
            double v = kd_coord(t, p[i], dim);
            if (v < pv) {
                uint32_t x = p[lt];
                p[lt++] = p[i];
                p[i++] = x;
            } else if (v > pv) {
                uint32_t x = p[--gt];
                p[gt] = p[i];
                p[i] = x;
            } else
                i++;
        }
        if (k < lt)
            hi = lt;
        else if (k >= gt)
            lo = gt;
        else
            return;
    }
}

static int32_t kd_build(kd_t* t, uint32_t lo, uint32_t hi)
{
    int32_t id;
    kd_node* nd;
    if (t->nn == t->ncap) {
        uint32_t nc = t->ncap ? t->ncap * 2 : 64;
        kd_node* nn = (kd_node*)dyn_nat_realloc(t->nodes, sizeof(kd_node) * nc);
        if (!nn)
            return -2;
        t->nodes = nn;
        t->ncap = nc;
    }
    id = (int32_t)t->nn++;
    nd = &t->nodes[id];
    nd->lo = lo;
    nd->hi = hi;
    nd->l = nd->r = -1;
    nd->dim = 0;
    nd->split = 0;
    if (hi - lo > KD_LEAF) {
        uint32_t best = 0, dim, i, mid = lo + (hi - lo) / 2;
        double spread = -1;
        int32_t l, r;
        for (dim = 0; dim < t->d; dim++) {
            double mn = HUGE_VAL, mx = -HUGE_VAL;
            for (i = lo; i < hi; i++) {
                double v = kd_coord(t, t->perm[i], dim);
                if (v < mn)
                    mn = v;
                if (v > mx)
                    mx = v;
            }
            if (mx - mn > spread) {
                spread = mx - mn;
                best = dim;
            }
        }
        if (spread <= 0)
            return id;
        kd_select(t, lo, hi, mid, best);
        t->nodes[id].dim = best;
        t->nodes[id].split = kd_coord(t, t->perm[mid], best);
        l = kd_build(t, lo, mid);
        if (l == -2)
            return -2;
        r = kd_build(t, mid, hi);
        if (r == -2)
            return -2;
        t->nodes[id].l = l;
        t->nodes[id].r = r;
    }
    return id;
}

static void kd_free(kd_t* t)
{
    dyn_nat_free(t->pts);
    dyn_nat_free(t->perm);
    dyn_nat_free(t->nodes);
    memset(t, 0, sizeof(*t));
}

static int kd_init(kd_t* t, double* pts, uint32_t n, uint32_t d)
{
    uint32_t i;
    memset(t, 0, sizeof(*t));
    t->d = d;
    t->n = n;
    t->pts = pts;
    t->rng = SDB_FIXED_SEED;
    t->perm = (uint32_t*)dyn_nat_malloc(sizeof(uint32_t) * ((size_t)n + 1));
    if (!t->perm)
        return -1;
    for (i = 0; i < n; i++)
        t->perm[i] = i;
    if (n && kd_build(t, 0, n) == -2)
        return -1;
    return 0;
}

typedef struct {
    double d2;
    uint32_t i;
} kd_hit;

typedef struct {
    const kd_t* t;
    const double* q;
    kd_hit* h;
    uint32_t k, n;
} kd_knn_ctx;

static int kd_hit_worse(const kd_hit* a, const kd_hit* b)
{
    return a->d2 > b->d2 || (a->d2 == b->d2 && a->i > b->i);
}

static void kd_knn_push(kd_knn_ctx* c, double d2, uint32_t idx)
{
    kd_hit x = { d2, idx };
    uint32_t i;
    if (c->n < c->k) {
        i = c->n++;
        while (i) {
            uint32_t p = (i - 1) / 2;
            if (!kd_hit_worse(&x, &c->h[p]))
                break;
            c->h[i] = c->h[p];
            i = p;
        }
        c->h[i] = x;
        return;
    }
    if (!kd_hit_worse(&c->h[0], &x))
        return;
    i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        const kd_hit* mv = &x;
        if (l < c->n && kd_hit_worse(&c->h[l], mv)) {
            m = l;
            mv = &c->h[l];
        }
        if (r < c->n && kd_hit_worse(&c->h[r], mv))
            m = r;
        if (m == i)
            break;
        c->h[i] = c->h[m];
        i = m;
    }
    c->h[i] = x;
}

static double kd_d2(const kd_t* t, const double* q, uint32_t idx)
{
    const double* p = &t->pts[(size_t)idx * t->d];
    double s = 0;
    uint32_t j;
    for (j = 0; j < t->d; j++) {
        double x = p[j] - q[j];
        s += x * x;
    }
    return s;
}

static void kd_knn(kd_knn_ctx* c, int32_t ni)
{
    const kd_node* nd = &c->t->nodes[ni];
    double diff;
    int32_t first, second;
    if (nd->l < 0) {
        uint32_t i;
        for (i = nd->lo; i < nd->hi; i++) {
            uint32_t idx = c->t->perm[i];
            kd_knn_push(c, kd_d2(c->t, c->q, idx), idx);
        }
        return;
    }
    diff = c->q[nd->dim] - nd->split;
    first = diff < 0 ? nd->l : nd->r;
    second = diff < 0 ? nd->r : nd->l;
    kd_knn(c, first);
    if (c->n < c->k || diff * diff <= c->h[0].d2)
        kd_knn(c, second);
}

static int kd_hit_cmp(const void* a, const void* b)
{
    const kd_hit *x = (const kd_hit*)a, *y = (const kd_hit*)b;
    if (x->d2 != y->d2)
        return x->d2 < y->d2 ? -1 : 1;
    return (x->i > y->i) - (x->i < y->i);
}

typedef struct {
    kd_hit* h;
    uint32_t n, cap;
    int oom;
} kd_list;

static void kd_list_add(kd_list* l, double d2, uint32_t i)
{
    if (l->oom)
        return;
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 32;
        kd_hit* nh = (kd_hit*)dyn_nat_realloc(l->h, sizeof(kd_hit) * nc);
        if (!nh) {
            l->oom = 1;
            return;
        }
        l->h = nh;
        l->cap = nc;
    }
    l->h[l->n].d2 = d2;
    l->h[l->n].i = i;
    l->n++;
}

static void kd_radius(const kd_t* t, int32_t ni, const double* q, double r2, kd_list* out)
{
    const kd_node* nd = &t->nodes[ni];
    double diff;
    if (nd->l < 0) {
        uint32_t i;
        for (i = nd->lo; i < nd->hi; i++) {
            double d2 = kd_d2(t, q, t->perm[i]);
            if (d2 <= r2)
                kd_list_add(out, d2, t->perm[i]);
        }
        return;
    }
    diff = q[nd->dim] - nd->split;
    if (diff <= 0 || diff * diff <= r2)
        kd_radius(t, nd->l, q, r2, out);
    if (diff >= 0 || diff * diff <= r2)
        kd_radius(t, nd->r, q, r2, out);
}

static void kd_box(const kd_t* t, int32_t ni, const double* lo, const double* hi, kd_list* out)
{
    const kd_node* nd = &t->nodes[ni];
    if (nd->l < 0) {
        uint32_t i, j;
        for (i = nd->lo; i < nd->hi; i++) {
            const double* p = &t->pts[(size_t)t->perm[i] * t->d];
            for (j = 0; j < t->d; j++)
                if (p[j] < lo[j] || p[j] > hi[j])
                    break;
            if (j == t->d)
                kd_list_add(out, (double)t->perm[i], t->perm[i]);
        }
        return;
    }
    if (lo[nd->dim] <= nd->split)
        kd_box(t, nd->l, lo, hi, out);
    if (hi[nd->dim] >= nd->split)
        kd_box(t, nd->r, lo, hi, out);
}

typedef struct {
    kd_t t;
} sdb_kd_w;

static JSClassID sdb_kd_class_id;

static void sdb_kd_free(void* native)
{
    sdb_kd_w* w = (sdb_kd_w*)native;
    if (!w)
        return;
    kd_free(&w->t);
    dyn_nat_free(w);
}

static void sdb_kd_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_kd_free(JS_GetOpaque(val, sdb_kd_class_id));
}

static const JSClassDef sdb_kd_class = {
    "KDTree",
    .finalizer = sdb_kd_finalizer,
};

static int sdb_kd_rows(JSContext* ctx, JSValueConst v, double** out, uint32_t* pn, uint32_t* pd)
{
    JSValue lv, row;
    uint32_t n, i, d = 0, rd;
    double *pts = NULL, *r;
    lv = JS_GetPropertyStr(ctx, v, "length");
    if (JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    for (i = 0; i < n; i++) {
        row = JS_GetPropertyUint32(ctx, v, i);
        if (JS_IsException(row)) {
            dyn_nat_free(pts);
            return -1;
        }
        if (sdb_f64_list(ctx, row, "KDTree point", &r, &rd)) {
            JS_FreeValue(ctx, row);
            dyn_nat_free(pts);
            return -1;
        }
        JS_FreeValue(ctx, row);
        if (i == 0) {
            d = rd;
            if (d == 0 || d > KD_MAX_DIMS || (uint64_t)n * d > SDB_MAX_ITEMS) {
                dyn_nat_free(r);
                JS_ThrowRangeError(ctx, "KDTree: points need 1..%d dimensions and at most %u coordinates", KD_MAX_DIMS, SDB_MAX_ITEMS);
                return -1;
            }
            pts = (double*)dyn_nat_malloc(sizeof(double) * (size_t)n * d);
            if (!pts) {
                dyn_nat_free(r);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        } else if (rd != d) {
            dyn_nat_free(r);
            dyn_nat_free(pts);
            JS_ThrowRangeError(ctx, "KDTree: point %u has %u coordinates, expected %u", (unsigned)i, (unsigned)rd, (unsigned)d);
            return -1;
        }
        memcpy(&pts[(size_t)i * d], r, sizeof(double) * d);
        dyn_nat_free(r);
    }
    *out = pts;
    *pn = n;
    *pd = d;
    return 0;
}

static JSValue sdb_kd_wrap(JSContext* ctx, JSValueConst new_target, double* pts, uint32_t n, uint32_t d)
{
    sdb_kd_w* w;
    uint32_t i;
    for (i = 0; i < n * d; i++)
        if (!isfinite(pts[i])) {
            dyn_nat_free(pts);
            return JS_ThrowRangeError(ctx, "KDTree: coordinate %u of point %u is not finite", (unsigned)(i % d), (unsigned)(i / d));
        }
    w = (sdb_kd_w*)dyn_nat_calloc(1, sizeof(*w));
    if (!w) {
        dyn_nat_free(pts);
        return ds2_oom(ctx);
    }
    if (kd_init(&w->t, pts, n, d) < 0) {
        sdb_kd_free(w);
        return ds2_oom(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, sdb_kd_class_id, w, sdb_kd_free);
}

static JSValue sdb_kd_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    double* pts;
    uint32_t n, d;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        uint32_t total;
        if (dyn_size_arg(ctx, argv[1], KD_MAX_DIMS, &d))
            return JS_EXCEPTION;
        if (d == 0)
            return JS_ThrowRangeError(ctx, "KDTree: dims must be 1..%d", KD_MAX_DIMS);
        if (sdb_f64_list(ctx, argv[0], "KDTree points", &pts, &total))
            return JS_EXCEPTION;
        if (total % d) {
            dyn_nat_free(pts);
            return JS_ThrowRangeError(ctx, "KDTree: %u coordinates is not a multiple of %u dims", (unsigned)total, (unsigned)d);
        }
        n = total / d;
    } else {
        if (JS_IsArray(ctx, argv[0]) <= 0)
            return JS_ThrowTypeError(ctx, "KDTree: expected number[][] or a flat array with dims");
        if (sdb_kd_rows(ctx, argv[0], &pts, &n, &d))
            return JS_EXCEPTION;
        if (n == 0)
            return JS_ThrowRangeError(ctx, "KDTree: an empty point list needs an explicit dims argument");
    }
    return sdb_kd_wrap(ctx, new_target, pts, n, d);
}

static int sdb_kd_point(JSContext* ctx, JSValueConst v, uint32_t d, double** out)
{
    uint32_t n, i;
    if (sdb_f64_list(ctx, v, "KDTree query", out, &n))
        return -1;
    if (n != d) {
        dyn_nat_free(*out);
        JS_ThrowRangeError(ctx, "KDTree: query has %u coordinates, expected %u", (unsigned)n, (unsigned)d);
        return -1;
    }
    for (i = 0; i < n; i++)
        if ((*out)[i] != (*out)[i]) {
            dyn_nat_free(*out);
            JS_ThrowRangeError(ctx, "KDTree: query coordinate %u is NaN", (unsigned)i);
            return -1;
        }
    return 0;
}

static JSValue sdb_kd_result(JSContext* ctx, const kd_hit* h, uint32_t n, int sq)
{
    int32_t* idx = (int32_t*)dyn_nat_malloc(sizeof(int32_t) * ((size_t)n + 1));
    double* dist = (double*)dyn_nat_malloc(sizeof(double) * ((size_t)n + 1));
    JSValue o, a, b;
    uint32_t i;
    if (!idx || !dist) {
        dyn_nat_free(idx);
        dyn_nat_free(dist);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        idx[i] = (int32_t)h[i].i;
        dist[i] = sq ? sqrt(h[i].d2) : h[i].d2;
    }
    a = sdb_typed_out(ctx, idx, n, JS_TYPED_ARRAY_INT32);
    b = sdb_typed_out(ctx, dist, n, JS_TYPED_ARRAY_FLOAT64);
    dyn_nat_free(idx);
    dyn_nat_free(dist);
    if (JS_IsException(a) || JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return o;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "indices", a, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, o, "distances", b, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue sdb_kd_knn(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_kd_w* w;
    double* q = NULL;
    uint32_t k = 1, m = 1, i, total;
    kd_knn_ctx c;
    kd_hit* all;
    JSValue r;
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic != 1 && dyn_size_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, SDB_MAX_ITEMS, &k))
        return JS_EXCEPTION;
    if (magic == 2) {
        uint32_t nq;
        if (sdb_f64_list(ctx, argv[0], "knnMany queries", &q, &nq))
            return JS_EXCEPTION;
        if (nq % w->t.d) {
            dyn_nat_free(q);
            return JS_ThrowRangeError(ctx, "knnMany: %u coordinates is not a multiple of %u dims", (unsigned)nq, (unsigned)w->t.d);
        }
        m = nq / w->t.d;
        for (i = 0; i < nq; i++)
            if (q[i] != q[i]) {
                dyn_nat_free(q);
                return JS_ThrowRangeError(ctx, "knnMany: query coordinate %u is NaN", (unsigned)i);
            }
    } else if (sdb_kd_point(ctx, argv[0], w->t.d, &q))
        return JS_EXCEPTION;
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w) {
        dyn_nat_free(q);
        return JS_EXCEPTION;
    }
    if (k > w->t.n)
        k = w->t.n;
    if (magic == 1) {
        if (!w->t.n) {
            dyn_nat_free(q);
            return JS_NewInt32(ctx, -1);
        }
        k = 1;
    }
    total = k * m;
    if ((uint64_t)k * m > SDB_MAX_ITEMS) {
        dyn_nat_free(q);
        return JS_ThrowRangeError(ctx, "knn: result too large");
    }
    all = (kd_hit*)dyn_nat_malloc(sizeof(kd_hit) * ((size_t)total + 1));
    if (!all) {
        dyn_nat_free(q);
        return ds2_oom(ctx);
    }
    for (i = 0; i < m && k; i++) {
        c.t = &w->t;
        c.q = q + (size_t)i * w->t.d;
        c.h = all + (size_t)i * k;
        c.k = k;
        c.n = 0;
        kd_knn(&c, 0);
        qsort(c.h, c.n, sizeof(kd_hit), kd_hit_cmp);
    }
    dyn_nat_free(q);
    if (magic == 1) {
        r = JS_NewInt64(ctx, all[0].i);
        dyn_nat_free(all);
        return r;
    }
    r = sdb_kd_result(ctx, all, total, 1);
    dyn_nat_free(all);
    return r;
}

static JSValue sdb_kd_region(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_kd_w* w;
    double *a = NULL, *b = NULL, r = 0;
    kd_list l = { NULL, 0, 0, 0 };
    JSValue out;
    uint32_t i;
    (void)argc;
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (sdb_kd_point(ctx, argv[0], w->t.d, &a))
        return JS_EXCEPTION;
    if (magic == 0) {
        if (sdb_num_arg(ctx, argv[1], "radius", &r)) {
            dyn_nat_free(a);
            return JS_EXCEPTION;
        }
    } else if (sdb_kd_point(ctx, argv[1], w->t.d, &b)) {
        dyn_nat_free(a);
        return JS_EXCEPTION;
    }
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (w && w->t.n) {
        if (magic == 0) {
            if (r >= 0)
                kd_radius(&w->t, 0, a, r * r, &l);
            qsort(l.h, l.n, sizeof(kd_hit), kd_hit_cmp);
        } else
            kd_box(&w->t, 0, a, b, &l);
    }
    dyn_nat_free(a);
    dyn_nat_free(b);
    if (!w) {
        dyn_nat_free(l.h);
        return JS_EXCEPTION;
    }
    if (l.oom) {
        dyn_nat_free(l.h);
        return ds2_oom(ctx);
    }
    if (magic == 1) {
        uint32_t* ids = (uint32_t*)dyn_nat_malloc(sizeof(uint32_t) * ((size_t)l.n + 1));
        if (!ids) {
            dyn_nat_free(l.h);
            return ds2_oom(ctx);
        }
        for (i = 0; i < l.n; i++)
            ids[i] = l.h[i].i;
        qsort(ids, l.n, sizeof(uint32_t), rb_u32_cmp);
        out = sdb_typed_out(ctx, ids, l.n, JS_TYPED_ARRAY_INT32);
        dyn_nat_free(ids);
    } else
        out = sdb_kd_result(ctx, l.h, l.n, 1);
    dyn_nat_free(l.h);
    return out;
}

static JSValue sdb_kd_point_m(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_kd_w* w;
    uint32_t i;
    (void)argc;
    if (dyn_idx_arg(ctx, argv[0], &i))
        return JS_EXCEPTION;
    w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (i >= w->t.n)
        return JS_UNDEFINED;
    return sdb_typed_out(ctx, &w->t.pts[(size_t)i * w->t.d], w->t.d, JS_TYPED_ARRAY_FLOAT64);
}

static JSValue sdb_kd_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_kd_w* w = (sdb_kd_w*)dyn_plain_get(ctx, this_val, sdb_kd_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, magic ? w->t.d : w->t.n);
}

static const JSCFunctionListEntry sdb_kd_proto[] = {
    JS_CFUNC_MAGIC_DEF("knn", 2, sdb_kd_knn, 0),
    JS_CFUNC_MAGIC_DEF("nearest", 1, sdb_kd_knn, 1),
    JS_CFUNC_MAGIC_DEF("knnMany", 2, sdb_kd_knn, 2),
    JS_CFUNC_MAGIC_DEF("radius", 2, sdb_kd_region, 0),
    JS_CFUNC_MAGIC_DEF("box", 2, sdb_kd_region, 1),
    JS_CFUNC_DEF("point", 1, sdb_kd_point_m),
    JS_CGETSET_MAGIC_DEF("size", sdb_kd_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("dims", sdb_kd_getter, NULL, 1),
};

#define CH_MAX_REPLICAS 1000u
#define CH_MAX_WEIGHT 1000u
#define CH_MAX_POINTS (1u << 22)

typedef struct {
    char* name;
    uint32_t len;
    uint32_t weight;
} ch_node;

typedef struct {
    uint64_t h;
    uint32_t node;
} ch_pt;

typedef struct {
    ch_node* nodes;
    uint32_t nn, ncap;
    ch_pt* ring;
    uint32_t nr;
    uint32_t replicas;
    sdb_smap idx;
} ch_t;

static _Thread_local const ch_t* ch_sort_ctx;

static int ch_pt_cmp(const void* a, const void* b)
{
    const ch_pt *x = (const ch_pt*)a, *y = (const ch_pt*)b;
    const ch_node *nx, *ny;
    uint32_t m;
    int c;
    if (x->h != y->h)
        return x->h < y->h ? -1 : 1;
    nx = &ch_sort_ctx->nodes[x->node];
    ny = &ch_sort_ctx->nodes[y->node];
    m = nx->len < ny->len ? nx->len : ny->len;
    c = m ? memcmp(nx->name, ny->name, m) : 0;
    return c ? c : (nx->len > ny->len) - (nx->len < ny->len);
}

static int ch_rebuild(ch_t* c)
{
    uint64_t total = 0;
    uint32_t i, r, k = 0;
    ch_pt* ring;
    for (i = 0; i < c->nn; i++)
        total += (uint64_t)c->replicas * c->nodes[i].weight;
    if (total > CH_MAX_POINTS)
        return -2;
    ring = (ch_pt*)dyn_nat_malloc(sizeof(ch_pt) * ((size_t)total + 1));
    if (!ring)
        return -1;
    for (i = 0; i < c->nn; i++) {
        size_t len = c->nodes[i].len;
        char* buf = (char*)dyn_nat_malloc(len + 16);
        if (!buf) {
            dyn_nat_free(ring);
            return -1;
        }
        if (len)
            memcpy(buf, c->nodes[i].name, len);
        buf[len] = '#';
        for (r = 0; r < c->replicas * c->nodes[i].weight; r++) {
            int dn = snprintf(buf + len + 1, 15, "%u", (unsigned)r);
            ring[k].h = dyn_xxh64((const uint8_t*)buf, len + 1 + (size_t)dn, 0);
            ring[k].node = i;
            k++;
        }
        dyn_nat_free(buf);
    }
    ch_sort_ctx = c;
    qsort(ring, k, sizeof(ch_pt), ch_pt_cmp);
    ch_sort_ctx = NULL;
    dyn_nat_free(c->ring);
    c->ring = ring;
    c->nr = k;
    return 0;
}

static uint32_t ch_locate(const ch_t* c, uint64_t h)
{
    uint32_t lo = 0, hi = c->nr;
    while (lo < hi) {
        uint32_t m = (lo + hi) >> 1;
        if (c->ring[m].h < h)
            lo = m + 1;
        else
            hi = m;
    }
    return lo == c->nr ? 0 : lo;
}

static void ch_free(ch_t* c)
{
    uint32_t i;
    for (i = 0; i < c->nn; i++)
        dyn_nat_free(c->nodes[i].name);
    dyn_nat_free(c->nodes);
    dyn_nat_free(c->ring);
    sdb_smap_free(&c->idx);
}

static int ch_set_node(ch_t* c, const char* name, size_t n, uint32_t weight)
{
    uint64_t h = sdb_hash(name, n);
    sdb_sent* e = sdb_smap_find(&c->idx, name, n, h);
    int rc;
    if (e) {
        uint32_t old = c->nodes[e->v].weight;
        c->nodes[e->v].weight = weight;
        rc = ch_rebuild(c);
        if (rc < 0)
            c->nodes[e->v].weight = old;
        return rc;
    }
    if (n > UINT32_MAX - 1)
        return -1;
    if (c->nn == c->ncap) {
        uint32_t nc = c->ncap ? c->ncap * 2 : 8;
        ch_node* nn = (ch_node*)dyn_nat_realloc(c->nodes, sizeof(ch_node) * nc);
        if (!nn)
            return -1;
        c->nodes = nn;
        c->ncap = nc;
    }
    c->nodes[c->nn].name = (char*)sdb_bytes_dup(name, n);
    if (!c->nodes[c->nn].name)
        return -1;
    c->nodes[c->nn].len = (uint32_t)n;
    c->nodes[c->nn].weight = weight;
    if (!sdb_smap_insert(&c->idx, c->nodes[c->nn].name, n, h, c->nn)) {
        dyn_nat_free(c->nodes[c->nn].name);
        return -1;
    }
    c->nn++;
    rc = ch_rebuild(c);
    if (rc < 0) {
        c->nn--;
        e = sdb_smap_find(&c->idx, name, n, h);
        if (e)
            sdb_smap_remove(&c->idx, e);
        dyn_nat_free(c->nodes[c->nn].name);
    }
    return rc;
}

static int ch_remove_node(ch_t* c, const char* name, size_t n)
{
    uint64_t h = sdb_hash(name, n);
    sdb_sent* e = sdb_smap_find(&c->idx, name, n, h);
    uint32_t i, last;
    if (!e)
        return 0;
    i = (uint32_t)e->v;
    sdb_smap_remove(&c->idx, e);
    last = c->nn - 1;
    dyn_nat_free(c->nodes[i].name);
    if (i != last) {
        c->nodes[i] = c->nodes[last];
        e = sdb_smap_find(&c->idx, c->nodes[i].name, c->nodes[i].len, sdb_hash(c->nodes[i].name, c->nodes[i].len));
        if (e)
            e->v = i;
    }
    c->nn--;
    if (ch_rebuild(c) < 0) {
        dyn_nat_free(c->ring);
        c->ring = NULL;
        c->nr = 0;
        return -1;
    }
    return 1;
}

typedef struct {
    ch_t c;
} sdb_ch_w;

static JSClassID sdb_ch_class_id;

static void sdb_ch_free(void* native)
{
    sdb_ch_w* w = (sdb_ch_w*)native;
    if (!w)
        return;
    ch_free(&w->c);
    dyn_nat_free(w);
}

static void sdb_ch_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_ch_free(JS_GetOpaque(val, sdb_ch_class_id));
}

static const JSClassDef sdb_ch_class = {
    "ConsistentHash",
    .finalizer = sdb_ch_finalizer,
};

static JSValue sdb_ch_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_ch_w* w;
    uint32_t rep = 160;
    if (argc >= 1 && sdb_opt_u32(ctx, argv[0], "replicas", &rep))
        return JS_EXCEPTION;
    if (rep < 1 || rep > CH_MAX_REPLICAS)
        return JS_ThrowRangeError(ctx, "replicas must be 1..%u", CH_MAX_REPLICAS);
    w = (sdb_ch_w*)dyn_nat_calloc(1, sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    w->c.replicas = rep;
    return dyn_plain_wrap(ctx, new_target, sdb_ch_class_id, w, sdb_ch_free);
}

static JSValue sdb_ch_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ch_w* w;
    ds2_str k;
    uint32_t weight = 1;
    int rc;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (dyn_size_arg(ctx, argv[1], CH_MAX_WEIGHT, &weight))
            return JS_EXCEPTION;
        if (weight == 0)
            return JS_ThrowRangeError(ctx, "weight must be 1..%u", CH_MAX_WEIGHT);
    }
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    rc = ch_set_node(&w->c, k.p, k.n, weight);
    ds2_str_release(ctx, &k);
    if (rc == -2)
        return JS_ThrowRangeError(ctx, "ConsistentHash: more than %u ring points", CH_MAX_POINTS);
    if (rc < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_ch_node_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_ch_w* w;
    ds2_str k;
    JSValue r;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    if (magic == 0) {
        int rc = ch_remove_node(&w->c, k.p, k.n);
        r = rc < 0 ? ds2_oom(ctx) : JS_NewBool(ctx, rc);
    } else if (magic == 1) {
        r = JS_NewBool(ctx, sdb_smap_find(&w->c.idx, k.p, k.n, sdb_hash(k.p, k.n)) != NULL);
    } else {
        if (!w->c.nr)
            r = JS_UNDEFINED;
        else {
            const ch_node* nd = &w->c.nodes[w->c.ring[ch_locate(&w->c, dyn_xxh64((const uint8_t*)k.p, k.n, 0))].node];
            r = JS_NewStringLen(ctx, nd->name, nd->len);
        }
    }
    ds2_str_release(ctx, &k);
    return r;
}

static JSValue sdb_ch_get_n(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ch_w* w;
    ds2_str k;
    uint32_t want, got = 0, i, pos, out = 0;
    uint8_t* seen;
    JSValue arr;
    (void)argc;
    if (dyn_size_arg(ctx, argv[1], 1u << 20, &want))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr) || !w->c.nr || !want) {
        ds2_str_release(ctx, &k);
        return arr;
    }
    if (want > w->c.nn)
        want = w->c.nn;
    seen = (uint8_t*)dyn_nat_calloc(w->c.nn, 1);
    if (!seen) {
        ds2_str_release(ctx, &k);
        JS_FreeValue(ctx, arr);
        return ds2_oom(ctx);
    }
    pos = ch_locate(&w->c, dyn_xxh64((const uint8_t*)k.p, k.n, 0));
    ds2_str_release(ctx, &k);
    for (i = 0; i < w->c.nr && got < want; i++) {
        uint32_t nd = w->c.ring[(pos + i) % w->c.nr].node;
        if (seen[nd])
            continue;
        seen[nd] = 1;
        got++;
        if (sdb_push(ctx, arr, &out, JS_NewStringLen(ctx, w->c.nodes[nd].name, w->c.nodes[nd].len)) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
    }
    dyn_nat_free(seen);
    return arr;
}

static JSValue sdb_ch_nodes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ch_w* w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    JSValue arr;
    uint32_t i, out = 0;
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    for (i = 0; i < w->c.nn && !JS_IsException(arr); i++)
        if (sdb_push(ctx, arr, &out, JS_NewStringLen(ctx, w->c.nodes[i].name, w->c.nodes[i].len)) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    return arr;
}

static JSValue sdb_ch_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_ch_w* w = (sdb_ch_w*)dyn_plain_get(ctx, this_val, sdb_ch_class_id);
    if (!w)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, magic == 0 ? w->c.nn : magic == 1 ? w->c.replicas
                                                              : w->c.nr);
}

static JSValue sdb_ch_jump(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_str k;
    uint32_t buckets;
    uint64_t key;
    int64_t b = -1, j = 0;
    (void)this_val;
    (void)argc;
    if (dyn_size_arg(ctx, argv[1], 0x7FFFFFFFu, &buckets))
        return JS_EXCEPTION;
    if (buckets == 0)
        return JS_ThrowRangeError(ctx, "buckets must be at least 1");
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    key = dyn_xxh64((const uint8_t*)k.p, k.n, 0);
    ds2_str_release(ctx, &k);
    while (j < (int64_t)buckets) {
        b = j;
        key = key * 2862933555777941757ULL + 1;
        j = (int64_t)((double)(b + 1) * ((double)(1LL << 31) / (double)((key >> 33) + 1)));
    }
    return JS_NewInt64(ctx, b);
}

static const JSCFunctionListEntry sdb_ch_proto[] = {
    JS_CFUNC_DEF("addNode", 1, sdb_ch_add),
    JS_CFUNC_MAGIC_DEF("removeNode", 1, sdb_ch_node_op, 0),
    JS_CFUNC_MAGIC_DEF("hasNode", 1, sdb_ch_node_op, 1),
    JS_CFUNC_MAGIC_DEF("get", 1, sdb_ch_node_op, 2),
    JS_CFUNC_DEF("getN", 2, sdb_ch_get_n),
    JS_CFUNC_DEF("nodes", 0, sdb_ch_nodes),
    JS_CGETSET_MAGIC_DEF("size", sdb_ch_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("replicas", sdb_ch_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("points", sdb_ch_getter, NULL, 2),
};

#define II_TOK_MAX 128
#define II_MAX_GROUPS 32

typedef struct {
    uint32_t doc;
    uint32_t tf;
} ii_post;

typedef struct {
    char* s;
    uint32_t len;
    uint32_t n, cap;
    ii_post* p;
} ii_term;

typedef struct {
    uint32_t term;
    uint32_t tf;
} ii_dt;

typedef struct {
    uint32_t id;
    uint32_t len;
    uint32_t nt;
    uint32_t alive;
    ii_dt* t;
} ii_doc;

typedef struct {
    ii_term* terms;
    uint32_t nterms, tcap;
    sdb_smap tidx;
    ii_doc* docs;
    uint32_t nslots, dcap;
    uint32_t* freel;
    uint32_t nfree, fcap;
    sdb_imap didx;
    uint32_t live;
    uint64_t total_len;
    double k1, b;
    sdb_smap stop;
    char** stopw;
    uint32_t nstop;
    double* acc;
    uint32_t* stamp;
    uint32_t* mask;
    uint32_t* touched;
    uint32_t scap, epoch;
    uint32_t* tstamp;
    uint32_t* tcount;
    uint32_t tscap, tepoch;
} ii_t;

static int ii_word_byte(const unsigned char* s, size_t n, size_t i, size_t* skip)
{
    unsigned char c = s[i];
    *skip = 1;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
        return 1;
    if (c < 0x80)
        return 0;
    if (c == 0xC2 && i + 1 < n && s[i + 1] >= 0xA0 && s[i + 1] <= 0xBF && s[i + 1] != 0xAA && s[i + 1] != 0xB5 && s[i + 1] != 0xBA) {
        *skip = 2;
        return 0;
    }
    if (c == 0xE2 && i + 2 < n && (s[i + 1] == 0x80 || s[i + 1] == 0x81)) {
        *skip = 3;
        return 0;
    }
    if (c == 0xE3 && i + 2 < n && s[i + 1] == 0x80) {
        *skip = 3;
        return 0;
    }
    if (c == 0xEF && i + 2 < n && ((s[i + 1] == 0xBC && s[i + 2] >= 0x81 && s[i + 2] <= 0x8F) || (s[i + 1] == 0xBD && s[i + 2] >= 0x9B))) {
        *skip = 3;
        return 0;
    }
    return 1;
}

typedef int (*ii_tok_fn)(void* ud, const char* tok, size_t n);

static int ii_tokenize(const char* str, size_t n, ii_tok_fn fn, void* ud)
{
    const unsigned char* s = (const unsigned char*)str;
    char buf[II_TOK_MAX];
    size_t i = 0, skip;
    while (i < n) {
        size_t len = 0;
        int trunc = 0;
        while (i < n && !ii_word_byte(s, n, i, &skip))
            i += skip;
        while (i < n && ii_word_byte(s, n, i, &skip)) {
            unsigned char c = s[i];
            if (len < II_TOK_MAX)
                buf[len++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            else
                trunc = 1;
            i++;
        }
        if (trunc) {
            size_t k = len;
            while (k > 0 && ((unsigned char)buf[k - 1] & 0xC0) == 0x80)
                k--;
            if (k > 0) {
                unsigned char lead = (unsigned char)buf[k - 1];
                size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3
                    : lead >= 0xC0                            ? 2
                                                              : 1;
                if (len - (k - 1) < need)
                    len = k - 1;
            }
        }
        if (len) {
            int rc = fn(ud, buf, len);
            if (rc)
                return rc;
        }
    }
    return 0;
}

static int ii_is_stop(const ii_t* x, const char* t, size_t n)
{
    return x->nstop && sdb_smap_find(&x->stop, t, n, sdb_hash(t, n)) != NULL;
}

static int64_t ii_term_get(ii_t* x, const char* t, size_t n, int create)
{
    uint64_t h = sdb_hash(t, n);
    sdb_sent* e = sdb_smap_find(&x->tidx, t, n, h);
    ii_term* tm;
    if (e)
        return (int64_t)e->v;
    if (!create)
        return -1;
    if (x->nterms == x->tcap) {
        uint32_t nc = x->tcap ? x->tcap * 2 : 64;
        ii_term* nt;
        if (nc > SDB_MAX_ITEMS)
            return -2;
        nt = (ii_term*)dyn_nat_realloc(x->terms, sizeof(ii_term) * nc);
        if (!nt)
            return -2;
        x->terms = nt;
        x->tcap = nc;
    }
    tm = &x->terms[x->nterms];
    memset(tm, 0, sizeof(*tm));
    tm->s = (char*)sdb_bytes_dup(t, n);
    if (!tm->s)
        return -2;
    tm->len = (uint32_t)n;
    if (!sdb_smap_insert(&x->tidx, tm->s, n, h, x->nterms)) {
        dyn_nat_free(tm->s);
        return -2;
    }
    return (int64_t)x->nterms++;
}

static int ii_tscratch(ii_t* x)
{
    if (x->tscap < x->nterms) {
        uint32_t nc = x->tcap;
        uint32_t* a = (uint32_t*)dyn_nat_realloc(x->tstamp, sizeof(uint32_t) * nc);
        uint32_t* b;
        if (!a)
            return -1;
        x->tstamp = a;
        b = (uint32_t*)dyn_nat_realloc(x->tcount, sizeof(uint32_t) * nc);
        if (!b)
            return -1;
        x->tcount = b;
        memset(x->tstamp + x->tscap, 0, sizeof(uint32_t) * (nc - x->tscap));
        x->tscap = nc;
    }
    return 0;
}

typedef struct {
    ii_t* x;
    ii_dt* list;
    uint32_t n, cap;
    uint32_t len;
    int oom;
} ii_add_ctx;

static int ii_add_tok(void* ud, const char* t, size_t n)
{
    ii_add_ctx* c = (ii_add_ctx*)ud;
    ii_t* x = c->x;
    int64_t ti;
    if (ii_is_stop(x, t, n))
        return 0;
    ti = ii_term_get(x, t, n, 1);
    if (ti < 0 || ii_tscratch(x) < 0) {
        c->oom = 1;
        return 1;
    }
    c->len++;
    if (x->tstamp[ti] == x->tepoch) {
        c->list[x->tcount[ti]].tf++;
        return 0;
    }
    if (c->n == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 16;
        ii_dt* nl = (ii_dt*)dyn_nat_realloc(c->list, sizeof(ii_dt) * nc);
        if (!nl) {
            c->oom = 1;
            return 1;
        }
        c->list = nl;
        c->cap = nc;
    }
    x->tstamp[ti] = x->tepoch;
    x->tcount[ti] = c->n;
    c->list[c->n].term = (uint32_t)ti;
    c->list[c->n].tf = 1;
    c->n++;
    return 0;
}

static void ii_bump_tepoch(ii_t* x)
{
    if (++x->tepoch == 0) {
        if (x->tstamp)
            memset(x->tstamp, 0, sizeof(uint32_t) * x->tscap);
        x->tepoch = 1;
    }
}

static int ii_post_push(ii_term* tm, uint32_t slot, uint32_t tf)
{
    if (tm->n == tm->cap) {
        uint32_t nc = tm->cap ? tm->cap * 2 : 4;
        ii_post* np = (ii_post*)dyn_nat_realloc(tm->p, sizeof(ii_post) * nc);
        if (!np)
            return -1;
        tm->p = np;
        tm->cap = nc;
    }
    tm->p[tm->n].doc = slot;
    tm->p[tm->n].tf = tf;
    tm->n++;
    return 0;
}

static void ii_remove_slot(ii_t* x, uint32_t slot)
{
    ii_doc* d = &x->docs[slot];
    uint32_t i, j;
    for (i = 0; i < d->nt; i++) {
        ii_term* tm = &x->terms[d->t[i].term];
        for (j = 0; j < tm->n; j++)
            if (tm->p[j].doc == slot) {
                tm->p[j] = tm->p[--tm->n];
                break;
            }
    }
    x->total_len -= d->len;
    dyn_nat_free(d->t);
    d->t = NULL;
    d->nt = 0;
    d->alive = 0;
    x->live--;
    if (x->nfree == x->fcap) {
        uint32_t nc = x->fcap ? x->fcap * 2 : 16;
        uint32_t* nf = (uint32_t*)dyn_nat_realloc(x->freel, sizeof(uint32_t) * nc);
        if (!nf)
            return;
        x->freel = nf;
        x->fcap = nc;
    }
    x->freel[x->nfree++] = slot;
}

static int ii_remove(ii_t* x, uint32_t id)
{
    sdb_ient* e = sdb_imap_find(&x->didx, id);
    uint32_t slot;
    if (!e)
        return 0;
    slot = e->v;
    sdb_imap_remove(&x->didx, e);
    ii_remove_slot(x, slot);
    return 1;
}

static int ii_slot_alloc(ii_t* x, uint32_t* out)
{
    if (x->nfree) {
        *out = x->freel[--x->nfree];
        return 0;
    }
    if (x->nslots == x->dcap) {
        uint32_t nc = x->dcap ? x->dcap * 2 : 64;
        ii_doc* nd;
        if (nc > SDB_MAX_ITEMS)
            return -1;
        nd = (ii_doc*)dyn_nat_realloc(x->docs, sizeof(ii_doc) * nc);
        if (!nd)
            return -1;
        x->docs = nd;
        x->dcap = nc;
    }
    *out = x->nslots++;
    return 0;
}

static int ii_install(ii_t* x, uint32_t id, ii_dt* list, uint32_t nt, uint32_t len)
{
    uint32_t slot, i;
    ii_doc* d;
    if (ii_slot_alloc(x, &slot) < 0)
        return -1;
    if (!sdb_imap_put(&x->didx, id, slot)) {
        if (x->nfree < x->fcap)
            x->freel[x->nfree++] = slot;
        else if (slot == x->nslots - 1)
            x->nslots--;
        return -1;
    }
    d = &x->docs[slot];
    d->id = id;
    d->len = len;
    d->nt = nt;
    d->t = list;
    d->alive = 1;
    x->live++;
    x->total_len += len;
    for (i = 0; i < nt; i++)
        if (ii_post_push(&x->terms[list[i].term], slot, list[i].tf) < 0) {
            uint32_t j;
            for (j = 0; j < i; j++)
                x->terms[list[j].term].n--;
            d->nt = 0;
            d->t = NULL;
            if (sdb_imap_find(&x->didx, id))
                sdb_imap_remove(&x->didx, sdb_imap_find(&x->didx, id));
            d->alive = 0;
            x->live--;
            x->total_len -= len;
            if (x->nfree < x->fcap)
                x->freel[x->nfree++] = slot;
            return -1;
        }
    return 0;
}

static int ii_add(ii_t* x, uint32_t id, const char* text, size_t n)
{
    ii_add_ctx c;
    memset(&c, 0, sizeof(c));
    c.x = x;
    ii_remove(x, id);
    ii_bump_tepoch(x);
    ii_tokenize(text, n, ii_add_tok, &c);
    if (c.oom || ii_install(x, id, c.list, c.n, c.len) < 0) {
        dyn_nat_free(c.list);
        return -1;
    }
    return 0;
}

static void ii_free(ii_t* x)
{
    uint32_t i;
    for (i = 0; i < x->nterms; i++) {
        dyn_nat_free(x->terms[i].s);
        dyn_nat_free(x->terms[i].p);
    }
    dyn_nat_free(x->terms);
    for (i = 0; i < x->nslots; i++)
        dyn_nat_free(x->docs[i].t);
    dyn_nat_free(x->docs);
    dyn_nat_free(x->freel);
    sdb_smap_free(&x->tidx);
    sdb_imap_free(&x->didx);
    for (i = 0; i < x->nstop; i++)
        dyn_nat_free(x->stopw[i]);
    dyn_nat_free(x->stopw);
    sdb_smap_free(&x->stop);
    dyn_nat_free(x->acc);
    dyn_nat_free(x->stamp);
    dyn_nat_free(x->mask);
    dyn_nat_free(x->touched);
    dyn_nat_free(x->tstamp);
    dyn_nat_free(x->tcount);
}

static int ii_add_stop(ii_t* x, const char* t, size_t n)
{
    char** ns;
    char* s;
    if (sdb_smap_find(&x->stop, t, n, sdb_hash(t, n)))
        return 0;
    ns = (char**)dyn_nat_realloc(x->stopw, sizeof(char*) * (x->nstop + 1));
    if (!ns)
        return -1;
    x->stopw = ns;
    s = (char*)sdb_bytes_dup(t, n);
    if (!s)
        return -1;
    if (!sdb_smap_insert(&x->stop, s, n, sdb_hash(t, n), x->nstop)) {
        dyn_nat_free(s);
        return -1;
    }
    x->stopw[x->nstop++] = s;
    return 0;
}

static int ii_stop_tok(void* ud, const char* t, size_t n)
{
    return ii_add_stop((ii_t*)ud, t, n) < 0 ? -1 : 0;
}

typedef struct {
    ii_t* x;
    char toks[II_MAX_GROUPS][II_TOK_MAX];
    uint32_t lens[II_MAX_GROUPS];
    uint32_t n;
} ii_q_ctx;

static int ii_q_tok(void* ud, const char* t, size_t n)
{
    ii_q_ctx* q = (ii_q_ctx*)ud;
    uint32_t i;
    if (ii_is_stop(q->x, t, n))
        return 0;
    for (i = 0; i < q->n; i++)
        if (q->lens[i] == n && memcmp(q->toks[i], t, n) == 0)
            return 0;
    if (q->n == II_MAX_GROUPS)
        return 1;
    memcpy(q->toks[q->n], t, n);
    q->lens[q->n++] = (uint32_t)n;
    return 0;
}

static int ii_scratch(ii_t* x)
{
    if (x->scap < x->nslots) {
        uint32_t nc = x->dcap;
        double* a = (double*)dyn_nat_realloc(x->acc, sizeof(double) * nc);
        uint32_t *b, *c, *d;
        if (!a)
            return -1;
        x->acc = a;
        b = (uint32_t*)dyn_nat_realloc(x->stamp, sizeof(uint32_t) * nc);
        if (!b)
            return -1;
        x->stamp = b;
        c = (uint32_t*)dyn_nat_realloc(x->mask, sizeof(uint32_t) * nc);
        if (!c)
            return -1;
        x->mask = c;
        d = (uint32_t*)dyn_nat_realloc(x->touched, sizeof(uint32_t) * nc);
        if (!d)
            return -1;
        x->touched = d;
        memset(x->stamp + x->scap, 0, sizeof(uint32_t) * (nc - x->scap));
        x->scap = nc;
    }
    if (++x->epoch == 0) {
        memset(x->stamp, 0, sizeof(uint32_t) * x->scap);
        x->epoch = 1;
    }
    return 0;
}

typedef struct {
    double s;
    uint32_t id;
} ii_hit;

static int ii_hit_worse(const ii_hit* a, const ii_hit* b)
{
    return a->s < b->s || (a->s == b->s && a->id > b->id);
}

static int ii_hit_cmp(const void* a, const void* b)
{
    const ii_hit *x = (const ii_hit*)a, *y = (const ii_hit*)b;
    if (x->s != y->s)
        return x->s > y->s ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id);
}

static void ii_heap_push(ii_hit* h, uint32_t* n, uint32_t k, ii_hit v)
{
    uint32_t i;
    if (*n < k) {
        i = (*n)++;
        while (i) {
            uint32_t p = (i - 1) / 2;
            if (!ii_hit_worse(&v, &h[p]))
                break;
            h[i] = h[p];
            i = p;
        }
        h[i] = v;
        return;
    }
    if (!ii_hit_worse(&h[0], &v))
        return;
    i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        const ii_hit* mv = &v;
        if (l < *n && ii_hit_worse(&h[l], mv)) {
            m = l;
            mv = &h[l];
        }
        if (r < *n && ii_hit_worse(&h[r], mv))
            m = r;
        if (m == i)
            break;
        h[i] = h[m];
        i = m;
    }
    h[i] = v;
}

static void ii_score_term(ii_t* x, uint32_t ti, uint32_t g, double avg, uint32_t* ntouched)
{
    ii_term* tm = &x->terms[ti];
    double N = x->live, df = tm->n, idf, k1 = x->k1, b = x->b;
    uint32_t j;
    if (!tm->n)
        return;
    idf = log(1 + (N - df + 0.5) / (df + 0.5));
    for (j = 0; j < tm->n; j++) {
        uint32_t slot = tm->p[j].doc;
        double tf = tm->p[j].tf, dl = x->docs[slot].len;
        double s = idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * (avg > 0 ? dl / avg : 0)));
        if (x->stamp[slot] != x->epoch) {
            x->stamp[slot] = x->epoch;
            x->acc[slot] = 0;
            x->mask[slot] = 0;
            x->touched[(*ntouched)++] = slot;
        }
        x->acc[slot] += s;
        x->mask[slot] |= 1u << g;
    }
}

static int ii_search(ii_t* x, const char* q, size_t qn, int and_mode, int prefix, uint32_t limit, ii_hit** out, uint32_t* nout)
{
    ii_q_ctx* qc;
    uint32_t g, ntouched = 0, i, nh = 0, full;
    double avg;
    ii_hit* h;
    *out = NULL;
    *nout = 0;
    qc = (ii_q_ctx*)dyn_nat_malloc(sizeof(*qc));
    if (!qc)
        return -1;
    qc->x = x;
    qc->n = 0;
    ii_tokenize(q, qn, ii_q_tok, qc);
    if (!qc->n || !x->live || !limit) {
        dyn_nat_free(qc);
        return 0;
    }
    if (ii_scratch(x) < 0) {
        dyn_nat_free(qc);
        return -1;
    }
    avg = (double)x->total_len / x->live;
    for (g = 0; g < qc->n; g++) {
        if (prefix && g == qc->n - 1) {
            for (i = 0; i < x->nterms; i++)
                if (x->terms[i].len >= qc->lens[g] && memcmp(x->terms[i].s, qc->toks[g], qc->lens[g]) == 0)
                    ii_score_term(x, i, g, avg, &ntouched);
        } else {
            int64_t ti = ii_term_get(x, qc->toks[g], qc->lens[g], 0);
            if (ti >= 0)
                ii_score_term(x, (uint32_t)ti, g, avg, &ntouched);
        }
    }
    full = qc->n == 32 ? 0xFFFFFFFFu : (1u << qc->n) - 1;
    dyn_nat_free(qc);
    if (limit > ntouched)
        limit = ntouched;
    h = (ii_hit*)dyn_nat_malloc(sizeof(ii_hit) * ((size_t)limit + 1));
    if (!h)
        return -1;
    for (i = 0; i < ntouched && limit; i++) {
        uint32_t slot = x->touched[i];
        ii_hit v;
        if (and_mode && x->mask[slot] != full)
            continue;
        v.s = x->acc[slot];
        v.id = x->docs[slot].id;
        ii_heap_push(h, &nh, limit, v);
    }
    qsort(h, nh, sizeof(ii_hit), ii_hit_cmp);
    *out = h;
    *nout = nh;
    return 0;
}

typedef struct {
    ii_t x;
} sdb_ii_w;

static JSClassID sdb_ii_class_id;

static void sdb_ii_free(void* native)
{
    sdb_ii_w* w = (sdb_ii_w*)native;
    if (!w)
        return;
    ii_free(&w->x);
    dyn_nat_free(w);
}

static void sdb_ii_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_ii_free(JS_GetOpaque(val, sdb_ii_class_id));
}

static const JSClassDef sdb_ii_class = {
    "InvertedIndex",
    .finalizer = sdb_ii_finalizer,
};

static sdb_ii_w* sdb_ii_new(double k1, double b)
{
    sdb_ii_w* w = (sdb_ii_w*)dyn_nat_calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->x.k1 = k1;
    w->x.b = b;
    w->x.tepoch = 1;
    sdb_imap_init(&w->x.didx);
    return w;
}

static JSValue sdb_ii_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    double k1 = 1.2, b = 0.75;
    JSValue sw = JS_UNDEFINED;
    JSValueConst opts = argc >= 1 ? argv[0] : JS_UNDEFINED;
    if (sdb_opt_f64(ctx, opts, "k1", &k1) || sdb_opt_f64(ctx, opts, "b", &b))
        return JS_EXCEPTION;
    if (!(k1 >= 0 && k1 <= 100) || !(b >= 0 && b <= 1))
        return JS_ThrowRangeError(ctx, "InvertedIndex: k1 must be 0..100 and b 0..1");
    if (sdb_opt_get(ctx, opts, "stopWords", &sw))
        return JS_EXCEPTION;
    w = sdb_ii_new(k1, b);
    if (!w) {
        JS_FreeValue(ctx, sw);
        return ds2_oom(ctx);
    }
    if (!JS_IsUndefined(sw)) {
        JSValue lv;
        uint32_t n, i;
        if (JS_IsArray(ctx, sw) <= 0) {
            JS_FreeValue(ctx, sw);
            sdb_ii_free(w);
            return JS_ThrowTypeError(ctx, "stopWords must be an array of strings");
        }
        lv = JS_GetPropertyStr(ctx, sw, "length");
        if (JS_ToUint32(ctx, &n, lv)) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, sw);
            sdb_ii_free(w);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (n > 100000) {
            JS_FreeValue(ctx, sw);
            sdb_ii_free(w);
            return JS_ThrowRangeError(ctx, "stopWords: at most 100000 entries");
        }
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, sw, i);
            ds2_str s;
            int rc;
            if (JS_IsException(e) || !JS_IsString(e)) {
                if (!JS_IsException(e))
                    JS_ThrowTypeError(ctx, "stopWords[%u] is not a string", (unsigned)i);
                JS_FreeValue(ctx, e);
                JS_FreeValue(ctx, sw);
                sdb_ii_free(w);
                return JS_EXCEPTION;
            }
            rc = ds2_str_get(ctx, &s, e);
            JS_FreeValue(ctx, e);
            if (rc) {
                JS_FreeValue(ctx, sw);
                sdb_ii_free(w);
                return JS_EXCEPTION;
            }
            rc = ii_tokenize(s.p, s.n, ii_stop_tok, &w->x);
            ds2_str_release(ctx, &s);
            if (rc) {
                JS_FreeValue(ctx, sw);
                sdb_ii_free(w);
                return ds2_oom(ctx);
            }
        }
    }
    JS_FreeValue(ctx, sw);
    return dyn_plain_wrap(ctx, new_target, sdb_ii_class_id, w, sdb_ii_free);
}

static JSValue sdb_ii_add(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    uint32_t id;
    ds2_str t;
    int rc;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &t, argv[1]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &t);
        return JS_EXCEPTION;
    }
    rc = ii_add(&w->x, id, t.p, t.n);
    ds2_str_release(ctx, &t);
    if (rc < 0)
        return ds2_oom(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue sdb_ii_id_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_ii_w* w;
    uint32_t id;
    sdb_ient* e;
    (void)argc;
    if (sdb_u32_arg(ctx, argv[0], "id", &id))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewBool(ctx, ii_remove(&w->x, id));
    e = sdb_imap_find(&w->x.didx, id);
    if (magic == 1)
        return JS_NewBool(ctx, e != NULL);
    return e ? JS_NewInt64(ctx, w->x.docs[e->v].len) : JS_UNDEFINED;
}

static JSValue sdb_ii_search(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    ds2_str q;
    uint32_t limit = 10, n, i, out = 0;
    int prefix = 0, and_mode = 0;
    ii_hit* h;
    JSValue arr, mode = JS_UNDEFINED;
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    if (sdb_opt_u32(ctx, opts, "limit", &limit) || sdb_opt_bool(ctx, opts, "prefix", &prefix) || sdb_opt_get(ctx, opts, "mode", &mode))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(mode)) {
        const char* m = JS_ToCString(ctx, mode);
        JS_FreeValue(ctx, mode);
        if (!m)
            return JS_EXCEPTION;
        if (!strcmp(m, "and"))
            and_mode = 1;
        else if (strcmp(m, "or")) {
            JS_FreeCString(ctx, m);
            return JS_ThrowRangeError(ctx, "mode must be \"and\" or \"or\"");
        }
        JS_FreeCString(ctx, m);
    }
    if (limit > SDB_MAX_ITEMS)
        limit = SDB_MAX_ITEMS;
    if (ds2_str_get(ctx, &q, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &q);
        return JS_EXCEPTION;
    }
    if (ii_search(&w->x, q.p, q.n, and_mode, prefix, limit, &h, &n) < 0) {
        ds2_str_release(ctx, &q);
        return ds2_oom(ctx);
    }
    ds2_str_release(ctx, &q);
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++)
        if (sdb_push(ctx, arr, &out, sdb_pair(ctx, JS_NewInt64(ctx, h[i].id), JS_NewFloat64(ctx, h[i].s))) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    dyn_nat_free(h);
    return arr;
}

typedef struct {
    char buf[II_TOK_MAX];
    size_t n;
} ii_first_ctx;

static int ii_first_tok(void* ud, const char* t, size_t n)
{
    ii_first_ctx* c = (ii_first_ctx*)ud;
    memcpy(c->buf, t, n);
    c->n = n;
    return 1;
}

static JSValue sdb_ii_doc_freq(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    ds2_str t;
    ii_first_ctx c;
    int64_t ti;
    (void)argc;
    if (ds2_str_get(ctx, &t, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &t);
        return JS_EXCEPTION;
    }
    c.n = 0;
    ii_tokenize(t.p, t.n, ii_first_tok, &c);
    ds2_str_release(ctx, &t);
    if (!c.n)
        return JS_NewInt32(ctx, 0);
    ti = ii_term_get(&w->x, c.buf, c.n, 0);
    return JS_NewInt64(ctx, ti >= 0 ? w->x.terms[ti].n : 0);
}

typedef struct {
    uint32_t df;
    uint32_t ti;
} ii_sugg;

static _Thread_local const ii_t* ii_sort_ctx;

static int ii_sugg_cmp(const void* a, const void* b)
{
    const ii_sugg *x = (const ii_sugg*)a, *y = (const ii_sugg*)b;
    const ii_term *tx, *ty;
    uint32_t m;
    int c;
    if (x->df != y->df)
        return x->df > y->df ? -1 : 1;
    tx = &ii_sort_ctx->terms[x->ti];
    ty = &ii_sort_ctx->terms[y->ti];
    m = tx->len < ty->len ? tx->len : ty->len;
    c = m ? memcmp(tx->s, ty->s, m) : 0;
    return c ? c : (tx->len > ty->len) - (tx->len < ty->len);
}

static JSValue sdb_ii_suggest(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_ii_w* w;
    ds2_str t;
    ii_first_ctx c;
    uint32_t limit = 10, i, n = 0, out = 0;
    ii_sugg* s;
    JSValue arr;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && dyn_size_arg(ctx, argv[1], SDB_MAX_ITEMS, &limit))
        return JS_EXCEPTION;
    if (ds2_str_get(ctx, &t, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    if (!w) {
        ds2_str_release(ctx, &t);
        return JS_EXCEPTION;
    }
    c.n = 0;
    ii_tokenize(t.p, t.n, ii_first_tok, &c);
    ds2_str_release(ctx, &t);
    s = (ii_sugg*)dyn_nat_malloc(sizeof(ii_sugg) * ((size_t)w->x.nterms + 1));
    if (!s)
        return ds2_oom(ctx);
    for (i = 0; i < w->x.nterms; i++) {
        const ii_term* tm = &w->x.terms[i];
        if (tm->n && tm->len >= c.n && (c.n == 0 || memcmp(tm->s, c.buf, c.n) == 0)) {
            s[n].df = tm->n;
            s[n].ti = i;
            n++;
        }
    }
    ii_sort_ctx = &w->x;
    qsort(s, n, sizeof(ii_sugg), ii_sugg_cmp);
    ii_sort_ctx = NULL;
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && i < limit && !JS_IsException(arr); i++) {
        const ii_term* tm = &w->x.terms[s[i].ti];
        if (sdb_push(ctx, arr, &out, sdb_pair(ctx, JS_NewStringLen(ctx, tm->s, tm->len), JS_NewInt64(ctx, tm->n))) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_nat_free(s);
    return arr;
}

static int ii_collect_tok(void* ud, const char* t, size_t n)
{
    JSContext* ctx = ((JSContext**)ud)[0];
    JSValue* arr = (JSValue*)((void**)ud)[1];
    uint32_t* i = (uint32_t*)((void**)ud)[2];
    return sdb_push(ctx, *arr, i, JS_NewStringLen(ctx, t, n)) < 0 ? -1 : 0;
}

static JSValue sdb_ii_tokenize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ds2_str t;
    JSValue arr;
    uint32_t i = 0;
    void* ud[3];
    (void)this_val;
    (void)argc;
    if (ds2_str_get(ctx, &t, argv[0]))
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (!JS_IsException(arr)) {
        ud[0] = ctx;
        ud[1] = &arr;
        ud[2] = &i;
        if (ii_tokenize(t.p, t.n, ii_collect_tok, ud)) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    ds2_str_release(ctx, &t);
    return arr;
}

static JSValue sdb_ii_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_ii_w* w = (sdb_ii_w*)dyn_plain_get(ctx, this_val, sdb_ii_class_id);
    uint32_t i, n = 0;
    if (!w)
        return JS_EXCEPTION;
    if (magic == 0)
        return JS_NewInt64(ctx, w->x.live);
    if (magic == 1) {
        for (i = 0; i < w->x.nterms; i++)
            n += w->x.terms[i].n != 0;
        return JS_NewInt64(ctx, n);
    }
    return JS_NewFloat64(ctx, w->x.live ? (double)w->x.total_len / w->x.live : 0);
}

static const JSCFunctionListEntry sdb_ii_proto[] = {
    JS_CFUNC_DEF("add", 2, sdb_ii_add),
    JS_CFUNC_MAGIC_DEF("remove", 1, sdb_ii_id_op, 0),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_ii_id_op, 1),
    JS_CFUNC_MAGIC_DEF("docLength", 1, sdb_ii_id_op, 2),
    JS_CFUNC_DEF("search", 1, sdb_ii_search),
    JS_CFUNC_DEF("docFreq", 1, sdb_ii_doc_freq),
    JS_CFUNC_DEF("suggest", 1, sdb_ii_suggest),
    JS_CGETSET_MAGIC_DEF("size", sdb_ii_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("termCount", sdb_ii_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("avgLength", sdb_ii_getter, NULL, 2),
};

#define IN_CHUNK (64u * 1024u)

typedef struct in_chunk {
    struct in_chunk* next;
    size_t used, cap;
    char data[];
} in_chunk;

typedef struct {
    const char* p;
    uint32_t len;
} in_str;

typedef struct {
    in_chunk* chunks;
    in_str* s;
    uint32_t n, cap;
    sdb_smap idx;
    uint64_t bytes;
} in_t;

static const char* in_store(in_t* t, const char* p, size_t n)
{
    in_chunk* c = t->chunks;
    char* d;
    if (!c || c->cap - c->used < n + 1) {
        size_t cap = n + 1 > IN_CHUNK ? n + 1 : IN_CHUNK;
        in_chunk* nc = (in_chunk*)dyn_nat_malloc(sizeof(in_chunk) + cap);
        if (!nc)
            return NULL;
        nc->used = 0;
        nc->cap = cap;
        if (c && n + 1 > IN_CHUNK) {
            nc->next = c->next;
            c->next = nc;
        } else {
            nc->next = c;
            t->chunks = nc;
        }
        c = nc;
    }
    d = c->data + c->used;
    if (n)
        memcpy(d, p, n);
    d[n] = 0;
    c->used += n + 1;
    t->bytes += n + 1;
    return d;
}

static int64_t in_intern(in_t* t, const char* p, size_t n, int create)
{
    uint64_t h = sdb_hash(p, n);
    sdb_sent* e = sdb_smap_find(&t->idx, p, n, h);
    const char* d;
    if (e)
        return (int64_t)e->v;
    if (!create)
        return -1;
    if (n > UINT32_MAX - 1 || t->n >= 0x7FFFFFFFu)
        return -2;
    if (t->n == t->cap) {
        uint32_t nc = t->cap ? t->cap * 2 : 64;
        in_str* ns = (in_str*)dyn_nat_realloc(t->s, sizeof(in_str) * nc);
        if (!ns)
            return -2;
        t->s = ns;
        t->cap = nc;
    }
    d = in_store(t, p, n);
    if (!d)
        return -2;
    if (!sdb_smap_insert(&t->idx, d, n, h, t->n))
        return -2;
    t->s[t->n].p = d;
    t->s[t->n].len = (uint32_t)n;
    return (int64_t)t->n++;
}

static void in_free(in_t* t)
{
    in_chunk* c = t->chunks;
    while (c) {
        in_chunk* nx = c->next;
        dyn_nat_free(c);
        c = nx;
    }
    dyn_nat_free(t->s);
    sdb_smap_free(&t->idx);
    memset(t, 0, sizeof(*t));
}

typedef struct {
    in_t t;
} sdb_in_w;

static JSClassID sdb_in_class_id;

static void sdb_in_free(void* native)
{
    sdb_in_w* w = (sdb_in_w*)native;
    if (!w)
        return;
    in_free(&w->t);
    dyn_nat_free(w);
}

static void sdb_in_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    sdb_in_free(JS_GetOpaque(val, sdb_in_class_id));
}

static const JSClassDef sdb_in_class = {
    "Interner",
    .finalizer = sdb_in_finalizer,
};

static JSValue sdb_in_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    (void)argc;
    (void)argv;
    w = (sdb_in_w*)dyn_nat_calloc(1, sizeof(*w));
    if (!w)
        return ds2_oom(ctx);
    return dyn_plain_wrap(ctx, new_target, sdb_in_class_id, w, sdb_in_free);
}

static JSValue sdb_in_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    sdb_in_w* w;
    ds2_str k;
    int64_t id;
    (void)argc;
    if (ds2_str_get(ctx, &k, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w) {
        ds2_str_release(ctx, &k);
        return JS_EXCEPTION;
    }
    id = in_intern(&w->t, k.p, k.n, magic == 0);
    ds2_str_release(ctx, &k);
    if (id == -2)
        return ds2_oom(ctx);
    if (magic == 2)
        return JS_NewBool(ctx, id >= 0);
    return id >= 0 ? JS_NewInt64(ctx, id) : JS_UNDEFINED;
}

static JSValue sdb_in_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    double d;
    (void)argc;
    if (JS_ToFloat64(ctx, &d, argv[0]))
        return JS_EXCEPTION;
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w)
        return JS_EXCEPTION;
    if (!(d >= 0) || d != trunc(d) || d >= (double)w->t.n)
        return JS_UNDEFINED;
    return JS_NewStringLen(ctx, w->t.s[(uint32_t)d].p, w->t.s[(uint32_t)d].len);
}

static JSValue sdb_in_intern_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    JSValue lv;
    uint32_t n, i;
    uint32_t* ids;
    JSValue out;
    (void)argc;
    if (JS_IsArray(ctx, argv[0]) <= 0)
        return JS_ThrowTypeError(ctx, "internMany: expected an array of strings");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n > SDB_MAX_ITEMS)
        return JS_ThrowRangeError(ctx, "internMany: more than %u strings", SDB_MAX_ITEMS);
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w)
        return JS_EXCEPTION;
    ids = (uint32_t*)dyn_nat_malloc(sizeof(uint32_t) * ((size_t)n + 1));
    if (!ids)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        JSValue* fast;
        uint32_t cnt;
        JSValue e;
        ds2_str k;
        int64_t id;
        if (JS_GetFastArray(ctx, argv[0], &fast, &cnt) && i < cnt)
            e = JS_DupValue(ctx, fast[i]);
        else
            e = JS_GetPropertyUint32(ctx, argv[0], i);
        if (JS_IsException(e)) {
            dyn_nat_free(ids);
            return JS_EXCEPTION;
        }
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            dyn_nat_free(ids);
            return JS_ThrowTypeError(ctx, "internMany: element %u is not a string", (unsigned)i);
        }
        if (ds2_str_get(ctx, &k, e)) {
            JS_FreeValue(ctx, e);
            dyn_nat_free(ids);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
        id = w ? in_intern(&w->t, k.p, k.n, 1) : -3;
        ds2_str_release(ctx, &k);
        if (id < 0) {
            dyn_nat_free(ids);
            return id == -3 ? JS_EXCEPTION : ds2_oom(ctx);
        }
        ids[i] = (uint32_t)id;
    }
    out = sdb_typed_out(ctx, ids, n, JS_TYPED_ARRAY_UINT32);
    dyn_nat_free(ids);
    return out;
}

static JSValue sdb_in_get_many(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    sdb_in_w* w;
    uint32_t *ids, n, i, out = 0;
    JSValue arr;
    (void)argc;
    if (sdb_u32_list(ctx, argv[0], "getMany", &ids, &n))
        return JS_EXCEPTION;
    w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w) {
        dyn_nat_free(ids);
        return JS_EXCEPTION;
    }
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++) {
        JSValue v = ids[i] < w->t.n ? JS_NewStringLen(ctx, w->t.s[ids[i]].p, w->t.s[ids[i]].len) : JS_UNDEFINED;
        if (sdb_push(ctx, arr, &out, v) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    dyn_nat_free(ids);
    return arr;
}

static JSValue sdb_in_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdb_in_w* w = (sdb_in_w*)dyn_plain_get(ctx, this_val, sdb_in_class_id);
    if (!w)
        return JS_EXCEPTION;
    return magic ? JS_NewInt64(ctx, (int64_t)w->t.bytes) : JS_NewInt64(ctx, w->t.n);
}

static const JSCFunctionListEntry sdb_in_proto[] = {
    JS_CFUNC_MAGIC_DEF("intern", 1, sdb_in_op, 0),
    JS_CFUNC_MAGIC_DEF("lookup", 1, sdb_in_op, 1),
    JS_CFUNC_MAGIC_DEF("has", 1, sdb_in_op, 2),
    JS_CFUNC_DEF("get", 1, sdb_in_get),
    JS_CFUNC_DEF("internMany", 1, sdb_in_intern_many),
    JS_CFUNC_DEF("getMany", 1, sdb_in_get_many),
    JS_CGETSET_MAGIC_DEF("size", sdb_in_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("bytes", sdb_in_getter, NULL, 1),
};

#define DYN_TID_ZSET 31
#define DYN_TID_ORDEREDMAP 32
#define DYN_TID_ROARING 33
#define DYN_TID_TDIGEST 34
#define DYN_TID_TOPK 35
#define DYN_TID_CUCKOO 36
#define DYN_TID_INDEXEDHEAP 37
#define DYN_TID_KDTREE 38
#define DYN_TID_CONSISTENTHASH 39
#define DYN_TID_INVERTEDINDEX 40
#define DYN_TID_INTERNER 41

static JSValue sdb_bad(JSContext* ctx)
{
    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
}

static int sdb_zset_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_zset_w* z = JS_GetOpaque(obj, sdb_zset_class_id);
    sdb_sln* x;
    (void)ctx;
    if (dyn_ser_u32(w, z->sl.len) < 0)
        return -1;
    for (x = z->sl.head->lv[0].fwd; x; x = x->lv[0].fwd)
        if (dyn_ser_blob(w, x->key, x->klen) < 0 || dyn_ser_f64(w, x->score) < 0)
            return -1;
    return 0;
}

static JSValue sdb_zset_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_zset_w* z;
    (void)opts;
    if (dyn_de_count(r, &n, 12) < 0)
        return sdb_bad(ctx);
    if (dyn_de_charge(r, (uint64_t)n * 96) < 0)
        return sdb_bad(ctx);
    z = sdb_zset_new(ctx);
    if (!z)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* k = dyn_de_blob(r, &kn);
        double sc = dyn_de_f64(r);
        if (!dyn_de_ok(r) || sc != sc || sdb_sl_find(&z->sl, k, kn)) {
            sdb_zset_free(z);
            return sdb_bad(ctx);
        }
        if (sdb_sl_insert(&z->sl, sc, k, kn, JS_UNDEFINED, NULL) < 0) {
            sdb_zset_free(z);
            return ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_zset_class_id, z, sdb_zset_free);
}

static int sdb_omap_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_omap_w* o = JS_GetOpaque(obj, sdb_omap_class_id);
    sdb_sln* x;
    JSValue vals;
    uint32_t i = 0;
    int rc;
    if (dyn_ser_u32(w, o->sl.len) < 0)
        return -1;
    vals = JS_NewArray(ctx);
    if (JS_IsException(vals))
        return -1;
    for (x = o->sl.head->lv[0].fwd; x; x = x->lv[0].fwd) {
        if (dyn_ser_blob(w, x->key, x->klen) < 0 || sdb_push(ctx, vals, &i, JS_DupValue(ctx, x->val)) < 0) {
            JS_FreeValue(ctx, vals);
            return -1;
        }
    }
    rc = dyn_codec_write_values(ctx, w, vals);
    JS_FreeValue(ctx, vals);
    return rc;
}

static JSValue sdb_omap_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_omap_w* o;
    JSValue vals, obj;
    const char** ks;
    size_t* kl;
    (void)opts;
    if (dyn_de_count(r, &n, 4) < 0)
        return sdb_bad(ctx);
    if (dyn_de_charge(r, (uint64_t)n * 112) < 0)
        return sdb_bad(ctx);
    ks = (const char**)dyn_nat_malloc(sizeof(char*) * ((size_t)n + 1));
    kl = (size_t*)dyn_nat_malloc(sizeof(size_t) * ((size_t)n + 1));
    if (!ks || !kl) {
        dyn_nat_free(ks);
        dyn_nat_free(kl);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        ks[i] = dyn_de_blob(r, &kl[i]);
        if (!dyn_de_ok(r)) {
            dyn_nat_free(ks);
            dyn_nat_free(kl);
            return sdb_bad(ctx);
        }
    }
    vals = ds2_read_cells(ctx, r, n);
    if (JS_IsException(vals)) {
        dyn_nat_free(ks);
        dyn_nat_free(kl);
        return vals;
    }
    o = (sdb_omap_w*)dyn_nat_malloc(sizeof(*o));
    if (!o || sdb_sl_init(&o->sl, JS_GetRuntime(ctx)) < 0) {
        dyn_nat_free(o);
        JS_FreeValue(ctx, vals);
        dyn_nat_free(ks);
        dyn_nat_free(kl);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v;
        if (sdb_sl_find(&o->sl, ks[i], kl[i])) {
            sdb_omap_free(o);
            JS_FreeValue(ctx, vals);
            dyn_nat_free(ks);
            dyn_nat_free(kl);
            return sdb_bad(ctx);
        }
        v = JS_GetPropertyUint32(ctx, vals, i);
        if (JS_IsException(v) || sdb_sl_insert(&o->sl, 0, ks[i], kl[i], v, NULL) < 0) {
            JS_FreeValue(ctx, v);
            sdb_omap_free(o);
            JS_FreeValue(ctx, vals);
            dyn_nat_free(ks);
            dyn_nat_free(kl);
            return ds2_oom(ctx);
        }
    }
    JS_FreeValue(ctx, vals);
    dyn_nat_free(ks);
    dyn_nat_free(kl);
    obj = dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_omap_class_id, o, sdb_omap_free);
    return obj;
}

static int sdb_rb_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_rb_w* b = JS_GetOpaque(obj, sdb_rb_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, b->r.n) < 0)
        return -1;
    for (i = 0; i < b->r.n; i++) {
        const rb_ct* c = &b->r.c[i];
        if (dyn_ser_u16(w, c->key) < 0 || dyn_ser_u8(w, c->type) < 0 || dyn_ser_u32(w, c->card) < 0)
            return -1;
        if (c->type == RB_ARR) {
            const uint16_t* a = (const uint16_t*)c->data;
            uint32_t j;
            for (j = 0; j < c->card; j++)
                if (dyn_ser_u16(w, a[j]) < 0)
                    return -1;
        } else if (c->type == RB_BMP) {
            const uint64_t* x = (const uint64_t*)c->data;
            uint32_t j;
            for (j = 0; j < RB_WORDS; j++)
                if (dyn_ser_u64(w, x[j]) < 0)
                    return -1;
        }
    }
    return 0;
}

static JSValue sdb_rb_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_rb_w* b;
    (void)opts;
    if (dyn_de_count(r, &n, 7) < 0 || n > 65536)
        return sdb_bad(ctx);
    b = (sdb_rb_w*)dyn_nat_calloc(1, sizeof(*b));
    if (!b || rb_reserve(&b->r, n) < 0) {
        dyn_nat_free(b);
        return ds2_oom(ctx);
    }
    for (i = 0; i < n; i++) {
        rb_ct* c = &b->r.c[i];
        uint16_t key = dyn_de_u16(r);
        uint8_t type = dyn_de_u8(r);
        uint32_t card = dyn_de_u32(r), j;
        memset(c, 0, sizeof(*c));
        if (!dyn_de_ok(r) || (i && key <= b->r.c[i - 1].key))
            goto bad;
        c->key = key;
        if (type == RB_ARR) {
            uint16_t* a;
            if (card == 0 || card > RB_ARR_MAX || dyn_de_left(r) < (size_t)card * 2 || dyn_de_charge(r, (uint64_t)card * 2) < 0)
                goto bad;
            a = (uint16_t*)dyn_nat_malloc(sizeof(uint16_t) * card);
            if (!a)
                goto oom;
            for (j = 0; j < card; j++) {
                a[j] = dyn_de_u16(r);
                if (j && a[j] <= a[j - 1]) {
                    dyn_nat_free(a);
                    goto bad;
                }
            }
            c->type = RB_ARR;
            c->data = a;
            c->cap = card;
            c->card = card;
        } else if (type == RB_BMP) {
            uint64_t* x;
            if (card <= RB_ARR_MAX || card >= 65536 || dyn_de_left(r) < 8u * RB_WORDS || dyn_de_charge(r, 8u * RB_WORDS) < 0)
                goto bad;
            x = (uint64_t*)dyn_nat_malloc(sizeof(uint64_t) * RB_WORDS);
            if (!x)
                goto oom;
            for (j = 0; j < RB_WORDS; j++)
                x[j] = dyn_de_u64(r);
            if (rb_popcnt(x) != card) {
                dyn_nat_free(x);
                goto bad;
            }
            c->type = RB_BMP;
            c->data = x;
            c->card = card;
        } else if (type == RB_FULL) {
            if (card != 65536)
                goto bad;
            c->type = RB_FULL;
            c->card = 65536;
        } else
            goto bad;
        if (!dyn_de_ok(r))
            goto bad_inc;
        b->r.n = i + 1;
        continue;
    bad_inc:
        b->r.n = i + 1;
        goto bad;
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_rb_class_id, b, sdb_rb_free);
bad:
    sdb_rb_free(b);
    return sdb_bad(ctx);
oom:
    sdb_rb_free(b);
    return ds2_oom(ctx);
}

static int sdb_td_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_td_w* t = JS_GetOpaque(obj, sdb_td_class_id);
    uint32_t i;
    (void)ctx;
    td_compress(&t->t);
    if (dyn_ser_f64(w, t->t.delta) < 0 || dyn_ser_f64(w, t->t.min) < 0 || dyn_ser_f64(w, t->t.max) < 0 || dyn_ser_f64(w, t->t.sum) < 0 || dyn_ser_u32(w, t->t.n) < 0)
        return -1;
    for (i = 0; i < t->t.n; i++)
        if (dyn_ser_f64(w, t->t.c[i].m) < 0 || dyn_ser_f64(w, t->t.c[i].w) < 0)
            return -1;
    return 0;
}

static JSValue sdb_td_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    double delta = dyn_de_f64(r), mn = dyn_de_f64(r), mx = dyn_de_f64(r), sum = dyn_de_f64(r);
    uint32_t n, i;
    sdb_td_w* t;
    double total = 0;
    (void)opts;
    if (!dyn_de_ok(r) || !(delta >= TD_MIN_DELTA && delta <= TD_MAX_DELTA) || dyn_de_count(r, &n, 16) < 0)
        return sdb_bad(ctx);
    t = (sdb_td_w*)dyn_nat_calloc(1, sizeof(*t));
    if (!t || td_init(&t->t, delta) < 0) {
        dyn_nat_free(t);
        return ds2_oom(ctx);
    }
    if (n > t->t.cap || (n && !(mn <= mx && isfinite(mn) && isfinite(mx))) || !isfinite(sum)) {
        sdb_td_free(t);
        return sdb_bad(ctx);
    }
    for (i = 0; i < n; i++) {
        double m = dyn_de_f64(r), wt = dyn_de_f64(r);
        if (!dyn_de_ok(r) || !isfinite(m) || !(wt > 0) || !isfinite(wt) || m < mn || m > mx || (i && m < t->t.c[i - 1].m)) {
            sdb_td_free(t);
            return sdb_bad(ctx);
        }
        t->t.c[i].m = m;
        t->t.c[i].w = wt;
        total += wt;
    }
    t->t.n = n;
    t->t.total = total;
    t->t.sum = sum;
    t->t.min = n ? mn : HUGE_VAL;
    t->t.max = n ? mx : -HUGE_VAL;
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_td_class_id, t, sdb_td_free);
}

static int sdb_tk_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_tk_w* t = JS_GetOpaque(obj, sdb_tk_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, t->t.k) < 0 || dyn_ser_u32(w, t->t.n) < 0 || dyn_ser_f64(w, t->t.total) < 0)
        return -1;
    for (i = 0; i < t->t.n; i++) {
        const tk_slot* s = &t->t.s[i];
        if (dyn_ser_blob(w, s->key, s->klen) < 0 || dyn_ser_f64(w, s->count) < 0 || dyn_ser_f64(w, s->err) < 0)
            return -1;
    }
    return 0;
}

static JSValue sdb_tk_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t k = dyn_de_u32(r), n, i;
    double total;
    sdb_tk_w* t;
    (void)opts;
    if (!dyn_de_ok(r) || k == 0 || k > TK_MAX_K || dyn_de_count(r, &n, 20) < 0 || n > k)
        return sdb_bad(ctx);
    total = dyn_de_f64(r);
    if (!dyn_de_ok(r) || !(total >= 0) || !isfinite(total) || dyn_de_charge(r, (uint64_t)k * 48) < 0)
        return sdb_bad(ctx);
    t = sdb_tk_new(k);
    if (!t)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* key = dyn_de_blob(r, &kn);
        double c = dyn_de_f64(r), e = dyn_de_f64(r);
        tk_slot* s = &t->t.s[i];
        uint64_t h;
        if (!dyn_de_ok(r) || !(c > 0) || !isfinite(c) || !(e >= 0) || e > c || kn > UINT32_MAX - 1) {
            sdb_tk_free(t);
            return sdb_bad(ctx);
        }
        h = sdb_hash(key, kn);
        if (sdb_smap_find(&t->t.idx, key, kn, h)) {
            sdb_tk_free(t);
            return sdb_bad(ctx);
        }
        s->key = (char*)sdb_bytes_dup(key, kn);
        if (!s->key || !sdb_smap_insert(&t->t.idx, s->key, kn, h, i)) {
            dyn_nat_free(s->key);
            s->key = NULL;
            sdb_tk_free(t);
            return ds2_oom(ctx);
        }
        s->klen = (uint32_t)kn;
        s->count = c;
        s->err = e;
        t->t.heap[i] = i;
        s->hpos = i;
        t->t.n = i + 1;
        tk_up(&t->t, i);
    }
    t->t.total = total;
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_tk_class_id, t, sdb_tk_free);
}

static int sdb_cf_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_cf_w* f = JS_GetOpaque(obj, sdb_cf_class_id);
    uint64_t i, total = (uint64_t)f->c.nb * CF_SLOTS;
    (void)ctx;
    if (dyn_ser_u32(w, f->c.nb) < 0 || dyn_ser_u32(w, f->c.count) < 0 || dyn_ser_u8(w, (uint8_t)f->c.has_victim) < 0 || dyn_ser_u16(w, f->c.victim_fp) < 0 || dyn_ser_u32(w, f->c.victim_i) < 0)
        return -1;
    for (i = 0; i < total; i++)
        if (dyn_ser_u16(w, f->c.t[i]) < 0)
            return -1;
    return 0;
}

static JSValue sdb_cf_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t nb = dyn_de_u32(r), count = dyn_de_u32(r), vi, used = 0;
    uint8_t hv = dyn_de_u8(r);
    uint16_t vf = dyn_de_u16(r);
    uint64_t i, total;
    sdb_cf_w* f;
    (void)opts;
    vi = dyn_de_u32(r);
    if (!dyn_de_ok(r) || nb == 0 || (nb & (nb - 1)) || (uint64_t)nb * CF_SLOTS > 2ull * CF_MAX_CAPACITY || hv > 1 || (hv && (!vf || vi >= nb)))
        return sdb_bad(ctx);
    total = (uint64_t)nb * CF_SLOTS;
    if (dyn_de_left(r) < total * 2 || dyn_de_charge(r, total * 2) < 0)
        return sdb_bad(ctx);
    f = sdb_cf_new(nb);
    if (!f)
        return ds2_oom(ctx);
    for (i = 0; i < total; i++) {
        f->c.t[i] = dyn_de_u16(r);
        used += f->c.t[i] != 0;
    }
    if (!dyn_de_ok(r) || used + hv != count) {
        sdb_cf_free(f);
        return sdb_bad(ctx);
    }
    f->c.count = count;
    f->c.has_victim = hv;
    f->c.victim_fp = vf;
    f->c.victim_i = vi;
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_cf_class_id, f, sdb_cf_free);
}

static int sdb_ih_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_ih_w* h = JS_GetOpaque(obj, sdb_ih_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u8(w, (uint8_t)h->q.max) < 0 || dyn_ser_u32(w, h->q.n) < 0)
        return -1;
    for (i = 0; i < h->q.n; i++)
        if (dyn_ser_u32(w, h->q.h[i].id) < 0 || dyn_ser_f64(w, h->q.h[i].p) < 0)
            return -1;
    return 0;
}

static JSValue sdb_ih_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint8_t mx = dyn_de_u8(r);
    uint32_t n, i;
    sdb_ih_w* h;
    (void)opts;
    if (!dyn_de_ok(r) || mx > 1 || dyn_de_count(r, &n, 12) < 0 || dyn_de_charge(r, (uint64_t)n * 40) < 0)
        return sdb_bad(ctx);
    h = sdb_ih_new(mx);
    if (!h)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        uint32_t id = dyn_de_u32(r);
        double p = dyn_de_f64(r);
        if (!dyn_de_ok(r) || p != p || sdb_imap_find(&h->q.pos, id)) {
            sdb_ih_free(h);
            return sdb_bad(ctx);
        }
        if (ih_push(&h->q, id, p) < 0) {
            sdb_ih_free(h);
            return ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_ih_class_id, h, sdb_ih_free);
}

static int sdb_kd_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_kd_w* k = JS_GetOpaque(obj, sdb_kd_class_id);
    size_t i, total = (size_t)k->t.n * k->t.d;
    (void)ctx;
    if (dyn_ser_u32(w, k->t.d) < 0 || dyn_ser_u32(w, k->t.n) < 0)
        return -1;
    for (i = 0; i < total; i++)
        if (dyn_ser_f64(w, k->t.pts[i]) < 0)
            return -1;
    return 0;
}

static JSValue sdb_kd_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t d = dyn_de_u32(r), n = dyn_de_u32(r);
    uint64_t total, i;
    double* pts;
    (void)opts;
    if (!dyn_de_ok(r) || d == 0 || d > KD_MAX_DIMS)
        return sdb_bad(ctx);
    total = (uint64_t)n * d;
    if (total > SDB_MAX_ITEMS || dyn_de_left(r) < total * 8 || dyn_de_charge(r, total * 24) < 0)
        return sdb_bad(ctx);
    pts = (double*)dyn_nat_malloc(sizeof(double) * (size_t)(total + 1));
    if (!pts)
        return ds2_oom(ctx);
    for (i = 0; i < total; i++)
        pts[i] = dyn_de_f64(r);
    if (!dyn_de_ok(r)) {
        dyn_nat_free(pts);
        return sdb_bad(ctx);
    }
    return sdb_kd_wrap(ctx, JS_UNDEFINED, pts, n, d);
}

static int sdb_ch_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_ch_w* c = JS_GetOpaque(obj, sdb_ch_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, c->c.replicas) < 0 || dyn_ser_u32(w, c->c.nn) < 0)
        return -1;
    for (i = 0; i < c->c.nn; i++)
        if (dyn_ser_blob(w, c->c.nodes[i].name, c->c.nodes[i].len) < 0 || dyn_ser_u32(w, c->c.nodes[i].weight) < 0)
            return -1;
    return 0;
}

static JSValue sdb_ch_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t rep = dyn_de_u32(r), n, i;
    sdb_ch_w* c;
    (void)opts;
    if (!dyn_de_ok(r) || rep < 1 || rep > CH_MAX_REPLICAS || dyn_de_count(r, &n, 8) < 0)
        return sdb_bad(ctx);
    c = (sdb_ch_w*)dyn_nat_calloc(1, sizeof(*c));
    if (!c)
        return ds2_oom(ctx);
    c->c.replicas = rep;
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* k = dyn_de_blob(r, &kn);
        uint32_t wt = dyn_de_u32(r);
        int rc;
        if (!dyn_de_ok(r) || wt < 1 || wt > CH_MAX_WEIGHT || sdb_smap_find(&c->c.idx, k, kn, sdb_hash(k, kn))) {
            sdb_ch_free(c);
            return sdb_bad(ctx);
        }
        rc = ch_set_node(&c->c, k, kn, wt);
        if (rc < 0) {
            sdb_ch_free(c);
            return rc == -2 ? sdb_bad(ctx) : ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_ch_class_id, c, sdb_ch_free);
}

static int sdb_ii_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_ii_w* ii = JS_GetOpaque(obj, sdb_ii_class_id);
    ii_t* x = &ii->x;
    uint32_t i, j;
    (void)ctx;
    if (dyn_ser_f64(w, x->k1) < 0 || dyn_ser_f64(w, x->b) < 0 || dyn_ser_u32(w, x->nstop) < 0)
        return -1;
    for (i = 0; i < x->nstop; i++)
        if (dyn_ser_blob(w, x->stopw[i], strlen(x->stopw[i])) < 0)
            return -1;
    if (dyn_ser_u32(w, x->nterms) < 0)
        return -1;
    for (i = 0; i < x->nterms; i++)
        if (dyn_ser_blob(w, x->terms[i].s, x->terms[i].len) < 0)
            return -1;
    if (dyn_ser_u32(w, x->live) < 0)
        return -1;
    for (i = 0; i < x->nslots; i++) {
        const ii_doc* d = &x->docs[i];
        if (!d->alive)
            continue;
        if (dyn_ser_u32(w, d->id) < 0 || dyn_ser_u32(w, d->len) < 0 || dyn_ser_u32(w, d->nt) < 0)
            return -1;
        for (j = 0; j < d->nt; j++)
            if (dyn_ser_u32(w, d->t[j].term) < 0 || dyn_ser_u32(w, d->t[j].tf) < 0)
                return -1;
    }
    return 0;
}

static JSValue sdb_ii_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    double k1 = dyn_de_f64(r), b = dyn_de_f64(r);
    uint32_t ns, nt, nd, i, j;
    sdb_ii_w* ii;
    ii_t* x;
    (void)opts;
    if (!dyn_de_ok(r) || !(k1 >= 0 && k1 <= 100) || !(b >= 0 && b <= 1) || dyn_de_count(r, &ns, 4) < 0)
        return sdb_bad(ctx);
    ii = sdb_ii_new(k1, b);
    if (!ii)
        return ds2_oom(ctx);
    x = &ii->x;
    for (i = 0; i < ns; i++) {
        size_t n;
        const char* s = dyn_de_blob(r, &n);
        if (!dyn_de_ok(r) || n == 0 || n > II_TOK_MAX || memchr(s, 0, n))
            goto bad;
        if (ii_add_stop(x, s, n) < 0)
            goto oom;
    }
    if (dyn_de_count(r, &nt, 4) < 0 || dyn_de_charge(r, (uint64_t)nt * 96) < 0)
        goto bad;
    for (i = 0; i < nt; i++) {
        size_t n;
        const char* s = dyn_de_blob(r, &n);
        int64_t ti;
        if (!dyn_de_ok(r) || n == 0 || n > II_TOK_MAX || ii_term_get(x, s, n, 0) >= 0)
            goto bad;
        ti = ii_term_get(x, s, n, 1);
        if (ti < 0)
            goto oom;
    }
    if (dyn_de_count(r, &nd, 12) < 0 || dyn_de_charge(r, (uint64_t)nd * 64) < 0)
        goto bad;
    for (i = 0; i < nd; i++) {
        uint32_t id = dyn_de_u32(r), len = dyn_de_u32(r), cnt;
        uint64_t sumtf = 0;
        ii_dt* list;
        if (!dyn_de_ok(r) || sdb_imap_find(&x->didx, id) || dyn_de_count(r, &cnt, 8) < 0 || dyn_de_charge(r, (uint64_t)cnt * 24) < 0)
            goto bad;
        list = (ii_dt*)dyn_nat_malloc(sizeof(ii_dt) * ((size_t)cnt + 1));
        if (!list)
            goto oom;
        if (ii_tscratch(x) < 0) {
            dyn_nat_free(list);
            goto oom;
        }
        ii_bump_tepoch(x);
        for (j = 0; j < cnt; j++) {
            list[j].term = dyn_de_u32(r);
            list[j].tf = dyn_de_u32(r);
            if (!dyn_de_ok(r) || list[j].term >= x->nterms || list[j].tf == 0 || x->tstamp[list[j].term] == x->tepoch) {
                dyn_nat_free(list);
                goto bad;
            }
            x->tstamp[list[j].term] = x->tepoch;
            sumtf += list[j].tf;
        }
        if (sumtf != len) {
            dyn_nat_free(list);
            goto bad;
        }
        if (ii_install(x, id, list, cnt, len) < 0) {
            dyn_nat_free(list);
            goto oom;
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_ii_class_id, ii, sdb_ii_free);
bad:
    sdb_ii_free(ii);
    return sdb_bad(ctx);
oom:
    sdb_ii_free(ii);
    return ds2_oom(ctx);
}

static int sdb_in_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    sdb_in_w* t = JS_GetOpaque(obj, sdb_in_class_id);
    uint32_t i;
    (void)ctx;
    if (dyn_ser_u32(w, t->t.n) < 0)
        return -1;
    for (i = 0; i < t->t.n; i++)
        if (dyn_ser_blob(w, t->t.s[i].p, t->t.s[i].len) < 0)
            return -1;
    return 0;
}

static JSValue sdb_in_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i;
    sdb_in_w* t;
    (void)opts;
    if (dyn_de_count(r, &n, 4) < 0 || dyn_de_charge(r, (uint64_t)n * 48) < 0)
        return sdb_bad(ctx);
    t = (sdb_in_w*)dyn_nat_calloc(1, sizeof(*t));
    if (!t)
        return ds2_oom(ctx);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char* k = dyn_de_blob(r, &kn);
        int64_t id;
        if (!dyn_de_ok(r) || in_intern(&t->t, k, kn, 0) >= 0) {
            sdb_in_free(t);
            return sdb_bad(ctx);
        }
        id = in_intern(&t->t, k, kn, 1);
        if (id < 0) {
            sdb_in_free(t);
            return ds2_oom(ctx);
        }
    }
    return dyn_plain_wrap(ctx, JS_UNDEFINED, sdb_in_class_id, t, sdb_in_free);
}

static const dyn_codec_t sdb_codecs[] = {
    { 0, DYN_TID_ZSET, "ZSet", sdb_zset_write, sdb_zset_read },
    { 0, DYN_TID_ORDEREDMAP, "OrderedMap", sdb_omap_write, sdb_omap_read },
    { 0, DYN_TID_ROARING, "RoaringBitmap", sdb_rb_write, sdb_rb_read },
    { 0, DYN_TID_TDIGEST, "TDigest", sdb_td_write, sdb_td_read },
    { 0, DYN_TID_TOPK, "TopK", sdb_tk_write, sdb_tk_read },
    { 0, DYN_TID_CUCKOO, "CuckooFilter", sdb_cf_write, sdb_cf_read },
    { 0, DYN_TID_INDEXEDHEAP, "IndexedHeap", sdb_ih_write, sdb_ih_read },
    { 0, DYN_TID_KDTREE, "KDTree", sdb_kd_write, sdb_kd_read },
    { 0, DYN_TID_CONSISTENTHASH, "ConsistentHash", sdb_ch_write, sdb_ch_read },
    { 0, DYN_TID_INVERTEDINDEX, "InvertedIndex", sdb_ii_write, sdb_ii_read },
    { 0, DYN_TID_INTERNER, "Interner", sdb_in_write, sdb_in_read },
};

static JSClassID* const sdb_class_ids[] = {
    &sdb_zset_class_id,
    &sdb_omap_class_id,
    &sdb_rb_class_id,
    &sdb_td_class_id,
    &sdb_tk_class_id,
    &sdb_cf_class_id,
    &sdb_ih_class_id,
    &sdb_kd_class_id,
    &sdb_ch_class_id,
    &sdb_ii_class_id,
    &sdb_in_class_id,
};

static int sdb_register_codecs(void)
{
    size_t i;
    for (i = 0; i < countof(sdb_codecs); i++) {
        dyn_codec_t c = sdb_codecs[i];
        c.class_id = *sdb_class_ids[i];
        if (dyn_codec_register(&c) < 0)
            return -1;
    }
    return 0;
}

static int sdb_static(JSContext* ctx, JSClassID id, const char* name, JSCFunction* fn, int len)
{
    JSValue proto = JS_GetClassProto(ctx, id), ctor;
    int rc = -1;
    if (!JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    if (JS_IsFunction(ctx, ctor))
        rc = JS_DefinePropertyValueStr(ctx, ctor, name, JS_NewCFunction(ctx, fn, name, len), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0 ? -1 : 0;
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, proto);
    return rc;
}

#define SDB_REGISTER(pfx, name)                                         \
    if (dyn_register_plain_class(ctx, m, &pfx##_class_id, &pfx##_class, \
            pfx##_proto, countof(pfx##_proto),                          \
            pfx##_ctor, name)                                           \
        < 0)                                                            \
        return -1;

static int sdb_register(JSContext* ctx, JSModuleDef* m)
{
    sdb_capture_typed_cids(ctx);
    SDB_REGISTER(sdb_zset, "ZSet")
    SDB_REGISTER(sdb_omap, "OrderedMap")
    SDB_REGISTER(sdb_rb, "RoaringBitmap")
    SDB_REGISTER(sdb_td, "TDigest")
    SDB_REGISTER(sdb_tk, "TopK")
    SDB_REGISTER(sdb_cf, "CuckooFilter")
    SDB_REGISTER(sdb_ih, "IndexedHeap")
    SDB_REGISTER(sdb_kd, "KDTree")
    SDB_REGISTER(sdb_ch, "ConsistentHash")
    SDB_REGISTER(sdb_ii, "InvertedIndex")
    SDB_REGISTER(sdb_in, "Interner")
    if (sdb_static(ctx, sdb_ch_class_id, "jump", sdb_ch_jump, 2) < 0 || sdb_static(ctx, sdb_ii_class_id, "tokenize", sdb_ii_tokenize, 1) < 0)
        return -1;
    return 0;
}

static void sdb_add_exports(JSContext* ctx, JSModuleDef* m)
{
    static const char* const names[] = {
        "ZSet",
        "OrderedMap",
        "RoaringBitmap",
        "TDigest",
        "TopK",
        "CuckooFilter",
        "IndexedHeap",
        "KDTree",
        "ConsistentHash",
        "InvertedIndex",
        "Interner",
    };
    size_t i;
    for (i = 0; i < countof(names); i++)
        JS_AddModuleExport(ctx, m, names[i]);
}
