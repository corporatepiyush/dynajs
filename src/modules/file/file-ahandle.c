#include "file-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_FILE)

#include <math.h>

JSClassID dyn_fah_class_id;
static JSClassID dyn_fab_rclass_id;
static JSClassID dyn_fab_wclass_id;

static const char* const dyn_fah_ops[] = { "open", "read", "read", "write", "sync", "stat", "close",
    "read", "readBytes", "readInto", "readLine", "next", "return", "close", "write", "flush", "sync", "close" };

static JSValue dyn_fab_result(JSContext* ctx, dyn_fa_job_t* j, int* bad);

static void dyn_fah_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_fah_t* h = (dyn_fah_t*)JS_GetOpaque(val, dyn_fah_class_id);
    (void)rt;
    if (!h)
        return;
    if (h->fd >= 0)
        close(h->fd);
    free(h);
}

static const JSClassDef dyn_fah_class = {
    "FileHandle",
    .finalizer = dyn_fah_finalizer,
};

static ssize_t dyn_fah_io(dyn_fah_op_t* o, int wr)
{
    uint8_t* p = o->buf + o->got;
    size_t n = o->len - o->got;
    off_t at = (off_t)(o->pos + (int64_t)o->got);
    if (wr)
        return o->haspos ? pwrite(o->fd, p, n, at) : write(o->fd, p, n);
    return o->haspos ? pread(o->fd, p, n, at) : read(o->fd, p, n);
}

static int dyn_fah_closer(int k)
{
    return k == DYN_FAH_CLOSE || k == DYN_FAH_RRET || k == DYN_FAH_RCLOSE || k == DYN_FAH_WCLOSE;
}

static int dyn_fab_flush(int fd, dyn_fabuf_t* b)
{
    size_t off = 0;
    int e = 0;
    while (off < b->end) {
        ssize_t n = write(fd, b->p + off, b->end - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            e = n < 0 ? errno : EIO;
            break;
        }
        off += (size_t)n;
    }
    if (off && off < b->end)
        memmove(b->p, b->p + off, b->end - off);
    b->end -= off;
    if (!e)
        b->dirty = 0;
    return e;
}

void dyn_fab_free(dyn_fabuf_t* b, int fd)
{
    if (!b)
        return;
    if (b->dirty && fd >= 0)
        dyn_fab_flush(fd, b);
    free(b->p);
    free(b);
}

static int dyn_fab_fill(int fd, dyn_fabuf_t* b)
{
    ssize_t n;
    if (b->start < b->end)
        return 1;
    b->start = b->end = 0;
    if (b->eof)
        return 0;
    do
        n = read(fd, b->p, b->cap);
    while (n < 0 && errno == EINTR);
    if (n < 0)
        return -1;
    b->end = (size_t)n;
    if (n == 0)
        b->eof = 1;
    return n > 0;
}

static int dyn_fab_room(dyn_fah_op_t* o, size_t need)
{
    size_t nc;
    uint8_t* q;
    if (need <= o->outcap)
        return 0;
    if (need > DYN_MAX_INPUT) {
        o->err = EFBIG;
        return -1;
    }
    nc = o->outcap ? o->outcap : 256;
    while (nc < need)
        nc *= 2;
    if (nc > DYN_MAX_INPUT)
        nc = DYN_MAX_INPUT;
    q = (uint8_t*)realloc(o->out, nc);
    if (!q) {
        o->err = ENOMEM;
        return -1;
    }
    o->out = q;
    o->outcap = nc;
    return 0;
}

static void dyn_fab_repair(dyn_fah_op_t* o)
{
    size_t need;
    uint8_t* fixed;
    int changed;
    if (!o->outlen || dyn_utf8_valid(o->out, o->outlen))
        return;
    need = dyn_utf8_lossy_into(o->out, o->outlen, NULL, &changed);
    fixed = (uint8_t*)malloc(need ? need : 1);
    if (!fixed) {
        o->err = ENOMEM;
        return;
    }
    o->outlen = dyn_utf8_lossy_into(o->out, o->outlen, fixed, &changed);
    free(o->out);
    o->out = fixed;
    o->outcap = need;
}

static void dyn_fab_take(dyn_fah_op_t* o, int line)
{
    dyn_fabuf_t* b = o->fb;
    int saw = 0;
    for (;;) {
        size_t avail, take;
        uint8_t* nl = NULL;
        int r;
        if (!line && o->want >= 0 && (int64_t)o->outlen >= o->want)
            return;
        r = dyn_fab_fill(o->fd, b);
        if (r < 0) {
            o->err = errno;
            return;
        }
        if (r == 0)
            break;
        saw = 1;
        avail = b->end - b->start;
        if (line)
            nl = (uint8_t*)memchr(b->p + b->start, '\n', avail);
        take = nl ? (size_t)(nl - (b->p + b->start)) : avail;
        if (!line && o->want >= 0 && take > (size_t)(o->want - (int64_t)o->outlen))
            take = (size_t)(o->want - (int64_t)o->outlen);
        if (dyn_fab_room(o, o->outlen + take) < 0)
            return;
        if (take)
            memcpy(o->out + o->outlen, b->p + b->start, take);
        o->outlen += take;
        b->start += take;
        if (nl) {
            b->start++;
            if (o->outlen && o->out[o->outlen - 1] == '\r')
                o->outlen--;
            return;
        }
    }
    if (line && !saw && !o->outlen)
        o->isnull = 1;
}

