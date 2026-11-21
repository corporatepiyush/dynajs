#include "structures-internal.h"
#include "core/ds/ds-suffix-array.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

typedef struct {
    dyn_sa_t* sa;
    int is_string;
    uint32_t* u16_at;
    uint8_t* starts;
    size_t text_units;
} dsx_sa_t;

static void dsx_sa_free(void* native)
{
    dsx_sa_t* s = (dsx_sa_t*)native;
    if (!s)
        return;
    dyn_sa_free(s->sa);
    dyn_ds_free(s->u16_at);
    dyn_ds_free(s->starts);
    dyn_ds_free(s);
}

JSClassID dsx_sa_class_id;

DYN_FINALIZER(dsx_sa)

const JSClassDef dsx_sa_class = { "SuffixArray", .finalizer = dsx_sa_finalizer };

JSValue dsx_sa_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    dsx_sa_t* s;
    dsx_bytes b;
    int is_string, cache = 0;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "SuffixArray: the options must be an object");
        if (!dsx_opt_keys_ok(ctx, argv[1], "cache", "SuffixArray", "constructor") || dsx_cache_opt(ctx, argv[1], "SuffixArray", "bucket", &cache) < 0)
            return JS_EXCEPTION;
    }
    is_string = JS_IsString(argv[0]);
    if (!is_string) {
        size_t off, len, bpe;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
        if (JS_IsException(ab))
            return JS_ThrowTypeError(ctx, "SuffixArray: expected a string or a Uint8Array");
        JS_FreeValue(ctx, ab);
        if (bpe != 1)
            return JS_ThrowTypeError(ctx, "SuffixArray: expected a string or a Uint8Array");
    }
    if (dsx_key_bytes(ctx, argv[0], !is_string, &b))
        return JS_EXCEPTION;
    if (b.n >= (size_t)UINT32_MAX - 1) {
        dsx_key_bytes_free(ctx, &b);
        return JS_ThrowRangeError(ctx, "SuffixArray: text too long");
    }
    s = (dsx_sa_t*)dyn_ds_malloc(sizeof(*s));
    if (!s) {
        dsx_key_bytes_free(ctx, &b);
        return JS_ThrowOutOfMemory(ctx);
    }
    memset(s, 0, sizeof(*s));
    s->is_string = is_string;
    s->sa = dyn_sa_build(b.p, b.n);
    if (s->sa && is_string) {
        size_t i, u = 0;
        s->u16_at = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (b.n + 1));
        s->starts = (uint8_t*)dyn_ds_malloc(b.n + 1);
        if (s->u16_at && s->starts) {
            for (i = 0; i < b.n;) {
                uint8_t c = b.p[i];
                size_t l = c < 0x80 ? 1 : c < 0xE0 ? 2
                    : c < 0xF0                     ? 3
                                                   : 4,
                       j;
                for (j = 0; j < l && i + j < b.n; j++) {
                    s->u16_at[i + j] = (uint32_t)u;
                    s->starts[i + j] = j == 0;
                }
                i += l;
                u += l == 4 ? 2 : 1;
            }
            s->u16_at[b.n] = (uint32_t)u;
            s->starts[b.n] = 1;
            s->text_units = u;
        }
    } else if (s->sa) {
        s->text_units = b.n;
    }
    dsx_key_bytes_free(ctx, &b);
    if (!s->sa || (is_string && (!s->u16_at || !s->starts)) || (cache && dyn_sa_bucket(s->sa) < 0)) {
        dsx_sa_free(s);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsx_sa_class_id, s, dsx_sa_free);
}

static uint32_t dsx_sa_pos(const dsx_sa_t* s, uint32_t bytepos)
{
    return s->is_string ? s->u16_at[bytepos] : bytepos;
}

static JSValue dsx_sa_array(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    dsx_sa_t* s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    const uint32_t *sa, *lcp = NULL;
    uint32_t *out, n, i, k = 0, run_min = UINT32_MAX;
    JSValue r;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    n = (uint32_t)dyn_sa_len(s->sa);
    sa = dyn_sa_array(s->sa);
    if (magic) {
        lcp = dyn_sa_lcp(s->sa);
        if (!lcp)
            return JS_ThrowOutOfMemory(ctx);
    }
    out = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (n ? n : 1));
    if (!out)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < n; i++) {
        if (magic && lcp[i] < run_min)
            run_min = lcp[i];
        if (s->is_string && !s->starts[sa[i]])
            continue;
        if (magic) {
            uint32_t l = run_min;
            if (s->is_string)
                l = s->u16_at[sa[i] + l] - s->u16_at[sa[i]];
            out[k++] = l;
            run_min = UINT32_MAX;
        } else {
            out[k++] = dsx_sa_pos(s, sa[i]);
        }
    }
    r = dsx_typed_copy(ctx, out, k, sizeof(uint32_t), JS_TYPED_ARRAY_UINT32);
    dyn_ds_free(out);
    return r;
}

