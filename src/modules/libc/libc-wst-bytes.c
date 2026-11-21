#include "libc-wst.h"
#include <math.h>

typedef struct SByobReq {
    JSValue obj;
    JSValue controller;
    SRBSC* c;
    JSValue view;
} SByobReq;

static const double wst_esize[13] = { 1, 1, 1, 2, 2, 4, 4, 8, 8, 2, 4, 8, 1 };

static void rbsc_call_pull_if_needed(JSContext* ctx, SRBSC* c);

int wst_view_info(JSContext* ctx, JSValueConst v, WVInfo* vi)
{
    int r = JS_GetViewInfo(ctx, v, &vi->buffer, &vi->off, &vi->len, &vi->type);
    if (r <= 0) {
        vi->buffer = JS_UNDEFINED;
        return r;
    }
    vi->esize = wst_esize[vi->type];
    vi->length = vi->len / vi->esize;
    return 1;
}

int wst_is_ab(JSValueConst v)
{
    return JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER && !JS_IsSharedArrayBuffer(v);
}

int wst_is_detached(JSValueConst ab)
{
    return JS_IsArrayBufferDetached(ab) == 1;
}

double wst_ab_len(JSContext* ctx, JSValueConst ab)
{
    size_t n = 0;
    (void)ctx;
    JS_PeekArrayBuffer(ab, &n);
    return (double)n;
}

static uint8_t* ab_data(JSContext* ctx, JSValueConst ab)
{
    size_t n;
    (void)ctx;
    return JS_PeekArrayBuffer(ab, &n);
}

JSValue wst_transfer_ab(JSContext* ctx, JSValueConst ab)
{
    WstRt* w = wst_rt(ctx);
    if (!w)
        return JS_EXCEPTION;
    if (wst_is_detached(ab))
        return wst_throw(ctx, 0, "the ArrayBuffer is detached");
    return JS_Call(ctx, w->ab_transfer, ab, 0, NULL);
}

JSValue wst_new_view(JSContext* ctx, int type, JSValueConst buffer, double off, double length)
{
    JSValue args[3];
    if (type == JS_VIEW_TYPE_DATAVIEW)
        return JS_NewDataView(ctx, buffer, (uint64_t)off, (uint64_t)length);
    args[0] = buffer;
    args[1] = JS_NewFloat64(ctx, off);
    args[2] = JS_NewFloat64(ctx, length);
    return JS_NewTypedArray(ctx, 3, (JSValueConst*)args, (JSTypedArrayEnum)type);
}

JSValue wst_new_u8(JSContext* ctx, JSValueConst buffer, double off, double len)
{
    return wst_new_view(ctx, JS_TYPED_ARRAY_UINT8, buffer, off, len);
}

static void copy_bytes(JSContext* ctx, JSValueConst dst, double doff, JSValueConst src, double soff, double n)
{
    uint8_t* d = ab_data(ctx, dst);
    uint8_t* s = ab_data(ctx, src);
    if (d && s && n > 0)
        memmove(d + (size_t)doff, s + (size_t)soff, (size_t)n);
}

static JSValue clone_buffer(JSContext* ctx, JSValueConst src, double off, double len)
{
    uint8_t* s = ab_data(ctx, src);
    if (!s)
        return wst_throw(ctx, 0, "the ArrayBuffer is detached");
    return JS_NewArrayBufferCopy(ctx, s + (size_t)off, (size_t)len);
}

int wst_require_view(JSContext* ctx, JSValueConst v, const char* what, WVInfo* vi)
{
    int r = wst_view_info(ctx, v, vi);
    if (r < 0)
        return -1;
    if (r == 0) {
        wst_throw(ctx, 0, "%s must be an ArrayBufferView", what);
        return -1;
    }
    if (!wst_is_ab(vi->buffer)) {
        JS_FreeValue(ctx, vi->buffer);
        vi->buffer = JS_UNDEFINED;
        wst_throw(ctx, 0, "%s must not be backed by a SharedArrayBuffer", what);
        return -1;
    }
    return 0;
}

static void pullinto_free(JSRuntime* rt, SPullInto* d)
{
    JS_FreeValueRT(rt, d->buffer);
    d->buffer = JS_UNDEFINED;
}

static void pending_free(JSRuntime* rt, SQ* q)
{
    uint32_t i;
    for (i = 0; i < q->len; i++)
        pullinto_free(rt, sq_at(q, i));
    sq_release(rt, q);
}

static void bq_free(JSRuntime* rt, SRBSC* c)
{
    uint32_t i;
    for (i = 0; i < c->q.len; i++)
        JS_FreeValueRT(rt, ((SByteEntry*)sq_at(&c->q, i))->buffer);
    sq_release(rt, &c->q);
    c->qtotal = 0;
}

void wst_rbsc_free(JSRuntime* rt, SRBSC* c)
{
    bq_free(rt, c);
    JS_FreeValueRT(rt, c->byob_request);
    sa_free(rt, &c->pull);
    sa_free(rt, &c->cancel);
    pending_free(rt, &c->pending);
}

static void rbsc_finalizer(JSRuntime* rt, JSValue val)
{
    wst_ctl_fin(rt, wst_opaque(val, WSTC_RBSC));
}

static void rbsc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    wst_ctl_mark(rt, wst_opaque(val, WSTC_RBSC), mf);
}