static void dyn_fab_setup(dyn_fah_op_t* o)
{
    dyn_fabuf_t* b = (dyn_fabuf_t*)malloc(sizeof(*b));
    struct stat st;
    if (b) {
        memset(b, 0, sizeof(*b));
        b->cap = o->bsize;
        b->p = (uint8_t*)malloc(b->cap);
    }
    if (!b || !b->p) {
        free(b);
        o->err = ENOMEM;
        return;
    }
    o->own_fb = b;
    if (o->bmode == 1 && fstat(o->fd, &st) == 0)
        dyn_io_advise_seq_read(o->fd, st.st_size);
    if (o->bmode == 2 && o->pos > 0)
        dyn_io_preallocate(o->fd, (off_t)o->pos);
}

static void dyn_fab_run(dyn_fah_op_t* o)
{
    dyn_fabuf_t* b = o->fb;
    int e;
    switch (o->kind) {
    case DYN_FAH_RTEXT:
    case DYN_FAH_RBYTES:
    case DYN_FAH_RINTO:
        dyn_fab_take(o, 0);
        if (!o->err && o->kind == DYN_FAH_RTEXT)
            dyn_fab_repair(o);
        return;
    case DYN_FAH_RLINE:
    case DYN_FAH_RNEXT:
        dyn_fab_take(o, 1);
        if (!o->err && !o->isnull)
            dyn_fab_repair(o);
        return;
    case DYN_FAH_WPUT:
        if (b->end + o->len > b->cap && (o->err = dyn_fab_flush(o->fd, b)) != 0)
            return;
        if (o->len >= b->cap) {
            while (o->got < o->len) {
                ssize_t r = write(o->fd, o->buf + o->got, o->len - o->got);
                if (r < 0 && errno == EINTR)
                    continue;
                if (r <= 0) {
                    o->err = r < 0 ? errno : EIO;
                    return;
                }
                o->got += (size_t)r;
            }
            return;
        }
        if (o->len)
            memcpy(b->p + b->end, o->buf, o->len);
        b->end += o->len;
        if (o->len)
            b->dirty = 1;
        o->got = o->len;
        return;
    case DYN_FAH_WFLUSH:
        o->err = dyn_fab_flush(o->fd, b);
        return;
    case DYN_FAH_WSYNC:
        o->err = dyn_fab_flush(o->fd, b);
        if (!o->err && dyn_io_durable_sync(o->fd) < 0)
            o->err = errno ? errno : EIO;
        return;
    case DYN_FAH_WCLOSE:
        e = dyn_fab_flush(o->fd, b);
        if (close(o->fd) < 0 && errno != EINTR && !e)
            e = errno;
        o->err = e;
        return;
    default:
        if (close(o->fd) < 0 && errno != EINTR)
            o->err = errno;
        return;
    }
}

void dyn_fah_run(dyn_fah_op_t* o)
{
    ssize_t r;
    int wr = o->kind == DYN_FAH_WRITE;
    if (o->kind >= DYN_FAH_RTEXT) {
        dyn_fab_run(o);
        return;
    }
    switch (o->kind) {
    case DYN_FAH_OPEN:
        o->fd = dyn_fs_path_open(o->path, o->flags | O_CLOEXEC, o->mode);
        if (o->fd < 0)
            o->err = errno;
        else
            o->own_fd = o->fd;
        if (o->fd >= 0 && o->bmode)
            dyn_fab_setup(o);
        return;
    case DYN_FAH_READ:
    case DYN_FAH_READINTO:
    case DYN_FAH_WRITE:
        while (o->got < o->len) {
            r = dyn_fah_io(o, wr);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                o->err = errno;
                return;
            }
            if (r == 0) {
                if (wr)
                    o->err = EIO;
                return;
            }
            o->got += (size_t)r;
        }
        return;
    case DYN_FAH_SYNC:
#ifdef F_FULLFSYNC
        if (fcntl(o->fd, F_FULLFSYNC) == 0)
            return;
#endif
        if (fsync(o->fd) < 0)
            o->err = errno;
        return;
    case DYN_FAH_STAT:
        if (fstat(o->fd, &o->st) < 0)
            o->err = errno;
        return;
    default:
        if (close(o->fd) < 0 && errno != EINTR)
            o->err = errno;
        return;
    }
}

static JSValue dyn_fah_into(JSContext* ctx, dyn_fah_op_t* o, int* bad)
{
    size_t off, len, bpe, abn, n;
    (void)len;
    uint8_t* base;
    JSValue ab = JS_GetArrayBufferView(ctx, o->into, &off, &len, &bpe);
    if (JS_IsException(ab))
        return ab;
    base = JS_GetArrayBuffer(ctx, &abn, ab);
    JS_FreeValue(ctx, ab);
    if (!base)
        return JS_EXCEPTION;
    n = o->got;
    if (off > abn)
        n = 0;
    else if (n > abn - off)
        n = abn - off;
    if (n)
        memcpy(base + off, o->buf, n);
    *bad = 0;
    return JS_NewInt64(ctx, (int64_t)n);
}

