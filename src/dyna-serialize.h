/*
 * The DYNS codec registry: how a native class says how to write and read
 * itself. The envelope, the writer and the bounds-checked reader are pure C in
 * src/core/dyn-serial.{c,h}; this is the thin JS-side dispatch over them.
 *
 * A codec lives in the SAME translation unit as the class it serialises,
 * because the container structs are static there. Each TU registers its codecs
 * from its module init -- the same place and the same time its JSClassIDs are
 * assigned, so there is no lazily-built table and nothing to race.
 */
#ifndef DYNA_SERIALIZE_H
#define DYNA_SERIALIZE_H

#include "dynajs.h"

#ifdef CONFIG_NATIVE_MODULES

#include "core/dyn-serial.h"

/* type_id space. Ranges are handed out so that structures and ML never collide
 * even though they version independently. */
#define DYN_TID_BITSET        1
#define DYN_TID_UNIONFIND     2
#define DYN_TID_DEQUE         3
#define DYN_TID_FENWICK       4
#define DYN_TID_RINGBUFFER    5
#define DYN_TID_SEGTREE       6
#define DYN_TID_BLOOMFILTER   7
#define DYN_TID_TRIE          8
#define DYN_TID_LRU           9
#define DYN_TID_SORTEDSET    10
#define DYN_TID_SORTEDMAP    11
#define DYN_TID_HEAP         12
#define DYN_TID_LIST         13
#define DYN_TID_GRAPH        14
#define DYN_TID_MULTISET     20
#define DYN_TID_MULTIMAP     21
#define DYN_TID_BIMAP        22
#define DYN_TID_TABLE        23
#define DYN_TID_RANGESET     24
#define DYN_TID_RANGEMAP     25
#define DYN_TID_INTERVALTREE 26
#define DYN_TID_MINMAXHEAP   27
#define DYN_TID_COUNTMIN     28
#define DYN_TID_HYPERLOGLOG  29
#define DYN_TID_BTREE        30
/* 100.. is the dyna:ml range; see src/dyna-ml.c. */
#define DYN_TID_ML_BASE     100

/* Write `obj` (already known to be of this codec's class) into `w`. Return 0,
 * or -1 with a pending JS exception. */
typedef int (*dyn_codec_write_fn)(JSContext *ctx, dyn_ser_t *w,
                                  JSValueConst obj);
/* Build a fresh object from `r`. `opts` is the caller's second argument to
 * Class.deserialize -- undefined unless the caller supplied one; only the
 * types that genuinely cannot be reconstructed without it (Heap's comparator)
 * look at it. Return JS_EXCEPTION on a malformed record. */
typedef JSValue (*dyn_codec_read_fn)(JSContext *ctx, dyn_de_t *r,
                                     JSValueConst opts);

typedef struct {
    JSClassID class_id;
    uint16_t type_id;
    const char *name;
    dyn_codec_write_fn write;
    dyn_codec_read_fn read;
} dyn_codec_t;

/* Register one codec. Called from a module init, alongside the class ids. */
int dyn_codec_register(const dyn_codec_t *c);

/* Install .serialize() and .deserialize() on every REGISTERED codec's class.
 * Call AFTER all dyn_codec_register() calls and after the classes exist --
 * it reaches each constructor through its prototype's `constructor`. */
int dyn_codec_install_methods(JSContext *ctx);
const dyn_codec_t *dyn_codec_by_class(JSClassID id);
const dyn_codec_t *dyn_codec_by_type(uint16_t type_id);

/* Element payloads for the containers that hold arbitrary JS values: one
 * JS_WriteObject blob per container, written WITHOUT bytecode or
 * SharedArrayBuffer permission, so a record can never smuggle executable code
 * into a reader. */
int dyn_codec_write_values(JSContext *ctx, dyn_ser_t *w, JSValueConst arr);
/* Returns a fresh Array, or JS_EXCEPTION. Rejects a payload that does not read
 * back as an Array. */
JSValue dyn_codec_read_values(JSContext *ctx, dyn_de_t *r);

/* Throw a TypeError naming `code`'s meaning. Always returns JS_EXCEPTION. */
JSValue dyn_codec_throw(JSContext *ctx, int code);

/* Registered by src/dyna-serialize.c into dyna:structures. */
int dyn_serializer_register(JSContext *ctx, JSModuleDef *m);
void dyn_serializer_add_exports(JSContext *ctx, JSModuleDef *m);

#endif /* CONFIG_NATIVE_MODULES */

#endif /* DYNA_SERIALIZE_H */
