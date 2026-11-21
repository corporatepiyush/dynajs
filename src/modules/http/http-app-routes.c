#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_ROUTE_INDEX_MIN 16
#ifndef DYN_APP_STATIC_INLINE_MAX
#define DYN_APP_STATIC_INLINE_MAX 16384
#endif

static const char* const http_route_val_keys[] = { "status", "contentType", "body" };

static const char* const http_static_keys[] = { "maxFileSize", "allow" };

struct dyn_route_idx {
    uint32_t mask;
    uint32_t* slot;
    uint32_t* tag;
};

static uint64_t dyn_route_hash(const char* p)
{
    return dyn_ds_xxh64(p, strlen(p), 0);
}

dyn_route_idx_t* dyn_route_index_build(const dyn_route_t* routes, size_t n_routes)
{
    dyn_route_idx_t* m;
    size_t cap = 32, i;
    if (n_routes < DYN_ROUTE_INDEX_MIN || n_routes > UINT32_MAX / 4)
        return NULL;
    while (cap < n_routes * 2)
        cap <<= 1;
    m = (dyn_route_idx_t*)malloc(sizeof(*m) + cap * 2 * sizeof(uint32_t));
    if (!m)
        return NULL;
    m->mask = (uint32_t)cap - 1;
    m->slot = (uint32_t*)(void*)(m + 1);
    m->tag = m->slot + cap;
    memset(m->slot, 0, cap * sizeof(uint32_t));
    for (i = 0; i < n_routes; i++) {
        uint64_t h = dyn_route_hash(routes[i].path);
        uint32_t pos = (uint32_t)h & m->mask;
        while (m->slot[pos] && !(m->tag[pos] == (uint32_t)(h >> 32) && strcmp(routes[m->slot[pos] - 1].path, routes[i].path) == 0))
            pos = (pos + 1) & m->mask;
        m->slot[pos] = (uint32_t)i + 1;
        m->tag[pos] = (uint32_t)(h >> 32);
    }
    return m;
}

void dyn_route_index_free(dyn_route_idx_t* idx)
{
    free(idx);
}

const dyn_route_t* dyn_route_lookup(const dyn_route_t* routes,
    size_t n_routes, const dyn_route_idx_t* idx, const char* path)
{
    size_t i;
    if (idx) {
        uint64_t h = dyn_route_hash(path);
        uint32_t pos = (uint32_t)h & idx->mask, t = (uint32_t)(h >> 32);
        for (; idx->slot[pos]; pos = (pos + 1) & idx->mask)
            if (idx->tag[pos] == t && strcmp(routes[idx->slot[pos] - 1].path, path) == 0)
                return &routes[idx->slot[pos] - 1];
        return NULL;
    }
    for (i = 0; i < n_routes; i++) {
        if (strcmp(routes[i].path, path) == 0)
            return &routes[i];
    }
    return NULL;
}

struct dyn_app_ridx {
    size_t n;
    uint32_t mask;
    uint32_t* ex;
    uint32_t* ex_tag;
    uint32_t* dy;
    uint32_t* dy_tag;
    uint32_t* dy_tail;
    uint32_t* nxt;
    uint32_t* gen;
    uint32_t* mnt;
    uint32_t n_gen, n_mnt;
};

static const char* dyn_ridx_seg(const char* s, size_t* len)
{
    const char* e;
    while (*s == '/')
        s++;
    e = strchr(s, '/');
    *len = e ? (size_t)(e - s) : strlen(s);
    return s;
}

void dyn_app_ridx_free(dyn_app_t* app)
{
    free(app->ridx);
    app->ridx = NULL;
}

