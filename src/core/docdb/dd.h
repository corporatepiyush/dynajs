#ifndef DYN_DOCDB_DD_H
#define DYN_DOCDB_DD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../bson/bson.h"
#include "../ds/ds-roaring-bitmap.h"

#if defined(__has_attribute)
#if __has_attribute(uninitialized)
#define DD_SCRATCH __attribute__((uninitialized))
#endif
#endif
#ifndef DD_SCRATCH
#define DD_SCRATCH
#endif

#define DD_E_OK 0
#define DD_E_INTERNAL 1
#define DD_E_BADVALUE 2
#define DD_E_FAILEDTOPARSE 9
#define DD_E_TYPEMISMATCH 14
#define DD_E_INDEXNOTFOUND 27
#define DD_E_NAMESPACENOTFOUND 26
#define DD_E_PATHNOTVIABLE 28
#define DD_E_CONFLICTINGUPDATE 40
#define DD_E_NSEXISTS 48
#define DD_E_IMMUTABLEFIELD 66
#define DD_E_INVALIDOPTIONS 72
#define DD_E_INDEXOPTIONSCONFLICT 85
#define DD_E_INDEXKEYSPECSCONFLICT 86
#define DD_E_DOCVALIDATION 121
#define DD_E_PARALLELARRAYS 171
#define DD_E_HISTORYLOST 286
#define DD_E_DUPKEY 11000
#define DD_E_OOM 146
#define DD_E_BADSNAPSHOT 100001
#define DD_E_IO 100002
#define DD_E_TOOLARGE 10334
#define DD_E_REGEXTIMEOUT 100003

#define DD_MAX_DOC (16u * 1024u * 1024u)
#define DD_MAX_PATH_PARTS 64
#define DD_CHUNK_SIZE (1u << 20)

typedef struct {
    int code;
    char msg[320];
} dd_err;

