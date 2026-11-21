#ifndef DYN_LOGK_LOGK_H
#define DYN_LOGK_LOGK_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DYN_LINE_INLINE 512

typedef struct {
    char* p;
    size_t n, cap;
    int truncated;
    char inln[DYN_LINE_INLINE];
} dyn_line_t;

void dyn_line_init(dyn_line_t* b);

void dyn_line_free(dyn_line_t* b);

#define DYN_LOG_MAX_LINE (64u * 1024u)
#define DYN_LOG_TAIL_RESERVE 16

void dyn_line_put_slow(dyn_line_t* b, const char* s, size_t n);

static inline void dyn_line_put(dyn_line_t* b, const char* s, size_t n)
{
    size_t e = b->n + n;
    if (e <= b->cap && e <= DYN_LOG_MAX_LINE - DYN_LOG_TAIL_RESERVE && !b->truncated) {
        memcpy(b->p + b->n, s, n);
        b->n = e;
        return;
    }
    dyn_line_put_slow(b, s, n);
}

static inline void dyn_line_puts(dyn_line_t* b, const char* s)
{
    dyn_line_put(b, s, strlen(s));
}

void dyn_line_force(dyn_line_t* b, const char* s, size_t n);

extern const uint8_t DYN_LOG_ESC_BM[32];

void dyn_line_json_str(dyn_line_t* b, const char* s, size_t n);

void dyn_line_esc_ctrl(dyn_line_t* b, const char* s, size_t n);

void dyn_line_json_repair(dyn_line_t* b);

int dyn_debug_match(const char* env, const char* ns);

#endif
