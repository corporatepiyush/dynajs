#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#include "dyna-serialize.h"

#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_CODEC_MAX 64

static dyn_codec_t dyn_codecs[DYN_CODEC_MAX];
static int dyn_codec_count;

int dyn_codec_register(const dyn_codec_t* c)
{
    if (dyn_codec_count >= DYN_CODEC_MAX)
        return -1;
    dyn_codecs[dyn_codec_count++] = *c;
    return 0;
}

const dyn_codec_t* dyn_codec_by_class(JSClassID id)
{
    int i;
    for (i = 0; i < dyn_codec_count; i++)
        if (dyn_codecs[i].class_id == id)
            return &dyn_codecs[i];
    return NULL;
}

const dyn_codec_t* dyn_codec_by_type(uint16_t type_id)
{
    int i;
    for (i = 0; i < dyn_codec_count; i++)
        if (dyn_codecs[i].type_id == type_id)
            return &dyn_codecs[i];
    return NULL;
}

JSValue dyn_codec_throw(JSContext* ctx, int code)
{
    return JS_ThrowTypeError(ctx, "%s", dyn_de_strerror(code));
}

static JSValue dyn_serialize_u8array(JSContext* ctx, const uint8_t* data,
    size_t len)
{
    static const uint8_t zero_stub = 0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    if (len == 0)
        data = &zero_stub;
    ab = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

int dyn_codec_write_values(JSContext* ctx, dyn_ser_t* w, JSValueConst arr)
{
    size_t n = 0;
    uint8_t* p = JS_WriteObject(ctx, &n, arr, 0);
    int r;
    if (!p)
        return -1;
    r = dyn_ser_blob(w, p, n);
    js_free(ctx, p);
    if (r < 0)
        JS_ThrowOutOfMemory(ctx);
    return r;
}

JSValue dyn_codec_read_values(JSContext* ctx, dyn_de_t* r)
{
    size_t n = 0;
    const char* p = dyn_de_blob(r, &n);
    JSValue v;

    if (!dyn_de_ok(r) || !p)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    v = JS_ReadObject(ctx, (const uint8_t*)p, n, 0);
    if (JS_IsException(v))
        return v;
    if (!JS_IsArray(ctx, v)) {
        JS_FreeValue(ctx, v);
        return JS_ThrowTypeError(ctx, "malformed DYNS element payload");
    }
    return v;
}

static const dyn_codec_t* codec_for(JSContext* ctx, JSValueConst v)
{
    JSClassID id = JS_GetClassID(v);
    (void)ctx;
    return id ? dyn_codec_by_class(id) : NULL;
}

static const uint8_t* bytes_of(JSContext* ctx, JSValueConst v, size_t* len,
    JSValue* keep)
{
    size_t off = 0, blen = 0, esz = 0;
    JSValue ab;
    uint8_t* p;

    *keep = JS_UNDEFINED;
    ab = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &esz);
    if (!JS_IsException(ab)) {
        size_t total = 0;
        p = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!p)
            return NULL;
        *len = blen;
        return p + off;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    p = JS_GetArrayBuffer(ctx, len, v);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "expected bytes (Uint8Array or ArrayBuffer)");
        return NULL;
    }
    return p;
}

static JSValue dyn_ds_serialize_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const dyn_codec_t* c = codec_for(ctx, this_val);
    dyn_ser_t w;
    uint8_t* buf;
    size_t len;
    JSValue out;
    (void)argc;
    (void)argv;

    if (!c)
        return JS_ThrowTypeError(ctx,
            "serialize: not a serializable dyna:structures container");
    dyn_ser_init(&w);
    if (dyn_ser_begin(&w, c->type_id, 0) < 0 || c->write(ctx, &w, this_val) < 0 || dyn_ser_finish(&w) < 0) {
        dyn_ser_free(&w);
        if (!JS_HasException(ctx))
            JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    buf = dyn_ser_take(&w, &len);
    out = dyn_serialize_u8array(ctx, buf, len);
    free(buf);
    return out;
}

static JSValue dyn_ds_deserialize_static(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic)
{
    const uint8_t* p;
    size_t len = 0;
    JSValue keep;
    dyn_de_t r;
    uint16_t tid = 0;
    uint32_t flags = 0;
    const dyn_codec_t *self, *c;
    int rc;
    (void)this_val;

    if (magic < 0 || magic >= dyn_codec_count)
        return JS_ThrowInternalError(ctx, "deserialize: bad codec index");
    self = &dyn_codecs[magic];
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s.deserialize(bytes) requires bytes",
            self->name);
    p = bytes_of(ctx, argv[0], &len, &keep);
    if (!p)
        return JS_EXCEPTION;
    rc = dyn_de_open(&r, p, len, &tid, &flags, 0);
    if (rc != DYN_DE_OK) {
        JS_FreeValue(ctx, keep);
        return dyn_codec_throw(ctx, rc);
    }
    if (tid != self->type_id) {
        c = dyn_codec_by_type(tid);
        JS_FreeValue(ctx, keep);
        return JS_ThrowTypeError(ctx,
            "%s.deserialize: these bytes are a %s record, not a %s",
            self->name, c ? c->name : "unknown", self->name);
    }
    {
        JSValue out = self->read(ctx, &r, argc >= 2 ? argv[1] : JS_UNDEFINED);
        JS_FreeValue(ctx, keep);
        if (!JS_IsException(out) && !dyn_de_ok(&r)) {
            JS_FreeValue(ctx, out);
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        }
        return out;
    }
}

int dyn_codec_install_methods(JSContext* ctx)
{
    int i;
    for (i = 0; i < dyn_codec_count; i++) {
        JSValue proto = JS_GetClassProto(ctx, dyn_codecs[i].class_id);
        JSValue ctor;
        if (!JS_IsObject(proto)) {
            JS_FreeValue(ctx, proto);
            continue;
        }
        JS_SetPropertyStr(ctx, proto, "serialize",
            JS_NewCFunction(ctx, dyn_ds_serialize_method, "serialize", 0));
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        if (JS_IsFunction(ctx, ctor))
            JS_SetPropertyStr(ctx, ctor, "deserialize",
                JS_NewCFunctionMagic(ctx, dyn_ds_deserialize_static,
                    "deserialize", 1,
                    JS_CFUNC_generic_magic, i));
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, proto);
    }
    return 0;
}

int dyn_serializer_register(JSContext* ctx, JSModuleDef* m)
{
    (void)ctx;
    (void)m;
    return 0;
}

void dyn_serializer_add_exports(JSContext* ctx, JSModuleDef* m)
{
    (void)ctx;
    (void)m;
}

#endif
