#include "rtc-sdp.h"
#include "rtc-ice.h"

#include <stddef.h>
#include <stdlib.h>

void rtc_sb_init(rtc_sb* b)
{
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
    b->err = 0;
}

void rtc_sb_free(rtc_sb* b)
{
    free(b->buf);
    rtc_sb_init(b);
}

void rtc_sb_put(rtc_sb* b, const char* s, size_t n)
{
    if (b->err)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 512;
        char* nb;
        while (nc < b->len + n + 1)
            nc *= 2;
        nb = (char*)realloc(b->buf, nc);
        if (!nb) {
            b->err = 1;
            return;
        }
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

void rtc_sb_str(rtc_sb* b, const char* s)
{
    rtc_sb_put(b, s, strlen(s));
}

void rtc_sb_u64(rtc_sb* b, uint64_t v)
{
    char t[24];
    int i = 24;
    do {
        t[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    rtc_sb_put(b, t + i, (size_t)(24 - i));
}

void rtc_sb_line(rtc_sb* b, const char* a, const char* c)
{
    rtc_sb_str(b, a);
    if (c)
        rtc_sb_str(b, c);
    rtc_sb_put(b, "\r\n", 2);
}

static const char* const rtc_fp_names[] = { "", "sha-1", "sha-224", "sha-256", "sha-384", "sha-512" };
static const uint8_t rtc_fp_lens[] = { 0, 20, 28, 32, 48, 64 };

int rtc_sdp_fp_alg(const char* s, size_t n)
{
    int i;
    for (i = 1; i < 6; i++) {
        size_t k = strlen(rtc_fp_names[i]), j;
        if (k != n)
            continue;
        for (j = 0; j < n; j++) {
            char c = s[j];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
            if (c != rtc_fp_names[i][j])
                break;
        }
        if (j == n)
            return i;
    }
    return RTC_FP_NONE;
}

const char* rtc_sdp_fp_name(int alg)
{
    return alg > 0 && alg < 6 ? rtc_fp_names[alg] : "";
}

size_t rtc_sdp_fp_len(int alg)
{
    return alg > 0 && alg < 6 ? rtc_fp_lens[alg] : 0;
}

static int rtc_tok(const char** p, const char* e, const char** ts, size_t* tl)
{
    const char* q = *p;
    while (q < e && (*q == ' ' || *q == '\t'))
        q++;
    if (q >= e)
        return 0;
    *ts = q;
    while (q < e && *q != ' ' && *q != '\t')
        q++;
    *tl = (size_t)(q - *ts);
    *p = q;
    return 1;
}

static int rtc_u64(const char* s, size_t n, uint64_t* out)
{
    uint64_t v = 0;
    size_t i;
    if (n == 0 || n > 20)
        return -1;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        if (v > (UINT64_MAX - 9) / 10)
            return -1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static int rtc_eq(const char* s, size_t n, const char* lit)
{
    return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

static int rtc_ice_chars(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/'))
            return 0;
    }
    return n > 0;
}

static int rtc_host_chars(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.'))
            return 0;
    }
    return n > 0;
}

static void rtc_cpyn(char* dst, size_t cap, const char* s, size_t n)
{
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

int rtc_sdp_parse_candidate(const char* s, size_t n, rtc_sdp_cand* c)
{
    const char *p = s, *e = s + n, *t;
    size_t tl;
    uint64_t v;
    const char* addr_s;
    size_t addr_l;
    memset(c, 0, sizeof(*c));
    if (n >= 2 && s[0] == 'a' && s[1] == '=') {
        p += 2;
    }
    if ((size_t)(e - p) >= 10 && !memcmp(p, "candidate:", 10))
        p += 10;
    if (!rtc_tok(&p, e, &t, &tl) || tl > 32 || !rtc_ice_chars(t, tl))
        return -1;
    rtc_cpyn(c->foundation, sizeof(c->foundation), t, tl);
    if (!rtc_tok(&p, e, &t, &tl) || rtc_u64(t, tl, &v) || v < 1 || v > 256)
        return -1;
    c->comp = (uint32_t)v;
    if (!rtc_tok(&p, e, &t, &tl))
        return -1;
    c->udp = (tl == 3 && (t[0] | 32) == 'u' && (t[1] | 32) == 'd' && (t[2] | 32) == 'p');
    if (!c->udp && !(tl == 3 && (t[0] | 32) == 't' && (t[1] | 32) == 'c' && (t[2] | 32) == 'p'))
        return -1;
    if (!rtc_tok(&p, e, &t, &tl) || rtc_u64(t, tl, &v) || v > 0xFFFFFFFFu)
        return -1;
    c->prio = (uint32_t)v;
    if (!rtc_tok(&p, e, &addr_s, &addr_l))
        return -1;
    if (!rtc_tok(&p, e, &t, &tl) || rtc_u64(t, tl, &v) || v > 65535)
        return -1;
    if (rtc_addr_parse(addr_s, addr_l, (uint16_t)v, &c->addr) < 0) {
        if (addr_l >= sizeof(c->host) || !rtc_host_chars(addr_s, addr_l))
            return -1;
        memset(&c->addr, 0, sizeof(c->addr));
        c->addr.port = (uint16_t)v;
        rtc_cpyn(c->host, sizeof(c->host), addr_s, addr_l);
    }
    if (!rtc_tok(&p, e, &t, &tl) || !rtc_eq(t, tl, "typ"))
        return -1;
    if (!rtc_tok(&p, e, &t, &tl) || tl >= sizeof(c->typ))
        return -1;
    rtc_cpyn(c->typ, sizeof(c->typ), t, tl);
    if (!strcmp(c->typ, "host"))
        c->type = RTC_CAND_HOST;
    else if (!strcmp(c->typ, "srflx"))
        c->type = RTC_CAND_SRFLX;
    else if (!strcmp(c->typ, "prflx"))
        c->type = RTC_CAND_PRFLX;
    else if (!strcmp(c->typ, "relay"))
        c->type = RTC_CAND_RELAY;
    else
        return -1;
    while (rtc_tok(&p, e, &t, &tl)) {
        const char* vs;
        size_t vl;
        if (!rtc_tok(&p, e, &vs, &vl))
            break;
        if (rtc_eq(t, tl, "raddr")) {
            if (rtc_addr_parse(vs, vl, 0, &c->rel) == 0)
                c->has_rel = 1;
        } else if (rtc_eq(t, tl, "rport")) {
            if (rtc_u64(vs, vl, &v) == 0 && v <= 65535)
                c->rel.port = (uint16_t)v;
        } else if (rtc_eq(t, tl, "tcptype"))
            rtc_cpyn(c->tcptype, sizeof(c->tcptype), vs, vl);
    }
    return 0;
}

size_t rtc_sdp_write_candidate(const rtc_sdp_cand* c, char* out, size_t cap)
{
    rtc_sb b;
    char ip[64];
    size_t n;
    rtc_sb_init(&b);
    rtc_sb_str(&b, "candidate:");
    rtc_sb_str(&b, c->foundation);
    rtc_sb_put(&b, " ", 1);
    rtc_sb_u64(&b, c->comp);
    rtc_sb_str(&b, c->udp ? " udp " : " tcp ");
    rtc_sb_u64(&b, c->prio);
    rtc_sb_put(&b, " ", 1);
    if (!c->addr.fam && c->host[0])
        rtc_sb_str(&b, c->host);
    else {
        rtc_addr_fmt(&c->addr, ip, sizeof(ip));
        rtc_sb_str(&b, ip);
    }
    rtc_sb_put(&b, " ", 1);
    rtc_sb_u64(&b, c->addr.port);
    rtc_sb_str(&b, " typ ");
    rtc_sb_str(&b, c->typ);
    if (c->has_rel) {
        rtc_sb_str(&b, " raddr ");
        rtc_addr_fmt(&c->rel, ip, sizeof(ip));
        rtc_sb_str(&b, ip);
        rtc_sb_str(&b, " rport ");
        rtc_sb_u64(&b, c->rel.port);
    }
    rtc_sb_str(&b, " generation 0");
    n = b.err ? 0 : b.len;
    if (n >= cap)
        n = 0;
    if (n)
        memcpy(out, b.buf, n + 1);
    else if (cap)
        out[0] = 0;
    rtc_sb_free(&b);
    return n;
}

static const char rtc_empty[] = "";

static void* rtc_arena_alloc(rtc_arena* a, size_t n)
{
    uintptr_t at;
    size_t pad;
    if (n > ((size_t)-1) / 4)
        return NULL;
    if (a->n) {
        at = (uintptr_t)(a->chunk[a->n - 1] + a->used);
        pad = (size_t)(-at & 15u);
        if (a->used + pad + n <= a->last) {
            a->used += pad + n;
            return a->chunk[a->n - 1] + a->used - n;
        }
    }
    {
        size_t cap = a->hint > 1024 ? a->hint : 1024;
        unsigned char* nc;
        if (a->n)
            cap = cap / 4 > 1024 ? cap / 4 : 1024;
        if (cap < n + 16)
            cap = n + 16;
        if (a->n == a->cap) {
            int ncap = a->cap ? a->cap * 2 : 4;
            unsigned char** np = (unsigned char**)realloc(a->chunk, sizeof(*np) * (size_t)ncap);
            if (!np)
                return NULL;
            a->bytes += sizeof(*np) * (size_t)(ncap - a->cap);
            a->chunk = np;
            a->cap = ncap;
        }
        nc = (unsigned char*)malloc(cap);
        if (!nc)
            return NULL;
        a->chunk[a->n++] = nc;
        a->last = cap;
        a->bytes += cap;
        at = (uintptr_t)nc;
        pad = (size_t)(-at & 15u);
        a->used = pad + n;
        return nc + pad;
    }
}

static void rtc_media_init(rtc_sdp_media* m)
{
    memset(m, 0, sizeof(*m));
    m->kind = m->proto = m->fmt_text = m->conn = m->mid = m->ufrag = m->pwd = m->ice_opts = rtc_empty;
}

void rtc_sdp_limits_default(rtc_sdp_limits* l)
{
    l->max_input = RTC_SDP_MAX_INPUT;
    l->line_max = RTC_SDP_LINE_MAX;
    l->media = RTC_SDP_MAX_MEDIA;
    l->codecs = RTC_SDP_MAX_CODECS;
    l->ext = RTC_SDP_MAX_EXT;
    l->cands = RTC_SDP_MAX_CANDS;
    l->ssrc = RTC_SDP_MAX_SSRC;
    l->msid = RTC_SDP_MAX_MSID;
    l->groups = RTC_SDP_MAX_GROUPS;
    l->group_ssrcs = RTC_SDP_GROUP_SSRCS;
    l->extra = RTC_SDP_MAX_EXTRA;
}

void rtc_sdp_init(rtc_sdp* sdp)
{
    memset(sdp, 0, sizeof(*sdp));
    rtc_sdp_limits_default(&sdp->lim);
    sdp->origin = sdp->name = sdp->timing = sdp->conn = sdp->ice_opts = sdp->ufrag = sdp->pwd = rtc_empty;
}

void rtc_sdp_free(rtc_sdp* sdp)
{
    int i;
    for (i = 0; i < sdp->arena.n; i++)
        free(sdp->arena.chunk[i]);
    free(sdp->arena.chunk);
    {
        rtc_sdp_limits l = sdp->lim;
        rtc_sdp_init(sdp);
        sdp->lim = l;
    }
}

void rtc_sdp_reset(rtc_sdp* sdp, size_t hint)
{
    rtc_sdp_free(sdp);
    sdp->arena.hint = hint;
}

void* rtc_sdp_alloc(rtc_sdp* sdp, size_t n)
{
    return rtc_arena_alloc(&sdp->arena, n ? n : 1);
}

size_t rtc_sdp_bytes(const rtc_sdp* sdp)
{
    return sdp->arena.bytes;
}

const char* rtc_sdp_strdup(rtc_sdp* sdp, const char* s, size_t n)
{
    char* d;
    if (!n)
        return rtc_empty;
    d = (char*)rtc_arena_alloc(&sdp->arena, n + 1);
    if (!d)
        return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static const char* rtc_dup(rtc_sdp* sdp, const char* s, size_t n, size_t max)
{
    return n > max ? NULL : rtc_sdp_strdup(sdp, s, n);
}

static void* rtc_grow(rtc_sdp* sdp, void* base, int n, int* cap, size_t elem, int max)
{
    void* nb;
    int nc;
    if (n >= max)
        return NULL;
    if (n < *cap)
        return base;
    nc = *cap ? *cap * 2 : 4;
    if (nc > max)
        nc = max;
    nb = rtc_arena_alloc(&sdp->arena, (size_t)nc * elem);
    if (!nb)
        return NULL;
    if (n)
        memcpy(nb, base, (size_t)n * elem);
    *cap = nc;
    return nb;
}

rtc_sdp_media* rtc_sdp_add_media(rtc_sdp* sdp)
{
    rtc_sdp_media* a = (rtc_sdp_media*)rtc_grow(sdp, sdp->media, sdp->nmedia, &sdp->cap_media, sizeof(rtc_sdp_media), sdp->lim.media);
    if (!a)
        return NULL;
    sdp->media = a;
    rtc_media_init(&a[sdp->nmedia]);
    return &a[sdp->nmedia++];
}

rtc_sdp_codec* rtc_sdp_add_codec(rtc_sdp* sdp, rtc_sdp_media* m)
{
    rtc_sdp_codec* a = (rtc_sdp_codec*)rtc_grow(sdp, m->codecs, m->ncodecs, &m->cap_codecs, sizeof(rtc_sdp_codec), sdp->lim.codecs);
    if (!a)
        return NULL;
    m->codecs = a;
    memset(&a[m->ncodecs], 0, sizeof(*a));
    a[m->ncodecs].name = a[m->ncodecs].fmtp = rtc_empty;
    return &a[m->ncodecs++];
}

rtc_sdp_ext* rtc_sdp_add_ext(rtc_sdp* sdp, rtc_sdp_media* m)
{
    rtc_sdp_ext* a = (rtc_sdp_ext*)rtc_grow(sdp, m->ext, m->next, &m->cap_ext, sizeof(rtc_sdp_ext), sdp->lim.ext);
    if (!a)
        return NULL;
    m->ext = a;
    memset(&a[m->next], 0, sizeof(*a));
    a[m->next].uri = a[m->next].attrs = rtc_empty;
    return &a[m->next++];
}

rtc_sdp_msid* rtc_sdp_add_msid(rtc_sdp* sdp, rtc_sdp_media* m)
{
    rtc_sdp_msid* a = (rtc_sdp_msid*)rtc_grow(sdp, m->msid, m->nmsid, &m->cap_msid, sizeof(rtc_sdp_msid), sdp->lim.msid);
    if (!a)
        return NULL;
    m->msid = a;
    a[m->nmsid].stream = a[m->nmsid].track = rtc_empty;
    return &a[m->nmsid++];
}

int rtc_sdp_add_bundle(rtc_sdp* sdp, const char* mid)
{
    const char** a = (const char**)rtc_grow(sdp, (void*)sdp->bundle, sdp->nbundle, &sdp->cap_bundle, sizeof(char*), sdp->lim.media);
    const char* d = a ? rtc_dup(sdp, mid, strlen(mid), RTC_SDP_MID_MAX - 1) : NULL;
    if (!d)
        return -1;
    sdp->bundle = a;
    a[sdp->nbundle++] = d;
    return 0;
}

static rtc_sdp_cand* rtc_add_cand(rtc_sdp* sdp, rtc_sdp_media* m)
{
    rtc_sdp_cand* a = (rtc_sdp_cand*)rtc_grow(sdp, m->cands, m->ncands, &m->cap_cands, sizeof(rtc_sdp_cand), sdp->lim.cands);
    if (!a)
        return NULL;
    m->cands = a;
    return &a[m->ncands++];
}

rtc_sdp_ssrc* rtc_sdp_add_ssrc(rtc_sdp* sdp, rtc_sdp_media* m)
{
    rtc_sdp_ssrc* a = (rtc_sdp_ssrc*)rtc_grow(sdp, m->ssrcs, m->nssrc, &m->cap_ssrc, sizeof(rtc_sdp_ssrc), sdp->lim.ssrc);
    if (!a)
        return NULL;
    m->ssrcs = a;
    memset(&a[m->nssrc], 0, sizeof(*a));
    a[m->nssrc].cname = a[m->nssrc].msid.stream = a[m->nssrc].msid.track = rtc_empty;
    return &a[m->nssrc++];
}

rtc_sdp_ssrc_group* rtc_sdp_add_group(rtc_sdp* sdp, rtc_sdp_media* m)
{
    rtc_sdp_ssrc_group* a = (rtc_sdp_ssrc_group*)rtc_grow(sdp, m->groups, m->ngroups, &m->cap_groups, sizeof(rtc_sdp_ssrc_group), sdp->lim.groups);
    if (!a)
        return NULL;
    m->groups = a;
    memset(&a[m->ngroups], 0, sizeof(*a));
    a[m->ngroups].semantics = rtc_empty;
    return &a[m->ngroups++];
}

int rtc_sdp_extra_add(rtc_sdp* sdp, rtc_sdp_media* m, char type, const char* v, size_t n)
{
    const char*** arr = m ? &m->extra : &sdp->extra;
    int* cnt = m ? &m->nextra : &sdp->nextra;
    int* cap = m ? &m->cap_extra : &sdp->cap_extra;
    const char** a;
    char* d;
    size_t i;
    for (i = 0; i < n; i++)
        if (v[i] == '\n' || v[i] == '\r' || v[i] == 0)
            return -1;
    if (n > sdp->lim.line_max)
        return -1;
    a = (const char**)rtc_grow(sdp, (void*)*arr, *cnt, cap, sizeof(char*), sdp->lim.extra);
    d = a ? (char*)rtc_arena_alloc(&sdp->arena, n + 3) : NULL;
    if (!d)
        return -1;
    d[0] = type;
    d[1] = '=';
    memcpy(d + 2, v, n);
    d[n + 2] = 0;
    *arr = a;
    a[(*cnt)++] = d;
    return 0;
}

static int rtc_parse_fp(const char* v, size_t n, uint8_t* alg, uint8_t* fp, uint8_t* fpl)
{
    const char* p = v;
    const char *e = v + n, *t;
    size_t tl;
    int a, k;
    if (!rtc_tok(&p, e, &t, &tl))
        return -1;
    a = rtc_sdp_fp_alg(t, tl);
    if (!a || !rtc_tok(&p, e, &t, &tl))
        return -1;
    k = rtc_hex_colon_parse(t, tl, fp, 64);
    if (k < 0 || (size_t)k != rtc_sdp_fp_len(a))
        return -1;
    *alg = (uint8_t)a;
    *fpl = (uint8_t)k;
    return 0;
}

static int rtc_parse_setup(const char* v, size_t n)
{
    if (rtc_eq(v, n, "actpass"))
        return RTC_SETUP_ACTPASS;
    if (rtc_eq(v, n, "active"))
        return RTC_SETUP_ACTIVE;
    if (rtc_eq(v, n, "passive"))
        return RTC_SETUP_PASSIVE;
    if (rtc_eq(v, n, "holdconn"))
        return 4;
    return -1;
}

static int rtc_parse_dir(const char* v, size_t n)
{
    if (rtc_eq(v, n, "sendrecv"))
        return RTC_DIR_SENDRECV;
    if (rtc_eq(v, n, "sendonly"))
        return RTC_DIR_SENDONLY;
    if (rtc_eq(v, n, "recvonly"))
        return RTC_DIR_RECVONLY;
    if (rtc_eq(v, n, "inactive"))
        return RTC_DIR_INACTIVE;
    return -1;
}

static const char* const rtc_dir_names[] = { "sendrecv", "sendonly", "recvonly", "inactive" };

static rtc_sdp_codec* rtc_codec_for(rtc_sdp_media* m, uint64_t pt)
{
    int i;
    for (i = 0; i < m->ncodecs; i++)
        if (m->codecs[i].pt == pt)
            return &m->codecs[i];
    return NULL;
}

static int rtc_ice_cred_ok(const char* v, size_t n, size_t lo, size_t hi)
{
    return n >= lo && n <= hi && rtc_ice_chars(v, n);
}

static int rtc_token_chars(const char* s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if ((unsigned char)s[i] <= 0x20 || (unsigned char)s[i] >= 0x7f)
            return 0;
    return n > 0;
}

static int rtc_set(rtc_sdp* sdp, const char** dst, const char* v, size_t n, size_t max)
{
    const char* d = rtc_dup(sdp, v, n, max);
    if (!d)
        return -1;
    *dst = d;
    return 0;
}

static int rtc_ice_opts(rtc_sdp* sdp, const char* v, size_t n, const char** dst, uint8_t* trickle)
{
    const char *p = v, *e = v + n, *t;
    size_t tl;
    if (rtc_set(sdp, dst, v, n, sdp->lim.line_max) < 0)
        return -1;
    while (rtc_tok(&p, e, &t, &tl))
        if (rtc_eq(t, tl, "trickle"))
            *trickle = 1;
    return 0;
}

static int rtc_attr_rtpmap(rtc_sdp* sdp, rtc_sdp_media* m, const char* v, size_t vn)
{
    const char *p = v, *e = v + vn, *t;
    size_t tl, i, sl1 = 0, sl2 = 0;
    uint64_t x;
    rtc_sdp_codec* c;
    if (!rtc_tok(&p, e, &t, &tl) || rtc_u64(t, tl, &x) || x > 127)
        return -1;
    c = rtc_codec_for(m, x);
    if (!c)
        return 1;
    if (c->has_rtpmap || !rtc_tok(&p, e, &t, &tl))
        return -1;
    for (i = 0; i < tl; i++)
        if (t[i] == '/') {
            if (!sl1)
                sl1 = i;
            else if (!sl2)
                sl2 = i;
            else
                return -1;
        }
    if (!sl1 || !rtc_token_chars(t, sl1) || rtc_set(sdp, &c->name, t, sl1, RTC_SDP_TOKEN_MAX - 1) < 0)
        return -1;
    if (rtc_u64(t + sl1 + 1, (sl2 ? sl2 : tl) - sl1 - 1, &x) || x == 0 || x > 0xFFFFFFFFu)
        return -1;
    c->clock = (uint32_t)x;
    if (sl2) {
        if (rtc_u64(t + sl2 + 1, tl - sl2 - 1, &x) || x == 0 || x > 255)
            return -1;
        c->channels = (uint8_t)x;
    }
    if (rtc_tok(&p, e, &t, &tl))
        return -1;
    c->has_rtpmap = 1;
    return 0;
}

int rtc_sdp_fmtp_get(const char* fmtp, const char* key, const char** v, size_t* n)
{
    const char* p = fmtp;
    size_t kl = strlen(key);
    while (*p) {
        const char *ks, *ke, *vs, *ve;
        size_t i;
        while (*p == ' ' || *p == ';')
            p++;
        ks = p;
        while (*p && *p != '=' && *p != ';')
            p++;
        ke = p;
        while (ke > ks && ke[-1] == ' ')
            ke--;
        vs = ve = p;
        if (*p == '=') {
            vs = ++p;
            while (*p && *p != ';')
                p++;
            ve = p;
            while (vs < ve && *vs == ' ')
                vs++;
            while (ve > vs && ve[-1] == ' ')
                ve--;
        }
        if ((size_t)(ke - ks) == kl) {
            for (i = 0; i < kl; i++) {
                char a = ks[i], b = key[i];
                if (a >= 'A' && a <= 'Z')
                    a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z')
                    b = (char)(b + 32);
                if (a != b)
                    break;
            }
            if (i == kl) {
                *v = vs;
                *n = (size_t)(ve - vs);
                return 1;
            }
        }
    }
    return 0;
}

static int rtc_attr_fmtp(rtc_sdp* sdp, rtc_sdp_media* m, const char* v, size_t vn)
{
    const char *p = v, *e = v + vn, *t;
    size_t tl;
    uint64_t x;
    rtc_sdp_codec* c;
    if (!rtc_tok(&p, e, &t, &tl) || rtc_u64(t, tl, &x) || x > 127)
        return -1;
    c = rtc_codec_for(m, x);
    if (!c)
        return 1;
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    if (c->has_fmtp || p == e || rtc_set(sdp, &c->fmtp, p, (size_t)(e - p), sdp->lim.line_max) < 0)
        return -1;
    c->has_fmtp = 1;
    if (rtc_sdp_fmtp_get(c->fmtp, "apt", &t, &tl)) {
        if (rtc_u64(t, tl, &x) || x > 127)
            return -1;
        c->rtx_apt = (uint8_t)x;
        c->rtx_apt_set = 1;
    }
    return 0;
}

static int rtc_attr_rtcp_fb(rtc_sdp_media* m, const char* v, size_t vn)
{
    const char *p = v, *e = v + vn, *t, *t2, *t3;
    size_t tl, tl2 = 0, tl3;
    int i, all;
    uint8_t f = 0;
    uint64_t x = 0;
    if (!rtc_tok(&p, e, &t, &tl))
        return -1;
    all = rtc_eq(t, tl, "*");
    if (!all && (rtc_u64(t, tl, &x) || x > 127))
        return -1;
    if (!rtc_tok(&p, e, &t, &tl))
        return -1;
    if (!rtc_tok(&p, e, &t2, &tl2))
        tl2 = 0;
    if (rtc_tok(&p, e, &t3, &tl3))
        return 1;
    if (rtc_eq(t, tl, "nack"))
        f = tl2 == 0 ? RTC_FB_NACK : rtc_eq(t2, tl2, "pli") ? RTC_FB_PLI : 0;
    else if (rtc_eq(t, tl, "ccm"))
        f = tl2 && rtc_eq(t2, tl2, "fir") ? RTC_FB_FIR : 0;
    else if (rtc_eq(t, tl, "transport-cc") && !tl2)
        f = RTC_FB_TWCC;
    else if (rtc_eq(t, tl, "goog-remb") && !tl2)
        f = RTC_FB_REMB;
    if (!f || (!all && !rtc_codec_for(m, x)))
        return 1;
    for (i = 0; i < m->ncodecs; i++)
        if (all || m->codecs[i].pt == x)
            m->codecs[i].fb |= f;
    return 0;
}

static int rtc_attr_extmap(rtc_sdp* sdp, rtc_sdp_media* m, const char* v, size_t vn)
{
    const char *p = v, *e = v + vn, *t;
    size_t tl, i;
    uint64_t x;
    rtc_sdp_ext* x2;
    if (!rtc_tok(&p, e, &t, &tl))
        return -1;
    for (i = 0; i < tl && t[i] != '/'; i++) {
    }
    if (rtc_u64(t, i, &x) || x < 1 || x > 255 || !(x2 = rtc_sdp_add_ext(sdp, m)))
        return -1;
    x2->id = (uint16_t)x;
    if (i < tl) {
        int d = rtc_parse_dir(t + i + 1, tl - i - 1);
        if (d < 0)
            return -1;
        x2->has_dir = 1;
        x2->dir = (uint8_t)d;
    }
    if (!rtc_tok(&p, e, &t, &tl) || rtc_set(sdp, &x2->uri, t, tl, sdp->lim.line_max) < 0)
        return -1;
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    return rtc_set(sdp, &x2->attrs, p, (size_t)(e - p), sdp->lim.line_max);
}

static int rtc_msid_parse(rtc_sdp* sdp, const char* v, size_t vn, rtc_sdp_msid* o)
{
    const char *p = v, *e = v + vn, *t;
    size_t tl;
    o->stream = o->track = rtc_empty;
    if (!rtc_tok(&p, e, &t, &tl) || rtc_set(sdp, &o->stream, t, tl, RTC_SDP_TOKEN_MAX - 1) < 0)
        return -1;
    if (rtc_tok(&p, e, &t, &tl) && rtc_set(sdp, &o->track, t, tl, RTC_SDP_TOKEN_MAX - 1) < 0)
        return -1;
    return rtc_tok(&p, e, &t, &tl) ? -1 : 0;
}

static int rtc_attr_ssrc(rtc_sdp* sdp, rtc_sdp_media* m, const char* v, size_t vn)
{
    const char *p = v, *e = v + vn, *t;
    size_t tl, i;
    uint64_t x;
    int k;
    rtc_sdp_ssrc* s;
    if (!rtc_tok(&p, e, &t, &tl) || rtc_u64(t, tl, &x) || x > 0xFFFFFFFFu)
        return -1;
    while (p < e && *p == ' ')
        p++;
    if (p == e)
        return -1;
    for (k = 0; k < m->nssrc; k++)
        if (m->ssrcs[k].id == x)
            break;
    if (k == m->nssrc) {
        if (!(s = rtc_sdp_add_ssrc(sdp, m)))
            return -1;
        s->id = (uint32_t)x;
    } else
        s = &m->ssrcs[k];
    for (i = 0; p + i < e && p[i] != ':'; i++) {
    }
    if (rtc_eq(p, i, "cname") && p + i < e && !s->cname[0])
        return p + i + 1 == e || rtc_set(sdp, &s->cname, p + i + 1, (size_t)(e - p - i - 1), sdp->lim.line_max) < 0 ? -1 : 0;
    if (rtc_eq(p, i, "msid") && p + i < e && !s->msid.stream[0])
        return rtc_msid_parse(sdp, p + i + 1, (size_t)(e - p - i - 1), &s->msid);
    return 1;
}

static int rtc_attr_ssrc_group(rtc_sdp* sdp, rtc_sdp_media* m, const char* v, size_t vn)
{
    const char *p = v, *e = v + vn, *t;
    size_t tl;
    uint64_t x;
    const char* q;
    int n = 0, k = 0;
    rtc_sdp_ssrc_group* g;
    if (!rtc_tok(&p, e, &t, &tl) || !(g = rtc_sdp_add_group(sdp, m)) || rtc_set(sdp, &g->semantics, t, tl, RTC_SDP_TOKEN_MAX - 1) < 0)
        return -1;
    for (q = p; rtc_tok(&q, e, &t, &tl);)
        if (++n > sdp->lim.group_ssrcs)
            return -1;
    if (!n || !(g->ssrc = (uint32_t*)rtc_arena_alloc(&sdp->arena, sizeof(uint32_t) * (size_t)n)))
        return -1;
    while (rtc_tok(&p, e, &t, &tl)) {
        if (rtc_u64(t, tl, &x) || x > 0xFFFFFFFFu)
            return -1;
        g->ssrc[k++] = (uint32_t)x;
    }
    g->n = k;
    return 0;
}

static int rtc_sdp_session_attr(rtc_sdp* sdp, const char* a, size_t an, const char* v, size_t vn)
{
    if (rtc_eq(a, an, "group")) {
        const char *p = v, *e = v + vn, *t;
        size_t tl;
        if (!rtc_tok(&p, e, &t, &tl) || !rtc_eq(t, tl, "BUNDLE"))
            return 1;
        sdp->nbundle_groups++;
        if (sdp->has_bundle)
            return 1;
        sdp->has_bundle = 1;
        while (rtc_tok(&p, e, &t, &tl)) {
            const char** b = (const char**)rtc_grow(sdp, (void*)sdp->bundle, sdp->nbundle, &sdp->cap_bundle, sizeof(char*), sdp->lim.media);
            if (!b || rtc_set(sdp, &b[sdp->nbundle], t, tl, RTC_SDP_MID_MAX - 1) < 0)
                return -1;
            sdp->bundle = b;
            sdp->nbundle++;
        }
        return 0;
    }
    if (rtc_eq(a, an, "ice-lite") && !vn) {
        sdp->ice_lite = 1;
        return 0;
    }
    return 1;
}

static int rtc_sdp_common_attr(rtc_sdp* sdp, rtc_sdp_media* m, const char* a, size_t an, const char* v, size_t vn)
{
    if (rtc_eq(a, an, "ice-ufrag")) {
        const char** d = m ? &m->ufrag : &sdp->ufrag;
        if ((*d)[0] || !rtc_ice_cred_ok(v, vn, 4, 256))
            return -1;
        return rtc_set(sdp, d, v, vn, RTC_SDP_CRED_MAX - 1);
    }
    if (rtc_eq(a, an, "ice-pwd")) {
        const char** d = m ? &m->pwd : &sdp->pwd;
        if ((*d)[0] || !rtc_ice_cred_ok(v, vn, 22, 256))
            return -1;
        return rtc_set(sdp, d, v, vn, RTC_SDP_CRED_MAX - 1);
    }
    if (rtc_eq(a, an, "ice-options"))
        return rtc_ice_opts(sdp, v, vn, m ? &m->ice_opts : &sdp->ice_opts, m ? &m->trickle : &sdp->trickle);
    if (rtc_eq(a, an, "fingerprint")) {
        uint8_t alg, fp[64], fpl;
        if (rtc_parse_fp(v, vn, &alg, fp, &fpl) < 0)
            return -1;
        if (m ? m->fp_len : sdp->fp_len)
            return 1;
        memcpy(m ? m->fp : sdp->fp, fp, fpl);
        *(m ? &m->fp_alg : &sdp->fp_alg) = alg;
        *(m ? &m->fp_len : &sdp->fp_len) = fpl;
        return 0;
    }
    if (rtc_eq(a, an, "setup")) {
        int st = rtc_parse_setup(v, vn);
        if (st < 0)
            return -1;
        if (st == 4)
            *(m ? &m->holdconn : &sdp->holdconn) = 1;
        else
            *(m ? &m->setup : &sdp->setup) = (uint8_t)st;
        return 0;
    }
    if (rtc_eq(a, an, "extmap-allow-mixed") && !vn) {
        *(m ? &m->allow_mixed : &sdp->allow_mixed) = 1;
        return 0;
    }
    return 2;
}

static int rtc_sdp_media_attr(rtc_sdp* sdp, rtc_sdp_media* m, const char* a, size_t an, const char* v, size_t vn)
{
    uint64_t x;
    int d;
    if (rtc_eq(a, an, "mid")) {
        if (m->mid[0] || !rtc_token_chars(v, vn))
            return -1;
        return rtc_set(sdp, &m->mid, v, vn, RTC_SDP_MID_MAX - 1);
    }
    if (rtc_eq(a, an, "sctp-port")) {
        if (rtc_u64(v, vn, &x) || x > 65535)
            return -1;
        m->sctp_port = (uint16_t)x;
        m->has_sctp_port = 1;
        return 0;
    }
    if (rtc_eq(a, an, "max-message-size")) {
        if (rtc_u64(v, vn, &x))
            return -1;
        m->max_msg = x > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)x;
        m->has_max_msg = 1;
        return 0;
    }
    if (rtc_eq(a, an, "candidate")) {
        rtc_sdp_cand c, *slot;
        if (rtc_sdp_parse_candidate(v, vn, &c) == 0) {
            if (!(slot = rtc_add_cand(sdp, m)))
                return -1;
            *slot = c;
        }
        return 1;
    }
    if (rtc_eq(a, an, "end-of-candidates")) {
        m->eoc = 1;
        return 1;
    }
    d = vn ? -1 : rtc_parse_dir(a, an);
    if (d >= 0) {
        if (m->has_dir)
            return -1;
        m->dir = (uint8_t)d;
        m->has_dir = 1;
        return 0;
    }
    if (!vn && rtc_eq(a, an, "rtcp-mux"))
        m->rtcp_mux = 1;
    else if (!vn && rtc_eq(a, an, "rtcp-mux-only"))
        m->rtcp_mux_only = 1;
    else if (!vn && rtc_eq(a, an, "rtcp-rsize"))
        m->rtcp_rsize = 1;
    else if (!vn && rtc_eq(a, an, "bundle-only"))
        m->bundle_only = 1;
    else if (rtc_eq(a, an, "msid")) {
        rtc_sdp_msid* s = rtc_sdp_add_msid(sdp, m);
        if (!s || rtc_msid_parse(sdp, v, vn, s) < 0)
            return -1;
    } else if (rtc_eq(a, an, "rtpmap"))
        return rtc_attr_rtpmap(sdp, m, v, vn);
    else if (rtc_eq(a, an, "fmtp"))
        return rtc_attr_fmtp(sdp, m, v, vn);
    else if (rtc_eq(a, an, "rtcp-fb"))
        return rtc_attr_rtcp_fb(m, v, vn);
    else if (rtc_eq(a, an, "extmap"))
        return rtc_attr_extmap(sdp, m, v, vn);
    else if (rtc_eq(a, an, "ssrc"))
        return rtc_attr_ssrc(sdp, m, v, vn);
    else if (rtc_eq(a, an, "ssrc-group"))
        return rtc_attr_ssrc_group(sdp, m, v, vn);
    else
        return 1;
    return 0;
}

static int rtc_conn_ok(const char* v, size_t n)
{
    const char *p = v, *e = v + n, *t;
    size_t tl;
    if (!rtc_tok(&p, e, &t, &tl) || !rtc_eq(t, tl, "IN"))
        return 0;
    if (!rtc_tok(&p, e, &t, &tl) || !(rtc_eq(t, tl, "IP4") || rtc_eq(t, tl, "IP6")))
        return 0;
    if (!rtc_tok(&p, e, &t, &tl) || !rtc_token_chars(t, tl))
        return 0;
    return !rtc_tok(&p, e, &t, &tl);
}

static int rtc_is_rtp(const char* proto)
{
    return strstr(proto, "RTP/") != NULL;
}

static int rtc_parse_mline(rtc_sdp* sdp, rtc_sdp_media* m, const char* v, size_t vn)
{
    const char *q = v, *e = v + vn, *t, *fs = NULL, *fe = NULL;
    size_t tl, i;
    uint64_t x;
    if (!rtc_tok(&q, e, &t, &tl) || !rtc_token_chars(t, tl) || rtc_set(sdp, &m->kind, t, tl, RTC_SDP_TOKEN_MAX - 1) < 0)
        return -1;
    if (!rtc_tok(&q, e, &t, &tl))
        return -1;
    for (i = 0; i < tl && t[i] != '/'; i++) {
    }
    if (rtc_u64(t, i, &x) || x > 65535)
        return -1;
    m->port = (uint16_t)x;
    if (i < tl) {
        if (rtc_u64(t + i + 1, tl - i - 1, &x) || x == 0 || x > 65535)
            return -1;
        m->port_count = (uint16_t)x;
        m->has_port_count = 1;
    }
    if (!rtc_tok(&q, e, &t, &tl) || !rtc_token_chars(t, tl) || rtc_set(sdp, &m->proto, t, tl, RTC_SDP_TOKEN_MAX - 1) < 0)
        return -1;
    while (rtc_tok(&q, e, &t, &tl)) {
        if (rtc_is_rtp(m->proto) && rtc_u64(t, tl, &x) == 0 && x < 128) {
            rtc_sdp_codec* c;
            if (rtc_codec_for(m, x) || !(c = rtc_sdp_add_codec(sdp, m)))
                return -1;
            c->pt = (uint8_t)x;
        } else {
            if (!rtc_token_chars(t, tl))
                return -1;
            if (!fs)
                fs = t;
            fe = t + tl;
        }
    }
    if (fs) {
        char* d;
        size_t n = 0, k;
        const char* p = fs;
        if ((size_t)(fe - fs) > sdp->lim.line_max || !(d = (char*)rtc_arena_alloc(&sdp->arena, (size_t)(fe - fs) + 1)))
            return -1;
        while (rtc_tok(&p, fe, &t, &tl)) {
            if (rtc_is_rtp(m->proto) && rtc_u64(t, tl, &x) == 0 && x < 128)
                continue;
            if (n)
                d[n++] = ' ';
            for (k = 0; k < tl; k++)
                d[n++] = t[k];
        }
        d[n] = 0;
        m->fmt_text = d;
    }
    return m->ncodecs || fs ? 0 : -1;
}

static int rtc_parse_origin(rtc_sdp* sdp, const char* v, size_t vn)
{
    const char *q = v, *e = v + vn, *t;
    size_t tl;
    uint64_t x;
    int k = 0;
    if (sdp->has_o || rtc_set(sdp, &sdp->origin, v, vn, sdp->lim.line_max) < 0)
        return -1;
    while (rtc_tok(&q, e, &t, &tl)) {
        if (k == 1 || k == 2) {
            if (rtc_u64(t, tl, &x))
                return -1;
            if (k == 1)
                sdp->sess_id = x;
            else
                sdp->sess_ver = x;
        }
        k++;
    }
    if (k != 6)
        return -1;
    sdp->has_o = 1;
    return 0;
}

static int rtc_sdp_line(rtc_sdp* sdp, rtc_sdp_media** mp, char ty, const char* v, size_t vn, int* have_v, const char** err)
{
    rtc_sdp_media* m = *mp;
    int r = 0;
    switch (ty) {
    case 'v':
        if (*have_v || !rtc_eq(v, vn, "0")) {
            *err = "unsupported SDP version";
            return -1;
        }
        *have_v = 1;
        return 0;
    case 'o':
        if (m || rtc_parse_origin(sdp, v, vn) < 0) {
            *err = "malformed o= line";
            return -1;
        }
        return 0;
    case 's':
        if (m || sdp->has_s || rtc_set(sdp, &sdp->name, v, vn, sdp->lim.line_max) < 0) {
            *err = "malformed s= line";
            return -1;
        }
        sdp->has_s = 1;
        return 0;
    case 't':
        if (m)
            r = 1;
        else if (sdp->has_t || rtc_set(sdp, &sdp->timing, v, vn, sdp->lim.line_max) < 0) {
            *err = "malformed t= line";
            return -1;
        } else
            sdp->has_t = 1;
        break;
    case 'c':
        if (!rtc_conn_ok(v, vn)) {
            *err = "malformed c= line";
            return -1;
        }
        if (m ? m->has_c : sdp->has_c)
            r = 1;
        else if (rtc_set(sdp, m ? &m->conn : &sdp->conn, v, vn, sdp->lim.line_max) < 0) {
            *err = "malformed c= line";
            return -1;
        } else
            *(m ? &m->has_c : &sdp->has_c) = 1;
        break;
    case 'm':
        if (!(m = rtc_sdp_add_media(sdp))) {
            *err = "too many m= sections";
            return -1;
        }
        *mp = m;
        if (rtc_parse_mline(sdp, m, v, vn) < 0) {
            *err = "malformed m= line";
            return -1;
        }
        return 0;
    case 'a': {
        size_t i;
        const char* av;
        size_t avn;
        for (i = 0; i < vn && v[i] != ':'; i++) {
        }
        av = i < vn ? v + i + 1 : v + vn;
        avn = i < vn ? vn - i - 1 : 0;
        if (!i) {
            *err = "malformed a= attribute";
            return -1;
        }
        r = rtc_sdp_common_attr(sdp, m, v, i, av, avn);
        if (r == 2)
            r = m ? rtc_sdp_media_attr(sdp, m, v, i, av, avn) : rtc_sdp_session_attr(sdp, v, i, av, avn);
        if (r < 0) {
            *err = "malformed a= attribute";
            return -1;
        }
        break;
    }
    default:
        r = 1;
        break;
    }
    if (r == 1 && rtc_sdp_extra_add(sdp, m, ty, v, vn) < 0) {
        *err = "too many unrecognised SDP lines";
        return -1;
    }
    return 0;
}

int rtc_sdp_parse(rtc_sdp* sdp, const char* s, size_t n, const char** err)
{
    const char *p = s, *e = s + n;
    rtc_sdp_media* m = NULL;
    int line = 0, have_v = 0;
    rtc_sdp_reset(sdp, n + n / 2 + 1024);
    *err = NULL;
    if (n > sdp->lim.max_input) {
        *err = "SDP larger than its input limit";
        return -1;
    }
    while (p < e) {
        const char* le = memchr(p, '\n', (size_t)(e - p));
        const char* lend = le ? le : e;
        const char* v;
        size_t vn;
        if (lend > p && lend[-1] == '\r')
            lend--;
        line++;
        if (lend == p) {
            p = le ? le + 1 : e;
            continue;
        }
        if (lend - p < 2 || p[1] != '=' || p[0] < 'a' || p[0] > 'z') {
            *err = "malformed SDP line";
            return -1;
        }
        v = p + 2;
        vn = (size_t)(lend - v);
        if (vn > sdp->lim.line_max || memchr(v, 0, vn) || memchr(v, '\r', vn)) {
            *err = "malformed SDP line";
            return -1;
        }
        if (line == 1 && p[0] != 'v') {
            *err = "SDP must start with v=";
            return -1;
        }
        if (rtc_sdp_line(sdp, &m, p[0], v, vn, &have_v, err) < 0) {
            if (!*err)
                *err = "out of memory";
            return -1;
        }
        p = le ? le + 1 : e;
    }
    if (!have_v) {
        *err = "SDP must start with v=";
        return -1;
    }
    return 0;
}

int rtc_sdp_media_rejected(const rtc_sdp_media* m)
{
    return m->port == 0 && !m->bundle_only;
}

static int rtc_in_bundle(const rtc_sdp* sdp, const char* mid)
{
    int i;
    for (i = 0; i < sdp->nbundle; i++)
        if (!strcmp(sdp->bundle[i], mid))
            return 1;
    return 0;
}

int rtc_sdp_check(const rtc_sdp* sdp, const char** err)
{
    int i, j, k;
    *err = NULL;
    if (!sdp->has_o || !sdp->has_s || !sdp->has_t) {
        *err = "missing o=, s= or t= line (RFC 8866 5)";
        return -1;
    }
    if (sdp->nbundle_groups > 1) {
        *err = "more than one BUNDLE group is not supported";
        return -1;
    }
    for (i = 0; i < sdp->nbundle; i++) {
        for (j = 0; j < i; j++)
            if (!strcmp(sdp->bundle[i], sdp->bundle[j])) {
                *err = "a mid appears twice in the BUNDLE group (RFC 9143 7.2)";
                return -1;
            }
        for (j = 0; j < sdp->nmedia; j++)
            if (!strcmp(sdp->bundle[i], sdp->media[j].mid))
                break;
        if (j == sdp->nmedia) {
            *err = "the BUNDLE group names a mid with no m= section (RFC 9143 7.2)";
            return -1;
        }
    }
    for (i = 0; i < sdp->nmedia; i++) {
        const rtc_sdp_media* m = &sdp->media[i];
        if (!sdp->has_c && !m->has_c) {
            *err = "an m= section has no c= line and there is no session c= line (RFC 8866 5.7)";
            return -1;
        }
        if (m->mid[0])
            for (j = 0; j < i; j++)
                if (!strcmp(m->mid, sdp->media[j].mid)) {
                    *err = "duplicate a=mid (RFC 9143 7.2.1)";
                    return -1;
                }
        if (m->bundle_only && (m->port != 0 || !m->mid[0] || !rtc_in_bundle(sdp, m->mid))) {
            *err = "a=bundle-only needs port 0 and a bundled m= section (RFC 9143 6)";
            return -1;
        }
        if (m->rtcp_mux_only && !m->rtcp_mux) {
            *err = "a=rtcp-mux-only without a=rtcp-mux (RFC 8858 3)";
            return -1;
        }
        for (j = 0; j < m->next; j++)
            for (k = 0; k < j; k++)
                if (m->ext[j].id == m->ext[k].id) {
                    *err = "duplicate a=extmap id (RFC 8285 5)";
                    return -1;
                }
        for (j = 0; j < m->ncodecs; j++) {
            const rtc_sdp_codec* c = &m->codecs[j];
            if (c->pt >= 96 && !c->has_rtpmap && !rtc_sdp_media_rejected(m)) {
                *err = "a dynamic payload type has no a=rtpmap (RFC 8866 6.6)";
                return -1;
            }
        }
    }
    return 0;
}

static void rtc_sb_extra(rtc_sb* b, const char* const* x, int n, const char* letters, int others)
{
    int i;
    for (i = 0; i < n; i++)
        if ((strchr(letters, x[i][0]) != NULL) != (others != 0))
            rtc_sb_line(b, x[i], NULL);
}

static void rtc_sb_fp(rtc_sb* b, int alg, const uint8_t* fp, size_t n)
{
    char hx[64 * 3 + 1];
    rtc_hex_upper_colon(fp, n, hx, sizeof(hx));
    rtc_sb_str(b, "a=fingerprint:");
    rtc_sb_str(b, rtc_sdp_fp_name(alg));
    rtc_sb_put(b, " ", 1);
    rtc_sb_line(b, hx, NULL);
}

static void rtc_sb_transport(rtc_sb* b, const char* uf, const char* pw, const char* opts, int alg, const uint8_t* fp, size_t fpl,
    int setup, int hold)
{
    static const char* const setups[] = { "", "actpass", "active", "passive" };
    if (uf[0])
        rtc_sb_line(b, "a=ice-ufrag:", uf);
    if (pw[0])
        rtc_sb_line(b, "a=ice-pwd:", pw);
    if (opts[0])
        rtc_sb_line(b, "a=ice-options:", opts);
    if (fpl)
        rtc_sb_fp(b, alg, fp, fpl);
    if (setup)
        rtc_sb_line(b, "a=setup:", setups[setup & 3]);
    if (hold)
        rtc_sb_str(b, "a=setup:holdconn\r\n");
}

static void rtc_sb_pt_attr(rtc_sb* b, const char* attr, unsigned pt)
{
    rtc_sb_str(b, attr);
    rtc_sb_u64(b, pt);
    rtc_sb_put(b, " ", 1);
}

static void rtc_sb_codec(rtc_sb* b, const rtc_sdp_codec* c)
{
    static const char* const fbn[] = { "goog-remb", "transport-cc", "ccm fir", "nack", "nack pli" };
    static const uint8_t fbv[] = { RTC_FB_REMB, RTC_FB_TWCC, RTC_FB_FIR, RTC_FB_NACK, RTC_FB_PLI };
    size_t i;
    if (c->has_rtpmap) {
        rtc_sb_pt_attr(b, "a=rtpmap:", c->pt);
        rtc_sb_str(b, c->name);
        rtc_sb_put(b, "/", 1);
        rtc_sb_u64(b, c->clock);
        if (c->channels) {
            rtc_sb_put(b, "/", 1);
            rtc_sb_u64(b, c->channels);
        }
        rtc_sb_put(b, "\r\n", 2);
    }
    for (i = 0; i < sizeof(fbv); i++)
        if (c->fb & fbv[i]) {
            rtc_sb_pt_attr(b, "a=rtcp-fb:", c->pt);
            rtc_sb_line(b, fbn[i], NULL);
        }
    if (c->has_fmtp) {
        rtc_sb_pt_attr(b, "a=fmtp:", c->pt);
        rtc_sb_line(b, c->fmtp, NULL);
    }
}

static void rtc_sb_msid(rtc_sb* b, const char* pre, const rtc_sdp_msid* s)
{
    rtc_sb_str(b, pre);
    rtc_sb_str(b, s->stream);
    if (s->track[0]) {
        rtc_sb_put(b, " ", 1);
        rtc_sb_str(b, s->track);
    }
    rtc_sb_put(b, "\r\n", 2);
}

static void rtc_sb_media(rtc_sb* b, const rtc_sdp_media* m)
{
    int i, j;
    rtc_sb_str(b, "m=");
    rtc_sb_str(b, m->kind);
    rtc_sb_put(b, " ", 1);
    rtc_sb_u64(b, m->port);
    if (m->has_port_count) {
        rtc_sb_put(b, "/", 1);
        rtc_sb_u64(b, m->port_count);
    }
    rtc_sb_put(b, " ", 1);
    rtc_sb_str(b, m->proto);
    for (i = 0; i < m->ncodecs; i++) {
        rtc_sb_put(b, " ", 1);
        rtc_sb_u64(b, m->codecs[i].pt);
    }
    if (m->fmt_text[0]) {
        rtc_sb_put(b, " ", 1);
        rtc_sb_str(b, m->fmt_text);
    }
    rtc_sb_put(b, "\r\n", 2);
    rtc_sb_extra(b, m->extra, m->nextra, "i", 0);
    if (m->has_c)
        rtc_sb_line(b, "c=", m->conn);
    rtc_sb_extra(b, m->extra, m->nextra, "cbk", 0);
    rtc_sb_transport(b, m->ufrag, m->pwd, m->ice_opts, m->fp_alg, m->fp, m->fp_len, m->setup, m->holdconn);
    if (m->mid[0])
        rtc_sb_line(b, "a=mid:", m->mid);
    if (m->allow_mixed)
        rtc_sb_str(b, "a=extmap-allow-mixed\r\n");
    for (i = 0; i < m->next; i++) {
        const rtc_sdp_ext* x = &m->ext[i];
        rtc_sb_str(b, "a=extmap:");
        rtc_sb_u64(b, x->id);
        if (x->has_dir) {
            rtc_sb_put(b, "/", 1);
            rtc_sb_str(b, rtc_dir_names[x->dir & 3]);
        }
        rtc_sb_put(b, " ", 1);
        rtc_sb_str(b, x->uri);
        if (x->attrs[0]) {
            rtc_sb_put(b, " ", 1);
            rtc_sb_str(b, x->attrs);
        }
        rtc_sb_put(b, "\r\n", 2);
    }
    if (m->has_dir)
        rtc_sb_line(b, "a=", rtc_dir_names[m->dir & 3]);
    for (i = 0; i < m->nmsid; i++)
        rtc_sb_msid(b, "a=msid:", &m->msid[i]);
    if (m->rtcp_mux)
        rtc_sb_str(b, "a=rtcp-mux\r\n");
    if (m->rtcp_mux_only)
        rtc_sb_str(b, "a=rtcp-mux-only\r\n");
    if (m->rtcp_rsize)
        rtc_sb_str(b, "a=rtcp-rsize\r\n");
    if (m->bundle_only)
        rtc_sb_str(b, "a=bundle-only\r\n");
    for (i = 0; i < m->ncodecs; i++)
        rtc_sb_codec(b, &m->codecs[i]);
    for (i = 0; i < m->ngroups; i++) {
        rtc_sb_str(b, "a=ssrc-group:");
        rtc_sb_str(b, m->groups[i].semantics);
        for (j = 0; j < m->groups[i].n; j++) {
            rtc_sb_put(b, " ", 1);
            rtc_sb_u64(b, m->groups[i].ssrc[j]);
        }
        rtc_sb_put(b, "\r\n", 2);
    }
    for (i = 0; i < m->nssrc; i++) {
        const rtc_sdp_ssrc* s = &m->ssrcs[i];
        if (s->cname[0]) {
            rtc_sb_str(b, "a=ssrc:");
            rtc_sb_u64(b, s->id);
            rtc_sb_line(b, " cname:", s->cname);
        }
        if (s->msid.stream[0]) {
            rtc_sb_str(b, "a=ssrc:");
            rtc_sb_u64(b, s->id);
            rtc_sb_msid(b, " msid:", &s->msid);
        }
    }
    if (m->has_sctp_port) {
        rtc_sb_str(b, "a=sctp-port:");
        rtc_sb_u64(b, m->sctp_port);
        rtc_sb_put(b, "\r\n", 2);
    }
    if (m->has_max_msg) {
        rtc_sb_str(b, "a=max-message-size:");
        rtc_sb_u64(b, m->max_msg);
        rtc_sb_put(b, "\r\n", 2);
    }
    rtc_sb_extra(b, m->extra, m->nextra, "a", 0);
    rtc_sb_extra(b, m->extra, m->nextra, "icbka", 1);
}

int rtc_sdp_write(const rtc_sdp* sdp, rtc_sb* b)
{
    int i;
    rtc_sb_str(b, "v=0\r\n");
    if (sdp->has_o)
        rtc_sb_line(b, "o=", sdp->origin);
    if (sdp->has_s)
        rtc_sb_line(b, "s=", sdp->name);
    rtc_sb_extra(b, sdp->extra, sdp->nextra, "iuep", 0);
    if (sdp->has_c)
        rtc_sb_line(b, "c=", sdp->conn);
    rtc_sb_extra(b, sdp->extra, sdp->nextra, "cb", 0);
    if (sdp->has_t)
        rtc_sb_line(b, "t=", sdp->timing);
    rtc_sb_extra(b, sdp->extra, sdp->nextra, "rzk", 0);
    if (sdp->has_bundle) {
        rtc_sb_str(b, "a=group:BUNDLE");
        for (i = 0; i < sdp->nbundle; i++) {
            rtc_sb_put(b, " ", 1);
            rtc_sb_str(b, sdp->bundle[i]);
        }
        rtc_sb_put(b, "\r\n", 2);
    }
    if (sdp->allow_mixed)
        rtc_sb_str(b, "a=extmap-allow-mixed\r\n");
    if (sdp->ice_lite)
        rtc_sb_str(b, "a=ice-lite\r\n");
    rtc_sb_transport(b, sdp->ufrag, sdp->pwd, sdp->ice_opts, sdp->fp_alg, sdp->fp, sdp->fp_len, sdp->setup, sdp->holdconn);
    rtc_sb_extra(b, sdp->extra, sdp->nextra, "a", 0);
    rtc_sb_extra(b, sdp->extra, sdp->nextra, "iuepcbrzka", 1);
    for (i = 0; i < sdp->nmedia; i++)
        rtc_sb_media(b, &sdp->media[i]);
    return b->err ? -1 : 0;
}
