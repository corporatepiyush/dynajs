#ifndef SCHEMA_INTERNAL_H
#define SCHEMA_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-pct.h"
#include "core/dyn-fmtcheck.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include "libregexp.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_SC_MAX_DEPTH 512
#define DYN_SC_MAX_REFCHAIN 64
#define DYN_SC_MAX_ERRORS 256
#define DYN_SC_MAX_LIST (1 << 20)
#define DYN_SC_MAX_PATTERN (64 * 1024)
#define DYN_SC_T_OBJECT 4
#define DYN_SC_T_ARRAY 8
#define DYN_SC_T_NUMBER 16
#define DYN_SC_T_STRING 32

typedef struct dyn_scnode dyn_scnode_t;

typedef struct {
    dyn_scnode_t** v;
    int n;
} dyn_scsubs_t;

typedef struct {
    char** names;
    dyn_scnode_t** v;
    int n;
} dyn_scnamed_t;

typedef struct {
    char* pat;
    uint8_t* re;
    dyn_scnode_t* node;
} dyn_scpat_t;

typedef struct {
    char** props;
    char*** need;
    int* nneed;
    int n;
} dyn_screq_t;

enum {
    SC_F_TYPE = 1u << 0,
    SC_F_CONST = 1u << 1,
    SC_F_ENUM = 1u << 2,
    SC_F_MIN = 1u << 3,
    SC_F_MAX = 1u << 4,
    SC_F_EMIN = 1u << 5,
    SC_F_EMAX = 1u << 6,
    SC_F_MULT = 1u << 7,
    SC_F_MINLEN = 1u << 8,
    SC_F_MAXLEN = 1u << 9,
    SC_F_PAT = 1u << 10,
    SC_F_MINITEMS = 1u << 11,
    SC_F_MAXITEMS = 1u << 12,
    SC_F_UNIQUE = 1u << 13,
    SC_F_MINPROPS = 1u << 14,
    SC_F_MAXPROPS = 1u << 15,
    SC_F_REQ = 1u << 16,
    SC_F_PROPS = 1u << 17,
    SC_F_PATTS = 1u << 18,
    SC_F_ADDL = 1u << 19,
    SC_F_ITEMS = 1u << 20,
    SC_F_PREFIX = 1u << 21,
    SC_F_ALL = 1u << 22,
    SC_F_ANY = 1u << 23,
    SC_F_ONE = 1u << 24,
    SC_F_NOT = 1u << 25,
    SC_F_IF = 1u << 26,
    SC_F_REF = 1u << 27,
    SC_F_DEPREQ = 1u << 28,
    SC_F_CONT = 1u << 29,
    SC_F_MINCONT = 1u << 30,
    SC_F_MAXCONT = 1u << 31,
    SC_F_PNAMES = 1ull << 32,
    SC_F_DEPSCH = 1ull << 33,
    SC_F_UNEVALP = 1ull << 34,
    SC_F_UNEVALI = 1ull << 35,
    SC_F_FMT = 1ull << 37,
    SC_F_UREACH = 1ull << 36,
    SC_F_DREF = 1ull << 38,
};

enum { SC_FMT_REGISTERED = 0,
    SC_FMT_ANNOTATION = 1,
    SC_FMT_ASSERTION = 2 };

typedef struct dyn_scres dyn_scres_t;

enum { SC_SCHEMA = 0,
    SC_BOOL = 1 };

struct dyn_scnode {
    uint64_t f;
    int kind;
    int idx;
    int boolv;
    int type_mask;
    double dmin, dmax, dmult;
    double demin, demax;
    int minlen, maxlen;
    int minitems, maxitems;
    int minprops, maxprops;
    int mincont, maxcont;
    int unique;
    JSValue val;
    JSValue* enumv;
    int n_enum;
    uint8_t* re;
    char* ref;
    char* fmt;
    dyn_scnode_t* ref_tgt;
    char* dref;
    char* dref_name;
    dyn_scnode_t* dref_tgt;
    dyn_scres_t* res;
    dyn_scnamed_t props;
    dyn_scnamed_t depsch;
    dyn_scnode_t* pn;
    dyn_scnode_t* cont;
    dyn_scpat_t* patts;
    int n_patts;
    int addl_allow;
    dyn_scnode_t* addl;
    int unevalp_allow;
    dyn_scnode_t* unevalp;
    int unevali_allow;
    dyn_scnode_t* unevali;
    dyn_scnode_t* items;
    dyn_scsubs_t prefix;
    dyn_scnamed_t required;
    dyn_screq_t depreq;
    dyn_scsubs_t all, any, one;
    dyn_scnode_t *notn, *ifn, *then, *els;
};

