#ifndef DYN_FSOPS_FSOPS_H
#define DYN_FSOPS_FSOPS_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <sys/file.h>
#endif

#if defined(__APPLE__)
#include <copyfile.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif

typedef struct {
    char** items;
    size_t count, cap;
    int oom;
} dyn_glob_res;

#define DYN_GLOB_MAX_ALTS 4096

typedef struct {
    char** v;
    size_t n, cap;
} dyn_glob_alts;

typedef struct {
    int fd;
    int excl;
    char* lockfile;
} dyn_flock_t;

static inline int dyn_is_dot_or_dotdot(const char* name)
{
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static inline int dyn_glob_has_wildcard(const char* s)
{
    return strpbrk(s, "*?[") != NULL;
}

int dyn_fs_path_open(const char* path, int flags, mode_t mode);
char* dyn_join(const char* a, const char* b);
const char* dyn_fs_errno_code(int e);
int dyn_fs_mkdirp(const char* path, mode_t mode);
const char* dyn_fs_tempdir_str(void);
int dyn_fs_temp_prefix_ok(const char* prefix, size_t len);
char* dyn_fs_temp_template(const char* prefix);
int dyn_fs_remove_tree(const char* path, int* perr, const char** pwhat);
int dyn_fs_write_atomic(const char* path, const uint8_t* data, size_t len);

int dyn_glob_match(const char* p, const char* s);
void dyn_glob_res_free(dyn_glob_res* r);
void dyn_glob_walk(dyn_glob_res* res, const char* base, char** segs,
    int nseg, int si, const char* rel, int is_abs,
    int depth);
int dyn_glob_str_cmp(const void* a, const void* b);
int dyn_glob_expand(const char* pat, dyn_glob_alts* out);
void dyn_glob_alts_free(dyn_glob_alts* a);
int dyn_glob_match_alts(const dyn_glob_alts* a, const char* s);

void dyn_flock_sleep_ms(int64_t ms);
int dyn_flock_try(dyn_flock_t* l, const char* path);
void dyn_flock_release(dyn_flock_t* l);

int dyn_copy_fds(int in, int out);
const char* dyn_sniff_bytes(const uint8_t* p, size_t n);

#endif
