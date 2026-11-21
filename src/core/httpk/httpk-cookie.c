#include "httpk.h"

void httpk_jar_init(httpk_jar_t* j, size_t max_cookies)
{
    memset(j, 0, sizeof *j);
    j->max_cookies = max_cookies ? max_cookies : 3000;
}

static void httpk_ck_free(httpk_cookie_t* c)
{
    free(c->name);
    free(c->value);
    free(c->domain);
    free(c->path);
}

static void httpk_jar_del(httpk_jar_t* j, size_t i)
{
    httpk_ck_free(&j->v[i]);
    j->v[i] = j->v[--j->n];
}

void httpk_jar_free(httpk_jar_t* j)
{
    size_t i;
    for (i = 0; i < j->n; i++)
        httpk_ck_free(&j->v[i]);
    free(j->v);
    memset(j, 0, sizeof *j);
}

void httpk_jar_purge(httpk_jar_t* j, int64_t now_s)
{
    size_t i = 0;
    while (i < j->n) {
        if (j->v[i].persistent && j->v[i].expires <= now_s)
            httpk_jar_del(j, i);
        else
            i++;
    }
}

static int httpk_wsp(char c) { return c == ' ' || c == '\t'; }

static void httpk_trim(const char* s, size_t* b, size_t* e)
{
    while (*b < *e && httpk_wsp(s[*b]))
        (*b)++;
    while (*e > *b && httpk_wsp(s[*e - 1]))
        (*e)--;
}

static int httpk_is_ip(const char* h, size_t n)
{
    size_t i, dots = 0;
    if (n && h[0] == '[')
        return 1;
    for (i = 0; i < n; i++) {
        if (h[i] == '.')
            dots++;
        else if (h[i] < '0' || h[i] > '9')
            return 0;
    }
    return dots == 3;
}

static int httpk_domain_match(const char* host, size_t hn, const char* dom,
    size_t dn)
{
    if (hn == dn)
        return memcmp(host, dom, hn) == 0;
    if (hn > dn && host[hn - dn - 1] == '.' && memcmp(host + hn - dn, dom, dn) == 0)
        return !httpk_is_ip(host, hn);
    return 0;
}

static int httpk_path_match(const char* rp, size_t rn, const char* cp)
{
    size_t cn = strlen(cp);
    if (rn < cn || memcmp(rp, cp, cn) != 0)
        return 0;
    return rn == cn || cp[cn - 1] == '/' || rp[cn] == '/';
}

typedef struct {
    char host[256];
    size_t hn;
    const char* path;
    size_t pn;
    int https;
} httpk_req_t;

static int httpk_req_of(const char* url, httpk_req_t* r)
{
    httpk_url_t u;
    size_t i;
    if (!url || httpk_url_split(url, strlen(url), &u) < 0 || u.host_len >= sizeof r->host)
        return -1;
    for (i = 0; i < u.host_len; i++)
        r->host[i] = (char)httpk_lc((unsigned char)url[u.host + i]);
    r->hn = u.host_len;
    r->host[r->hn] = 0;
    r->path = u.path_len ? url + u.path : "/";
    r->pn = u.path_len ? u.path_len : 1;
    r->https = u.https;
    return 0;
}

static char* httpk_default_path(const char* p, size_t n)
{
    size_t k;
    if (n == 0 || p[0] != '/')
        return httpk_strndup("/", 1);
    k = n;
    while (k > 0 && p[k - 1] != '/')
        k--;
    if (k <= 1)
        return httpk_strndup("/", 1);
    return httpk_strndup(p, k - 1);
}

static int httpk_attr_is(const char* s, size_t n, const char* lit)
{
    return httpk_ci_eq(s, n, lit);
}