struct dyn_scres {
    char* uri;
    char* prefix;
    dyn_scnode_t* root;
    char** anames;
    dyn_scnode_t** anodes;
    int* adyn;
    int na;
};

typedef struct {
    dyn_scnode_t* root;
    dyn_scnode_t** nodes;
    int n_nodes, cap_nodes;
    dyn_scres_t** res;
    int n_res, cap_res;
    int fmt_mode;
} dyn_schema_t;

extern JSClassID dyn_schema_class_id;

typedef struct {
    JSContext* ctx;
    dyn_schema_t* sch;
    char** paths;
    dyn_scnode_t** pnode;
    int n_paths, cap_paths;
    int depth;
    dyn_scres_t* cur;
    const char* base_uri;
    char* base_owned;
    JSValue resources;
    int n_docs;
    int no_validation;
    JSValue docv[64];
    char docp[64][16];
    int ndocv;
} dyn_sccomp_t;

char* dyn_uri_resolve(JSContext* ctx, const char* base, const char* ref);

dyn_scres_t* dyn_sc_res_find(dyn_schema_t* sch, const char* uri, size_t ulen);

dyn_scres_t* dyn_sc_res_add(dyn_sccomp_t* c, const char* uri, size_t ulen,
    const char* prefix, size_t plen, dyn_scnode_t* root);

int dyn_sc_res_anchor_add(dyn_sccomp_t* c, dyn_scres_t* r, const char* name,
    dyn_scnode_t* n, int dynamic);

dyn_scnode_t* dyn_sc_res_anchor(const dyn_scres_t* r, const char* name, int dynamic_only);

void dyn_sc_res_free_rt(JSRuntime* rt, dyn_scres_t* r);

dyn_scnode_t* dyn_sc_new_node(dyn_sccomp_t* c);

static inline int dyn_sc_kw(JSContext* ctx, JSValueConst obj, const char* name,
    JSValue* out)
{
    *out = JS_GetPropertyStr(ctx, obj, name);
    return JS_IsException(*out) ? -1 : 0;
}

