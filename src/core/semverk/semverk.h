#ifndef DYN_SEMVERK_SEMVERK_H
#define DYN_SEMVERK_SEMVERK_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SV_MAX_SAFE 9007199254740991ULL
#define SEMVER_MAX_IDS 24
#define SV_MAX_SETS 16
#define SV_MAX_CMPS 24

typedef struct {
    const char* s;
    size_t len;
    int numeric;
} SemVerId;

typedef struct {
    uint64_t major, minor, patch;
    int n_pre;
    SemVerId pre[SEMVER_MAX_IDS];
    int n_build;
    SemVerId build[SEMVER_MAX_IDS];
} SemVer;

static inline int sv_is_digit(char c) { return c >= '0' && c <= '9'; }

size_t sv_ws_lead(const char* s, size_t n);

size_t sv_ws_trail(const char* s, size_t n);

int semver_parse(const char* s, size_t n, SemVer* out);

int sv_cmp_num(const char* a, size_t al, const char* b, size_t bl);

int semver_compare(const SemVer* a, const SemVer* b);

enum { OP_LT,
    OP_LTE,
    OP_GT,
    OP_GTE,
    OP_EQ,
    OP_TILDE,
    OP_CARET };

typedef struct {
    int op;
    SemVer v;
} Comparator;

typedef struct {
    Comparator* c;
    int n, cap;
} CompSet;

typedef struct {
    CompSet s[SV_MAX_SETS];
    int n;
} Range;

int sv_add_cmp(CompSet* set, int op, uint64_t M, uint64_t m, uint64_t p,
    const SemVerId* pre, int npre, int add_zero);

int semverk_parse_range(const char* s, size_t n, Range* out);

void sv_range_release(Range* r);

void sv_range_free(Range* r);

int semverk_cmp_test(const SemVer* v, const Comparator* c);

int semverk_range_test(const SemVer* v, const Range* rg);

int sv_cmp_pre(const SemVerId* a, int an, const SemVerId* b, int bn);

int sv_range_intersects(const Range* ra, const Range* rb);

void semverk_sort_indices(const SemVer* svs, uint32_t* idx, uint32_t* tmp, uint32_t n);

typedef struct {
    int is_num;
    uint64_t num;
    const char* str;
    size_t slen;
} IncId;

typedef struct {
    uint64_t major, minor, patch;
    int n_pre;
    IncId pre[SEMVER_MAX_IDS + 2];
} IncVer;

enum { REL_MAJOR,
    REL_MINOR,
    REL_PATCH,
    REL_PREMAJOR,
    REL_PREMINOR,
    REL_PREPATCH,
    REL_PRERELEASE,
    REL_RELEASE };



#endif
