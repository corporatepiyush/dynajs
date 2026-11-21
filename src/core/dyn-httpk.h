#ifndef DYN_HTTPK_H
#define DYN_HTTPK_H

#include <stddef.h>
#include <stdint.h>
#include "dyn-sb.h"

extern const uint8_t httpk_tchar[256];

int httpk_name_ok(const char* s, size_t n);
int httpk_value_ok(const char* s, size_t n);
int httpk_value_gen_ok(const char* s, size_t n);
int httpk_ci_eq(const char* a, size_t an, const char* lit);
int httpk_list_has(const char* v, size_t n, const char* tok);
int httpk_list_last_is(const char* v, size_t n, const char* tok);
int httpk_qvalue(const char* v, size_t n, const char* tok);
int httpk_weight(const char* el, size_t n);
int httpk_is_framing_name(const char* s, size_t n);

enum {
    HTTPK_FIELD_LIST = 0,
    HTTPK_FIELD_SINGLETON = 1,
    HTTPK_FIELD_SET_COOKIE = 2
};
int httpk_field_kind(const char* name, size_t n);

typedef struct {
    uint32_t name, name_len, value, value_len;
} httpk_hdr_t;

enum {
    HTTPK_OK = 0,
    HTTPK_E_STATUS,
    HTTPK_E_LINE,
    HTTPK_E_FOLD,
    HTTPK_E_TOO_MANY,
    HTTPK_E_CL,
    HTTPK_E_CL_EMPTY,
    HTTPK_E_CL_DUP,
    HTTPK_E_CL_TE,
    HTTPK_E_DUP
};

typedef struct {
    int status;
    int minor;
    uint32_t reason, reason_len;
    size_t head_len;
    httpk_hdr_t* hdrs;
    uint32_t n_hdrs, cap_hdrs;
    uint64_t content_length;
    uint8_t has_cl, chunked, te_unframed, conn_close, conn_keepalive;
    uint32_t dup_field;
} httpk_head_t;

size_t httpk_head_end(const char* buf, size_t n, size_t* scan_from);
int httpk_status_line(const char* p, size_t n, int* status, int* minor,
    size_t* reason_off, size_t* reason_len);
int httpk_is_interim(int status);
int httpk_resp_head(const char* buf, size_t head_len, httpk_head_t* h);
const char* httpk_strerror(int code);
int httpk_bodyless(int status, const char* method);

enum {
    HTTPK_HV_NONE = 0,
    HTTPK_HV_ONE = 1,
    HTTPK_HV_COMBINED = 2,
    HTTPK_HV_CONFLICT = -1,
    HTTPK_HV_OOM = -2
};
int httpk_hdr_value(const httpk_head_t* h, const char* buf, const char* name,
    dyn_sb_t* out);
const char* httpk_hdr_first(const httpk_head_t* h, const char* buf,
    const char* name, size_t* vlen);
int httpk_hdr_next(const httpk_head_t* h, const char* buf, const char* name,
    uint32_t* it, const char** v, size_t* vlen);

enum {
    HTTPK_DC_MORE = 0,
    HTTPK_DC_DONE = 1,
    HTTPK_DC_BAD = -1
};

typedef struct {
    uint64_t left;
    uint64_t total;
    uint64_t trailer;
    uint32_t line;
    uint8_t state, digits;
    const char* why;
} httpk_dechunk_t;

void httpk_dechunk_init(httpk_dechunk_t* d);
int httpk_dechunk(httpk_dechunk_t* d, uint8_t* buf, size_t n,
    size_t* consumed, size_t* out_n);
int httpk_chunk_size(const char* line, size_t n, uint64_t* size);

int64_t httpk_http_date(const char* s, size_t n);
int64_t httpk_cookie_date(const char* s, size_t n);
double httpk_delta_seconds(const char* s, size_t n);

#define HTTPK_IMF_FIXDATE_LEN 29
#define HTTPK_DATE_LINE_LEN 37
size_t httpk_imf_fixdate(int64_t t, char* out);
const char* httpk_date_line(int64_t now);
uint64_t httpk_date_line_builds(void);
int httpk_block_has(const char* blk, size_t n, const char* name);

