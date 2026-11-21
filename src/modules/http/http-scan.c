#include <string.h>
#include "http-scan.h"
#include "core/dyn-httpk.h"
#include "dyna-io.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)

int dyn_ci_eq(const char* a, const char* b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
    }
    return 1;
}

static const uint8_t scan_name_lc[256] = {
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
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x21,
    0x00,
    0x23,
    0x24,
    0x25,
    0x26,
    0x27,
    0x00,
    0x00,
    0x2a,
    0x2b,
    0x00,
    0x2d,
    0x2e,
    0x00,
    0x30,
    0x31,
    0x32,
    0x33,
    0x34,
    0x35,
    0x36,
    0x37,
    0x38,
    0x39,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x61,
    0x62,
    0x63,
    0x64,
    0x65,
    0x66,
    0x67,
    0x68,
    0x69,
    0x6a,
    0x6b,
    0x6c,
    0x6d,
    0x6e,
    0x6f,
    0x70,
    0x71,
    0x72,
    0x73,
    0x74,
    0x75,
    0x76,
    0x77,
    0x78,
    0x79,
    0x7a,
    0x00,
    0x00,
    0x00,
    0x5e,
    0x5f,
    0x60,
    0x61,
    0x62,
    0x63,
    0x64,
    0x65,
    0x66,
    0x67,
    0x68,
    0x69,
    0x6a,
    0x6b,
    0x6c,
    0x6d,
    0x6e,
    0x6f,
    0x70,
    0x71,
    0x72,
    0x73,
    0x74,
    0x75,
    0x76,
    0x77,
    0x78,
    0x79,
    0x7a,
    0x00,
    0x7c,
    0x00,
    0x7e,
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
};

#define SCAN_SEEN_MAX (DYN_APP_MAX_HEADER_COUNT + 1)
#define SCAN_ONES 0x0101010101010101ull
#define SCAN_HIGHS 0x8080808080808080ull

typedef struct {
    const char* p;
    size_t len;
    uint32_t h;
} scan_seen_t;

static inline uint64_t scan_ctl_any(uint64_t x)
{
    uint64_t del = x ^ (SCAN_ONES * 0x7f);
    return (((x - SCAN_ONES * 0x20) & ~x) | ((del - SCAN_ONES) & ~del)) & SCAN_HIGHS;
}

static const char* scan_crlf_from(const char* p, const char* end)
{
    while (end - p > 1) {
        p = (const char*)memchr(p, '\r', (size_t)(end - p - 1));
        if (!p)
            return NULL;
        if (p[1] == '\n')
            return p;
        p++;
    }
    return NULL;
}

static inline int scan_lit(const char* p, const char* lit, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (scan_name_lc[(unsigned char)p[i]] != (uint8_t)lit[i])
            return 0;
    return 1;
}

enum {
    SCAN_NAME_OTHER,
    SCAN_NAME_DUP_OK,
    SCAN_NAME_HOST,
    SCAN_NAME_CONN,
    SCAN_NAME_CT,
    SCAN_NAME_CL,
    SCAN_NAME_AE,
    SCAN_NAME_TE,
};

static int scan_name_kind(const char* n, size_t len)
{
    switch (len) {
    case 4:
        if (scan_lit(n, "host", 4))
            return SCAN_NAME_HOST;
        if (scan_lit(n, "vary", 4))
            return SCAN_NAME_DUP_OK;
        return SCAN_NAME_OTHER;
    case 6:
        if (scan_lit(n, "cookie", 6) || scan_lit(n, "accept", 6))
            return SCAN_NAME_DUP_OK;
        return SCAN_NAME_OTHER;
    case 10:
        if (scan_lit(n, "connection", 10))
            return SCAN_NAME_CONN;
        if (scan_lit(n, "set-cookie", 10))
            return SCAN_NAME_DUP_OK;
        return SCAN_NAME_OTHER;
    case 12:
        return scan_lit(n, "content-type", 12) ? SCAN_NAME_CT : SCAN_NAME_OTHER;
    case 13:
        return scan_lit(n, "cache-control", 13) ? SCAN_NAME_DUP_OK : SCAN_NAME_OTHER;
    case 14:
        return scan_lit(n, "content-length", 14) ? SCAN_NAME_CL : SCAN_NAME_OTHER;
    case 15:
        if (scan_lit(n, "accept-encoding", 15))
            return SCAN_NAME_AE;
        if (scan_lit(n, "accept-language", 15))
            return SCAN_NAME_DUP_OK;
        return SCAN_NAME_OTHER;
    case 17:
        return scan_lit(n, "transfer-encoding", 17) ? SCAN_NAME_TE : SCAN_NAME_OTHER;
    default:
        return SCAN_NAME_OTHER;
    }
}