void wst_rbsc_mark(JSRuntime* rt, SRBSC* c, JS_MarkFunc* mf)
{
    uint32_t i;
    for (i = 0; i < c->q.len; i++)
        JS_MarkValue(rt, ((SByteEntry*)sq_at(&c->q, i))->buffer, mf);
    JS_MarkValue(rt, c->byob_request, mf);
    sa_mark(rt, &c->pull, mf);
    sa_mark(rt, &c->cancel, mf);
    for (i = 0; i < c->pending.len; i++)
        JS_MarkValue(rt, ((SPullInto*)sq_at(&c->pending, i))->buffer, mf);
}

static void byobreq_finalizer(JSRuntime* rt, JSValue val)
{
    SByobReq* r = wst_opaque(val, WSTC_BYOBREQ);
    if (!r)
        return;
    JS_FreeValueRT(rt, r->controller);
    JS_FreeValueRT(rt, r->view);
    js_free_rt(rt, r);
}

static void byobreq_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mf)
{
    SByobReq* r = wst_opaque(val, WSTC_BYOBREQ);
    if (!r)
        return;
    JS_MarkValue(rt, r->controller, mf);
    JS_MarkValue(rt, r->view, mf);
}

static int has_default_reader(SRS* s)
{
    return s->rd && !s->rd->byob;
}

static int has_byob_reader(SRS* s)
{
    return s->rd && s->rd->byob;
}

static double rbsc_desired_num(SRBSC* c)
{
    return c->hwm - c->qtotal;
}

static int rbsc_should_call_pull(SRBSC* c)
{
    SRS* s = c->s;
    if (s->state != RS_READABLE)
        return 0;
    if (c->close_requested)
        return 0;
    if (!c->started)
        return 0;
    if (has_default_reader(s) && wst_rs_num_read_requests(s) > 0)
        return 1;
    if (has_byob_reader(s) && wst_rs_num_read_requests(s) > 0)
        return 1;
    return rbsc_desired_num(c) > 0;
}

static void rbsc_clear_algorithms(JSContext* ctx, SRBSC* c)
{
    sa_clear(ctx, &c->pull);
    sa_clear(ctx, &c->cancel);
}

static void rbsc_invalidate_byob_request(JSContext* ctx, SRBSC* c)
{
    SByobReq* r;
    JSValue ro, co, vo;
    if (JS_IsNull(c->byob_request))
        return;
    ro = c->byob_request;
    r = wst_opaque(ro, WSTC_BYOBREQ);
    co = r->controller;
    vo = r->view;
    r->controller = JS_UNDEFINED;
    r->c = NULL;
    r->view = JS_NULL;
    c->byob_request = JS_NULL;
    JS_FreeValue(ctx, co);
    JS_FreeValue(ctx, vo);
    JS_FreeValue(ctx, ro);
}

static void rbsc_clear_pending(JSContext* ctx, SRBSC* c)
{
    rbsc_invalidate_byob_request(ctx, c);
    pending_free(JS_GetRuntime(ctx), &c->pending);
    sq_init(&c->pending, sizeof(SPullInto));
}

void wst_rbsc_error(JSContext* ctx, SRBSC* c, JSValueConst e)
{
    SRS* s = c->s;
    if (s->state != RS_READABLE)
        return;
    rbsc_clear_pending(ctx, c);
    bq_free(JS_GetRuntime(ctx), c);
    sq_init(&c->q, sizeof(SByteEntry));
    rbsc_clear_algorithms(ctx, c);
    wst_rs_error(ctx, s, e);
}

static void rbsc_error_exc(JSContext* ctx, SRBSC* c)
{
    JSValue e = JS_GetException(ctx);
    wst_rbsc_error(ctx, c, e);
    JS_Throw(ctx, e);
}

static void rbsc_shift_pending(SRBSC* c, SPullInto* out)
{
    sq_shift(&c->pending, out);
}

static int rbsc_enqueue_chunk(JSContext* ctx, SRBSC* c, JSValue buffer, double off, double len)
{
    SByteEntry* e = sq_push(ctx, &c->q);
    if (!e) {
        JS_FreeValue(ctx, buffer);
        return -1;
    }
    e->buffer = buffer;
    e->byte_offset = off;
    e->byte_length = len;
    c->qtotal += len;
    return 0;
}

static int rbsc_enqueue_cloned(JSContext* ctx, SRBSC* c, JSValueConst buffer, double off, double len)
{
    JSValue clone = clone_buffer(ctx, buffer, off, len);
    if (JS_IsException(clone)) {
        rbsc_error_exc(ctx, c);
        return -1;
    }
    return rbsc_enqueue_chunk(ctx, c, clone, 0, len);
}

static int rbsc_enqueue_detached_pull_into(JSContext* ctx, SRBSC* c)
{
    SPullInto d;
    SPullInto* first = sq_peek(&c->pending);
    if (first->bytes_filled > 0) {
        JSValue b = JS_DupValue(ctx, first->buffer);
        int r = rbsc_enqueue_cloned(ctx, c, b, first->byte_offset, first->bytes_filled);
        JS_FreeValue(ctx, b);
        if (r < 0)
            return -1;
    }
    rbsc_shift_pending(c, &d);
    pullinto_free(JS_GetRuntime(ctx), &d);
    return 0;
}

static JSValue rbsc_convert(JSContext* ctx, SPullInto* d)
{
    JSValue buf = wst_transfer_ab(ctx, d->buffer), v;
    if (JS_IsException(buf))
        return buf;
    v = wst_new_view(ctx, d->ta_kind, buf, d->byte_offset, d->bytes_filled / d->element_size);
    JS_FreeValue(ctx, buf);
    return v;
}

