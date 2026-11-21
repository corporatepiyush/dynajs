#include "httpk.h"

void httpk_sse_init(httpk_sse_t* p, httpk_sse_cb cb, void* ud, size_t max_event)
{
    memset(p, 0, sizeof *p);
    dyn_sb_init(&p->line);
    dyn_sb_init(&p->data);
    dyn_sb_init(&p->type);
    dyn_sb_init(&p->id);
    p->cb = cb;
    p->ud = ud;
    p->max_event = max_event ? max_event : (size_t)16 << 20;
    p->retry_ms = -1;
}

void httpk_sse_free(httpk_sse_t* p)
{
    dyn_sb_free(&p->line);
    dyn_sb_free(&p->data);
    dyn_sb_free(&p->type);
    dyn_sb_free(&p->id);
}

const char* httpk_sse_last_id(const httpk_sse_t* p, size_t* n)
{
    *n = p->id.n;
    return p->id.p ? p->id.p : "";
}

static int httpk_sse_dispatch(httpk_sse_t* p)
{
    httpk_sse_event_t ev;
    int rc;
    if (!p->has_data) {
        p->type.n = 0;
        p->data.n = 0;
        return HTTPK_SSE_OK;
    }
    ev.type = p->type.n ? p->type.p : "message";
    ev.type_len = p->type.n ? p->type.n : 7;
    ev.data = p->data.p ? p->data.p : "";
    ev.data_len = p->data.n ? p->data.n - 1 : 0;
    ev.id = p->id.p ? p->id.p : "";
    ev.id_len = p->id.n;
    ev.retry_ms = p->retry_ms;
    rc = p->cb ? p->cb(p->ud, &ev) : 0;
    p->data.n = 0;
    p->type.n = 0;
    p->has_data = 0;
    p->retry_ms = -1;
    if (rc) {
        p->aborted = 1;
        return HTTPK_SSE_E_ABORT;
    }
    return HTTPK_SSE_OK;
}

static int httpk_sse_line(httpk_sse_t* p, const char* l, size_t n)
{
    size_t c = 0, vb;
    if (!p->bom_checked) {
        p->bom_checked = 1;
        if (n >= 3 && (unsigned char)l[0] == 0xef && (unsigned char)l[1] == 0xbb
            && (unsigned char)l[2] == 0xbf) {
            l += 3;
            n -= 3;
        }
    }
    if (n == 0) {
        return httpk_sse_dispatch(p);
    }
    if (l[0] == ':')
        return HTTPK_SSE_OK;
    while (c < n && l[c] != ':')
        c++;
    vb = c < n ? c + 1 : n;
    if (vb < n && l[vb] == ' ')
        vb++;
    if (c == 5 && memcmp(l, "event", 5) == 0) {
        p->type.n = 0;
        dyn_sb_put(&p->type, l + vb, n - vb);
    } else if (c == 4 && memcmp(l, "data", 4) == 0) {
        if (p->data.n + (n - vb) + 1 > p->max_event)
            return HTTPK_SSE_E_TOO_BIG;
        dyn_sb_put(&p->data, l + vb, n - vb);
        dyn_sb_putc(&p->data, '\n');
        p->has_data = 1;
    } else if (c == 2 && memcmp(l, "id", 2) == 0) {
        if (!memchr(l + vb, 0, n - vb)) {
            p->id.n = 0;
            dyn_sb_put(&p->id, l + vb, n - vb);
        }
    } else if (c == 5 && memcmp(l, "retry", 5) == 0) {
        size_t k;
        int64_t v = 0;
        for (k = vb; k < n; k++) {
            if (l[k] < '0' || l[k] > '9' || v > (INT64_MAX - 9) / 10)
                break;
            v = v * 10 + (l[k] - '0');
        }
        if (k == n && n > vb)
            p->retry_ms = v;
    }
    if (p->data.oom || p->type.oom || p->id.oom)
        return HTTPK_SSE_E_OOM;
    return HTTPK_SSE_OK;
}

int httpk_sse_feed(httpk_sse_t* p, const char* buf, size_t n)
{
    size_t i = 0;
    if (p->aborted)
        return HTTPK_SSE_E_ABORT;
    while (i < n) {
        size_t j;
        int rc;
        if (p->pending_cr) {
            p->pending_cr = 0;
            if (buf[i] == '\n') {
                i++;
                continue;
            }
        }
        {
            const char* nl = (const char*)memchr(buf + i, '\n', n - i);
            size_t e = nl ? (size_t)(nl - buf) : n;
            const char* cr = (const char*)memchr(buf + i, '\r', e - i);
            j = cr ? (size_t)(cr - buf) : e;
        }
        if (j == n) {
            if (p->line.n + (n - i) > p->max_event)
                return HTTPK_SSE_E_TOO_BIG;
            dyn_sb_put(&p->line, buf + i, n - i);
            return p->line.oom ? HTTPK_SSE_E_OOM : HTTPK_SSE_OK;
        }
        if (p->line.n) {
            if (p->line.n + (j - i) > p->max_event)
                return HTTPK_SSE_E_TOO_BIG;
            dyn_sb_put(&p->line, buf + i, j - i);
            if (p->line.oom)
                return HTTPK_SSE_E_OOM;
            rc = httpk_sse_line(p, p->line.p, p->line.n);
            p->line.n = 0;
        } else {
            rc = httpk_sse_line(p, buf + i, j - i);
        }
        if (rc)
            return rc;
        if (buf[j] == '\r') {
            if (j + 1 < n) {
                if (buf[j + 1] == '\n')
                    j++;
            } else {
                p->pending_cr = 1;
            }
        }
        i = j + 1;
    }
    return HTTPK_SSE_OK;
}

static int httpk_sse_field_ok(const char* s, size_t n)
{
    return !memchr(s, '\n', n) && !memchr(s, '\r', n) && !memchr(s, 0, n);
}

int httpk_sse_frame(dyn_sb_t* out, const char* event, size_t en,
    const char* id, size_t idn, const char* data, size_t dn, int64_t retry_ms)
{
    size_t i = 0;
    if ((event && !httpk_sse_field_ok(event, en)) || (id && !httpk_sse_field_ok(id, idn)))
        return -1;
    if (event) {
        dyn_sb_put(out, "event: ", 7);
        dyn_sb_put(out, event, en);
        dyn_sb_putc(out, '\n');
    }
    if (id) {
        dyn_sb_put(out, "id: ", 4);
        dyn_sb_put(out, id, idn);
        dyn_sb_putc(out, '\n');
    }
    if (retry_ms >= 0) {
        char num[24];
        int k = 0;
        char tmp[24];
        int64_t v = retry_ms;
        do {
            tmp[k++] = (char)('0' + (int)(v % 10));
            v /= 10;
        } while (v && k < (int)sizeof tmp);
        {
            int m;
            for (m = 0; m < k; m++)
                num[m] = tmp[k - 1 - m];
        }
        dyn_sb_put(out, "retry: ", 7);
        dyn_sb_put(out, num, (size_t)k);
        dyn_sb_putc(out, '\n');
    }
    for (;;) {
        size_t ls = i;
        while (i < dn && data[i] != '\n' && data[i] != '\r')
            i++;
        dyn_sb_put(out, "data: ", 6);
        dyn_sb_put(out, data + ls, i - ls);
        dyn_sb_putc(out, '\n');
        if (i >= dn)
            break;
        if (data[i] == '\r' && i + 1 < dn && data[i + 1] == '\n')
            i++;
        i++;
    }
    dyn_sb_putc(out, '\n');
    return out->oom ? -1 : 0;
}
