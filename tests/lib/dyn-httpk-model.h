/* dyn-httpk-model.h -- src/core/httpk model shared by tests/http/test_httpk_core.c (seeded rng) and
 * src/fuzz/fuzz_httpk.c (fuzz bytes). hm_check_head: a generated or mutated response head against an independent
 * reference parser (RFC 9112 4/5, 6.1/6.3 framing, RFC 9110 5.3 singletons) and httpk_head_end fed in random steps
 * against one shot; hm_check_chunk: a generated chunked body decodes to the record when split anywhere, and any bytes
 * decode the same split or whole and the same as an RFC 9112 7.1 reference dechunker (with httpk's size, extension and
 * trailer limits; a trailer line may not start with LF or NUL); hm_check_sse: the WHATWG event-stream algorithm over
 * the whole buffer against httpk_sse_feed in random chunks; hm_heavy adds fk_mutate edits to every generated
 * buffer; hm_check_date: IMF-fixdate / asctime / cookie-date against a proleptic Gregorian calendar
 * (no 29 Feb in a common year). Each returns NULL or a message naming the broken rule. */
#ifndef DYN_HTTPK_MODEL_H
#define DYN_HTTPK_MODEL_H

#include "dyn-httpk.h"
#include "dyn-fuzzkit.h"

#include <stdio.h>
#include <string.h>

#define HM_CAP 8192
#define HM_EV 256
#define HM_EXT_MAX 4096
#define HM_TRAILER_MAX 16384

static int hm_heavy;

typedef struct {
    const uint8_t* p;
    size_t n;
    fk_rng_t* r;
} hm_src_t;

static uint32_t hm_pick(hm_src_t* s, uint32_t k)
{
    uint32_t v;
    if (s->r)
        return fk_below(s->r, k);
    if (!s->n || !k)
        return 0;
    v = *s->p++;
    s->n--;
    return v % k;
}

typedef struct {
    char b[HM_CAP];
    size_t n;
} hm_buf_t;

static void hm_put(hm_buf_t* b, const char* p, size_t n)
{
    if (b->n + n > HM_CAP)
        n = HM_CAP - b->n;
    memcpy(b->b + b->n, p, n);
    b->n += n;
}

static void hm_puts(hm_buf_t* b, const char* s) { hm_put(b, s, strlen(s)); }

static void hm_rand(hm_src_t* s, hm_buf_t* b, const char* alpha, uint32_t max)
{
    uint32_t k = hm_pick(s, max + 1), al = (uint32_t)strlen(alpha);
    while (k--) {
        char c = alpha[hm_pick(s, al)];
        hm_put(b, &c, 1);
    }
}

static void hm_mutate(hm_src_t* s, hm_buf_t* b)
{
    static const char hot[] = "\r\n\0 \t:,;0aZ\x7f\xff";
    uint32_t k = hm_pick(s, 4);
    while (k-- && b->n) {
        size_t i = hm_pick(s, (uint32_t)b->n);
        b->b[i] = hm_pick(s, 2) ? hot[hm_pick(s, sizeof hot - 1)] : (char)hm_pick(s, 256);
    }
    if (hm_heavy && s->r)
        fk_mutate(s->r, (uint8_t*)b->b, b->n, 1 + fk_below(s->r, 8));
    if (hm_pick(s, 4) == 0)
        b->n = hm_pick(s, (uint32_t)b->n + 1);
}

static int hm_lceq(const char* a, size_t n, const char* lit)
{
    size_t i;
    if (strlen(lit) != n)
        return 0;
    for (i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)lit[i];
        if ((x >= 'A' && x <= 'Z' ? x + 32 : x) != y)
            return 0;
    }
    return 1;
}

