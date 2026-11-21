#ifndef LIBC_WST_H
#define LIBC_WST_H

#include "libc-internal.h"
#include "dyna-nat.h"

enum {
    WSTC_RS,
    WSTC_RSDR,
    WSTC_RSBR,
    WSTC_RSDC,
    WSTC_RBSC,
    WSTC_BYOBREQ,
    WSTC_WS,
    WSTC_WSW,
    WSTC_WSDC,
    WSTC_TS,
    WSTC_TSDC,
    WSTC_BLQS,
    WSTC_CQS,
    WSTC_TES,
    WSTC_TDS,
    WSTC_CS,
    WSTC_DS,
    WSTC_RSIT,
    WSTC_TEE,
    WSTC_PIPE,
    WSTC_FROM,
    WSTC_ADP,
    WSTC_N
};

enum {
    WSA_value,
    WSA_done,
    WSA_highWaterMark,
    WSA_size,
    WSA_start,
    WSA_pull,
    WSA_cancel,
    WSA_type,
    WSA_autoAllocateChunkSize,
    WSA_write,
    WSA_close,
    WSA_abort,
    WSA_mode,
    WSA_prototype,
    WSA_readable,
    WSA_writable,
    WSA_preventClose,
    WSA_preventAbort,
    WSA_preventCancel,
    WSA_signal,
    WSA_min,
    WSA_transform,
    WSA_flush,
    WSA_readableType,
    WSA_writableType,
    WSA_N
};

enum { WSX_NATIVES, WSX_FAST_PIPE, WSX_NATIVE_PIPE, WSX_PORT_PAIR, WSX_PORT_CLAIM, WSX_FD_SYNC, WSX_FD_READ, WSX_FD_WRITE, WSX_FD_WATCH, WSX_FD_FLUSH, WSX_STDIN, WSX_STDIN_RS, WSX_DISPOSE, WSX_OBJ_PROTO, WSX_MAP_CTOR, WSX_MAP_GET, WSX_MAP_SET, WSX_ERR0, WSX_ERR_LAST = WSX_ERR0 + 6, WSX_N };

typedef struct WstRt {
    JSAtom atoms[WSA_N];
    JSValue rs_iter_proto;
    JSValue abort_ctor;
    JSValue ab_ctor;
    JSValue ab_transfer;
    JSValue sig_ctor;
    JSValue sig_add;
    JSValue sig_remove;
    JSValue x[WSX_N];
    JSValue blen_size, count_size, dec_ctor, dec_decode, type_error_ctor, pc_ctor, pc_close;
    JSValue ctors[WSTC_N];
    int nat_st;
} WstRt;

extern JSClassID wst_cid[WSTC_N];

typedef struct SDef {
    JSValue p;
    int st;
    uint32_t tag;
} SDef;

typedef struct SQ {
    uint8_t* a;
    uint32_t head, len, cap, esz;
} SQ;

typedef JSValue SAlgoC(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv);
typedef void SAlgoU(JSContext* ctx, JSValueConst data, int argc, JSValueConst* argv, JSValueConst rx);

typedef struct SAlgo {
    union {
        SAlgoC* cfn;
        SAlgoU* ufn;
    };
    JSValue fn;
    union {
        JSValue self;
        JSValue data;
    };
} SAlgo;

enum { SA_NONE, SA_JS, SA_C, SA_U };

static inline int sa_kind(const SAlgo* a)
{
    if (JS_IsObject(a->fn))
        return SA_JS;
    if (!a->cfn)
        return SA_NONE;
    return JS_IsNull(a->fn) ? SA_U : SA_C;
}

typedef struct SQV {
    JSValue v;
    double size;
    int sentinel;
} SQV;

struct SReadReq;
typedef struct SRROps {
    void (*chunk)(JSContext* ctx, struct SReadReq* rr, JSValueConst chunk);
    void (*close)(JSContext* ctx, struct SReadReq* rr, JSValueConst chunk);
    void (*error)(JSContext* ctx, struct SReadReq* rr, JSValueConst e);
} SRROps;

typedef struct SReadReq {
    const SRROps* ops;
    JSValue a;
    JSValue b;
} SReadReq;

