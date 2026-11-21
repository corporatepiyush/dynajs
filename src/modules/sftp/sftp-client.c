#include "sftp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SFTP) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <math.h>
#include <stdio.h>

enum {
    K_STATUS = 1,
    K_ATTRS,
    K_NAME1,
    K_OPEN,
    K_OPENDIR,
    K_DREAD,
    K_DCLOSE,
    K_LSOPEN,
    K_LSREAD,
    K_LSCLOSE,
    K_FREAD,
    K_PREAD,
    K_FWRITE,
    K_PWRITE,
    K_FCLOSE,
    K_RFOPEN,
    K_WFOPEN
};

typedef struct sftp_cli sftp_cli_t;
typedef struct sftp_file sftp_file_t;

typedef struct {
    uint32_t id;
    uint64_t off;
    uint32_t len;
    int st;
    uint8_t* d;
    uint32_t n;
} ra_slot_t;

struct sftp_file {
    int refs;
    sftp_cli_t* cli;
    JSValue cliobj;
    sftp_file_t *next, *prev;
    uint8_t h[256];
    uint32_t hl;
    int kind;
    int open, closing, eof, has_obj;
    uint64_t rpos;
    ra_slot_t* ra;
    int nra;
    uint32_t rcode;
    char* rmsg;
    JSValue rres, rrej;
    sshb_t wq;
    size_t wq_off;
    uint64_t woff;
    int winfl;
    uint32_t wcode;
    char* wmsg;
    JSValue wres, wrej;
    JSValue cres, crej;
    sshb_t acc;
    JSValue pin;
};

typedef struct {
    uint32_t id;
    int kind;
    JSValue res, rej, aux;
    sftp_file_t* f;
    uint8_t h[256];
    uint32_t hl;
    uint32_t pflags;
    sshb_t data;
} sftp_req_t;

struct sftp_cli {
    ssh_chan_t* ch;
    JSContext* ctx;
    JSRuntime* rt;
    JSValue sshobj;
    JSValue ores, orej, cres;
    int state;
    uint32_t version, next_id;
    sftp_req_t** rq;
    int nrq, crq;
    sftp_file_t* files;
    sshb_t in;
    size_t off;
    sshb_t pk;
    uint32_t max_inflight, chunk;
    uint64_t requests, rbytes, wbytes;
    int max_seen;
    int swept;
    char err[200];
};

typedef struct {
    sftp_cli_t* cli;
    JSValue cliobj;
    uint8_t h[256];
    uint32_t hl;
    int open;
} sftp_dir_t;

static JSClassID sftp_cli_cid, sftp_file_cid, sftp_dir_cid;

static void fv(JSRuntime* rt, JSValue* v)
{
    if (!JS_IsUndefined(*v)) {
        JS_FreeValueRT(rt, *v);
        *v = JS_UNDEFINED;
    }
}

static void call1(JSContext* ctx, JSValueConst fn, JSValueConst arg)
{
    JSValue r;
    if (!JS_IsFunction(ctx, fn) || ssh_js_off())
        return;
    r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
}

static void settle(JSContext* ctx, JSRuntime* rt, JSValue* res, JSValue* rej, int ok, JSValue v)
{
    call1(ctx, ok ? *res : *rej, v);
    JS_FreeValue(ctx, v);
    fv(rt, res);
    fv(rt, rej);
}

static JSValue f_unpin_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

static void f_pin(sftp_file_t* f, JSContext* ctx, JSValueConst obj)
{
    if (JS_IsUndefined(f->pin))
        f->pin = JS_DupValue(ctx, obj);
}

static void f_idle(sftp_file_t* f, JSContext* ctx, JSRuntime* rt)
{
    JSValue v = f->pin;
    if (JS_IsUndefined(v) || !JS_IsUndefined(f->rres) || !JS_IsUndefined(f->wres) || !JS_IsUndefined(f->cres))
        return;
    if (JS_EnqueueJob(ctx, f_unpin_job, 1, &v) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    f->pin = JS_UNDEFINED;
    JS_FreeValueRT(rt, v);
}

static void f_settle(sftp_file_t* f, JSContext* ctx, JSRuntime* rt, JSValue* res, JSValue* rej, int ok, JSValue v)
{
    settle(ctx, rt, res, rej, ok, v);
    f_idle(f, ctx, rt);
}

static JSValue err_of(sftp_cli_t* c, uint32_t code, const char* msg)
{
    return sftp_error(c->ctx, code, msg, strlen(msg));
}

static JSValue rejected(JSContext* ctx, JSValue e)
{
    JSValue f[2], p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, e);
        return p;
    }
    call1(ctx, f[1], e);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, f[0]);
    JS_FreeValue(ctx, f[1]);
    return p;
}

static void f_free_raw(JSRuntime* rt, sftp_file_t* f)
{
    int i;
    for (i = 0; i < f->nra; i++)
        free(f->ra[i].d);
    free(f->ra);
    free(f->rmsg);
    free(f->wmsg);
    sshb_free(&f->wq);
    sshb_free(&f->acc);
    fv(rt, &f->rres);
    fv(rt, &f->rrej);
    fv(rt, &f->wres);
    fv(rt, &f->wrej);
    fv(rt, &f->cres);
    fv(rt, &f->crej);
    fv(rt, &f->cliobj);
    free(f);
}

static void f_unref(sftp_file_t* f)
{
    if (--f->refs > 0)
        return;
    if (f->prev)
        f->prev->next = f->next;
    else if (f->cli->files == f)
        f->cli->files = f->next;
    if (f->next)
        f->next->prev = f->prev;
    f_free_raw(f->cli->rt, f);
}

static sftp_file_t* f_new(sftp_cli_t* c, JSValueConst cliobj, int kind, const uint8_t* h, uint32_t hl)
{
    sftp_file_t* f = (sftp_file_t*)calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->ra = (ra_slot_t*)calloc(c->max_inflight, sizeof *f->ra);
    if (!f->ra) {
        free(f);
        return NULL;
    }
    f->refs = 1;
    f->cli = c;
    f->cliobj = kind == 0 ? JS_DupValue(c->ctx, cliobj) : JS_UNDEFINED;
    f->kind = kind;
    f->open = 1;
    memcpy(f->h, h, hl);
    f->hl = hl;
    f->rres = f->rrej = f->wres = f->wrej = f->cres = f->crej = JS_UNDEFINED;
    f->pin = JS_UNDEFINED;
    f->next = c->files;
    if (c->files)
        c->files->prev = f;
    c->files = f;
    return f;
}

static void req_free(sftp_cli_t* c, sftp_req_t* r)
{
    fv(c->rt, &r->res);
    fv(c->rt, &r->rej);
    fv(c->rt, &r->aux);
    sshb_free(&r->data);
    if (r->f)
        f_unref(r->f);
    free(r);
}

static sftp_req_t* req_new(sftp_cli_t* c, int kind, uint8_t type)
{
    sftp_req_t* r;
    if (c->nrq == c->crq) {
        int n = c->crq ? c->crq * 2 : 16;
        sftp_req_t** q = (sftp_req_t**)realloc(c->rq, (size_t)n * sizeof *q);
        if (!q)
            return NULL;
        c->rq = q;
        c->crq = n;
    }
    r = (sftp_req_t*)calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->res = r->rej = r->aux = JS_UNDEFINED;
    r->kind = kind;
    r->id = c->next_id++;
    if (sftp_pkt_begin(&c->pk, type) || sshb_u32(&c->pk, r->id)) {
        free(r);
        return NULL;
    }
    return r;
}

static int req_send(sftp_cli_t* c, sftp_req_t* r)
{
    if (sftp_send(c->ch, &c->pk) < 0) {
        req_free(c, r);
        return -1;
    }
    c->rq[c->nrq++] = r;
    c->requests++;
    if (c->nrq > c->max_seen)
        c->max_seen = c->nrq;
    return 0;
}

static sftp_req_t* req_take(sftp_cli_t* c, uint32_t id)
{
    int i;
    for (i = 0; i < c->nrq; i++)
        if (c->rq[i]->id == id) {
            sftp_req_t* r = c->rq[i];
            memmove(c->rq + i, c->rq + i + 1, (size_t)(c->nrq - i - 1) * sizeof *c->rq);
            c->nrq--;
            return r;
        }
    return NULL;
}

