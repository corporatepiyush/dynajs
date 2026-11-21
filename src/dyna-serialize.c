#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && (defined(CONFIG_NATIVE_MODULE_STRUCTURES) || defined(CONFIG_NATIVE_MODULE_GRAPH))

#include "dyna-serialize.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_CODEC_MAX 64

static dyn_codec_t dyn_codecs[DYN_CODEC_MAX];
static _Atomic int dyn_codec_count;
static pthread_mutex_t dyn_codec_lock = PTHREAD_MUTEX_INITIALIZER;

int dyn_codec_register(const dyn_codec_t* c)
{
    int i, n, rc = 0;
    pthread_mutex_lock(&dyn_codec_lock);
    n = atomic_load_explicit(&dyn_codec_count, memory_order_relaxed);
    for (i = 0; i < n; i++)
        if (dyn_codecs[i].type_id == c->type_id)
            break;
    if (i < n) {
        if (dyn_codecs[i].class_id != c->class_id)
            rc = -1;
    } else if (n >= DYN_CODEC_MAX) {
        rc = -1;
    } else {
        dyn_codecs[n] = *c;
        atomic_store_explicit(&dyn_codec_count, n + 1, memory_order_release);
    }
    pthread_mutex_unlock(&dyn_codec_lock);
    return rc;
}

static int dyn_codec_n(void)
{
    return atomic_load_explicit(&dyn_codec_count, memory_order_acquire);
}

const dyn_codec_t* dyn_codec_by_class(JSClassID id)
{
    int i, n = dyn_codec_n();
    for (i = 0; i < n; i++)
        if (dyn_codecs[i].class_id == id)
            return &dyn_codecs[i];
    return NULL;
}

const dyn_codec_t* dyn_codec_by_type(uint16_t type_id)
{
    int i, n = dyn_codec_n();
    for (i = 0; i < n; i++)
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
    ab = JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW ? JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &esz) : JS_EXCEPTION;
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

    if (magic < 0 || magic >= dyn_codec_n())
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

static struct {
    uint16_t type_id;
    const dyn_codec_persist_t* p;
} dyn_phooks[DYN_CODEC_MAX];
static _Atomic int dyn_phook_count;

int dyn_codec_persist_register(uint16_t type_id, const dyn_codec_persist_t* p)
{
    int i, n, rc = 0;
    pthread_mutex_lock(&dyn_codec_lock);
    n = atomic_load_explicit(&dyn_phook_count, memory_order_relaxed);
    for (i = 0; i < n; i++)
        if (dyn_phooks[i].type_id == type_id)
            break;
    if (i < n) {
        if (dyn_phooks[i].p != p)
            rc = -1;
    } else if (n >= DYN_CODEC_MAX) {
        rc = -1;
    } else {
        dyn_phooks[n].type_id = type_id;
        dyn_phooks[n].p = p;
        atomic_store_explicit(&dyn_phook_count, n + 1, memory_order_release);
    }
    pthread_mutex_unlock(&dyn_codec_lock);
    return rc;
}

static const dyn_codec_persist_t* dyn_phook(uint16_t type_id)
{
    int i, n = atomic_load_explicit(&dyn_phook_count, memory_order_acquire);
    for (i = 0; i < n; i++)
        if (dyn_phooks[i].type_id == type_id)
            return dyn_phooks[i].p;
    return NULL;
}

int dyn_ds_pstate_track(dyn_ds_pstate* ps, const void* p, uint64_t len)
{
    uint64_t n = dyn_persist_chunks(len, 14);
    if (n != ps->nchunks || !ps->hashes) {
        uint64_t* h = (uint64_t*)realloc(ps->hashes, (size_t)(n ? n : 1) * sizeof(uint64_t));
        uint64_t* d;
        if (!h)
            return -1;
        ps->hashes = h;
        d = (uint64_t*)realloc(ps->dirty, (size_t)((n + 63) / 64 + 1) * sizeof(uint64_t));
        if (!d)
            return -1;
        ps->dirty = d;
        ps->nchunks = n;
        ps->hash_ok = 0;
    }
    memset(ps->dirty, 0, (size_t)((n + 63) / 64 + 1) * sizeof(uint64_t));
    dyn_persist_chunk_diff(p, len, 14, ps->hashes, ps->hash_ok, ps->dirty);
    return 0;
}

