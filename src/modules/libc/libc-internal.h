#ifndef LIBC_INTERNAL_H
#define LIBC_INTERNAL_H

#include <stdlib.h>
#include "core/dyn-common.h"
#include "core/dyn-thread-name.h"
#include "core/dyn-stdio.h"
#ifdef CONFIG_NATIVE_MODULES
#include "dyna-evloop.h"
#endif
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>

#if defined(_WIN32)
#include <windows.h>
#include <conio.h>
#include <utime.h>
#else
#include <dlfcn.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <poll.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#define JS_WATCH_KQUEUE
#include <sys/event.h>
#elif defined(__linux__) && !defined(__COSMOPOLITAN__)
#define JS_WATCH_EPOLL
#include <sys/epoll.h>
#endif

#if defined(__FreeBSD__)
extern char** environ;
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
typedef sig_t sighandler_t;
#endif

#if defined(__APPLE__)
#if !defined(environ)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#endif
#endif

#endif

#define USE_WORKER

#ifdef USE_WORKER
#include <pthread.h>
#include <stdatomic.h>
#endif

#include "cutils.h"
#include "list.h"
#include "dyna-libc.h"
#include "core/dyn-simdk.h"
#include "dyna-io.h"

#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

typedef struct {
    struct list_head link;
    int fd;
    int poll_fd_index;
    JSValue rw_func[2];
    int watch_mask;
    int watch_re;
    uint32_t watch_id;
    uint32_t watch_seen;
} JSOSRWHandler;

typedef struct {
    struct list_head link;
    int sig_num;
    JSValue func;
} JSOSSignalHandler;

typedef struct JSOSTimer {
    struct JSOSTimer* id_next;
    struct JSOSTimer* lnext;
    struct JSOSTimer* lprev;
    struct JSOSTimerList* list;
    uint64_t seq;
    int timer_id;
    int repeat;
    int64_t delay;
    int64_t timeout;
    JSValue func;
    int argc;
    JSValue* args;
} JSOSTimer;

typedef struct JSOSTimerList {
    struct JSOSTimerList* map_next;
    JSOSTimer* head;
    JSOSTimer* tail;
    int64_t off;
    int64_t key_timeout;
    uint64_t key_seq;
    uint32_t heap_idx;
} JSOSTimerList;

typedef struct {
    struct list_head link;
    uint8_t* data;
    size_t data_len;
    uint8_t** sab_tab;
    size_t sab_tab_len;
} JSWorkerMessage;

typedef struct JSWaker {
#ifdef _WIN32
    HANDLE handle;
#else
    int read_fd;
    int write_fd;
#endif
} JSWaker;

typedef struct {
    int ref_count;
#ifdef USE_WORKER
    pthread_mutex_t mutex;
#endif
    struct list_head msg_queue;
    size_t queued_bytes;
    JSWaker waker;
} JSWorkerMessagePipe;

typedef struct {
    struct list_head link;
    JSWorkerMessagePipe* recv_pipe;
    JSValue on_message_func;
    int poll_fd_index;
} JSWorkerMessageHandler;

typedef struct {
    struct list_head link;
    JSValue obj;
} JSWorkerObjRef;

typedef struct {
    struct list_head link;
    JSValue promise;
    JSValue reason;
} JSRejectedPromiseEntry;

extern uint64_t js_std_exec_deadline_ms;

void js_std_apply_budgets(JSRuntime* rt);

void js_std_arm_exec_deadline(uint64_t timeout_ms);

int js_std_interrupt_handler(JSRuntime* rt, void* opaque);

int js_std_deadline_remaining_ms(void);