static dyn_app_ridx_t* dyn_app_ridx_get(dyn_app_t* app)
{
    dyn_app_ridx_t* x = app->ridx;
    size_t n = app->n_routes, cap = 32, i;
    if (x && x->n == n)
        return x;
    dyn_app_ridx_free(app);
    if (n < DYN_ROUTE_INDEX_MIN || n > UINT32_MAX / 4)
        return NULL;
    while (cap < n * 2)
        cap <<= 1;
    x = (dyn_app_ridx_t*)malloc(sizeof(*x) + (cap * 5 + n * 3) * sizeof(uint32_t));
    if (!x)
        return NULL;
    x->n = n;
    x->mask = (uint32_t)cap - 1;
    x->ex = (uint32_t*)(void*)(x + 1);
    x->ex_tag = x->ex + cap;
    x->dy = x->ex_tag + cap;
    x->dy_tag = x->dy + cap;
    x->dy_tail = x->dy_tag + cap;
    x->nxt = x->dy_tail + cap;
    x->gen = x->nxt + n;
    x->mnt = x->gen + n;
    x->n_gen = x->n_mnt = 0;
    memset(x->ex, 0, cap * sizeof(uint32_t));
    memset(x->dy, 0, cap * sizeof(uint32_t));
    for (i = 0; i < n; i++) {
        const dyn_app_route_t* rt = &app->routes[i];
        x->nxt[i] = 0;
        if (rt->type == APP_DYN) {
            size_t sl;
            const char* sg = dyn_ridx_seg(rt->dyn_pattern, &sl);
            uint64_t h;
            uint32_t pos;
            if (sg[0] == ':' || sg[0] == '*') {
                x->gen[x->n_gen++] = (uint32_t)i;
                continue;
            }
            h = dyn_ds_xxh64(sg, sl, 0);
            pos = (uint32_t)h & x->mask;
            for (; x->dy[pos]; pos = (pos + 1) & x->mask) {
                size_t ol;
                const char* og;
                if (x->dy_tag[pos] != (uint32_t)(h >> 32))
                    continue;
                og = dyn_ridx_seg(app->routes[x->dy[pos] - 1].dyn_pattern, &ol);
                if (ol == sl && memcmp(og, sg, sl) == 0)
                    break;
            }
            if (x->dy[pos]) {
                x->nxt[x->dy_tail[pos] - 1] = (uint32_t)i + 1;
                x->dy_tail[pos] = (uint32_t)i + 1;
            } else {
                x->dy[pos] = x->dy_tail[pos] = (uint32_t)i + 1;
                x->dy_tag[pos] = (uint32_t)(h >> 32);
            }
        } else if (rt->type == APP_STATIC || rt->type == APP_PROXY) {
            x->mnt[x->n_mnt++] = (uint32_t)i;
        } else {
            uint64_t h = dyn_route_hash(rt->path);
            uint32_t pos = (uint32_t)h & x->mask;
            for (; x->ex[pos]; pos = (pos + 1) & x->mask)
                if (x->ex_tag[pos] == (uint32_t)(h >> 32)
                    && strcmp(app->routes[x->ex[pos] - 1].path, rt->path) == 0)
                    break;
            if (!x->ex[pos]) {
                x->ex[pos] = (uint32_t)i + 1;
                x->ex_tag[pos] = (uint32_t)(h >> 32);
            }
        }
    }
    app->ridx = x;
    return x;
}

static int dyn_app_mount_hit(const dyn_app_route_t* rt, const char* path)
{
    size_t plen = strlen(rt->path);
    int slash_end = plen > 0 && rt->path[plen - 1] == '/';
    return strncmp(path, rt->path, plen) == 0
        && (slash_end || path[plen] == '/' || path[plen] == '\0');
}

const dyn_app_route_t* dyn_app_route_find(dyn_app_t* app, const char* path,
    const dyn_app_route_t** pmount)
{
    dyn_app_ridx_t* x = dyn_app_ridx_get(app);
    const dyn_app_route_t* mount = NULL;
    size_t r;
    if (x) {
        uint64_t h = dyn_route_hash(path);
        uint32_t pos = (uint32_t)h & x->mask, t = (uint32_t)(h >> 32);
        for (; x->ex[pos]; pos = (pos + 1) & x->mask)
            if (x->ex_tag[pos] == t && strcmp(app->routes[x->ex[pos] - 1].path, path) == 0) {
                *pmount = NULL;
                return &app->routes[x->ex[pos] - 1];
            }
        for (r = 0; r < x->n_mnt; r++)
            if (dyn_app_mount_hit(&app->routes[x->mnt[r]], path)) {
                *pmount = &app->routes[x->mnt[r]];
                return NULL;
            }
        *pmount = NULL;
        return NULL;
    }
    for (r = 0; r < app->n_routes; r++) {
        const dyn_app_route_t* rt = &app->routes[r];
        if (rt->type == APP_DYN)
            continue;
        if (rt->type == APP_STATIC || rt->type == APP_PROXY) {
            if (!mount && dyn_app_mount_hit(rt, path))
                mount = rt;
        } else if (strcmp(rt->path, path) == 0) {
            *pmount = mount;
            return rt;
        }
    }
    *pmount = mount;
    return NULL;
}