static void kick(sftp_cli_t* c)
{
    if (c->ch && c->ch->conn)
        ssh_chan_kick(c->ch);
}

static int ready(sftp_cli_t* c)
{
    return c->state == 2 && c->ch && c->ch->conn && !c->ch->sent_close;
}

static int put_handle(sftp_cli_t* c, const uint8_t* h, uint32_t hl)
{
    return sshb_str(&c->pk, h, hl);
}

static int put_u64(sshb_t* b, uint64_t v)
{
    return sshb_u32(b, (uint32_t)(v >> 32)) || sshb_u32(b, (uint32_t)v) ? -1 : 0;
}

static void f_send_close(sftp_file_t* f);

static int f_read_req(sftp_file_t* f, ra_slot_t* s)
{
    sftp_cli_t* c = f->cli;
    sftp_req_t* r = req_new(c, K_FREAD, SFTP_READ);
    if (!r || put_handle(c, f->h, f->hl) || put_u64(&c->pk, s->off) || sshb_u32(&c->pk, s->len)) {
        if (r)
            free(r);
        return -1;
    }
    r->f = f;
    f->refs++;
    s->id = r->id;
    s->st = 0;
    return req_send(c, r);
}

static void f_fill(sftp_file_t* f)
{
    sftp_cli_t* c = f->cli;
    while (!f->eof && !f->rmsg && f->open && !f->closing && ready(c) && f->nra < (int)c->max_inflight) {
        ra_slot_t* s = &f->ra[f->nra];
        memset(s, 0, sizeof *s);
        s->off = f->rpos;
        s->len = c->chunk;
        if (f_read_req(f, s) < 0) {
            f->rcode = SFTP_FAILURE;
            f->rmsg = strdup("cannot send a read request");
            return;
        }
        f->nra++;
        f->rpos += c->chunk;
    }
}

static void ra_pop(sftp_file_t* f)
{
    free(f->ra[0].d);
    memmove(f->ra, f->ra + 1, (size_t)(f->nra - 1) * sizeof *f->ra);
    f->nra--;
}

static void ra_clear(sftp_file_t* f)
{
    while (f->nra)
        ra_pop(f);
}

static void sink_done(sftp_file_t* f)
{
    if (f->open && !f->closing)
        f_send_close(f);
}

static void f_advance(sftp_file_t* f)
{
    sftp_cli_t* c = f->cli;
    JSContext* ctx = c->ctx;
    for (;;) {
        ra_slot_t* s;
        if (f->rmsg) {
            ra_clear(f);
            if (f->kind == 0 && !JS_IsUndefined(f->rrej))
                f_settle(f, ctx, c->rt, &f->rres, &f->rrej, 0, err_of(c, f->rcode, f->rmsg));
            else if (f->kind == 1)
                sink_done(f);
            return;
        }
        if (!f->nra) {
            if (f->eof) {
                if (f->kind == 0 && !JS_IsUndefined(f->rres))
                    f_settle(f, ctx, c->rt, &f->rres, &f->rrej, 1, JS_NULL);
                else if (f->kind == 1)
                    sink_done(f);
                return;
            }
            break;
        }
        s = &f->ra[0];
        if (s->st == 0)
            break;
        if (s->st == 2) {
            f->eof = 1;
            ra_clear(f);
            continue;
        }
        if (f->kind == 0 && JS_IsUndefined(f->rres))
            break;
        if (f->kind == 0) {
            JSValue u8 = dyn_nat_u8_copy(ctx, s->d, s->n);
            if (JS_IsException(u8))
                u8 = JS_GetException(ctx);
            f_settle(f, ctx, c->rt, &f->rres, &f->rrej, 1, u8);
        } else if (sshb_put(&f->acc, s->d, s->n)) {
            f->rcode = SFTP_FAILURE;
            f->rmsg = strdup("out of memory");
            continue;
        }
        if (s->n < s->len) {
            s->off += s->n;
            s->len -= s->n;
            free(s->d);
            s->d = NULL;
            s->n = 0;
            if (f_read_req(f, s) < 0) {
                f->rcode = SFTP_FAILURE;
                f->rmsg = strdup("cannot send a read request");
                continue;
            }
        } else
            ra_pop(f);
        if (f->kind == 0)
            break;
    }
    f_fill(f);
}

static void f_wpump(sftp_file_t* f)
{
    sftp_cli_t* c = f->cli;
    while (!f->wmsg && ready(c) && f->winfl < (int)c->max_inflight && f->wq.len > f->wq_off) {
        size_t n = f->wq.len - f->wq_off;
        sftp_req_t* r;
        if (n > c->chunk)
            n = c->chunk;
        r = req_new(c, K_FWRITE, SFTP_WRITE);
        if (!r || put_handle(c, f->h, f->hl) || put_u64(&c->pk, f->woff) || sshb_str(&c->pk, f->wq.p + f->wq_off, n)) {
            if (r)
                free(r);
            f->wcode = SFTP_FAILURE;
            f->wmsg = strdup("cannot send a write request");
            break;
        }
        r->f = f;
        f->refs++;
        if (req_send(c, r) < 0) {
            f->wcode = SFTP_FAILURE;
            f->wmsg = strdup("cannot send a write request");
            break;
        }
        f->woff += n;
        f->wq_off += n;
        f->winfl++;
        c->wbytes += n;
    }
    if (f->wmsg)
        f->wq_off = f->wq.len;
    if (f->wq_off == f->wq.len)
        f->wq.len = f->wq_off = 0;
    else if (f->wq_off > (1u << 20)) {
        memmove(f->wq.p, f->wq.p + f->wq_off, f->wq.len - f->wq_off);
        f->wq.len -= f->wq_off;
        f->wq_off = 0;
    }
}

static void f_wsettle(sftp_file_t* f)
{
    sftp_cli_t* c = f->cli;
    if (!JS_IsUndefined(f->wres) && (f->wmsg || f->wq.len == f->wq_off)) {
        if (f->wmsg)
            f_settle(f, c->ctx, c->rt, &f->wres, &f->wrej, 0, err_of(c, f->wcode, f->wmsg));
        else
            f_settle(f, c->ctx, c->rt, &f->wres, &f->wrej, 1, JS_UNDEFINED);
    }
    if (f->winfl == 0 && f->wq.len == f->wq_off && f->open && (f->closing == 1 || (f->kind == 2 && !f->closing)))
        f_send_close(f);
}

static void f_send_close(sftp_file_t* f)
{
    sftp_cli_t* c = f->cli;
    sftp_req_t* r;
    f->closing = 2;
    ra_clear(f);
    r = ready(c) ? req_new(c, K_FCLOSE, SFTP_CLOSE) : NULL;
    if (!r || put_handle(c, f->h, f->hl)) {
        if (r)
            free(r);
        f->open = 0;
        if (!JS_IsUndefined(f->crej))
            f_settle(f, c->ctx, c->rt, &f->cres, &f->crej, 0, err_of(c, SFTP_CONNECTION_LOST, c->err[0] ? c->err : "the SFTP session is closed"));
        return;
    }
    r->f = f;
    f->refs++;
    (void)req_send(c, r);
}

static void f_closed(sftp_file_t* f, uint32_t code, const char* msg)
{
    sftp_cli_t* c = f->cli;
    JSContext* ctx = c->ctx;
    f->open = 0;
    if (!JS_IsUndefined(f->rres))
        f_settle(f, ctx, c->rt, &f->rres, &f->rrej, 0, err_of(c, SFTP_FAILURE, "the file was closed"));
    if (JS_IsUndefined(f->crej))
        return;
    if (f->kind == 1 && f->rmsg)
        f_settle(f, ctx, c->rt, &f->cres, &f->crej, 0, err_of(c, f->rcode, f->rmsg));
    else if (f->wmsg)
        f_settle(f, ctx, c->rt, &f->cres, &f->crej, 0, err_of(c, f->wcode, f->wmsg));
    else if (code != SFTP_OK)
        f_settle(f, ctx, c->rt, &f->cres, &f->crej, 0, sftp_error(ctx, code, msg, strlen(msg)));
    else if (f->kind == 1) {
        JSValue u8 = dyn_nat_u8_copy(ctx, f->acc.p, f->acc.len);
        if (JS_IsException(u8))
            u8 = JS_GetException(ctx);
        f_settle(f, ctx, c->rt, &f->cres, &f->crej, 1, u8);
    } else
        f_settle(f, ctx, c->rt, &f->cres, &f->crej, 1, JS_UNDEFINED);
    if (f->kind != 0 && f->has_obj == 0 && f->refs > 0) {
        f->has_obj = -1;
        f_unref(f);
    }
}