typedef struct JSThreadState {
    struct list_head os_rw_handlers;
    struct list_head os_signal_handlers;
    struct JSOSTimerList** timer_heap;
    struct JSOSTimer** timer_ids;
    struct JSOSTimerList** timer_lists;
    struct JSOSTimerList* timer_list_last;
    uint64_t timer_seq;
    uint32_t timer_heap_cap;
    uint32_t timer_ids_cap;
    uint32_t n_timer_lists;
    uint32_t timer_lists_cap;
    uint32_t n_idle_timer_lists;
    uint32_t n_mapped_timer_lists;
    int n_timers;
    struct list_head port_list;
    struct list_head worker_obj_list;
    struct list_head rejected_promise_list;
    int eval_script_recurse;
    int next_timer_id;
    JSWorkerMessagePipe *recv_pipe, *send_pipe;
#if !defined(_WIN32)
    struct pollfd* poll_fds;
    int poll_fds_size;
    int watch_fd;
    int watch_failed;
    int deadline_kq;
    uint32_t watch_serial;
    uint32_t watch_next_id;
    int64_t watch_swept_ms;
#endif
    int io_reactor_fd;
    int io_reactor_weak;
    void (*io_reactor_drain)(void* udata);
    int (*io_reactor_wait)(void* udata, int timeout_ms);
    int (*io_reactor_wait_us)(void* udata, int64_t timeout_us);
    int (*io_reactor_watch)(void* udata, int fd, void (*ready)(void* arg), void* arg);
    int (*io_reactor_unwatch)(void* udata, int fd);
    int io_reactor_watched_fd;
    int poster_ready;
    int64_t last_io_poll_us;
    int64_t clock_us;
    void* io_reactor_udata;
    struct JSLoopPoster* poster;
    int post_holds;
    void (*shutdown_hook)(JSRuntime* rt);
    void (*io_reactor_detach)(JSRuntime* rt);
    uint32_t rw_gen;
    uint32_t port_gen;
    uint64_t stat_waits;
    uint64_t stat_timers;
    uint64_t stat_fd_events;
    uint64_t stat_messages;
    uint64_t stat_posts;
    uint64_t stat_signals;
    JSValue job_error;
    int job_error_set;
    JSValue streams_ns;
    int streams_ns_set;
    uint32_t streams_own;
    void* wst;
    void* blob;
} JSThreadState;

#define JS_WATCH_RW 3

extern _Atomic(int) js_main_wake_wfd;

int js_loop_poster_ensure(JSThreadState* ts);

void js_main_loop_wake(void);

JSOSRWHandler* find_rh(JSThreadState* ts, int fd);

void free_rw_handler(JSRuntime* rt, JSOSRWHandler* rh);

extern _Atomic(int) js_live_worker_threads;

extern _Atomic(uint64_t) os_pending_signals;

typedef int (*JSOSPollFunc)(JSContext* ctx);

extern _Atomic(JSOSPollFunc) os_poll_func;

void* js_std_dbuf_realloc(void* opaque, void* ptr, size_t size);

static inline void js_std_dbuf_init(JSContext* ctx, DynBuf* s)
{
    dbuf_init2(s, JS_GetRuntime(ctx), js_std_dbuf_realloc);
}

JSValue js_printf_internal(JSContext* ctx,
    int argc, JSValueConst* argv, FILE* fp);

