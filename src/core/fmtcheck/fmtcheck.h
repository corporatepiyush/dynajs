#ifndef DYN_FMTCHECK_FMTCHECK_H
#define DYN_FMTCHECK_FMTCHECK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define DYN_FM_UNKNOWN (-2)

typedef int (*dyn_fm_idna_fn)(const char* s, size_t n);

typedef struct {
    const char *scheme, *auth, *path, *query, *frag;
    size_t sl, al, pl, ql, fl;
    int has_scheme, has_auth, has_query, has_frag;
} dyn_uri_t;

int dyn_fm_check(const char* fmt, const char* s, size_t n, dyn_fm_idna_fn idna);
void dyn_uri_parse(const char* u, dyn_uri_t* o);
size_t dyn_uri_remove_dots(char* in, size_t n);

#endif
