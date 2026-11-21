#ifndef DYN_URLREC_URLREC_H
#define DYN_URLREC_URLREC_H

#include "../dyn-pct.h"
#include "../dyn-sb.h"
#include "../dyn-cdefs.h"
#include "../idna/idna-tables.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void dyn_sb_sink(void* ud, const char* b, size_t n)
{
    dyn_sb_put((dyn_sb_t*)ud, b, n);
}

static const uint8_t dyn_url_cls[256] = {
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7f,
    0x7e,
    0x00,
    0x3e,
    0x5e,
    0x00,
    0x40,
    0x00,
    0x10,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x44,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x44,
    0x04,
    0x7e,
    0x04,
    0x7e,
    0x46,
    0x44,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x44,
    0x44,
    0x44,
    0x46,
    0x00,
    0x26,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x06,
    0x44,
    0x06,
    0x00,
    0x7f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
    0x3f,
};

static inline int dyn_url_cls_has(unsigned set, unsigned char c)
{
    return (dyn_url_cls[c] >> set) & 1;
}

static inline void dyn_pct_encode(dyn_sb_t* out, const char* s, size_t n, int form)
{
    dyn_pct_encode_core(out, dyn_sb_sink, s, n, form);
}

typedef enum {
    DYN_SET_OPAQUE,
    DYN_SET_PATH,
    DYN_SET_USERINFO,
    DYN_SET_QUERY,
    DYN_SET_QUERY_SPECIAL,
    DYN_SET_FRAGMENT
} dyn_parse_set_t;

void dyn_parse_encode(dyn_sb_t* b, const char* s, size_t n,
    dyn_parse_set_t set);

typedef struct {
    char *scheme, *username, *password, *host, *port, *path, *query, *fragment;
    char* drive;
    int has_authority;
    int refs;
    uint32_t query_version;
} dyn_url_t;

void dyn_url_free(void* p);

