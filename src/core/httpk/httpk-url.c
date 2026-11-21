#include "httpk.h"

int httpk_url_split(const char* url, size_t n, httpk_url_t* u)
{
    size_t i, a, ae, hb, he;
    memset(u, 0, sizeof *u);
    if (n >= 7 && httpk_ci_eq(url, 7, "http://")) {
        i = 7;
    } else if (n >= 8 && httpk_ci_eq(url, 8, "https://")) {
        i = 8;
        u->https = 1;
    } else {
        return -1;
    }
    a = i;
    while (i < n && url[i] != '/' && url[i] != '?' && url[i] != '#') {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c == 0x7f || c == '\\')
            return -1;
        i++;
    }
    ae = i;
    hb = a;
    {
        size_t k;
        for (k = a; k < ae; k++)
            if (url[k] == '@')
                hb = k + 1;
    }
    he = ae;
    if (hb < ae && url[hb] == '[') {
        const char* rb = (const char*)memchr(url + hb, ']', ae - hb);
        if (!rb)
            return -1;
        he = (size_t)(rb - url) + 1;
        u->ipv6 = 1;
        if (he < ae && url[he] != ':')
            return -1;
    } else {
        const char* colon = (const char*)memchr(url + hb, ':', ae - hb);
        if (colon)
            he = (size_t)(colon - url);
    }
    if (he == hb)
        return -1;
    u->port = u->https ? 443 : 80;
    u->default_port = 1;
    if (he < ae) {
        size_t k;
        unsigned long p = 0;
        if (url[he] != ':')
            return -1;
        for (k = he + 1; k < ae; k++) {
            if (url[k] < '0' || url[k] > '9')
                return -1;
            p = p * 10 + (unsigned long)(url[k] - '0');
            if (p > 65535)
                return -1;
        }
        if (k > he + 1) {
            if (p == 0)
                return -1;
            u->port = (uint16_t)p;
            u->default_port = u->port == (u->https ? 443 : 80);
        }
    }
    u->host = (uint32_t)hb;
    u->host_len = (uint32_t)(he - hb);
    u->path = (uint32_t)ae;
    while (i < n && url[i] != '?' && url[i] != '#')
        i++;
    u->path_len = (uint32_t)(i - ae);
    if (i < n && url[i] == '?') {
        size_t q = ++i;
        while (i < n && url[i] != '#')
            i++;
        u->query = (uint32_t)q;
        u->query_len = (uint32_t)(i - q);
    }
    return 0;
}

int httpk_same_origin(const char* a, const char* b)
{
    httpk_url_t ua, ub;
    size_t i;
    if (httpk_url_split(a, strlen(a), &ua) < 0 || httpk_url_split(b, strlen(b), &ub) < 0)
        return 0;
    if (ua.https != ub.https || ua.port != ub.port || ua.host_len != ub.host_len)
        return 0;
    for (i = 0; i < ua.host_len; i++)
        if (httpk_lc((unsigned char)a[ua.host + i]) != httpk_lc((unsigned char)b[ub.host + i]))
            return 0;
    return 1;
}

int httpk_is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307
        || status == 308;
}

int httpk_redirect_plan(int status, const char* method, const char* cur_url,
    const char* next_url, int hop, int max_hops, unsigned flags,
    httpk_redirect_t* out)
{
    httpk_url_t nu;
    out->method = method;
    out->drop_body = 0;
    out->strip_credentials = 0;
    if (!httpk_is_redirect(status) || !next_url || !*next_url)
        return HTTPK_REDIR_NONE;
    if (hop >= max_hops)
        return HTTPK_REDIR_E_TOO_MANY;
    if (httpk_url_split(next_url, strlen(next_url), &nu) < 0)
        return HTTPK_REDIR_E_SCHEME;
    if (!(flags & HTTPK_REDIR_ALLOW_SELF) && strcmp(next_url, cur_url) == 0)
        return HTTPK_REDIR_E_LOOP;
    if (!nu.https && cur_url && httpk_ci_eq(cur_url, 8, "https://")
        && !(flags & HTTPK_REDIR_ALLOW_DOWNGRADE))
        return HTTPK_REDIR_E_DOWNGRADE;
    if (method && ((status == 303 && !httpk_ci_eq(method, strlen(method), "HEAD")) || ((status == 301 || status == 302) && httpk_ci_eq(method, strlen(method), "POST")))) {
        out->method = "GET";
        out->drop_body = !httpk_ci_eq(method, strlen(method), "GET");
    }
    if (!(flags & HTTPK_REDIR_KEEP_CREDENTIALS) && !httpk_same_origin(cur_url, next_url))
        out->strip_credentials = 1;
    return HTTPK_REDIR_FOLLOW;
}
