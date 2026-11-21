#ifndef LOG_INTERNAL_H
#define LOG_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-simdk.h"
#include "core/dyn-logk.h"
#include "dtoa.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>

extern pthread_mutex_t dyn_log_mu;

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

enum { DYN_LV_TRACE,
    DYN_LV_DEBUG,
    DYN_LV_INFO,
    DYN_LV_WARN,
    DYN_LV_ERROR,
    DYN_LV_FATAL,
    DYN_LV_SILENT };

extern const char* const DYN_LV_NAME[7];

enum { DYN_TS_EPOCH,
    DYN_TS_ISO,
    DYN_TS_NONE };

enum { DYN_FMT_JSON,
    DYN_FMT_TEXT };

typedef struct dyn_log_sink dyn_log_sink_t;

typedef struct {
    int level;
    int ts_mode;
    int fmt;
    char* name;
    char* base;
    size_t base_len;
    char* base_text;
    size_t base_text_len;
    dyn_log_sink_t* sink;
    JSValue dest_fn;
    int64_t emit_count;
    int64_t sample_n;
    int pid_on, host_on;
    int64_t cached_sec;
    char cached_iso[20];
} dyn_logger_t;

extern JSClassID dyn_logger_class_id;

void dyn_sink_unref(dyn_log_sink_t* s);

void dyn_logger_finalizer(JSRuntime* rt, JSValue val);

void dyn_logger_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

static inline const char* dyn_str_bytes(JSContext* ctx, const char** ps, size_t* plen,
    JSValueConst v)
{
    return *ps = JS_ToCStringLen(ctx, plen, v);
}

void dyn_line_i64(dyn_line_t* b, int64_t v);

static inline int64_t log_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

const char* dyn_iso_sec(dyn_logger_t* L, int64_t ms, int* millis_out);

void dyn_line_time(dyn_line_t* b, dyn_logger_t* L, int64_t ms);

#define DYN_LOG_MAX_DEPTH 8

typedef struct {
    const void* p[DYN_LOG_MAX_DEPTH];
    int n;
} dyn_seen_t;

typedef struct {
    JSPropertyEnum* tab;
    uint32_t len;
    int heap;
    JSPropertyEnum stackbuf[32];
} dyn_props_t;

int dyn_props_get(JSContext* ctx, JSValueConst v, dyn_props_t* p);

static inline void dyn_props_free(JSContext* ctx, dyn_props_t* p);

static inline const char* dyn_key_bytes(JSContext* ctx, size_t* plen, JSAtom atom,
    int* owned);

void dyn_line_value(JSContext* ctx, dyn_line_t* b, JSValueConst v,
    int depth, dyn_seen_t* seen);

void dyn_line_number(JSContext* ctx, dyn_line_t* b, JSValueConst v);

int dyn_key_reserved(dyn_logger_t* L, const char* k, size_t n,
    int reserve_err);

static inline void dyn_props_free(JSContext* ctx, dyn_props_t* p)
{
    uint32_t i;
    if (p->heap == 2) {
        JS_FreePropertyEnum(ctx, p->tab, p->len);
        return;
    }
    for (i = 0; i < p->len; i++)
        JS_FreeAtom(ctx, p->tab[i].atom);
    if (p->heap == 1)
        free(p->tab);
}

static inline const char* dyn_key_bytes(JSContext* ctx, size_t* plen, JSAtom atom,
    int* owned)
{
    const char* ks = JS_AtomBorrowASCII(ctx, plen, atom);
    *owned = 0;
    if (ks)
        return ks;
    ks = JS_AtomToCStringLen(ctx, plen, atom);
    *owned = ks != NULL;
    return ks;
}

int dyn_line_fields(JSContext* ctx, dyn_line_t* b, dyn_logger_t* L,
    JSValueConst v, int reserve_err);

int dyn_line_error(JSContext* ctx, dyn_line_t* b, JSValueConst v);

struct dyn_log_sink {
    struct dyn_log_sink* next;
    int refs;
    int fd;
    char* path;
    uint64_t max_size;
    int64_t freq_ms;
    int named_date;
    int numbered;
    int keep_count;
    int use_symlink;
    int64_t period_ms;
    char* base;
    char* ext;
    char* active;
    uint64_t written;
    char* buf;
    size_t bufn, bufcap;
    int64_t last_retry;
    uint64_t write_errors;
};

int dyn_write_all(int fd, const char* p, size_t n);

void dyn_sink_flush(dyn_log_sink_t* s);

void dyn_sink_write(dyn_log_sink_t* s, int64_t now,
    const char* data, size_t n);

int dyn_parse_size(JSContext* ctx, JSValueConst v, uint64_t* out);

int dyn_parse_frequency(JSContext* ctx, JSValueConst v, int64_t* out,
    int* named_date);

dyn_log_sink_t* dyn_sink_new(JSContext* ctx, const char* path,
    uint64_t max_size, int64_t freq_ms,
    int named_date, int keep_count,
    int use_symlink, size_t bufcap, int do_mkdir);

void dyn_text_value(JSContext* ctx, dyn_line_t* b, JSValueConst v);

dyn_logger_t* dyn_logger_of(JSContext* ctx, JSValueConst v);

JSValue dyn_log_emit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_logger_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_logger_child(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_logger_enabled(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_logger_get_level(JSContext* ctx, JSValueConst this_val);

JSValue dyn_logger_set_level(JSContext* ctx, JSValueConst this_val,
    JSValueConst val);

JSValue dyn_logger_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_debug(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