JSValue dyn_fah_result(JSContext* ctx, dyn_fa_job_t* j, int* bad)
{
    dyn_fah_op_t* o = &j->u.h;
    JSValue v;
    *bad = 1;
    if (dyn_fah_closer(o->kind) && o->h) {
        o->h->fd = -1;
        dyn_fab_free(o->h->fb, -1);
        o->h->fb = NULL;
    }
    if (o->kind >= DYN_FAH_RTEXT)
        return dyn_fab_result(ctx, j, bad);
    if (o->err)
        return dyn_fs_throw(ctx, o->err, j->name, o->kind == DYN_FAH_OPEN ? o->path : NULL);
    switch (o->kind) {
    case DYN_FAH_OPEN: {
        dyn_fah_t* h;
        v = JS_NewObjectClass(ctx, (int)(o->bmode == 2 ? dyn_fab_wclass_id : o->bmode ? dyn_fab_rclass_id : dyn_fah_class_id));
        if (JS_IsException(v))
            return v;
        h = (dyn_fah_t*)malloc(sizeof(*h));
        if (!h) {
            JS_FreeValue(ctx, v);
            return JS_ThrowOutOfMemory(ctx);
        }
        memset(h, 0, sizeof(*h));
        h->fd = o->fd;
        h->append = (o->flags & O_APPEND) != 0;
        h->fb = o->own_fb;
        o->own_fb = NULL;
        o->own_fd = -1;
        JS_SetOpaque(v, h);
        *bad = 0;
        return v;
    }
    case DYN_FAH_READ:
        v = dyn_nat_u8_copy(ctx, o->buf, o->got);
        break;
    case DYN_FAH_READINTO:
        return dyn_fah_into(ctx, o, bad);
    case DYN_FAH_WRITE:
        v = JS_NewInt64(ctx, (int64_t)o->got);
        break;
    case DYN_FAH_STAT:
        v = dyn_fs_stat_obj(ctx, &o->st);
        break;
    default:
        v = JS_UNDEFINED;
        break;
    }
    if (!JS_IsException(v))
        *bad = 0;
    return v;
}

void dyn_fah_drop(JSContext* ctx, dyn_fa_job_t* j)
{
    JSValue hv = j->u.h.hv, into = j->u.h.into;
    j->u.h.hv = JS_UNDEFINED;
    j->u.h.into = JS_UNDEFINED;
    j->u.h.h = NULL;
    JS_FreeValue(ctx, into);
    JS_FreeValue(ctx, hv);
}

void dyn_fah_after(JSContext* ctx, dyn_fa_job_t* j)
{
    dyn_fah_t* h = j->u.h.h;
    if (h) {
        if (h->cur == j)
            h->cur = NULL;
        while (!h->cur && h->qhead) {
            dyn_fa_job_t* n = h->qhead;
            h->qhead = n->u.h.qnext;
            if (!h->qhead)
                h->qtail = NULL;
            n->u.h.qnext = NULL;
            n->u.h.fd = h->fd;
            n->u.h.fb = h->fb;
            h->cur = n;
            if (dyn_fa_offload(n) < 0) {
                h->cur = NULL;
                dyn_fa_refuse(ctx, n);
            }
        }
    }
    dyn_fah_drop(ctx, j);
}

void dyn_fah_kill(JSRuntime* rt, dyn_fa_job_t* j)
{
    dyn_fah_t* h = j->u.h.h;
    if (h) {
        if (h->cur == j) {
            if (!dyn_fah_closer(j->u.h.kind))
                j->u.h.own_fd = h->fd;
            j->u.h.own_fb = h->fb;
            h->fb = NULL;
            h->fd = -1;
            h->cur = NULL;
        }
        h->qhead = NULL;
        h->qtail = NULL;
        h->closing = 1;
    }
    j->u.h.h = NULL;
    JS_FreeValueRT(rt, j->u.h.into);
    JS_FreeValueRT(rt, j->u.h.hv);
    j->u.h.into = JS_UNDEFINED;
    j->u.h.hv = JS_UNDEFINED;
}

static dyn_fa_job_t* dyn_fah_job(JSContext* ctx, int kind, size_t extra)
{
    dyn_fa_job_t* j = (dyn_fa_job_t*)malloc(sizeof(*j) + extra);
    if (!j) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memset(j, 0, sizeof(*j));
    j->resolve = JS_UNDEFINED;
    j->reject = JS_UNDEFINED;
    j->op = DYN_FA_HANDLE;
    j->name = dyn_fah_ops[kind];
    j->u.h.kind = kind;
    j->u.h.hv = JS_UNDEFINED;
    j->u.h.into = JS_UNDEFINED;
    j->u.h.fd = -1;
    j->u.h.own_fd = -1;
    return j;
}

static void dyn_fah_unjob(dyn_fa_job_t* j)
{
    free(j->u.h.buf);
    free(j);
}

static JSValue dyn_fah_settled(JSContext* ctx, int err, const char* op)
{
    JSValue funcs[2], p, v, r;
    p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p))
        return p;
    if (err) {
        dyn_fs_throw(ctx, err, op, NULL);
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, p);
            return JS_EXCEPTION;
        }
        v = JS_GetException(ctx);
    } else {
        v = JS_UNDEFINED;
    }
    r = JS_Call(ctx, funcs[err ? 1 : 0], JS_UNDEFINED, 1, (JSValueConst*)&v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return p;
}

static JSValue dyn_fah_queue(JSContext* ctx, JSValueConst this_val, dyn_fah_t* h,
    dyn_fa_job_t* j, JSValueConst into)
{
    int armed;
    JSValue p = dyn_fa_arm(ctx, j, &armed);
    if (!armed)
        return p;
    j->u.h.h = h;
    j->u.h.hv = JS_DupValue(ctx, this_val);
    j->u.h.into = JS_DupValue(ctx, into);
    if (h->cur || h->qhead) {
        if (h->qtail)
            h->qtail->u.h.qnext = j;
        else
            h->qhead = j;
        h->qtail = j;
        return p;
    }
    h->cur = j;
    j->u.h.fd = h->fd;
    j->u.h.fb = h->fb;
    if (dyn_fa_offload(j) < 0) {
        h->cur = NULL;
        dyn_fa_refuse(ctx, j);
    }
    return p;
}

