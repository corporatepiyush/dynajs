#include "postgres-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_POSTGRES)
#include "core/dyn-pgwire.h"
#include "core/dyn-prng.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PG_URL_MAX 8192
#define PG_PGPASS_MAX (1 << 20)
#define PG_MAX_HOSTS 1024

enum { PU_USER, PU_PASS, PU_HOST, PU_PORT, PU_DB, PU_APP, PU_SSLMODE, PU_ROOT, PU_NEG, PU_CT, PU_TSA, PU_ADDR, PU_LB, PU_N };

static const char* const pg_url_keys[PU_N] = { "user", "password", "host", "port", "dbname", "application_name", "sslmode", "sslrootcert",
    "sslnegotiation", "connect_timeout", "target_session_attrs", "hostaddr", "load_balance_hosts" };

typedef struct {
    char* v[PU_N];
} pg_url_t;

static void pg_wipe(char* p, size_t n)
{
    volatile char* v = p;
    while (n--)
        *v++ = 0;
}

static void pg_url_free(pg_url_t* u)
{
    int i;
    for (i = 0; i < PU_N; i++)
        if (u->v[i]) {
            pg_wipe(u->v[i], strlen(u->v[i]));
            free(u->v[i]);
            u->v[i] = NULL;
        }
}

static char* pg_url_dec(JSContext* ctx, const char* s, size_t n)
{
    char* o = (char*)malloc(n + 1);
    long r;
    if (!o) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    r = dyn_pgw_uri_decode(s, n, o);
    if (r < 0) {
        pg_wipe(o, n);
        free(o);
        JS_ThrowTypeError(ctx, "PostgreSQL: url has %s",
            r == DYN_PGW_DEC_HEX ? "a bad percent-encoding" : r == DYN_PGW_DEC_NUL ? "a percent-encoded NUL" : "an unencoded space");
        return NULL;
    }
    o[r] = 0;
    return o;
}

static int pg_url_put(JSContext* ctx, pg_url_t* u, int k, const char* s, size_t n)
{
    char* o = pg_url_dec(ctx, s, n);
    if (!o)
        return -1;
    if (u->v[k]) {
        pg_wipe(u->v[k], strlen(u->v[k]));
        free(u->v[k]);
    }
    u->v[k] = o;
    return 0;
}

static int pg_url_param(JSContext* ctx, pg_url_t* u, const char* s, size_t kp, size_t kn, size_t vp, size_t vn)
{
    char* key = pg_url_dec(ctx, s + kp, kn);
    int i;
    if (!key)
        return -1;
    for (i = 0; i < PU_N; i++)
        if (!strcmp(key, pg_url_keys[i]))
            break;
    if (i == PU_N && !strcmp(key, "ssl")) {
        char* v = pg_url_dec(ctx, s + vp, vn);
        int ok = v && !strcmp(v, "true");
        free(key);
        if (!v)
            return -1;
        free(v);
        if (!ok) {
            JS_ThrowTypeError(ctx, "PostgreSQL: url parameter ssl only takes true");
            return -1;
        }
        return pg_url_put(ctx, u, PU_SSLMODE, "require", 7);
    }
    if (i == PU_N) {
        JS_ThrowTypeError(ctx, "PostgreSQL: url parameter '%.64s' is not supported", key);
        free(key);
        return -1;
    }
    free(key);
    return pg_url_put(ctx, u, i, s + vp, vn);
}

