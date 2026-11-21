#ifndef DYN_DOCDB_DD_AGG_H
#define DYN_DOCDB_DD_AGG_H

#include "dd.h"

typedef struct dd_xn dd_xn;

typedef struct {
    uint8_t type;
    uint8_t inbuf;
    uint32_t n;
    uint32_t off;
    const uint8_t* p;
} dd_xv;

typedef struct {
    const char* name;
    size_t nlen;
    dd_xv v;
} dd_xvar;

#define DD_XVARS 16

typedef struct {
    const uint8_t* root;
    dyn_bson_w buf;
    dd_xvar vars[DD_XVARS];
    int nvars;
    int64_t now;
    dd_err* e;
} dd_xctx;

int dd_expr_compile(dd_xn** out, dyn_bson_val spec, dd_err* e);
void dd_expr_free(dd_xn* x);
int dd_expr_eval(const dd_xn* x, dd_xctx* c, dd_xv* out);
int dd_expr_is_field(const dd_xn* x, const dd_path** p);
int dd_expr_lit(const dd_xn* x, dyn_bson_val* v);
enum { DD_XA_ADD = 1,
    DD_XA_SUB,
    DD_XA_MUL };
int dd_expr_arith(const dd_xn* x, int* op, const dd_xn* const** args, uint32_t* n);
void dd_xctx_init(dd_xctx* c, int64_t now, dd_err* e);
void dd_xctx_reset(dd_xctx* c, const uint8_t* root);
void dd_xctx_free(dd_xctx* c);
dyn_bson_val dd_xv_val(const dd_xctx* c, const dd_xv* v);
int dd_xv_truthy(const dd_xctx* c, const dd_xv* v);
int dd_xv_put(dd_xctx* c, uint8_t type, const void* p, uint32_t n, dd_xv* out);

#endif
