#include "httpk.h"

size_t httpk_head_end(const char* buf, size_t n, size_t* scan_from)
{
    size_t i = scan_from ? *scan_from : 0;
    while (i + 4 <= n) {
        const char* cr = (const char*)memchr(buf + i, '\r', n - i - 3);
        if (!cr)
            break;
        i = (size_t)(cr - buf);
        if (cr[1] == '\n' && cr[2] == '\r' && cr[3] == '\n')
            return i + 4;
        i++;
    }
    if (scan_from)
        *scan_from = n > 3 ? n - 3 : 0;
    return 0;
}

static int httpk_digit(char c) { return c >= '0' && c <= '9'; }

int httpk_status_line(const char* p, size_t n, int* status, int* minor,
    size_t* reason_off, size_t* reason_len)
{
    size_t i = 5, k;
    int st;
    if (n < 10 || memcmp(p, "HTTP/", 5) != 0 || !httpk_digit(p[5]))
        return -1;
    i = 6;
    *minor = -1;
    if (p[i] == '.') {
        if (!httpk_digit(p[i + 1]))
            return -1;
        *minor = p[i + 1] - '0';
        i += 2;
    }
    if (i + 4 > n || p[i] != ' ')
        return -1;
    i++;
    if (!httpk_digit(p[i]) || !httpk_digit(p[i + 1]) || !httpk_digit(p[i + 2]))
        return -1;
    st = (p[i] - '0') * 100 + (p[i + 1] - '0') * 10 + (p[i + 2] - '0');
    i += 3;
    if (i < n && p[i] != ' ')
        return -1;
    if (st > 599)
        return -1;
    if (i < n)
        i++;
    for (k = i; k < n; k++) {
        unsigned char c = (unsigned char)p[k];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return -1;
    }
    *status = st;
    *reason_off = i;
    *reason_len = n - i;
    return 0;
}

int httpk_is_interim(int status) { return status >= 100 && status < 200 && status != 101; }

int httpk_bodyless(int status, const char* method)
{
    if (status == 204 || status == 304 || (status >= 100 && status < 200))
        return 1;
    return method && httpk_ci_eq(method, strlen(method), "HEAD");
}

const char* httpk_strerror(int code)
{
    switch (code) {
    case HTTPK_OK:
        return "ok";
    case HTTPK_E_STATUS:
        return "malformed status line";
    case HTTPK_E_LINE:
        return "malformed header line";
    case HTTPK_E_FOLD:
        return "malformed header line (obsolete line folding)";
    case HTTPK_E_TOO_MANY:
        return "too many header fields";
    case HTTPK_E_CL:
        return "malformed Content-Length";
    case HTTPK_E_CL_EMPTY:
        return "empty Content-Length";
    case HTTPK_E_CL_DUP:
        return "conflicting duplicate Content-Length";
    case HTTPK_E_CL_TE:
        return "Content-Length with Transfer-Encoding: chunked";
    case HTTPK_E_DUP:
        return "conflicting values for a single-value header field";
    default:
        return "malformed message head";
    }
}

static int httpk_parse_cl(const char* v, size_t n, uint64_t* out)
{
    uint64_t x = 0;
    size_t i;
    if (n == 0)
        return -1;
    for (i = 0; i < n; i++) {
        if (!httpk_digit(v[i]) || x > (UINT64_MAX - 9) / 10)
            return -1;
        x = x * 10 + (uint64_t)(v[i] - '0');
    }
    *out = x;
    return 0;
}

static int httpk_head_field(httpk_head_t* h, const char* buf, size_t ls,
    size_t le)
{
    const char* line = buf + ls;
    size_t n = le - ls, c = 0, vb, ve;
    if (line[0] == ' ' || line[0] == '\t')
        return HTTPK_E_FOLD;
    while (c < n && line[c] != ':') {
        if (!httpk_tchar[(unsigned char)line[c]])
            return HTTPK_E_LINE;
        c++;
    }
    if (c == 0 || c == n)
        return HTTPK_E_LINE;
    vb = c + 1;
    ve = n;
    while (vb < ve && (line[vb] == ' ' || line[vb] == '\t'))
        vb++;
    while (ve > vb && (line[ve - 1] == ' ' || line[ve - 1] == '\t'))
        ve--;
    if (!httpk_value_ok(line + vb, ve - vb))
        return HTTPK_E_LINE;
    if (h->n_hdrs >= h->cap_hdrs)
        return HTTPK_E_TOO_MANY;
    {
        httpk_hdr_t* f = &h->hdrs[h->n_hdrs++];
        f->name = (uint32_t)ls;
        f->name_len = (uint32_t)c;
        f->value = (uint32_t)(ls + vb);
        f->value_len = (uint32_t)(ve - vb);
    }
    if (httpk_ci_eq(line, c, "content-length")) {
        uint64_t cl;
        if (ve == vb)
            return HTTPK_E_CL_EMPTY;
        if (httpk_parse_cl(line + vb, ve - vb, &cl) < 0)
            return HTTPK_E_CL;
        if (h->has_cl && cl != h->content_length)
            return HTTPK_E_CL_DUP;
        h->content_length = cl;
        h->has_cl = 1;
    } else if (httpk_ci_eq(line, c, "transfer-encoding")) {
        if (ve > vb) {
            if (httpk_list_last_is(line + vb, ve - vb, "chunked")) {
                h->chunked = 1;
                h->te_unframed = 0;
            } else {
                h->chunked = 0;
                h->te_unframed = 1;
            }
        }
    } else if (httpk_ci_eq(line, c, "connection")) {
        if (httpk_list_has(line + vb, ve - vb, "close"))
            h->conn_close = 1;
        if (httpk_list_has(line + vb, ve - vb, "keep-alive"))
            h->conn_keepalive = 1;
    }
    return HTTPK_OK;
}

