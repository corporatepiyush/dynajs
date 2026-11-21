#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)

static int hc_call_request(const hc_call_req_t* rq, const char* url,
    const httpk_url_t* u, dyn_sb_t* out)
{
    char target[8192];
    size_t tl = u->path_len ? u->path_len : 0, ql = u->query_len;
    char num[32];
    if (tl + ql + 3 > sizeof target)
        return -1;
    if (tl)
        memcpy(target, url + u->path, tl);
    else
        target[tl++] = '/';
    if (u->query && ql) {
        target[tl++] = '?';
        memcpy(target + tl, url + u->query, ql);
        tl += ql;
    }
    target[tl] = 0;
    if (httpk_req_line(out, rq->method, target) < 0)
        return -1;
    dyn_sb_put(out, "Host: ", 6);
    dyn_sb_put(out, url + u->host, u->host_len);
    if (!u->default_port) {
        snprintf(num, sizeof num, ":%u", (unsigned)u->port);
        dyn_sb_puts(out, num);
    }
    dyn_sb_put(out, "\r\n", 2);
    if (rq->headers)
        dyn_sb_puts(out, rq->headers);
    if (rq->body) {
        snprintf(num, sizeof num, "%zu", rq->body_len);
        dyn_sb_put(out, "Content-Length: ", 16);
        dyn_sb_puts(out, num);
        dyn_sb_put(out, "\r\n", 2);
    }
    dyn_sb_put(out, "Connection: close\r\n\r\n", 21);
    return out->oom ? -1 : 0;
}

static void hc_call_drop(const hc_call_req_t* rq, hc_conn_t* c)
{
    if (rq->on_fd)
        rq->on_fd(rq->fd_ud, -1);
    hc_conn_close(c);
}

int hc_call_open(const hc_call_req_t* rq, hc_head_t* hd, hc_stream_t** out,
    char* why, size_t whyn)
{
    httpk_url_t u;
    hc_conn_t conn;
    char host[256];
    dyn_sb_t req;
    int reason = 0, rc;
    size_t ul = strlen(rq->url);

    *out = NULL;
    memset(hd, 0, sizeof *hd);
    conn.fd = -1;
#ifdef CONFIG_TLS
    conn.tls = NULL;
#endif
    if (httpk_url_split(rq->url, ul, &u) < 0) {
        snprintf(why, whyn, "invalid URL");
        return HC_CALL_E_URL;
    }
    if (u.https) {
#ifdef CONFIG_TLS
        if (!rq->tls) {
            snprintf(why, whyn, "no TLS context for an https URL");
            return HC_CALL_E_TLS;
        }
#else
        snprintf(why, whyn, "this build has no TLS support: https needs the optional \"tls\" module");
        return HC_CALL_E_TLS;
#endif
    }
    {
        size_t hb = u.host, hl = u.host_len;
        if (u.ipv6 && hl >= 2) {
            hb++;
            hl -= 2;
        }
        if (hl >= sizeof host) {
            snprintf(why, whyn, "host name too long");
            return HC_CALL_E_URL;
        }
        memcpy(host, rq->url + hb, hl);
        host[hl] = 0;
    }
    if (hc_conn_open(&conn, host, u.port, rq->timeout_ms, NULL, NULL, &reason,
            why, whyn)
        < 0)
        return reason == DYN_NET_CONNECT_RESOLVE ? HC_CALL_E_RESOLVE
                                                 : HC_CALL_E_CONNECT;
    if (rq->on_fd && rq->on_fd(rq->fd_ud, conn.fd) != 0) {
        hc_conn_close(&conn);
        snprintf(why, whyn, "aborted");
        return HC_CALL_E_CONNECT;
    }
    dyn_net_set_nodelay(conn.fd);
#ifdef CONFIG_TLS
    if (u.https && hc_conn_tls_open(&conn, rq->tls, host, why, whyn) != 0) {
        hc_call_drop(rq, &conn);
        return HC_CALL_E_TLS;
    }
#endif
    dyn_sb_init(&req);
    if (hc_call_request(rq, rq->url, &u, &req) < 0) {
        dyn_sb_free(&req);
        hc_call_drop(rq, &conn);
        snprintf(why, whyn, "invalid request line or header");
        return HC_CALL_E_REQUEST;
    }
    rc = hc_send(&conn, req.p, req.n) < 0
        || (rq->body_len && hc_send(&conn, rq->body, rq->body_len) < 0);
    dyn_sb_free(&req);
    if (rc) {
        hc_call_drop(rq, &conn);
        snprintf(why, whyn, "sending the request failed");
        return HC_CALL_E_SEND;
    }
    rc = hc_read_head(&conn, hd, why, whyn);
    if (rc < 0) {
        hc_call_drop(rq, &conn);
        hc_head_free(hd);
        return rc == -2 ? HC_CALL_E_PARSE : HC_CALL_E_RECV;
    }
    *out = (hc_stream_t*)calloc(1, sizeof **out);
    if (!*out) {
        hc_call_drop(rq, &conn);
        hc_head_free(hd);
        snprintf(why, whyn, "out of memory");
        return HC_CALL_E_OOM;
    }
    hc_stream_init(*out, &conn, hd, rq->max_body ? rq->max_body : UINT64_MAX);
    return 0;
}

int hc_call_read_all(hc_stream_t* s, dyn_sb_t* out, char* why, size_t whyn)
{
    uint8_t buf[16384];
    for (;;) {
        long long r = hc_stream_body(s, buf, sizeof buf, why, whyn);
        if (r < 0)
            return -1;
        if (r == 0)
            return out->oom ? -1 : 0;
        dyn_sb_put(out, (const char*)buf, (size_t)r);
    }
}

const char* hc_call_strerror(int code)
{
    switch (code) {
    case HC_CALL_E_URL:
        return "invalid url";
    case HC_CALL_E_RESOLVE:
        return "dns resolution failed";
    case HC_CALL_E_CONNECT:
        return "connect failed";
    case HC_CALL_E_TLS:
        return "tls failed";
    case HC_CALL_E_REQUEST:
        return "invalid request";
    case HC_CALL_E_SEND:
        return "send failed";
    case HC_CALL_E_RECV:
        return "receive failed";
    case HC_CALL_E_PARSE:
        return "malformed response";
    case HC_CALL_E_OOM:
        return "out of memory";
    default:
        return "ok";
    }
}
#endif
