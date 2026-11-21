#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "dyn-persist.h"
#include "dyn-cdefs.h"
#include "dyn-hash.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

#define PF_IOV_BATCH (IOV_MAX < 512 ? IOV_MAX : 512)
#define PF_IO_CHUNK ((size_t)1 << 30)
#define PF_KEEP_MAX ((size_t)64 << 20)
#define PF_MAX_FILE ((uint64_t)1 << 46)

static const uint8_t pf_magic[8] = { 'D', 'Y', 'N', 'A', 'P', 'F', '\n', 0x1a };
static const uint8_t pf_zero[DYN_PERSIST_PAGE];

static int pf_fail(dyn_persist_err* e, int code, int err, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int pf_fail(dyn_persist_err* e, int code, int err, const char* fmt, ...)
{
    if (e) {
        va_list ap;
        e->code = code;
        e->sys_errno = err;
        va_start(ap, fmt);
        vsnprintf(e->msg, sizeof e->msg, fmt, ap);
        va_end(ap);
    }
    return code;
}

const char* dyn_persist_strerror(int code)
{
    switch (code) {
    case DYN_PERSIST_OK:
        return "ok";
    case DYN_PERSIST_E_IO:
        return "I/O error";
    case DYN_PERSIST_E_NOMEM:
        return "out of memory";
    case DYN_PERSIST_E_MAGIC:
        return "not a dyna persist file";
    case DYN_PERSIST_E_VERSION:
        return "unsupported persist format version";
    case DYN_PERSIST_E_ENDIAN:
        return "persist file has foreign byte order";
    case DYN_PERSIST_E_TRUNCATED:
        return "persist file is truncated";
    case DYN_PERSIST_E_CORRUPT:
        return "persist file is malformed";
    case DYN_PERSIST_E_CHECKSUM:
        return "persist file checksum mismatch";
    case DYN_PERSIST_E_TYPE:
        return "persist file holds another structure type";
    case DYN_PERSIST_E_ARG:
        return "invalid argument";
    case DYN_PERSIST_E_TOO_LARGE:
        return "persist file too large";
    case DYN_PERSIST_E_MISSING:
        return "persist file lacks a required section";
    default:
        return "unknown persist error";
    }
}

const char* dyn_persist_code_name(int code)
{
    switch (code) {
    case DYN_PERSIST_E_IO:
        return "EPERSIST_IO";
    case DYN_PERSIST_E_NOMEM:
        return "EPERSIST_NOMEM";
    case DYN_PERSIST_E_MAGIC:
        return "EPERSIST_MAGIC";
    case DYN_PERSIST_E_VERSION:
        return "EPERSIST_VERSION";
    case DYN_PERSIST_E_ENDIAN:
        return "EPERSIST_ENDIAN";
    case DYN_PERSIST_E_TRUNCATED:
        return "EPERSIST_TRUNCATED";
    case DYN_PERSIST_E_CORRUPT:
        return "EPERSIST_CORRUPT";
    case DYN_PERSIST_E_CHECKSUM:
        return "EPERSIST_CHECKSUM";
    case DYN_PERSIST_E_TYPE:
        return "EPERSIST_TYPE";
    case DYN_PERSIST_E_ARG:
        return "EPERSIST_ARG";
    case DYN_PERSIST_E_TOO_LARGE:
        return "EPERSIST_TOO_LARGE";
    case DYN_PERSIST_E_MISSING:
        return "EPERSIST_MISSING";
    default:
        return "EPERSIST";
    }
}

static void st32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void st64(uint8_t* p, uint64_t v)
{
    st32(p, (uint32_t)v);
    st32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t ld32(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t ld64(const uint8_t* p)
{
    return (uint64_t)ld32(p) | (uint64_t)ld32(p + 4) << 32;
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

size_t dyn_persist_host_page(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096;
}

void dyn_persist_w_init(dyn_persist_w* w)
{
    memset(w, 0, sizeof *w);
    w->open = -1;
}

void dyn_persist_w_reset(dyn_persist_w* w)
{
    uint8_t* buf = w->buf;
    size_t cap = w->cap;
    uint8_t* xbuf = w->xbuf;
    size_t xcap = w->xcap;
    dyn_persist_sect* s = w->s;
    uint32_t scap = w->scap;
    void* iov = w->iov;
    uint32_t iovcap = w->iovcap;
    if (cap > PF_KEEP_MAX) {
        free(buf);
        buf = NULL;
        cap = 0;
    }
    if (xcap > PF_KEEP_MAX) {
        free(xbuf);
        xbuf = NULL;
        xcap = 0;
    }
    memset(w, 0, sizeof *w);
    w->buf = buf;
    w->cap = cap;
    w->xbuf = xbuf;
    w->xcap = xcap;
    w->s = s;
    w->scap = scap;
    w->iov = iov;
    w->iovcap = iovcap;
    w->open = -1;
}

void dyn_persist_w_free(dyn_persist_w* w)
{
    free(w->buf);
    free(w->xbuf);
    free(w->s);
    free(w->iov);
    dyn_persist_w_init(w);
}

static pthread_key_t pf_tls_key;
static pthread_once_t pf_tls_once = PTHREAD_ONCE_INIT;

static void pf_tls_free(void* p)
{
    dyn_persist_w_free((dyn_persist_w*)p);
    free(p);
}

static void pf_tls_init(void)
{
    if (pthread_key_create(&pf_tls_key, pf_tls_free) != 0)
        pf_tls_key = (pthread_key_t)-1;
}

dyn_persist_w* dyn_persist_w_thread(void)
{
    dyn_persist_w* w;
    pthread_once(&pf_tls_once, pf_tls_init);
    w = (dyn_persist_w*)pthread_getspecific(pf_tls_key);
    if (!w) {
        w = (dyn_persist_w*)malloc(sizeof *w);
        if (!w)
            return NULL;
        dyn_persist_w_init(w);
        if (pthread_setspecific(pf_tls_key, w) != 0) {
            free(w);
            return NULL;
        }
    }
    dyn_persist_w_reset(w);
    return w;
}

static int buf_reserve(dyn_persist_w* w, size_t extra)
{
    size_t need, nc;
    uint8_t* nb;
    if (extra > SIZE_MAX - w->len)
        return -1;
    need = w->len + extra;
    if (need <= w->cap)
        return 0;
    nc = w->cap ? w->cap : 4096;
    while (nc < need) {
        if (nc > SIZE_MAX / 2)
            return -1;
        nc *= 2;
    }
    nb = (uint8_t*)realloc(w->buf, nc);
    if (!nb)
        return -1;
    w->buf = nb;
    w->cap = nc;
    return 0;
}

int dyn_persist_w_begin(dyn_persist_w* w, uint32_t type_id, uint64_t count)
{
    w->type_id = type_id;
    w->count = count;
    return w->err ? -1 : 0;
}

void dyn_persist_w_param(dyn_persist_w* w, unsigned i, uint64_t v)
{
    if (i < DYN_PERSIST_NPARAMS)
        w->params[i] = v;
    else
        w->err = 1;
}

static dyn_persist_sect* sect_push(dyn_persist_w* w)
{
    dyn_persist_sect* s;
    if (w->err || w->open >= 0 || w->ns >= DYN_PERSIST_MAX_SECTIONS) {
        w->err = 1;
        return NULL;
    }
    if (w->ns == w->scap) {
        uint32_t nc = w->scap ? w->scap * 2 : 16;
        dyn_persist_sect* ns = (dyn_persist_sect*)realloc(w->s, (size_t)nc * sizeof *ns);
        if (!ns) {
            w->err = 1;
            return NULL;
        }
        w->s = ns;
        w->scap = nc;
    }
    s = &w->s[w->ns++];
    memset(s, 0, sizeof *s);
    return s;
}

static int sect_check(uint32_t elem, uint32_t flags, uint64_t len)
{
    if (elem == 0 || elem > DYN_PERSIST_MAX_ELEM || (flags & ~DYN_PERSIST_S_KNOWN))
        return -1;
    if (len % elem || len > PF_MAX_FILE)
        return -1;
    return 0;
}

int dyn_persist_w_ref_dirty(dyn_persist_w* w, uint32_t kind, uint32_t tag,
    uint32_t elem, uint32_t flags, const void* p, uint64_t len,
    const uint64_t* dirty, uint32_t dirty_shift)
{
    dyn_persist_sect* s;
    if (sect_check(elem, flags, len) < 0 || (len && !p) || dirty_shift > 40) {
        w->err = 1;
        return -1;
    }
    s = sect_push(w);
    if (!s)
        return -1;
    s->kind = kind;
    s->tag = tag;
    s->elem = elem;
    s->flags = flags;
    s->p = len ? p : pf_zero;
    s->len = len;
    s->dirty = dirty;
    s->dirty_shift = dirty_shift;
    return 0;
}

int dyn_persist_w_ref(dyn_persist_w* w, uint32_t kind, uint32_t tag,
    uint32_t elem, uint32_t flags, const void* p, uint64_t len)
{
    return dyn_persist_w_ref_dirty(w, kind, tag, elem, flags, p, len, NULL, 0);
}

int dyn_persist_w_open(dyn_persist_w* w, uint32_t kind, uint32_t tag,
    uint32_t elem, uint32_t flags)
{
    dyn_persist_sect* s;
    if (sect_check(elem, flags, 0) < 0) {
        w->err = 1;
        return -1;
    }
    s = sect_push(w);
    if (!s)
        return -1;
    s->kind = kind;
    s->tag = tag;
    s->elem = elem;
    s->flags = flags;
    s->p = NULL;
    s->boff = w->len;
    w->open = (int)(w->ns - 1);
    return 0;
}

int dyn_persist_w_put(dyn_persist_w* w, const void* p, size_t n)
{
    if (w->err || w->open < 0 || buf_reserve(w, n) < 0) {
        w->err = 1;
        return -1;
    }
    if (n)
        memcpy(w->buf + w->len, p, n);
    w->len += n;
    return 0;
}

int dyn_persist_w_close(dyn_persist_w* w)
{
    dyn_persist_sect* s;
    if (w->err || w->open < 0) {
        w->err = 1;
        return -1;
    }
    s = &w->s[w->open];
    s->len = (uint64_t)(w->len - s->boff);
    w->open = -1;
    if (s->len % s->elem) {
        w->err = 1;
        return -1;
    }
    return 0;
}

static const uint8_t* sect_data(const dyn_persist_w* w, const dyn_persist_sect* s)
{
    return s->p ? (const uint8_t*)s->p : w->buf + s->boff;
}

static uint64_t pf_lineage(void)
{
    uint64_t v[4];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    v[0] = (uint64_t)ts.tv_sec;
    v[1] = (uint64_t)ts.tv_nsec;
    v[2] = (uint64_t)getpid();
    v[3] = (uint64_t)(uintptr_t)&ts;
    v[0] = dyn_xxh64((const uint8_t*)v, sizeof v, 0x9E3779B97F4A7C15ULL);
    return v[0] ? v[0] : 1;
}

#ifdef DYN_PERSIST_FAILPOINTS
int dyn_persist_fp_countdown;
int dyn_persist_fp_torn;
int dyn_persist_fp_hits;
int dyn_persist_fp_powerloss;
int dyn_persist_fp_fullsyncs;

typedef struct {
    uint64_t off, len;
    uint8_t* pre;
} pf_jent;

static pf_jent* pf_j;
static size_t pf_jn, pf_jcap;

static int pf_fp(void)
{
    if (dyn_persist_fp_countdown > 0 && --dyn_persist_fp_countdown == 0) {
        dyn_persist_fp_hits++;
        return 1;
    }
    return 0;
}

static void pf_journal_clear(void)
{
    size_t i;
    for (i = 0; i < pf_jn; i++)
        free(pf_j[i].pre);
    free(pf_j);
    pf_j = NULL;
    pf_jn = pf_jcap = 0;
}

static void pf_journal(int fd, uint64_t off, uint64_t len)
{
    struct stat st;
    pf_jent* je;
    uint64_t have = 0;
    if (pf_jn == pf_jcap) {
        size_t nc = pf_jcap ? pf_jcap * 2 : 16;
        pf_jent* np = (pf_jent*)realloc(pf_j, nc * sizeof *np);
        if (!np)
            abort();
        pf_j = np;
        pf_jcap = nc;
    }
    je = &pf_j[pf_jn++];
    je->off = off;
    je->len = len;
    je->pre = (uint8_t*)calloc(1, len ? (size_t)len : 1);
    if (!je->pre)
        abort();
    if (fstat(fd, &st) == 0 && (uint64_t)st.st_size > off)
        have = (uint64_t)st.st_size - off < len ? (uint64_t)st.st_size - off : len;
    if (have && pread(fd, je->pre, (size_t)have, (off_t)off) != (ssize_t)have)
        abort();
}

static void pf_powerloss(int fd)
{
    size_t i;
    if (dyn_persist_fp_powerloss && pf_jn > 1)
        for (i = pf_jn - 1; i-- > 0;)
            if (pwrite(fd, pf_j[i].pre, (size_t)pf_j[i].len, (off_t)pf_j[i].off) != (ssize_t)pf_j[i].len)
                abort();
    pf_journal_clear();
}
#else
static int pf_fp(void)
{
    return 0;
}

static void pf_journal_clear(void)
{
}

static void pf_journal(int fd, uint64_t off, uint64_t len)
{
    (void)fd;
    (void)off;
    (void)len;
}

static void pf_powerloss(int fd)
{
    (void)fd;
}
#endif

#define PF_CRASH -100

static int pf_sync_fd(int fd)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0) {
#ifdef DYN_PERSIST_FAILPOINTS
        dyn_persist_fp_fullsyncs++;
#endif
        return 0;
    }
#endif
    return fsync(fd);
}

static int pf_torn(void)
{
#ifdef DYN_PERSIST_FAILPOINTS
    return dyn_persist_fp_torn;
#else
    return 0;
#endif
}

static uint32_t slot_size_for(uint32_t nsect)
{
    return (uint32_t)align_up(DYN_PERSIST_HDR + (uint64_t)nsect * DYN_PERSIST_SEC, DYN_PERSIST_ALIGN);
}

static void pf_super_encode(uint8_t* h, uint32_t slot_size)
{
    memset(h, 0, DYN_PERSIST_SUPER);
    memcpy(h, pf_magic, 8);
    st32(h + 8, DYN_PERSIST_VERSION);
    memcpy(h + 12, &(uint32_t) { DYN_PERSIST_ENDIAN_TAG }, 4);
    st32(h + 16, slot_size);
    st32(h + 60, dyn_crc32c(h, 60));
}

typedef struct {
    uint32_t slot;
    uint64_t gen, file_size, base_end, ov_off;
    uint32_t ov_count, ov_crc;
    const uint64_t* offs;
} pf_slot_info;

static void pf_slot_encode(uint8_t* h, const dyn_persist_w* w, const pf_slot_info* si)
{
    uint32_t i;
    size_t hlen = DYN_PERSIST_HDR + (size_t)w->ns * DYN_PERSIST_SEC;
    memset(h, 0, w->slot_size);
    memcpy(h, pf_magic, 8);
    st32(h + 8, DYN_PERSIST_VERSION);
    memcpy(h + 12, &(uint32_t) { DYN_PERSIST_ENDIAN_TAG }, 4);
    st32(h + 16, w->type_id);
    st32(h + 20, w->flags);
    st32(h + 24, w->ns);
    st32(h + 28, DYN_PERSIST_PAGE);
    st64(h + 32, w->count);
    st64(h + 40, si->file_size);
    st64(h + 48, w->lineage);
    st64(h + 56, si->gen);
    for (i = 0; i < DYN_PERSIST_NPARAMS; i++)
        st64(h + 64 + 8 * i, w->params[i]);
    st32(h + 132, si->slot);
    st64(h + 136, si->base_end);
    st64(h + 144, si->ov_off);
    st32(h + 152, si->ov_count);
    st32(h + 156, si->ov_crc);
    for (i = 0; i < w->ns; i++) {
        const dyn_persist_sect* s = &w->s[i];
        uint8_t* t = h + DYN_PERSIST_HDR + (size_t)i * DYN_PERSIST_SEC;
        st32(t, s->kind);
        st32(t + 4, s->tag);
        st32(t + 8, s->elem);
        st32(t + 12, s->flags);
        st64(t + 16, si->offs ? si->offs[i] : s->off);
        st64(t + 24, s->len);
        st32(t + 32, s->crc);
        st32(t + 36, 0);
    }
    st32(h + 128, dyn_crc32c(h, hlen));
}

static uint32_t pf_empty_crc(void)
{
    return dyn_crc32c(pf_zero, 0);
}

int dyn_persist_w_layout(dyn_persist_w* w)
{
    uint64_t off;
    uint32_t i;
    pf_slot_info si;
    if (w->err || w->open >= 0)
        return -1;
    w->slot_size = slot_size_for(w->ns);
    off = DYN_PERSIST_SUPER + 2 * (uint64_t)w->slot_size;
    for (i = 0; i < w->ns; i++) {
        dyn_persist_sect* s = &w->s[i];
        uint64_t a = (s->flags & DYN_PERSIST_S_PAGE) ? DYN_PERSIST_PAGE : DYN_PERSIST_ALIGN;
        off = align_up(off, a);
        s->off = off;
        off += s->len;
        if (s->flags & DYN_PERSIST_S_PAGE)
            off = align_up(off, DYN_PERSIST_PAGE);
        if (off > PF_MAX_FILE) {
            w->err = 1;
            return -1;
        }
        s->crc = dyn_crc32c(sect_data(w, s), (size_t)s->len);
    }
    w->file_size = off;
    if (!w->lineage)
        w->lineage = pf_lineage();
    w->hdr_off = (size_t)align_up(w->len, 8);
    w->hdr_len = DYN_PERSIST_SUPER + (size_t)w->slot_size;
    if (buf_reserve(w, (w->hdr_off - w->len) + w->hdr_len) < 0) {
        w->err = 1;
        return -1;
    }
    pf_super_encode(w->buf + w->hdr_off, w->slot_size);
    memset(&si, 0, sizeof si);
    si.slot = 0;
    si.gen = w->gen + 1;
    si.file_size = w->file_size;
    si.base_end = w->file_size;
    si.ov_crc = pf_empty_crc();
    pf_slot_encode(w->buf + w->hdr_off + DYN_PERSIST_SUPER, w, &si);
    return 0;
}

int dyn_persist_w_image(dyn_persist_w* w, uint8_t** out, size_t* len)
{
    uint8_t* img;
    uint32_t i;
    *out = NULL;
    *len = 0;
    if (dyn_persist_w_layout(w) < 0)
        return -1;
    if (w->file_size > SIZE_MAX)
        return -1;
    img = (uint8_t*)calloc(1, (size_t)w->file_size);
    if (!img)
        return -1;
    memcpy(img, w->buf + w->hdr_off, w->hdr_len);
    for (i = 0; i < w->ns; i++)
        if (w->s[i].len)
            memcpy(img + w->s[i].off, sect_data(w, &w->s[i]), (size_t)w->s[i].len);
    *out = img;
    *len = (size_t)w->file_size;
    return 0;
}

typedef struct {
    int fd;
    struct iovec* iov;
    uint32_t n, cap;
    uint64_t pos;
    dyn_persist_err* e;
} pf_out;

static int out_flush(pf_out* o)
{
    struct iovec* v = o->iov;
    uint32_t n = o->n;
    if (n && pf_fp()) {
        if (pf_torn() && v->iov_len > 1) {
            ssize_t r = write(o->fd, v->iov_base, v->iov_len / 2);
            (void)r;
        }
        pf_fail(o->e, DYN_PERSIST_E_IO, 0, "failpoint");
        return PF_CRASH;
    }
    while (n) {
        ssize_t r = writev(o->fd, v, (int)n);
        size_t got;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return pf_fail(o->e, DYN_PERSIST_E_IO, errno, "write failed: %s", strerror(errno));
        }
        got = (size_t)r;
        o->pos += got;
        while (n && got >= v->iov_len) {
            got -= v->iov_len;
            v++;
            n--;
        }
        if (n && got) {
            v->iov_base = (uint8_t*)v->iov_base + got;
            v->iov_len -= got;
        }
    }
    o->n = 0;
    return 0;
}

static int out_add(pf_out* o, const void* p, uint64_t len)
{
    while (len) {
        size_t c = len > PF_IO_CHUNK ? PF_IO_CHUNK : (size_t)len;
        if (o->n == o->cap) {
            int rc = out_flush(o);
            if (rc < 0)
                return rc;
        }
        o->iov[o->n].iov_base = DYN_UNCONST(p);
        o->iov[o->n].iov_len = c;
        o->n++;
        p = (const uint8_t*)p + c;
        len -= c;
    }
    return 0;
}

static int out_pad(pf_out* o, uint64_t cur, uint64_t to)
{
    while (cur < to) {
        uint64_t c = to - cur;
        int rc;
        if (c > sizeof pf_zero)
            c = sizeof pf_zero;
        rc = out_add(o, pf_zero, c);
        if (rc < 0)
            return rc;
        cur += c;
    }
    return 0;
}

static int pf_fsync_dir(const char* path)
{
    char* dir;
    const char* slash = strrchr(path, '/');
    int fd, rc;
    if (!slash)
        dir = strdup(".");
    else if (slash == path)
        dir = strdup("/");
    else
        dir = strndup(path, (size_t)(slash - path));
    if (!dir)
        return -1;
    fd = open(dir, O_RDONLY | O_CLOEXEC);
    free(dir);
    if (fd < 0)
        return -1;
    rc = pf_sync_fd(fd);
    close(fd);
    return rc;
}

static int pf_write_all(dyn_persist_w* w, int fd, dyn_persist_err* e)
{
    pf_out o;
    uint64_t cur;
    uint32_t i;
    int rc;
    if (w->iovcap < PF_IOV_BATCH) {
        void* nv = realloc(w->iov, (size_t)PF_IOV_BATCH * sizeof(struct iovec));
        if (!nv)
            return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory");
        w->iov = nv;
        w->iovcap = PF_IOV_BATCH;
    }
    memset(&o, 0, sizeof o);
    o.fd = fd;
    o.iov = (struct iovec*)w->iov;
    o.cap = w->iovcap;
    o.e = e;
    if ((rc = out_add(&o, w->buf + w->hdr_off, w->hdr_len)) < 0)
        return rc;
    cur = w->hdr_len;
    for (i = 0; i < w->ns; i++) {
        const dyn_persist_sect* s = &w->s[i];
        if ((rc = out_pad(&o, cur, s->off)) < 0 || (rc = out_add(&o, sect_data(w, s), s->len)) < 0)
            return rc;
        cur = s->off + s->len;
    }
    if ((rc = out_pad(&o, cur, w->file_size)) < 0 || (rc = out_flush(&o)) < 0)
        return rc;
    if (o.pos != w->file_size)
        return pf_fail(e, DYN_PERSIST_E_IO, 0, "short write");
    return 0;
}

static int pf_pwrite_all(int fd, const uint8_t* p, uint64_t len, uint64_t off,
    dyn_persist_err* e)
{
    if (len && pf_fp()) {
        if (pf_torn() && len > 1) {
            size_t half = (size_t)(len / 2 > PF_IO_CHUNK ? PF_IO_CHUNK : len / 2);
            pf_journal(fd, off, half);
            if (pwrite(fd, p, half, (off_t)off) < 0)
                return pf_fail(e, DYN_PERSIST_E_IO, errno, "write failed: %s", strerror(errno));
        }
        pf_powerloss(fd);
        pf_fail(e, DYN_PERSIST_E_IO, 0, "failpoint");
        return PF_CRASH;
    }
    if (len)
        pf_journal(fd, off, len);
    while (len) {
        size_t c = len > PF_IO_CHUNK ? PF_IO_CHUNK : (size_t)len;
        ssize_t r = pwrite(fd, p, c, (off_t)off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return pf_fail(e, DYN_PERSIST_E_IO, errno, "write failed: %s", strerror(errno));
        }
        p += r;
        off += (uint64_t)r;
        len -= (uint64_t)r;
    }
    return 0;
}

static int pf_pzero(int fd, uint64_t from, uint64_t to, dyn_persist_err* e)
{
    while (from < to) {
        uint64_t c = to - from;
        int rc;
        if (c > sizeof pf_zero)
            c = sizeof pf_zero;
        rc = pf_pwrite_all(fd, pf_zero, c, from, e);
        if (rc < 0)
            return rc;
        from += c;
    }
    return 0;
}

static int pf_fsync(int fd, dyn_persist_err* e)
{
    if (pf_fp()) {
        pf_powerloss(fd);
        pf_fail(e, DYN_PERSIST_E_IO, 0, "failpoint");
        return PF_CRASH;
    }
    if (pf_sync_fd(fd) < 0)
        return pf_fail(e, DYN_PERSIST_E_IO, errno, "fsync failed: %s", strerror(errno));
    pf_journal_clear();
    return 0;
}

static int pf_pread_all(int fd, uint8_t* p, size_t len, uint64_t off)
{
    while (len) {
        ssize_t r = pread(fd, p, len, (off_t)off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return -1;
        p += r;
        off += (uint64_t)r;
        len -= (size_t)r;
    }
    return 0;
}

typedef struct {
    uint32_t slot_size, slot, nsect, type_id, flags;
    uint64_t count, file_size, lineage, gen, base_end, ov_off;
    uint32_t ov_count, ov_crc;
    uint64_t params[DYN_PERSIST_NPARAMS];
    const uint8_t* table;
} pf_hdr;

static int pf_slot_ok(const uint8_t* h, uint32_t slot_size, uint32_t idx)
{
    uint32_t nsect = ld32(h + 24), want, got;
    size_t hlen;
    uint8_t* cp;
    if (memcmp(h, pf_magic, 8) != 0 || ld32(h + 8) != DYN_PERSIST_VERSION || ld32(h + 132) != idx)
        return 0;
    {
        uint32_t endian;
        memcpy(&endian, h + 12, 4);
        if (endian != DYN_PERSIST_ENDIAN_TAG)
            return 0;
    }
    if (nsect > DYN_PERSIST_MAX_SECTIONS || DYN_PERSIST_HDR + (uint64_t)nsect * DYN_PERSIST_SEC > slot_size)
        return 0;
    hlen = DYN_PERSIST_HDR + (size_t)nsect * DYN_PERSIST_SEC;
    cp = (uint8_t*)malloc(hlen);
    if (!cp)
        return -1;
    memcpy(cp, h, hlen);
    want = ld32(cp + 128);
    st32(cp + 128, 0);
    got = dyn_crc32c(cp, hlen);
    free(cp);
    return got == want;
}

static int pf_parse(const uint8_t* h, size_t avail, uint64_t fsize,
    uint32_t expect_type, pf_hdr* out, dyn_persist_err* e)
{
    uint32_t slot_size, i, best = 2, endian;
    uint64_t best_gen = 0, data_start;
    const uint8_t* s;
    if (avail < 8 || memcmp(h, pf_magic, 8) != 0)
        return pf_fail(e, DYN_PERSIST_E_MAGIC, 0, "not a dyna persist file (bad magic)");
    if (avail < DYN_PERSIST_SUPER)
        return pf_fail(e, DYN_PERSIST_E_TRUNCATED, 0, "truncated superblock (%zu bytes)", avail);
    if (ld32(h + 8) != DYN_PERSIST_VERSION)
        return pf_fail(e, DYN_PERSIST_E_VERSION, 0, "format version %u, this build reads %u",
            (unsigned)ld32(h + 8), DYN_PERSIST_VERSION);
    memcpy(&endian, h + 12, 4);
    if (endian != DYN_PERSIST_ENDIAN_TAG)
        return pf_fail(e, DYN_PERSIST_E_ENDIAN, 0, "file was written with a foreign byte order");
    if (dyn_crc32c(h, 60) != ld32(h + 60))
        return pf_fail(e, DYN_PERSIST_E_CHECKSUM, 0, "superblock checksum mismatch");
    slot_size = ld32(h + 16);
    for (i = 20; i < 60; i++)
        if (h[i])
            return pf_fail(e, DYN_PERSIST_E_CORRUPT, 0, "superblock reserved bytes are not zero");
    if (slot_size < DYN_PERSIST_HDR || slot_size % DYN_PERSIST_ALIGN || slot_size > slot_size_for(DYN_PERSIST_MAX_SECTIONS))
        return pf_fail(e, DYN_PERSIST_E_CORRUPT, 0, "bad header slot size %u", (unsigned)slot_size);
    for (i = 0; i < 2; i++) {
        uint64_t so = DYN_PERSIST_SUPER + (uint64_t)i * slot_size;
        int ok;
        if (so + slot_size > avail)
            continue;
        ok = pf_slot_ok(h + so, slot_size, i);
        if (ok < 0)
            return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory");
        if (ok && (best == 2 || ld64(h + so + 56) > best_gen)) {
            best = i;
            best_gen = ld64(h + so + 56);
        }
    }
    if (best == 2) {
        if (DYN_PERSIST_SUPER + (uint64_t)slot_size > avail)
            return pf_fail(e, DYN_PERSIST_E_TRUNCATED, 0, "truncated header slot");
        return pf_fail(e, DYN_PERSIST_E_CHECKSUM, 0, "no intact header slot (checksum mismatch)");
    }
    s = h + DYN_PERSIST_SUPER + (size_t)best * slot_size;
    memset(out, 0, sizeof *out);
    out->slot_size = slot_size;
    out->slot = best;
    out->type_id = ld32(s + 16);
    out->flags = ld32(s + 20);
    out->nsect = ld32(s + 24);
    out->count = ld64(s + 32);
    out->file_size = ld64(s + 40);
    out->lineage = ld64(s + 48);
    out->gen = ld64(s + 56);
    for (i = 0; i < DYN_PERSIST_NPARAMS; i++)
        out->params[i] = ld64(s + 64 + 8 * i);
    out->base_end = ld64(s + 136);
    out->ov_off = ld64(s + 144);
    out->ov_count = ld32(s + 152);
    out->ov_crc = ld32(s + 156);
    out->table = s + DYN_PERSIST_HDR;
    if (expect_type && out->type_id != expect_type)
        return pf_fail(e, DYN_PERSIST_E_TYPE, 0, "file holds type %u, expected %u",
            (unsigned)out->type_id, (unsigned)expect_type);
    if (ld32(s + 28) != DYN_PERSIST_PAGE)
        return pf_fail(e, DYN_PERSIST_E_CORRUPT, 0, "bad page size");
    if (out->file_size > fsize)
        return pf_fail(e, DYN_PERSIST_E_TRUNCATED, 0, "file is %llu bytes, header says %llu",
            (unsigned long long)fsize, (unsigned long long)out->file_size);
    data_start = DYN_PERSIST_SUPER + 2 * (uint64_t)slot_size;
    if (out->base_end < data_start || out->base_end > out->file_size)
        return pf_fail(e, DYN_PERSIST_E_CORRUPT, 0, "bad base extent");
    if (out->ov_count > DYN_PERSIST_MAX_OVERLAYS
        || (out->ov_count && (out->ov_off % DYN_PERSIST_ALIGN || out->ov_off < out->base_end || out->ov_off > out->file_size || (uint64_t)out->ov_count * DYN_PERSIST_OV > out->file_size - out->ov_off)))
        return pf_fail(e, DYN_PERSIST_E_CORRUPT, 0, "bad overlay table extent");
    for (i = 0; i < out->nsect; i++) {
        const uint8_t* t = out->table + (size_t)i * DYN_PERSIST_SEC;
        uint32_t elem = ld32(t + 8), flags = ld32(t + 12);
        uint64_t off = ld64(t + 16), len = ld64(t + 24);
        if ((flags & ~DYN_PERSIST_S_KNOWN) || ld32(t + 36) != 0 || elem == 0 || elem > DYN_PERSIST_MAX_ELEM
            || off % DYN_PERSIST_ALIGN || ((flags & DYN_PERSIST_S_PAGE) && off % DYN_PERSIST_PAGE)
            || off < data_start || off > out->file_size || len > out->file_size - off || len % elem
            || ((flags & DYN_PERSIST_S_PAGE) && align_up(off + len, DYN_PERSIST_PAGE) > out->file_size))
            return pf_fail(e, DYN_PERSIST_E_CORRUPT, 0, "section %u has bad bounds or fields", (unsigned)i);
    }
    return 0;
}

static int pf_fill(const pf_hdr* hd, const uint8_t* ovt, dyn_persist_r* r,
    dyn_persist_err* e)
{
    uint32_t i;
    if (dyn_crc32c(ovt ? ovt : pf_zero, (size_t)hd->ov_count * DYN_PERSIST_OV) != hd->ov_crc)
        return pf_fail(e, DYN_PERSIST_E_CHECKSUM, 0, "overlay table checksum mismatch");
    r->type_id = hd->type_id;
    r->flags = hd->flags;
    r->nsect = hd->nsect;
    r->page = DYN_PERSIST_PAGE;
    r->slot = hd->slot;
    r->slot_size = hd->slot_size;
    r->count = hd->count;
    r->lineage = hd->lineage;
    r->gen = hd->gen;
    r->file_size = hd->file_size;
    r->base_end = hd->base_end;
    memcpy(r->params, hd->params, sizeof r->params);
    if (hd->nsect) {
        r->s = (dyn_persist_rsect*)calloc(hd->nsect, sizeof *r->s);
        if (!r->s)
            return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory");
    }
    for (i = 0; i < hd->nsect; i++) {
        const uint8_t* t = hd->table + (size_t)i * DYN_PERSIST_SEC;
        dyn_persist_rsect* s = &r->s[i];
        s->kind = ld32(t);
        s->tag = ld32(t + 4);
        s->elem = ld32(t + 8);
        s->flags = ld32(t + 12);
        s->off = ld64(t + 16);
        s->len = ld64(t + 24);
        s->crc = ld32(t + 32);
    }
    if (hd->ov_count) {
        r->ov = (dyn_persist_ov*)calloc(hd->ov_count, sizeof *r->ov);
        if (!r->ov)
            return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory");
    }
    for (i = 0; i < hd->ov_count; i++) {
        const uint8_t* t = ovt + (size_t)i * DYN_PERSIST_OV;
        dyn_persist_ov* o = &r->ov[i];
        const dyn_persist_rsect* s;
        o->sect = ld32(t);
        o->chunk = ld32(t + 4);
        o->src = ld64(t + 8);
        if (o->sect >= hd->nsect)
            goto bad;
        s = &r->s[o->sect];
        if (!(s->flags & DYN_PERSIST_S_PAGE) || align_up(s->off + s->len, DYN_PERSIST_PAGE) > hd->base_end
            || o->chunk >= dyn_persist_chunks(s->len, 14) || o->src % DYN_PERSIST_PAGE
            || o->src < hd->base_end || o->src > hd->file_size || hd->file_size - o->src < DYN_PERSIST_PAGE)
            goto bad;
        r->s[o->sect].noverlay++;
    }
    r->nov = hd->ov_count;
    return 0;
bad:
    return pf_fail(e, DYN_PERSIST_E_CORRUPT, 0, "overlay %u has bad bounds", (unsigned)i);
}

void dyn_persist_r_clear(dyn_persist_r* r)
{
    free(r->s);
    free(r->ov);
    r->s = NULL;
    r->ov = NULL;
    r->nov = 0;
}

static uint64_t ov_len(const dyn_persist_r* r, const dyn_persist_ov* o)
{
    const dyn_persist_rsect* s = &r->s[o->sect];
    uint64_t lo = (uint64_t)o->chunk * DYN_PERSIST_PAGE;
    return s->len - lo < DYN_PERSIST_PAGE ? s->len - lo : DYN_PERSIST_PAGE;
}

static void pf_compose_mem(dyn_persist_r* r, uint8_t* base)
{
    uint32_t i;
    for (i = 0; i < r->nov; i++) {
        const dyn_persist_ov* o = &r->ov[i];
        memcpy(base + r->s[o->sect].off + (uint64_t)o->chunk * DYN_PERSIST_PAGE, base + o->src,
            (size_t)ov_len(r, o));
    }
}

static int pf_verify(const dyn_persist_r* r, const uint8_t* base, dyn_persist_err* e)
{
    uint32_t i;
    for (i = 0; i < r->nsect; i++)
        if (dyn_crc32c(base + r->s[i].off, (size_t)r->s[i].len) != r->s[i].crc)
            return pf_fail(e, DYN_PERSIST_E_CHECKSUM, 0,
                "section %u (kind %u) checksum mismatch", (unsigned)i, (unsigned)r->s[i].kind);
    return 0;
}

int dyn_persist_validate(const uint8_t* buf, size_t size, int verify,
    uint32_t expect_type, dyn_persist_r* r, dyn_persist_err* e)
{
    pf_hdr hd;
    dyn_persist_r tmp;
    int rc;
    uint32_t i;
    if (!r)
        r = &tmp;
    memset(r, 0, sizeof *r);
    r->fd = -1;
    rc = pf_parse(buf, size, size, expect_type, &hd, e);
    if (rc == 0)
        rc = pf_fill(&hd, hd.ov_count ? buf + hd.ov_off : NULL, r, e);
    if (rc == 0 && verify) {
        if (r->nov) {
            uint8_t* cp = (uint8_t*)malloc(size);
            if (!cp)
                rc = pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory");
            else {
                memcpy(cp, buf, size);
                pf_compose_mem(r, cp);
                rc = pf_verify(r, cp, e);
                free(cp);
            }
        } else {
            rc = pf_verify(r, buf, e);
        }
    }
    if (rc == 0)
        for (i = 0; i < r->nsect; i++)
            r->s[i].p = buf + r->s[i].off;
    if (rc != 0 || r == &tmp)
        dyn_persist_r_clear(r);
    return rc;
}

static void pf_r_free(dyn_persist_r* r)
{
    if (r->base) {
        if (r->mode == DYN_PERSIST_MODE_MMAP)
            munmap(r->base, r->size ? r->size : 1);
        else
            free(r->base);
    }
    if (r->fd >= 0)
        close(r->fd);
    dyn_persist_r_clear(r);
    free(r);
}

static int pf_compose_map(dyn_persist_r* r, dyn_persist_err* e)
{
    uint32_t i;
    for (i = 0; i < r->nov; i++) {
        const dyn_persist_ov* o = &r->ov[i];
        uint8_t* at = r->base + r->s[o->sect].off + (uint64_t)o->chunk * DYN_PERSIST_PAGE;
        void* m = mmap(at, DYN_PERSIST_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, r->fd, (off_t)o->src);
        if (m != (void*)at)
            return pf_fail(e, DYN_PERSIST_E_IO, errno, "mmap of overlay failed: %s", strerror(errno));
    }
    return 0;
}

static int pf_finish_open(dyn_persist_r* r, int verify, uint32_t expect_type,
    dyn_persist_r** out, dyn_persist_err* e)
{
    pf_hdr hd;
    uint32_t i;
    int rc = pf_parse(r->base, r->size, r->size, expect_type, &hd, e);
    if (rc == 0)
        rc = pf_fill(&hd, hd.ov_count ? r->base + hd.ov_off : NULL, r, e);
    if (rc == 0 && r->nov)
        rc = r->mode == DYN_PERSIST_MODE_MMAP ? pf_compose_map(r, e) : (pf_compose_mem(r, r->base), 0);
    if (rc == 0 && verify)
        rc = pf_verify(r, r->base, e);
    if (rc != 0) {
        pf_r_free(r);
        return rc;
    }
    for (i = 0; i < r->nsect; i++)
        r->s[i].p = r->base + r->s[i].off;
    r->refs = 1;
    *out = r;
    return 0;
}

int dyn_persist_open(const char* path, int mode, int verify,
    uint32_t expect_type, dyn_persist_r** out, dyn_persist_err* e)
{
    int fd;
    struct stat st;
    size_t size;
    dyn_persist_r* r;
    if (e)
        memset(e, 0, sizeof *e);
    *out = NULL;
    if (mode != DYN_PERSIST_MODE_COPY && mode != DYN_PERSIST_MODE_MMAP)
        return pf_fail(e, DYN_PERSIST_E_ARG, 0, "bad mode");
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return pf_fail(e, DYN_PERSIST_E_IO, errno, "cannot open '%s': %s", path, strerror(errno));
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        int er = errno;
        close(fd);
        return pf_fail(e, DYN_PERSIST_E_IO, er, "'%s' is not a regular file", path);
    }
    if ((uint64_t)st.st_size > PF_MAX_FILE || (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
        close(fd);
        return pf_fail(e, DYN_PERSIST_E_TOO_LARGE, 0, "'%s' is too large", path);
    }
    size = (size_t)st.st_size;
    if (size < DYN_PERSIST_SUPER + DYN_PERSIST_HDR) {
        uint8_t small[DYN_PERSIST_SUPER + DYN_PERSIST_HDR];
        pf_hdr hd;
        int rc;
        if (size && pf_pread_all(fd, small, size, 0) < 0) {
            close(fd);
            return pf_fail(e, DYN_PERSIST_E_IO, errno, "read failed");
        }
        close(fd);
        rc = pf_parse(small, size, size, expect_type, &hd, e);
        return rc ? rc : pf_fail(e, DYN_PERSIST_E_TRUNCATED, 0, "truncated header");
    }
    r = (dyn_persist_r*)calloc(1, sizeof *r);
    if (!r) {
        close(fd);
        return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory");
    }
    r->fd = -1;
    r->mode = mode;
    r->size = size;
    if (mode == DYN_PERSIST_MODE_MMAP) {
        void* m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) {
            int er = errno;
            close(fd);
            free(r);
            return pf_fail(e, DYN_PERSIST_E_IO, er, "mmap failed: %s", strerror(er));
        }
        r->base = (uint8_t*)m;
        r->fd = fd;
    } else {
        void* m = NULL;
        size_t al = dyn_persist_host_page();
        if (posix_memalign(&m, al < DYN_PERSIST_PAGE ? al : DYN_PERSIST_PAGE, size) != 0 || !m) {
            close(fd);
            free(r);
            return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory reading %zu bytes", size);
        }
        r->base = (uint8_t*)m;
        if (pf_pread_all(fd, r->base, size, 0) < 0) {
            int er = errno;
            close(fd);
            free(r->base);
            free(r);
            return pf_fail(e, DYN_PERSIST_E_TRUNCATED, er, "'%s' shrank while being read", path);
        }
        close(fd);
    }
    return pf_finish_open(r, verify, expect_type, out, e);
}

int dyn_persist_from_bytes(const uint8_t* buf, size_t size, int verify,
    uint32_t expect_type, dyn_persist_r** out, dyn_persist_err* e)
{
    dyn_persist_r* r;
    if (e)
        memset(e, 0, sizeof *e);
    *out = NULL;
    r = (dyn_persist_r*)calloc(1, sizeof *r);
    if (!r)
        return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory");
    {
        void* m = NULL;
        size_t al = dyn_persist_host_page();
        if (posix_memalign(&m, al < DYN_PERSIST_PAGE ? al : DYN_PERSIST_PAGE, size ? size : 1) != 0 || !m) {
            free(r);
            return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory");
        }
        r->base = (uint8_t*)m;
    }
    if (size)
        memcpy(r->base, buf, size);
    r->fd = -1;
    r->mode = DYN_PERSIST_MODE_COPY;
    r->size = size;
    return pf_finish_open(r, verify, expect_type, out, e);
}

void dyn_persist_retain(dyn_persist_r* r)
{
    __atomic_add_fetch(&r->refs, 1, __ATOMIC_RELAXED);
}

void dyn_persist_release(dyn_persist_r* r)
{
    if (r && __atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL) == 0)
        pf_r_free(r);
}