enum {
    HTTPK_RETRY_SCRAPE = 1,
    HTTPK_RETRY_API = 2
};
int httpk_retry_status(int status, int policy);
double httpk_backoff_ms(double base_ms, double cap_ms, int attempt,
    uint64_t* rng);
double httpk_retry_after_ms(const char* retry_after, size_t ra_n,
    const char* retry_after_ms, size_t ram_n, int64_t now_s);

typedef struct {
    uint32_t host, host_len, path, path_len, query, query_len;
    uint16_t port;
    uint8_t https, default_port, ipv6;
} httpk_url_t;

int httpk_url_split(const char* url, size_t n, httpk_url_t* u);
int httpk_same_origin(const char* a, const char* b);

enum {
    HTTPK_REDIR_NONE = 0,
    HTTPK_REDIR_FOLLOW = 1,
    HTTPK_REDIR_E_TOO_MANY = -1,
    HTTPK_REDIR_E_DOWNGRADE = -2,
    HTTPK_REDIR_E_SCHEME = -3,
    HTTPK_REDIR_E_LOOP = -4
};
#define HTTPK_REDIR_ALLOW_DOWNGRADE 1u
#define HTTPK_REDIR_KEEP_CREDENTIALS 2u
#define HTTPK_REDIR_ALLOW_SELF 4u

typedef struct {
    const char* method;
    uint8_t drop_body, strip_credentials;
} httpk_redirect_t;

int httpk_is_redirect(int status);
int httpk_redirect_plan(int status, const char* method, const char* cur_url,
    const char* next_url, int hop, int max_hops, unsigned flags,
    httpk_redirect_t* out);
int httpk_is_credential_name(const char* s, size_t n);

typedef struct {
    const char* type;
    size_t type_len;
    const char* data;
    size_t data_len;
    const char* id;
    size_t id_len;
    int64_t retry_ms;
} httpk_sse_event_t;

typedef int (*httpk_sse_cb)(void* ud, const httpk_sse_event_t* ev);

typedef struct {
    dyn_sb_t line, data, type, id;
    httpk_sse_cb cb;
    void* ud;
    size_t max_event;
    int64_t retry_ms;
    uint8_t bom_checked, pending_cr, has_data, aborted;
} httpk_sse_t;

enum {
    HTTPK_SSE_OK = 0,
    HTTPK_SSE_E_TOO_BIG = -1,
    HTTPK_SSE_E_OOM = -2,
    HTTPK_SSE_E_ABORT = -3
};

void httpk_sse_init(httpk_sse_t* p, httpk_sse_cb cb, void* ud, size_t max_event);
int httpk_sse_feed(httpk_sse_t* p, const char* buf, size_t n);
void httpk_sse_free(httpk_sse_t* p);
const char* httpk_sse_last_id(const httpk_sse_t* p, size_t* n);
int httpk_sse_frame(dyn_sb_t* out, const char* event, size_t en,
    const char* id, size_t idn, const char* data, size_t dn, int64_t retry_ms);

typedef struct {
    char *name, *value, *domain, *path;
    int64_t expires;
    int64_t created;
    uint8_t host_only, secure, http_only, same_site, persistent;
} httpk_cookie_t;

typedef struct {
    httpk_cookie_t* v;
    size_t n, cap;
    size_t max_cookies;
    int64_t seq;
} httpk_jar_t;

enum {
    HTTPK_CK_STORED = 1,
    HTTPK_CK_DELETED = 2,
    HTTPK_CK_IGNORED = 0,
    HTTPK_CK_OOM = -1
};

void httpk_jar_init(httpk_jar_t* j, size_t max_cookies);
void httpk_jar_free(httpk_jar_t* j);
int httpk_jar_set(httpk_jar_t* j, const char* request_url, const char* sc,
    size_t n, int64_t now_s);
int httpk_jar_header(httpk_jar_t* j, const char* request_url, int64_t now_s,
    dyn_sb_t* out);
void httpk_jar_purge(httpk_jar_t* j, int64_t now_s);

int httpk_req_line(dyn_sb_t* out, const char* method, const char* target);
int httpk_req_header(dyn_sb_t* out, const char* name, size_t nn,
    const char* value, size_t vn);

#endif