static const char* scan_request_line(const char* line, const char* end, dyn_reqinfo_t* ri)
{
    const char* le = scan_crlf_from(line, end);
    const char *sp1, *sp2;
    if (!le)
        return NULL;
    sp1 = (const char*)memchr(line, ' ', (size_t)(le - line));
    sp2 = sp1 ? (const char*)memchr(sp1 + 1, ' ', (size_t)(le - sp1 - 1)) : NULL;
    ri->method = line;
    if (!sp1 || !sp2 || sp1 == line || sp2 == sp1 + 1) {
        ri->target = le;
        ri->bad_target = 1;
    } else {
        const char* t = sp1 + 1;
        const char* mc;
        int in_query = 0;
        ri->method_len = (size_t)(sp1 - line);
        ri->target = t;
        for (; t < sp2; t++) {
            unsigned char ch = (unsigned char)*t;
            if (ch < 0x21 || ch == 0x7f) {
                ri->bad_target = 1;
                break;
            }
            if (ch == '?')
                in_query = 1;
            else if (!in_query)
                ri->target_len++;
        }
        if (le - (sp2 + 1) != 8
            || (memcmp(sp2 + 1, "HTTP/1.1", 8) != 0 && memcmp(sp2 + 1, "HTTP/1.0", 8) != 0))
            ri->bad_target = 1;
        for (mc = line; mc < sp1; mc++)
            if (!httpk_tchar[(unsigned char)*mc]) {
                ri->bad_target = 1;
                break;
            }
    }
    ri->http11 = (le - line) >= 8 && memcmp(le - 8, "HTTP/1.1", 8) == 0;
    return le;
}

int dyn_scan_head(const char* buf, size_t avail, dyn_reqinfo_t* ri)
{
    const char* end = buf + avail;
    const char* line;
    scan_seen_t seen[SCAN_SEEN_MAX] DYN_IO_SCRATCH;
    size_t nseen = 0;

    memset(ri, 0, sizeof(*ri));
    line = scan_request_line(buf, end, ri);
    if (!line)
        return -1;
    line += 2;
    for (;;) {
        const char *c = line, *v, *ve, *q;
        size_t nlen;
        uint32_t h = 0;
        int kind;

        if (line >= end)
            return -1;
        if (end - line >= 2 && line[0] == '\r' && line[1] == '\n') {
            ri->head_len = (size_t)(line + 2 - buf);
            return 0;
        }
        ri->n_lines++;
        while (c < end) {
            uint8_t lc = scan_name_lc[(unsigned char)*c];
            if (!lc)
                break;
            h = h * 31u + lc;
            c++;
        }
        if (c >= end)
            return -1;
        if (*c != ':' || c == line)
            return scan_crlf_from(line, end) ? DYN_HEAD_ERR_LINE : -1;
        nlen = (size_t)(c - line);
        q = c + 1;
        for (;;) {
            unsigned char ch;
            while (end - q >= 8) {
                uint64_t x;
                memcpy(&x, q, 8);
                if (scan_ctl_any(x))
                    break;
                q += 8;
            }
            if (q >= end)
                return -1;
            ch = (unsigned char)*q;
            if ((ch >= 0x20 && ch != 0x7f) || ch == '\t') {
                q++;
                continue;
            }
            if (ch == '\r' && end - q > 1 && q[1] == '\n')
                break;
            return scan_crlf_from(q, end) ? DYN_HEAD_ERR_LINE : -1;
        }
        v = c + 1;
        ve = q;
        while (v < ve && (*v == ' ' || *v == '\t'))
            v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
            ve--;
        kind = scan_name_kind(line, nlen);
        if (kind != SCAN_NAME_DUP_OK && kind != SCAN_NAME_CONN && !(kind == SCAN_NAME_AE)) {
            size_t i;
            int dup = 0;
            for (i = 0; i < nseen; i++)
                if (seen[i].h == h && seen[i].len == nlen && dyn_ci_eq(line, seen[i].p, nlen)) {
                    dup = 1;
                    break;
                }
            if (dup)
                ri->dup = 1;
            else if (nseen < SCAN_SEEN_MAX) {
                seen[nseen].p = line;
                seen[nseen].len = nlen;
                seen[nseen].h = h;
                nseen++;
            }
        }
        switch (kind) {
        case SCAN_NAME_CL:
            if (!ri->cl_set) {
                ri->cl = v;
                ri->cl_len = (size_t)(ve - v);
                ri->cl_set = 1;
            }
            ri->cl_count++;
            break;
        case SCAN_NAME_TE:
            ri->has_te = 1;
            break;
        case SCAN_NAME_HOST:
            ri->host_count++;
            break;
        case SCAN_NAME_AE:
            if (!ri->ae) {
                ri->ae = v;
                ri->ae_len = (size_t)(ve - v);
            }
            break;
        case SCAN_NAME_CONN:
            if (!ri->conn) {
                ri->conn = v;
                ri->conn_len = (size_t)(ve - v);
            }
            break;
        case SCAN_NAME_CT:
            if (!ri->ct) {
                ri->ct = v;
                ri->ct_len = (size_t)(ve - v);
            }
            break;
        default:
            break;
        }
        line = q + 2;
    }
}

#endif