static int hm_tchar(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static int hm_vbyte(unsigned char c) { return c == '\t' || (c >= 0x20 && c != 0x7f); }

static int hm_list(const char* v, size_t n, const char* tok, int last_only)
{
    size_t i = 0;
    int hit = 0, any = 0;
    while (i <= n) {
        size_t j = i, b, e;
        while (j < n && v[j] != ',')
            j++;
        b = i;
        e = j;
        while (b < e && (v[b] == ' ' || v[b] == '\t'))
            b++;
        while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t'))
            e--;
        if (e > b) {
            int m = hm_lceq(v + b, e - b, tok);
            hit = last_only ? m : (hit || m);
            any = 1;
        }
        i = j + 1;
    }
    return any && hit;
}

static int hm_ci2(const char* a, size_t an, const char* b, size_t bn)
{
    size_t i;
    if (an != bn)
        return 0;
    for (i = 0; i < an; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if ((x >= 'A' && x <= 'Z' ? x + 32 : x) != (y >= 'A' && y <= 'Z' ? y + 32 : y))
            return 0;
    }
    return 1;
}

typedef struct {
    int rc, status, minor;
    size_t reason, reason_len, n;
    size_t nm[64], nl[64], vl[64], vo[64];
    uint64_t cl;
    int has_cl, chunked, te_unframed, close, keepalive;
    uint32_t dup;
} hm_head_t;

static void hm_ref_head(const char* p, size_t len, hm_head_t* r)
{
    size_t ls, le, i, k;
    memset(r, 0, sizeof *r);
    for (le = 0; le + 1 < len && !(p[le] == '\r' && p[le + 1] == '\n'); le++)
        if (p[le] == '\n' || p[le] == '\r')
            break;
    if (len < 4 || le + 1 >= len || p[le] != '\r' || p[le + 1] != '\n' || le < 10 || memcmp(p, "HTTP/", 5)) {
        r->rc = HTTPK_E_STATUS;
        return;
    }
    i = 6;
    r->minor = -1;
    if (!(p[5] >= '0' && p[5] <= '9') || (p[6] == '.' && !(p[7] >= '0' && p[7] <= '9'))) {
        r->rc = HTTPK_E_STATUS;
        return;
    }
    if (p[6] == '.') {
        r->minor = p[7] - '0';
        i = 8;
    }
    if (i + 4 > le || p[i] != ' ' || !(p[i + 1] >= '0' && p[i + 1] <= '9' && p[i + 2] >= '0' && p[i + 2] <= '9' && p[i + 3] >= '0' && p[i + 3] <= '9')
        || (i + 4 < le && p[i + 4] != ' ')) {
        r->rc = HTTPK_E_STATUS;
        return;
    }
    r->status = (p[i + 1] - '0') * 100 + (p[i + 2] - '0') * 10 + (p[i + 3] - '0');
    r->reason = i + 4 < le ? i + 5 : le;
    r->reason_len = le - r->reason;
    for (k = r->reason; k < le; k++)
        if (!hm_vbyte((unsigned char)p[k]))
            r->status = 1000;
    if (r->status > 599) {
        r->rc = HTTPK_E_STATUS;
        return;
    }
    for (ls = le + 2;; ls = le + 2) {
        size_t c, vb, ve;
        if (ls + 2 > len)
            break;
        for (le = ls; le < len && p[le] != '\r' && p[le] != '\n'; le++)
            ;
        if (le + 1 >= len || p[le] != '\r' || p[le + 1] != '\n') {
            r->rc = HTTPK_E_LINE;
            return;
        }
        if (le == ls)
            break;
        if (p[ls] == ' ' || p[ls] == '\t') {
            r->rc = HTTPK_E_FOLD;
            return;
        }
        for (c = ls; c < le && p[c] != ':'; c++)
            if (!hm_tchar((unsigned char)p[c])) {
                r->rc = HTTPK_E_LINE;
                return;
            }
        if (c == ls || c == le) {
            r->rc = HTTPK_E_LINE;
            return;
        }
        for (vb = c + 1; vb < le && (p[vb] == ' ' || p[vb] == '\t'); vb++)
            ;
        for (ve = le; ve > vb && (p[ve - 1] == ' ' || p[ve - 1] == '\t'); ve--)
            ;
        for (k = vb; k < ve; k++)
            if (!hm_vbyte((unsigned char)p[k])) {
                r->rc = HTTPK_E_LINE;
                return;
            }
        if (r->n == 64) {
            r->rc = HTTPK_E_TOO_MANY;
            return;
        }
        r->nm[r->n] = ls;
        r->nl[r->n] = c - ls;
        r->vo[r->n] = vb;
        r->vl[r->n++] = ve - vb;
        if (hm_lceq(p + ls, c - ls, "content-length")) {
            uint64_t x = 0;
            if (ve == vb) {
                r->rc = HTTPK_E_CL_EMPTY;
                return;
            }
            for (k = vb; k < ve; k++) {
                if (p[k] < '0' || p[k] > '9' || x > (UINT64_MAX - 9) / 10) {
                    r->rc = HTTPK_E_CL;
                    return;
                }
                x = x * 10 + (uint64_t)(p[k] - '0');
            }
            if (r->has_cl && x != r->cl) {
                r->rc = HTTPK_E_CL_DUP;
                return;
            }
            r->cl = x;
            r->has_cl = 1;
        } else if (hm_lceq(p + ls, c - ls, "transfer-encoding") && ve > vb) {
            r->chunked = hm_list(p + vb, ve - vb, "chunked", 1);
            r->te_unframed = !r->chunked;
        } else if (hm_lceq(p + ls, c - ls, "connection")) {
            r->close |= hm_list(p + vb, ve - vb, "close", 0);
            r->keepalive |= hm_list(p + vb, ve - vb, "keep-alive", 0);
        }
    }
    if (r->chunked && r->has_cl) {
        r->rc = HTTPK_E_CL_TE;
        return;
    }
    for (i = 1; i < r->n; i++)
        if (httpk_field_kind(p + r->nm[i], r->nl[i]) == HTTPK_FIELD_SINGLETON)
            for (k = 0; k < i; k++)
                if (hm_ci2(p + r->nm[i], r->nl[i], p + r->nm[k], r->nl[k])) {
                    if (r->vl[i] != r->vl[k] || memcmp(p + r->vo[i], p + r->vo[k], r->vl[i])) {
                        r->dup = (uint32_t)i;
                        r->rc = HTTPK_E_DUP;
                        return;
                    }
                    break;
                }
    if (r->minor == 0 && !r->keepalive)
        r->close = 1;
}

static void hm_gen_head(hm_src_t* s, hm_buf_t* b)
{
    static const char* names[] = { "Content-Length", "Transfer-Encoding", "Connection", "Content-Type", "Host", "ETag",
        "Set-Cookie", "Location", "Date", "X-Req", "content-length", "TRANSFER-ENCODING", "Age", "Vary" };
    static const char* vals[] = { "0", "12", "18446744073709551615", "18446744073709551616", "chunked", "gzip, chunked",
        "chunked, gzip", "gzip", "close", "keep-alive", "Upgrade, close", "text/html", "a=b", "", " 7 ", "1,2" };
    static const char vchar[] = "abcXYZ019 \t!\"#,;=()\x80\xff";
    uint32_t i, nh = hm_pick(s, 12);
    char st[16];
    b->n = 0;
    hm_puts(b, hm_pick(s, 6) ? (hm_pick(s, 3) ? "HTTP/1.1" : "HTTP/1.0") : "HTTP/2");
    snprintf(st, sizeof st, " %03u", 100 + hm_pick(s, 500));
    hm_puts(b, st);
    if (hm_pick(s, 4)) {
        hm_puts(b, " ");
        hm_rand(s, b, vchar, 12);
    }
    hm_puts(b, "\r\n");
    for (i = 0; i < nh; i++) {
        uint32_t k = hm_pick(s, 16);
        if (k < 14)
            hm_puts(b, names[k]);
        else
            hm_rand(s, b, "abcXYZ-_!~", 6);
        hm_puts(b, hm_pick(s, 3) ? ": " : ":");
        if (hm_pick(s, 3))
            hm_puts(b, vals[hm_pick(s, 16)]);
        else
            hm_rand(s, b, vchar, 10);
        if (!hm_pick(s, 4))
            hm_puts(b, " \t");
        hm_puts(b, "\r\n");
    }
    hm_puts(b, "\r\n");
}

static const char* hm_check_head(hm_src_t* s, const uint8_t* raw, size_t rawn)
{
    static hm_buf_t b;
    httpk_hdr_t hv[64];
    httpk_head_t h;
    hm_head_t r;
    size_t end, scan = 0, at = 0, i, e2 = 0;
    if (raw) {
        b.n = rawn > HM_CAP ? HM_CAP : rawn;
        memcpy(b.b, raw, b.n);
    } else {
        hm_gen_head(s, &b);
        if (hm_heavy || hm_pick(s, 3) == 0)
            hm_mutate(s, &b);
    }
    end = httpk_head_end(b.b, b.n, NULL);
    while (at < b.n && !e2) {
        at += 1 + hm_pick(s, 64);
        if (at > b.n)
            at = b.n;
        e2 = httpk_head_end(b.b, at, &scan);
    }
    if (e2 != end)
        return "head_end fed in steps disagrees with one shot";
    if (end) {
        for (i = 0; i + 3 < end - 1; i++)
            if (!memcmp(b.b + i, "\r\n\r\n", 4))
                return "head_end missed an earlier CRLFCRLF";
        if (memcmp(b.b + end - 4, "\r\n\r\n", 4))
            return "head_end does not end on CRLFCRLF";
    }
    if (!end)
        end = b.n;
    memset(&h, 0, sizeof h);
    h.hdrs = hv;
    h.cap_hdrs = 64;
    h.minor = -1;
    {
        int rc = httpk_resp_head(b.b, end, &h);
        hm_ref_head(b.b, end, &r);
        if (rc != r.rc)
            return "resp_head result differs from the reference parser";
        if (rc == HTTPK_E_DUP && h.dup_field != r.dup)
            return "resp_head names a different duplicate field";
        if (rc != HTTPK_OK)
            return NULL;
    }
    if (h.status != r.status || h.minor != r.minor || h.reason != r.reason || h.reason_len != r.reason_len || h.n_hdrs != r.n)
        return "status line or field count differs from the reference";
    for (i = 0; i < r.n; i++)
        if (hv[i].name != r.nm[i] || hv[i].name_len != r.nl[i] || hv[i].value != r.vo[i] || hv[i].value_len != r.vl[i])
            return "a field span differs from the reference";
    if (h.has_cl != r.has_cl || (r.has_cl && h.content_length != r.cl) || h.chunked != r.chunked
        || h.te_unframed != r.te_unframed || h.conn_close != r.close || h.conn_keepalive != r.keepalive)
        return "framing flags differ from the reference";
    return NULL;
}

typedef struct {
    int rc;
    size_t used, outn;
    uint64_t total;
    char out[HM_CAP];
} hm_dc_t;

static void hm_dechunk_split(hm_src_t* s, const char* in, size_t n, int whole, hm_dc_t* o)
{
    static uint8_t tmp[HM_CAP];
    httpk_dechunk_t d;
    size_t at = 0;
    httpk_dechunk_init(&d);
    o->rc = HTTPK_DC_MORE;
    o->used = o->outn = 0;
    while (o->rc == HTTPK_DC_MORE && at < n) {
        size_t step = whole ? n - at : 1 + hm_pick(s, 40), used = 0, outn = 0;
        if (step > n - at)
            step = n - at;
        memcpy(tmp, in + at, step);
        o->rc = httpk_dechunk(&d, tmp, step, &used, &outn);
        if (o->rc == HTTPK_DC_BAD)
            break;
        if (outn > used || used > step) {
            o->rc = -9;
            return;
        }
        memcpy(o->out + o->outn, tmp, outn);
        o->outn += outn;
        o->used += used;
        at += used;
        if (o->rc == HTTPK_DC_MORE && used != step) {
            o->rc = -8;
            return;
        }
    }
    o->total = d.total;
}

static int hm_hexv(uint8_t c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int hm_ref_dechunk(const uint8_t* p, size_t n, uint8_t* out, size_t* outn, size_t* used)
{
    size_t i = 0, o = 0;
    for (;;) {
        uint64_t v = 0;
        unsigned d = 0, k = 1, cnt = 0;
        int st = 0;
        while (i < n && hm_hexv(p[i]) >= 0) {
            if ((v >> 48) || d == 255)
                return HTTPK_DC_BAD;
            v = v << 4 | (uint64_t)hm_hexv(p[i++]);
            d++;
        }
        if (i == n)
            goto more;
        if (!d)
            return HTTPK_DC_BAD;
        for (;;) {
            uint8_t c;
            if (i == n)
                goto more;
            c = p[i++];
            if (st == 0) {
                if (c == '\r')
                    break;
                if (c != ' ' && c != '\t' && c != ';')
                    return HTTPK_DC_BAD;
                st = c == ';' ? 2 : 1;
                continue;
            }
            if (st == 1 && c != ' ' && c != '\t' && c != ';' && c != '\r')
                return HTTPK_DC_BAD;
            if (st == 2 && c != '\r' && ((c < 0x20 && c != '\t') || c == 0x7f))
                return HTTPK_DC_BAD;
            if (c == ';')
                st = 2;
            if (++k > HM_EXT_MAX)
                return HTTPK_DC_BAD;
            if (c == '\r')
                break;
        }
        if (i == n)
            goto more;
        if (p[i++] != '\n')
            return HTTPK_DC_BAD;
        if (v) {
            size_t take = n - i < v ? n - i : (size_t)v;
            memcpy(out + o, p + i, take);
            o += take;
            i += take;
            if (take < v || i == n)
                goto more;
            if (p[i++] != '\r')
                return HTTPK_DC_BAD;
            if (i == n)
                goto more;
            if (p[i++] != '\n')
                return HTTPK_DC_BAD;
            continue;
        }
        for (;;) {
            uint8_t c;
            if (i == n)
                goto more;
            c = p[i++];
            if (c == '\r') {
                if (i == n)
                    goto more;
                if (p[i++] != '\n')
                    return HTTPK_DC_BAD;
                *outn = o;
                *used = i;
                return HTTPK_DC_DONE;
            }
            do {
                if (c == '\n' || c == 0 || ++cnt > HM_TRAILER_MAX)
                    return HTTPK_DC_BAD;
                if (c == '\r')
                    break;
                if (i == n)
                    goto more;
                c = p[i++];
            } while (1);
            if (i == n)
                goto more;
            if (p[i++] != '\n')
                return HTTPK_DC_BAD;
        }
    }
more:
    *outn = o;
    *used = n;
    return HTTPK_DC_MORE;
}

static const char* hm_check_chunk(hm_src_t* s, const uint8_t* raw, size_t rawn)
{
    static hm_buf_t b;
    static char want[HM_CAP];
    static hm_dc_t x, y;
    static uint8_t rout[HM_CAP];
    size_t ro = 0, ru = 0;
    int rr;
    size_t wn = 0, enc_n = 0;
    int gen = !raw;
    if (raw) {
        b.n = rawn > HM_CAP ? HM_CAP : rawn;
        memcpy(b.b, raw, b.n);
    } else {
        uint32_t nc = hm_pick(s, 6), i;
        b.n = 0;
        for (i = 0; i <= nc; i++) {
            uint32_t len = i == nc ? 0 : 1 + hm_pick(s, 300), z = hm_pick(s, 3), k;
            char hex[32];
            for (k = 0; k < z; k++)
                hm_puts(&b, "0");
            snprintf(hex, sizeof hex, hm_pick(s, 2) ? "%x" : "%X", len);
            hm_puts(&b, hex);
            if (!hm_pick(s, 4))
                hm_puts(&b, " \t");
            if (!hm_pick(s, 3)) {
                hm_puts(&b, ";");
                hm_rand(s, &b, "ab=\"; \t\x80", 20);
            }
            hm_puts(&b, "\r\n");
            if (len) {
                size_t at = b.n;
                for (k = 0; k < len && b.n < HM_CAP - 600; k++) {
                    char c = (char)hm_pick(s, 256);
                    hm_put(&b, &c, 1);
                }
                if (b.n - at != len)
                    return NULL;
                memcpy(want + wn, b.b + at, len);
                wn += len;
                hm_puts(&b, "\r\n");
            }
        }
        for (i = hm_pick(s, 3); i; i--) {
            hm_rand(s, &b, "Xab-", 6);
            hm_puts(&b, ": v\x80\t\r\n");
        }
        hm_puts(&b, "\r\n");
        enc_n = b.n;
        if (hm_pick(s, 2))
            hm_puts(&b, "HTTP/1.1 200 OK\r\n");
        if (hm_heavy || hm_pick(s, 3) == 0) {
            hm_mutate(s, &b);
            gen = 0;
        }
    }
    hm_dechunk_split(s, b.b, b.n, 1, &x);
    hm_dechunk_split(s, b.b, b.n, 0, &y);
    if (x.rc < -1 || y.rc < -1)
        return "dechunk broke its consumed / out_n contract";
    if (x.rc != y.rc)
        return "dechunk result depends on how the body was split";
    if (x.rc != HTTPK_DC_BAD && (x.outn != y.outn || memcmp(x.out, y.out, x.outn) || x.total != y.total || x.total != x.outn))
        return "dechunk output depends on how the body was split";
    if (x.rc == HTTPK_DC_DONE && x.used != y.used)
        return "dechunk stops at a different byte when split";
    rr = hm_ref_dechunk((const uint8_t*)b.b, b.n, rout, &ro, &ru);
    if (rr != x.rc)
        return "dechunk result differs from the RFC 9112 7.1 reference";
    if (rr != HTTPK_DC_BAD && (ro != x.outn || memcmp(rout, x.out, ro)))
        return "dechunk output differs from the RFC 9112 7.1 reference";
    if (rr == HTTPK_DC_DONE && ru != x.used)
        return "dechunk stops at a different byte than the RFC 9112 7.1 reference";
    if (gen && (x.rc != HTTPK_DC_DONE || x.used != enc_n || x.outn != wn || memcmp(x.out, want, wn)))
        return "a valid chunked body does not decode to its record";
    return NULL;
}

typedef struct {
    size_t n;
    char log[HM_CAP * 2];
} hm_evlog_t;

static void hm_ev_add(hm_evlog_t* l, const char* t, size_t tn, const char* d, size_t dn, const char* id, size_t in, int64_t retry)
{
    char hdr[96];
    int k = snprintf(hdr, sizeof hdr, "[%zu|%zu|%zu|%lld]", tn, dn, in, (long long)retry);
    if (l->n + (size_t)k + tn + dn + in > sizeof l->log)
        return;
    memcpy(l->log + l->n, hdr, (size_t)k);
    l->n += (size_t)k;
    memcpy(l->log + l->n, t, tn);
    l->n += tn;
    memcpy(l->log + l->n, d, dn);
    l->n += dn;
    memcpy(l->log + l->n, id, in);
    l->n += in;
}

static int hm_sse_cb(void* ud, const httpk_sse_event_t* ev)
{
    hm_ev_add((hm_evlog_t*)ud, ev->type, ev->type_len, ev->data, ev->data_len, ev->id, ev->id_len, ev->retry_ms);
    return 0;
}

static void hm_ref_sse(const char* p, size_t n, hm_evlog_t* l)
{
    static char data[HM_CAP], type[HM_CAP], id[HM_CAP];
    size_t dn = 0, tn = 0, in = 0, i = 0;
    int has = 0;
    int64_t retry = -1;
    l->n = 0;
    if (n >= 3 && !memcmp(p, "\xef\xbb\xbf", 3))
        i = 3;
    while (i < n) {
        size_t e = i, c, vb;
        while (e < n && p[e] != '\r' && p[e] != '\n')
            e++;
        if (e == n)
            break;
        for (c = i; c < e && p[c] != ':'; c++)
            ;
        vb = c < e ? c + 1 : e;
        if (vb < e && p[vb] == ' ')
            vb++;
        if (e == i) {
            if (has)
                hm_ev_add(l, tn ? type : "message", tn ? tn : 7, data, dn ? dn - 1 : 0, id, in, retry), retry = -1;
            dn = tn = 0;
            has = 0;
        } else if (p[i] == ':') {
        } else if (c - i == 5 && !memcmp(p + i, "event", 5)) {
            memcpy(type, p + vb, e - vb);
            tn = e - vb;
        } else if (c - i == 4 && !memcmp(p + i, "data", 4)) {
            memcpy(data + dn, p + vb, e - vb);
            dn += e - vb;
            data[dn++] = '\n';
            has = 1;
        } else if (c - i == 2 && !memcmp(p + i, "id", 2)) {
            if (!memchr(p + vb, 0, e - vb)) {
                memcpy(id, p + vb, e - vb);
                in = e - vb;
            }
        } else if (c - i == 5 && !memcmp(p + i, "retry", 5) && e > vb) {
            size_t k;
            int64_t v = 0;
            for (k = vb; k < e && p[k] >= '0' && p[k] <= '9' && v <= (INT64_MAX - 9) / 10; k++)
                v = v * 10 + (p[k] - '0');
            if (k == e)
                retry = v;
        }
        i = e + (p[e] == '\r' && e + 1 < n && p[e + 1] == '\n' ? 2 : 1);
    }
}

static const char* hm_check_sse(hm_src_t* s, const uint8_t* raw, size_t rawn)
{
    static const char* lines[] = { "data: x", "data:y", "data", "data:  two", "event: e", "event", "id: 7", "id",
        "retry: 250", "retry: 9x", "retry:", ": note", "", "", "foo: bar", "id: a\0b", "data: \xc3\xa9", "dat", ":" };
    static const char* ends[] = { "\n", "\r", "\r\n" };
    static hm_buf_t b;
    static hm_evlog_t want, got;
    httpk_sse_t ps;
    size_t at = 0;
    int rc = 0;
    if (raw) {
        b.n = rawn > HM_CAP / 2 ? HM_CAP / 2 : rawn;
        memcpy(b.b, raw, b.n);
    } else {
        uint32_t k = hm_pick(s, 24);
        b.n = 0;
        if (!hm_pick(s, 5))
            hm_puts(&b, "\xef\xbb\xbf");
        while (k--) {
            uint32_t w = hm_pick(s, 20);
            if (w == 15)
                hm_put(&b, "id: a\0b", 7);
            else if (w == 19)
                hm_rand(s, &b, "ab: \r\n\0", 8);
            else
                hm_puts(&b, lines[w]);
            hm_puts(&b, ends[hm_pick(s, 3)]);
        }
        if (hm_heavy || hm_pick(s, 3) == 0)
            hm_mutate(s, &b);
    }
    hm_ref_sse(b.b, b.n, &want);
    got.n = 0;
    httpk_sse_init(&ps, hm_sse_cb, &got, HM_CAP);
    while (!rc && at < b.n) {
        size_t step = 1 + hm_pick(s, 16);
        if (step > b.n - at)
            step = b.n - at;
        rc = httpk_sse_feed(&ps, b.b + at, step);
        at += step;
    }
    httpk_sse_free(&ps);
    if (rc)
        return "sse_feed failed on a stream under max_event";
    if (got.n != want.n || memcmp(got.log, want.log, got.n))
        return "sse events differ from the WHATWG reference";
    return NULL;
}

static int hm_mdays(int y, int m)
{
    static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return md[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
}

static int64_t hm_leaps(int64_t x) { return x / 4 - x / 100 + x / 400; }

static int64_t hm_epoch(int y, int mo, int d, int hh, int mi, int ss)
{
    int64_t days = 365 * (int64_t)(y - 1970) + hm_leaps(y - 1) - hm_leaps(1969);
    int k;
    for (k = 1; k < mo; k++)
        days += hm_mdays(y, k);
    return (days + d - 1) * 86400 + hh * 3600 + mi * 60 + ss;
}

static const char* hm_check_date(hm_src_t* s)
{
    static const char* mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char buf[64];
    int y = 1601 + (int)hm_pick(s, 8399), mo = 1 + (int)hm_pick(s, 12), d = 1 + (int)hm_pick(s, 31);
    int hh = (int)hm_pick(s, 24), mi = (int)hm_pick(s, 60), ss = (int)hm_pick(s, 61), ok = d <= hm_mdays(y, mo);
    int64_t want = ok ? hm_epoch(y, mo, d, hh, mi, ss) : -1, got;
    if (hm_pick(s, 4) == 0) {
        y = 1900 + 4 * (int)hm_pick(s, 60) + (int)hm_pick(s, 2) * 100;
        mo = 2;
        d = 29;
        ok = hm_mdays(y, 2) == 29;
        want = ok ? hm_epoch(y, mo, d, hh, mi, ss) : -1;
    }
    snprintf(buf, sizeof buf, "Mon, %02d %s %04d %02d:%02d:%02d GMT", d, mon[mo - 1], y, hh, mi, ss);
    if ((got = httpk_http_date(buf, strlen(buf))) != want)
        return "IMF-fixdate parse differs from the calendar";
    snprintf(buf, sizeof buf, "Mon %s %2d %02d:%02d:%02d %04d", mon[mo - 1], d, hh, mi, ss, y);
    if (httpk_http_date(buf, strlen(buf)) != want)
        return "asctime-date parse differs from the calendar";
    snprintf(buf, sizeof buf, "%d %s %04d %02d:%02d:%02d", d, mon[mo - 1], y, hh, mi, ss);
    if (httpk_cookie_date(buf, strlen(buf)) != (ss > 59 ? -1 : want))
        return "cookie-date parse differs from the calendar";
    if (ok) {
        char fx[HTTPK_IMF_FIXDATE_LEN + 1];
        size_t fn = httpk_imf_fixdate(want, fx);
        if (fn != HTTPK_IMF_FIXDATE_LEN || httpk_http_date(fx, fn) != want)
            return "imf_fixdate does not round-trip through http_date";
    }
    return NULL;
}

#endif