enum { RS_READABLE, RS_CLOSED, RS_ERRORED };
enum { WS_WRITABLE, WS_ERRORING, WS_CLOSED, WS_ERRORED };

typedef struct SRS SRS;
typedef struct SReader SReader;
typedef struct SRSDC SRSDC;
typedef struct SRBSC SRBSC;
typedef struct SWS SWS;
typedef struct SWriter SWriter;
typedef struct SWSDC SWSDC;

struct SRS {
    JSValue obj;
    JSValue reader;
    SReader* rd;
    JSValue stored_error;
    JSValue ctl;
    void* c;
    JSValue native;
    int8_t state;
    int8_t disturbed;
    int8_t owning;
    int8_t bytes;
};

struct SReader {
    JSValue obj;
    int byob;
    JSValue stream;
    SRS* s;
    SDef closed;
    SQ reqs;
};

struct SRSDC {
    JSValue obj;
    JSValue stream;
    SRS* s;
    SQ q;
    double qtotal;
    int8_t started, close_requested, pull_again, pulling, size_set;
    JSValue size_fn;
    double hwm;
    SAlgo pull, cancel;
};

typedef struct SPullInto {
    JSValue buffer;
    double buffer_byte_length;
    double byte_offset;
    double byte_length;
    double bytes_filled;
    double minimum_fill;
    double element_size;
    int ta_kind;
    int reader_type;
} SPullInto;

typedef struct SByteEntry {
    JSValue buffer;
    double byte_offset;
    double byte_length;
} SByteEntry;

enum { RT_NONE, RT_DEFAULT, RT_BYOB };

struct SRBSC {
    JSValue obj;
    JSValue stream;
    SRS* s;
    SQ q;
    double qtotal;
    int8_t started, close_requested, pull_again, pulling, aacs_set;
    JSValue byob_request;
    double hwm;
    SAlgo pull, cancel;
    double aacs;
    SQ pending;
};

typedef struct SAbortReq {
    SDef d;
    JSValue reason;
    int was_already_erroring;
    int set;
} SAbortReq;

typedef struct SWSCold {
    SDef close_req;
    SDef in_flight_close;
    SAbortReq pending_abort;
    SDef abort_d;
} SWSCold;

struct SWS {
    JSValue obj;
    JSValue stored_error;
    JSValue writer;
    SWriter* w;
    JSValue ctl;
    SWSDC* c;
    SQ write_reqs;
    SDef in_flight_write;
    SWSCold* k;
    JSValue native;
    int8_t state;
    int8_t backpressure;
};

struct SWriter {
    JSValue obj;
    JSValue stream;
    SWS* s;
    SDef ready;
    SDef closed;
};

struct SWSDC {
    JSValue obj;
    JSValue stream;
    SWS* s;
    SQ q;
    double qtotal;
    JSValue abort_ctl;
    int8_t started;
    int8_t size_set;
    JSValue size_fn;
    double hwm;
    SAlgo write, close, abort;
};

typedef struct STSDC STSDC;
typedef struct STS {
    JSValue obj;
    JSValue readable;
    SRS* rs;
    JSValue writable;
    SWS* ws;
    int backpressure;
    SDef bp_change;
    JSValue bp_waiter;
    JSValue ctl;
    STSDC* c;
} STS;

struct STSDC {
    JSValue obj;
    JSValue stream;
    STS* t;
    SAlgo transform, flush, cancel;
    SDef finish;
    JSValue finish_reason;
};

typedef struct SCtlHead {
    JSValue obj;
    JSValue stream;
    void* s;
} SCtlHead;

JSValue wst_ctl_get(JSContext* ctx, int k, void* c, JSValueConst stream, JSValue* slot);
void wst_ctl_drop(JSRuntime* rt, void* c);
void wst_ctl_fin(JSRuntime* rt, void* c);
void wst_ctl_mark(JSRuntime* rt, void* c, JS_MarkFunc* mf);
JSValue wst_rs_ctl(JSContext* ctx, SRS* s);
JSValue wst_ws_ctl(JSContext* ctx, SWS* s);
void wst_rbsc_free(JSRuntime* rt, SRBSC* c);
void wst_rbsc_mark(JSRuntime* rt, SRBSC* c, JS_MarkFunc* mf);