int httpk_jar_set(httpk_jar_t* j, const char* request_url, const char* sc,
    size_t n, int64_t now_s)
{
    httpk_req_t rq;
    httpk_cookie_t ck;
    size_t i = 0, nb, ne, vb, ve, k;
    int have_max_age = 0;
    int64_t expires = 0, max_age_exp = 0;
    int have_expires = 0;
    char* dom_attr = NULL;
    char* path_attr = NULL;
    int rc;

    if (httpk_req_of(request_url, &rq) < 0)
        return HTTPK_CK_IGNORED;
    for (k = 0; k < n; k++)
        if (((unsigned char)sc[k] < 0x20 && sc[k] != '\t') || sc[k] == 0x7f)
            return HTTPK_CK_IGNORED;
    memset(&ck, 0, sizeof ck);
    while (i < n && sc[i] != ';')
        i++;
    {
        const char* eq = (const char*)memchr(sc, '=', i);
        if (eq) {
            nb = 0;
            ne = (size_t)(eq - sc);
            vb = ne + 1;
            ve = i;
        } else {
            nb = ne = 0;
            vb = 0;
            ve = i;
        }
    }
    httpk_trim(sc, &nb, &ne);
    httpk_trim(sc, &vb, &ve);
    if (ne == nb && ve == vb)
        return HTTPK_CK_IGNORED;
    if ((ne - nb) + (ve - vb) > HTTPK_COOKIE_MAX_BYTES)
        return HTTPK_CK_IGNORED;
    for (k = nb; k < ne; k++)
        if ((unsigned char)sc[k] < 0x20 || sc[k] == 0x7f)
            return HTTPK_CK_IGNORED;
    for (k = vb; k < ve; k++)
        if ((unsigned char)sc[k] < 0x20 || sc[k] == 0x7f)
            return HTTPK_CK_IGNORED;
    while (i < n) {
        size_t ab, ae, avb, ave;
        const char* eq;
        i++;
        ab = i;
        while (i < n && sc[i] != ';')
            i++;
        ae = i;
        eq = (const char*)memchr(sc + ab, '=', ae - ab);
        if (eq) {
            avb = (size_t)(eq - sc) + 1;
            ave = ae;
            ae = (size_t)(eq - sc);
        } else {
            avb = ave = ae;
        }
        httpk_trim(sc, &ab, &ae);
        httpk_trim(sc, &avb, &ave);
        if (ave - avb > 1024)
            continue;
        if (httpk_attr_is(sc + ab, ae - ab, "expires")) {
            int64_t t = httpk_cookie_date(sc + avb, ave - avb);
            if (t >= 0) {
                expires = t;
                have_expires = 1;
            }
        } else if (httpk_attr_is(sc + ab, ae - ab, "max-age")) {
            size_t q = avb;
            int neg = 0;
            int64_t v = 0;
            if (q < ave && sc[q] == '-') {
                neg = 1;
                q++;
            }
            if (q == ave)
                continue;
            for (; q < ave; q++) {
                if (sc[q] < '0' || sc[q] > '9')
                    break;
                if (v < (int64_t)1 << 40)
                    v = v * 10 + (sc[q] - '0');
            }
            if (q != ave)
                continue;
            have_max_age = 1;
            max_age_exp = (neg || v == 0) ? INT64_MIN : now_s + v;
        } else if (httpk_attr_is(sc + ab, ae - ab, "domain")) {
            size_t db = avb;
            if (db < ave && sc[db] == '.')
                db++;
            if (db < ave) {
                free(dom_attr);
                dom_attr = httpk_strndup(sc + db, ave - db);
                if (!dom_attr)
                    goto oom;
                for (k = 0; dom_attr[k]; k++)
                    dom_attr[k] = (char)httpk_lc((unsigned char)dom_attr[k]);
            }
        } else if (httpk_attr_is(sc + ab, ae - ab, "path")) {
            free(path_attr);
            path_attr = NULL;
            if (ave > avb && sc[avb] == '/') {
                path_attr = httpk_strndup(sc + avb, ave - avb);
                if (!path_attr)
                    goto oom;
            }
        } else if (httpk_attr_is(sc + ab, ae - ab, "secure")) {
            ck.secure = 1;
        } else if (httpk_attr_is(sc + ab, ae - ab, "httponly")) {
            ck.http_only = 1;
        } else if (httpk_attr_is(sc + ab, ae - ab, "samesite")) {
            ck.same_site = httpk_attr_is(sc + avb, ave - avb, "strict") ? 3
                : httpk_attr_is(sc + avb, ave - avb, "lax")             ? 2
                : httpk_attr_is(sc + avb, ave - avb, "none")            ? 1
                                                                        : 0;
        }
    }
    if (ck.secure && !rq.https)
        goto ignore;
    if (dom_attr) {
        size_t dn = strlen(dom_attr);
        if (!httpk_domain_match(rq.host, rq.hn, dom_attr, dn))
            goto ignore;
        if (!memchr(dom_attr, '.', dn) && !(dn == rq.hn && memcmp(dom_attr, rq.host, dn) == 0))
            goto ignore;
        ck.domain = dom_attr;
        dom_attr = NULL;
    } else {
        ck.host_only = 1;
        ck.domain = httpk_strndup(rq.host, rq.hn);
        if (!ck.domain)
            goto oom;
    }
    if (path_attr) {
        ck.path = path_attr;
        path_attr = NULL;
    } else {
        ck.path = httpk_default_path(rq.path, rq.pn);
        if (!ck.path)
            goto oom;
    }
    if (ne - nb >= 9 && memcmp(sc + nb, "__Secure-", 9) == 0 && !(ck.secure && rq.https))
        goto ignore;
    if (ne - nb >= 7 && memcmp(sc + nb, "__Host-", 7) == 0
        && !(ck.secure && rq.https && ck.host_only && strcmp(ck.path, "/") == 0))
        goto ignore;
    ck.name = httpk_strndup(sc + nb, ne - nb);
    ck.value = httpk_strndup(sc + vb, ve - vb);
    if (!ck.name || !ck.value)
        goto oom;
    if (have_max_age) {
        ck.persistent = 1;
        ck.expires = max_age_exp;
    } else if (have_expires) {
        ck.persistent = 1;
        ck.expires = expires;
    }
    for (k = 0; k < j->n; k++) {
        httpk_cookie_t* o = &j->v[k];
        if (strcmp(o->name, ck.name) == 0 && strcmp(o->domain, ck.domain) == 0
            && strcmp(o->path, ck.path) == 0) {
            if (o->secure && !rq.https)
                goto ignore;
            ck.created = o->created;
            httpk_jar_del(j, k);
            break;
        }
    }
    if (ck.persistent && ck.expires <= now_s) {
        httpk_ck_free(&ck);
        return HTTPK_CK_DELETED;
    }
    if (!ck.created)
        ck.created = ++j->seq;
    if (j->n >= j->max_cookies) {
        size_t old = 0;
        for (k = 1; k < j->n; k++)
            if (j->v[k].created < j->v[old].created)
                old = k;
        httpk_jar_del(j, old);
    }
    if (j->n == j->cap) {
        size_t nc = j->cap ? j->cap * 2 : 16;
        httpk_cookie_t* nv = (httpk_cookie_t*)realloc(j->v, nc * sizeof *nv);
        if (!nv)
            goto oom;
        j->v = nv;
        j->cap = nc;
    }
    j->v[j->n++] = ck;
    return HTTPK_CK_STORED;
ignore:
    rc = HTTPK_CK_IGNORED;
    goto out;
oom:
    rc = HTTPK_CK_OOM;
out:
    free(dom_attr);
    free(path_attr);
    httpk_ck_free(&ck);
    return rc;
}