void dyn_app_dyn_first(dyn_app_t* app, const char* path, dyn_app_dyn_it_t* it)
{
    dyn_app_ridx_t* x = dyn_app_ridx_get(app);
    size_t sl;
    const char* sg;
    uint64_t h;
    uint32_t pos;
    it->g = 0;
    if (!x) {
        it->lin = 1;
        it->next = 0;
        return;
    }
    it->lin = 0;
    it->next = 0;
    sg = dyn_ridx_seg(path, &sl);
    h = dyn_ds_xxh64(sg, sl, 0);
    for (pos = (uint32_t)h & x->mask; x->dy[pos]; pos = (pos + 1) & x->mask) {
        size_t ol;
        const char* og;
        if (x->dy_tag[pos] != (uint32_t)(h >> 32))
            continue;
        og = dyn_ridx_seg(app->routes[x->dy[pos] - 1].dyn_pattern, &ol);
        if (ol == sl && memcmp(og, sg, sl) == 0) {
            it->next = x->dy[pos];
            break;
        }
    }
}

long dyn_app_dyn_next(const dyn_app_t* app, dyn_app_dyn_it_t* it)
{
    const dyn_app_ridx_t* x = app->ridx;
    uint32_t b, g;
    if (it->lin) {
        while (it->next < app->n_routes) {
            uint32_t i = it->next++;
            if (app->routes[i].type == APP_DYN)
                return (long)i;
        }
        return -1;
    }
    b = it->next ? it->next - 1 : UINT32_MAX;
    g = it->g < x->n_gen ? x->gen[it->g] : UINT32_MAX;
    if (b == UINT32_MAX && g == UINT32_MAX)
        return -1;
    if (b < g) {
        it->next = x->nxt[b];
        return (long)b;
    }
    it->g++;
    return (long)g;
}