const dyn_persist_rsect* dyn_persist_find(const dyn_persist_r* r,
    uint32_t kind, uint32_t tag)
{
    uint32_t i;
    for (i = 0; i < r->nsect; i++)
        if (r->s[i].kind == kind && r->s[i].tag == tag)
            return &r->s[i];
    return NULL;
}

int dyn_persist_map_section(const dyn_persist_r* r,
    const dyn_persist_rsect* s, void* addr, size_t cap)
{
    size_t hp = dyn_persist_host_page();
    uint64_t span;
    uint32_t idx, i;
    void* m;
    if (r->mode != DYN_PERSIST_MODE_MMAP || r->fd < 0 || !(s->flags & DYN_PERSIST_S_PAGE))
        return -1;
    if (s < r->s || s >= r->s + r->nsect)
        return -1;
    idx = (uint32_t)(s - r->s);
    if (s->off % hp || ((uintptr_t)addr) % hp || DYN_PERSIST_PAGE % hp)
        return -1;
    span = align_up(s->len, DYN_PERSIST_PAGE);
    if (!span || span > cap)
        return -1;
    m = mmap(addr, (size_t)span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, r->fd, (off_t)s->off);
    if (m != addr)
        return -1;
    for (i = 0; i < r->nov; i++) {
        const dyn_persist_ov* o = &r->ov[i];
        uint8_t* at;
        if (o->sect != idx)
            continue;
        at = (uint8_t*)addr + (uint64_t)o->chunk * DYN_PERSIST_PAGE;
        if (mmap(at, DYN_PERSIST_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, r->fd, (off_t)o->src) != (void*)at)
            return -1;
    }
    return 0;
}

