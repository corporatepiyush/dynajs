#ifndef HTTP_SCAN_H
#define HTTP_SCAN_H

#include <stddef.h>
#include <stdint.h>

#define DYN_APP_MAX_HEADER_COUNT 256

int dyn_ci_eq(const char* a, const char* b, size_t n);

typedef enum {
    DYN_HEAD_OK = 0,
    DYN_HEAD_ERR_LINE = 1,
    DYN_HEAD_ERR_DUP = 2,
} dyn_head_err;

typedef struct {
    const char* method;
    size_t method_len;
    const char* target;
    size_t target_len;
    size_t head_len;
    size_t n_lines;
    const char* cl;
    size_t cl_len;
    const char *ae, *conn, *ct;
    size_t ae_len, conn_len, ct_len;
    int cl_count;
    int has_te;
    int host_count;
    int cl_set;
    int dup;
    int http11;
    int bad_target;
} dyn_reqinfo_t;

int dyn_scan_head(const char* buf, size_t avail, dyn_reqinfo_t* ri);

#endif
