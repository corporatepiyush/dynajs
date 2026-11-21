#ifndef VSERIALIZE_INTERNAL_H
#define VSERIALIZE_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && (defined(CONFIG_NATIVE_MODULE_VSERIALIZE) || defined(CONFIG_NATIVE_MODULE_DOCDB) || defined(CONFIG_NATIVE_MODULE_SCHEMA))

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-sb.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

uint64_t dyn_xxh64(const void* data, size_t len, uint64_t seed);

#define VS_MAX_DEPTH 256
#define VS_BIGNUM_MAX_BYTES 131072
#define VS_MAX_ITEMS (1u << 26)

enum { FMT_MSGPACK,
    FMT_CBOR };

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} vb_t;

__attribute__((noinline)) int vb_grow(vb_t* b, size_t n);

typedef struct {
    JSContext* ctx;
    vb_t b;
    int fmt, depth, sorted, bigint;
    JSValue* seen;
    int nseen, cseen;
} venc_t;

int venc_value(venc_t* e, JSValueConst v);

#define VDEC_TPL_DEPTH 8

typedef struct {
    JSContext* ctx;
    const uint8_t* p;
    size_t n, i;
    int fmt, depth, bigint;
    JSShapeCache tpl[VDEC_TPL_DEPTH];
} vdec_t;

JSValue vdec_value(vdec_t* d);

typedef struct {
    JSContext* ctx;
    JSValue *src, *dst;
    uint32_t* idx;
    uint32_t imask;
    int n, cap, depth;
    JSValue date_ctor, date_proto, date_gettime;
    JSValue regexp_ctor, regexp_proto;
    JSValue map_ctor, map_proto, map_set, map_entries;
    JSValue set_ctor, set_proto, set_add, set_values;
    JSValue object_proto, array_proto;
    int array_plain;
} vclone_t;

JSValue vclone(vclone_t* c, JSValueConst v);

JSValue vs_clone_bytes(vclone_t* c, JSValueConst v);

void vclone_free_cache(vclone_t* c);

int vclone_cache_protos(vclone_t* c);

#endif

#endif
