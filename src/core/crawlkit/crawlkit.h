#ifndef DYN_CRAWLKIT_CRAWLKIT_H
#define DYN_CRAWLKIT_CRAWLKIT_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../dyn-common.h"
#include "../dyn-simdk.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

typedef struct {
    char* path;
    size_t len;
    size_t grp;
    unsigned allow : 1;
} rb_rule_t;

typedef struct {
    rb_rule_t* rules;
    size_t n, cap;
    uint32_t* order;
    uint32_t bstart[258];
    double delay;
    char** sitemaps;
    size_t n_site, cap_site;
} rb_t;

typedef double (*rb_atod_fn)(const char* buf, const char** end);

void rb_free(rb_t* r);

size_t rb_token_len(const char* s, size_t n);

int rb_parse(rb_t* r, const char* s, size_t n, const char* agent, rb_atod_fn atod);

int rb_allows_path(const rb_t* r, const char* p, size_t pn);

void fe_dirs_add(char** out, size_t* len, size_t* cap,
    const char* tok, size_t tl);

char* fe_parse_robots_tag(const char* joined, const char* agent);

int fe_ip4_private(const unsigned char b[4]);

int fe_ip6_private(const unsigned char* b);

int fe_host_is_private(const char* host);

int fe_scheme_is_https(const char* url);

int fe_split(const char* url, char* host, size_t cap, const char** path);

long fe_resolve(const char* base, const char* ref, char* out,
    size_t cap);

#define FE_SM_MAX_URLS 50000
#define FE_SM_MAX_LOC_TEXT 2048

enum {
    FE_SM_OK,
    FE_SM_E_COMMENT,
    FE_SM_E_PI,
    FE_SM_E_CDATA,
    FE_SM_E_DECL,
    FE_SM_E_TAG,
    FE_SM_E_LOC_TAG,
    FE_SM_E_LOC_OPEN,
    FE_SM_E_LOC_CLOSE,
    FE_SM_E_TOO_MANY,
    FE_SM_E_OOM
};

typedef struct {
    char** v;
    size_t n, cap;
    int is_index;
} fe_sm_doc_t;

void fe_sm_doc_free(fe_sm_doc_t* d);

int fe_sm_scan(const char* p, size_t n, fe_sm_doc_t* out, size_t* eoff);

int fe_sm_format_error(int code, size_t off, char* buf, size_t cap);

#endif
