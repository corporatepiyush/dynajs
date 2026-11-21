#include "crawlkit.h"

void fe_sm_doc_free(fe_sm_doc_t* d)
{
    size_t i;
    for (i = 0; i < d->n; i++)
        free(d->v[i]);
    free(d->v);
    d->v = NULL;
    d->n = d->cap = 0;
}

static int fe_sm_push_own(fe_sm_doc_t* d, char* c, size_t n)
{
    if (d->n >= FE_SM_MAX_URLS) {
        free(c);
        return FE_SM_E_TOO_MANY;
    }
    if (d->n == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 16;
        char** nv = (char**)realloc(d->v, nc * sizeof(*nv));
        if (!nv) {
            free(c);
            return FE_SM_E_OOM;
        }
        d->v = nv;
        d->cap = nc;
    }
    c[n] = 0;
    d->v[d->n++] = c;
    return FE_SM_OK;
}

static const char* fe_sm_find(const char* hay, size_t hlen, const char* needle, size_t nlen)
{
    size_t i;
    if (nlen == 0 || hlen < nlen)
        return NULL;
    for (i = 0; i <= hlen - nlen; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    return NULL;
}

static size_t fe_sm_entity(const char* s, size_t rem, char out[4], int* outlen)
{
    static const struct {
        const char* nm;
        char c;
    } NAMED[] = {
        { "amp;", '&' },
        { "lt;", '<' },
        { "gt;", '>' },
        { "quot;", '"' },
        { "apos;", '\'' },
    };
    size_t i;
    *outlen = 0;
    if (rem < 4 || s[0] != '&')
        return 0;
    if (s[1] == '#') {
        int hex = (s[2] == 'x' || s[2] == 'X');
        unsigned long cp = 0;
        size_t digits = 0, start = hex ? 3 : 2;
        for (i = start; i < rem && s[i] != ';'; i++) {
            int dv = -1;
            if (s[i] >= '0' && s[i] <= '9')
                dv = s[i] - '0';
            else if (hex && s[i] >= 'a' && s[i] <= 'f')
                dv = s[i] - 'a' + 10;
            else if (hex && s[i] >= 'A' && s[i] <= 'F')
                dv = s[i] - 'A' + 10;
            if (dv < 0)
                return 0;
            cp = cp * (hex ? 16u : 10u) + (unsigned long)dv;
            if (cp > 0x10FFFF)
                return 0;
            digits++;
        }
        if (!digits || i >= rem || s[i] != ';')
            return 0;
        if (cp < 0x80) {
            out[0] = (char)cp;
            *outlen = 1;
        } else if (cp < 0x800) {
            out[0] = (char)(0xC0 | (cp >> 6));
            out[1] = (char)(0x80 | (cp & 0x3F));
            *outlen = 2;
        } else if (cp < 0x10000) {
            out[0] = (char)(0xE0 | (cp >> 12));
            out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (char)(0x80 | (cp & 0x3F));
            *outlen = 3;
        } else {
            out[0] = (char)(0xF0 | (cp >> 18));
            out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[3] = (char)(0x80 | (cp & 0x3F));
            *outlen = 4;
        }
        return i + 1;
    }
    for (i = 0; i < countof(NAMED); i++) {
        size_t ln = strlen(NAMED[i].nm);
        if (rem >= ln + 1 && memcmp(s + 1, NAMED[i].nm, ln) == 0) {
            out[0] = NAMED[i].c;
            *outlen = 1;
            return ln + 1;
        }
    }
    return 0;
}

static int fe_sm_ci_prefix(const char* p, size_t n, size_t at, const char* lit)
{
    size_t k;
    for (k = 0; lit[k]; k++)
        if (at + k >= n || (unsigned char)(p[at + k] | 0x20) != (unsigned char)(lit[k] | 0x20))
            return 0;
    return 1;
}

static int fe_sm_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int fe_sm_loc(const char* p, size_t n, size_t i, size_t name_end, fe_sm_doc_t* out,
    size_t* next, size_t* eoff)
{
    size_t ct = name_end, body, blen, q, max_q;
    const char* cl = NULL;
    while (ct < n && p[ct] != '>')
        ct++;
    if (ct >= n) {
        *eoff = i;
        return FE_SM_E_LOC_TAG;
    }
    if (p[ct - 1] == '/') {
        *next = ct + 1;
        return FE_SM_OK;
    }
    body = ct + 1;
    max_q = body + FE_SM_MAX_LOC_TEXT;
    if (n >= 5) {
        size_t last = max_q < n - 5 ? max_q : n - 5;
        q = body;
        while (q <= last) {
            const char* lt = (const char*)memchr(p + q, '<', last - q + 1);
            if (!lt)
                break;
            q = (size_t)(lt - p);
            if (p[q + 1] == '/' && (p[q + 2] | 0x20) == 'l' && (p[q + 3] | 0x20) == 'o' && (p[q + 4] | 0x20) == 'c') {
                cl = lt;
                break;
            }
            q++;
        }
    }
    if (!cl) {
        *eoff = i;
        return FE_SM_E_LOC_OPEN;
    }
    blen = (size_t)(cl - p) - body;
    cl = (const char*)memchr(p + body, '>', n - body);
    if (!cl) {
        *eoff = body;
        return FE_SM_E_LOC_CLOSE;
    }
    while (blen && fe_sm_space(p[body])) {
        body++;
        blen--;
    }
    while (blen && fe_sm_space(p[body + blen - 1]))
        blen--;
    if (blen) {
        char* dec = (char*)malloc(blen + 1);
        size_t di = 0, si = 0;
        int rc;
        if (!dec)
            return FE_SM_E_OOM;
        while (si < blen) {
            if (p[body + si] == '&') {
                char one[4];
                int one_n = 0;
                size_t used = fe_sm_entity(p + body + si, blen - si, one, &one_n);
                if (used) {
                    if (di + (size_t)one_n > blen) {
                        free(dec);
                        return FE_SM_E_OOM;
                    }
                    memcpy(dec + di, one, (size_t)one_n);
                    di += (size_t)one_n;
                    si += used;
                    continue;
                }
            }
            dec[di++] = p[body + si++];
        }
        rc = fe_sm_push_own(out, dec, di);
        if (rc != FE_SM_OK)
            return rc;
    }
    *next = (size_t)(cl - p) + 1;
    return FE_SM_OK;
}

static int fe_sm_skip(const char* p, size_t n, size_t* pi)
{
    size_t i = *pi;
    const char* e;
    if (fe_sm_ci_prefix(p, n, i + 1, "!--")) {
        e = fe_sm_find(p + i + 4, n - i - 4, "-->", 3);
        if (!e)
            return FE_SM_E_COMMENT;
        *pi = (size_t)(e - p) + 3;
        return -1;
    }
    if (i + 1 < n && p[i + 1] == '?') {
        e = fe_sm_find(p + i + 2, n - i - 2, "?>", 2);
        if (!e)
            return FE_SM_E_PI;
        *pi = (size_t)(e - p) + 2;
        return -1;
    }
    if (fe_sm_ci_prefix(p, n, i + 1, "![CDATA[")) {
        e = fe_sm_find(p + i + 9, n - i - 9, "]]>", 3);
        if (!e)
            return FE_SM_E_CDATA;
        *pi = (size_t)(e - p) + 3;
        return -1;
    }
    if (i + 1 < n && p[i + 1] == '!') {
        e = (const char*)memchr(p + i + 2, '>', n - i - 2);
        if (!e)
            return FE_SM_E_DECL;
        *pi = (size_t)(e - p) + 1;
        return -1;
    }
    return 0;
}

int fe_sm_scan(const char* p, size_t n, fe_sm_doc_t* out, size_t* eoff)
{
    size_t i = 0;
    int root_seen = 0, rc;

    memset(out, 0, sizeof(*out));
    *eoff = 0;
    while (i < n) {
        size_t tag_end, name_end;
        int is_close;
        if (p[i] != '<') {
            const char* lt = (const char*)memchr(p + i, '<', n - i);
            if (!lt)
                break;
            i = (size_t)(lt - p);
        }
        rc = fe_sm_skip(p, n, &i);
        if (rc < 0)
            continue;
        if (rc != 0) {
            *eoff = i;
            goto fail;
        }
        is_close = (i + 1 < n && p[i + 1] == '/');
        name_end = i + 1 + (is_close ? 1 : 0);
        while (name_end < n && p[name_end] != '>' && p[name_end] != '/' && !fe_sm_space(p[name_end]))
            name_end++;
        if (name_end >= n) {
            *eoff = i;
            rc = FE_SM_E_TAG;
            goto fail;
        }
        if (!is_close && !root_seen) {
            if (dyn_ascii_ieq_lower_lit(p + i + 1, name_end - (i + 1), "sitemapindex"))
                out->is_index = 1;
            root_seen = 1;
        }
        if (!is_close && dyn_ascii_ieq_lower_lit(p + i + 1, name_end - (i + 1), "loc")) {
            rc = fe_sm_loc(p, n, i, name_end, out, &i, eoff);
            if (rc != FE_SM_OK)
                goto fail;
            continue;
        }
        tag_end = name_end;
        while (tag_end < n && p[tag_end] != '>')
            tag_end++;
        if (tag_end >= n) {
            *eoff = i;
            rc = FE_SM_E_TAG;
            goto fail;
        }
        i = tag_end + 1;
    }
    return FE_SM_OK;
fail:
    fe_sm_doc_free(out);
    return rc;
}

int fe_sm_format_error(int code, size_t off, char* buf, size_t cap)
{
    switch (code) {
    case FE_SM_E_COMMENT:
        return snprintf(buf, cap, "Sitemap.parse: unterminated comment at byte %zu", off);
    case FE_SM_E_PI:
        return snprintf(buf, cap, "Sitemap.parse: unterminated processing instruction at byte %zu", off);
    case FE_SM_E_CDATA:
        return snprintf(buf, cap, "Sitemap.parse: unterminated CDATA at byte %zu", off);
    case FE_SM_E_DECL:
        return snprintf(buf, cap, "Sitemap.parse: unterminated <! declaration at byte %zu", off);
    case FE_SM_E_TAG:
        return snprintf(buf, cap, "Sitemap.parse: unterminated tag at byte %zu", off);
    case FE_SM_E_LOC_TAG:
        return snprintf(buf, cap, "Sitemap.parse: unterminated <loc> tag at byte %zu", off);
    case FE_SM_E_LOC_OPEN:
        return snprintf(buf, cap, "Sitemap.parse: <loc> at byte %zu never closes (over the %d-byte url budget?)", off, FE_SM_MAX_LOC_TEXT);
    case FE_SM_E_LOC_CLOSE:
        return snprintf(buf, cap, "Sitemap.parse: </loc> at byte %zu never closes", off);
    case FE_SM_E_TOO_MANY:
        return snprintf(buf, cap, "Sitemap.parse: more than %d <loc> entries", FE_SM_MAX_URLS);
    case FE_SM_E_OOM:
        return snprintf(buf, cap, "out of memory");
    default:
        return snprintf(buf, cap, "%s", "");
    }
}
