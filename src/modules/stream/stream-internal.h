#ifndef STREAM_INTERNAL_H
#define STREAM_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#include <stdio.h>
#define DYN_STREAM_CHUNK (1u << 17)

JSValue stream_errno_throw(JSContext* ctx, int e, const char* op,
    const char* path);

JSValue stream_promise_resolved(JSContext* ctx, JSValue val);

JSValue stream_promise_rejected(JSContext* ctx, JSValue exc);

void stream_drop_exc(JSContext* ctx);

JSValue stream_take_exc(JSContext* ctx);

JSValue stream_ret(JSContext* ctx, JSValue v);

uint8_t* stream_view_bytes(JSContext* ctx, JSValueConst v,
    size_t* plen, int require_bpe1);

typedef struct dyn_cps dyn_cps_t;

typedef struct dyn_cps_arm dyn_cps_arm_t;

typedef void (*dyn_cps_step_fn)(JSContext* ctx, dyn_cps_t* cp,
    JSValueConst val, int failed);

struct dyn_cps {
    dyn_cps_step_fn step;
    dyn_cps_arm_t* arm;
    int dead;
};

#define DYN_CPS_STAYS_FIRST(type) \
    _Static_assert(offsetof(type, cps) == 0, #type ".cps must stay first")

struct dyn_cps_arm {
    dyn_cps_t* cp;
    JSValue anchor;
    int settled;
};

extern JSClassID dyn_cps_anchor_class_id;

void dyn_cps_anchor_finalizer(JSRuntime* rt, JSValue val);

void dyn_cps_arm_kill(JSContext* ctx, JSRuntime* rt, dyn_cps_t* cp);

void dyn_cps_await(JSContext* ctx, dyn_cps_t* cp, dyn_cps_step_fn step,
    JSValue promise);

#define STREAM_SRC_BYTES 0

void stream_park_fail(JSContext* ctx, JSRuntime* rt,
    JSValue* resolve, JSValue* reject,
    const char* what);

void stream_op_sweep_tail(JSContext* ctx, JSRuntime* rt,
    dyn_cps_t* cp, JSValue* first, int n);

typedef struct {
    int kind;
    int fd;
    uint64_t off;
    uint8_t* buf;
    size_t len;
    char* path;
} stream_source_t;

extern JSClassID stream_source_class_id;

void stream_source_dispose(void* native);

JSValue stream_source_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#define STREAM_SINK_DEFAULT_BUF (1u << 17)

typedef struct {
    int fd;
    unsigned char* buf;
    unsigned cap;
    unsigned len;
    int append;
    int sticky_errno;
    uint64_t total;
    char* path;
} stream_sink_t;

extern JSClassID stream_sink_class_id;

unsigned stream_sink_clamp_bufsize(int64_t v);
int stream_sink_direct(JSContext* ctx, JSValueConst sink, JSValueConst write_fn, const uint8_t* p, size_t n);

void stream_sink_dispose(void* native);

int stream_sink_flush_native(stream_sink_t* w);

int stream_sink_open(stream_sink_t* w, JSContext* ctx);

JSValue stream_sink_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue stream_sink_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    dyn_cps_t cps;
    JSValue src, dst;
    JSValue read_fn, write_fn;
    JSValue on_chunk;
    JSValue chunk;
    JSValue jresolve, jreject;
    JSValue exc;
    uint64_t total;
    size_t chunk_read;
    size_t chunk_written;
    int terminal;
} pipe_state_t;

DYN_CPS_STAYS_FIRST(pipe_state_t);

_Static_assert(offsetof(pipe_state_t, exc) == offsetof(pipe_state_t, src) + 8 * sizeof(JSValue),
    "pipe_state_t pins must stay contiguous");


JSValue stream_pipe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#define LINE_MODE_NDJSON 1

typedef struct {
    dyn_cps_t cps;
    JSValue it_obj;
    JSValue jresolve, jreject;
} lines_op_t;

DYN_CPS_STAYS_FIRST(lines_op_t);

extern JSClassID lines_iter_class_id;

_Static_assert(offsetof(lines_op_t, jreject) == offsetof(lines_op_t, it_obj) + 2 * sizeof(JSValue),
    "lines_op_t pins must stay contiguous");

void lines_iter_finalizer(JSRuntime* rt, JSValue val);

void lines_iter_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark);

JSValue lines_iter_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue lines_iter_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue lines_iter_async_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue stream_lines_common(JSContext* ctx, int argc,
    JSValueConst* argv, int mode);