static void rbsc_commit(JSContext* ctx, SRS* s, SPullInto* d)
{
    int done = s->state == RS_CLOSED;
    JSValue view = rbsc_convert(ctx, d);
    if (JS_IsException(view)) {
        wst_drop_exc(ctx);
        return;
    }
    if (d->reader_type == RT_DEFAULT)
        wst_rs_fulfill_read_request(ctx, s, view, done);
    else
        wst_rs_fulfill_read_into_request(ctx, s, view, done);
    JS_FreeValue(ctx, view);
}

static void rbsc_handle_queue_drain(JSContext* ctx, SRBSC* c)
{
    if (c->qtotal == 0 && c->close_requested) {
        rbsc_clear_algorithms(ctx, c);
        wst_rs_close(ctx, c->s);
    } else {
        rbsc_call_pull_if_needed(ctx, c);
    }
}

static int rbsc_fill_from_queue(JSContext* ctx, SRBSC* c, SPullInto* d)
{
    double max_copy = c->qtotal < d->byte_length - d->bytes_filled ? c->qtotal : d->byte_length - d->bytes_filled;
    double max_filled = d->bytes_filled + max_copy;
    double total = max_copy;
    double rem = fmod(max_filled, d->element_size);
    double aligned = max_filled - rem;
    int ready = 0;
    if (aligned >= d->minimum_fill) {
        total = aligned - d->bytes_filled;
        ready = 1;
    }
    while (total > 0) {
        SByteEntry* head = sq_peek(&c->q);
        double n = total < head->byte_length ? total : head->byte_length;
        copy_bytes(ctx, d->buffer, d->byte_offset + d->bytes_filled, head->buffer, head->byte_offset, n);
        if (head->byte_length == n) {
            SByteEntry e;
            sq_shift(&c->q, &e);
            JS_FreeValue(ctx, e.buffer);
        } else {
            head->byte_offset += n;
            head->byte_length -= n;
        }
        c->qtotal -= n;
        d->bytes_filled += n;
        total -= n;
    }
    return ready;
}

static void rbsc_fill_read_request(JSContext* ctx, SRBSC* c, SReadReq* rr)
{
    SByteEntry e;
    JSValue view;
    sq_shift(&c->q, &e);
    c->qtotal -= e.byte_length;
    rbsc_handle_queue_drain(ctx, c);
    view = wst_new_u8(ctx, e.buffer, e.byte_offset, e.byte_length);
    JS_FreeValue(ctx, e.buffer);
    if (JS_IsException(view)) {
        JSValue ex = JS_GetException(ctx);
        wst_rr_error(ctx, rr, ex);
        JS_FreeValue(ctx, ex);
        return;
    }
    wst_rr_chunk(ctx, rr, view);
    JS_FreeValue(ctx, view);
}

static void rbsc_process_pull_intos(JSContext* ctx, SRBSC* c, SQ* filled)
{
    sq_init(filled, sizeof(SPullInto));
    while (c->pending.len > 0) {
        SPullInto* d;
        if (c->qtotal == 0)
            break;
        d = sq_peek(&c->pending);
        if (rbsc_fill_from_queue(ctx, c, d)) {
            SPullInto* slot = sq_push(ctx, filled);
            if (!slot)
                break;
            rbsc_shift_pending(c, slot);
        }
    }
}

static void commit_all(JSContext* ctx, SRS* s, SQ* filled)
{
    SPullInto d;
    while (filled->len) {
        sq_shift(filled, &d);
        rbsc_commit(ctx, s, &d);
        pullinto_free(JS_GetRuntime(ctx), &d);
    }
    sq_release(JS_GetRuntime(ctx), filled);
}

static void rbsc_process_read_requests(JSContext* ctx, SRBSC* c)
{
    SReader* r = c->s->rd;
    SReadReq rr;
    while (r->reqs.len > 0) {
        if (c->qtotal == 0)
            return;
        sq_shift(&r->reqs, &rr);
        rbsc_fill_read_request(ctx, c, &rr);
    }
}

