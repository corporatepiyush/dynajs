#ifndef DYNA_SERIALIZE_H
#define DYNA_SERIALIZE_H

#include "dynajs.h"

#ifdef CONFIG_NATIVE_MODULES

#include "core/dyn-serial.h"

#define DYN_TID_BITSET 1
#define DYN_TID_UNIONFIND 2
#define DYN_TID_DEQUE 3
#define DYN_TID_FENWICK 4
#define DYN_TID_RINGBUFFER 5
#define DYN_TID_SEGTREE 6
#define DYN_TID_BLOOMFILTER 7
#define DYN_TID_TRIE 8
#define DYN_TID_LRU 9
#define DYN_TID_SORTEDSET 10
#define DYN_TID_SORTEDMAP 11
#define DYN_TID_HEAP 12
#define DYN_TID_LIST 13
#define DYN_TID_GRAPH 14
#define DYN_TID_MULTISET 20
#define DYN_TID_MULTIMAP 21
#define DYN_TID_BIMAP 22
#define DYN_TID_TABLE 23
#define DYN_TID_RANGESET 24
#define DYN_TID_RANGEMAP 25
#define DYN_TID_INTERVALTREE 26
#define DYN_TID_MINMAXHEAP 27
#define DYN_TID_COUNTMIN 28
#define DYN_TID_HYPERLOGLOG 29
#define DYN_TID_BTREE 30
#define DYN_TID_ML_BASE 100

typedef int (*dyn_codec_write_fn)(JSContext* ctx, dyn_ser_t* w,
    JSValueConst obj);
typedef JSValue (*dyn_codec_read_fn)(JSContext* ctx, dyn_de_t* r,
    JSValueConst opts);

typedef struct {
    JSClassID class_id;
    uint16_t type_id;
    const char* name;
    dyn_codec_write_fn write;
    dyn_codec_read_fn read;
} dyn_codec_t;

int dyn_codec_register(const dyn_codec_t* c);

int dyn_codec_install_methods(JSContext* ctx);
const dyn_codec_t* dyn_codec_by_class(JSClassID id);
const dyn_codec_t* dyn_codec_by_type(uint16_t type_id);

int dyn_codec_write_values(JSContext* ctx, dyn_ser_t* w, JSValueConst arr);
JSValue dyn_codec_read_values(JSContext* ctx, dyn_de_t* r);

JSValue dyn_codec_throw(JSContext* ctx, int code);

int dyn_serializer_register(JSContext* ctx, JSModuleDef* m);
void dyn_serializer_add_exports(JSContext* ctx, JSModuleDef* m);

#endif

#endif
