#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_APP_UPLOAD_CHUNK (256u * 1024u)
#define DYN_APP_UPLOAD_MAXW 4

typedef struct dyn_app_upload_w {
    dyn_app_upload_t* u;
    dyn_app_conn_t* c;
    uint8_t* base;
    uint8_t* buf;
    int64_t off;
    size_t len;
} dyn_app_upload_w_t;

static const char* const http_upload_keys[] = { "dir", "maxFileSize", "allow" };

static unsigned long dyn_app_upload_seq;

static void dyn_app_upload_finish(dyn_app_conn_t* c)
{
    dyn_app_upload_t* u = c->up;
    JSContext* ctx = c->app->ctx;
    JSValue pathv, meta, r;
    JSValueConst args[2];
    c->up = NULL;
    close(u->fd);
    pathv = JS_NewString(ctx, u->path);
    meta = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, meta, "size", JS_NewInt64(ctx, u->size), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta, "contentType", JS_NewString(ctx, u->ctype), JS_PROP_C_W_E);
    args[0] = pathv;
    args[1] = meta;
    r = JS_Call(ctx, u->handler, JS_UNDEFINED, 2, args);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_send_err(c, 500, "{\"error\":\"handler failed\"}");
    } else {
        const char* base = strrchr(u->path, '/');
        JSValue reply, pj;
        const char* ps;
        if (!JS_IsObject(r) || JS_IsFunction(ctx, r)) {
            reply = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, reply, "ok", JS_TRUE, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, reply, "id", JS_NewString(ctx, base ? base + 1 : u->path), JS_PROP_C_W_E);
        } else
            reply = JS_DupValue(ctx, r);
        pj = JS_JSONStringify(ctx, reply, JS_UNDEFINED, JS_UNDEFINED);
        ps = JS_IsException(pj) || JS_IsUndefined(pj) ? NULL : JS_ToCString(ctx, pj);
        if (ps)
            dyn_app_send_json(c, 200, ps, strlen(ps));
        else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            dyn_app_send_err(c, 500, "{\"error\":\"handler result is not JSON\"}");
        }
        if (ps)
            JS_FreeCString(ctx, ps);
        JS_FreeValue(ctx, pj);
        JS_FreeValue(ctx, reply);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, pathv);
    JS_FreeValue(ctx, meta);
    JS_FreeValue(ctx, u->handler);
    free(u->path);
    free(u);
}

static void dyn_app_upload_wdone(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud);

static void dyn_app_upload_resume(dyn_app_conn_t* c)
{
    dyn_app_upload_t* u = c->up;
    struct dyn_aio* aio = c->app->aio;

    if (u->err) {
        dyn_app_conn_close(c);
        return;
    }
    while (u->remaining > 0 && u->wbusy < DYN_APP_UPLOAD_MAXW) {
        size_t take = dyn_iobuf_rlen(&c->in);
        dyn_app_upload_w_t* w;
        if (take == 0)
            return;
        if ((int64_t)take > u->remaining)
            take = (size_t)u->remaining;
        if (take > DYN_APP_UPLOAD_CHUNK)
            take = DYN_APP_UPLOAD_CHUNK;
        w = (dyn_app_upload_w_t*)malloc(sizeof(*w));
        if (w)
            w->base = (uint8_t*)malloc(take);
        if (!w || !w->base) {
            if (w)
                free(w->base);
            free(w);
            u->err = -ENOMEM;
            dyn_app_conn_close(c);
            return;
        }
        w->buf = w->base;
        memcpy(w->base, dyn_iobuf_rdata(&c->in), take);
        w->u = u;
        w->c = c;
        w->off = u->off;
        w->len = take;
        u->off += take;
        u->remaining -= take;
        dyn_iobuf_consume(&c->in, take);
        u->wbusy++;
        c->refs++;
        dyn_aio_write(aio, u->fd, w->buf, take, w->off,
            dyn_app_upload_wdone, w);
        if (!c->up || c->closed)
            return;
    }
    if (u->remaining == 0 && u->wbusy == 0)
        dyn_app_upload_finish(c);
}

static void dyn_app_upload_wdone(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* ud)
{
    dyn_app_upload_w_t* w = (dyn_app_upload_w_t*)ud;
    dyn_app_upload_t* u = w->u;
    dyn_app_conn_t* c = w->c;
    (void)aio;
    (void)buf;
    (void)len;

    if (res == -EINTR) {
        if (dyn_aio_write(c->app->aio, u->fd, w->buf, w->len, w->off,
                dyn_app_upload_wdone, w)
            == 0)
            return;
        res = -EIO;
    }
    if (res > 0 && (size_t)res < w->len) {
        w->buf += res;
        w->off += res;
        w->len -= (size_t)res;
        if (dyn_aio_write(c->app->aio, u->fd, w->buf, w->len, w->off,
                dyn_app_upload_wdone, w)
            == 0)
            return;
        res = -EIO;
    }
    if (res == 0)
        res = -EIO;
    free(w->base);
    free(w);
    u->wbusy--;
    if (res < 0 && !u->err)
        u->err = res;
    if (c->closed) {
        dyn_app_conn_unref(c);
        return;
    }
    dyn_app_upload_resume(c);
    dyn_app_conn_unref(c);
}