void dyn_ds_pstate_committed(dyn_ds_pstate* ps, int ok)
{
    if (ps)
        ps->hash_ok = ok;
}

void dyn_ds_pstate_free(dyn_ds_pstate* ps)
{
    free(ps->hashes);
    free(ps->dirty);
    if (ps->map)
        dyn_persist_release(ps->map);
    memset(ps, 0, sizeof *ps);
}

JSValue dyn_ds_persist_bad(JSContext* ctx, const char* what)
{
    JSValue err = JS_NewError(ctx);
    char msg[200];
    if (JS_IsException(err))
        return err;
    snprintf(msg, sizeof msg, "load: persist file is malformed: %s", what);
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, err, "code", JS_NewString(ctx, "EPERSIST_CORRUPT"), JS_PROP_C_W_E);
    return JS_Throw(ctx, err);
}

static JSValue dyn_persist_throw(JSContext* ctx, const char* cls, const char* op,
    const char* path, const dyn_persist_err* e)
{
    JSValue err = JS_NewError(ctx);
    char msg[512];
    if (JS_IsException(err))
        return err;
    snprintf(msg, sizeof msg, "%s.%s(\"%.200s\"): %s: %s", cls, op, path,
        dyn_persist_strerror(e->code), e->msg);
    JS_DefinePropertyValueStr(ctx, err, "message", JS_NewString(ctx, msg), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, err, "code", JS_NewString(ctx, dyn_persist_code_name(e->code)), JS_PROP_C_W_E);
    if (e->sys_errno)
        JS_DefinePropertyValueStr(ctx, err, "errno", JS_NewInt32(ctx, e->sys_errno), JS_PROP_C_W_E);
    return JS_Throw(ctx, err);
}

static void dyn_persist_tag_corrupt(JSContext* ctx)
{
    JSValue ex = JS_GetException(ctx);
    if (JS_IsError(ctx, ex)) {
        JSValue code = JS_GetPropertyStr(ctx, ex, "code");
        if (JS_IsUndefined(code))
            JS_DefinePropertyValueStr(ctx, ex, "code", JS_NewString(ctx, "EPERSIST_CORRUPT"), JS_PROP_C_W_E);
        JS_FreeValue(ctx, code);
    }
    JS_Throw(ctx, ex);
}

