#ifndef FILE_INTERNAL_H
#define FILE_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

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

#include "core/dyn-path.h"
#include "core/dyn-fsops.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

extern JSClassID dyn_path_class_id;

void dyn_path_finalizer(JSRuntime* rt, JSValue val);

int dyn_is_path(JSValueConst v);

const char* dyn_path_arg(JSContext* ctx, JSValueConst v, const char* what);

const char* dyn_path_arg_len(JSContext* ctx, JSValueConst v,
    const char* what, size_t* plen);

JSValue dyn_fs_throw(JSContext* ctx, int e, const char* op,
    const char* path);
JSValue __attribute__((format(printf, 2, 3))) dyn_fs_throw_type(JSContext* ctx, const char* fmt, ...);

const char* dyn_path_borrow(JSContext* ctx, JSValueConst v, const char* what,
    size_t* plen);

JSValue dyn_path_new_from(JSContext* ctx, const char* s, size_t n);

void dyn_path_unborrow(JSContext* ctx, const char* p);

JSValue dyn_path_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_path_get_str(JSContext* ctx, JSValueConst this_val);

JSValue dyn_path_get_dirname(JSContext* ctx, JSValueConst this_val);

JSValue dyn_path_get_basename(JSContext* ctx, JSValueConst this_val);

JSValue dyn_path_get_extname(JSContext* ctx, JSValueConst this_val);

JSValue dyn_path_get_is_absolute(JSContext* ctx, JSValueConst this_val);