void dyn_app_upload_drain(dyn_app_conn_t* c)
{
    dyn_app_upload_resume(c);
}

void dyn_app_upload_start(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* base, size_t head_len, int64_t clen)
{
    JSContext* ctx = c->app->ctx;
    const char* ct;
    size_t ctlen = 0;
    int64_t cap = rt->max_file > 0 ? rt->max_file : (int64_t)(16 * 1024 * 1024);
    char ctbuf[128], fpath[2048];
    dyn_app_upload_t* u;
    int fd;

    if (clen < 0 || clen > cap) {
        dyn_app_reject_linger(c, 413, "{\"error\":\"too large\"}", clen);
        return;
    }
    ct = dyn_req_header(base, head_len, "content-type", &ctlen);
    {
        size_t n = ctlen < sizeof(ctbuf) - 1 ? ctlen : sizeof(ctbuf) - 1;
        char* sc;
        memcpy(ctbuf, ct ? ct : "", n);
        ctbuf[n] = 0;
        if ((sc = strchr(ctbuf, ';')) != NULL)
            *sc = 0;
        {
            size_t cl = strlen(ctbuf);
            while (cl && ctbuf[cl - 1] == ' ')
                ctbuf[--cl] = 0;
        }
    }
    if (rt->allow) {
        size_t k;
        int ok = 0;
        for (k = 0; k < rt->n_allow; k++)
            if (!strcasecmp(rt->allow[k], ctbuf)) {
                ok = 1;
                break;
            }
        if (!ok) {
            dyn_app_reject_linger(c, 415, "{\"error\":\"type not allowed\"}", clen);
            return;
        }
    }
    fd = -1;
    for (int attempt = 0; attempt < 64 && fd < 0; attempt++) {
        snprintf(fpath, sizeof(fpath), "%s/up_%ld_%lu", rt->dir, (long)getpid(),
            ++dyn_app_upload_seq);
        fd = open(fpath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    if (fd < 0) {
        dyn_app_send_err(c, 500, "{\"error\":\"open failed\"}");
        dyn_app_conn_close(c);
        return;
    }
    u = (dyn_app_upload_t*)calloc(1, sizeof(*u));
    if (!u) {
        close(fd);
        dyn_app_conn_close(c);
        return;
    }
    u->fd = fd;
    u->remaining = clen;
    u->size = clen;
    u->handler = JS_DupValue(ctx, rt->handler);
    u->path = strdup(fpath);
    if (!u->path) {
        JS_FreeValue(ctx, u->handler);
        free(u);
        close(fd);
        dyn_app_send_err(c, 500, "{\"error\":\"out of memory\"}");
        dyn_app_conn_close(c);
        return;
    }
    snprintf(u->ctype, sizeof(u->ctype), "%s", ctbuf);
    c->up = u;
    dyn_iobuf_consume(&c->in, head_len);
    c->hdr_scan_from = 0;
    c->hdr_counted = 0;
    c->hdr_lines = 0;
    dyn_app_upload_drain(c);
}

JSValue dyn_app_upload(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    const char *path = NULL, *dir = NULL;
    dyn_app_route_t* nr;
    char** allow = NULL;
    size_t n_allow = 0, k;
    int64_t maxf = 0;
    JSValue handler = JS_UNDEFINED, v;

    if (argc < 3 || !JS_IsObject(argv[1]) || !JS_IsFunction(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "upload(path, {dir,...}, handler)");
    if (dyn_opts_strict(ctx, argv[1], http_upload_keys, 3))
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    v = JS_GetPropertyStr(ctx, argv[1], "dir");
    dir = JS_IsUndefined(v) ? NULL
                            : dyn_path_borrow(ctx, v, "upload opts.dir", NULL);
    JS_FreeValue(ctx, v);
    if (!dir) {
        JS_FreeCString(ctx, path);
        if (JS_IsUndefined(v))
            return JS_ThrowTypeError(ctx, "upload: opts.dir required");
        return JS_EXCEPTION;
    }
    v = JS_GetPropertyStr(ctx, argv[1], "maxFileSize");
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &maxf, v)) {
            JS_FreeValue(ctx, v);
            JS_FreeCString(ctx, path);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, v);
    if (dyn_app_parse_allow(ctx, argv[1], &allow, &n_allow) < 0)
        goto ufail;
    handler = JS_DupValue(ctx, argv[2]);

    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        goto ufail;
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_ThrowOutOfMemory(ctx);
        goto ufail;
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(path);
    nr->type = APP_UPLOAD;
    nr->handler = handler;
    nr->dir = strdup(dir);
    nr->max_file = maxf;
    nr->allow = allow;
    nr->n_allow = n_allow;
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
ufail:
    for (k = 0; k < n_allow; k++)
        free(allow[k]);
    free(allow);
    JS_FreeValue(ctx, handler);
    JS_FreeCString(ctx, path);
    return JS_EXCEPTION;
}
#endif