static JSValue entry_js(sftp_cli_t* c, sshr_t* r)
{
    JSContext* ctx = c->ctx;
    size_t nn, ln;
    const uint8_t* n = sshr_str(r, &nn);
    const uint8_t* l = sshr_str(r, &ln);
    sftp_attr_t a;
    JSValue o;
    if (sftp_attr_get(r, &a) < 0)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "filename", JS_NewStringLen(ctx, (const char*)n, nn));
    JS_SetPropertyStr(ctx, o, "longname", JS_NewStringLen(ctx, (const char*)l, ln));
    JS_SetPropertyStr(ctx, o, "attrs", sftp_attr_js(ctx, &a));
    return o;
}

static int names_into(sftp_cli_t* c, sshr_t* r, JSValueConst arr)
{
    uint32_t i, n = sshr_u32(r);
    int64_t len = 0;
    if (r->err || n > 100000)
        return -1;
    {
        JSValue lv = JS_GetPropertyStr(c->ctx, arr, "length");
        int bad = JS_ToInt64(c->ctx, &len, lv);
        JS_FreeValue(c->ctx, lv);
        if (bad < 0)
            return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue e = entry_js(c, r);
        if (JS_IsException(e))
            return -1;
        JS_SetPropertyInt64(c->ctx, arr, len + i, e);
    }
    return 0;
}

static JSValue file_obj(sftp_cli_t* c, sftp_file_t* f)
{
    JSValue o = JS_NewObjectClass(c->ctx, (int)sftp_file_cid);
    if (JS_IsException(o))
        return o;
    JS_SetOpaque(o, f);
    f->has_obj = 1;
    return o;
}

static int dir_send(sftp_cli_t* c, int kind, uint8_t type, const uint8_t* h, uint32_t hl, sftp_req_t** out)
{
    sftp_req_t* r = req_new(c, kind, type);
    if (!r || put_handle(c, h, hl)) {
        if (r)
            free(r);
        return -1;
    }
    memcpy(r->h, h, hl);
    r->hl = hl;
    *out = r;
    return 0;
}

static void on_resp(sftp_cli_t* c, uint8_t t, sshr_t* rd)
{
    JSContext* ctx = c->ctx;
    uint32_t id = sshr_u32(rd), code = SFTP_OK;
    const uint8_t* msg = (const uint8_t*)"";
    size_t ml = 0;
    sftp_req_t* r = rd->err ? NULL : req_take(c, id);
    if (!r)
        return;
    if (t == SFTP_STATUS) {
        code = sshr_u32(rd);
        msg = sshr_str(rd, &ml);
        if (rd->err) {
            code = SFTP_BAD_MESSAGE;
            msg = (const uint8_t*)"malformed status";
            ml = strlen((const char*)msg);
        }
    }
#define FAIL_REQ()                                                                                 \
    do {                                                                                           \
        if (t == SFTP_STATUS)                                                                      \
            settle(ctx, c->rt, &r->res, &r->rej, 0, sftp_error(ctx, code, (const char*)msg, ml)); \
        else                                                                                       \
            settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_BAD_MESSAGE, "unexpected reply")); \
    } while (0)
    switch (r->kind) {
    case K_STATUS:
    case K_PWRITE:
    case K_DCLOSE:
        if (t == SFTP_STATUS && code == SFTP_OK)
            settle(ctx, c->rt, &r->res, &r->rej, 1, JS_UNDEFINED);
        else
            FAIL_REQ();
        break;
    case K_ATTRS:
        if (t == SFTP_ATTRS) {
            sftp_attr_t a;
            if (sftp_attr_get(rd, &a) == 0)
                settle(ctx, c->rt, &r->res, &r->rej, 1, sftp_attr_js(ctx, &a));
            else
                settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_BAD_MESSAGE, "malformed attributes"));
        } else
            FAIL_REQ();
        break;
    case K_NAME1:
        if (t == SFTP_NAME && sshr_u32(rd) >= 1) {
            size_t n;
            const uint8_t* p = sshr_str(rd, &n);
            if (!rd->err)
                settle(ctx, c->rt, &r->res, &r->rej, 1, JS_NewStringLen(ctx, (const char*)p, n));
            else
                settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_BAD_MESSAGE, "malformed name"));
        } else
            FAIL_REQ();
        break;
    case K_OPEN:
    case K_OPENDIR:
    case K_LSOPEN:
    case K_RFOPEN:
    case K_WFOPEN: {
        size_t hl = 0;
        const uint8_t* h = t == SFTP_HANDLE ? sshr_str(rd, &hl) : NULL;
        if (t != SFTP_HANDLE || rd->err || hl == 0 || hl > 256) {
            FAIL_REQ();
            break;
        }
        if (r->kind == K_OPENDIR) {
            sftp_dir_t* d = (sftp_dir_t*)calloc(1, sizeof *d);
            JSValue o = d ? JS_NewObjectClass(ctx, (int)sftp_dir_cid) : JS_EXCEPTION;
            if (JS_IsException(o)) {
                free(d);
                settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_FAILURE, "out of memory"));
                break;
            }
            d->cli = c;
            d->cliobj = JS_DupValue(ctx, c->ch->jsobj);
            memcpy(d->h, h, hl);
            d->hl = (uint32_t)hl;
            d->open = 1;
            JS_SetOpaque(o, d);
            settle(ctx, c->rt, &r->res, &r->rej, 1, o);
        } else if (r->kind == K_LSOPEN) {
            sftp_req_t* n;
            if (dir_send(c, K_LSREAD, SFTP_READDIR, h, (uint32_t)hl, &n) < 0) {
                settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_FAILURE, "cannot send a readdir request"));
                break;
            }
            n->res = r->res;
            n->rej = r->rej;
            n->aux = JS_NewArray(ctx);
            r->res = r->rej = JS_UNDEFINED;
            (void)req_send(c, n);
        } else {
            sftp_file_t* f = f_new(c, c->ch->jsobj, r->kind == K_OPEN ? 0 : r->kind == K_RFOPEN ? 1 : 2, h, (uint32_t)hl);
            if (!f) {
                settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_FAILURE, "out of memory"));
                break;
            }
            if (r->kind == K_OPEN) {
                JSValue o = file_obj(c, f);
                if (JS_IsException(o)) {
                    f_unref(f);
                    settle(ctx, c->rt, &r->res, &r->rej, 0, JS_GetException(ctx));
                    break;
                }
                settle(ctx, c->rt, &r->res, &r->rej, 1, o);
            } else {
                f->cres = r->res;
                f->crej = r->rej;
                r->res = r->rej = JS_UNDEFINED;
                if (f->kind == 1) {
                    f_fill(f);
                    f_advance(f);
                } else {
                    f->wq = r->data;
                    memset(&r->data, 0, sizeof r->data);
                    f_wpump(f);
                    f_wsettle(f);
                }
            }
        }
        break;
    }
    case K_DREAD:
        if (t == SFTP_STATUS && code == SFTP_EOF)
            settle(ctx, c->rt, &r->res, &r->rej, 1, JS_NULL);
        else if (t == SFTP_NAME) {
            JSValue arr = JS_NewArray(ctx);
            if (names_into(c, rd, arr) < 0) {
                JS_FreeValue(ctx, arr);
                settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_BAD_MESSAGE, "malformed directory listing"));
            } else
                settle(ctx, c->rt, &r->res, &r->rej, 1, arr);
        } else
            FAIL_REQ();
        break;
    case K_LSREAD: {
        sftp_req_t* n = NULL;
        int eof = t == SFTP_STATUS && code == SFTP_EOF;
        if (t == SFTP_NAME && names_into(c, rd, r->aux) == 0) {
            if (dir_send(c, K_LSREAD, SFTP_READDIR, r->h, r->hl, &n) == 0) {
                n->res = r->res;
                n->rej = r->rej;
                n->aux = r->aux;
                r->res = r->rej = r->aux = JS_UNDEFINED;
                (void)req_send(c, n);
            } else
                settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_FAILURE, "cannot send a readdir request"));
            break;
        }
        if (dir_send(c, K_LSCLOSE, SFTP_CLOSE, r->h, r->hl, &n) == 0) {
            if (eof) {
                n->res = r->res;
                n->rej = r->rej;
                n->aux = r->aux;
                r->res = r->rej = r->aux = JS_UNDEFINED;
            }
            (void)req_send(c, n);
        }
        if (!eof && !JS_IsUndefined(r->rej))
            FAIL_REQ();
        else if (eof && !JS_IsUndefined(r->res))
            settle(ctx, c->rt, &r->res, &r->rej, 1, JS_DupValue(ctx, r->aux));
        break;
    }
    case K_LSCLOSE:
        if (!JS_IsUndefined(r->res))
            settle(ctx, c->rt, &r->res, &r->rej, 1, JS_DupValue(ctx, r->aux));
        break;
    case K_PREAD:
        if (t == SFTP_DATA) {
            size_t n;
            const uint8_t* p = sshr_str(rd, &n);
            if (rd->err)
                settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_BAD_MESSAGE, "malformed data"));
            else {
                c->rbytes += n;
                settle(ctx, c->rt, &r->res, &r->rej, 1, dyn_nat_u8_copy(ctx, p, n));
            }
        } else if (t == SFTP_STATUS && code == SFTP_EOF)
            settle(ctx, c->rt, &r->res, &r->rej, 1, JS_NULL);
        else
            FAIL_REQ();
        break;
    case K_FREAD: {
        sftp_file_t* f = r->f;
        int i;
        for (i = 0; i < f->nra; i++)
            if (f->ra[i].id == id && f->ra[i].st == 0)
                break;
        if (i == f->nra)
            break;
        if (t == SFTP_DATA) {
            size_t n;
            const uint8_t* p = sshr_str(rd, &n);
            if (rd->err || n > f->ra[i].len) {
                f->rcode = SFTP_BAD_MESSAGE;
                f->rmsg = strdup("malformed or oversized data reply");
            } else if (n == 0) {
                f->ra[i].st = 2;
            } else {
                f->ra[i].d = (uint8_t*)malloc(n);
                if (!f->ra[i].d) {
                    f->rcode = SFTP_FAILURE;
                    f->rmsg = strdup("out of memory");
                } else {
                    memcpy(f->ra[i].d, p, n);
                    f->ra[i].n = (uint32_t)n;
                    f->ra[i].st = 1;
                    c->rbytes += n;
                }
            }
        } else if (t == SFTP_STATUS && code == SFTP_EOF)
            f->ra[i].st = 2;
        else if (!f->rmsg) {
            f->rcode = t == SFTP_STATUS ? code : SFTP_BAD_MESSAGE;
            f->rmsg = t == SFTP_STATUS ? strndup((const char*)msg, ml) : strdup("unexpected reply");
        }
        f_advance(f);
        break;
    }
    case K_FWRITE: {
        sftp_file_t* f = r->f;
        f->winfl--;
        if ((t != SFTP_STATUS || code != SFTP_OK) && !f->wmsg) {
            f->wcode = t == SFTP_STATUS ? code : SFTP_BAD_MESSAGE;
            f->wmsg = t == SFTP_STATUS ? strndup((const char*)msg, ml) : strdup("unexpected reply");
        }
        f_wpump(f);
        f_wsettle(f);
        break;
    }
    case K_FCLOSE:
        f_closed(r->f, t == SFTP_STATUS ? code : SFTP_BAD_MESSAGE, t == SFTP_STATUS ? (const char*)msg : "unexpected reply");
        break;
    default:
        break;
    }
