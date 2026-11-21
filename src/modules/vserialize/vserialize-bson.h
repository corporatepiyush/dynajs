#ifndef VSERIALIZE_BSON_H
#define VSERIALIZE_BSON_H

#include "dyna-nat.h"
#include "core/dyn-bson.h"

#if defined(CONFIG_NATIVE_MODULES) && (defined(CONFIG_NATIVE_MODULE_VSERIALIZE) || defined(CONFIG_NATIVE_MODULE_DOCDB))

#define DYN_BSONJS_PROMOTE_LONGS 1
#define DYN_BSONJS_TRUSTED 2
#define DYN_BSONJS_KEYCACHE 4

enum {
    DYN_BSONJS_OBJECTID,
    DYN_BSONJS_TIMESTAMP,
    DYN_BSONJS_DECIMAL128,
    DYN_BSONJS_BINARY,
    DYN_BSONJS_REGEXP,
    DYN_BSONJS_MINKEY,
    DYN_BSONJS_MAXKEY,
    DYN_BSONJS_CODE,
    DYN_BSONJS_NCLASSES
};

typedef struct {
    JSClassID cls[DYN_BSONJS_NCLASSES];
    JSClassID date_cls, regexp_cls;
} DynBsonJsKinds;

int dyn_bsonjs_ensure(JSContext* ctx, const DynBsonJsKinds** kinds);

JSValue dyn_bsonjs_ctor(JSContext* ctx, int which);

typedef struct {
    JSContext* ctx;
    dyn_bson_w* w;
    const DynBsonJsKinds* k;
    int depth;
    int flags;
} DynBsonEnc;

int dyn_bsonjs_encode_doc(DynBsonEnc* e, JSValueConst obj);

int dyn_bsonjs_encode_elem(DynBsonEnc* e, const char* key, size_t klen, JSValueConst v);

int dyn_bsonjs_encode_value_doc(DynBsonEnc* e, JSValueConst v);

typedef struct {
    JSContext* ctx;
    const DynBsonJsKinds* k;
    int flags;
    int depth;
    JSShapeCache tpl[8];
    struct DynBsonKc* kc;
} DynBsonDec;

void dyn_bsonjs_dec_init(DynBsonDec* d, JSContext* ctx, const DynBsonJsKinds* k, int flags);

void dyn_bsonjs_dec_free(DynBsonDec* d);

JSValue dyn_bsonjs_decode_doc(DynBsonDec* d, const uint8_t* doc, int is_array);

JSValue dyn_bsonjs_decode_val(DynBsonDec* d, dyn_bson_val v);

int dyn_bsonjs_oid_of(JSContext* ctx, JSValueConst v, uint8_t out[12]);

JSValue dyn_bsonjs_new_oid(JSContext* ctx, const uint8_t oid[12]);

JSValue dyn_bsonjs_make(JSContext* ctx, int which, const uint8_t* p, size_t n, uint8_t sub, const char* s2);

int dyn_bsonjs_payload(JSValueConst v, int which, const uint8_t** p, size_t* n, uint8_t* sub, const char** s2);

JSValue dyn_bsonjs_encode_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_bsonjs_decode_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int dyn_bsonjs_register(JSContext* ctx, JSModuleDef* m);

int dyn_bsonjs_add_exports(JSContext* ctx, JSModuleDef* m);

#endif

#endif