static int pg_url_parse(JSContext* ctx, const char* s, size_t n, pg_url_t* u)
{
    dyn_pgw_uri_t x;
    size_t at, h, hn, p, pn, hl = 0, pl = 0;
    char *hb, *pb;
    int rc = dyn_pgw_uri_split(s, n, &x), hp, k, first = 1;
    if (rc == DYN_PGW_URI_SCHEME) {
        JS_ThrowTypeError(ctx, "PostgreSQL: url must start with postgres:// or postgresql://");
        return -1;
    }
    if (x.has_user && pg_url_put(ctx, u, PU_USER, s + x.user, x.user_n) < 0)
        return -1;
    if (x.has_pass && pg_url_put(ctx, u, PU_PASS, s + x.pass, x.pass_n) < 0)
        return -1;
    if (rc) {
        JS_ThrowTypeError(ctx, "PostgreSQL: url has a malformed IPv6 host");
        return -1;
    }
    hb = (char*)malloc(x.net_end - x.net + 1);
    pb = hb ? (char*)malloc(x.net_end - x.net + 1) : NULL;
    if (!pb) {
        free(hb);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    at = x.net;
    while (dyn_pgw_uri_host(s, x.net_end, &at, &h, &hn, &p, &pn, &hp) > 0) {
        if (!first) {
            hb[hl++] = ',';
            pb[pl++] = ',';
        }
        first = 0;
        memcpy(hb + hl, s + h, hn);
        hl += hn;
        if (hp) {
            memcpy(pb + pl, s + p, pn);
            pl += pn;
        }
    }
    rc = (hl && pg_url_put(ctx, u, PU_HOST, hb, hl) < 0) || (pl && pg_url_put(ctx, u, PU_PORT, pb, pl) < 0);
    free(hb);
    free(pb);
    if (rc)
        return -1;
    if (x.has_db && pg_url_put(ctx, u, PU_DB, s + x.db, x.db_n) < 0)
        return -1;
    if (!x.has_q)
        return 0;
    at = x.q;
    while ((k = dyn_pgw_uri_param(s, x.q + x.q_n, &at, &h, &hn, &p, &pn)) > 0)
        if (pg_url_param(ctx, u, s, h, hn, p, pn) < 0)
            return -1;
    if (k < 0) {
        JS_ThrowTypeError(ctx, "PostgreSQL: url parameter %s", k == -1 ? "without '='" : "with a second '='");
        return -1;
    }
    return 0;
}

static int pg_take(JSContext* ctx, char** dst, pg_url_t* u, int k, const char* env)
{
    const char* e;
    if (*dst)
        return 0;
    if (u->v[k] && u->v[k][0]) {
        *dst = u->v[k];
        u->v[k] = NULL;
        return 0;
    }
    if (!env || !(e = getenv(env)) || !e[0])
        return 0;
    *dst = strdup(e);
    if (!*dst) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static int pg_port_of(JSContext* ctx, const char* s, size_t n, const char* what, uint16_t* out)
{
    size_t i = 0, d;
    long long v = 0;
    int neg = 0;
    if (!n) {
        *out = 5432;
        return 0;
    }
    while (i < n && isspace((unsigned char)s[i]))
        i++;
    if (i < n && (s[i] == '+' || s[i] == '-'))
        neg = s[i++] == '-';
    for (d = i; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        if (v <= 2147483648LL)
            v = v * 10 + (s[i] - '0');
    if (i == d || v > (neg ? 2147483648LL : 2147483647LL))
        goto badint;
    while (i < n && isspace((unsigned char)s[i]))
        i++;
    if (i < n)
        goto badint;
    if (neg || v < 1 || v > 65535) {
        JS_ThrowRangeError(ctx, "PostgreSQL: invalid port number: \"%.*s\" (%s)", (int)n, s, what);
        return -1;
    }
    *out = (uint16_t)v;
    return 0;
badint:
    JS_ThrowRangeError(ctx, "PostgreSQL: invalid integer value \"%.*s\" for connection option \"port\" (%s)", (int)n, s, what);
    return -1;
}

static int pg_count(const char* s)
{
    int n = 1;
    for (; *s; s++)
        if (*s == ',' && n <= PG_MAX_HOSTS)
            n++;
    return n;
}

static size_t pg_elen(const char* s)
{
    size_t n = 0;
    while (s[n] && s[n] != ',')
        n++;
    return n;
}

static char* pg_dupn(const char* s, size_t n)
{
    char* d = (char*)malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

void pg_hosts_free(dyn_pg_t* g)
{
    int i;
    for (i = 0; g->hosts && i < g->nh; i++) {
        free(g->hosts[i].host);
        free(g->hosts[i].addr);
    }
    free(g->hosts);
    g->hosts = NULL;
    g->nh = 0;
}

int pg_host_load(dyn_pg_t* g, int i)
{
    struct pg_hent* e = &g->hosts[i];
    free(g->host);
    free(g->path);
    g->host = g->path = NULL;
    g->hi = i;
    g->port = e->port;
    g->addr_cur = e->addr;
    if (!e->addr && e->host && e->host[0] == '/') {
        size_t hl = strlen(e->host);
        g->path = (char*)malloc(hl + 24);
        if (!g->path)
            return -1;
        snprintf(g->path, hl + 24, "%s/.s.PGSQL.%u", e->host, (unsigned)e->port);
        return 0;
    }
    if (e->host && !(g->host = strdup(e->host)))
        return -1;
    return 0;
}

int pg_pgpass_read(const char* file, const char* host, const char* port, const char* db, const char* user, char** out)
{
    struct stat st;
    char *buf, *pw;
    size_t n = 0, cap;
    long k;
    int fd = open(file, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    *out = NULL;
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & (S_IRWXG | S_IRWXO)) || st.st_size > PG_PGPASS_MAX) {
        close(fd);
        return 0;
    }
    cap = (size_t)st.st_size;
    buf = (char*)malloc(cap + 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    while (n < cap) {
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    if (memchr(buf, 0, n)) {
        pg_wipe(buf, n);
        free(buf);
        return 0;
    }
    pw = (char*)malloc(n + 1);
    if (!pw) {
        pg_wipe(buf, n);
        free(buf);
        return -1;
    }
    k = dyn_pgw_pgpass_find(buf, n, host, port, db, user, pw);
    pg_wipe(buf, n);
    free(buf);
    if (k < 0) {
        free(pw);
        return 0;
    }
    pw[k] = 0;
    *out = pw;
    return 1;
}

struct pg_pp_job {
    dyn_pg_t* g;
    JSRuntime* rt;
    struct pg_pp_job* next;
    struct pg_pp_job* prev;
    char* pw;
    const char *path, *host, *port, *db, *user;
    size_t size;
    int rc, started;
    char s[];
};

static _Thread_local struct pg_pp_job* pg_pp_jobs;

static void pg_pp_free(struct pg_pp_job* j)
{
    if (j->next)
        j->next->prev = j->prev;
    if (j->prev)
        j->prev->next = j->next;
    else if (pg_pp_jobs == j)
        pg_pp_jobs = j->next;
    if (j->pw) {
        pg_wipe(j->pw, strlen(j->pw));
        free(j->pw);
    }
    pg_wipe((char*)j, j->size);
    free(j);
}

static void pg_pp_work(void* a)
{
    struct pg_pp_job* j = (struct pg_pp_job*)a;
    j->rc = pg_pgpass_read(j->path, j->host, j->port, j->db, j->user, &j->pw);
}

static void pg_pp_done(void* a)
{
    struct pg_pp_job* j = (struct pg_pp_job*)a;
    dyn_pg_t* g = j->g;
    JSRuntime* rt = j->rt;
    int rc = j->rc;
    if (g) {
        g->pgpass_job = NULL;
        if (rc > 0 && !g->pass) {
            g->pass = j->pw;
            j->pw = NULL;
            g->pass_pp = 1;
        }
    }
    pg_pp_free(j);
    if (g)
        pg_after_pgpass(g, rc);
    dyn_net_reactor_release_rt(rt);
}

static void pg_pp_reap(void)
{
    while (pg_pp_jobs) {
        struct pg_pp_job* j = pg_pp_jobs;
        if (j->g)
            j->g->pgpass_job = NULL;
        pg_pp_free(j);
    }
}

void pg_pgpass_job_orphan(dyn_pg_t* g)
{
    struct pg_pp_job* j = g->pgpass_job;
    if (!j)
        return;
    g->pgpass_job = NULL;
    if (j->started)
        j->g = NULL;
    else
        pg_pp_free(j);
}

void pg_pgpass_start(dyn_pg_t* g)
{
    struct pg_pp_job* j = g->pgpass_job;
    j->started = 1;
    j->rt = g->rt;
    dyn_net_reactor_hold();
    j->next = pg_pp_jobs;
    if (pg_pp_jobs)
        pg_pp_jobs->prev = j;
    pg_pp_jobs = j;
    if (dyn_net_on_reactor_free(pg_pp_reap) != 0 || dyn_aio_offload(g->aio, pg_pp_work, pg_pp_done, j) < 0) {
        j->rc = -1;
        pg_pp_done(j);
    }
}

static const char* pg_pp_put(char** p, const char* a, size_t na, const char* b, size_t nb)
{
    char* d = *p;
    memcpy(d, a, na);
    memcpy(d + na, b, nb);
    d[na + nb] = 0;
    *p = d + na + nb + 1;
    return d;
}

int pg_pgpass_prep(dyn_pg_t* g)
{
    const char *user = g->user ? g->user : "postgres", *db = g->database ? g->database : user;
    const char* host = g->path ? "localhost" : g->host ? g->host : g->addr_cur ? g->addr_cur : "localhost";
    struct pg_pp_job* j;
    char portb[8], *p;
    size_t lf, lh, lp, ld, lu, sz;
    if (g->pass_pp && g->pass) {
        pg_wipe(g->pass, strlen(g->pass));
        free(g->pass);
        g->pass = NULL;
    }
    g->pass_pp = 0;
    if (!g->pp_file || g->pass)
        return 0;
    snprintf(portb, sizeof portb, "%u", (unsigned)g->port);
    lf = strlen(g->pp_file);
    lh = strlen(host);
    lp = strlen(portb);
    ld = strlen(db);
    lu = strlen(user);
    sz = sizeof *j + lf + lh + lp + ld + lu + 5;
    j = (struct pg_pp_job*)calloc(1, sz);
    if (!j)
        return -1;
    j->size = sz;
    p = j->s;
    j->path = pg_pp_put(&p, g->pp_file, lf, "", 0);
    j->host = pg_pp_put(&p, host, lh, "", 0);
    j->port = pg_pp_put(&p, portb, lp, "", 0);
    j->db = pg_pp_put(&p, db, ld, "", 0);
    j->user = pg_pp_put(&p, user, lu, "", 0);
    j->g = g;
    g->pgpass_job = j;
    return 1;
}

static int pg_pgpass(JSContext* ctx, dyn_pg_t* g)
{
    const char *f = getenv("PGPASSFILE"), *home = getenv("HOME");
    if (!f || !f[0]) {
        size_t n;
        if (!home || !home[0])
            return 0;
        n = strlen(home);
        g->pp_file = (char*)malloc(n + 9);
        if (g->pp_file) {
            memcpy(g->pp_file, home, n);
            memcpy(g->pp_file + n, "/.pgpass", 9);
        }
    } else {
        g->pp_file = strdup(f);
    }
    if (!g->pp_file || pg_pgpass_prep(g) < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static int pg_url_tls(JSContext* ctx, dyn_pg_t* g, pg_url_t* u, const pg_ci_t* ci)
{
    const char* m = u->v[PU_SSLMODE];
    int want = -1;
    if (m) {
        if (!strcmp(m, "disable")) {
            want = 0;
        } else if (!strcmp(m, "require") || !strcmp(m, "verify-ca") || !strcmp(m, "verify-full")) {
            want = 1;
        } else if (!strcmp(m, "allow") || !strcmp(m, "prefer")) {
            JS_ThrowRangeError(ctx, "PostgreSQL: sslmode %s is refused: it lets a network attacker downgrade the connection to plaintext; use require, verify-full or disable", m);
            return -1;
        } else {
            JS_ThrowTypeError(ctx, "PostgreSQL: sslmode must be disable, require, verify-ca or verify-full");
            return -1;
        }
    }
    if (ci->tls < 0 && want == 0)
        g->use_tls = 0;
    if (ci->tls < 0 && want == 1) {
#ifndef CONFIG_TLS
        (void)dyn_tls_missing(ctx, "PostgreSQL", "tls");
        return -1;
#else
        g->use_tls = 1;
#endif
    }
#ifdef CONFIG_TLS
    if (g->use_tls && !g->tls_ca && u->v[PU_ROOT] && u->v[PU_ROOT][0] && strcmp(u->v[PU_ROOT], "system")) {
        g->tls_ca = u->v[PU_ROOT];
        u->v[PU_ROOT] = NULL;
    }
#endif
    if (g->ssl_direct < 0) {
        const char* v = u->v[PU_NEG];
        g->ssl_direct = v && !strcmp(v, "direct");
        if (v && v[0] && !g->ssl_direct && strcmp(v, "postgres")) {
            JS_ThrowTypeError(ctx, "PostgreSQL: sslnegotiation must be postgres or direct");
            return -1;
        }
    }
    if (g->ssl_direct && !g->use_tls) {
        JS_ThrowTypeError(ctx, "PostgreSQL: sslNegotiation 'direct' needs tls: true");
        return -1;
    }
    return 0;
}

static int pg_url_timeout(JSContext* ctx, dyn_pg_t* g, pg_url_t* u, const pg_ci_t* ci)
{
    const char* v = u->v[PU_CT];
    long t = PG_CONNECT_TIMEOUT;
    if (v && v[0]) {
        char* e;
        long sec = strtol(v, &e, 10);
        if (*e || e == v) {
            JS_ThrowTypeError(ctx, "PostgreSQL: connect_timeout must be an integer number of seconds");
            return -1;
        }
        t = sec <= 0 ? 0 : sec == 1 ? 2000 : sec > 86400 ? 86400000L : sec * 1000;
    }
    if (ci->ct >= 0)
        return 0;
    g->connect_ms = (uint64_t)t;
    g->connect_deadline_ms = t ? dyn_timer_now_ms() + (uint64_t)t : 0;
    return 0;
}

static int pg_url_tsa(JSContext* ctx, dyn_pg_t* g, pg_url_t* u, int use_env)
{
    static const char* const tv[] = { "any", "read-write", "read-only", "primary", "standby", "prefer-standby" };
    int i;
    if (pg_take(ctx, &g->tsa_s, u, PU_TSA, use_env ? "PGTARGETSESSIONATTRS" : NULL) < 0 || pg_take(ctx, &g->lb_s, u, PU_LB, use_env ? "PGLOADBALANCEHOSTS" : NULL) < 0)
        return -1;
    g->tsa = PG_TSA_ANY;
    if (g->tsa_s && g->tsa_s[0]) {
        for (i = 0; i < 6 && strcmp(g->tsa_s, tv[i]); i++)
            ;
        if (i == 6) {
            JS_ThrowRangeError(ctx, "PostgreSQL: invalid target_session_attrs value: \"%.64s\"", g->tsa_s);
            return -1;
        }
        g->tsa = (uint8_t)i;
    }
    g->lb = 0;
    if (g->lb_s && g->lb_s[0] && strcmp(g->lb_s, "disable")) {
        if (strcmp(g->lb_s, "random")) {
            JS_ThrowRangeError(ctx, "PostgreSQL: invalid load_balance_hosts value: \"%.64s\"", g->lb_s);
            return -1;
        }
        g->lb = 1;
    }
    return 0;
}

static int pg_url_hosts(JSContext* ctx, dyn_pg_t* g, pg_url_t* u, int use_env, const pg_ci_t* ci)
{
    const char *ps = NULL, *what = "url", *h, *a, *p;
    int nh = 1, np = 0, i;
    if (!g->path && (pg_take(ctx, &g->host, u, PU_HOST, use_env ? "PGHOST" : NULL) < 0 || pg_take(ctx, &g->hostaddr, u, PU_ADDR, use_env ? "PGHOSTADDR" : NULL) < 0))
        return -1;
    if (ci->port < 0) {
        ps = u->v[PU_PORT];
        if (!ps || !ps[0]) {
            ps = use_env ? getenv("PGPORT") : NULL;
            what = "PGPORT";
        }
        if (ps && !ps[0])
            ps = NULL;
    }
    if (g->path) {
        if (ps && pg_count(ps) != 1) {
            JS_ThrowRangeError(ctx, "PostgreSQL: could not match %d port numbers to 1 hosts", pg_count(ps));
            return -1;
        }
        return ps ? pg_port_of(ctx, ps, strlen(ps), what, &g->port) : 0;
    }
    if (g->hostaddr && g->hostaddr[0])
        nh = pg_count(g->hostaddr);
    else if (g->host && g->host[0])
        nh = pg_count(g->host);
    if (nh > PG_MAX_HOSTS) {
        JS_ThrowRangeError(ctx, "PostgreSQL: at most %d hosts are supported", PG_MAX_HOSTS);
        return -1;
    }
    if (g->hostaddr && g->hostaddr[0] && g->host && g->host[0] && pg_count(g->host) != nh) {
        JS_ThrowRangeError(ctx, "PostgreSQL: could not match %d host names to %d hostaddr values", pg_count(g->host), nh);
        return -1;
    }
    if (ps && (np = pg_count(ps)) != 1 && np != nh) {
        JS_ThrowRangeError(ctx, "PostgreSQL: could not match %d port numbers to %d hosts", np, nh);
        return -1;
    }
    g->hosts = (struct pg_hent*)calloc((size_t)nh, sizeof *g->hosts);
    if (!g->hosts) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    g->nh = nh;
    h = g->host && g->host[0] ? g->host : NULL;
    a = g->hostaddr && g->hostaddr[0] ? g->hostaddr : NULL;
    p = ps;
    for (i = 0; i < nh; i++) {
        struct pg_hent* e = &g->hosts[i];
        size_t n;
        if (h) {
            n = pg_elen(h);
            if (n && !(e->host = pg_dupn(h, n)))
                goto oom;
            h += n + (h[n] == ',');
        }
        if (a) {
            n = pg_elen(a);
            if (n && !(e->addr = pg_dupn(a, n)))
                goto oom;
            a += n + (a[n] == ',');
        }
        if (ci->port >= 0) {
            e->port = (uint16_t)ci->port;
        } else if (p) {
            n = pg_elen(p);
            if (pg_port_of(ctx, p, n, what, &e->port) < 0)
                return -1;
            if (np > 1)
                p += n + (p[n] == ',');
        } else {
            e->port = 5432;
        }
    }
    if (g->lb && g->nh > 1) {
        dyn_prng_t r;
        if (dyn_prng_seed_random(&r) < 0)
            dyn_prng_seed(&r, (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)g);
        for (i = 1; i < g->nh; i++) {
            int j = (int)dyn_prng_next_bounded(&r, (uint64_t)i + 1);
            struct pg_hent t = g->hosts[j];
            g->hosts[j] = g->hosts[i];
            g->hosts[i] = t;
        }
    }
    if (pg_host_load(g, 0) < 0)
        goto oom;
    return 0;
oom:
    JS_ThrowOutOfMemory(ctx);
    return -1;
}

int pg_conninfo(JSContext* ctx, dyn_pg_t* g, JSValueConst url, int use_env, const pg_ci_t* ci)
{
    pg_url_t u;
    const char* s = NULL;
    size_t n = 0;
    int rc = -1;
    memset(&u, 0, sizeof(u));
    if (!JS_IsUndefined(url) && !JS_IsNull(url)) {
        if (!JS_IsString(url) && !JS_IsObject(url)) {
            JS_ThrowTypeError(ctx, "PostgreSQL: url must be a string or URL");
            return -1;
        }
        s = JS_ToCStringLen(ctx, &n, url);
        if (!s)
            return -1;
        if (n > PG_URL_MAX) {
            JS_ThrowRangeError(ctx, "PostgreSQL: url is longer than %d bytes", PG_URL_MAX);
            goto out;
        }
        if (memchr(s, 0, n)) {
            JS_ThrowTypeError(ctx, "PostgreSQL: url contains a NUL byte");
            goto out;
        }
        if (pg_url_parse(ctx, s, n, &u) < 0)
            goto out;
    }
    if (pg_take(ctx, &g->user, &u, PU_USER, use_env ? "PGUSER" : NULL) < 0 || pg_take(ctx, &g->database, &u, PU_DB, use_env ? "PGDATABASE" : NULL) < 0
        || pg_take(ctx, &g->appname, &u, PU_APP, NULL) < 0 || pg_take(ctx, &g->pass, &u, PU_PASS, use_env ? "PGPASSWORD" : NULL) < 0
        || pg_url_tsa(ctx, g, &u, use_env) < 0 || pg_url_hosts(ctx, g, &u, use_env, ci) < 0 || pg_url_tls(ctx, g, &u, ci) < 0 || pg_url_timeout(ctx, g, &u, ci) < 0)
        goto out;
    if (use_env && !g->pass && pg_pgpass(ctx, g) < 0)
        goto out;
#ifdef CONFIG_TLS
    for (int i = 0; g->use_tls && i < g->nh; i++)
        if (g->hosts[i].addr && !g->hosts[i].host) {
            JS_ThrowRangeError(ctx, "PostgreSQL: tls needs a host name to verify; give host alongside hostaddr");
            goto out;
        }
#endif
    rc = 0;
out:
    if (s)
        JS_FreeCString(ctx, s);
    pg_url_free(&u);
    return rc;
}

#endif
