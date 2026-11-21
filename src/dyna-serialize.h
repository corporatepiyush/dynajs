#ifndef DYNA_SERIALIZE_H
#define DYNA_SERIALIZE_H

#include "dynajs.h"

#ifdef CONFIG_NATIVE_MODULES

#include "core/dyn-persist.h"
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

#define DYN_PKIND_RECORD 1u
#define DYN_PKIND_WORDS 2u
#define DYN_PKIND_PARR_LAYOUT 16u
#define DYN_PKIND_PARR_FIELD 17u
#define DYN_PKIND_PARR_META 18u
#define DYN_PKIND_PARR_DIR 19u

typedef struct {
    uint64_t lineage, gen;
    dyn_persist_r* map;
    uint64_t* hashes;
    uint64_t* dirty;
    uint64_t nchunks;
    int hash_ok;
} dyn_ds_pstate;

typedef struct {
    int (*sections)(JSContext* ctx, JSValueConst obj, dyn_persist_w* w);
    dyn_ds_pstate* (*state)(JSValueConst obj);
    JSValue (*load)(JSContext* ctx, dyn_persist_r* r, int borrow);
    void (*committed)(JSValueConst obj, int ok);
} dyn_codec_persist_t;

int dyn_codec_persist_register(uint16_t type_id, const dyn_codec_persist_t* p);
int dyn_ds_pstate_track(dyn_ds_pstate* ps, const void* p, uint64_t len);
void dyn_ds_pstate_free(dyn_ds_pstate* ps);
void dyn_ds_pstate_committed(dyn_ds_pstate* ps, int ok);
JSValue dyn_ds_persist_bad(JSContext* ctx, const char* what);
extern _Thread_local int dyn_ds_rebuild_threw;

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