int httpk_resp_head(const char* buf, size_t head_len, httpk_head_t* h)
{
    size_t ls, le;
    const char* eol;
    size_t ro = 0, rl = 0;
    int rc;
    h->n_hdrs = 0;
    h->dup_field = 0;
    h->content_length = 0;
    h->has_cl = h->chunked = h->te_unframed = 0;
    h->conn_close = h->conn_keepalive = 0;
    h->head_len = head_len;
    if (head_len < 4)
        return HTTPK_E_STATUS;
    eol = (const char*)memchr(buf, '\r', head_len);
    if (!eol || (size_t)(eol - buf) + 1 >= head_len || eol[1] != '\n')
        return HTTPK_E_STATUS;
    if (memchr(buf, '\n', (size_t)(eol - buf)))
        return HTTPK_E_STATUS;
    if (httpk_status_line(buf, (size_t)(eol - buf), &h->status, &h->minor, &ro,
            &rl)
        < 0)
        return HTTPK_E_STATUS;
    h->reason = (uint32_t)ro;
    h->reason_len = (uint32_t)rl;
    ls = (size_t)(eol - buf) + 2;
    while (ls + 2 <= head_len) {
        const char* cr = (const char*)memchr(buf + ls, '\r', head_len - ls);
        if (!cr || (size_t)(cr - buf) + 1 >= head_len || cr[1] != '\n')
            return HTTPK_E_LINE;
        le = (size_t)(cr - buf);
        if (memchr(buf + ls, '\n', le - ls))
            return HTTPK_E_LINE;
        if (le == ls)
            break;
        rc = httpk_head_field(h, buf, ls, le);
        if (rc != HTTPK_OK)
            return rc;
        ls = le + 2;
    }
    if (h->chunked && h->has_cl)
        return HTTPK_E_CL_TE;
    {
        uint32_t first[HTTPK_SINGLETONS], i;
        for (i = 0; i < HTTPK_SINGLETONS; i++)
            first[i] = UINT32_MAX;
        for (i = 0; i < h->n_hdrs; i++) {
            const httpk_hdr_t* f = &h->hdrs[i];
            int k = httpk_singleton_slot(buf + f->name, f->name_len);
            const httpk_hdr_t* g;
            if (k < 0)
                continue;
            if (first[k] == UINT32_MAX) {
                first[k] = i;
                continue;
            }
            g = &h->hdrs[first[k]];
            if (g->value_len != f->value_len
                || memcmp(buf + g->value, buf + f->value, f->value_len) != 0) {
                h->dup_field = i;
                return HTTPK_E_DUP;
            }
        }
    }
    if (h->minor == 0 && !h->conn_keepalive)
        h->conn_close = 1;
    return HTTPK_OK;
}

const char* httpk_hdr_first(const httpk_head_t* h, const char* buf,
    const char* name, size_t* vlen)
{
    uint32_t it = 0;
    const char* v = NULL;
    if (httpk_hdr_next(h, buf, name, &it, &v, vlen))
        return v;
    return NULL;
}

int httpk_hdr_next(const httpk_head_t* h, const char* buf, const char* name,
    uint32_t* it, const char** v, size_t* vlen)
{
    uint32_t i;
    for (i = *it; i < h->n_hdrs; i++) {
        const httpk_hdr_t* f = &h->hdrs[i];
        if (httpk_ci_eq(buf + f->name, f->name_len, name)) {
            *v = buf + f->value;
            *vlen = f->value_len;
            *it = i + 1;
            return 1;
        }
    }
    *it = h->n_hdrs;
    return 0;
}

int httpk_hdr_value(const httpk_head_t* h, const char* buf, const char* name,
    dyn_sb_t* out)
{
    uint32_t it = 0;
    const char *v, *first = NULL;
    size_t vl, fl = 0;
    int count = 0, kind = httpk_field_kind(name, strlen(name));
    while (httpk_hdr_next(h, buf, name, &it, &v, &vl)) {
        if (count == 0) {
            first = v;
            fl = vl;
            dyn_sb_put(out, v, vl);
        } else if (kind == HTTPK_FIELD_SINGLETON) {
            if (vl != fl || memcmp(v, first, vl) != 0)
                return HTTPK_HV_CONFLICT;
        } else {
            dyn_sb_put(out, kind == HTTPK_FIELD_SET_COOKIE ? "\n" : ", ", kind == HTTPK_FIELD_SET_COOKIE ? 1 : 2);
            dyn_sb_put(out, v, vl);
        }
        count++;
    }
    if (out->oom)
        return HTTPK_HV_OOM;
    if (count == 0)
        return HTTPK_HV_NONE;
    return count == 1 || kind == HTTPK_FIELD_SINGLETON ? HTTPK_HV_ONE
                                                       : HTTPK_HV_COMBINED;
}