#undef FAIL_REQ
    req_free(c, r);
}

static void cli_fail(sftp_cli_t* c, const char* why)
{
    JSContext* ctx = c->ctx;
    sftp_file_t* f;
    if (c->state == 3)
        return;
    if (!c->err[0])
        snprintf(c->err, sizeof c->err, "%s", why ? why : "the SFTP session closed");
    if (!JS_IsUndefined(c->orej))
        settle(ctx, c->rt, &c->ores, &c->orej, 0, err_of(c, SFTP_CONNECTION_LOST, c->err));
    c->state = 3;
    while (c->nrq) {
        sftp_req_t* r = c->rq[--c->nrq];
        if (!JS_IsUndefined(r->rej))
            settle(ctx, c->rt, &r->res, &r->rej, 0, err_of(c, SFTP_CONNECTION_LOST, c->err));
        if (r->f && r->kind == K_FWRITE)
            r->f->winfl--;
        req_free(c, r);
    }
    for (f = c->files; f; f = f->next) {
        f->open = 0;
        if (!f->wmsg) {
            f->wcode = SFTP_CONNECTION_LOST;
            f->wmsg = strdup(c->err);
        }
        ra_clear(f);
        if (!JS_IsUndefined(f->rrej))
            f_settle(f, ctx, c->rt, &f->rres, &f->rrej, 0, err_of(c, SFTP_CONNECTION_LOST, c->err));
        if (!JS_IsUndefined(f->wrej))
            f_settle(f, ctx, c->rt, &f->wres, &f->wrej, 0, err_of(c, SFTP_CONNECTION_LOST, c->err));
        if (!JS_IsUndefined(f->crej))
            f_settle(f, ctx, c->rt, &f->cres, &f->crej, 0, err_of(c, SFTP_CONNECTION_LOST, c->err));
    }
    for (f = c->files; f;) {
        sftp_file_t* nx = f->next;
        if (f->kind != 0 && f->has_obj == 0) {
            f->has_obj = -1;
            f_unref(f);
        }
        f = nx;
    }
}

static void cli_on_opened(ssh_chan_t* ch, const char* err)
{
    sftp_cli_t* c = (sftp_cli_t*)ch->opq;
    if (!c)
        return;
    if (err) {
        cli_fail(c, err);
        return;
    }
    if (sftp_pkt_begin(&c->pk, SFTP_INIT) || sshb_u32(&c->pk, 3) || sftp_send(ch, &c->pk) < 0) {
        cli_fail(c, "cannot send the SFTP init");
        return;
    }
    c->state = 1;
}

static void cli_on_data(ssh_chan_t* ch, int which, const uint8_t* p, size_t n)
{
    sftp_cli_t* c = (sftp_cli_t*)ch->opq;
    if (!c || which || c->state == 3)
        return;
    if (sshb_put(&c->in, p, n)) {
        cli_fail(c, "out of memory");
        ssh_chan_close(ch);
        return;
    }
    while (c->state != 3) {
        size_t avail = c->in.len - c->off;
        uint32_t L;
        sshr_t r;
        uint8_t t;
        if (avail < 4)
            break;
        L = ssh_be32(c->in.p + c->off);
        if (L < 1 || L > SFTP_CLI_MAX_MSG) {
            cli_fail(c, "the server sent a malformed or oversized SFTP packet");
            ssh_chan_close(ch);
            break;
        }
        if (avail < 4 + (size_t)L)
            break;
        sshr_init(&r, c->in.p + c->off + 4, L);
        c->off += 4 + (size_t)L;
        t = sshr_u8(&r);
        if (c->state == 1) {
            if (t != SFTP_VERSION) {
                cli_fail(c, "the server did not answer the SFTP init with a version");
                ssh_chan_close(ch);
                break;
            }
            c->version = sshr_u32(&r);
            if (r.err || c->version < 3) {
                cli_fail(c, "the server speaks an SFTP version older than 3");
                ssh_chan_close(ch);
                break;
            }
            c->state = 2;
            if (!JS_IsUndefined(c->ores)) {
                JSValue self = ch->jsobj;
                call1(c->ctx, c->ores, self);
                fv(c->rt, &c->ores);
                fv(c->rt, &c->orej);
            }
            continue;
        }
        on_resp(c, t, &r);
    }
    if (c->off == c->in.len)
        c->in.len = c->off = 0;
    else if (c->off > (64u << 10)) {
        memmove(c->in.p, c->in.p + c->off, c->in.len - c->off);
        c->in.len -= c->off;
        c->off = 0;
    }
    kick(c);
}

static void cli_on_eof(ssh_chan_t* ch)
{
    sftp_cli_t* c = (sftp_cli_t*)ch->opq;
    if (c)
        cli_fail(c, "the server ended the SFTP session");
    ssh_chan_close(ch);
}