typedef struct {
    const httpk_cookie_t* c;
    size_t plen;
} httpk_ck_ref_t;

static int httpk_ck_order(const void* a, const void* b)
{
    const httpk_ck_ref_t* x = (const httpk_ck_ref_t*)a;
    const httpk_ck_ref_t* y = (const httpk_ck_ref_t*)b;
    if (x->plen != y->plen)
        return x->plen > y->plen ? -1 : 1;
    return x->c->created < y->c->created ? -1 : x->c->created > y->c->created;
}

int httpk_jar_header(httpk_jar_t* j, const char* request_url, int64_t now_s,
    dyn_sb_t* out)
{
    httpk_req_t rq;
    httpk_ck_ref_t* refs;
    size_t i, m = 0;
    if (httpk_req_of(request_url, &rq) < 0 || j->n == 0)
        return 0;
    httpk_jar_purge(j, now_s);
    if (j->n == 0)
        return 0;
    refs = (httpk_ck_ref_t*)malloc(j->n * sizeof *refs);
    if (!refs) {
        out->oom = 1;
        return -1;
    }
    for (i = 0; i < j->n; i++) {
        const httpk_cookie_t* c = &j->v[i];
        size_t dn = strlen(c->domain);
        if (c->host_only ? !(dn == rq.hn && memcmp(c->domain, rq.host, dn) == 0)
                         : !httpk_domain_match(rq.host, rq.hn, c->domain, dn))
            continue;
        if (!httpk_path_match(rq.path, rq.pn, c->path))
            continue;
        if (c->secure && !rq.https)
            continue;
        refs[m].c = c;
        refs[m].plen = strlen(c->path);
        m++;
    }
    qsort(refs, m, sizeof *refs, httpk_ck_order);
    for (i = 0; i < m; i++) {
        if (i)
            dyn_sb_put(out, "; ", 2);
        if (refs[i].c->name[0]) {
            dyn_sb_puts(out, refs[i].c->name);
            dyn_sb_putc(out, '=');
        }
        dyn_sb_puts(out, refs[i].c->value);
    }
    free(refs);
    return out->oom ? -1 : (int)m;
}
