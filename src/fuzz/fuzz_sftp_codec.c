#include "modules/sftp/sftp-internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void fz_die(int line)
{
    fprintf(stderr, "fuzz_sftp_codec: oracle failed at line %d\n", line);
    abort();
}

#define FZ(c) \
    do { \
        if (!(c)) \
            fz_die(__LINE__); \
    } while (0)

#define KNOWN (SFTP_A_SIZE | SFTP_A_UIDGID | SFTP_A_PERM | SFTP_A_TIME)

static uint32_t nbe32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int n_get(const uint8_t* p, size_t n, sftp_attr_t* a, size_t* used)
{
    size_t o = 4;
    uint32_t f, i, cnt;
    memset(a, 0, sizeof *a);
    if (n < 4)
        return -1;
    f = nbe32(p);
    if (f & SFTP_A_SIZE) {
        if (n - o < 8)
            return -1;
        a->size = (uint64_t)nbe32(p + o) << 32 | nbe32(p + o + 4);
        o += 8;
    }
    if (f & SFTP_A_UIDGID) {
        if (n - o < 8)
            return -1;
        a->uid = nbe32(p + o);
        a->gid = nbe32(p + o + 4);
        o += 8;
    }
    if (f & SFTP_A_PERM) {
        if (n - o < 4)
            return -1;
        a->perm = nbe32(p + o);
        o += 4;
    }
    if (f & SFTP_A_TIME) {
        if (n - o < 8)
            return -1;
        a->atime = nbe32(p + o);
        a->mtime = nbe32(p + o + 4);
        o += 8;
    }
    if (f & SFTP_A_EXT) {
        if (n - o < 4)
            return -1;
        cnt = nbe32(p + o);
        o += 4;
        if (cnt > 64)
            return -1;
        for (i = 0; i < 2 * cnt; i++) {
            if (n - o < 4 || n - o - 4 < nbe32(p + o))
                return -1;
            o += 4 + nbe32(p + o);
        }
    }
    a->flags = f & KNOWN;
    *used = o;
    return 0;
}

static size_t n_len(uint32_t f)
{
    return 4 + (f & SFTP_A_SIZE ? 8 : 0) + (f & SFTP_A_UIDGID ? 8 : 0) + (f & SFTP_A_PERM ? 4 : 0) + (f & SFTP_A_TIME ? 8 : 0);
}

static int a_eq(const sftp_attr_t* a, const sftp_attr_t* b)
{
    return a->flags == b->flags && a->size == b->size && a->uid == b->uid && a->gid == b->gid && a->perm == b->perm && a->atime == b->atime
        && a->mtime == b->mtime;
}

static void a_canon(sftp_attr_t* a)
{
    a->flags &= KNOWN;
    if (!(a->flags & SFTP_A_SIZE))
        a->size = 0;
    if (!(a->flags & SFTP_A_UIDGID))
        a->uid = a->gid = 0;
    if (!(a->flags & SFTP_A_PERM))
        a->perm = 0;
    if (!(a->flags & SFTP_A_TIME))
        a->atime = a->mtime = 0;
}

static void m_raw(const uint8_t* d, size_t n)
{
    sftp_attr_t a, b, c;
    sshr_t r;
    sshb_t o = { 0 };
    size_t used = 0;
    int rc, nr;
    sshr_init(&r, d, n);
    rc = sftp_attr_get(&r, &a);
    nr = n_get(d, n, &b, &used);
    FZ(rc == nr);
    if (rc < 0)
        return;
    FZ(r.off == used && a_eq(&a, &b));
    FZ(sftp_attr_put(&o, &a) == 0 && o.len == n_len(a.flags));
    sshr_init(&r, o.p, o.len);
    FZ(sftp_attr_get(&r, &c) == 0 && r.off == o.len && a_eq(&a, &c));
    if (!(nbe32(d) & SFTP_A_EXT))
        FZ(o.len == used && !memcmp(o.p + 4, d + 4, used - 4) && nbe32(o.p) == (nbe32(d) & KNOWN));
    sshb_free(&o);
}

static uint32_t rd32(const uint8_t* d, size_t n, size_t* i)
{
    uint32_t v = 0;
    unsigned k;
    for (k = 0; k < 4; k++)
        v = v << 8 | (*i < n ? d[(*i)++] : 0);
    return v;
}