static void cli_on_closed(ssh_chan_t* ch, const char* err)
{
    sftp_cli_t* c = (sftp_cli_t*)ch->opq;
    if (!c)
        return;
    cli_fail(c, err ? err : "the SFTP channel closed");
    if (!JS_IsUndefined(c->cres)) {
        call1(c->ctx, c->cres, JS_UNDEFINED);
        fv(c->rt, &c->cres);
    }
}

static const ssh_chan_ops_t cli_ops = { cli_on_opened, cli_on_data, cli_on_eof, NULL, cli_on_closed, NULL };

static void cli_sweep(JSContext* ctx, JSRuntime* rt, void* opaque)
{
    sftp_cli_t* c = (sftp_cli_t*)opaque;
    sftp_file_t* f;
    int i;
    JSValue* pins;
    int np = 0;
    (void)ctx;
    c->swept = 0;
    if (!c->err[0])
        snprintf(c->err, sizeof c->err, "the runtime is shutting down");
    for (f = c->files; f; f = f->next)
        np += !JS_IsUndefined(f->pin);
    pins = np ? (JSValue*)malloc((size_t)np * sizeof *pins) : NULL;
    np = 0;
    c->state = 3;
    for (i = 0; i < c->nrq; i++) {
        fv(rt, &c->rq[i]->res);
        fv(rt, &c->rq[i]->rej);
        fv(rt, &c->rq[i]->aux);
    }
    for (f = c->files; f; f = f->next) {
        fv(rt, &f->rres);
        fv(rt, &f->rrej);
        fv(rt, &f->wres);
        fv(rt, &f->wrej);
        fv(rt, &f->cres);
        fv(rt, &f->crej);
        if (pins && !JS_IsUndefined(f->pin)) {
            pins[np++] = f->pin;
            f->pin = JS_UNDEFINED;
        }
    }
    fv(rt, &c->ores);
    fv(rt, &c->orej);
    fv(rt, &c->cres);
    for (i = 0; i < np; i++)
        JS_FreeValueRT(rt, pins[i]);
    free(pins);
}

static sftp_cli_t* cli_get(JSContext* ctx, JSValueConst v)
{
    return (sftp_cli_t*)JS_GetOpaque2(ctx, v, sftp_cli_cid);
}

static const char* const open_keys[] = { "dyna" };
static const char* const open_meta_keys[] = { "maxInflight", "chunkBytes" };

static int meta_num(JSContext* ctx, JSValueConst o, const char* k, double lo, double hi, uint32_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "SFTP: dyna.%s must be a number", k);
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    if (!(d >= lo && d <= hi) || floor(d) != d) {
        JS_ThrowRangeError(ctx, "SFTP: dyna.%s must be an integer in %.0f..%.0f", k, lo, hi);
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static JSValue cli_open(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_conn_t* conn;
    sftp_cli_t* c;
    JSValue obj, p, f[2];
    uint32_t maxi = 64, chunk = 32768;
    (void)this_val;
    conn = argc > 0 ? ssh_client_of(ctx, argv[0]) : NULL;
    if (!conn) {
        if (argc > 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_ThrowTypeError(ctx, "SFTP: open expects a connected SSHClient");
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JSValue m;
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "SFTP: options must be an object");
        if (dyn_opts_strict(ctx, argv[1], open_keys, (int)countof(open_keys)))
            return JS_EXCEPTION;
        m = JS_GetPropertyStr(ctx, argv[1], "dyna");
        if (!JS_IsUndefined(m)) {
            int bad = !JS_IsObject(m);
            if (bad)
                JS_ThrowTypeError(ctx, "SFTP: dyna must be an object");
            if (bad || dyn_opts_strict(ctx, m, open_meta_keys, (int)countof(open_meta_keys))
                || meta_num(ctx, m, "maxInflight", 1, 1024, &maxi) || meta_num(ctx, m, "chunkBytes", 1024, SFTP_MAX_READ, &chunk)) {
                JS_FreeValue(ctx, m);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, m);
    }
    c = (sftp_cli_t*)calloc(1, sizeof *c);
    if (!c)
        return JS_ThrowOutOfMemory(ctx);
    c->ctx = ctx;
    c->rt = JS_GetRuntime(ctx);
    c->ores = c->orej = c->cres = JS_UNDEFINED;
    c->max_inflight = maxi;
    if (JS_AddShutdownSweep(c->rt, cli_sweep, c) == 0)
        c->swept = 1;
    c->chunk = chunk;
    c->next_id = 1;
    obj = JS_NewObjectClass(ctx, (int)sftp_cli_cid);
    if (JS_IsException(obj)) {
        free(c);
        return obj;
    }
    JS_SetOpaque(obj, c);
    c->sshobj = JS_DupValue(ctx, argv[0]);
    p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, obj);
        return p;
    }
    c->ores = f[0];
    c->orej = f[1];
    c->ch = ssh_subsys_open(conn, "sftp", &cli_ops, c, obj);
    if (!c->ch)
        cli_fail(c, conn->err ? conn->err : "cannot open an SFTP channel (connection not ready or dyna.maxChannels reached)");
    JS_FreeValue(ctx, obj);
    return p;
}

static JSValue cli_ctor(JSContext* ctx, JSValueConst nt, int argc, JSValueConst* argv)
{
    (void)nt;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "SFTP: use SFTPClient.open(sshClient)");
}

static int path_arg(JSContext* ctx, JSValueConst v, const char** s, size_t* n)
{
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "SFTP: a path must be a string");
        return -1;
    }
    *s = JS_ToCStringLen(ctx, n, v);
    return *s ? 0 : -1;
}

static JSValue send_simple(JSContext* ctx, sftp_cli_t* c, sftp_req_t* r)
{
    JSValue f[2], p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p)) {
        req_free(c, r);
        return p;
    }
    r->res = f[0];
    r->rej = f[1];
    if (req_send(c, r) < 0) {
        JS_FreeValue(ctx, p);
        return rejected(ctx, err_of(c, SFTP_CONNECTION_LOST, "cannot send the request"));
    }
    kick(c);
    return p;
}

static JSValue not_ready(JSContext* ctx, sftp_cli_t* c)
{
    return rejected(ctx, err_of(c, SFTP_NO_CONNECTION, c->err[0] ? c->err : "the SFTP session is not open"));
}

enum {
    P_STAT,
    P_LSTAT,
    P_REALPATH,
    P_READLINK,
    P_REMOVE,
    P_RMDIR,
    P_MKDIR,
    P_SETSTAT,
    P_RENAME,
    P_SYMLINK,
    P_READDIR,
    P_OPENDIR,
    P_OPEN,
    P_READFILE,
    P_WRITEFILE
};

static uint32_t parse_flags(JSContext* ctx, JSValueConst v, int* bad)
{
    const char* s;
    uint32_t f = 0;
    *bad = 0;
    if (JS_IsUndefined(v))
        return SFTP_FXF_READ;
    s = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    if (!s) {
        *bad = 1;
        JS_ThrowTypeError(ctx, "SFTP: flags must be one of r, r+, w, wx, w+, wx+, a, ax, a+, ax+");
        return 0;
    }
    if (!strcmp(s, "r"))
        f = SFTP_FXF_READ;
    else if (!strcmp(s, "r+"))
        f = SFTP_FXF_READ | SFTP_FXF_WRITE;
    else if (!strcmp(s, "w"))
        f = SFTP_FXF_WRITE | SFTP_FXF_CREAT | SFTP_FXF_TRUNC;
    else if (!strcmp(s, "wx"))
        f = SFTP_FXF_WRITE | SFTP_FXF_CREAT | SFTP_FXF_TRUNC | SFTP_FXF_EXCL;
    else if (!strcmp(s, "w+"))
        f = SFTP_FXF_READ | SFTP_FXF_WRITE | SFTP_FXF_CREAT | SFTP_FXF_TRUNC;
    else if (!strcmp(s, "wx+"))
        f = SFTP_FXF_READ | SFTP_FXF_WRITE | SFTP_FXF_CREAT | SFTP_FXF_TRUNC | SFTP_FXF_EXCL;
    else if (!strcmp(s, "a"))
        f = SFTP_FXF_WRITE | SFTP_FXF_APPEND | SFTP_FXF_CREAT;
    else if (!strcmp(s, "ax"))
        f = SFTP_FXF_WRITE | SFTP_FXF_APPEND | SFTP_FXF_CREAT | SFTP_FXF_EXCL;
    else if (!strcmp(s, "a+"))
        f = SFTP_FXF_READ | SFTP_FXF_WRITE | SFTP_FXF_APPEND | SFTP_FXF_CREAT;
    else if (!strcmp(s, "ax+"))
        f = SFTP_FXF_READ | SFTP_FXF_WRITE | SFTP_FXF_APPEND | SFTP_FXF_CREAT | SFTP_FXF_EXCL;
    else {
        *bad = 1;
        JS_ThrowTypeError(ctx, "SFTP: flags must be one of r, r+, w, wx, w+, wx+, a, ax, a+, ax+");
    }
    JS_FreeCString(ctx, s);
    return f;
}