static int pf_full_save(dyn_persist_w* w, const char* path,
    dyn_persist_save_opt* o, dyn_persist_err* e)
{
    static unsigned seq;
    char* tmp;
    size_t plen = strlen(path);
    int fd, rc;
    tmp = (char*)malloc(plen + 48);
    if (!tmp)
        return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "out of memory");
    snprintf(tmp, plen + 48, "%s.tmp.%ld.%u", path, (long)getpid(),
        __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED));
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        rc = pf_fail(e, DYN_PERSIST_E_IO, errno, "cannot create '%s': %s", tmp, strerror(errno));
        free(tmp);
        return rc;
    }
    if ((rc = pf_write_all(w, fd, e)) < 0)
        goto fail;
    if (o->fsync && (rc = pf_fsync(fd, e)) < 0)
        goto fail;
    if (close(fd) < 0) {
        fd = -1;
        rc = pf_fail(e, DYN_PERSIST_E_IO, errno, "close failed: %s", strerror(errno));
        goto fail;
    }
    fd = -1;
    if (pf_fp()) {
        rc = pf_fail(e, DYN_PERSIST_E_IO, 0, "failpoint");
        rc = PF_CRASH;
        goto fail;
    }
    if (rename(tmp, path) < 0) {
        rc = pf_fail(e, DYN_PERSIST_E_IO, errno, "cannot rename into '%s': %s", path, strerror(errno));
        goto fail;
    }
    if (o->fsync && pf_fp()) {
        free(tmp);
        return pf_fail(e, DYN_PERSIST_E_IO, 0, "failpoint");
    }
    if (o->fsync)
        pf_fsync_dir(path);
    free(tmp);
    o->bytes_written = w->file_size;
    o->file_size = w->file_size;
    w->gen++;
    return 0;
