#ifndef DYN_TEXTDIFF_TEXTDIFF_H
#define DYN_TEXTDIFF_TEXTDIFF_H

#include <stddef.h>
#include <stdint.h>


enum { DYN_TD_LINES,
    DYN_TD_WORDS,
    DYN_TD_CHARS };

enum { DYN_TD_OK = 0,
    DYN_TD_ENOMEM = -1,
    DYN_TD_ETOKENS = -2,
    DYN_TD_ECOSTLY = -3,
    DYN_TD_ESYNTAX = -4,
    DYN_TD_ECONFLICT = -5 };

#define DYN_TD_MSG_CAP 256

typedef struct {
    uint8_t* buf;
    size_t size, cap;
    int error;
} dyn_td_buf_t;

#define DYN_TD_MAX_TOKENS (4u * 1024u * 1024u)

typedef struct {
    uint32_t *off, *len, *id;
    uint32_t n;
} dyn_td_toks_t;

enum { DYN_TD_DEL = -1,
    DYN_TD_EQ = 0,
    DYN_TD_INS = 1 };

typedef struct {
    int8_t op;
    uint32_t a0, a1, b0, b1;
} dyn_td_hunk_t;

typedef struct {
    dyn_td_hunk_t* v;
    uint32_t n, cap;
} dyn_td_script_t;

typedef struct {
    dyn_td_toks_t ta, tb;
    dyn_td_script_t sc;
} dyn_td_res_t;

int dyn_td_diff(const uint8_t* a, uint32_t na, const uint8_t* b, uint32_t nb,
    int mode, dyn_td_res_t* r);
int dyn_td_diff_ex(const uint8_t* a, uint32_t na, const uint8_t* b, uint32_t nb,
    int mode, uint64_t ceil, dyn_td_res_t* r);
void dyn_td_res_free(dyn_td_res_t* r);
void dyn_td_buf_free(dyn_td_buf_t* b);
int dyn_td_unified(const uint8_t* a, const uint8_t* b, const dyn_td_res_t* res,
    uint32_t ctxl, const char* an, const char* bn, dyn_td_buf_t* o);
int dyn_td_apply(const uint8_t* ts, uint32_t tn, const uint8_t* p, size_t pn,
    dyn_td_buf_t* o, char* msg);

#endif