JSValue js_loadScript(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef JSModuleDef*(JSInitModuleFunc)(JSContext * ctx,
    const char* module_name);

int js_module_check_attributes(JSContext* ctx, void* opaque,
    JSValueConst attributes);

JSModuleDef* js_module_loader(JSContext* ctx,
    const char* module_name, void* opaque,
    JSValueConst attributes);

#if defined(_WIN32)
void setenv(const char* name, const char* value, int overwrite);
#endif

JSValue js_std_setenv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_getenviron(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int get_bool_option(JSContext* ctx, BOOL* pbool,
    JSValueConst obj,
    const char* option);

extern JSClassID js_std_file_class_id;

typedef struct {
    FILE* f;
    BOOL close_in_finalizer;
} JSSTDFile;

void js_std_file_finalizer(JSRuntime* rt, JSValue val);

static inline ssize_t js_get_errno(ssize_t ret)
{
    if (ret == -1)
        ret = -errno;
    return ret;
}

JSValue js_new_std_file(JSContext* ctx, FILE* f,
    BOOL close_in_finalizer);

static inline void js_set_error_object(JSContext* ctx, JSValue obj, int err)
{
    if (!JS_IsUndefined(obj)) {
        JS_SetPropertyStr(ctx, obj, "errno", JS_NewInt32(ctx, err));
    }
}

JSValue js_std_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_fdopen(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_printf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_puts(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue js_std_file_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_printf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

void js_print_value_write(void* opaque, const char* buf, size_t len);

JSValue js_std_file_printObject(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_tell(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int is_bigint);

JSValue js_std_file_seek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_eof(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_error(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_clearerr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_fileno(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_read_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue js_std_file_write_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_getline(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_readAsString(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_getByte(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_std_file_putByte(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_seek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_read_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue js_os_isatty(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_ttyGetWinSize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_ttySetRaw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_remove(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_rename(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static inline BOOL is_main_thread(JSRuntime* rt)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    return !ts->recv_pipe;
}

#if !defined(_WIN32)
void js_watch_apply(JSThreadState* ts, JSOSRWHandler* rh, int want);
#endif

JSValue js_os_setReadHandler(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSOSSignalHandler* find_sh(JSThreadState* ts, int sig_num);

static inline void free_sh(JSRuntime* rt, JSOSSignalHandler* sh)
{
    list_del(&sh->link);
    JS_FreeValueRT(rt, sh->func);
    js_free_rt(rt, sh);
}

#if defined(_WIN32)
typedef void (*sighandler_t)(int sig_num);
#endif

JSValue js_os_signal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int64_t get_time_ms(void);

int64_t get_time_ns(void);
static inline int64_t js_time_us(void)
{
    return get_time_ns() / 1000;
}

JSValue js_os_loop_stats(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_now(JSContext* ctx, JSValue this_val,
    int argc, JSValue* argv);

void free_timer_body(JSRuntime* rt, JSOSTimer* th);

int timer_insert(JSRuntime* rt, JSThreadState* ts, JSOSTimer* th, int64_t off);

void timer_ids_grow(JSRuntime* rt, JSThreadState* ts);

void free_timer(JSRuntime* rt, JSOSTimer* th);

JSOSTimer* find_timer_by_id(JSThreadState* ts, int timer_id);

static inline JSOSTimer* timer_first(JSThreadState* ts)
{
    return ts->timer_heap[0]->head;
}

JSValue js_os_setTimeout(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue js_os_clearTimeout(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_sleepAsync(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

void call_handler(JSContext* ctx, JSValueConst func);

void call_timer(JSContext* ctx, JSValueConst func, int argc,
    JSValueConst* args);

#ifdef USE_WORKER
int js_waker_init(JSWaker* w);

void js_waker_signal(JSWaker* w);

void js_waker_close(JSWaker* w);

void js_free_message(JSWorkerMessage* msg);
#endif

int js_os_poll(JSContext* ctx);

JSValue js_os_getcwd(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_chdir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_mkdir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_readdir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_stat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int is_lstat);

JSValue js_os_utimes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_sleep(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#if defined(_WIN32)
char* realpath(const char* path, char* buf);
#endif

JSValue js_os_realpath(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#if !defined(_WIN32)
JSValue js_os_symlink(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_readlink(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

char** build_envp(JSContext* ctx, JSValueConst obj);

#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <spawn.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__APPLE__)
#define DYN_SPAWN_HAVE_ADDCHDIR 1
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 29)
#define DYN_SPAWN_HAVE_ADDCHDIR 1
#endif
#elif defined(__ANDROID__)
#if __ANDROID_API__ >= 34
#define DYN_SPAWN_HAVE_ADDCHDIR 1
#endif
#elif defined(__linux__) && !defined(__GLIBC__)
#define DYN_SPAWN_HAVE_ADDCHDIR 1
#endif

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
#define DYN_SPAWN_HAVE_ADDCLOSEFROM 1
#endif
#endif

extern _Thread_local int os_exec_own_group;

int os_exec_spawn_child(const char* file, char** argv, char** envp,
    int std_fds[3], const char* cwd,
    uid_t uid, gid_t gid, int use_path, pid_t* out);

JSValue js_os_exec(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_getpid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_waitpid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_pipe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_kill(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_dup(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_os_dup2(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
#endif

#ifdef USE_WORKER
extern JSClassID js_worker_class_id;

void* js_sab_alloc(void* opaque, size_t size);

void js_sab_free(void* opaque, void* ptr);

void js_sab_dup(void* opaque, void* ptr);

int js_sab_valid_ptr(void* opaque, void* ptr, size_t byte_length,
    size_t max_byte_length);

extern void* (*dyn_wmsg_alloc_fn)(size_t);

extern void (*dyn_wmsg_free_fn)(void*);

void dyn_wmsg_free(void* p);

void js_free_message_pipe(JSWorkerMessagePipe* ps);

void js_worker_finalizer(JSRuntime* rt, JSValue val);

void js_worker_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);

JSValue js_worker_ctor_internal(JSContext* ctx, JSValueConst new_target,
    JSWorkerMessagePipe* recv_pipe,
    JSWorkerMessagePipe* send_pipe);

JSValue js_worker_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue js_worker_postMessage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_worker_set_onmessage(JSContext* ctx, JSValueConst this_val,
    JSValueConst func);

JSValue js_worker_get_onmessage(JSContext* ctx, JSValueConst this_val);
#endif

#if defined(_WIN32)
#define OS_PLATFORM "win32"
#elif defined(__APPLE__)
#define OS_PLATFORM "darwin"
#elif defined(__EMSCRIPTEN__)
#define OS_PLATFORM "js"
#else
#define OS_PLATFORM "linux"
#endif

extern const JSCFunctionListEntry js_os_funcs[74];

int js_os_init(JSContext* ctx, JSModuleDef* m);

JSValue js_print_internal(JSContext* ctx, int argc, JSValueConst* argv,
    FILE* fp);

JSValue js_print(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    int bytes_needed;
    int bytes_seen;
    uint32_t codepoint;
    uint8_t lower, upper;
} JSUtf8DecodeState;

void js_textcodec_install(JSContext* ctx, JSValueConst global_obj);
void js_textcodec_utf8_init(JSUtf8DecodeState* st);
int js_textcodec_utf8_feed(JSContext* ctx, DynBuf* dbuf, const uint8_t* buf, size_t len, JSUtf8DecodeState* st, int final);
void js_blob_install(JSContext* ctx, JSValueConst global_obj);
void js_blob_install_url(JSContext* ctx, JSValueConst global_obj);
int js_blob_is(JSValueConst v);
void js_blob_free_rt(JSRuntime* rt, JSThreadState* ts);
JSValue js_blob_bytes(JSContext* ctx, JSValueConst blob);
JSValue js_blob_type(JSContext* ctx, JSValueConst blob);
JSValue js_blob_file_name(JSContext* ctx, JSValueConst blob);
JSValue js_blob_from_bytes(JSContext* ctx, JSValueConst bytes, JSValueConst type);
JSValue js_blob_form_entry(JSContext* ctx, JSValueConst value, JSValueConst filename, int has_filename);
JSValue js_os_open_as_blob(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

void js_streams_install(JSContext* ctx, JSValueConst global_obj);
void js_domex_install(JSContext* ctx, JSValueConst global_obj);
void wst_free_rt(JSRuntime* rt, JSThreadState* ts);
int wst_ns_install(JSContext* ctx, JSValueConst ns, int all);
JSValue js_streams_natives(JSContext* ctx);
int js_streams_loaded(JSContext* ctx);
JSValue js_streams_transfer_prepare(JSContext* ctx, JSValueConst msg, JSValueConst transfer);
JSValue js_streams_transfer_revive(JSContext* ctx, JSValue data);
int js_stream_port_natives(JSContext* ctx, JSValueConst natives);

extern _Atomic(int) js_std_deadline_delivered;

static inline int js_std_deadline_hit(void)
{
    int hit = js_std_exec_deadline_ms != 0
        && (uint64_t)get_time_ms() > js_std_exec_deadline_ms;
    if (hit)
        atomic_store_explicit(&js_std_deadline_delivered, 1, memory_order_relaxed);
    return hit;
}

JSValue js_global_btoa(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue js_global_atob(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#if defined(CONFIG_NATIVE_MODULE_VSERIALIZE) || defined(CONFIG_NATIVE_MODULE_HTTP)
void dyn_global_install_from_module(JSContext* ctx,
    const char* module,
    const char* holder,
    const char* const* pairs,
    int npairs);
#endif

void js_std_add_helpers(JSContext* ctx, int argc, char** argv);

void js_std_init_handlers(JSRuntime* rt);

void js_std_free_handlers(JSRuntime* rt);

int js_is_dom_exception(JSValueConst v);

static inline void js_std_dump_error1(JSContext* ctx, JSValueConst exception_val)
{
    dyn_stdio_sync();
    if (js_is_dom_exception(exception_val)) {
        JSValue s = JS_ToString(ctx, exception_val);
        const char* cs = JS_IsException(s) ? NULL : JS_ToCString(ctx, s);
        JS_FreeValue(ctx, s);
        if (cs) {
            fprintf(stderr, "%s\n", cs);
            JS_FreeCString(ctx, cs);
            return;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_PrintValue(ctx, js_print_value_write, stderr, exception_val, NULL);
    fputc('\n', stderr);
}

void js_std_dump_error(JSContext* ctx);

JSRejectedPromiseEntry* find_rejected_promise(JSContext* ctx, JSThreadState* ts,
    JSValueConst promise);

int js_std_loop(JSContext* ctx);

JSValue js_std_await(JSContext* ctx, JSValue obj);
JSValue dyn_utf8_decode_bytes(JSContext* ctx, const uint8_t* p, size_t n, int strip_bom);

#endif