int dyn_route_copy(JSContext* ctx, const char* path, JSValueConst val,
    dyn_route_t* r)
{
    const char *body = NULL, *ct = NULL;
    size_t body_len = 0;
    int32_t status = 200;
    int body_owned = 0;

    r->path = NULL;
    r->content_type = NULL;
    r->body = NULL;
    r->body_len = 0;
    r->status = 200;

    {
        const char* p;
        for (p = path; *p; p++)
            if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f) {
                JS_ThrowTypeError(ctx,
                    "route path must not contain control characters");
                return -1;
            }
    }
    if (JS_IsString(val)) {
        body = JS_ToCStringLen(ctx, &body_len, val);
        if (!body)
            return -1;
        ct = NULL;
    } else if (JS_IsFunction(ctx, val)) {
        JS_ThrowTypeError(ctx,
            "route value must be a string or {status, contentType, body}; "
            "dyna:http servers serve static routes only -- use App.rpc/App "
            "handlers for dynamic responses");
        return -1;
    } else if (JS_IsObject(val)) {
        if (dyn_opts_strict(ctx, val, http_route_val_keys, 3))
            return -1;
        JSValue vs = JS_GetPropertyStr(ctx, val, "status");
        JSValue vc = JS_GetPropertyStr(ctx, val, "contentType");
        JSValue vb = JS_GetPropertyStr(ctx, val, "body");
        if (!JS_IsUndefined(vs) && !JS_IsNull(vs)) {
            if (JS_ToInt32(ctx, &status, vs)) {
                JS_FreeValue(ctx, vs);
                JS_FreeValue(ctx, vc);
                JS_FreeValue(ctx, vb);
                return -1;
            }
        }
        JS_FreeValue(ctx, vs);
        if (status < 100 || status > 599) {
            if (ct)
                JS_FreeCString(ctx, ct);
            if (body)
                JS_FreeCString(ctx, body);
            JS_FreeValue(ctx, vc);
            JS_FreeValue(ctx, vb);
            JS_ThrowRangeError(ctx, "http.route: status must be in [100, 599]");
            return -1;
        }
        if (!JS_IsUndefined(vc) && !JS_IsNull(vc)) {
            size_t ct_len = 0;
            ct = JS_ToCStringLen(ctx, &ct_len, vc);
            if (!ct) {
                JS_FreeValue(ctx, vc);
                JS_FreeValue(ctx, vb);
                return -1;
            }
            {
                if (!httpk_value_gen_ok(ct, ct_len)) {
                    JS_FreeCString(ctx, ct);
                    JS_FreeValue(ctx, vc);
                    JS_FreeValue(ctx, vb);
                    JS_ThrowTypeError(ctx,
                        "route contentType must not contain control "
                        "characters");
                    return -1;
                }
                if (ct_len > DYN_HTTP_CT_MAX) {
                    JS_FreeCString(ctx, ct);
                    JS_FreeValue(ctx, vc);
                    JS_FreeValue(ctx, vb);
                    JS_ThrowRangeError(ctx,
                        "route contentType must be at most %d bytes",
                        DYN_HTTP_CT_MAX);
                    return -1;
                }
            }
        }
        JS_FreeValue(ctx, vc);
        if (!JS_IsUndefined(vb) && !JS_IsNull(vb)) {
            if (JS_IsString(vb)) {
                body = JS_ToCStringLen(ctx, &body_len, vb);
                if (!body) {
                    if (ct)
                        JS_FreeCString(ctx, ct);
                    JS_FreeValue(ctx, vb);
                    return -1;
                }
            } else {
                size_t off = 0, total = 0, bpe = 0;
                uint8_t* base;
                uint8_t* nb;
                JSValue ab = JS_GetArrayBufferView(ctx, vb, &off, &body_len,
                    &bpe);
                if (JS_IsException(ab)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    base = JS_GetArrayBuffer(ctx, &total, vb);
                    if (!base) {
                        JS_FreeValue(ctx, vb);
                        if (ct)
                            JS_FreeCString(ctx, ct);
                        JS_ThrowTypeError(ctx,
                            "route body must be a string or a byte view "
                            "(Uint8Array, ArrayBuffer)");
                        return -1;
                    }
                    off = 0;
                    body_len = total;
                } else {
                    base = JS_GetArrayBuffer(ctx, &total, ab);
                    JS_FreeValue(ctx, ab);
                    if (!base) {
                        JS_FreeValue(ctx, vb);
                        if (ct)
                            JS_FreeCString(ctx, ct);
                        return -1;
                    }
                }
                nb = (uint8_t*)malloc(body_len > 0 ? body_len : 1);
                if (!nb) {
                    JS_FreeValue(ctx, vb);
                    if (ct)
                        JS_FreeCString(ctx, ct);
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                memcpy(nb, base + off, body_len);
                body = (const char*)nb;
                body_owned = 1;
            }
        }
        JS_FreeValue(ctx, vb);
    } else {
        JS_ThrowTypeError(ctx, "route value must be a string or an object");
        return -1;
    }

    r->status = status;
    r->path = strdup(path);
    r->content_type = strdup(ct ? ct : "text/plain");
    r->body = (char*)malloc(body_len + 1);
    if (!r->path || !r->content_type || !r->body) {
        if (body) {
            if (body_owned)
                free(DYN_UNCONST(body));
            else
                JS_FreeCString(ctx, body);
        }
        if (ct)
            JS_FreeCString(ctx, ct);
        free(r->path);
        free(r->content_type);
        free(r->body);
        r->path = r->content_type = r->body = NULL;
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (body_len > 0)
        memcpy(r->body, body, body_len);
    r->body[body_len] = '\0';
    r->body_len = body_len;
    if (body) {
        if (body_owned)
            free(DYN_UNCONST(body));
        else
            JS_FreeCString(ctx, body);
    }
    if (ct)
        JS_FreeCString(ctx, ct);
    return 0;
}

static int dyn_app_allowed(const dyn_app_route_t* rt, const char* path)
{
    size_t k;
    const char *dot, *ct;
    if (!rt->allow)
        return 1;
    dot = strrchr(path, '.');
    ct = dyn_app_content_type(path);
    for (k = 0; k < rt->n_allow; k++) {
        if (dot && !strcasecmp(rt->allow[k], dot))
            return 1;
        if (!strcasecmp(rt->allow[k], ct))
            return 1;
    }
    return 0;
}

static int dyn_app_static_segments(const char* sub)
{
    const char* p = sub;
    while (*p) {
        const char* seg;
        size_t len;
        while (*p == '/')
            p++;
        seg = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - seg);
        if (len == 0)
            continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.')
            return 403;
        if (seg[0] == '.' && !(len == 11 && memcmp(seg, ".well-known", 11) == 0))
            return 404;
    }
    return 0;
}