int wst_rbsc_enqueue(JSContext* ctx, SRBSC* c, JSValueConst chunk)
{
    SRS* s = c->s;
    WVInfo vi;
    JSValue transferred, pin;
    SQ filled;
    int r;

    if (c->close_requested || s->state != RS_READABLE)
        return 0;
    if (wst_view_info(ctx, chunk, &vi) <= 0)
        return -1;
    if (wst_is_detached(vi.buffer)) {
        JS_FreeValue(ctx, vi.buffer);
        wst_throw(ctx, 0, "chunk's buffer is detached");
        return -1;
    }
    transferred = wst_transfer_ab(ctx, vi.buffer);
    JS_FreeValue(ctx, vi.buffer);
    if (JS_IsException(transferred))
        return -1;
    pin = JS_DupValue(ctx, c->s->obj);
    if (c->pending.len > 0) {
        SPullInto* first = sq_peek(&c->pending);
        JSValue nb;
        if (wst_is_detached(first->buffer)) {
            wst_throw(ctx, 0, "the BYOB request's buffer has been detached");
            goto fail;
        }
        rbsc_invalidate_byob_request(ctx, c);
        first = sq_peek(&c->pending);
        nb = wst_transfer_ab(ctx, first->buffer);
        if (JS_IsException(nb))
            goto fail;
        JS_FreeValue(ctx, first->buffer);
        first->buffer = nb;
        if (first->reader_type == RT_NONE && rbsc_enqueue_detached_pull_into(ctx, c) < 0)
            goto fail;
    }
    if (has_default_reader(s)) {
        rbsc_process_read_requests(ctx, c);
        if (wst_rs_num_read_requests(s) == 0) {
            r = rbsc_enqueue_chunk(ctx, c, transferred, vi.off, vi.len);
            transferred = JS_UNDEFINED;
            if (r < 0)
                goto fail;
        } else {
            JSValue view;
            if (c->pending.len > 0) {
                SPullInto d;
                rbsc_shift_pending(c, &d);
                pullinto_free(JS_GetRuntime(ctx), &d);
            }
            view = wst_new_u8(ctx, transferred, vi.off, vi.len);
            if (JS_IsException(view))
                goto fail;
            wst_rs_fulfill_read_request(ctx, s, view, 0);
            JS_FreeValue(ctx, view);
        }
    } else if (has_byob_reader(s)) {
        r = rbsc_enqueue_chunk(ctx, c, transferred, vi.off, vi.len);
        transferred = JS_UNDEFINED;
        if (r < 0)
            goto fail;
        rbsc_process_pull_intos(ctx, c, &filled);
        commit_all(ctx, s, &filled);
    } else {
        r = rbsc_enqueue_chunk(ctx, c, transferred, vi.off, vi.len);
        transferred = JS_UNDEFINED;
        if (r < 0)
            goto fail;
    }
    JS_FreeValue(ctx, transferred);
    rbsc_call_pull_if_needed(ctx, c);
    JS_FreeValue(ctx, pin);
    return 0;
fail:
    JS_FreeValue(ctx, transferred);
    JS_FreeValue(ctx, pin);
    return -1;
}

int wst_rbsc_close(JSContext* ctx, SRBSC* c)
{
    SRS* s = c->s;
    if (c->close_requested || s->state != RS_READABLE)
        return 0;
    if (c->qtotal > 0) {
        c->close_requested = 1;
        return 0;
    }
    if (c->pending.len > 0) {
        SPullInto* first = sq_peek(&c->pending);
        if (fmod(first->bytes_filled, first->element_size) != 0) {
            JSValue e = wst_err(ctx, 0, "insufficient bytes to fill elements in the given buffer");
            wst_rbsc_error(ctx, c, e);
            JS_Throw(ctx, e);
            return -1;
        }
    }
    rbsc_clear_algorithms(ctx, c);
    wst_rs_close(ctx, s);
    return 0;
}

void wst_rbsc_pull_done(JSContext* ctx, SRBSC* c, int is_reject, JSValueConst arg)
{
    if (is_reject) {
        wst_rbsc_error(ctx, c, arg);
        return;
    }
    c->pulling = 0;
    if (c->pull_again) {
        c->pull_again = 0;
        rbsc_call_pull_if_needed(ctx, c);
    }
}

static void rbsc_call_pull_if_needed(JSContext* ctx, SRBSC* c)
{
    JSValue pin;
    JSValueConst arg;
    if (!rbsc_should_call_pull(c))
        return;
    if (c->pulling) {
        c->pull_again = 1;
        return;
    }
    c->pulling = 1;
    pin = JS_DupValue(ctx, c->s->obj);
    arg = sa_kind(&c->pull) == SA_JS ? wst_rs_ctl(ctx, c->s) : JS_UNDEFINED;
    if (JS_IsException(arg)) {
        JSValue e = JS_GetException(ctx);
        wst_rbsc_error(ctx, c, e);
        JS_FreeValue(ctx, e);
    } else {
        sa_call_upon(ctx, &c->pull, 1, &arg, c->s->obj);
    }
    JS_FreeValue(ctx, pin);
}

void wst_rbsc_pull_steps(JSContext* ctx, SRBSC* c, SReadReq* rr)
{
    SRS* s = c->s;
    if (c->qtotal > 0) {
        rbsc_fill_read_request(ctx, c, rr);
        return;
    }
    if (c->aacs_set) {
        WstRt* w = wst_rt(ctx);
        if (!w)
            return;
        JSValue n = JS_NewFloat64(ctx, c->aacs), buffer;
        SPullInto* d;
        buffer = JS_CallConstructor(ctx, w->ab_ctor, 1, (JSValueConst*)&n);
        if (JS_IsException(buffer)) {
            JSValue e;
            if (JS_HasUncatchableException(ctx)) {
                JS_FreeValue(ctx, rr->a);
                JS_FreeValue(ctx, rr->b);
                return;
            }
            e = JS_GetException(ctx);
            wst_rr_error(ctx, rr, e);
            JS_FreeValue(ctx, e);
            return;
        }
        d = sq_push(ctx, &c->pending);
        if (!d) {
            JS_FreeValue(ctx, buffer);
            return;
        }
        d->buffer = buffer;
        d->buffer_byte_length = c->aacs;
        d->byte_offset = 0;
        d->byte_length = c->aacs;
        d->bytes_filled = 0;
        d->minimum_fill = 1;
        d->element_size = 1;
        d->ta_kind = JS_TYPED_ARRAY_UINT8;
        d->reader_type = RT_DEFAULT;
    }
    wst_rs_add_read_request(ctx, s, rr);
    rbsc_call_pull_if_needed(ctx, c);
}