static int dyn_fah_index(JSContext* ctx, JSValueConst v, double max, const char* what, double* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= 0 && d <= max) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "file.async: %s must be an integer in 0..%.0f", what, max);
        return -1;
    }
    *out = d;
    return 0;
}

static int dyn_fah_pos(JSContext* ctx, int argc, JSValueConst* argv, int64_t* pos, int* has)
{
    double d;
    *has = 0;
    *pos = 0;
    if (argc < 2 || JS_IsUndefined(argv[1]) || JS_IsNull(argv[1]))
        return 0;
    if (dyn_fah_index(ctx, argv[1], 9007199254740991.0, "position", &d) < 0)
        return -1;
    *pos = (int64_t)d;
    *has = 1;
    return 0;
}

static JSValue dyn_fah_read(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fah_t* h = (dyn_fah_t*)JS_GetOpaque2(ctx, this_val, dyn_fah_class_id);
    JSValueConst a0 = argc > 0 ? argv[0] : JS_UNDEFINED;
    dyn_fa_job_t* j;
    double d = 65536;
    size_t len;
    int64_t pos;
    int has, view = 0;

    if (!h)
        return JS_EXCEPTION;
    if (JS_IsObject(a0)) {
        size_t off, bpe, abn;
        JSValue ab;
        if (JS_CheckWritableBuffer(ctx, a0) < 0)
            return JS_EXCEPTION;
        ab = JS_GetArrayBufferView(ctx, a0, &off, &len, &bpe);
        if (JS_IsException(ab))
            return JS_EXCEPTION;
        if (!JS_GetArrayBuffer(ctx, &abn, ab)) {
            JS_FreeValue(ctx, ab);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, ab);
        if (off > abn || len > abn - off)
            return JS_ThrowRangeError(ctx, "file.read: view out of bounds");
        if (len > DYN_MAX_INPUT)
            return JS_ThrowRangeError(ctx, "file.read: view exceeds %u bytes (DYN_MAX_INPUT)", (unsigned)DYN_MAX_INPUT);
        view = 1;
    } else {
        if (!JS_IsUndefined(a0) && dyn_fah_index(ctx, a0, (double)DYN_MAX_INPUT, "length", &d) < 0)
            return JS_EXCEPTION;
        len = (size_t)d;
    }
    if (dyn_fah_pos(ctx, argc, argv, &pos, &has) < 0)
        return JS_EXCEPTION;
    if (h->closing)
        return dyn_fah_settled(ctx, EBADF, "read");
    j = dyn_fah_job(ctx, view ? DYN_FAH_READINTO : DYN_FAH_READ, 0);
    if (!j)
        return JS_EXCEPTION;
    j->u.h.buf = (uint8_t*)malloc(len ? len : 1);
    if (!j->u.h.buf) {
        dyn_fah_unjob(j);
        return JS_ThrowOutOfMemory(ctx);
    }
    j->u.h.len = len;
    j->u.h.pos = pos;
    j->u.h.haspos = has;
    return dyn_fah_queue(ctx, this_val, h, j, view ? a0 : JS_UNDEFINED);
}

static JSValue dyn_fah_write(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fah_t* h = (dyn_fah_t*)JS_GetOpaque2(ctx, this_val, dyn_fah_class_id);
    const uint8_t* data = NULL;
    const char* str = NULL;
    dyn_fa_job_t* j;
    size_t len = 0;
    int64_t pos;
    int has;

    if (!h)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "file.write: data is required");
    if (dyn_file_payload(ctx, argv[0], &data, &len, &str))
        return JS_EXCEPTION;
    j = dyn_fah_job(ctx, DYN_FAH_WRITE, 0);
    if (j)
        j->u.h.buf = (uint8_t*)malloc(len ? len : 1);
    if (!j || !j->u.h.buf) {
        if (str)
            JS_FreeCString(ctx, str);
        if (j)
            dyn_fah_unjob(j);
        return j ? JS_ThrowOutOfMemory(ctx) : JS_EXCEPTION;
    }
    if (len)
        memcpy(j->u.h.buf, data, len);
    if (str)
        JS_FreeCString(ctx, str);
    j->u.h.len = len;
    if (dyn_fah_pos(ctx, argc, argv, &pos, &has) < 0) {
        dyn_fah_unjob(j);
        return JS_EXCEPTION;
    }
    if (h->closing) {
        dyn_fah_unjob(j);
        return dyn_fah_settled(ctx, EBADF, "write");
    }
    j->u.h.pos = pos;
    j->u.h.haspos = has && !h->append;
    return dyn_fah_queue(ctx, this_val, h, j, JS_UNDEFINED);
}