void dyn_app_serve_static(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    const char* reqpath,
    const char* base, size_t head_len)
{
    const char* sub = reqpath + strlen(rt->path);
    char fpath[2048];
    struct stat st;
    int64_t cap;
    dyn_iobuf_t out;
    char head[512];
    int n, ffd;
    int head_only;
    int64_t rstart = -1, rend = -1;
    int range_unsat = 0;

    head_only = (head_len >= 5 && memcmp(base, "HEAD ", 5) == 0);

    {
        int seg_verdict = dyn_app_static_segments(sub);
        if (seg_verdict) {
            dyn_app_send_err(c, seg_verdict, seg_verdict == 403 ? "{\"error\":\"forbidden\"}" : "{\"error\":\"not found\"}");
            return;
        }
    }
    while (*sub == '/')
        sub++;
    snprintf(fpath, sizeof(fpath), "%s/%s", rt->dir, *sub ? sub : "index.html");

    {
        int st_code = 404;
        ffd = dyn_static_open(rt->sroot, *sub ? sub : "index.html", &st_code);
        if (ffd < 0) {
            dyn_app_send_err(c, st_code, st_code == 403 ? "{\"error\":\"forbidden\"}" : "{\"error\":\"not found\"}");
            return;
        }
    }
    if (fstat(ffd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(ffd);
        dyn_app_send_err(c, 404, "{\"error\":\"not found\"}");
        return;
    }
    cap = rt->max_file > 0 ? rt->max_file : (int64_t)(32 * 1024 * 1024);
    if ((int64_t)st.st_size > cap) {
        close(ffd);
        dyn_app_send_err(c, 413, "{\"error\":\"too large\"}");
        return;
    }
    if (!dyn_app_allowed(rt, fpath)) {
        close(ffd);
        dyn_app_send_err(c, 403, "{\"error\":\"type not allowed\"}");
        return;
    }
    {
        size_t rv_len = 0;
        const char* rv = dyn_req_header(base, head_len, "range", &rv_len);
        if (rv && rv_len > 6 && strncasecmp(rv, "bytes=", 6) == 0 && !memchr(rv, ',', rv_len)) {
            size_t b = 6, e = rv_len;
            int64_t S = -1, E = -1;
            while (b < e && (rv[b] == ' ' || rv[b] == '\t'))
                b++;
            while (e > b && (rv[e - 1] == ' ' || rv[e - 1] == '\t'))
                e--;
            if (dyn_hm_one_range(rv, b, e, (int64_t)st.st_size, &S, &E) == 0) {
                rstart = S;
                rend = E;
            } else {
                range_unsat = 1;
            }
        }
    }
    if (range_unsat) {
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 416 Range Not Satisfiable\r\n%s"
            "Content-Range: bytes */%lld\r\nContent-Length: 0\r\n"
            "Connection: %s\r\n\r\n",
            dyn_http_date(NULL, 0), (long long)st.st_size, dyn_app_conn_token(c));
        close(ffd);
        if (n > 0) {
            dyn_iobuf_init(&out);
            dyn_iobuf_append(&out, head, (size_t)n);
            c->refs++;
            if (dyn_aio_send(c->app->aio, c->fd, out.data, out.len, DYN_AIO_HOLD,
                    dyn_app_out_last, c)
                < 0)
                dyn_app_out_last(NULL, -ECONNRESET, NULL, 0, c);
            dyn_iobuf_free(&out);
            dyn_app_met_response(c, 0);
        }
        return;
    }
    if (rstart >= 0)
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 206 Partial Content\r\n%sContent-Type: %s\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Accept-Ranges: bytes\r\nContent-Length: %lld\r\n"
            "Connection: %s\r\n\r\n",
            dyn_http_date(NULL, 0), dyn_app_content_type(fpath), (long long)rstart,
            (long long)rend, (long long)st.st_size,
            (long long)(rend - rstart + 1), dyn_app_conn_token(c));
    else
        n = snprintf(head, sizeof(head),
            "HTTP/1.1 200 OK\r\n%sContent-Type: %s\r\n"
            "Accept-Ranges: bytes\r\nContent-Length: %lld\r\n"
            "Connection: %s\r\n\r\n",
            dyn_http_date(NULL, 0), dyn_app_content_type(fpath), (long long)st.st_size, dyn_app_conn_token(c));
    dyn_iobuf_init(&out);
    if (n > 0 && !head_only) {
        off_t boff = rstart >= 0 ? (off_t)rstart : 0;
        size_t blen = rstart >= 0 ? (size_t)(rend - rstart + 1) : (size_t)st.st_size;
        if (blen <= DYN_APP_STATIC_INLINE_MAX && dyn_iobuf_reserve(&out, (size_t)n + blen) == 0) {
            ssize_t got;
            memcpy(out.data, head, (size_t)n);
            got = blen ? pread(ffd, out.data + n, blen, boff) : 0;
            if (got == (ssize_t)blen) {
                out.len = (size_t)n + blen;
                close(ffd);
                c->refs++;
                if (dyn_aio_send(c->app->aio, c->fd, out.data, out.len, DYN_AIO_HOLD,
                        dyn_app_out_last, c)
                    < 0)
                    dyn_app_out_last(NULL, -ECONNRESET, NULL, 0, c);
                dyn_iobuf_free(&out);
                dyn_app_met_response(c, (double)blen);
                return;
            }
        }
        out.len = 0;
    }
    if (n > 0) {
        dyn_iobuf_append(&out, head, (size_t)n);
        c->refs++;
        if (dyn_aio_send(c->app->aio, c->fd, out.data, out.len, DYN_AIO_HOLD,
                head_only ? dyn_app_out_last : dyn_app_out_done, c)
            < 0)
            (head_only ? dyn_app_out_last : dyn_app_out_done)(NULL, -ECONNRESET, NULL, 0, c);
    }
    dyn_iobuf_free(&out);
    if (head_only) {
        close(ffd);
        dyn_app_met_response(c, 0);
        return;
    }
    c->refs++;
    {
        int rc;
        c->file_inflight = 1;
        if (rstart >= 0)
            rc = dyn_aio_sendfile(c->app->aio, c->fd, ffd, (off_t)rstart,
                (size_t)(rend - rstart + 1), dyn_app_out_file_done, c);
        else
            rc = dyn_aio_sendfile(c->app->aio, c->fd, ffd, 0, (size_t)st.st_size,
                dyn_app_out_file_done, c);
        if (rc < 0) {
            c->file_inflight = 0;
            dyn_app_conn_close(c);
            dyn_app_conn_unref(c);
        }
    }
    dyn_app_met_response(c, (double)(rstart >= 0 ? rend - rstart + 1 : st.st_size));
}

