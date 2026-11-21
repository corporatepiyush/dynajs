#ifndef DOCDB_INTERNAL_H
#define DOCDB_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DOCDB)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-bson.h"
#include "core/docdb/dd.h"
#include "../vserialize/vserialize-bson.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct DocStream DocStream;

typedef struct {
    dd_db* db;
    int async_jobs;
    uint64_t gen;
    JSContext* ctx;
    DocStream** streams;
    int nstreams, capstreams;
} DocDb;

typedef struct {
    JSValue dbobj;
    char* name;
    size_t name_n;
    dd_coll* cached;
    uint64_t cached_gen;
} DocColl;

extern JSClassID docdb_db_class_id;
extern JSClassID docdb_coll_class_id;
extern JSClassID docdb_err_class_id;
extern JSClassID docdb_stream_class_id;

JSValue docdb_throw(JSContext* ctx, const dd_err* e);
JSValue docdb_throw_code(JSContext* ctx, int code, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

DocDb* docdb_db_get(JSContext* ctx, JSValueConst v);
dd_coll* docdb_coll_get(JSContext* ctx, JSValueConst this_val, DocDb** dbp, int create);
dd_coll* docdb_coll_read(JSContext* ctx, JSValueConst this_val, DocDb** dbp, dd_coll** tmp);
JSValue docdb_coll_new(JSContext* ctx, JSValueConst dbobj, const char* name, size_t n);

int docdb_encode_doc(JSContext* ctx, JSValueConst v, dyn_bson_w* w, const char* what);
int docdb_encode_insert(JSContext* ctx, JSValueConst v, dyn_bson_w* w);
int docdb_encode_opt_doc(JSContext* ctx, JSValueConst v, dyn_bson_w* w, const char* what);
JSValue docdb_decode(JSContext* ctx, const uint8_t* doc);
JSValue docdb_decode_val(JSContext* ctx, dyn_bson_val v);
JSValue docdb_decode_many(JSContext* ctx, const uint8_t* const* docs, uint32_t n, const dd_proj* proj);

int docdb_opt(JSContext* ctx, JSValueConst opts, const char* name, JSValue* out);
int docdb_opt_bool(JSContext* ctx, JSValueConst opts, const char* name, int def, int* out);
int docdb_opt_u64(JSContext* ctx, JSValueConst opts, const char* name, uint64_t* out);
int docdb_opt_doc(JSContext* ctx, JSValueConst opts, const char* name, dyn_bson_w* w, int* present);

int64_t docdb_now_ms(void);

typedef struct {
    dd_filter f;
    dd_sort sort;
    dd_proj proj;
    int has_proj;
    uint64_t skip, limit;
    dyn_bson_w fbuf;
} DocQuery;

int docdb_query_init(JSContext* ctx, DocQuery* q, JSValueConst filter, JSValueConst opts);
void docdb_query_free(DocQuery* q);
int docdb_query_run(JSContext* ctx, dd_coll* c, DocQuery* q, dd_idvec* out, dd_plan* plan_out, dd_stats* st);
int docdb_query_exec(JSContext* ctx, dd_coll* c, DocQuery* q, dd_plan* plan, dd_idvec* out, dd_plan* plan_out,
    dd_stats* st);

int docdb_coll_funcs_count(void);
extern const JSCFunctionListEntry docdb_coll_funcs[];
extern const int docdb_coll_nfuncs;
extern const JSCFunctionListEntry docdb_db_funcs[];
extern const int docdb_db_nfuncs;

JSValue docdb_db_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
JSValue docdb_db_load_static(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue docdb_persist(JSContext* ctx, JSValueConst dbobj, DocDb* d, dd_coll* only, const char* path, int do_fsync);
JSValue docdb_load(JSContext* ctx, JSValueConst dbobj, DocDb* d, const char* path, int fresh);

JSValue docdb_aggregate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue docdb_watch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
void docdb_streams_on_drop(dd_db* db, dd_coll* c, void* owner);
void docdb_streams_kick(DocDb* d);
void docdb_streams_detach(DocDb* d);
int docdb_register_stream(JSContext* ctx, JSModuleDef* m);
JSValue docdb_explain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue docdb_create_index(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue docdb_drop_index(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue docdb_list_indexes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int docdb_write_insert(JSContext* ctx, dd_coll* c, const uint8_t* doc, size_t n, dd_err* e);
int docdb_write_replace(JSContext* ctx, dd_coll* c, uint32_t docno, const uint8_t* doc, size_t n, int op,
    const dd_upd_desc* desc, dd_err* e);
int docdb_write_delete(JSContext* ctx, dd_coll* c, uint32_t docno);

#endif

#endif