JSValue wst_rbsc_cancel_steps(JSContext* ctx, SRBSC* c, JSValueConst reason)
{
    JSValue r;
    rbsc_clear_pending(ctx, c);
    bq_free(JS_GetRuntime(ctx), c);
    sq_init(&c->q, sizeof(SByteEntry));
    r = sa_call(ctx, &c->cancel, 1, &reason);
    rbsc_clear_algorithms(ctx, c);
    return r;
}

void wst_rbsc_release_steps(JSContext* ctx, SRBSC* c)
{
    SPullInto first, d;
    if (c->pending.len == 0)
        return;
    rbsc_shift_pending(c, &first);
    first.reader_type = RT_NONE;
    while (c->pending.len) {
        rbsc_shift_pending(c, &d);
        pullinto_free(JS_GetRuntime(ctx), &d);
    }
    sq_release(JS_GetRuntime(ctx), &c->pending);
    sq_init(&c->pending, sizeof(SPullInto));
    *(SPullInto*)sq_push(ctx, &c->pending) = first;
}

static void rbsc_pull_into(JSContext* ctx, SRBSC* c, JSValueConst view, double min, SReadReq* rr)
{
    SRS* s = c->s;
    WVInfo vi;
    SPullInto d, *slot;
    JSValue buffer, e;

    wst_view_info(ctx, view, &vi);
    buffer = wst_transfer_ab(ctx, vi.buffer);
    JS_FreeValue(ctx, vi.buffer);
    if (JS_IsException(buffer)) {
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, rr->a);
            JS_FreeValue(ctx, rr->b);
            return;
        }
        e = JS_GetException(ctx);
        wst_rr_error(ctx, rr, e);
        JS_FreeValue(ctx, e);
        return;
    }
    d.buffer = buffer;
    d.buffer_byte_length = wst_ab_len(ctx, buffer);
    d.byte_offset = vi.off;
    d.byte_length = vi.len;
    d.bytes_filled = 0;
    d.minimum_fill = min * vi.esize;
    d.element_size = vi.esize;
    d.ta_kind = vi.type;
    d.reader_type = RT_BYOB;
    if (c->pending.len > 0) {
        slot = sq_push(ctx, &c->pending);
        if (slot)
            *slot = d;
        wst_rs_add_read_request(ctx, s, rr);
        return;
    }
    if (s->state == RS_CLOSED) {
        JSValue v = wst_new_view(ctx, d.ta_kind, d.buffer, d.byte_offset, 0);
        pullinto_free(JS_GetRuntime(ctx), &d);
        wst_rr_close(ctx, rr, v);
        JS_FreeValue(ctx, v);
        return;
    }
    if (c->qtotal > 0) {
        if (rbsc_fill_from_queue(ctx, c, &d)) {
            JSValue fv = rbsc_convert(ctx, &d);
            pullinto_free(JS_GetRuntime(ctx), &d);
            rbsc_handle_queue_drain(ctx, c);
            wst_rr_chunk(ctx, rr, fv);
            JS_FreeValue(ctx, fv);
            return;
        }
        if (c->close_requested) {
            e = wst_err(ctx, 0, "insufficient bytes to fill elements in the given buffer");
            pullinto_free(JS_GetRuntime(ctx), &d);
            wst_rbsc_error(ctx, c, e);
            wst_rr_error(ctx, rr, e);
            JS_FreeValue(ctx, e);
            return;
        }
    }
    slot = sq_push(ctx, &c->pending);
    if (slot)
        *slot = d;
    wst_rs_add_read_request(ctx, s, rr);
    rbsc_call_pull_if_needed(ctx, c);
}

static void rbsc_respond_internal(JSContext* ctx, SRBSC* c, double bytes_written)
{
    SRS* s = c->s;
    SPullInto* first;
    SQ filled;
    JSValue pin = JS_DupValue(ctx, s->obj);
    rbsc_invalidate_byob_request(ctx, c);
    first = sq_peek(&c->pending);
    if (s->state == RS_CLOSED) {
        if (first->reader_type == RT_NONE) {
            SPullInto d;
            rbsc_shift_pending(c, &d);
            pullinto_free(JS_GetRuntime(ctx), &d);
        }
        if (has_byob_reader(s)) {
            int n = wst_rs_num_read_requests(s);
            sq_init(&filled, sizeof(SPullInto));
            while ((int)filled.len < n && c->pending.len) {
                SPullInto* slot = sq_push(ctx, &filled);
                if (!slot)
                    break;
                rbsc_shift_pending(c, slot);
            }
            commit_all(ctx, s, &filled);
        }
    } else {
        first->bytes_filled += bytes_written;
        if (first->reader_type == RT_NONE) {
            if (rbsc_enqueue_detached_pull_into(ctx, c) == 0) {
                rbsc_process_pull_intos(ctx, c, &filled);
                commit_all(ctx, s, &filled);
            }
        } else if (first->bytes_filled >= first->minimum_fill) {
            SPullInto d;
            double rem;
            int ok = 0;
            rbsc_shift_pending(c, &d);
            rem = fmod(d.bytes_filled, d.element_size);
            if (rem > 0)
                ok = rbsc_enqueue_cloned(ctx, c, d.buffer, d.byte_offset + d.bytes_filled - rem, rem);
            if (ok == 0) {
                d.bytes_filled -= rem;
                rbsc_process_pull_intos(ctx, c, &filled);
                rbsc_commit(ctx, s, &d);
                commit_all(ctx, s, &filled);
            } else {
                wst_drop_exc(ctx);
            }
            pullinto_free(JS_GetRuntime(ctx), &d);
        }
    }
    rbsc_call_pull_if_needed(ctx, c);
    JS_FreeValue(ctx, pin);
}