static inline char* dyn_strndup(const char* s, size_t n)
{
    char* r = (char*)malloc(n + 1);
    if (!r)
        return NULL;
    if (n)
        memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static inline int dyn_url_set_str(char** dst, const char* s, size_t n)
{
    char* r = dyn_strndup(s, n);
    if (!r)
        return -1;
    free(*dst);
    *dst = r;
    return 0;
}

int dyn_url_set_enc(char** dst, const char* s, size_t n,
    dyn_parse_set_t set);

int dyn_url_take_enc(char** dst, dyn_sb_t* b, dyn_parse_set_t set, int raw);

int dyn_url_normalize_host(dyn_url_t* u);

const char* dyn_url_default_port(const char* scheme);

static inline int dyn_scheme_is_special(const char* scheme)
{
    return !strcmp(scheme, "http") || !strcmp(scheme, "https") || !strcmp(scheme, "ws") || !strcmp(scheme, "wss") || !strcmp(scheme, "ftp") || !strcmp(scheme, "file");
}

static inline void dyn_lower(char* s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

int dyn_url_parse(dyn_url_t* u, const char* s, size_t n,
    const dyn_url_t* base);

void dyn_url_serialize(dyn_sb_t* b, const dyn_url_t* u);

typedef struct {
    char *k, *v;
    size_t kn, vn;
} dyn_sp_pair_t;

typedef struct {
    dyn_sp_pair_t* a;
    size_t n, cap;
} dyn_sp_list_t;

typedef struct {
    char* query;
    dyn_url_t* host;
    dyn_sp_list_t list;
    int list_valid;
    int list_canonical;
    uint32_t cached_version;
    uint32_t query_version;
} dyn_sp_t;

typedef struct {
    dyn_sp_pair_t p;
    size_t idx;
} dyn_sp_sort_ent_t;

typedef enum { DYN_PCT_PATH,
    DYN_PCT_QUERY,
    DYN_PCT_FRAGMENT } dyn_pct_set_t;

enum { URLF_PATH,
    URLF_QUERY,
    URLF_FRAGMENT,
    URLF_HOST,
    URLF_PORT };

static inline void dyn_pct_decode(dyn_sb_t* out, const char* s, size_t n, int plus_space)
{
    dyn_pct_decode_core(out, dyn_sb_sink, s, n, plus_space);
}

static inline void dyn_sp_list_init(dyn_sp_list_t* l)
{
    memset(l, 0, sizeof(*l));
}

static inline const char* dyn_sp_query_str(const dyn_sp_t* sp)
{
    const char* q = sp->host ? sp->host->query : sp->query;
    return q ? q : "";
}

static inline uint32_t dyn_sp_version(const dyn_sp_t* sp)
{
    return sp->host ? sp->host->query_version : sp->query_version;
}

static inline void dyn_sp_cache_drop(dyn_sp_t* sp)
{
    sp->list_valid = 0;
}

static inline int dyn_url_has_byte(const char* s, size_t n, char c)
{
    return memchr(s, c, n) != NULL;
}

static inline char* dyn_strip_tabnl(const char* s, size_t n, size_t* outn)
{
    char* r;
    size_t i, w = 0;
    int dirty = 0;
    for (i = 0; i < n; i++)
        if (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            dirty = 1;
            break;
        }
    if (!dirty)
        return NULL;
    r = (char*)malloc(n);
    if (!r)
        return NULL;
    for (i = 0; i < n; i++)
        if (s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
            r[w++] = s[i];
    *outn = w;
    return r;
}

static inline int dyn_url_put(char** slot, const char* v)
{
    char* n = v ? dyn_strndup(v, strlen(v)) : NULL;
    if (v && !n)
        return -1;
    free(*slot);
    *slot = n;
    return 0;
}

static inline int dyn_pct_forbidden(dyn_pct_set_t set, unsigned char c)
{
    return dyn_url_cls_has(set == DYN_PCT_PATH ? 1u : set == DYN_PCT_QUERY ? 3u
                                                                           : 5u,
        c);
}

static inline void dyn_href_component(dyn_sb_t* b, const char* s, dyn_pct_set_t set)
{
    static const char HEX[] = "0123456789ABCDEF";
    unsigned bit = set == DYN_PCT_PATH ? 1u : set == DYN_PCT_QUERY ? 3u
                                                                   : 5u;
    size_t i;
    if (!s)
        return;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        int h1, h2;
        if (c != '%' && !dyn_url_cls_has(bit, c)) {
            size_t j = i + 1;
            while (s[j] && s[j] != '%' && !dyn_url_cls_has(bit, (unsigned char)s[j]))
                j++;
            dyn_sb_put(b, s + i, j - i);
            i = j - 1;
            continue;
        }
        if (c == '%' && (h1 = dyn_hexval((unsigned char)s[i + 1])) >= 0
            && (h2 = dyn_hexval((unsigned char)s[i + 2])) >= 0) {
            dyn_sb_putc(b, '%');
            dyn_sb_putc(b, s[i + 1]);
            dyn_sb_putc(b, s[i + 2]);
            i += 2;
            continue;
        }
        if (!dyn_url_cls_has(bit, c)) {
            dyn_sb_putc(b, (char)c);
        } else {
            dyn_sb_putc(b, '%');
            dyn_sb_putc(b, HEX[c >> 4]);
            dyn_sb_putc(b, HEX[c & 0xF]);
        }
    }
}

static inline char* dyn_url_candidate(const dyn_url_t* u, int field, const char* raw,
    size_t* outn)
{
    dyn_sb_t b;
    dyn_url_t tmp = *u;
    char* r;
    const char* v = (raw && *raw) ? raw : NULL;

    switch (field) {
    case URLF_PATH:
    case URLF_QUERY:
        tmp.path = field == URLF_PATH ? NULL : tmp.path;
        tmp.query = NULL;
        tmp.fragment = NULL;
        dyn_sb_init(&b);
        dyn_url_serialize(&b, &tmp);
        if (field == URLF_QUERY && v)
            dyn_sb_putc(&b, '?');
        if (v)
            dyn_sb_puts(&b, v);
        break;
    case URLF_FRAGMENT:
        tmp.fragment = DYN_UNCONST(v);
        break;
    case URLF_HOST:
        return NULL;
    case URLF_PORT:
        tmp.port = DYN_UNCONST(v);
        break;
    default:
        return NULL;
    }
    if (field == URLF_FRAGMENT || field == URLF_PORT) {
        dyn_sb_init(&b);
        dyn_url_serialize(&b, &tmp);
    }
    if (b.oom) {
        dyn_sb_free(&b);
        return NULL;
    }
    r = dyn_strndup(b.p ? b.p : "", b.n);
    *outn = b.n;
    dyn_sb_free(&b);
    return r;
}

static inline int dyn_url_adopt(dyn_url_t* u, const char* text, size_t n, int field,
    int move_query, int move_frag)
{
    dyn_url_t fresh;
    int rc = 1;

    memset(&fresh, 0, sizeof fresh);
    if (dyn_url_parse(&fresh, text, n, NULL) != 0) {
        free(fresh.scheme);
        free(fresh.username);
        free(fresh.password);
        free(fresh.host);
        free(fresh.port);
        free(fresh.path);
        free(fresh.query);
        free(fresh.fragment);
        free(fresh.drive);
        return 0;
    }
    if (dyn_url_put(&u->path, fresh.path) < 0
        || ((field == URLF_PATH || field == URLF_QUERY)
            && move_query && dyn_url_put(&u->query, fresh.query) < 0)
        || ((field == URLF_PATH || field == URLF_QUERY || field == URLF_FRAGMENT)
            && move_frag && dyn_url_put(&u->fragment, fresh.fragment) < 0)
        || ((field == URLF_HOST || field == URLF_PORT)
            && (dyn_url_put(&u->host, fresh.host) < 0
                || dyn_url_put(&u->port, fresh.port) < 0)))
        rc = -1;
    if (rc == 1)
        u->query_version++;
    free(fresh.scheme);
    free(fresh.username);
    free(fresh.password);
    free(fresh.host);
    free(fresh.port);
    free(fresh.path);
    free(fresh.query);
    free(fresh.fragment);
    free(fresh.drive);
    return rc;
}

void dyn_sp_free(void* p);
void dyn_sp_list_free(dyn_sp_list_t* l);
int dyn_sp_list_put(dyn_sp_list_t* l, const char* k, size_t kn,
    const char* v, size_t vn);
int dyn_sp_set_query(dyn_sp_t* sp, const char* q, size_t n);
dyn_sp_list_t* dyn_sp_list_ref(dyn_sp_t* sp);
void dyn_sp_parse(const char* q, size_t n, dyn_sp_list_t* l);
int dyn_sp_serialize(const dyn_sp_list_t* l, dyn_sb_t* out);
int dyn_sp_apply(dyn_sp_t* sp, const dyn_sp_list_t* l);
int dyn_sp_cmp(const void* a, const void* b);
int dyn_sp_key_is(const dyn_sp_pair_t* p, const char* k, size_t kn);
int dyn_sp_val_is(const dyn_sp_pair_t* p, const char* v, size_t vn);

#endif