JSValue dyn_path_compose(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_path_relative_to(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_path_equals(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_path_basename_without(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_path_to_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_path_static(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

#define DYN_FILE_DEFAULT_BUF (1u << 17)
#define DYN_FILE_MIN_BUF 4096u
#define DYN_FILE_MAX_BUF (1u << 26)

static inline unsigned dyn_file_clamp_bufsize(int64_t v)
{
    if (v <= 0)
        return DYN_FILE_DEFAULT_BUF;
    if (v < DYN_FILE_MIN_BUF)
        return DYN_FILE_MIN_BUF;
    if (v > DYN_FILE_MAX_BUF)
        return DYN_FILE_MAX_BUF;
    return (unsigned)v;
}

extern JSClassID dyn_freader_class_id;

typedef struct {
    unsigned char* buf;
    size_t cap;
    size_t start;
    size_t end;
    int fd;
    int eof;
} dyn_freader_t;

_Static_assert(sizeof(dyn_freader_t) == 4 * sizeof(void*) + 8,
    "dyn_freader_t regained padding: keep the two ints adjacent");

JSValue dyn_freader_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_freader_read(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_freader_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_freader_read_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_freader_read_line(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#include "dyna-aio.h"
#include "dyna-evloop.h"
#include "core/dyn-timer.h"

extern JSClassID dyn_fwriter_class_id;

typedef struct {
    unsigned char* buf;
    size_t cap;
    size_t len;
    int fd;
    int dirty;
} dyn_fwriter_t;

_Static_assert(sizeof(dyn_fwriter_t) == 3 * sizeof(void*) + 8,
    "dyn_fwriter_t regained padding: keep the two ints adjacent");

JSValue dyn_fwriter_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

int dyn_file_payload(JSContext* ctx, JSValueConst v, const uint8_t** pdata,
    size_t* plen, const char** powned);

JSValue dyn_fwriter_write(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fwriter_flush(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fwriter_sync(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

int dyn_file_slurp_path(const char* path, dyn_iobuf_t* src, int* perr);
int dyn_file_write_path(const char* path, const uint8_t* data, size_t len,
    int append, int atomic, int* stage);
JSValue dyn_file_bytes_take(JSContext* ctx, dyn_iobuf_t* src);

JSValue dyn_file_write_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#if defined(__APPLE__)
#define DYN_STAT_MTIM(st) ((st).st_mtimespec)
#define DYN_STAT_ATIM(st) ((st).st_atimespec)
#define DYN_STAT_CTIM(st) ((st).st_ctimespec)
#else
#define DYN_STAT_MTIM(st) ((st).st_mtim)
#define DYN_STAT_ATIM(st) ((st).st_atim)
#define DYN_STAT_CTIM(st) ((st).st_ctim)
#endif

#define DYN_DIRENT_INLINE 48

typedef struct {
    char* heap;
    char inl[DYN_DIRENT_INLINE];
    int is_dir, is_file, is_symlink;
} dyn_dirent_t;

enum {
    DYN_FS_STAT,
    DYN_FS_LSTAT,
    DYN_FS_EXISTS,
    DYN_FS_READDIR,
    DYN_FS_MKDIR,
    DYN_FS_REMOVE,
    DYN_FS_REMOVEALL,
    DYN_FS_RENAME,
    DYN_FS_COPY,
    DYN_FS_MOVE,
    DYN_FS_SYMLINK,
    DYN_FS_READLINK,
    DYN_FS_REALPATH,
    DYN_FS_CHMOD,
    DYN_FS_NOPS
};

enum {
    DYN_FS_EK_ERRNO,
    DYN_FS_EK_OOM,
    DYN_FS_EK_ISDIR,
    DYN_FS_EK_NOTREG,
    DYN_FS_EK_SAME
};

typedef struct dyn_fs_req {
    const char* path;
    const char* path2;
    const char* epath;
    int op;
    int err;
    int ekind;
    int flag;
    int borrowed;
    int32_t mode;
    union {
        struct stat st;
        struct {
            dyn_dirent_t* ents;
            size_t n;
        } dir;
        struct {
            char* s;
            size_t n;
        } str;
        int64_t num;
        int yes;
    } u;
} dyn_fs_req_t;

extern const char* const dyn_fs_op_names[DYN_FS_NOPS];
int dyn_fs_req_parse(JSContext* ctx, int op, int argc, JSValueConst* argv, dyn_fs_req_t* q);
void dyn_fs_req_unborrow(JSContext* ctx, dyn_fs_req_t* q);
void dyn_fs_req_run(dyn_fs_req_t* q);
JSValue dyn_fs_req_result(JSContext* ctx, dyn_fs_req_t* q);
void dyn_fs_req_clear(dyn_fs_req_t* q);
JSValue dyn_fs_req_sync(JSContext* ctx, int op, int argc, JSValueConst* argv);
void dyn_fs_copy_run(dyn_fs_req_t* q);
void dyn_fs_move_run(dyn_fs_req_t* q);

enum {
    DYN_FA_TEXT,
    DYN_FA_BYTES,
    DYN_FA_WRITE,
    DYN_FA_FS,
    DYN_FA_HANDLE
};

enum {
    DYN_FAH_OPEN,
    DYN_FAH_READ,
    DYN_FAH_READINTO,
    DYN_FAH_WRITE,
    DYN_FAH_SYNC,
    DYN_FAH_STAT,
    DYN_FAH_CLOSE,
    DYN_FAH_RTEXT,
    DYN_FAH_RBYTES,
    DYN_FAH_RINTO,
    DYN_FAH_RLINE,
    DYN_FAH_RNEXT,
    DYN_FAH_RRET,
    DYN_FAH_RCLOSE,
    DYN_FAH_WPUT,
    DYN_FAH_WFLUSH,
    DYN_FAH_WSYNC,
    DYN_FAH_WCLOSE
};

typedef struct dyn_fa_job dyn_fa_job_t;

typedef struct {
    uint8_t* p;
    size_t cap;
    size_t start;
    size_t end;
    int eof;
    int dirty;
} dyn_fabuf_t;

typedef struct {
    int fd;
    int closing;
    int append;
    dyn_fabuf_t* fb;
    dyn_fa_job_t* cur;
    dyn_fa_job_t* qhead;
    dyn_fa_job_t* qtail;
} dyn_fah_t;

typedef struct {
    dyn_fah_t* h;
    JSValue hv;
    JSValue into;
    dyn_fa_job_t* qnext;
    char* path;
    uint8_t* buf;
    size_t len;
    size_t got;
    int64_t pos;
    struct stat st;
    int kind;
    int fd;
    int own_fd;
    int err;
    int haspos;
    int flags;
    int32_t mode;
    dyn_fabuf_t* fb;
    dyn_fabuf_t* own_fb;
    uint8_t* out;
    size_t outlen;
    size_t outcap;
    size_t bsize;
    int64_t want;
    int isnull;
    int bmode;
} dyn_fah_op_t;

struct dyn_fa_job {
    struct dyn_fa_job* prev;
    struct dyn_fa_job* next;
    JSContext* ctx;
    JSRuntime* rt;
    JSValue resolve;
    JSValue reject;
    const char* name;
    int op;
    int dead;
    int held;
    int linked;
    dyn_aio_t* aio;
    union {
        struct {
            char* path;
            uint8_t* data;
            size_t len;
            dyn_iobuf_t out;
            uint8_t* fixed;
            size_t fixed_len;
            int append;
            int atomic;
            int err;
            int stage;
            int rc;
        } io;
        dyn_fs_req_t fs;
        dyn_fah_op_t h;
    } u;
};

extern JSClassID dyn_fah_class_id;
JSValue dyn_fa_arm(JSContext* ctx, dyn_fa_job_t* j, int* armed);
int dyn_fa_offload(dyn_fa_job_t* j);
void dyn_fa_refuse(JSContext* ctx, dyn_fa_job_t* j);
void dyn_fah_run(dyn_fah_op_t* o);
JSValue dyn_fah_result(JSContext* ctx, dyn_fa_job_t* j, int* bad);
void dyn_fah_after(JSContext* ctx, dyn_fa_job_t* j);
void dyn_fah_drop(JSContext* ctx, dyn_fa_job_t* j);
void dyn_fah_kill(JSRuntime* rt, dyn_fa_job_t* j);
void dyn_fab_free(dyn_fabuf_t* b, int fd);
int dyn_fah_init(JSContext* ctx, JSValueConst o);
JSValue dyn_fs_stat_obj(JSContext* ctx, const struct stat* st);

JSValue dyn_fs_stat(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_lstat(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_exists(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_read_dir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_make_dir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_remove(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_remove_all(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_fs_rename(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_symlink(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_read_link(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_fs_real_path(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_fs_chmod(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_glob(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_temp_dir(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_make_temp_dir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_fs_make_temp_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#if defined(__APPLE__)
#include <copyfile.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif

JSValue dyn_fs_copy_file(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_fs_move(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fs_sniff_type(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#include <dirent.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/inotify.h>
#endif

#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif

extern JSClassID dyn_watch_class_id;

extern const JSClassDef dyn_watch_class;

JSValue dyn_watch_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dyn_watch_proto[6];

JSValue dyn_pdir_get(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

extern JSClassID dyn_flock_class_id;

JSValue dyn_flock_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_flock_with_lock(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    JSValue path;
} dyn_fh_t;

extern JSClassID dyn_fh_class_id;

void dyn_fh_finalizer(JSRuntime* rt, JSValue val);

void dyn_fh_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

JSValue dyn_fh_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

dyn_fh_t* dyn_fh_of(JSContext* ctx, JSValueConst v);

JSValue dyn_fh_append(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fh_move_to(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fh_copy_to(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_fh_stream(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_fh_get_path(JSContext* ctx, JSValueConst this_val);

JSValue dyn_fh_to_string(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

extern JSClassID dyn_gl_class_id;

void dyn_gl_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_gl_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_gl_matches(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_gl_expand(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_gl_filter(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_gl_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue dyn_gl_static_match(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