static int dsx_u32_cmp(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static JSValue dsx_sa_search(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    dsx_sa_t* s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    dsx_bytes b;
    size_t first = 0, cnt, i;
    (void)argc;
    if (!s)
        return JS_EXCEPTION;
    if (dsx_key_bytes(ctx, argv[0], !s->is_string && !JS_IsString(argv[0]), &b))
        return JS_EXCEPTION;
    s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    if (!s) {
        dsx_key_bytes_free(ctx, &b);
        return JS_EXCEPTION;
    }
    if (b.n == 0) {
        dsx_key_bytes_free(ctx, &b);
        return magic ? JS_NewArray(ctx) : JS_NewInt64(ctx, 0);
    }
    cnt = dyn_sa_find(s->sa, b.p, b.n, &first);
    dsx_key_bytes_free(ctx, &b);
    if (!magic)
        return JS_NewInt64(ctx, (int64_t)cnt);
    {
        uint32_t* pos = (uint32_t*)dyn_ds_malloc(sizeof(uint32_t) * (cnt ? cnt : 1));
        const uint32_t* sa = dyn_sa_array(s->sa);
        JSValue arr;
        if (!pos)
            return JS_ThrowOutOfMemory(ctx);
        for (i = 0; i < cnt; i++)
            pos[i] = dsx_sa_pos(s, sa[first + i]);
        qsort(pos, cnt, sizeof(uint32_t), dsx_u32_cmp);
        arr = JS_NewArray(ctx);
        for (i = 0; !JS_IsException(arr) && i < cnt; i++)
            if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, JS_NewInt64(ctx, pos[i]), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                arr = JS_EXCEPTION;
            }
        dyn_ds_free(pos);
        return arr;
    }
}

static JSValue dsx_sa_longest_repeat(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dsx_sa_t* s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    size_t pos = 0, len = 0;
    uint32_t start, units = 0;
    JSValue o;
    (void)argc;
    (void)argv;
    if (!s)
        return JS_EXCEPTION;
    if (!dyn_sa_lcp(s->sa))
        return JS_ThrowOutOfMemory(ctx);
    if (!s->is_string) {
        len = dyn_sa_longest_repeat(s->sa, &pos);
        units = (uint32_t)len;
    } else {
        const uint32_t* sa = dyn_sa_array(s->sa);
        const uint32_t* lcp = dyn_sa_lcp(s->sa);
        uint32_t n = (uint32_t)dyn_sa_len(s->sa), i, run = UINT32_MAX;
        for (i = 0; i < n; i++) {
            if (lcp[i] < run)
                run = lcp[i];
            if (!s->starts[sa[i]])
                continue;
            if (run) {
                uint32_t u = s->u16_at[sa[i] + run] - s->u16_at[sa[i]];
                if (u > units) {
                    units = u;
                    pos = sa[i];
                }
            }
            run = UINT32_MAX;
        }
    }
    if (!units)
        return JS_UNDEFINED;
    start = dsx_sa_pos(s, (uint32_t)pos);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, "start", JS_NewInt64(ctx, start), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "length", JS_NewInt64(ctx, units), JS_PROP_C_W_E);
    return o;
}

static JSValue dsx_sa_length(JSContext* ctx, JSValueConst this_val)
{
    dsx_sa_t* s = dyn_plain_get(ctx, this_val, dsx_sa_class_id);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)s->text_units);
}

const JSCFunctionListEntry dsx_sa_proto[] = {
    JS_CFUNC_MAGIC_DEF("array", 0, dsx_sa_array, 0),
    JS_CFUNC_MAGIC_DEF("lcp", 0, dsx_sa_array, 1),
    JS_CFUNC_MAGIC_DEF("count", 1, dsx_sa_search, 0),
    JS_CFUNC_MAGIC_DEF("find", 1, dsx_sa_search, 1),
    JS_CFUNC_DEF("longestRepeat", 0, dsx_sa_longest_repeat),
    JS_CGETSET_DEF("length", dsx_sa_length, NULL),
};

int dsx_sa_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dsx_sa_t* s = JS_GetOpaque(obj, dsx_sa_class_id);
    if (!s)
        return -1;
    if (dyn_ser_u8(w, (uint8_t)s->is_string) < 0 || dyn_ser_blob(w, dyn_sa_text(s->sa), dyn_sa_len(s->sa)) < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

JSValue dsx_sa_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint8_t is_string = dyn_de_u8(r);
    size_t n = 0;
    const char* p;
    JSValue text, obj;
    (void)opts;
    p = dyn_de_blob(r, &n);
    if (!dyn_de_ok(r) || !p || is_string > 1)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    text = is_string ? JS_NewStringLen(ctx, p, n) : dyn_nat_u8_copy(ctx, (const uint8_t*)p, n);
    if (JS_IsException(text))
        return text;
    obj = dsx_sa_ctor(ctx, JS_UNDEFINED, 1, (JSValueConst*)&text);
    JS_FreeValue(ctx, text);
    return obj;
}

#endif