fail:
    if (fd >= 0)
        close(fd);
    if (rc != PF_CRASH)
        unlink(tmp);
    free(tmp);
    return e && e->code ? e->code : DYN_PERSIST_E_IO;
}

static int ov_cmp(const void* a, const void* b)
{
    const dyn_persist_ov *x = (const dyn_persist_ov*)a, *y = (const dyn_persist_ov*)b;
    if (x->sect != y->sect)
        return x->sect < y->sect ? -1 : 1;
    return x->chunk < y->chunk ? -1 : x->chunk > y->chunk;
}

static void mark_chunks16(const dyn_persist_sect* s, uint64_t* bits)
{
    uint64_t n = dyn_persist_chunks(s->len, s->dirty_shift), j;
    for (j = 0; j < n; j++) {
        uint64_t lo, hi, c;
        if (!(s->dirty[j >> 6] & ((uint64_t)1 << (j & 63))))
            continue;
        lo = j << s->dirty_shift;
        hi = lo + ((uint64_t)1 << s->dirty_shift);
        if (hi > s->len)
            hi = s->len;
        for (c = lo / DYN_PERSIST_PAGE; c * DYN_PERSIST_PAGE < hi; c++)
            bits[c >> 6] |= (uint64_t)1 << (c & 63);
    }
}