static int bytes_arg(JSContext* ctx, JSValueConst v, const uint8_t** p, size_t* n, const char** cs)
{
    *cs = NULL;
    if (JS_IsString(v)) {
        *cs = JS_ToCStringLen(ctx, n, v);
        *p = (const uint8_t*)*cs;
        return *cs ? 0 : -1;
    }
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_BUFFER) {
        *p = JS_GetArrayBuffer(ctx, n, v);
        return *p || *n == 0 ? 0 : -1;
    }
    if (JS_GetBufferKind(v) == JS_BUFFER_KIND_VIEW) {
        size_t off = 0, len = 0, bpe = 0, abn = 0;
        JSValue ab = JS_GetArrayBufferView(ctx, v, &off, &len, &bpe);
        uint8_t* base;
        if (JS_IsException(ab))
            return -1;
        base = JS_GetArrayBuffer(ctx, &abn, ab);
        JS_FreeValue(ctx, ab);
        if ((!base && len) || off > abn || len > abn - off) {
            JS_ThrowTypeError(ctx, "SFTP: detached or out-of-bounds buffer");
            return -1;
        }
        *p = base ? base + off : (const uint8_t*)"";
        *n = len;
        return 0;
    }
    JS_ThrowTypeError(ctx, "SFTP: data must be a string, ArrayBuffer or ArrayBufferView");
    return -1;
}

static JSValue cli_path_op(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sftp_cli_t* c = cli_get(ctx, this_val);
    const char *a = NULL, *b = NULL;
    size_t an = 0, bn = 0;
    sftp_attr_t at;
    sftp_req_t* r = NULL;
    uint32_t pflags = 0;
    int bad = 0, kind;
    uint8_t type;
    JSValue ret;
    if (!c)
        return JS_EXCEPTION;
    memset(&at, 0, sizeof at);
    if (path_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &a, &an) < 0)
        return JS_EXCEPTION;
    if (magic == P_RENAME || magic == P_SYMLINK) {
        if (path_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &b, &bn) < 0) {
            JS_FreeCString(ctx, a);
            return JS_EXCEPTION;
        }
    }
    if (magic == P_MKDIR || magic == P_SETSTAT || magic == P_OPEN || magic == P_WRITEFILE) {
        JSValueConst av = argc > (magic == P_MKDIR || magic == P_SETSTAT ? 1 : 2) ? argv[magic == P_MKDIR || magic == P_SETSTAT ? 1 : 2] : JS_UNDEFINED;
        if (magic == P_SETSTAT && !JS_IsObject(av)) {
            JS_FreeCString(ctx, a);
            return JS_ThrowTypeError(ctx, "SFTP: setstat expects an attrs object");
        }
        if (sftp_attr_from_js(ctx, av, &at) < 0) {
            JS_FreeCString(ctx, a);
            return JS_EXCEPTION;
        }
    }
    if (magic == P_OPEN) {
        pflags = parse_flags(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &bad);
        if (bad) {
            JS_FreeCString(ctx, a);
            return JS_EXCEPTION;
        }
    }
    if (!ready(c)) {
        JS_FreeCString(ctx, a);
        if (b)
            JS_FreeCString(ctx, b);
        return not_ready(ctx, c);
    }
    switch (magic) {
    case P_STAT:
        kind = K_ATTRS, type = SFTP_STAT;
        break;
    case P_LSTAT:
        kind = K_ATTRS, type = SFTP_LSTAT;
        break;
    case P_REALPATH:
        kind = K_NAME1, type = SFTP_REALPATH;
        break;
    case P_READLINK:
        kind = K_NAME1, type = SFTP_READLINK;
        break;
    case P_REMOVE:
        kind = K_STATUS, type = SFTP_REMOVE;
        break;
    case P_RMDIR:
        kind = K_STATUS, type = SFTP_RMDIR;
        break;
    case P_MKDIR:
        kind = K_STATUS, type = SFTP_MKDIR;
        break;
    case P_SETSTAT:
        kind = K_STATUS, type = SFTP_SETSTAT;
        break;
    case P_RENAME:
        kind = K_STATUS, type = SFTP_RENAME;
        break;
    case P_SYMLINK:
        kind = K_STATUS, type = SFTP_SYMLINK;
        break;
    case P_READDIR:
        kind = K_LSOPEN, type = SFTP_OPENDIR;
        break;
    case P_OPENDIR:
        kind = K_OPENDIR, type = SFTP_OPENDIR;
        break;
    case P_READFILE:
        kind = K_RFOPEN, type = SFTP_OPEN, pflags = SFTP_FXF_READ;
        break;
    case P_WRITEFILE:
        kind = K_WFOPEN, type = SFTP_OPEN, pflags = SFTP_FXF_WRITE | SFTP_FXF_CREAT | SFTP_FXF_TRUNC;
        break;
    default:
        kind = K_OPEN, type = SFTP_OPEN;
        break;
    }
    r = req_new(c, kind, type);
    bad = !r;
    if (r) {
        sshb_t* pk = &c->pk;
        if (magic == P_SYMLINK)
            bad = sshb_str(pk, a, an) || sshb_str(pk, b, bn);
        else
            bad = sshb_str(pk, a, an);
        if (!bad && magic == P_RENAME)
            bad = sshb_str(pk, b, bn);
        if (!bad && (type == SFTP_OPEN))
            bad = sshb_u32(pk, pflags) || sftp_attr_put(pk, &at);
        if (!bad && (magic == P_MKDIR || magic == P_SETSTAT))
            bad = sftp_attr_put(pk, &at);
    }
    JS_FreeCString(ctx, a);
    if (b)
        JS_FreeCString(ctx, b);
    if (bad) {
        if (r)
            free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == P_WRITEFILE) {
        const uint8_t* dp;
        size_t dn;
        const char* cs;
        if (bytes_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &dp, &dn, &cs) < 0) {
            req_free(c, r);
            return JS_EXCEPTION;
        }
        bad = sshb_put(&r->data, dp, dn);
        if (cs)
            JS_FreeCString(ctx, cs);
        if (bad) {
            req_free(c, r);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    ret = send_simple(ctx, c, r);
    return ret;
}

static JSValue cli_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sftp_cli_t* c = cli_get(ctx, this_val);
    JSValue f[2], p;
    (void)argc;
    (void)argv;
    if (!c)
        return JS_EXCEPTION;
    if (!c->ch || !c->ch->conn || c->ch->state == SSH_CH_CLOSED) {
        p = JS_NewPromiseCapability(ctx, f);
        if (!JS_IsException(p)) {
            call1(ctx, f[0], JS_UNDEFINED);
            JS_FreeValue(ctx, f[0]);
            JS_FreeValue(ctx, f[1]);
        }
        return p;
    }
    p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p))
        return p;
    fv(c->rt, &c->cres);
    c->cres = f[0];
    JS_FreeValue(ctx, f[1]);
    if (c->state != 3)
        cli_fail(c, "the SFTP session was closed");
    ssh_chan_close(c->ch);
    kick(c);
    return p;
}

static JSValue cli_get_version(JSContext* ctx, JSValueConst this_val)
{
    sftp_cli_t* c = cli_get(ctx, this_val);
    return c ? JS_NewUint32(ctx, c->version) : JS_EXCEPTION;
}