static JSValue dyn_fah_seek(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fah_t* h = (dyn_fah_t*)JS_GetOpaque2(ctx, this_val, dyn_fah_class_id);
    double d;
    int wh = SEEK_SET;
    off_t r;

    if (!h)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &d, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(d >= -9007199254740991.0 && d <= 9007199254740991.0) || d != floor(d))
        return JS_ThrowRangeError(ctx, "file.seek: offset must be an integer in -(2^53-1)..2^53-1");
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (!s)
            return JS_EXCEPTION;
        if (!strcmp(s, "cur"))
            wh = SEEK_CUR;
        else if (!strcmp(s, "end"))
            wh = SEEK_END;
        else if (strcmp(s, "set")) {
            JS_FreeCString(ctx, s);
            return JS_ThrowTypeError(ctx, "file.seek: whence must be \"set\", \"cur\" or \"end\"");
        }
        JS_FreeCString(ctx, s);
    }
    if (h->closing)
        return dyn_fs_throw(ctx, EBADF, "seek", NULL);
    if (h->cur || h->qhead)
        return JS_ThrowTypeError(ctx, "file.seek: a call is still queued or in flight");
    r = lseek(h->fd, (off_t)d, wh);
    if (r < 0)
        return dyn_fs_throw(ctx, errno, "seek", NULL);
    return JS_NewInt64(ctx, (int64_t)r);
}

static JSValue dyn_fah_simple(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    dyn_fah_t* h = (dyn_fah_t*)JS_GetOpaque2(ctx, this_val, dyn_fah_class_id);
    dyn_fa_job_t* j;
    (void)argc;
    (void)argv;

    if (!h)
        return JS_EXCEPTION;
    if (h->closing)
        return dyn_fah_settled(ctx, magic == DYN_FAH_CLOSE ? 0 : EBADF, dyn_fah_ops[magic]);
    j = dyn_fah_job(ctx, magic, 0);
    if (!j)
        return JS_EXCEPTION;
    if (magic == DYN_FAH_CLOSE)
        h->closing = 1;
    return dyn_fah_queue(ctx, this_val, h, j, JS_UNDEFINED);
}

static JSValue dyn_fah_closed(JSContext* ctx, JSValueConst this_val)
{
    dyn_fah_t* h = (dyn_fah_t*)JS_GetOpaque2(ctx, this_val, dyn_fah_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, h->closing);
}

static int dyn_fah_flags(JSContext* ctx, JSValueConst o, dyn_fa_job_t* j)
{
    static const char* const keys[] = { "flags", "mode" };
    static const char* const names[] = { "r", "r+", "w", "w+", "wx", "a", "a+" };
    static const int bits[] = { O_RDONLY, O_RDWR, O_WRONLY | O_CREAT | O_TRUNC,
        O_RDWR | O_CREAT | O_TRUNC, O_WRONLY | O_CREAT | O_EXCL,
        O_WRONLY | O_CREAT | O_APPEND, O_RDWR | O_CREAT | O_APPEND };
    JSValue v;
    double d;
    size_t i;
    if (dyn_opts_strict(ctx, o, keys, 2))
        return -1;
    v = JS_GetPropertyStr(ctx, o, "flags");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        const char* s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        for (i = 0; i < countof(names); i++)
            if (!strcmp(s, names[i]))
                break;
        JS_FreeCString(ctx, s);
        if (i == countof(names)) {
            JS_ThrowTypeError(ctx, "file.open: flags must be one of r, r+, w, w+, wx, a, a+");
            return -1;
        }
        j->u.h.flags = bits[i];
    }
    v = JS_GetPropertyStr(ctx, o, "mode");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int rc = dyn_fah_index(ctx, v, 4095.0, "mode", &d);
        JS_FreeValue(ctx, v);
        if (rc < 0)
            return -1;
        j->u.h.mode = (int32_t)d;
    }
    return 0;
}

static JSValue dyn_fah_open(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_fa_job_t* j;
    size_t plen = 0;
    const char* p;
    JSValue pr;
    int armed;
    (void)this_val;

    p = dyn_path_arg_len(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "path", &plen);
    if (!p)
        return JS_EXCEPTION;
    j = dyn_fah_job(ctx, DYN_FAH_OPEN, plen + 1);
    if (!j) {
        dyn_path_unborrow(ctx, p);
        return JS_EXCEPTION;
    }
    j->u.h.path = (char*)(j + 1);
    memcpy(j->u.h.path, p, plen);
    j->u.h.path[plen] = 0;
    dyn_path_unborrow(ctx, p);
    j->u.h.flags = O_RDONLY;
    j->u.h.mode = 0666;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (!JS_IsObject(argv[1])) {
            free(j);
            return JS_ThrowTypeError(ctx, "file.open: options must be an object");
        }
        if (dyn_fah_flags(ctx, argv[1], j) < 0) {
            free(j);
            return JS_EXCEPTION;
        }
    }
    pr = dyn_fa_arm(ctx, j, &armed);
    if (armed && dyn_fa_offload(j) < 0)
        dyn_fa_refuse(ctx, j);
    return pr;
}

static const JSCFunctionListEntry dyn_fah_proto[] = {
    JS_CFUNC_DEF("read", 0, dyn_fah_read),
    JS_CFUNC_DEF("write", 1, dyn_fah_write),
    JS_CFUNC_DEF("seek", 1, dyn_fah_seek),
    JS_CFUNC_MAGIC_DEF("sync", 0, dyn_fah_simple, DYN_FAH_SYNC),
    JS_CFUNC_MAGIC_DEF("stat", 0, dyn_fah_simple, DYN_FAH_STAT),
    JS_CFUNC_MAGIC_DEF("close", 0, dyn_fah_simple, DYN_FAH_CLOSE),
    JS_CFUNC_MAGIC_DEF("[Symbol.asyncDispose]", 0, dyn_fah_simple, DYN_FAH_CLOSE),
    JS_CGETSET_DEF("closed", dyn_fah_closed, NULL),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "FileHandle", JS_PROP_CONFIGURABLE),
};

