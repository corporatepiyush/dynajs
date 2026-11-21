#ifndef SYS_INTERNAL_H
#define SYS_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "core/dyn-par.h"
#include "dtoa.h"
#include "cutils.h"
#include "core/dyn-simdk.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SYS)

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <grp.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#endif

#include <sys/resource.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <crt_externs.h>
#define dyn_environ (*_NSGetEnviron())
#else
extern char** environ;
#define dyn_environ environ
#endif

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

JSValue dyn_sys_throw(JSContext* ctx, int e, const char* op,
    const char* path);

const char* dyn_sys_cstr(JSContext* ctx, JSValueConst v,
    const char* what);

JSValue dyn_sys_env(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_sys_get_env(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_sys_set_env(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#else
#include <sys/statvfs.h>
#endif

JSValue dyn_sys_cpu_info(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_sys_mem_info(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_sys_load_avg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_sys_uptime(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_sys_disk_usage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>
#include "core/dyn-sb.h"
#include "core/dyn-childreg.h"
#define DYN_EXEC_MAXBUF (8u << 20)
#define DYN_EXEC_GRACE_MS 2000
#define DYN_EXEC_CHUNK 65536

const char* dyn_signal_name(int sig);

int dyn_path_lookup(const char* name, const char* path, char* buf,
    size_t bufsz, int skip_empty);

void dyn_kill_group(pid_t pid, int sig);

int64_t dyn_now_ms(void);

void dyn_strv_free(char** v);
int dyn_env_extend_opt(JSContext* ctx, JSValueConst o, const char* who, char*** envp);

typedef struct {
    char **argv, **envp;
    char* cwd;
    char path[PATH_MAX];
    long maxfd;
    int in[2], out[2], err[2];
    int64_t timeout_ms;
    size_t maxbuf;
    uid_t uid;
    gid_t gid;
    const uint8_t* input;
    size_t inlen;
    int no_path_cwd;
    sigset_t sigmask;
} dyn_exec_t;

long dyn_exec_fd_limit(void);

void dyn_exec_child(const dyn_exec_t* e);

JSValue dyn_exec(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_which(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

struct dyn_aio;

struct dyn_aio* dyn_net_reactor_acquire(JSContext* ctx);

void dyn_net_reactor_release(JSContext* ctx);

void dyn_net_reactor_release_rt(JSRuntime* rt);

int dyn_net_on_drain(void (*fn)(void*), void* udata);

void dyn_net_off_drain(void* udata);

#include <strings.h>
#include "dyna-aio.h"
#include "dyna-evloop.h"

typedef struct dyn_spawn dyn_spawn_t;

typedef struct {
    dyn_spawn_t* sp;
    int fd;
    int eof;
    int err;
    int watch;
    uint8_t* buf;
    size_t len, cap, head;
    int pending;
    JSValue rbuf;
    JSValue rresolve, rreject;
    JSValue promise;
} dyn_spawn_pipe_t;

typedef struct {
    dyn_spawn_t* sp;
    int fd;
    int piped;
    int close_after_drain;
    int err;
    int err_errno;
    JSValue flush_resolve, flush_reject;
} dyn_spawn_stdin_t;

struct dyn_spawn {
    int refs;
    int torn_down;
    int user_closed;
    JSRuntime* rt;
    JSContext* ctx;
    pid_t pid;
    int exited, code, sig, timed_out, reaped;
    int wait_settled;
    JSValue wait_resolve, wait_reject;
    dyn_spawn_pipe_t out, err;
    dyn_spawn_stdin_t in;
    uint8_t* wq;
    size_t wlen, wcap, woff;
    size_t maxpipe;
    struct dyn_aio* aio;
    int hooked;
    int64_t deadline, kill_at;
    int term_sent;
    int kill_fatal;
    int dead;
};

extern JSClassID dyn_spawn_class_id;

extern JSClassID dyn_spawn_pipe_class_id;

extern JSClassID dyn_spawn_stdin_class_id;

void spawn_release(dyn_spawn_t* sp);

static inline void spawn_unpark(dyn_spawn_t* sp);

void spawn_pipe_force_eof(dyn_spawn_pipe_t* p);

uint8_t* spawn_view_bytes(JSContext* ctx, JSValueConst v, size_t* plen);

void spawn_settle(JSContext* ctx, JSValue* resolve, JSValue* reject,
    JSValue val, int failed);

static inline void spawn_close_fd(dyn_spawn_t* sp, int* fd)
{
    if (*fd >= 0) {
        if (sp->aio)
            dyn_evloop_del(dyn_aio_evloop(sp->aio), *fd);
        close(*fd);
        *fd = -1;
    }
}

void spawn_kill_child(dyn_spawn_t* sp, int sig);

static inline void spawn_aio_release_if_idle(dyn_spawn_t* sp)
{
    if (sp->aio) {
        if (!sp->exited || sp->out.fd >= 0 || sp->err.fd >= 0
            || sp->in.fd >= 0)
            return;
        dyn_net_reactor_release_rt(sp->rt);
        sp->aio = NULL;
    }
}

JSValue spawn_wait_result(dyn_spawn_t* sp);

void spawn_shutdown_sweep(JSContext* ctx, JSRuntime* rt, void* opaque);

int spawn_try_reap(dyn_spawn_t* sp);

static inline void spawn_unhook_if_done(dyn_spawn_t* sp)
{
    if (sp->exited && sp->hooked) {
        dyn_net_off_drain(sp);
        sp->hooked = 0;
    }
}

static inline void spawn_unpark(dyn_spawn_t* sp)
{
    spawn_release(sp);
}

void spawn_pipe_finalizer(JSRuntime* rt, JSValue val);

void spawn_stdin_finalizer(JSRuntime* rt, JSValue val);

static inline void spawn_after_reap(dyn_spawn_t* sp)
{
    if (sp->timed_out || sp->kill_fatal) {
        spawn_pipe_force_eof(&sp->out);
        spawn_pipe_force_eof(&sp->err);
    }
    spawn_unhook_if_done(sp);
    spawn_aio_release_if_idle(sp);
}

void spawn_pipe_fail_pending(dyn_spawn_pipe_t* p, const char* msg);

JSValue spawn_pipe_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue spawn_pipe_async_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
JSValue spawn_pipe_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue spawn_stdin_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue spawn_stdin_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue spawn_stdin_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_spawn_wait(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_spawn_kill(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue spawn_view_get(JSContext* ctx, JSValueConst this_val,
    JSClassID cid, void* opaque);

JSValue spawn_get_stdin(JSContext* ctx, JSValueConst this_val);

int spawn_env_at(JSContext* ctx, JSValueConst obj, JSAtom atom,
    char** slot);

int spawn_opt_int(JSContext* ctx, JSValueConst o, const char* key,
    int64_t floor_, const char* what, int64_t* out);

int spawn_opt_id(JSContext* ctx, JSValueConst o, const char* key,
    int64_t* out);

JSValue dyn_spawn_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

#endif

#endif