WstRt* wst_rt(JSContext* ctx);
int wst_slot(JSContext* ctx, JSValue* slot, JSValue v);
JSAtom wst_atom(JSContext* ctx, int a);
const char* wst_atom_name(int a);

typedef struct WstClassSpec {
    int k;
    const char* name;
    JSCFunction* ctor;
    int len;
    const JSCFunctionListEntry* proto;
    int nproto;
    const JSCFunctionListEntry* stat;
    int nstat;
    JSClassFinalizer* fin;
    JSClassGCMark* mark;
} WstClassSpec;

#define WST_FN(name, len, fn) { name, JS_PROP_C_W_E, JS_DEF_CFUNC, 0, .u = { .func = { len, JS_CFUNC_generic, { .generic = fn } } } }
#define WST_GET(name, fn) { name, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, JS_DEF_CGETSET, 0, .u = { .getset = { .get = { .getter = fn }, .set = { .setter = NULL } } } }
#define WST_TAG(name) JS_PROP_STRING_DEF("[Symbol.toStringTag]", name, JS_PROP_CONFIGURABLE)

int wst_register(JSContext* ctx, const WstClassSpec* sp, JSValue* ctor_out);
JSValue wst_illegal_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);
#define WST_NEED_NEW(ctx, nt) do { if (JS_IsUndefined(nt)) return wst_throw(ctx, 0, "class constructors must be invoked with 'new'"); } while (0)
void wst_free_rt(JSRuntime* rt, JSThreadState* ts);
void* wst_opaque(JSValueConst v, int k);
JSValue wst_new_obj(JSContext* ctx, int k, JSValueConst proto, size_t sz, void** out);
JSValue wst_proto_from_target(JSContext* ctx, JSValueConst new_target);
JSValue wst_proto(JSContext* ctx, int k);

void sdef_init(SDef* d);
int sdef_new(JSContext* ctx, SDef* d);
void sdef_resolve(JSContext* ctx, SDef* d, JSValueConst v);
void sdef_reject(JSContext* ctx, SDef* d, JSValueConst e);
int sdef_new_resolved(JSContext* ctx, SDef* d, JSValueConst v);
int sdef_new_rejected(JSContext* ctx, SDef* d, JSValueConst e);
void sdef_free(JSRuntime* rt, SDef* d);
void sdef_mark(JSRuntime* rt, SDef* d, JS_MarkFunc* mf);

void sq_init(SQ* q, uint32_t esz);
void* sq_push(JSContext* ctx, SQ* q);
void* sq_peek(SQ* q);
void* sq_at(SQ* q, uint32_t i);
void sq_shift(SQ* q, void* out);
void sq_release(JSRuntime* rt, SQ* q);
void sq_steal(SQ* dst, SQ* src);
int sq_grow(JSContext* ctx, SQ* q);

static inline void* sq_push_t(JSContext* ctx, SQ* q, size_t sz)
{
    void* slot;
    if (q->len == q->cap && sq_grow(ctx, q) < 0)
        return NULL;
    slot = q->a + (size_t)((q->head + q->len) & (q->cap - 1)) * sz;
    q->len++;
    return slot;
}

static inline void sq_shift_t(SQ* q, void* out, size_t sz)
{
    memcpy(out, q->a + (size_t)q->head * sz, sz);
    q->head = (q->head + 1) & (q->cap - 1);
    if (--q->len == 0)
        q->head = 0;
}

void sa_init(SAlgo* a);
void sa_set_js(JSContext* ctx, SAlgo* a, JSValueConst fn, JSValueConst self);
void sa_set_c(JSContext* ctx, SAlgo* a, SAlgoC* cfn, JSValueConst data);
void sa_set_u(JSContext* ctx, SAlgo* a, SAlgoU* ufn, JSValueConst data);
void sa_clear(JSContext* ctx, SAlgo* a);
void sa_free(JSRuntime* rt, SAlgo* a);
void sa_mark(JSRuntime* rt, SAlgo* a, JS_MarkFunc* mf);
JSValue sa_call(JSContext* ctx, SAlgo* a, int argc, JSValueConst* argv);
void sa_call_upon(JSContext* ctx, SAlgo* a, int argc, JSValueConst* argv, JSValueConst rx);