int wst_rbsc_respond(JSContext* ctx, SRBSC* c, double bytes_written)
{
    SPullInto* first = sq_peek(&c->pending);
    JSValue nb;
    if (c->s->state == RS_CLOSED) {
        if (bytes_written != 0) {
            wst_throw(ctx, 0, "bytesWritten must be 0 when calling respond() on a closed stream");
            return -1;
        }
    } else {
        if (bytes_written == 0) {
            wst_throw(ctx, 0, "bytesWritten must be greater than 0 when calling respond() on a readable stream");
            return -1;
        }
        if (first->bytes_filled + bytes_written > first->byte_length) {
            wst_throw(ctx, 1, "bytesWritten out of range");
            return -1;
        }
    }
    nb = wst_transfer_ab(ctx, first->buffer);
    if (JS_IsException(nb))
        return -1;
    first = sq_peek(&c->pending);
    JS_FreeValue(ctx, first->buffer);
    first->buffer = nb;
    rbsc_respond_internal(ctx, c, bytes_written);
    return 0;
}

int wst_rbsc_respond_with_new_view(JSContext* ctx, SRBSC* c, JSValueConst view)
{
    WVInfo vi;
    SPullInto* first;
    JSValue nb;
    double n;
    if (wst_view_info(ctx, view, &vi) <= 0)
        return -1;
    first = sq_peek(&c->pending);
    if (c->s->state == RS_CLOSED) {
        if (vi.len != 0) {
            wst_throw(ctx, 0, "the view's length must be 0 when calling respondWithNewView() on a closed stream");
            goto fail;
        }
    } else if (vi.len == 0) {
        wst_throw(ctx, 0, "the view's length must be greater than 0 when calling respondWithNewView() on a readable stream");
        goto fail;
    }
    if (first->byte_offset + first->bytes_filled != vi.off) {
        wst_throw(ctx, 1, "the region specified by view does not match byobRequest");
        goto fail;
    }
    if (first->buffer_byte_length != wst_ab_len(ctx, vi.buffer)) {
        wst_throw(ctx, 1, "the buffer of view has different capacity than byobRequest");
        goto fail;
    }
    if (first->bytes_filled + vi.len > first->byte_length) {
        wst_throw(ctx, 1, "the region specified by view is larger than byobRequest");
        goto fail;
    }
    n = vi.len;
    nb = wst_transfer_ab(ctx, vi.buffer);
    JS_FreeValue(ctx, vi.buffer);
    if (JS_IsException(nb))
        return -1;
    first = sq_peek(&c->pending);
    JS_FreeValue(ctx, first->buffer);
    first->buffer = nb;
    rbsc_respond_internal(ctx, c, n);
    return 0;
fail:
    JS_FreeValue(ctx, vi.buffer);
    return -1;
}

JSValue wst_rbsc_get_byob_request(JSContext* ctx, SRBSC* c)
{
    if (JS_IsNull(c->byob_request) && c->pending.len > 0) {
        SPullInto* first = sq_peek(&c->pending);
        SByobReq* r;
        JSValue view = wst_new_u8(ctx, first->buffer, first->byte_offset + first->bytes_filled, first->byte_length - first->bytes_filled);
        JSValue obj, co = wst_rs_ctl(ctx, c->s);
        if (JS_IsException(co)) {
            JS_FreeValue(ctx, view);
            return co;
        }
        if (JS_IsException(view))
            return view;
        obj = wst_new_obj(ctx, WSTC_BYOBREQ, JS_UNDEFINED, sizeof(SByobReq), (void**)&r);
        if (JS_IsException(obj)) {
            JS_FreeValue(ctx, view);
            return obj;
        }
        r->controller = JS_DupValue(ctx, co);
        r->c = c;
        r->view = view;
        c->byob_request = obj;
    }
    return JS_DupValue(ctx, c->byob_request);
}

JSValue wst_rbsc_desired(JSContext* ctx, SRBSC* c)
{
    if (c->s->state == RS_ERRORED)
        return JS_NULL;
    if (c->s->state == RS_CLOSED)
        return JS_NewInt32(ctx, 0);
    return JS_NewFloat64(ctx, rbsc_desired_num(c));
}

static JSValue rbsc_start_rx(JSContext* ctx, JSValueConst data, int magic, int is_reject, JSValueConst arg)
{
    SRS* s = wst_opaque(data, WSTC_RS);
    SRBSC* c = s->c;
    (void)magic;
    if (is_reject) {
        wst_rbsc_error(ctx, c, arg);
        return JS_UNDEFINED;
    }
    c->started = 1;
    rbsc_call_pull_if_needed(ctx, c);
    return JS_UNDEFINED;
}

static JSValue rbsc_start_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)argc;
    return rbsc_start_rx(ctx, argv[0], 0, 0, argv[1]);
}