static JSValue cli_get_stats(JSContext* ctx, JSValueConst this_val)
{
    sftp_cli_t* c = cli_get(ctx, this_val);
    JSValue o;
    if (!c)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "requests", JS_NewFloat64(ctx, (double)c->requests));
    JS_SetPropertyStr(ctx, o, "bytesRead", JS_NewFloat64(ctx, (double)c->rbytes));
    JS_SetPropertyStr(ctx, o, "bytesWritten", JS_NewFloat64(ctx, (double)c->wbytes));
    JS_SetPropertyStr(ctx, o, "maxInflight", JS_NewInt32(ctx, c->max_seen));
    JS_SetPropertyStr(ctx, o, "inflight", JS_NewInt32(ctx, c->nrq));
    return o;
}

static sftp_file_t* file_get(JSContext* ctx, JSValueConst v)
{
    return (sftp_file_t*)JS_GetOpaque2(ctx, v, sftp_file_cid);
}

static JSValue new_pending(JSContext* ctx, JSValue* res, JSValue* rej)
{
    JSValue f[2], p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p))
        return p;
    *res = f[0];
    *rej = f[1];
    return p;
}

static JSValue file_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sftp_file_t* f = file_get(ctx, this_val);
    sftp_cli_t* c;
    JSValue p;
    (void)argc;
    (void)argv;
    if (!f)
        return JS_EXCEPTION;
    c = f->cli;
    if (!JS_IsUndefined(f->rres))
        return rejected(ctx, err_of(c, SFTP_FAILURE, "a read() is already pending on this file"));
    if (f->rmsg)
        return rejected(ctx, err_of(c, f->rcode, f->rmsg));
    if (!f->open || f->closing)
        return rejected(ctx, err_of(c, SFTP_FAILURE, "the file is closed"));
    p = new_pending(ctx, &f->rres, &f->rrej);
    if (JS_IsException(p))
        return p;
    f_pin(f, ctx, this_val);
    f_fill(f);
    f_advance(f);
    kick(c);
    return p;
}

static int num_arg(JSContext* ctx, JSValueConst v, const char* what, double hi, double* out)
{
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "SFTP: %s must be a number", what);
        return -1;
    }
    JS_ToFloat64(ctx, out, v);
    if (!(*out >= 0 && *out <= hi) || floor(*out) != *out) {
        JS_ThrowRangeError(ctx, "SFTP: %s must be an integer in 0..%.0f", what, hi);
        return -1;
    }
    return 0;
}

static JSValue file_pread(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sftp_file_t* f = file_get(ctx, this_val);
    sftp_cli_t* c;
    double off, len;
    sftp_req_t* r;
    if (!f)
        return JS_EXCEPTION;
    c = f->cli;
    if (num_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "position", 9007199254740991.0, &off) < 0
        || num_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "length", SFTP_MAX_READ, &len) < 0)
        return JS_EXCEPTION;
    if (!f->open || f->closing || !ready(c))
        return rejected(ctx, err_of(c, SFTP_FAILURE, "the file is closed"));
    r = req_new(c, K_PREAD, SFTP_READ);
    if (!r || put_handle(c, f->h, f->hl) || put_u64(&c->pk, (uint64_t)off) || sshb_u32(&c->pk, (uint32_t)len)) {
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return send_simple(ctx, c, r);
}

static JSValue file_write(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sftp_file_t* f = file_get(ctx, this_val);
    sftp_cli_t* c;
    const uint8_t* p;
    size_t n;
    const char* cs;
    int bad;
    JSValue pr;
    if (!f)
        return JS_EXCEPTION;
    c = f->cli;
    if (bytes_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &p, &n, &cs) < 0)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(f->wres)) {
        if (cs)
            JS_FreeCString(ctx, cs);
        return rejected(ctx, err_of(c, SFTP_FAILURE, "a write() is already pending on this file"));
    }
    if (f->wmsg || !f->open || f->closing || !ready(c)) {
        if (cs)
            JS_FreeCString(ctx, cs);
        return rejected(ctx, f->wmsg ? err_of(c, f->wcode, f->wmsg) : err_of(c, SFTP_FAILURE, "the file is closed"));
    }
    bad = sshb_put(&f->wq, p, n);
    if (cs)
        JS_FreeCString(ctx, cs);
    if (bad)
        return JS_ThrowOutOfMemory(ctx);
    pr = new_pending(ctx, &f->wres, &f->wrej);
    if (JS_IsException(pr))
        return pr;
    f_pin(f, ctx, this_val);
    f_wpump(f);
    f_wsettle(f);
    kick(c);
    return pr;
}

static JSValue file_pwrite(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sftp_file_t* f = file_get(ctx, this_val);
    sftp_cli_t* c;
    double off;
    const uint8_t* p;
    size_t n;
    const char* cs;
    sftp_req_t* r;
    int bad;
    if (!f)
        return JS_EXCEPTION;
    c = f->cli;
    if (num_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "position", 9007199254740991.0, &off) < 0)
        return JS_EXCEPTION;
    if (bytes_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &p, &n, &cs) < 0)
        return JS_EXCEPTION;
    if (n > SFTP_MAX_READ) {
        if (cs)
            JS_FreeCString(ctx, cs);
        return JS_ThrowRangeError(ctx, "SFTP: pwrite data must be at most %u bytes; use write() for streams", (unsigned)SFTP_MAX_READ);
    }
    if (!f->open || f->closing || !ready(c)) {
        if (cs)
            JS_FreeCString(ctx, cs);
        return rejected(ctx, err_of(c, SFTP_FAILURE, "the file is closed"));
    }
    r = req_new(c, K_PWRITE, SFTP_WRITE);
    bad = !r || put_handle(c, f->h, f->hl) || put_u64(&c->pk, (uint64_t)off) || sshb_str(&c->pk, p, n);
    if (cs)
        JS_FreeCString(ctx, cs);
    if (bad) {
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->wbytes += n;
    return send_simple(ctx, c, r);
}