static int pf_try_incremental(dyn_persist_w* w, const char* path,
    dyn_persist_save_opt* o, dyn_persist_err* e)
{
    int fd, rc = 1;
    struct stat st;
    uint8_t sb[DYN_PERSIST_SUPER], *hb = NULL, *ovt = NULL, *slot = NULL, *ovw = NULL;
    uint64_t* offs = NULL;
    uint64_t** bits = NULL;
    dyn_persist_ov* nov = NULL;
    dyn_persist_r cur;
    pf_hdr hd;
    size_t hn;
    uint64_t at, written = 0;
    uint32_t i, n = 0, cap = 0, ss;
    memset(&cur, 0, sizeof cur);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size < DYN_PERSIST_SUPER)
        goto out;
    if (pf_pread_all(fd, sb, sizeof sb, 0) < 0)
        goto out;
    ss = ld32(sb + 16);
    if (ss != w->slot_size)
        goto out;
    hn = DYN_PERSIST_SUPER + 2 * (size_t)ss;
    if ((uint64_t)st.st_size < hn)
        goto out;
    hb = (uint8_t*)malloc(hn);
    if (!hb || pf_pread_all(fd, hb, hn, 0) < 0)
        goto out;
    if (pf_parse(hb, hn, (uint64_t)st.st_size, w->type_id, &hd, NULL) != 0)
        goto out;
    if (hd.lineage != w->lineage || hd.gen != w->gen || hd.nsect != w->ns || hd.flags != w->flags)
        goto out;
    if ((uint64_t)st.st_size - hd.base_end > hd.base_end)
        goto out;
    if (hd.ov_count) {
        ovt = (uint8_t*)malloc((size_t)hd.ov_count * DYN_PERSIST_OV);
        if (!ovt || pf_pread_all(fd, ovt, (size_t)hd.ov_count * DYN_PERSIST_OV, hd.ov_off) < 0)
            goto out;
    }
    if (pf_fill(&hd, ovt, &cur, NULL) != 0)
        goto out;
    for (i = 0; i < w->ns; i++) {
        const dyn_persist_sect* s = &w->s[i];
        const dyn_persist_rsect* c = &cur.s[i];
        if (c->kind != s->kind || c->tag != s->tag || c->elem != s->elem || c->flags != s->flags || c->len != s->len)
            goto out;
    }
    offs = (uint64_t*)calloc(w->ns ? w->ns : 1, sizeof *offs);
    bits = (uint64_t**)calloc(w->ns ? w->ns : 1, sizeof *bits);
    if (!offs || !bits)
        goto out;
    for (i = 0; i < w->ns; i++) {
        const dyn_persist_sect* s = &w->s[i];
        const dyn_persist_rsect* c = &cur.s[i];
        int in_base = (s->flags & DYN_PERSIST_S_PAGE) && align_up(c->off + c->len, DYN_PERSIST_PAGE) <= hd.base_end;
        offs[i] = c->off;
        if (in_base && s->dirty) {
            uint64_t nc = dyn_persist_chunks(s->len, 14);
            bits[i] = (uint64_t*)calloc((size_t)((nc + 63) / 64 + 1), sizeof(uint64_t));
            if (!bits[i])
                goto out;
            mark_chunks16(s, bits[i]);
        }
    }
    for (i = 0; i < cur.nov; i++) {
        const dyn_persist_ov* ov = &cur.ov[i];
        if (!bits[ov->sect] || (bits[ov->sect][ov->chunk >> 6] & ((uint64_t)1 << (ov->chunk & 63))))
            continue;
        if (n == cap) {
            uint32_t nc2 = cap ? cap * 2 : 64;
            dyn_persist_ov* np = (dyn_persist_ov*)realloc(nov, (size_t)nc2 * sizeof *np);
            if (!np)
                goto out;
            nov = np;
            cap = nc2;
        }
        nov[n++] = *ov;
    }
    at = align_up((uint64_t)st.st_size, DYN_PERSIST_PAGE);
    for (i = 0; i < w->ns; i++) {
        const dyn_persist_sect* s = &w->s[i];
        const uint8_t* d = sect_data(w, s);
        if (bits[i]) {
            uint64_t nc = dyn_persist_chunks(s->len, 14), c;
            for (c = 0; c < nc; c++) {
                uint64_t lo = c * DYN_PERSIST_PAGE, ln = s->len - lo < DYN_PERSIST_PAGE ? s->len - lo : DYN_PERSIST_PAGE;
                if (!(bits[i][c >> 6] & ((uint64_t)1 << (c & 63))))
                    continue;
                if ((rc = pf_pwrite_all(fd, d + lo, ln, at, e)) < 0
                    || (rc = pf_pzero(fd, at + ln, at + DYN_PERSIST_PAGE, e)) < 0)
                    goto out;
                if (n == cap) {
                    uint32_t nc2 = cap ? cap * 2 : 64;
                    dyn_persist_ov* np = (dyn_persist_ov*)realloc(nov, (size_t)nc2 * sizeof *np);
                    if (!np) {
                        rc = 1;
                        goto out;
                    }
                    nov = np;
                    cap = nc2;
                }
                nov[n].sect = i;
                nov[n].chunk = (uint32_t)c;
                nov[n].src = at;
                n++;
                written += DYN_PERSIST_PAGE;
                at += DYN_PERSIST_PAGE;
            }
            continue;
        }
        if (s->dirty) {
            uint64_t k, nch = dyn_persist_chunks(s->len, s->dirty_shift), any = 0;
            for (k = 0; k < nch && !any; k++)
                any = s->dirty[k >> 6] & ((uint64_t)1 << (k & 63));
            if (!any)
                continue;
        }
        {
            uint64_t a = (s->flags & DYN_PERSIST_S_PAGE) ? DYN_PERSIST_PAGE : DYN_PERSIST_ALIGN;
            uint64_t j;
            at = align_up(at, a);
            if ((rc = pf_pwrite_all(fd, d, s->len, at, e)) < 0)
                goto out;
            offs[i] = at;
            at += s->len;
            written += s->len;
            if (s->flags & DYN_PERSIST_S_PAGE) {
                uint64_t to = align_up(at, DYN_PERSIST_PAGE);
                if ((rc = pf_pzero(fd, at, to, e)) < 0)
                    goto out;
                at = to;
            }
            for (j = 0; j < n;)
                if (nov[j].sect == i)
                    nov[j] = nov[--n];
                else
                    j++;
        }
    }
    if (n > DYN_PERSIST_MAX_OVERLAYS) {
        rc = 1;
        goto out;
    }
    qsort(nov, n, sizeof *nov, ov_cmp);
    {
        pf_slot_info si;
        size_t tn = (size_t)n * DYN_PERSIST_OV;
        memset(&si, 0, sizeof si);
        at = align_up(at, DYN_PERSIST_ALIGN);
        if (tn) {
            ovw = (uint8_t*)malloc(tn);
            if (!ovw) {
                rc = 1;
                goto out;
            }
            for (i = 0; i < n; i++) {
                st32(ovw + (size_t)i * DYN_PERSIST_OV, nov[i].sect);
                st32(ovw + (size_t)i * DYN_PERSIST_OV + 4, nov[i].chunk);
                st64(ovw + (size_t)i * DYN_PERSIST_OV + 8, nov[i].src);
            }
            if ((rc = pf_pwrite_all(fd, ovw, tn, at, e)) < 0)
                goto out;
            written += tn;
            si.ov_off = at;
            at += tn;
        }
        si.ov_count = n;
        si.ov_crc = dyn_crc32c(tn ? ovw : pf_zero, tn);
        si.slot = 1 - hd.slot;
        si.gen = hd.gen + 1;
        si.file_size = at;
        si.base_end = hd.base_end;
        si.offs = offs;
        if (o->fsync && (rc = pf_fsync(fd, e)) < 0)
            goto out;
        slot = (uint8_t*)malloc(w->slot_size);
        if (!slot) {
            rc = 1;
            goto out;
        }
        pf_slot_encode(slot, w, &si);
        if ((rc = pf_pwrite_all(fd, slot, w->slot_size, DYN_PERSIST_SUPER + (uint64_t)si.slot * w->slot_size, e)) < 0)
            goto out;
        written += w->slot_size;
        if (o->fsync && (rc = pf_fsync(fd, e)) < 0)
            goto out;
        o->file_size = si.file_size;
    }
    o->bytes_written = written;
    o->wrote_incremental = 1;
    w->gen++;
    rc = 0;