static int dyn_persist_bool_opt(JSContext* ctx, JSValueConst opts, const char* name,
    int def, int* out)
{
    JSValue v;
    *out = def;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
    v = JS_GetPropertyStr(ctx, opts, name);
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (!JS_IsBool(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "option '%s' must be a boolean", name);
            return -1;
        }
        *out = JS_ToBool(ctx, v);
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static _Thread_local int dyn_persist_busy;
_Thread_local int dyn_ds_rebuild_threw;

static int dyn_persist_record(JSContext* ctx, const dyn_codec_t* c, JSValueConst obj,
    dyn_persist_w* w)
{
    dyn_ser_t sr;
    int rc;
    sr.buf = w->xbuf;
    sr.cap = w->xcap;
    sr.len = 0;
    sr.err = 0;
    rc = (dyn_ser_begin(&sr, c->type_id, 0) < 0 || c->write(ctx, &sr, obj) < 0 || dyn_ser_finish(&sr) < 0) ? -1 : 0;
    w->xbuf = sr.buf;
    w->xcap = sr.cap;
    if (rc < 0) {
        if (!JS_HasException(ctx))
            JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return dyn_persist_w_ref(w, DYN_PKIND_RECORD, 0, 1, 0, sr.buf, sr.len);
}

static JSValue dyn_ds_save_method(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const dyn_codec_t* c = codec_for(ctx, this_val);
    const dyn_codec_persist_t* hook;
    dyn_ds_pstate* ps = NULL;
    dyn_persist_w local, *w;
    dyn_persist_save_opt o;
    dyn_persist_err e;
    const char* path;
    char what[96];
    int do_fsync, incremental, rc, nested;
    JSValue out;

    if (!c)
        return JS_ThrowTypeError(ctx, "save: not a dyna:structures container");
    snprintf(what, sizeof what, "%s.save(path, opts?): path", c->name);
    path = dyn_path_borrow(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, what, NULL);
    if (!path)
        return JS_EXCEPTION;
    if (dyn_persist_bool_opt(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "fsync", 1, &do_fsync) < 0
        || dyn_persist_bool_opt(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "incremental", 0, &incremental) < 0)
        return JS_EXCEPTION;
    nested = dyn_persist_busy;
    if (nested) {
        dyn_persist_w_init(&local);
        w = &local;
    } else {
        w = dyn_persist_w_thread();
        if (!w)
            return JS_ThrowOutOfMemory(ctx);
    }
    dyn_persist_busy++;
    hook = dyn_phook(c->type_id);
    dyn_persist_w_begin(w, c->type_id, 0);
    if (hook) {
        ps = hook->state(this_val);
        rc = hook->sections(ctx, this_val, w);
    } else {
        rc = dyn_persist_record(ctx, c, this_val, w);
    }
    if (rc < 0) {
        if (!JS_HasException(ctx))
            JS_ThrowOutOfMemory(ctx);
        out = JS_EXCEPTION;
        goto done;
    }
    memset(&o, 0, sizeof o);
    o.fsync = do_fsync;
    o.incremental = incremental && ps && ps->lineage && ps->hash_ok;
    if (ps) {
        w->lineage = ps->lineage;
        w->gen = ps->gen;
    }
    rc = dyn_persist_save(w, path, &o, &e);
    if (hook && hook->committed)
        hook->committed(this_val, rc == 0);
    if (rc != 0) {
        out = dyn_persist_throw(ctx, c->name, "save", path, &e);
        goto done;
    }
    if (ps) {
        ps->lineage = w->lineage;
        ps->gen = w->gen;
    }
    out = JS_NewObject(ctx);
    if (!JS_IsException(out)) {
        JS_DefinePropertyValueStr(ctx, out, "path", JS_DupValue(ctx, argv[0]), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, out, "bytes", JS_NewInt64(ctx, (int64_t)o.file_size), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, out, "written", JS_NewInt64(ctx, (int64_t)o.bytes_written), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, out, "incremental", JS_NewBool(ctx, o.wrote_incremental), JS_PROP_C_W_E);
    }
done:
    dyn_persist_busy--;
    if (nested)
        dyn_persist_w_free(&local);
    else
        dyn_persist_w_reset(w);
    return out;
}

static JSValue dyn_ds_load_static(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const dyn_codec_t* self;
    const dyn_codec_persist_t* hook;
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue cmp = JS_UNDEFINED, out;
    dyn_persist_r* r = NULL;
    dyn_persist_err e;
    const char* path;
    char what[96];
    int use_mmap, verify, rc;
    (void)this_val;

    if (magic < 0 || magic >= dyn_codec_n())
        return JS_ThrowInternalError(ctx, "load: bad codec index");
    self = &dyn_codecs[magic];
    snprintf(what, sizeof what, "%s.load(path, opts?): path", self->name);
    path = dyn_path_borrow(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, what, NULL);
    if (!path)
        return JS_EXCEPTION;
    if (dyn_persist_bool_opt(ctx, opts, "mmap", 0, &use_mmap) < 0
        || dyn_persist_bool_opt(ctx, opts, "verify", 1, &verify) < 0)
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        cmp = JS_GetPropertyStr(ctx, opts, "compare");
        if (JS_IsException(cmp))
            return cmp;
        if (!JS_IsUndefined(cmp) && !JS_IsFunction(ctx, cmp)) {
            JS_FreeValue(ctx, cmp);
            return JS_ThrowTypeError(ctx, "%s.load: option 'compare' must be a function", self->name);
        }
    }
    rc = dyn_persist_open(path, use_mmap ? DYN_PERSIST_MODE_MMAP : DYN_PERSIST_MODE_COPY, verify,
        self->type_id, &r, &e);
    if (rc != 0) {
        if (rc == DYN_PERSIST_E_TYPE) {
            dyn_persist_r* any = NULL;
            dyn_persist_err e2;
            if (dyn_persist_open(path, DYN_PERSIST_MODE_COPY, 0, 0, &any, &e2) == 0) {
                const dyn_codec_t* other = dyn_codec_by_type((uint16_t)any->type_id);
                snprintf(e.msg, sizeof e.msg, "the file holds a %s, not a %s",
                    other ? other->name : "structure of unknown type", self->name);
                dyn_persist_release(any);
            }
        }
        out = dyn_persist_throw(ctx, self->name, "load", path, &e);
        goto done;
    }
    hook = dyn_phook(self->type_id);
    if (hook) {
        out = hook->load(ctx, r, use_mmap);
        if (!JS_IsException(out)) {
            dyn_ds_pstate* ps = hook->state(out);
            if (ps) {
                ps->lineage = r->lineage;
                ps->gen = r->gen;
                ps->hash_ok = 0;
            }
        }
    } else {
        const dyn_persist_rsect* s = dyn_persist_find(r, DYN_PKIND_RECORD, 0);
        dyn_de_t d;
        uint16_t tid = 0;
        uint32_t flags = 0;
        if (!s) {
            e.code = DYN_PERSIST_E_MISSING;
            e.sys_errno = 0;
            snprintf(e.msg, sizeof e.msg, "no record section");
            out = dyn_persist_throw(ctx, self->name, "load", path, &e);
            goto done;
        }
        rc = dyn_de_open(&d, s->p, (size_t)s->len, &tid, &flags, 0);
        if (rc != DYN_DE_OK) {
            e.code = rc == DYN_DE_BAD_CRC ? DYN_PERSIST_E_CHECKSUM : DYN_PERSIST_E_CORRUPT;
            e.sys_errno = 0;
            snprintf(e.msg, sizeof e.msg, "record: %s", dyn_de_strerror(rc));
            out = dyn_persist_throw(ctx, self->name, "load", path, &e);
            goto done;
        }
        if (tid != self->type_id) {
            out = dyn_ds_persist_bad(ctx, "record type differs from the file type");
            goto done;
        }
        dyn_ds_rebuild_threw = 0;
        out = self->read(ctx, &d, cmp);
        if (!JS_IsException(out) && !dyn_de_ok(&d)) {
            JS_FreeValue(ctx, out);
            out = dyn_ds_persist_bad(ctx, "record ends early");
        } else if (JS_IsException(out) && !(dyn_ds_rebuild_threw && JS_IsFunction(ctx, cmp))) {
            dyn_persist_tag_corrupt(ctx);
        }
        dyn_ds_rebuild_threw = 0;
    }
done:
    if (r)
        dyn_persist_release(r);
    JS_FreeValue(ctx, cmp);
    return out;
}

int dyn_codec_install_methods(JSContext* ctx)
{
    int i, n = dyn_codec_n();
    for (i = 0; i < n; i++) {
        JSValue proto = JS_GetClassProto(ctx, dyn_codecs[i].class_id);
        JSValue ctor;
        if (!JS_IsObject(proto)) {
            JS_FreeValue(ctx, proto);
            continue;
        }
        JS_DefinePropertyValueStr(ctx, proto, "serialize",
            JS_NewCFunction(ctx, dyn_ds_serialize_method, "serialize", 0), JS_PROP_C_W_E);
        if (dyn_codecs[i].type_id < DYN_TID_ML_BASE)
            JS_DefinePropertyValueStr(ctx, proto, "save",
                JS_NewCFunction(ctx, dyn_ds_save_method, "save", 1), JS_PROP_C_W_E);
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        if (JS_IsFunction(ctx, ctor)) {
            JS_DefinePropertyValueStr(ctx, ctor, "deserialize",
                JS_NewCFunctionMagic(ctx, dyn_ds_deserialize_static,
                    "deserialize", 1,
                    JS_CFUNC_generic_magic, i),
                JS_PROP_C_W_E);
            if (dyn_codecs[i].type_id < DYN_TID_ML_BASE)
                JS_DefinePropertyValueStr(ctx, ctor, "load",
                    JS_NewCFunctionMagic(ctx, dyn_ds_load_static,
                        "load", 1,
                        JS_CFUNC_generic_magic, i),
                    JS_PROP_C_W_E);
        }
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