static void m_gen(const uint8_t* d, size_t n)
{
    sftp_attr_t a, b;
    sshb_t o = { 0 };
    sshr_t r;
    size_t i = 0, base;
    uint32_t ne, k;
    a.flags = rd32(d, n, &i);
    a.size = (uint64_t)rd32(d, n, &i) << 32 | rd32(d, n, &i);
    a.uid = rd32(d, n, &i);
    a.gid = rd32(d, n, &i);
    a.perm = rd32(d, n, &i);
    a.atime = rd32(d, n, &i);
    a.mtime = rd32(d, n, &i);
    FZ(sftp_attr_put(&o, &a) == 0);
    a_canon(&a);
    FZ(o.len == n_len(a.flags) && nbe32(o.p) == a.flags);
    base = o.len;
    ne = i < n ? d[i++] % 70 : 0;
    if (ne) {
        ssh_put_be32(o.p, a.flags | SFTP_A_EXT);
        sshb_u32(&o, ne);
        for (k = 0; k < 2 * ne; k++) {
            size_t l = i < n ? d[i++] % 9 : 0;
            sshb_str(&o, i + l <= n ? d + i : (const uint8_t*)"xxxxxxxx", l);
            i += l;
        }
        if (i < n && d[i] % 5 == 0)
            o.len -= 1 + d[i] % (o.len - base);
    }
    sshr_init(&r, o.p, o.len);
    if (ne > 64 || (ne && i < n && d[i] % 5 == 0)) {
        FZ(sftp_attr_get(&r, &b) < 0);
    } else {
        FZ(sftp_attr_get(&r, &b) == 0 && r.off == o.len && a_eq(&a, &b));
    }
    sshb_free(&o);
}

static void m_longname(const uint8_t* d, size_t n)
{
    static const char TY[] = "?pc?d?b?-?l?s???";
    sftp_attr_t a;
    sshb_t o = { 0 };
    size_t i = 0, nl;
    char mode[11], want[200], when[32];
    unsigned uid, gid;
    unsigned long long sz;
    int k, used = 0;
    uint32_t p;
    struct tm tmv;
    time_t t;
    memset(&a, 0, sizeof a);
    a.perm = rd32(d, n, &i);
    a.uid = rd32(d, n, &i);
    a.gid = rd32(d, n, &i);
    a.size = (uint64_t)rd32(d, n, &i) << 32 | rd32(d, n, &i);
    a.mtime = rd32(d, n, &i);
    nl = n - (i < n ? i : n);
    sftp_longname(&a, (const char*)d + (i < n ? i : n), nl, &o);
    p = a.perm;
    mode[0] = TY[(p >> 12) & 15] == '?' ? '-' : TY[(p >> 12) & 15];
    for (k = 0; k < 9; k++)
        mode[1 + k] = (p >> (8 - k)) & 1 ? "rwxrwxrwx"[k] : '-';
    if (p & 04000)
        mode[3] = mode[3] == 'x' ? 's' : 'S';
    if (p & 02000)
        mode[6] = mode[6] == 'x' ? 's' : 'S';
    if (p & 01000)
        mode[9] = mode[9] == 'x' ? 't' : 'T';
    mode[10] = 0;
    FZ(o.len >= 10 + nl && !memcmp(o.p, mode, 10));
    FZ(nl == 0 || !memcmp(o.p + o.len - nl, d + i, nl));
    memcpy(want, o.p, o.len - nl < sizeof want - 1 ? o.len - nl : sizeof want - 1);
    want[o.len - nl < sizeof want - 1 ? o.len - nl : sizeof want - 1] = 0;
    FZ(sscanf(want + 10, "    1 %u %u %llu %n", &uid, &gid, &sz, &used) == 3);
    FZ(uid == a.uid && gid == a.gid && sz == a.size);
    t = (time_t)a.mtime;
    if (gmtime_r(&t, &tmv) && strftime(when, sizeof when, "%b %e %H:%M", &tmv))
        FZ(strlen(want) >= strlen(when) + 1 && !memcmp(want + strlen(want) - strlen(when) - 1, when, strlen(when)));
    sshb_free(&o);
}

static void m_misc(const uint8_t* d, size_t n)
{
    static const char* const names[] = { "OK", "EOF", "NO_SUCH_FILE", "PERMISSION_DENIED", "FAILURE", "BAD_MESSAGE", "NO_CONNECTION",
        "CONNECTION_LOST", "OP_UNSUPPORTED" };
    size_t i = 0;
    uint32_t code = rd32(d, n, &i);
    sshb_t b = { 0 };
    FZ(!strcmp(sftp_status_name(code), code < 9 ? names[code] : "FAILURE"));
    FZ(sftp_pkt_begin(&b, (uint8_t)code) == 0 && b.len == 5 && b.p[4] == (uint8_t)code);
    sshb_put(&b, d + (i < n ? i : n), n - (i < n ? i : n));
    sftp_pkt_end(&b);
    FZ(nbe32(b.p) == b.len - 4);
    sshb_free(&b);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 1)
        return 0;
    switch (data[0] % 4) {
    case 0:
        m_raw(data + 1, size - 1);
        break;
    case 1:
        m_gen(data + 1, size - 1);
        break;
    case 2:
        m_longname(data + 1, size - 1);
        break;
    default:
        m_misc(data + 1, size - 1);
        break;
    }
    return 0;
}
