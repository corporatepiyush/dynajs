#ifndef DYN_IDNA_IDNA_H
#define DYN_IDNA_IDNA_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

enum {
    IDNA_ST_VALID = 0,
    IDNA_ST_IGNORED = 1,
    IDNA_ST_DEVIATION = 2,
    IDNA_ST_DISALLOWED = 3,
    IDNA_ST_MAPPED = 4,
};

typedef struct {
    uint32_t* p;
    size_t n, cap;
    int oom;
} dyn_cp_t;

typedef struct {
    size_t off, len;
    int decoded;
} dyn_idna_lab_t;

typedef struct {
    dyn_idna_lab_t* a;
    size_t n, cap;
    int oom;
} dyn_idna_labs_t;

typedef struct {
    int trans;
    dyn_cp_t cps;
    dyn_idna_labs_t labs;
    int nerrors;
    const char* stage;
    int oom;
} dyn_idna_proc_t;

static inline void dyn_cp_init(dyn_cp_t* b)
{
    b->p = NULL;
    b->n = 0;
    b->cap = 0;
    b->oom = 0;
}

static inline void dyn_cp_free(dyn_cp_t* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static inline int dyn_cp_put(dyn_cp_t* b, uint32_t c)
{
    if (b->oom)
        return -1;
    if (b->n == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        uint32_t* np = (uint32_t*)realloc(b->p, nc * sizeof(uint32_t));
        if (!np) {
            b->oom = 1;
            return -1;
        }
        b->p = np;
        b->cap = nc;
    }
    b->p[b->n++] = c;
    return 0;
}

int dyn_idna_map_cp(uint32_t cp, int transitional, int drop_disallowed,
    dyn_cp_t* out);

int pc_encode(const uint32_t* cps, int n, dyn_cp_t* out);
int pc_decode(const char* s, size_t n, dyn_cp_t* out);

int idna_nfc(const uint32_t* src, int n, uint32_t** pout);
int idna_is_nfc(const uint32_t* cps, int n);
void dyn_idna_labs_free(dyn_idna_labs_t* l);
dyn_idna_proc_t dyn_idna_process(const uint32_t* src, int n, int transitional,
    int mode, int strict);
void dyn_idna_to_ascii(dyn_idna_proc_t* p, const uint32_t* src, int n,
    int transitional, int strict, dyn_cp_t* out_ascii);
int dyn_idna_ascii_fast_check(const char* s, size_t len);
int dyn_idna_hostname_ok(const char* s, size_t n);
int dyn_idna_utf8_next(const uint8_t* p, int max_len, const uint8_t** pp);

#endif