JSValue wst_err(JSContext* ctx, int range, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
JSValue wst_throw(JSContext* ctx, int range, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
JSValue wst_rejected(JSContext* ctx, JSValueConst e);
JSValue wst_reject_err(JSContext* ctx, int range, const char* msg);
JSValue wst_reject_exc(JSContext* ctx);
JSValue wst_resolved_undef(JSContext* ctx);
JSValue wst_promise_resolved_with(JSContext* ctx, JSValueConst v);
JSValue wst_promise_call(JSContext* ctx, JSValueConst fn, JSValueConst self, int argc, JSValueConst* argv);
JSValue wst_iter_result(JSContext* ctx, JSValueConst v, int done);
JSValue wst_rx(JSContext* ctx, JSValue* slot, JSPromiseReactionFunc* fn, JSValueConst data, int magic);
int wst_upon(JSContext* ctx, JSValueConst p, JSValueConst rx);
int wst_upon_resolved_with(JSContext* ctx, JSValueConst v, JSPromiseReactionFunc* fn, JSValueConst data);
int wst_upon_start(JSContext* ctx, JSValueConst v, JSJobFunc* job, JSPromiseReactionFunc* fn, JSValueConst data);
JSValue wst_transform_undef(JSContext* ctx, JSValueConst p);
JSValue wst_then(JSContext* ctx, JSValueConst p, JSPromiseReactionFunc* fn, JSValueConst data, int magic);
void wst_drop_exc(JSContext* ctx);
JSValue wst_ret(JSContext* ctx, JSValue v);

int wst_to_dict(JSContext* ctx, JSValueConst v, const char* what, JSValue* out);
JSValue wst_dict_get(JSContext* ctx, JSValueConst d, int a);
int wst_cb_member(JSContext* ctx, JSValueConst d, int a, const char* what, JSValue* out);
int wst_enforce_ull(JSContext* ctx, JSValueConst v, const char* what, double* out);
int wst_strategy(JSContext* ctx, JSValueConst s, const char* what, double* hwm, int* hwm_set, JSValue* size);
int wst_extract_hwm(JSContext* ctx, double hwm, int hwm_set, double def, double* out);
int wst_size_call(JSContext* ctx, JSValueConst size_fn, JSValueConst chunk, JSValue* out);

static inline int wst_size_of(JSContext* ctx, JSValueConst size_fn, int size_set, JSValueConst chunk, JSValue* out)
{
    if (!size_set) {
        *out = JS_NewInt32(ctx, 1);
        return 0;
    }
    return wst_size_call(ctx, size_fn, chunk, out);
}

int wst_queue_push_slow(JSContext* ctx, SQ* q, double* total, JSValueConst v, JSValueConst size);

static inline int wst_queue_push(JSContext* ctx, SQ* q, double* total, JSValueConst v, JSValueConst size)
{
    SQV* e;
    if (JS_VALUE_GET_TAG(size) != JS_TAG_INT || JS_VALUE_GET_INT(size) < 0)
        return wst_queue_push_slow(ctx, q, total, v, size);
    e = sq_push_t(ctx, q, sizeof(SQV));
    if (!e)
        return -1;
    e->v = JS_DupValue(ctx, v);
    e->size = JS_VALUE_GET_INT(size);
    e->sentinel = 0;
    *total += e->size;
    return 0;
}

static inline JSValue wst_queue_shift(SQ* q, double* total)
{
    SQV e;
    sq_shift_t(q, &e, sizeof(SQV));
    *total -= e.size;
    if (*total < 0)
        *total = 0;
    return e.v;
}

static inline int wst_bool(JSContext* ctx, JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_BOOL)
        return JS_VALUE_GET_INT(v);
    return JS_ToBool(ctx, v);
}

JSValue wst_ws_new(JSContext* ctx, JSValueConst proto, SWS** out);
int wst_ws_locked(SWS* s);
JSValue wst_ws_abort(JSContext* ctx, SWS* s, JSValueConst reason);
JSValue wst_ws_close(JSContext* ctx, SWS* s);
int wst_ws_setup(JSContext* ctx, SWS* s, JSValueConst start_fn, JSValueConst start_self, JSValueConst start_value,
    SAlgo* write, SAlgo* close, SAlgo* abort, double hwm, JSValueConst size_fn, int size_set);
JSValue wst_ws_create(JSContext* ctx, SAlgo* write, SAlgo* close, SAlgo* abort, JSValueConst start_value, double hwm,
    JSValueConst size_fn, int size_set, SWS** out);
JSValue wst_ws_acquire_writer(JSContext* ctx, SWS* s, SWriter** out);
void wst_writer_release(JSContext* ctx, SWriter* w);
JSValue wst_writer_write(JSContext* ctx, SWriter* w, JSValueConst chunk);
void wst_writer_write_native(JSContext* ctx, SWriter* w, JSValueConst chunk, JSValueConst rx, uint32_t tag);
JSValue wst_writer_close_with_error_propagation(JSContext* ctx, SWriter* w);
JSValue wst_writer_desired(JSContext* ctx, SWriter* w);
void wst_wsdc_error_if_needed(JSContext* ctx, SWSDC* c, JSValueConst e);
void wst_ws_finish_erroring_if_started(JSContext* ctx, SWS* s, JSValueConst e);
int wst_init_ws(JSContext* ctx, JSValue* ctors);
int wst_init_rs(JSContext* ctx, JSValue* ctors);
int wst_init_bytes(JSContext* ctx, JSValue* ctors);

typedef struct WVInfo {
    JSValue buffer;
    double off, len, length, esize;
    int type;
} WVInfo;
int wst_view_info(JSContext* ctx, JSValueConst v, WVInfo* vi);
int wst_require_view(JSContext* ctx, JSValueConst v, const char* what, WVInfo* vi);
int wst_is_ab(JSValueConst v);
int wst_is_detached(JSValueConst ab);
double wst_ab_len(JSContext* ctx, JSValueConst ab);
JSValue wst_transfer_ab(JSContext* ctx, JSValueConst ab);
JSValue wst_new_view(JSContext* ctx, int type, JSValueConst buffer, double off, double length);
JSValue wst_new_u8(JSContext* ctx, JSValueConst buffer, double off, double len);
void wst_rbsc_error(JSContext* ctx, SRBSC* c, JSValueConst e);
int wst_rbsc_enqueue(JSContext* ctx, SRBSC* c, JSValueConst chunk);
int wst_rbsc_close(JSContext* ctx, SRBSC* c);
int wst_rbsc_respond(JSContext* ctx, SRBSC* c, double bytes_written);
int wst_rbsc_respond_with_new_view(JSContext* ctx, SRBSC* c, JSValueConst view);
JSValue wst_rbsc_get_byob_request(JSContext* ctx, SRBSC* c);
JSValue wst_rbsc_desired(JSContext* ctx, SRBSC* c);
JSValue wst_rbs_create(JSContext* ctx, JSValueConst start_value, SAlgo* pull, SAlgo* cancel, SRS** out);

extern const SRROps wst_rr_promise_ops;
void wst_rr_chunk(JSContext* ctx, SReadReq* rr, JSValueConst chunk);
void wst_rr_close(JSContext* ctx, SReadReq* rr, JSValueConst chunk);
void wst_rr_error(JSContext* ctx, SReadReq* rr, JSValueConst e);
void wst_free_value_queue(JSRuntime* rt, SQ* q, double* total);
JSValue wst_rs_new(JSContext* ctx, JSValueConst proto, SRS** out);
int wst_rs_locked(SRS* s);
void wst_rs_close(JSContext* ctx, SRS* s);
void wst_rs_error(JSContext* ctx, SRS* s, JSValueConst e);
void wst_rs_fulfill_read_request(JSContext* ctx, SRS* s, JSValueConst chunk, int done);
void wst_rs_fulfill_read_into_request(JSContext* ctx, SRS* s, JSValueConst chunk, int done);
int wst_rs_num_read_requests(SRS* s);
int wst_rs_add_read_request(JSContext* ctx, SRS* s, SReadReq* rr);
JSValue wst_rs_cancel(JSContext* ctx, SRS* s, JSValueConst reason);
JSValue wst_rs_acquire_reader(JSContext* ctx, SRS* s, int byob, SReader** out);
void wst_reader_release(JSContext* ctx, SReader* r);
void wst_reader_read(JSContext* ctx, SReader* r, SReadReq* rr);
JSValue wst_reader_read_promise(JSContext* ctx, SReader* r);
void wst_rsdc_error(JSContext* ctx, SRSDC* c, JSValueConst e);
void wst_rsdc_close(JSContext* ctx, SRSDC* c);
int wst_rsdc_enqueue(JSContext* ctx, SRSDC* c, JSValueConst chunk);
JSValue wst_rsdc_desired(JSContext* ctx, SRSDC* c);
int wst_rsdc_has_backpressure(SRSDC* c);
int wst_rsdc_can_close_or_enqueue(SRSDC* c);
int wst_rsdc_setup(JSContext* ctx, SRS* s, JSValueConst start_fn, JSValueConst start_self, JSValueConst start_value,
    SAlgo* pull, SAlgo* cancel, double hwm, JSValueConst size_fn, int size_set);
JSValue wst_rs_create(JSContext* ctx, JSValueConst start_value, SAlgo* pull, SAlgo* cancel, double hwm,
    JSValueConst size_fn, int size_set, SRS** out);
SRS* wst_rs_of(JSContext* ctx, JSValueConst v, const char* what);

int wst_rbsc_setup(JSContext* ctx, SRS* s, JSValueConst start_fn, JSValueConst start_self, JSValueConst start_value,
    SAlgo* pull, SAlgo* cancel, double hwm, double aacs, int aacs_set);
void wst_rbsc_pull_steps(JSContext* ctx, SRBSC* c, SReadReq* rr);
void wst_rbsc_pull_done(JSContext* ctx, SRBSC* c, int is_reject, JSValueConst arg);
JSValue wst_rbsc_cancel_steps(JSContext* ctx, SRBSC* c, JSValueConst reason);
void wst_rbsc_release_steps(JSContext* ctx, SRBSC* c);
JSValue wst_byob_read_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int wst_rs_tee(JSContext* ctx, SRS* s, JSValue* out);
int wst_init_pipe(JSContext* ctx);
int wst_init_ts(JSContext* ctx, JSValue* ctors);
int wst_tsdc_enqueue(JSContext* ctx, STSDC* c, JSValueConst chunk);
void wst_tsdc_error(JSContext* ctx, STSDC* c, JSValueConst e);
void wst_tsdc_terminate(JSContext* ctx, STSDC* c);
JSValue wst_ts_create(JSContext* ctx, SAlgo* transform, SAlgo* flush, SAlgo* cancel, STS** out);
int wst_init_misc(JSContext* ctx, JSValue* ctors);
JSValue wst_ts_shell(JSContext* ctx, JSValue readable, SRS* rs, JSValue writable, SWS* ws);
int wst_ws_close_queued_or_in_flight(SWS* s);
int wst_ws_has_op_in_flight(SWS* s);
int wst_buffer_source_bytes(JSContext* ctx, JSValueConst v, const char* what, JSValue* out);
int wst_bytes_of_chunk(JSContext* ctx, JSValueConst v, const char* what, JSValue* out);
JSValue wst_fast_pipe(JSContext* ctx, SRS* s, SWS* d, int prevent_close, int prevent_abort, int prevent_cancel);
int wst_init_adapt(JSContext* ctx, JSValueConst internal);
JSValue wst_rs_pipe_to(JSContext* ctx, SRS* s, SWS* d, int prevent_close, int prevent_abort, int prevent_cancel, JSValueConst signal);
void wst_byob_reader_read(JSContext* ctx, SReader* r, JSValueConst view, double min, SReadReq* rr);
JSValue wst_byobreq_view(JSContext* ctx, JSValueConst req);
JSValue wst_rs_pipe_to_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue wst_rs_pipe_through_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue wst_rs_from_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

#endif