int wst_rbsc_setup(JSContext* ctx, SRS* s, JSValueConst start_fn, JSValueConst start_self, JSValueConst start_value,
    SAlgo* pull, SAlgo* cancel, double hwm, double aacs, int aacs_set)
{
    SRBSC* c;
    JSValue cobj, r;
    int i;

    c = js_mallocz(ctx, sizeof(SRBSC));
    if (!c) {
        sa_clear(ctx, pull);
        sa_clear(ctx, cancel);
        return -1;
    }
    c->obj = JS_UNDEFINED;
    c->stream = JS_UNDEFINED;
    c->s = s;
    sq_init(&c->q, sizeof(SByteEntry));
    sq_init(&c->pending, sizeof(SPullInto));
    c->byob_request = JS_NULL;
    c->hwm = hwm;
    c->pull = *pull;
    c->cancel = *cancel;
    sa_init(pull);
    sa_init(cancel);
    c->aacs = aacs;
    c->aacs_set = aacs_set;
    s->c = c;
    s->bytes = 1;
    if (JS_IsUndefined(start_fn)) {
        r = JS_DupValue(ctx, start_value);
    } else {
        cobj = wst_rs_ctl(ctx, s);
        if (JS_IsException(cobj))
            return -1;
        r = JS_Call(ctx, start_fn, start_self, 1, (JSValueConst*)&cobj);
        if (JS_IsException(r))
            return -1;
    }
    i = wst_upon_start(ctx, r, rbsc_start_job, rbsc_start_rx, s->obj);
    JS_FreeValue(ctx, r);
    return i < 0 ? -1 : 0;
}

JSValue wst_rbs_create(JSContext* ctx, JSValueConst start_value, SAlgo* pull, SAlgo* cancel, SRS** out)
{
    SRS* s;
    JSValue obj = wst_rs_new(ctx, JS_UNDEFINED, &s);
    if (JS_IsException(obj)) {
        sa_clear(ctx, pull);
        sa_clear(ctx, cancel);
        return obj;
    }
    if (wst_rbsc_setup(ctx, s, JS_UNDEFINED, JS_UNDEFINED, start_value, pull, cancel, 0, 0, 0) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (out)
        *out = s;
    return obj;
}

JSValue wst_byob_read_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SReader* r = wst_opaque(this_val, WSTC_RSBR);
    JSValueConst view = argc > 0 ? argv[0] : JS_UNDEFINED;
    WVInfo vi;
    JSValue d, mv, p;
    double min = 1;
    SReadReq rr;
    SRS* s;

    vi.buffer = JS_UNDEFINED;
    if (!r) {
        wst_throw(ctx, 0, "this is not a ReadableStreamBYOBReader");
        goto fail;
    }
    if (wst_require_view(ctx, view, "read: view", &vi) < 0)
        goto fail;
    if (wst_to_dict(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "options", &d) < 0)
        goto fail;
    mv = wst_dict_get(ctx, d, WSA_min);
    JS_FreeValue(ctx, d);
    if (JS_IsException(mv))
        goto fail;
    if (!JS_IsUndefined(mv)) {
        int rc = wst_enforce_ull(ctx, mv, "options.min", &min);
        JS_FreeValue(ctx, mv);
        if (rc < 0)
            goto fail;
    }
    if (vi.len == 0) {
        wst_throw(ctx, 0, "read: view must have a non-zero byteLength");
        goto fail;
    }
    if (wst_ab_len(ctx, vi.buffer) == 0) {
        wst_throw(ctx, 0, "read: view's buffer must have a non-zero byteLength");
        goto fail;
    }
    if (wst_is_detached(vi.buffer)) {
        wst_throw(ctx, 0, "read: view's buffer has been detached");
        goto fail;
    }
    if (min == 0) {
        wst_throw(ctx, 0, "options.min must be greater than 0");
        goto fail;
    }
    if (vi.type != JS_VIEW_TYPE_DATAVIEW) {
        if (min > vi.length) {
            wst_throw(ctx, 1, "options.min must be <= view.length");
            goto fail;
        }
    } else if (min > vi.len) {
        wst_throw(ctx, 1, "options.min must be <= view.byteLength");
        goto fail;
    }
    if (!r->s) {
        wst_throw(ctx, 0, "the reader has no stream (lock released)");
        goto fail;
    }
    JS_FreeValue(ctx, vi.buffer);
    p = JS_NewPromisePending(ctx);
    if (JS_IsException(p))
        return p;
    rr.ops = &wst_rr_promise_ops;
    rr.a = JS_DupValue(ctx, p);
    rr.b = JS_UNDEFINED;
    s = r->s;
    s->disturbed = 1;
    if (s->state == RS_ERRORED)
        wst_rr_error(ctx, &rr, s->stored_error);
    else
        rbsc_pull_into(ctx, s->c, view, min, &rr);
    return wst_ret(ctx, p);
fail:
    JS_FreeValue(ctx, vi.buffer);
    return wst_reject_exc(ctx);
}

static SRBSC* rbsc_this(JSContext* ctx, JSValueConst v)
{
    SRBSC* c = wst_opaque(v, WSTC_RBSC);
    if (!c)
        wst_throw(ctx, 0, "this is not a ReadableByteStreamController");
    return c;
}

