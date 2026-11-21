#ifndef DS_INVERTED_INDEX_H
#define DS_INVERTED_INDEX_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_INVIDX_TOK_MAX 128
#define DYN_INVIDX_MAX_GROUPS 32

typedef struct {
    uint32_t doc;
    uint32_t tf;
} dyn_invidx_post;

typedef struct {
    char* s;
    uint32_t len;
    uint32_t n, cap;
    dyn_invidx_post* p;
} dyn_invidx_term;

typedef struct {
    uint32_t term;
    uint32_t tf;
} dyn_invidx_dt;

typedef struct {
    uint32_t id;
    uint32_t len;
    uint32_t nt;
    uint32_t alive;
    dyn_invidx_dt* t;
} dyn_invidx_doc;

typedef struct {
    dyn_invidx_term* terms;
    uint32_t nterms, tcap;
    dyn_ds_smap tidx;
    dyn_invidx_doc* docs;
    uint32_t nslots, dcap;
    uint32_t* freel;
    uint32_t nfree, fcap;
    dyn_ds_imap didx;
    uint32_t live;
    uint64_t total_len;
    double k1, b;
    dyn_ds_smap stop;
    char** stopw;
    uint32_t nstop;
    double* acc;
    uint32_t* stamp;
    uint32_t* mask;
    uint32_t* touched;
    uint32_t scap, epoch;
    uint32_t* tstamp;
    uint32_t* tcount;
    uint32_t tscap, tepoch;
} dyn_invidx_t;

typedef struct {
    dyn_invidx_t* x;
    dyn_invidx_dt* list;
    uint32_t n, cap;
    uint32_t len;
    int oom;
} dyn_invidx_add_ctx;

typedef struct {
    dyn_invidx_t* x;
    char toks[DYN_INVIDX_MAX_GROUPS][DYN_INVIDX_TOK_MAX];
    uint32_t lens[DYN_INVIDX_MAX_GROUPS];
    uint32_t n;
} dyn_invidx_q_ctx;

typedef struct {
    double s;
    uint32_t id;
} dyn_invidx_hit;

int dyn_invidx_tokenize(const char* str, size_t n, ii_tok_fn fn, void* ud);

int64_t dyn_invidx_term_get(dyn_invidx_t* x, const char* t, size_t n, int create);

int dyn_invidx_tscratch(dyn_invidx_t* x);

void dyn_invidx_bump_tepoch(dyn_invidx_t* x);

int dyn_invidx_remove(dyn_invidx_t* x, uint32_t id);

int dyn_invidx_install(dyn_invidx_t* x, uint32_t id, dyn_invidx_dt* list, uint32_t nt, uint32_t len);

int dyn_invidx_add(dyn_invidx_t* x, uint32_t id, const char* text, size_t n);

void dyn_invidx_free(dyn_invidx_t* x);

int dyn_invidx_add_stop(dyn_invidx_t* x, const char* t, size_t n);

int dyn_invidx_stop_tok(void* ud, const char* t, size_t n);

int dyn_invidx_search(dyn_invidx_t* x, const char* q, size_t qn, int and_mode, int prefix, uint32_t limit, dyn_invidx_hit** out, uint32_t* nout);

void dyn_invidx_init(dyn_invidx_t* x, double k1, double b);

#ifdef __cplusplus
}
#endif

#endif