JSValue dyn_app_static(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    const char *prefix, *dir;
    dyn_app_route_t* nr;
    char** allow = NULL;
    size_t n_allow = 0;
    int64_t maxf = 0;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "static(prefix, dir[, opts])");
    prefix = JS_ToCString(ctx, argv[0]);
    if (!prefix)
        return JS_EXCEPTION;
    dir = dyn_path_borrow(ctx, argv[1], "static(prefix, root)", NULL);
    if (!dir) {
        JS_FreeCString(ctx, prefix);
        return JS_EXCEPTION;
    }
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (dyn_opts_strict(ctx, argv[2], http_static_keys, 2)) {
            JS_FreeCString(ctx, prefix);
            return JS_EXCEPTION;
        }
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "maxFileSize");
        if (!JS_IsUndefined(v)) {
            if (JS_ToInt64(ctx, &maxf, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, prefix);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        if (dyn_app_parse_allow(ctx, argv[2], &allow, &n_allow) < 0)
            goto fail;
    }

    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        goto fail;
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(prefix);
    nr->type = APP_STATIC;
    nr->handler = JS_UNDEFINED;
    nr->dir = strdup(dir);
    nr->sroot = dyn_static_root_new(dir);
    if (!nr->path || !nr->dir || !nr->sroot) {
        free(nr->path);
        free(nr->dir);
        dyn_static_root_free(nr->sroot);
        memset(nr, 0, sizeof(*nr));
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    nr->max_file = maxf;
    nr->allow = allow;
    nr->n_allow = n_allow;
    app->n_routes++;
    JS_FreeCString(ctx, prefix);
    return JS_UNDEFINED;
fail:
    {
        size_t k;
        for (k = 0; k < n_allow; k++)
            free(allow[k]);
        free(allow);
    }
    JS_FreeCString(ctx, prefix);
    return JS_EXCEPTION;
}
#endif