static inline uint64_t dyn_sc_arr_len(JSContext* ctx, JSValue arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint64_t len;
    if (JS_ToIndex(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        return (uint64_t)-1;
    }
    JS_FreeValue(ctx, lv);
    return len;
}

static inline int dyn_sc_is_array(JSContext* ctx, JSValueConst v)
{
    int r = JS_IsArray(ctx, v);
    if (r < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    return r;
}

int dyn_sc_compile(dyn_sccomp_t* c, JSValueConst sv, const char* path,
    size_t plen, dyn_scnode_t** out);

int dyn_sc_type_name(const char* s, size_t n, int* mask);

int dyn_sc_read_num(JSContext* ctx, JSValueConst v, const char* kw,
    double* out);

int dyn_sc_read_int(JSContext* ctx, JSValueConst v, const char* kw,
    int* out);

int dyn_sc_read_map(JSContext* ctx, JSValueConst v, const char* kw,
    char*** pnames, JSValue** pvals, int* pn);

JSValue dyn_sc_fmt_lookup(JSContext* ctx, const char* name);

int dyn_sc_str_kw(JSContext* ctx, JSValueConst sv, const char* kw, char** out);

int dyn_sc_read_anchors(dyn_sccomp_t* c, JSValueConst sv, dyn_scnode_t* n);

int dyn_sc_check_list(JSContext* ctx, JSValueConst v, const char* kw,
    uint64_t* len);

int dyn_sc_load_doc(dyn_sccomp_t* c, const char* uri, size_t ulen);

int dyn_sc_check_pure_ref_cycles(dyn_sccomp_t* c);

void dyn_sc_schema_free_rt(JSRuntime* rt, dyn_schema_t* sch);

int dyn_sc_inst_type(JSContext* ctx, JSValueConst v);

int dyn_sc_get_own(JSContext* ctx, JSValueConst obj, JSAtom a,
    JSValue* out);

int dyn_sc_utf8_to_utf16(JSContext* ctx, const char* s, size_t n,
    uint16_t** out);

typedef struct {
    JSValueConst a, b;
} dyn_sc_pair_t;

int dyn_sc_equal(JSContext* ctx, JSValueConst a, JSValueConst b,
    int depth, dyn_sc_pair_t* seen, int nseen);

typedef struct {
    const char* p;
    size_t n;
} dyn_scseg_t;

typedef struct {
    dyn_scseg_t* seg;
    size_t len, cap;
} dyn_scpath_t;

typedef struct {
    char** props;
    int* plens;
    int nprops, cprops;
    uint64_t* items;
    int nitems, citems;
} dyn_sceval_t;

void dyn_sc_eval_free(JSContext* ctx, dyn_sceval_t* e);

int dyn_sc_eval_add_prop(JSContext* ctx, dyn_sceval_t* e,
    const char* name, size_t len);

int dyn_sc_eval_has_prop(const dyn_sceval_t* e, const char* name,
    size_t len);

int dyn_sc_eval_add_item(JSContext* ctx, dyn_sceval_t* e, uint64_t idx);

int dyn_sc_eval_has_item(const dyn_sceval_t* e, uint64_t idx);

int dyn_sc_eval_merge(JSContext* ctx, dyn_sceval_t* dst,
    dyn_sceval_t* src);

typedef struct {
    char* path;
    int pathlen;
    char *message, *keyword;
} dyn_scerr_t;

typedef struct {
    dyn_scerr_t* e;
    int n, cap;
    int done;
    int drops;
} dyn_scerrs_t;

typedef struct {
    JSContext* ctx;
    dyn_schema_t* sch;
    dyn_scerrs_t* errs;
    dyn_scpath_t path;
    int depth;
    dyn_scnode_t* refstk[DYN_SC_MAX_REFCHAIN];
    int refn;
    dyn_sc_pair_t* eqseen;
    dyn_sceval_t* cur_eval;
    dyn_scres_t** dstack;
    int dn;
} dyn_vctx_t;

static inline void dyn_sc_errs_reset(JSContext* ctx, dyn_scerrs_t* s)
{
    int i;
    for (i = 0; i < s->n; i++) {
        js_free(ctx, s->e[i].path);
        js_free(ctx, s->e[i].message);
        js_free(ctx, s->e[i].keyword);
    }
    s->n = 0;
    s->done = 0;
}

static inline void dyn_sc_errs_free(JSContext* ctx, dyn_scerrs_t* s)
{
    dyn_sc_errs_reset(ctx, s);
    js_free(ctx, s->e);
    s->e = NULL;
    s->cap = 0;
}

int dyn_sc_errs_move(JSContext* ctx, dyn_scerrs_t* dst,
    dyn_scerrs_t* src);

int dyn_sc_type_ok(JSContext* ctx, int mask, JSValueConst inst);

void dyn_sc_type_str(int mask, char* buf, size_t cap);

int dyn_sc_check_top(dyn_vctx_t* v, dyn_scnode_t* n, JSValueConst inst);

#if defined(CONFIG_NATIVE_MODULE_URL)
#define DYN_FM_IDNA dyn_idna_hostname_ok
#else
#define DYN_FM_IDNA NULL
#endif

int dyn_fm_builtin_engine(JSContext* ctx, const char* fmt, const char* s, size_t n);

static inline int dyn_fm_builtin(JSContext* ctx, const char* fmt, const char* s, size_t n)
{
    int r = dyn_fm_check(fmt, s, n, DYN_FM_IDNA);
    return r != DYN_FM_UNKNOWN ? r : dyn_fm_builtin_engine(ctx, fmt, s, n);
}

int dyn_sc_read_options(JSContext* ctx, JSValueConst opts, dyn_sccomp_t* c, dyn_schema_t* sch);

JSValue dyn_sc_compile_entry(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_sc_validate_impl(JSContext* ctx, dyn_schema_t* sch,
    JSValueConst inst);

JSValue dyn_sc_schema_validate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
