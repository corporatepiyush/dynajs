#ifndef DYN_PATH_H
#define DYN_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_PATH_SEP '/'
#define DYN_PATH_DELIM ':'

static inline size_t dyn_path_normalize_cap(size_t n) { return n + 2; }

static inline size_t dyn_path_join_cap(size_t sum, size_t count)
{
    return sum + (count ? count - 1 : 0) + 2;
}

static inline size_t dyn_path_resolve_cap(size_t sum, size_t count)
{
    return dyn_path_join_cap(sum, count) + 1;
}

static inline size_t dyn_path_relative_cap(size_t from_n, size_t to_n)
{
    return 2 * (from_n + to_n) + 16;
}

static inline size_t dyn_path_cstr_cap(size_t n) { return n + 1; }

size_t dyn_path_normalize(const char* p, size_t n, char* out);

size_t dyn_path_join(const char* const* parts, const size_t* lens, size_t count,
    char* out, char* scratch);

size_t dyn_path_resolve(const char* const* parts, const size_t* lens,
    size_t count, char* out, char* scratch);

size_t dyn_path_relative(const char* from, size_t from_n, const char* to,
    size_t to_n, char* out, char* scratch);

typedef struct {
    size_t dir_off, dir_len;
    size_t base_off, base_len;
    size_t ext_off, ext_len;
    int dir_is_dot;
    int dir_is_root;
    int is_absolute;
} dyn_path_split_t;

void dyn_path_split(const char* p, size_t n, dyn_path_split_t* s);

void dyn_path_basename(const char* p, size_t n, const char* suffix,
    size_t suffix_n, size_t* off, size_t* len);

static inline int dyn_path_is_absolute(const char* p, size_t n)
{
    return n > 0 && p[0] == DYN_PATH_SEP;
}

#ifdef __cplusplus
}
#endif

#endif
