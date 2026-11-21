#ifndef AI_INTERNAL_H
#define AI_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "core/dyn-httpk.h"
#include "core/dyn-sb.h"
#include "modules/http/http-conn.h"
#include "cutils.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

#define AI_ANTHROPIC_VERSION "2026-10-21"
#define AI_ANTHROPIC_BASE_URL "https://api.anthropic.com"
#define AI_OPENAI_BASE_URL "https://api.openai.com/v1"
#define AI_CHAT_MAX_INDEX 1024

enum {
    AI_KIND_ANTHROPIC = 0,
    AI_KIND_OPENAI = 1
};
#define AI_TIMEOUT_MS 600000
#define AI_MAX_RETRIES 2
#define AI_MAX_RETRIES_CAP 10
#define AI_BACKOFF_BASE_MS 500.0
#define AI_BACKOFF_CAP_MS 8000.0
#define AI_RETRY_AFTER_CAP_MS 60000.0
#define AI_MAX_BODY ((uint64_t)256 * 1024 * 1024)
#define AI_MAX_EVENT ((size_t)64 * 1024 * 1024)

typedef struct ai_client {
    int refs;
    int closed;
    int kind;
    char* org;
    char* project;
    char* base_url;
    char* api_key;
    char* auth_token;
    char* version;
    char* betas;
    char* extra_headers;
    char* ca;
    int64_t timeout_ms;
    int max_retries;
    uint64_t rng;
#ifdef CONFIG_TLS
    dyn_tls_ctx_t* tls;
#endif
} ai_client_t;

enum {
    AI_OUT_JSON = 0,
    AI_OUT_STREAM = 1,
    AI_OUT_BYTES = 2,
    AI_OUT_JSONL = 3
};

#define AI_FILE_EXPIRES_MIN 3600
#define AI_FILE_EXPIRES_MAX 7776000
#define AI_BATCH_MAX_REQUESTS 100000
#define AI_FILENAME_MAX 255
#define AI_FILES_IDS_MAX 100
#define AI_BOUNDARY_LEN 39
#define AI_MAX_FILE ((uint64_t)512 * 1024 * 1024)

#define AI_E_ABORTED (-100)

typedef struct ai_cancel {
    pthread_mutex_t mu;
    int refs;
    int cancelled;
    int fd;
} ai_cancel_t;

typedef struct ai_req {
    ai_client_t* cl;
    char* method;
    char* url;
    char* headers;
    uint8_t* body;
    size_t body_len;
    int out;
    int emb_f32;
    int64_t timeout_ms;
    int max_retries;
    int code;
    char why[256];
    int status;
    int attempts;
    hc_head_t hd;
    hc_stream_t* st;
    dyn_sb_t resp;
    ai_cancel_t* cancel;
    JSRuntime* rt;
    JSValue signal;
    JSValue listener;
} ai_req_t;

typedef struct {
    char* type;
    size_t type_len;
    char* data;
    size_t data_len;
} ai_rawev_t;

typedef struct {
    dyn_sb_t buf;
    uint8_t field;
    uint8_t live;
} ai_blockbuf_t;

enum {
    AI_FIELD_NONE = 0,
    AI_FIELD_TEXT,
    AI_FIELD_THINKING,
    AI_FIELD_JSON
};

typedef struct ai_accum {
    httpk_sse_t sse;
    ai_rawev_t* q;
    size_t qn, qcap, qhead;
    ai_blockbuf_t* bb;
    size_t nbb;
    JSValue msg;
    int started;
    int stopped;
    int oom;
    int too_big;
    int kind;
    struct ai_chat* chat;
} ai_accum_t;

extern JSClassID ai_apierror_class_id;
extern JSClassID ai_client_class_id;
extern JSClassID ai_ns_class_id;
extern JSClassID ai_stream_class_id;
extern JSClassID ai_accum_class_id;
extern JSClassID ai_results_class_id;
extern JSClassID ai_openai_class_id;
extern JSClassID ai_chat_stream_class_id;
extern JSClassID ai_chat_accum_class_id;

ai_client_t* ai_client_ref(ai_client_t* cl);
void ai_client_unref(ai_client_t* cl);
int ai_client_tls(JSContext* ctx, ai_client_t* cl);
JSValue ai_client_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);
ai_client_t* ai_ns_client(JSContext* ctx, JSValueConst this_val);
JSValue ai_ns_new(JSContext* ctx, JSValueConst owner, ai_client_t* cl,
    const JSCFunctionListEntry* fns, int n);

