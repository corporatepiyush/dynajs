#include "urlrec.h"

void dyn_url_free(void* p)
{
    dyn_url_t* u = (dyn_url_t*)p;
    free(u->scheme);
    free(u->username);
    free(u->password);
    free(u->host);
    free(u->port);
    free(u->path);
    free(u->query);
    free(u->fragment);
    free(u->drive);
    free(u);
}

static char* dyn_encode_dup(const char* s, size_t n, dyn_parse_set_t set)
{
    dyn_sb_t b;
    char* r;
    size_t i = 0;
    while (i < n && !dyn_url_cls_has((unsigned)set, (unsigned char)s[i]))
        i++;
    if (i == n)
        return dyn_strndup(s, n);
    dyn_sb_init(&b);
    dyn_sb_put(&b, s, i);
    dyn_parse_encode(&b, s + i, n - i, set);
    if (b.oom) {
        dyn_sb_free(&b);
        return NULL;
    }
    r = dyn_strndup(b.p ? b.p : "", b.n);
    dyn_sb_free(&b);
    return r;
}

int dyn_url_set_enc(char** dst, const char* s, size_t n,
    dyn_parse_set_t set)
{
    char* r = dyn_encode_dup(s, n, set);
    if (!r)
        return -1;
    free(*dst);
    *dst = r;
    return 0;
}

int dyn_url_take_enc(char** dst, dyn_sb_t* b, dyn_parse_set_t set, int raw)
{
    size_t i = 0;
    if (b->oom)
        return -1;
    if (!raw)
        while (i < b->n && !dyn_url_cls_has((unsigned)set, (unsigned char)b->p[i]))
            i++;
    if (raw || i == b->n) {
        dyn_sb_putc(b, 0);
        if (b->oom)
            return -1;
        free(*dst);
        *dst = b->p;
        b->p = NULL;
        b->n = 0;
        b->cap = 0;
        return 0;
    }
    return dyn_url_set_enc(dst, b->p, b->n, set);
}

void dyn_url_serialize(dyn_sb_t* b, const dyn_url_t* u)
{
    dyn_sb_puts(b, u->scheme);
    dyn_sb_putc(b, ':');
    if (u->has_authority) {
        dyn_sb_puts(b, "//");
        if ((u->username && *u->username) || (u->password && *u->password)) {
            if (u->username)
                dyn_sb_puts(b, u->username);
            if (u->password && *u->password) {
                dyn_sb_putc(b, ':');
                dyn_sb_puts(b, u->password);
            }
            dyn_sb_putc(b, '@');
        }
        dyn_sb_puts(b, u->host ? u->host : "");
        if (u->port) {
            dyn_sb_putc(b, ':');
            dyn_sb_puts(b, u->port);
        }
    }
    if (u->path && !u->has_authority && u->path[0] == '/' && u->path[1] == '/')
        dyn_sb_puts(b, "/.");
    if (u->path) {
        if (u->has_authority)
            dyn_href_component(b, u->path, DYN_PCT_PATH);
        else
            dyn_sb_puts(b, u->path);
    }
    if (u->query) {
        dyn_sb_putc(b, '?');
        dyn_href_component(b, u->query, DYN_PCT_QUERY);
    }
    if (u->fragment) {
        dyn_sb_putc(b, '#');
        dyn_href_component(b, u->fragment, DYN_PCT_FRAGMENT);
    }
}