static JSValue rbsc_get_byob_request_m(JSContext* ctx, JSValueConst this_val)
{
    SRBSC* c = rbsc_this(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return wst_rbsc_get_byob_request(ctx, c);
}

static JSValue rbsc_get_desired_m(JSContext* ctx, JSValueConst this_val)
{
    SRBSC* c = rbsc_this(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    return wst_rbsc_desired(ctx, c);
}

static JSValue rbsc_close_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRBSC* c = rbsc_this(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    if (c->close_requested)
        return wst_throw(ctx, 0, "the stream has already been closed");
    if (c->s->state != RS_READABLE)
        return wst_throw(ctx, 0, "the stream is not readable");
    if (wst_rbsc_close(ctx, c) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue rbsc_enqueue_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRBSC* c = rbsc_this(ctx, this_val);
    JSValueConst chunk = argc > 0 ? argv[0] : JS_UNDEFINED;
    WVInfo vi;
    double blen;
    if (!c)
        return JS_EXCEPTION;
    if (wst_require_view(ctx, chunk, "enqueue: chunk", &vi) < 0)
        return JS_EXCEPTION;
    blen = wst_ab_len(ctx, vi.buffer);
    JS_FreeValue(ctx, vi.buffer);
    if (vi.len == 0)
        return wst_throw(ctx, 0, "enqueue: chunk must have a non-zero byteLength");
    if (blen == 0)
        return wst_throw(ctx, 0, "enqueue: chunk's buffer must have a non-zero byteLength");
    if (c->close_requested)
        return wst_throw(ctx, 0, "the stream is closed or draining");
    if (c->s->state != RS_READABLE)
        return wst_throw(ctx, 0, "the stream is not readable");
    if (wst_rbsc_enqueue(ctx, c, chunk) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue rbsc_error_m(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SRBSC* c = rbsc_this(ctx, this_val);
    if (!c)
        return JS_EXCEPTION;
    wst_rbsc_error(ctx, c, argc > 0 ? argv[0] : JS_UNDEFINED);
    return JS_UNDEFINED;
}

static SByobReq* byobreq_this(JSContext* ctx, JSValueConst v)
{
    SByobReq* r = wst_opaque(v, WSTC_BYOBREQ);
    if (!r)
        wst_throw(ctx, 0, "this is not a ReadableStreamBYOBRequest");
    return r;
}

static JSValue byobreq_get_view(JSContext* ctx, JSValueConst this_val)
{
    SByobReq* r = byobreq_this(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, r->view);
}

static JSValue byobreq_respond(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SByobReq* r = byobreq_this(ctx, this_val);
    WVInfo vi;
    double n;
    int det;
    if (!r)
        return JS_EXCEPTION;
    if (wst_enforce_ull(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "bytesWritten", &n) < 0)
        return JS_EXCEPTION;
    if (!r->c)
        return wst_throw(ctx, 0, "this BYOB request has been invalidated");
    if (wst_view_info(ctx, r->view, &vi) < 0)
        return JS_EXCEPTION;
    det = wst_is_detached(vi.buffer);
    JS_FreeValue(ctx, vi.buffer);
    if (det)
        return wst_throw(ctx, 0, "the BYOB request's buffer has been detached");
    if (wst_rbsc_respond(ctx, r->c, n) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue byobreq_respond_with_new_view(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SByobReq* r = byobreq_this(ctx, this_val);
    JSValueConst view = argc > 0 ? argv[0] : JS_UNDEFINED;
    WVInfo vi;
    int det;
    if (!r)
        return JS_EXCEPTION;
    if (wst_require_view(ctx, view, "respondWithNewView: view", &vi) < 0)
        return JS_EXCEPTION;
    det = wst_is_detached(vi.buffer);
    JS_FreeValue(ctx, vi.buffer);
    if (!r->c)
        return wst_throw(ctx, 0, "this BYOB request has been invalidated");
    if (det)
        return wst_throw(ctx, 0, "the given view's buffer has been detached");
    if (wst_rbsc_respond_with_new_view(ctx, r->c, view) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry rbsc_proto_funcs[] = {
    WST_GET("byobRequest", rbsc_get_byob_request_m),
    WST_GET("desiredSize", rbsc_get_desired_m),
    WST_FN("close", 0, rbsc_close_m),
    WST_FN("enqueue", 1, rbsc_enqueue_m),
    WST_FN("error", 0, rbsc_error_m),
    WST_TAG("ReadableByteStreamController"),
};

static const JSCFunctionListEntry byobreq_proto_funcs[] = {
    WST_GET("view", byobreq_get_view),
    WST_FN("respond", 1, byobreq_respond),
    WST_FN("respondWithNewView", 1, byobreq_respond_with_new_view),
    WST_TAG("ReadableStreamBYOBRequest"),
};

int wst_init_bytes(JSContext* ctx, JSValue* ctors)
{
    static const WstClassSpec specs[] = {
        { WSTC_RBSC, "ReadableByteStreamController", wst_illegal_ctor, 0, rbsc_proto_funcs, countof(rbsc_proto_funcs), NULL, 0, rbsc_finalizer, rbsc_mark },
        { WSTC_BYOBREQ, "ReadableStreamBYOBRequest", wst_illegal_ctor, 0, byobreq_proto_funcs, countof(byobreq_proto_funcs), NULL, 0, byobreq_finalizer, byobreq_mark },
    };
    size_t i;
    for (i = 0; i < countof(specs); i++)
        if (wst_register(ctx, &specs[i], &ctors[specs[i].k]) < 0)
            return -1;
    return 0;
}

void wst_byob_reader_read(JSContext* ctx, SReader* r, JSValueConst view, double min, SReadReq* rr)
{
    SRS* s = r->s;
    s->disturbed = 1;
    if (s->state == RS_ERRORED)
        wst_rr_error(ctx, rr, s->stored_error);
    else
        rbsc_pull_into(ctx, s->c, view, min, rr);
}

JSValue wst_byobreq_view(JSContext* ctx, JSValueConst req)
{
    SByobReq* r = wst_opaque(req, WSTC_BYOBREQ);
    return r ? JS_DupValue(ctx, r->view) : JS_UNDEFINED;
}