JSValue stream_lines(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum {
    DYN_STREAM_CODEC_DEFLATE,
    DYN_STREAM_CODEC_GZIP,
    DYN_STREAM_CODEC_ZSTD,
    DYN_STREAM_CODEC_BROTLI,
    DYN_STREAM_CODEC_LZ4,
};

#if defined(CONFIG_ZSTD)
#include <zstd.h>
#endif

#if defined(__APPLE__) && !defined(DYN_STREAM_PORTABLE_CODEC)
#include <compression.h>
#define DYN_STREAM_HAVE_BROTLI 1
#endif

#include "core/dyn-compress.h"
#include "core/dyn-hash.h"

#if !defined(DYN_STREAM_HAVE_BROTLI)
typedef enum {
    COMPRESSION_STATUS_OK = 0,
    COMPRESSION_STATUS_ERROR = -1,
    COMPRESSION_STATUS_END = 1
} compression_status;
typedef enum {
    COMPRESSION_STREAM_ENCODE = 0,
    COMPRESSION_STREAM_DECODE = 1
} compression_stream_operation;
typedef enum {
    COMPRESSION_ZLIB = 0x205,
    COMPRESSION_BROTLI = 0xB02
} compression_algorithm;
#define COMPRESSION_STREAM_FINALIZE 1
#define DYN_CS_LEVEL 6
#define DYN_CS_MAX_OUTPUT ((size_t)1 << 30)

typedef struct {
    int op;
    int ended;
    int ctx_ready;
    dyn_comp_ctx_t ctx;
    uint8_t* held;
    size_t held_len, held_cap;
    uint8_t* ready;
    size_t ready_len, ready_cap, ready_off;
} dyn_cs_state_t;

typedef struct {
    uint8_t* dst_ptr;
    size_t dst_size;
    const uint8_t* src_ptr;
    size_t src_size;
    dyn_cs_state_t* state;
} compression_stream;

compression_status compression_stream_init(compression_stream* cs,
    compression_stream_operation op, compression_algorithm algo);
void compression_stream_destroy(compression_stream* cs);
compression_status compression_stream_process(compression_stream* cs, int flags);
#endif

typedef struct {
    uint8_t* buf;
    size_t len, cap;
} cbuf_t;

int cbuf_reserve(cbuf_t* b, size_t extra);

static inline int cbuf_put(cbuf_t* b, const void* data, size_t n)
{
    if (cbuf_reserve(b, n) < 0)
        return -1;
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    return 0;
}

static inline void cbuf_free(cbuf_t* b)
{
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

JSValue stream_bytes_view(JSContext* ctx, const uint8_t* data,
    size_t len);

typedef struct {
    int codec;
    int level;
#if defined(CONFIG_ZSTD)
    ZSTD_CCtx* zc;
#endif
    dyn_defs_t* ds;
    compression_stream cs;
    int cs_active;
    uint32_t crc_reg;
    uint32_t isize;
    int header_emitted;
    int finalized;
    cbuf_t lz4_pend;
} codec_enc_t;

void codec_enc_free(codec_enc_t* e);

int codec_enc_init(codec_enc_t* e, int codec, int level);

const char* codec_enc_feed(codec_enc_t* e, const uint8_t* in,
    size_t in_len, int finalize, cbuf_t* out);

typedef struct {
    int codec;
#if defined(CONFIG_ZSTD)
    ZSTD_DStream* zd;
#endif
    compression_stream cs;
    int cs_active;
    cbuf_t in;
    cbuf_t out;
    int phase;
    uint32_t crc_reg, isize;
    uint32_t gz_want_crc, gz_want_isize;
    int gz_have_want;
    uint32_t lz4_block_max;
    int lz4_content_checksum;
    dyn_xxh32_ctx_t lz4_xxh;
    int done;
    int failed;
    int progressed;
} codec_dec_t;

void codec_dec_free(codec_dec_t* d);

int codec_dec_init(codec_dec_t* d, int codec);

void dec_take(cbuf_t* in, size_t n);

const char* codec_dec_run(codec_dec_t* d, int finalize, size_t reserve);

const char* codec_dec_finish(codec_dec_t* d);

int dyn_stream_web_natives(JSContext* ctx, JSValueConst natives);
typedef int (*dyn_push_emit_fn)(void* ud, uint8_t* buf, size_t len);
extern const char dyn_push_emit_failed[];
const char* dyn_push_codec_step(JSValueConst obj, const uint8_t* p, size_t n, int end, dyn_push_emit_fn emit, void* ud);
JSValue stream_web_bridge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);


#endif

#endif