static JSValue dyn_fab_iter(JSContext* ctx, JSValue value, int done)
{
    JSValue r = JS_NewObject(ctx);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, value);
        return r;
    }
    if (JS_DefinePropertyValueStr(ctx, r, "value", value, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, r, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

static JSValue dyn_fab_str(JSContext* ctx, dyn_fah_op_t* o)
{
    return JS_NewStringLen(ctx, o->out ? (const char*)o->out : "", o->outlen);
}

static JSValue dyn_fab_result(JSContext* ctx, dyn_fa_job_t* j, int* bad)
{
    dyn_fah_op_t* o = &j->u.h;
    JSValue v;
    if (o->err == EFBIG)
        return JS_ThrowRangeError(ctx, "file.%s: %s exceeds %u bytes (DYN_MAX_INPUT)", j->name,
            o->kind == DYN_FAH_RLINE || o->kind == DYN_FAH_RNEXT ? "line" : "read", (unsigned)DYN_MAX_INPUT);
    if (o->err == ENOMEM)
        return JS_ThrowOutOfMemory(ctx);
    if (o->err)
        return dyn_fs_throw(ctx, o->err, j->name, NULL);
    switch (o->kind) {
    case DYN_FAH_RTEXT:
        v = dyn_fab_str(ctx, o);
        break;
    case DYN_FAH_RLINE:
        v = o->isnull ? JS_NULL : dyn_fab_str(ctx, o);
        break;
    case DYN_FAH_RNEXT:
        v = o->isnull ? JS_UNDEFINED : dyn_fab_str(ctx, o);
        if (!JS_IsException(v))
            v = dyn_fab_iter(ctx, v, o->isnull);
        break;
    case DYN_FAH_RBYTES:
        v = dyn_nat_u8_copy(ctx, o->out ? o->out : (const uint8_t*)"", o->outlen);
        break;
    case DYN_FAH_RINTO:
        free(o->buf);
        o->buf = o->out;
        o->got = o->outlen;
        o->out = NULL;
        return dyn_fah_into(ctx, o, bad);
    case DYN_FAH_RRET:
        v = dyn_fab_iter(ctx, JS_UNDEFINED, 1);
        break;
    case DYN_FAH_WPUT:
        v = JS_NewInt64(ctx, (int64_t)o->len);
        break;
    default:
        v = JS_UNDEFINED;
        break;
    }
    if (!JS_IsException(v))
        *bad = 0;
    return v;
}

static JSValue dyn_fab_now(JSContext* ctx, JSValue v)
{
    JSValue funcs[2], p, r;
    int bad = JS_IsException(v);
    if (bad) {
        if (JS_HasUncatchableException(ctx))
            return JS_EXCEPTION;
        v = JS_GetException(ctx);
    }
    p = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(p)) {
        JS_FreeValue(ctx, v);
        return p;
    }
    r = JS_Call(ctx, funcs[bad], JS_UNDEFINED, 1, (JSValueConst*)&v);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return p;
}

static int dyn_fab_fast(JSContext* ctx, dyn_fabuf_t* b, int magic, int64_t want,
    const uint8_t* data, size_t len, JSValue* out)
{
    uint8_t* s = b->p + b->start;
    size_t avail = b->end - b->start, n, skip;
    uint8_t* nl;
    JSValue v;
    if (magic == DYN_FAH_WPUT) {
        if (len >= b->cap || len > b->cap - b->end)
            return 0;
        if (len) {
            memcpy(b->p + b->end, data, len);
            b->end += len;
            b->dirty = 1;
        }
        *out = JS_NewInt64(ctx, (int64_t)len);
        return 1;
    }
    if (magic == DYN_FAH_RLINE || magic == DYN_FAH_RNEXT) {
        nl = avail ? (uint8_t*)memchr(s, '\n', avail) : NULL;
        if (!nl)
            return 0;
        n = (size_t)(nl - s);
        skip = n + 1;
        if (n && s[n - 1] == '\r')
            n--;
        v = dyn_utf8_lossy_string(ctx, s, n);
        if (!JS_IsException(v) && magic == DYN_FAH_RNEXT)
            v = dyn_fab_iter(ctx, v, 0);
    } else if ((magic == DYN_FAH_RTEXT || magic == DYN_FAH_RBYTES) && want > 0 && (uint64_t)want <= avail) {
        skip = (size_t)want;
        v = magic == DYN_FAH_RTEXT ? dyn_utf8_lossy_string(ctx, s, skip) : dyn_nat_u8_copy(ctx, s, skip);
    } else {
        return 0;
    }
    if (!JS_IsException(v))
        b->start += skip;
    *out = v;
    return 1;
}

static JSValue dyn_fab_call(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    int rd = magic <= DYN_FAH_RCLOSE;
    dyn_fah_t* h = (dyn_fah_t*)JS_GetOpaque2(ctx, this_val, rd ? dyn_fab_rclass_id : dyn_fab_wclass_id);
    JSValueConst a0 = argc > 0 ? argv[0] : JS_UNDEFINED;
    const uint8_t* data = NULL;
    const char* str = NULL;
    dyn_fa_job_t* j;
    int64_t want = -1;
    size_t len = 0;
    double d;
    JSValue fv;

    if (!h)
        return JS_EXCEPTION;
    if ((magic == DYN_FAH_RTEXT || magic == DYN_FAH_RBYTES) && !JS_IsUndefined(a0)) {
        if (dyn_fah_index(ctx, a0, (double)DYN_MAX_INPUT, "length", &d) < 0)
            return JS_EXCEPTION;
        want = (int64_t)d;
    }
    if (magic == DYN_FAH_RINTO) {
        size_t off, bpe, abn;
        JSValue ab;
        if (!JS_IsObject(a0))
            return JS_ThrowTypeError(ctx, "file.readInto: a TypedArray or DataView is required");
        if (JS_CheckWritableBuffer(ctx, a0) < 0)
            return JS_EXCEPTION;
        ab = JS_GetArrayBufferView(ctx, a0, &off, &len, &bpe);
        if (JS_IsException(ab))
            return JS_EXCEPTION;
        if (!JS_GetArrayBuffer(ctx, &abn, ab)) {
            JS_FreeValue(ctx, ab);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, ab);
        if (off > abn || len > abn - off)
            return JS_ThrowRangeError(ctx, "file.readInto: view out of bounds");
        if (len > DYN_MAX_INPUT)
            return JS_ThrowRangeError(ctx, "file.readInto: view exceeds %u bytes (DYN_MAX_INPUT)", (unsigned)DYN_MAX_INPUT);
        want = (int64_t)len;
    }
    if (magic == DYN_FAH_WPUT) {
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "file.write: data is required");
        if (dyn_file_payload(ctx, a0, &data, &len, &str))
            return JS_EXCEPTION;
    }
    if (h->closing) {
        if (str)
            JS_FreeCString(ctx, str);
        if (magic == DYN_FAH_RNEXT || magic == DYN_FAH_RRET)
            return dyn_fab_now(ctx, dyn_fab_iter(ctx, JS_UNDEFINED, 1));
        return dyn_fah_settled(ctx, magic == DYN_FAH_RCLOSE || magic == DYN_FAH_WCLOSE ? 0 : EBADF, dyn_fah_ops[magic]);
    }
    if (!h->cur && !h->qhead && h->fb && dyn_fab_fast(ctx, h->fb, magic, want, data, len, &fv)) {
        if (str)
            JS_FreeCString(ctx, str);
        return dyn_fab_now(ctx, fv);
    }
    j = dyn_fah_job(ctx, magic, 0);
    if (j && magic == DYN_FAH_WPUT) {
        j->u.h.buf = (uint8_t*)malloc(len ? len : 1);
        if (j->u.h.buf && len)
            memcpy(j->u.h.buf, data, len);
        j->u.h.len = len;
    }
    if (str)
        JS_FreeCString(ctx, str);
    if (!j)
        return JS_EXCEPTION;
    if (magic == DYN_FAH_WPUT && !j->u.h.buf) {
        dyn_fah_unjob(j);
        return JS_ThrowOutOfMemory(ctx);
    }
    j->u.h.want = want;
    if (dyn_fah_closer(magic))
        h->closing = 1;
    return dyn_fah_queue(ctx, this_val, h, j, magic == DYN_FAH_RINTO ? a0 : JS_UNDEFINED);
}

