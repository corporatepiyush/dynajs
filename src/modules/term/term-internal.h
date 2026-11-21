#ifndef TERM_INTERNAL_H
#define TERM_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "core/dyn-stdio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TERM)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/select.h>

#if !defined(_WIN32)
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#endif

#include "core/dyn-sb.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_CMD_MAX_OPTS 256
#define DYN_CMD_MAX_ARGS 64
#define DYN_CMD_MAX_SUBS 64

enum { DYN_T_BOOL,
    DYN_T_STRING,
    DYN_T_NUMBER };

typedef struct {
    char* shortname;
    char* longname;
    char* desc;
    char* placeholder;
    char* env;
    int type;
    int required;
    int variadic;
    JSValue dflt;
} dyn_opt_t;

typedef struct {
    char* name;
    char* desc;
    int required;
    int variadic;
} dyn_arg_t;

typedef struct dyn_cmd {
    char *name, *desc;
    char* version;
    JSValue action;
    dyn_opt_t opts[DYN_CMD_MAX_OPTS];
    int n_opts;
    dyn_arg_t args[DYN_CMD_MAX_ARGS];
    int n_args;
    JSValue subs[DYN_CMD_MAX_SUBS];
    int n_subs;
    int allow_unknown;
} dyn_cmd_t;

extern JSClassID dyn_cmd_class_id;

void dyn_cmd_finalizer(JSRuntime* rt, JSValue val);

void dyn_cmd_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark);

static inline char* dyn_dup_span(const char* s, size_t n)
{
    char* r = (char*)malloc(n + 1);
    if (!r)
        return NULL;
    if (n)
        memcpy(r, s, n);
    r[n] = 0;
    return r;
}

char* dyn_arg_esc(const char* s);

JSValue dyn_cmd_option(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_cmd_argument(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_cmd_describe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_cmd_command(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_cmd_allow_unknown(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static inline dyn_opt_t* dyn_find_long(dyn_cmd_t* c, const char* s, size_t n)
{
    int i;
    for (i = 0; i < c->n_opts; i++)
        if (strlen(c->opts[i].longname) == n && memcmp(c->opts[i].longname, s, n) == 0)
            return &c->opts[i];
    return NULL;
}

static inline dyn_opt_t* dyn_find_short(dyn_cmd_t* c, char ch)
{
    int i;
    for (i = 0; i < c->n_opts; i++)
        if (c->opts[i].shortname && c->opts[i].shortname[0] == ch
            && c->opts[i].shortname[1] == 0)
            return &c->opts[i];
    return NULL;
}

int dyn_number_span(const char* s);

JSValue dyn_cmd_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int dyn_parse_one(JSContext* ctx, dyn_cmd_t* c, JSValueConst opts,
    const char** av, int n, int i, JSValueConst positional,
    uint32_t* n_pos, int* no_more_opts);

int dyn_env_apply(JSContext* ctx, dyn_cmd_t* c, JSValueConst opts,
    const unsigned char* supplied);

JSValue dyn_cmd_help(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_cmd_get_name(JSContext* ctx, JSValueConst this_val);

JSValue dyn_cmd_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_cmd_action(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_cmd_version(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    const char* name;
    const char* on;
    const char* off;
} dyn_style_t;

extern const dyn_style_t DYN_STYLES[46];

JSValue dyn_style_text(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#define DYN_INQ_SIZE 16384u

extern unsigned char dyn_inq[16384];

extern unsigned int dyn_inq_head;

extern unsigned int dyn_inq_n;

#if !defined(_WIN32)
typedef struct termios dyn_termios_t;
#else
typedef int dyn_termios_t;
#endif

int dyn_raw_on(dyn_termios_t* saved);

void dyn_raw_off(dyn_termios_t* saved, int on);

int dyn_byte_at(int fd, unsigned char* c, int timeout_ms);

typedef struct {
    int eof;
    char seq[16];
    int seqlen;
    char chbuf[8];
    const char* name;
    int ctrl, meta, shift;
} dyn_key_t;

int dyn_key_read(int fd, dyn_key_t* k);

JSValue dyn_key_obj(JSContext* ctx, const dyn_key_t* k);

extern JSClassID dyn_pbar_class_id;

void dyn_pbar_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_pbar_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_pbar_update(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_spin_class_id;

void dyn_spin_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_spin_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_spin_start(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_spin_tick(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_spin_stop(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