int dd_fail(dd_err* e, int code, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

const char* dd_code_name(int code);

const char* dd_repr(dyn_bson_val v, char* out, size_t cap);

typedef struct dd_chunk {
    struct dd_chunk* next;
    struct dd_chunk* prev;
    size_t cap, used, live;
    uint64_t retired_epoch;
    uint8_t* data;
    int owns_data;
} dd_chunk;

typedef struct dd_db dd_db;

typedef struct {
    dd_db* db;
    dd_chunk* cur;
    dd_chunk* head;
    size_t bytes;
    size_t nchunks;
} dd_arena;

void dd_arena_init(dd_arena* a, dd_db* db);
const uint8_t* dd_arena_put(dd_arena* a, const uint8_t* p, size_t n, dd_chunk** out_chunk);
void dd_arena_release(dd_arena* a, dd_chunk* c, size_t n);
dd_chunk* dd_arena_adopt(dd_arena* a, uint8_t* data, size_t n);
void dd_arena_drop_all(dd_arena* a);

typedef struct {
    uint64_t hash;
    uint32_t off;
    uint32_t len;
    uint32_t val;
} dd_kslot;

typedef struct {
    dd_kslot* slots;
    uint32_t cap, count, tombs;
    uint8_t* pool;
    size_t pool_n, pool_cap;
    size_t pool_dead;
} dd_kmap;

void dd_kmap_init(dd_kmap* m);
void dd_kmap_free(dd_kmap* m);
uint64_t dd_hash_bytes(const uint8_t* p, size_t n);
int dd_kmap_get(const dd_kmap* m, const uint8_t* k, size_t n, uint32_t* val);
int dd_kmap_put(dd_kmap* m, const uint8_t* k, size_t n, uint32_t val, uint32_t* existing);
int dd_kmap_reserve(dd_kmap* m, uint32_t n);
int dd_kmap_put_h(dd_kmap* m, const uint8_t* k, size_t n, uint64_t h, uint32_t val, uint32_t* existing);
int dd_kmap_del(dd_kmap* m, const uint8_t* k, size_t n);
const uint8_t* dd_kmap_key(const dd_kmap* m, uint32_t off);

typedef struct {
    char** part;
    uint32_t* plen;
    uint8_t* numeric;
    uint32_t* index;
    int n;
    char* text;
    size_t tlen;
} dd_path;

int dd_path_parse(dd_path* p, const char* s, size_t n, dd_err* e);
void dd_path_free(dd_path* p);

typedef int (*dd_visit_fn)(void* ud, dyn_bson_val v, int is_array_elem, int32_t first_arr_idx);

int dd_path_walk(const uint8_t* doc, const dd_path* p, dd_visit_fn fn, void* ud);

int dd_path_get_exact(const uint8_t* doc, const dd_path* p, dyn_bson_val* out);

int dd_lookup_dotted(const uint8_t* doc, const char* key, size_t klen, dyn_bson_val* out);

typedef struct {
    uint8_t* bc;
    int ncap;
    uint16_t* u16;
    size_t u16cap;
} dd_regex;

int dd_regex_compile(dd_regex* r, const char* pat, size_t plen, const char* opts, size_t olen, void* opaque, dd_err* e);
int dd_regex_match(dd_regex* r, const uint8_t* s, size_t n, void* opaque, dd_err* e);
void dd_regex_free(dd_regex* r);

enum {
    DD_F_AND = 1,
    DD_F_OR,
    DD_F_NOR,
    DD_F_TRUE,
    DD_F_FALSE,
    DD_F_FIELD,
};

enum {
    DD_OP_EQ = 1,
    DD_OP_NE,
    DD_OP_GT,
    DD_OP_GTE,
    DD_OP_LT,
    DD_OP_LTE,
    DD_OP_IN,
    DD_OP_NIN,
    DD_OP_EXISTS,
    DD_OP_TYPE,
    DD_OP_REGEX,
    DD_OP_ELEMMATCH_DOC,
    DD_OP_ELEMMATCH_VAL,
    DD_OP_SIZE,
    DD_OP_ALL,
    DD_OP_NOT,
    DD_OP_MOD,
};

typedef struct dd_fnode {
    uint8_t kind;
    uint8_t op;
    uint8_t neg;
    uint32_t nkids;
    struct dd_fnode** kids;
    dd_path* path;
    dyn_bson_val arg;
    dd_regex* re;
    uint8_t** inkeys;
    uint32_t* inklen;
    uint32_t nin;
    uint32_t type_mask;
    int64_t mod_d, mod_r;
    dd_regex* inre;
    uint32_t ninre;
    uint8_t in_null;
    uint8_t path_borrowed;
    uint8_t owns_path;
    uint8_t shared_top;
} dd_fnode;

typedef struct {
    dd_fnode* root;
    uint8_t* src;
    size_t src_n;
    int has_regex;
    int empty;
    void* re_opaque;
} dd_filter;

int dd_filter_compile(dd_filter* f, const uint8_t* bson, void* re_opaque, dd_err* e);
void dd_filter_free(dd_filter* f);

typedef struct {
    int32_t pos;
    dd_err* err;
    void* re_opaque;
    int failed;
    const uint8_t* cdoc;
    const char* ckey;
    uint32_t cklen;
    int cfound;
    dyn_bson_val cval;
} dd_match_ctx;

int dd_filter_match(const dd_filter* f, const uint8_t* doc, dd_match_ctx* mc);
int dd_node_match(const dd_fnode* n, const uint8_t* doc, dd_match_ctx* mc);
int dd_value_matches_ops(const dd_fnode* n, dyn_bson_val v, dd_match_ctx* mc);

typedef struct {
    dd_path path;
    int desc;
} dd_ixfield;

typedef struct dd_index {
    char* name;
    dd_ixfield* f;
    int nf;
    int unique, sparse, multikey;
    dd_filter* partial;
    uint8_t* spec;
    size_t spec_n;
    dd_kmap keys;
    dyn_roar_t* post;
    uint32_t nkeys, capkeys;
    uint32_t* koff;
    uint32_t* klen;
    uint32_t* sorted;
    uint32_t nsorted, capsorted;
    uint32_t* tail;
    uint32_t ntail, captail;
    uint8_t* inord;
    uint32_t nempty;
    uint64_t entries;
    struct dd_coll* owner;
    dyn_ks_buf skb;
    uint32_t* soffs;
    uint32_t scap;
} dd_index;

typedef struct {
    uint64_t seq;
    uint8_t* ev;
    uint32_t ev_n;
    uint32_t docno;
    uint8_t op;
} dd_event;

typedef struct {
    dd_event* ev;
    uint32_t cap;
    uint64_t head;
    uint64_t first_seq;
    uint64_t evicted;
} dd_ring;

typedef struct dd_coll {
    char* name;
    size_t name_n;
    dd_db* db;
    dd_arena arena;
    const uint8_t** doc;
    dd_chunk** dchunk;
    uint32_t nslots, capslots;
    uint32_t* freel;
    uint32_t nfree, capfree;
    uint32_t count;
    dd_kmap ids;
    dyn_ks_buf idkb;
    dd_index** ix;
    int nix;
    dd_ring ring;
    int ring_on;
    uint32_t ring_cap;
    uint8_t* options;
    size_t options_n;
    uint64_t version;
    void* js_validator;
} dd_coll;

struct dd_db {
    char* name;
    dd_coll** coll;
    int ncoll, capcoll;
    uint64_t epoch;
    uint64_t* pins;
    int npins, cappins;
    dd_chunk* retired;
    uint64_t seq;
    int ring_default;
    size_t retired_bytes;
    int watching;
    uint64_t unrecorded;
    void (*on_drop)(struct dd_db* db, dd_coll* c, void* owner);
    void* owner;
};

dd_db* dd_db_new(const char* name);
void dd_db_free(dd_db* db);
dd_coll* dd_db_find(dd_db* db, const char* name, size_t n);
dd_coll* dd_db_create(dd_db* db, const char* name, size_t n, dd_err* e);
int dd_db_drop(dd_db* db, const char* name, size_t n);
int dd_db_install(dd_db* db, dd_coll* c);
uint64_t dd_db_pin(dd_db* db);
void dd_db_unpin(dd_db* db, uint64_t epoch);
void dd_db_retire(dd_db* db, dd_chunk* c);
void dd_db_sweep(dd_db* db);

int dd_coll_name_ok(const char* s, size_t n, dd_err* e);
dd_coll* dd_coll_new(dd_db* db, const char* name, size_t n);
void dd_coll_free(dd_coll* c);
int dd_coll_id_key(const uint8_t* doc, dyn_ks_buf* kb, dd_err* e);
int dd_coll_find_id(const dd_coll* c, dyn_bson_val id, uint32_t* docno);
int dd_coll_reserve(dd_coll* c, uint32_t n);
int dd_coll_load_insert(dd_coll* c, const uint8_t* doc, size_t n, const uint8_t* key, size_t kn, uint64_t h, dd_err* e);
int dd_coll_insert(dd_coll* c, const uint8_t* doc, size_t n, uint32_t* docno, dd_err* e);
int dd_coll_replace(dd_coll* c, uint32_t docno, const uint8_t* doc, size_t n, dd_err* e);
int dd_coll_update(dd_coll* c, uint32_t docno, const uint8_t* doc, size_t n, dd_err* e);
int dd_coll_remove(dd_coll* c, uint32_t docno);
int dd_coll_check_doc(const uint8_t* doc, size_t n, int for_storage, dd_err* e);
void dd_coll_clear(dd_coll* c);

int dd_index_create(dd_coll* c, const uint8_t* keys, const uint8_t* opts, dd_index** out, dd_err* e);
int dd_index_create_unfilled(dd_coll* c, const uint8_t* keys, const uint8_t* opts, dd_index** out, dd_err* e);
int dd_index_fill(const dd_coll* c, dd_index* ix, dd_err* e);
void dd_index_fill_many(const dd_coll* c, dd_index** ix, int nix, dd_err* err, int* rc);
int dd_index_drop(dd_coll* c, const char* name, size_t n, dd_err* e);
dd_index* dd_index_find(dd_coll* c, const char* name, size_t n);
void dd_index_free(dd_index* ix);
int dd_index_doc_keys(const dd_index* ix, const uint8_t* doc, dyn_ks_buf* kb, uint32_t** offs, uint32_t* nkeys,
    uint32_t* cap, dd_err* e);
int dd_index_add(dd_index* ix, uint32_t docno, const uint8_t* doc, dd_err* e);
void dd_index_remove(dd_index* ix, uint32_t docno, const uint8_t* doc);
int dd_index_check_unique(dd_index* ix, uint32_t docno, const uint8_t* doc, dd_err* e);
int dd_index_merge(dd_index* ix);
int dd_index_lookup_eq(const dd_index* ix, const uint8_t* k, size_t n, uint32_t* kid);

typedef struct {
    const uint8_t* lo;
    uint32_t lo_n;
    int lo_incl;
    const uint8_t* hi;
    uint32_t hi_n;
    int hi_incl;
    int exact;
} dd_interval;

int dd_index_scan(dd_index* ix, const dd_interval* iv, int niv, dyn_roar_t* out, uint64_t* keys_examined);
typedef int (*dd_ix_take)(void* arg, const dyn_roar_t* post);
int dd_index_walk(dd_index* ix, const dd_interval* iv, int niv, int rev, dd_ix_take take, void* arg,
    uint64_t* keys_examined);
uint64_t dd_index_scan_card(dd_index* ix, const dd_interval* iv, int niv, uint64_t* keys_examined, uint64_t cap);

typedef struct {
    dd_path path;
    int dir;
} dd_sortkey;

typedef struct {
    dd_sortkey* k;
    int n;
} dd_sort;

int dd_sort_parse(dd_sort* s, const uint8_t* spec, dd_err* e);
void dd_sort_free(dd_sort* s);
void dd_sort_key(const dd_sort* s, const uint8_t* doc, dyn_ks_buf* kb);

typedef struct {
    uint64_t pre;
    uint32_t off;
    uint32_t len;
    uint32_t id;
} dd_sortrec;

int dd_sort_records(dd_sortrec* r, uint32_t n, const uint8_t* keys, uint64_t limit);

enum {
    DD_PLAN_COLLSCAN = 1,
    DD_PLAN_IDHACK,
    DD_PLAN_IXSCAN,
    DD_PLAN_IXAND,
    DD_PLAN_OR,
    DD_PLAN_EOF,
};

typedef struct {
    int ix;
    dd_interval* iv;
    int niv;
    uint8_t* bounds;
    size_t bounds_n;
    uint64_t est;
    uint64_t keys;
    int two_scan;
    dd_interval* iv2;
    int niv2;
    int field_n;
} dd_ixplan;

typedef struct {
    int stage;
    dd_ixplan* scans;
    int nscans;
    uint64_t est;
    int sort_ix;
    int sort_rev;
    int topk;
    int topk_rev;
    int topk_ix;
    char** rejected;
    int nrejected;
    uint32_t id_docno;
    int id_found;
    uint8_t* scratch;
    size_t scratch_n, scratch_cap;
} dd_plan;

int dd_plan_make(dd_coll* c, const dd_filter* f, const dd_sort* sort, dd_plan* p, dd_err* e);
void dd_plan_free(dd_plan* p);
const char* dd_plan_stage_name(int stage);

typedef struct {
    uint64_t keys_examined;
    uint64_t docs_examined;
    uint64_t n_returned;
    int used_index_sort;
} dd_stats;

typedef struct {
    uint32_t* ids;
    uint32_t n, cap;
} dd_idvec;

int dd_idvec_push(dd_idvec* v, uint32_t x);
void dd_idvec_free(dd_idvec* v);

int dd_plan_candidates(dd_coll* c, dd_plan* p, dd_idvec* out, int* all, dd_stats* st);
int dd_plan_topk(dd_coll* c, dd_plan* p, const dd_filter* f, uint64_t want, dd_idvec* out, dd_stats* st);
int dd_plan_count(dd_coll* c, const dd_plan* p, const dd_filter* f, uint64_t* n);
int dd_index_distinct_reps(dd_coll* c, dd_index* ix, dd_idvec* out);

typedef struct {
    const uint8_t* const* docs;
    uint32_t ndocs;
} dd_view;

int dd_exec_find(dd_view v, const uint32_t* cand, uint32_t ncand, int all, const dd_filter* f,
    const dd_sort* sort, uint64_t skip, uint64_t limit, dd_idvec* out, dd_stats* st, dd_err* e);

typedef struct dd_projnode {
    char* name;
    uint32_t nlen;
    struct dd_projnode* kids;
    uint32_t nkids, capkids;
    int leaf;
} dd_projnode;

typedef struct {
    dd_projnode root;
    int inclusion;
    int id_mode;
    int empty;
} dd_proj;

int dd_proj_parse(dd_proj* p, const uint8_t* spec, dd_err* e);
void dd_proj_free(dd_proj* p);
int dd_proj_apply(const dd_proj* p, const uint8_t* doc, dyn_bson_w* w);

typedef struct dd_upd dd_upd;

typedef struct {
    dyn_bson_w updated;
    dyn_bson_w removed;
    int n_updated, n_removed;
    int replaced;
} dd_upd_desc;

int dd_update_parse(dd_upd** out, const uint8_t* update, const uint8_t* array_filters, dd_err* e);
void dd_update_free(dd_upd* u);
int dd_update_is_replacement(const dd_upd* u);
int dd_update_apply(const dd_upd* u, const uint8_t* doc, int32_t pos, int is_insert, int64_t now_ms, dyn_bson_w* out,
    dd_upd_desc* desc, dd_err* e);
int dd_upsert_seed(const uint8_t* filter, dyn_bson_w* out, dd_err* e);

void dd_desc_init(dd_upd_desc* d);
void dd_desc_free(dd_upd_desc* d);

enum {
    DD_EV_INSERT = 1,
    DD_EV_UPDATE,
    DD_EV_REPLACE,
    DD_EV_DELETE,
    DD_EV_DROP,
    DD_EV_INVALIDATE,
};

const char* dd_event_name(int op);
int dd_ring_enable(dd_coll* c, uint32_t cap);
void dd_ring_free(dd_ring* r);
int dd_emit(dd_coll* c, int op, const uint8_t* id_elem_doc, const uint8_t* full, const dd_upd_desc* desc, uint32_t docno,
    int64_t now_ms);
const dd_event* dd_ring_at(const dd_ring* r, uint64_t seq);
uint64_t dd_ring_oldest(const dd_ring* r);
void dd_token_hex(uint64_t seq, char out[17]);
int dd_token_parse(const char* s, size_t n, uint64_t* seq);

typedef struct dd_agg dd_agg;

typedef struct {
    const char* name;
    size_t name_n;
    dd_view view;
} dd_agg_source;

int dd_agg_compile(dd_agg** out, const uint8_t* pipeline, void* re_opaque, dd_err* e);
void dd_agg_free(dd_agg* a);
int dd_agg_has_regex(const dd_agg* a);
int dd_agg_lookup_count(const dd_agg* a);
const char* dd_agg_lookup_name(const dd_agg* a, int i, size_t* n);
const uint8_t* dd_agg_leading_match(const dd_agg* a);
const uint8_t* dd_agg_leading_sort(const dd_agg* a);
int dd_agg_prefix_len(const dd_agg* a);
uint64_t dd_agg_prefix_skip(const dd_agg* a);
uint64_t dd_agg_prefix_limit(const dd_agg* a);
typedef struct dd_rows dd_rows;
int dd_agg_run(dd_agg* a, int whole, const uint8_t* const* input, uint32_t ninput, const dd_agg_source* srcs, int nsrc,
    int64_t now_ms, dd_rows** out, dd_err* e);
int dd_agg_stream_ok(const dd_agg* a);
int dd_agg_count_ok(const dd_agg* a);
int dd_agg_run_count(dd_agg* a, uint32_t n, const dd_agg_source* srcs, int nsrc, int64_t now_ms, dd_rows** out,
    dd_err* e);
uint32_t dd_rows_count(const dd_rows* r);
const uint8_t* dd_rows_at(const dd_rows* r, uint32_t i);
void dd_rows_free(dd_rows* r);

typedef struct {
    char* name;
    size_t name_n;
    const uint8_t* const* docs;
    uint32_t ndocs;
    const uint8_t* meta;
} dd_snap_coll;

int dd_snap_write(const char* path, const uint8_t* catalog, const dd_snap_coll* colls, int ncoll, int do_fsync,
    uint64_t* bytes_out, dd_err* e);

typedef struct {
    uint8_t* catalog;
    size_t catalog_n;
    dd_coll** colls;
    int ncoll;
    uint64_t bytes;
    uint64_t docs;
} dd_snap_loaded;

int dd_snap_read(const char* path, dd_db* owner, dd_snap_loaded* out, dd_err* e);
void dd_snap_loaded_free(dd_snap_loaded* out);
int dd_snap_coll_meta(const dd_coll* c, dyn_bson_w* w);
void dd_snap_loaded_free(dd_snap_loaded* l);

#endif