static JSValue dyn_fab_read_all(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    return dyn_fab_call(ctx, this_val, 0, NULL, DYN_FAH_RTEXT);
}

static JSValue dyn_fab_self(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)argc;
    (void)argv;
    if (!JS_GetOpaque2(ctx, this_val, dyn_fab_rclass_id))
        return JS_EXCEPTION;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_fab_closed(JSContext* ctx, JSValueConst this_val, int magic)
{
    dyn_fah_t* h = (dyn_fah_t*)JS_GetOpaque2(ctx, this_val, magic ? dyn_fab_wclass_id : dyn_fab_rclass_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, h->closing);
}

static int dyn_fab_int(JSContext* ctx, JSValueConst o, const char* key, int64_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, key);
    int rc = 0;
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt64(ctx, out, v))
        rc = -1;
    JS_FreeValue(ctx, v);
    return rc;
}

static JSValue dyn_fab_open(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "bufferSize", "preallocate", "append" };
    const char* name = magic == 2 ? "writer" : "reader";
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    dyn_fa_job_t* j;
    size_t plen = 0;
    const char* p;
    int64_t bs = 0, pre = 0;
    int append = 0, armed;
    JSValue pr;
    (void)this_val;

    if (!JS_IsUndefined(opts)) {
        if (!JS_IsObject(opts))
            return JS_ThrowTypeError(ctx, "file.%s: options must be an object", name);
        if (dyn_opts_strict(ctx, opts, keys, magic == 2 ? 3 : 1) || dyn_fab_int(ctx, opts, "bufferSize", &bs))
            return JS_EXCEPTION;
        if (magic == 2) {
            JSValue v;
            if (dyn_fab_int(ctx, opts, "preallocate", &pre))
                return JS_EXCEPTION;
            v = JS_GetPropertyStr(ctx, opts, "append");
            if (JS_IsException(v))
                return v;
            append = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (append < 0)
                return JS_EXCEPTION;
        }
    }
    p = dyn_path_arg_len(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "path", &plen);
    if (!p)
        return JS_EXCEPTION;
    j = dyn_fah_job(ctx, DYN_FAH_OPEN, plen + 1);
    if (!j) {
        dyn_path_unborrow(ctx, p);
        return JS_EXCEPTION;
    }
    j->name = name;
    j->u.h.path = (char*)(j + 1);
    memcpy(j->u.h.path, p, plen);
    j->u.h.path[plen] = 0;
    dyn_path_unborrow(ctx, p);
    j->u.h.bmode = magic;
    j->u.h.bsize = dyn_file_clamp_bufsize(bs);
    j->u.h.pos = pre > 0 ? pre : 0;
    j->u.h.flags = magic == 2 ? O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC) : O_RDONLY;
    j->u.h.mode = magic == 2 ? 0644 : 0;
    pr = dyn_fa_arm(ctx, j, &armed);
    if (armed && dyn_fa_offload(j) < 0)
        dyn_fa_refuse(ctx, j);
    return pr;
}