out:
    if (bits) {
        for (i = 0; i < w->ns; i++)
            free(bits[i]);
        free(bits);
    }
    free(offs);
    free(nov);
    free(ovw);
    free(slot);
    free(ovt);
    free(hb);
    dyn_persist_r_clear(&cur);
    pf_journal_clear();
    close(fd);
    if (rc == PF_CRASH)
        return DYN_PERSIST_E_IO;
    if (rc < 0 && e && e->code == 0)
        pf_fail(e, DYN_PERSIST_E_IO, 0, "incremental write failed");
    return rc < 0 ? (e && e->code ? e->code : DYN_PERSIST_E_IO) : rc;
}

int dyn_persist_save(dyn_persist_w* w, const char* path,
    dyn_persist_save_opt* o, dyn_persist_err* e)
{
    dyn_persist_save_opt dflt = { 1, 0, 0, 0, 0 };
    int rc;
    if (e)
        memset(e, 0, sizeof *e);
    if (!o)
        o = &dflt;
    o->wrote_incremental = 0;
    o->bytes_written = 0;
    pf_journal_clear();
    if (!path || !*path)
        return pf_fail(e, DYN_PERSIST_E_ARG, 0, "empty path");
    if (dyn_persist_w_layout(w) < 0)
        return pf_fail(e, DYN_PERSIST_E_NOMEM, 0, "cannot lay out persist file (out of memory or bad section)");
    o->file_size = w->file_size;
    if (o->incremental) {
        rc = pf_try_incremental(w, path, o, e);
        if (rc <= 0)
            return rc;
    }
    return pf_full_save(w, path, o, e);
}

uint64_t dyn_persist_chunks(uint64_t len, uint32_t shift)
{
    if (shift > 40)
        shift = 40;
    return (len + ((uint64_t)1 << shift) - 1) >> shift;
}

void dyn_persist_chunk_diff(const void* p, uint64_t len, uint32_t shift,
    uint64_t* hashes, int valid, uint64_t* dirty)
{
    uint64_t n, i;
    const uint8_t* b = (const uint8_t*)p;
    if (shift > 40)
        shift = 40;
    n = dyn_persist_chunks(len, shift);
    for (i = 0; i < n; i++) {
        uint64_t lo = i << shift, hi = lo + ((uint64_t)1 << shift), h;
        if (hi > len)
            hi = len;
        h = dyn_xxh64(b + lo, (size_t)(hi - lo), 0);
        if (!valid || h != hashes[i])
            dirty[i >> 6] |= (uint64_t)1 << (i & 63);
        else
            dirty[i >> 6] &= ~((uint64_t)1 << (i & 63));
        hashes[i] = h;
    }
}