ai_req_t* ai_req_new(JSContext* ctx, ai_client_t* cl, const char* method,
    const char* path, JSValueConst query, JSValueConst body, JSValueConst opts,
    int out);
ai_req_t* ai_req_new_raw(JSContext* ctx, ai_client_t* cl, const char* method,
    const char* path, JSValueConst query, const uint8_t* body, size_t body_len,
    const char* ctype, JSValueConst opts, int out);
int ai_id_path(JSContext* ctx, const char* who, const char* pre, JSValueConst id,
    const char* post, dyn_sb_t* out);
JSValue ai_iter_result(JSContext* ctx, JSValue v, int done);
void ai_req_free(ai_req_t* r);
void ai_path_seg(dyn_sb_t* out, const char* s, size_t n);
void ai_req_run(ai_req_t* r);
JSValue ai_req_result(JSContext* ctx, ai_req_t* r);
JSValue ai_req_start(JSContext* ctx, ai_req_t* r);
JSValue ai_req_error(JSContext* ctx, ai_req_t* r);

JSValue ai_error_new(JSContext* ctx, const char* message, int status,
    const char* type, const char* request_id, double retry_after_ms,
    JSValueConst headers, JSValueConst body);
JSValue ai_error_throw(JSContext* ctx, const char* message, int status,
    const char* type);
JSValue ai_error_from_body(JSContext* ctx, int status, const char* body,
    size_t n, JSValueConst headers, double retry_after_ms);
JSValue ai_error_from_event(JSContext* ctx, JSValueConst data);
int ai_error_status_of(const char* type);
JSValue ai_error_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);
int ai_error_register(JSContext* ctx, JSModuleDef* m);

void ai_accum_init(ai_accum_t* a);
void ai_accum_free(JSRuntime* rt, ai_accum_t* a);
int ai_accum_feed(ai_accum_t* a, const char* p, size_t n);
int ai_accum_pending(const ai_accum_t* a);
JSValue ai_accum_next(JSContext* ctx, ai_accum_t* a);
JSValue ai_accum_snapshot(JSContext* ctx, ai_accum_t* a);
void ai_accum_init_kind(ai_accum_t* a, int kind);
JSValue ai_chat_next(JSContext* ctx, ai_accum_t* a);
JSValue ai_chat_snapshot(JSContext* ctx, ai_accum_t* a);
void ai_chat_free(ai_accum_t* a);
JSValue ai_body_with_stream(JSContext* ctx, JSValueConst body, int usage);
const char* ai_client_name(const ai_client_t* cl);
ai_client_t* ai_client_this(JSContext* ctx, JSValueConst this_val);
int ai_openai_register(JSContext* ctx, JSModuleDef* m);
JSValue ai_emb_f32(JSContext* ctx, JSValue v, int status);
int ai_cancel_register(JSContext* ctx);
void ai_cancel_unref(ai_cancel_t* c);
int ai_cancel_on_fd(void* ud, int fd);
int ai_req_cancelled(ai_req_t* r);
void ai_req_drop_stream(ai_req_t* r);
int ai_req_signal(JSContext* ctx, ai_req_t* r, JSValueConst signal);
void ai_req_unlisten(JSContext* ctx, ai_req_t* r);
JSValue ai_abort_throw(JSContext* ctx);
JSValue ai_openai_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue ai_stream_new(JSContext* ctx, ai_req_t* r);
int ai_stream_register(JSContext* ctx, JSModuleDef* m);
int ai_anthropic_register(JSContext* ctx, JSModuleDef* m);
int ai_results_register(JSContext* ctx, JSModuleDef* m);

int ai_opts_check(JSContext* ctx, JSValueConst opts, const char* who,
    const char* const* keys, int n);
int ai_arr_len(JSContext* ctx, JSValueConst arr, int64_t* n);
int ai_betas_join(JSContext* ctx, JSValueConst arr, const char* who,
    dyn_sb_t* out);
int ai_hdr_block(JSContext* ctx, JSValueConst obj, const char* who,
    dyn_sb_t* out);
char* ai_opt_str(JSContext* ctx, JSValueConst opts, const char* key,
    const char* who, int* err);

#endif
#endif