static void dyn_fab_fin(dyn_fah_t* h)
{
    if (!h)
        return;
    dyn_fab_free(h->fb, h->fd);
    if (h->fd >= 0)
        close(h->fd);
    free(h);
}

static void dyn_fab_rfin(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_fab_fin((dyn_fah_t*)JS_GetOpaque(val, dyn_fab_rclass_id));
}

static void dyn_fab_wfin(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_fab_fin((dyn_fah_t*)JS_GetOpaque(val, dyn_fab_wclass_id));
}

static const JSClassDef dyn_fab_rclass = {
    "AsyncFileReader",
    .finalizer = dyn_fab_rfin,
};

static const JSClassDef dyn_fab_wclass = {
    "AsyncFileWriter",
    .finalizer = dyn_fab_wfin,
};

static const JSCFunctionListEntry dyn_fab_rproto[] = {
    JS_CFUNC_MAGIC_DEF("read", 0, dyn_fab_call, DYN_FAH_RTEXT),
    JS_CFUNC_DEF("readAll", 0, dyn_fab_read_all),
    JS_CFUNC_MAGIC_DEF("readBytes", 0, dyn_fab_call, DYN_FAH_RBYTES),
    JS_CFUNC_MAGIC_DEF("readInto", 1, dyn_fab_call, DYN_FAH_RINTO),
    JS_CFUNC_MAGIC_DEF("readLine", 0, dyn_fab_call, DYN_FAH_RLINE),
    JS_CFUNC_MAGIC_DEF("next", 0, dyn_fab_call, DYN_FAH_RNEXT),
    JS_CFUNC_MAGIC_DEF("return", 0, dyn_fab_call, DYN_FAH_RRET),
    JS_CFUNC_MAGIC_DEF("close", 0, dyn_fab_call, DYN_FAH_RCLOSE),
    JS_CFUNC_MAGIC_DEF("[Symbol.asyncDispose]", 0, dyn_fab_call, DYN_FAH_RCLOSE),
    JS_CFUNC_DEF("lines", 0, dyn_fab_self),
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, dyn_fab_self),
    JS_CGETSET_MAGIC_DEF("closed", dyn_fab_closed, NULL, 0),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "AsyncFileReader", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry dyn_fab_wproto[] = {
    JS_CFUNC_MAGIC_DEF("write", 1, dyn_fab_call, DYN_FAH_WPUT),
    JS_CFUNC_MAGIC_DEF("flush", 0, dyn_fab_call, DYN_FAH_WFLUSH),
    JS_CFUNC_MAGIC_DEF("sync", 0, dyn_fab_call, DYN_FAH_WSYNC),
    JS_CFUNC_MAGIC_DEF("close", 0, dyn_fab_call, DYN_FAH_WCLOSE),
    JS_CFUNC_MAGIC_DEF("[Symbol.asyncDispose]", 0, dyn_fab_call, DYN_FAH_WCLOSE),
    JS_CGETSET_MAGIC_DEF("closed", dyn_fab_closed, NULL, 1),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "AsyncFileWriter", JS_PROP_CONFIGURABLE),
};

static int dyn_fab_class(JSContext* ctx, JSClassID* id, const JSClassDef* def,
    const JSCFunctionListEntry* fl, int n)
{
    JSValue proto;
    JS_NewClassID(id);
    if (JS_NewClass(JS_GetRuntime(ctx), *id, def) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, proto, fl, n) < 0) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetClassProto(ctx, *id, proto);
    return 0;
}

static int dyn_fab_init(JSContext* ctx, JSValueConst o)
{
    static const char* const names[] = { "reader", "writer" };
    int i;
    if (dyn_fab_class(ctx, &dyn_fab_rclass_id, &dyn_fab_rclass, dyn_fab_rproto, (int)countof(dyn_fab_rproto)) < 0
        || dyn_fab_class(ctx, &dyn_fab_wclass_id, &dyn_fab_wclass, dyn_fab_wproto, (int)countof(dyn_fab_wproto)) < 0)
        return -1;
    for (i = 0; i < 2; i++) {
        JSValue fn = JS_NewCFunctionMagic(ctx, dyn_fab_open, names[i], 1, JS_CFUNC_generic_magic, i + 1);
        if (JS_IsException(fn) || JS_SetPropertyStr(ctx, o, names[i], fn) < 0)
            return -1;
    }
    return 0;
}

int dyn_fah_init(JSContext* ctx, JSValueConst o)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, fn;
    JS_NewClassID(&dyn_fah_class_id);
    if (JS_NewClass(rt, dyn_fah_class_id, &dyn_fah_class) < 0)
        return -1;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, proto, dyn_fah_proto, (int)countof(dyn_fah_proto)) < 0) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetClassProto(ctx, dyn_fah_class_id, proto);
    fn = JS_NewCFunction(ctx, dyn_fah_open, "open", 1);
    if (JS_IsException(fn))
        return -1;
    if (JS_SetPropertyStr(ctx, o, "open", fn) < 0)
        return -1;
    return dyn_fab_init(ctx, o);
}

#endif