static JSValue file_handle_op(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sftp_file_t* f = file_get(ctx, this_val);
    sftp_cli_t* c;
    sftp_attr_t at;
    sftp_req_t* r;
    if (!f)
        return JS_EXCEPTION;
    c = f->cli;
    if (magic) {
        if (argc < 1 || !JS_IsObject(argv[0]))
            return JS_ThrowTypeError(ctx, "SFTP: setstat expects an attrs object");
        if (sftp_attr_from_js(ctx, argv[0], &at) < 0)
            return JS_EXCEPTION;
    }
    if (!f->open || f->closing || !ready(c))
        return rejected(ctx, err_of(c, SFTP_FAILURE, "the file is closed"));
    r = req_new(c, magic ? K_STATUS : K_ATTRS, magic ? SFTP_FSETSTAT : SFTP_FSTAT);
    if (!r || put_handle(c, f->h, f->hl) || (magic && sftp_attr_put(&c->pk, &at))) {
        free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    return send_simple(ctx, c, r);
}

static JSValue file_close(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sftp_file_t* f = file_get(ctx, this_val);
    sftp_cli_t* c;
    JSValue p;
    (void)argc;
    (void)argv;
    if (!f)
        return JS_EXCEPTION;
    c = f->cli;
    if (f->closing || !f->open)
        return rejected(ctx, err_of(c, SFTP_FAILURE, "the file is already closed"));
    p = new_pending(ctx, &f->cres, &f->crej);
    if (JS_IsException(p))
        return p;
    f_pin(f, ctx, this_val);
    f->closing = 1;
    if (!JS_IsUndefined(f->rres))
        f_settle(f, ctx, c->rt, &f->rres, &f->rrej, 0, err_of(c, SFTP_FAILURE, "the file was closed"));
    f_wsettle(f);
    kick(c);
    return p;
}

static JSValue file_get_pos(JSContext* ctx, JSValueConst this_val)
{
    sftp_file_t* f = file_get(ctx, this_val);
    return f ? JS_NewFloat64(ctx, (double)f->woff) : JS_EXCEPTION;
}

static sftp_dir_t* dir_get(JSContext* ctx, JSValueConst v)
{
    return (sftp_dir_t*)JS_GetOpaque2(ctx, v, sftp_dir_cid);
}

static JSValue dir_op(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sftp_dir_t* d = dir_get(ctx, this_val);
    sftp_req_t* r;
    (void)argc;
    (void)argv;
    if (!d)
        return JS_EXCEPTION;
    if (!d->open || !ready(d->cli))
        return rejected(ctx, err_of(d->cli, SFTP_FAILURE, "the directory is closed"));
    if (dir_send(d->cli, magic ? K_DCLOSE : K_DREAD, magic ? SFTP_CLOSE : SFTP_READDIR, d->h, d->hl, &r) < 0)
        return JS_ThrowOutOfMemory(ctx);
    if (magic)
        d->open = 0;
    return send_simple(ctx, d->cli, r);
}

static void cli_finalizer(JSRuntime* rt, JSValue val)
{
    sftp_cli_t* c = (sftp_cli_t*)JS_GetOpaque(val, sftp_cli_cid);
    int i;
    if (!c)
        return;
    if (c->swept) {
        c->swept = 0;
        JS_RemoveShutdownSweep(rt, cli_sweep, c);
    }
    for (i = 0; i < c->nrq; i++) {
        sftp_req_t* r = c->rq[i];
        r->f = NULL;
        fv(rt, &r->res);
        fv(rt, &r->rej);
        fv(rt, &r->aux);
        sshb_free(&r->data);
        free(r);
    }
    free(c->rq);
    while (c->files) {
        sftp_file_t* f = c->files;
        c->files = f->next;
        f->next = f->prev = NULL;
        f->cli = NULL;
        if (f->has_obj != 1)
            f_free_raw(rt, f);
    }
    if (c->ch) {
        c->ch->opq = NULL;
        c->ch->ops = NULL;
        ssh_chan_unref(rt, c->ch);
    }
    fv(rt, &c->ores);
    fv(rt, &c->orej);
    fv(rt, &c->cres);
    fv(rt, &c->sshobj);
    sshb_free(&c->in);
    sshb_free(&c->pk);
    free(c);
}

static void file_mark_vals(JSRuntime* rt, sftp_file_t* f, JS_MarkFunc* mark)
{
    JS_MarkValue(rt, f->cliobj, mark);
    JS_MarkValue(rt, f->rres, mark);
    JS_MarkValue(rt, f->rrej, mark);
    JS_MarkValue(rt, f->wres, mark);
    JS_MarkValue(rt, f->wrej, mark);
    JS_MarkValue(rt, f->cres, mark);
    JS_MarkValue(rt, f->crej, mark);
}

static void cli_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    sftp_cli_t* c = (sftp_cli_t*)JS_GetOpaque(val, sftp_cli_cid);
    sftp_file_t* f;
    int i;
    if (!c)
        return;
    JS_MarkValue(rt, c->sshobj, mark);
    JS_MarkValue(rt, c->ores, mark);
    JS_MarkValue(rt, c->orej, mark);
    JS_MarkValue(rt, c->cres, mark);
    for (i = 0; i < c->nrq; i++) {
        JS_MarkValue(rt, c->rq[i]->res, mark);
        JS_MarkValue(rt, c->rq[i]->rej, mark);
        JS_MarkValue(rt, c->rq[i]->aux, mark);
    }
    for (f = c->files; f; f = f->next)
        if (f->has_obj != 1)
            file_mark_vals(rt, f, mark);
}

static void file_finalizer(JSRuntime* rt, JSValue val)
{
    sftp_file_t* f = (sftp_file_t*)JS_GetOpaque(val, sftp_file_cid);
    (void)rt;
    if (!f)
        return;
    f->has_obj = -1;
    if (!f->cli) {
        f_free_raw(rt, f);
        return;
    }
    f_unref(f);
}

static void file_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    sftp_file_t* f = (sftp_file_t*)JS_GetOpaque(val, sftp_file_cid);
    if (f)
        file_mark_vals(rt, f, mark);
}

static void dir_finalizer(JSRuntime* rt, JSValue val)
{
    sftp_dir_t* d = (sftp_dir_t*)JS_GetOpaque(val, sftp_dir_cid);
    if (!d)
        return;
    fv(rt, &d->cliobj);
    free(d);
}

static void dir_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    sftp_dir_t* d = (sftp_dir_t*)JS_GetOpaque(val, sftp_dir_cid);
    if (d)
        JS_MarkValue(rt, d->cliobj, mark);
}

static const JSClassDef cli_class = { "SFTPClient", .finalizer = cli_finalizer, .gc_mark = cli_mark };
static const JSClassDef file_class = { "SFTPFile", .finalizer = file_finalizer, .gc_mark = file_mark };
static const JSClassDef dir_class = { "SFTPDir", .finalizer = dir_finalizer, .gc_mark = dir_mark };

static const JSCFunctionListEntry cli_proto[] = {
    JS_CFUNC_MAGIC_DEF("stat", 1, cli_path_op, P_STAT),
    JS_CFUNC_MAGIC_DEF("lstat", 1, cli_path_op, P_LSTAT),
    JS_CFUNC_MAGIC_DEF("realpath", 1, cli_path_op, P_REALPATH),
    JS_CFUNC_MAGIC_DEF("readlink", 1, cli_path_op, P_READLINK),
    JS_CFUNC_MAGIC_DEF("remove", 1, cli_path_op, P_REMOVE),
    JS_CFUNC_MAGIC_DEF("rmdir", 1, cli_path_op, P_RMDIR),
    JS_CFUNC_MAGIC_DEF("mkdir", 2, cli_path_op, P_MKDIR),
    JS_CFUNC_MAGIC_DEF("setstat", 2, cli_path_op, P_SETSTAT),
    JS_CFUNC_MAGIC_DEF("rename", 2, cli_path_op, P_RENAME),
    JS_CFUNC_MAGIC_DEF("symlink", 2, cli_path_op, P_SYMLINK),
    JS_CFUNC_MAGIC_DEF("readdir", 1, cli_path_op, P_READDIR),
    JS_CFUNC_MAGIC_DEF("opendir", 1, cli_path_op, P_OPENDIR),
    JS_CFUNC_MAGIC_DEF("open", 3, cli_path_op, P_OPEN),
    JS_CFUNC_MAGIC_DEF("readFile", 1, cli_path_op, P_READFILE),
    JS_CFUNC_MAGIC_DEF("writeFile", 3, cli_path_op, P_WRITEFILE),
    JS_CFUNC_DEF("close", 0, cli_close),
    JS_CGETSET_DEF("version", cli_get_version, NULL),
    JS_CGETSET_DEF("stats", cli_get_stats, NULL),
};

static const JSCFunctionListEntry file_proto[] = {
    JS_CFUNC_DEF("read", 0, file_read),
    JS_CFUNC_DEF("pread", 2, file_pread),
    JS_CFUNC_DEF("write", 1, file_write),
    JS_CFUNC_DEF("pwrite", 2, file_pwrite),
    JS_CFUNC_MAGIC_DEF("stat", 0, file_handle_op, 0),
    JS_CFUNC_MAGIC_DEF("setstat", 1, file_handle_op, 1),
    JS_CFUNC_DEF("close", 0, file_close),
    JS_CGETSET_DEF("writePosition", file_get_pos, NULL),
};

static const JSCFunctionListEntry dir_proto[] = {
    JS_CFUNC_MAGIC_DEF("read", 0, dir_op, 0),
    JS_CFUNC_MAGIC_DEF("close", 0, dir_op, 1),
};

static int plain_class(JSContext* ctx, JSClassID* id, const JSClassDef* def, const JSCFunctionListEntry* fns, int n)
{
    JSValue proto;
    JS_NewClassID(id);
    if (JS_NewClass(JS_GetRuntime(ctx), *id, def) != 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, fns, n);
    JS_SetClassProto(ctx, *id, proto);
    return 0;
}

int sftp_client_init(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto, ctor;
    if (dyn_register_class(ctx, m, &sftp_cli_cid, &cli_class, cli_proto, (int)countof(cli_proto), cli_ctor, "SFTPClient") < 0)
        return -1;
    proto = JS_GetClassProto(ctx, sftp_cli_cid);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (JS_IsFunction(ctx, ctor))
        JS_SetPropertyStr(ctx, ctor, "open", JS_NewCFunction(ctx, cli_open, "open", 2));
    JS_FreeValue(ctx, ctor);
    if (plain_class(ctx, &sftp_file_cid, &file_class, file_proto, (int)countof(file_proto)) < 0
        || plain_class(ctx, &sftp_dir_cid, &dir_class, dir_proto, (int)countof(dir_proto)) < 0)
        return -1;
    return 0;
}

#endif
