#include "sftp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SFTP) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

#define SFTP_DEPTH 128
#define SFTP_OUT_HWM (256u << 10)
#define SFTP_DIR_BATCH 64

typedef struct {
    int kind;
    int fd;
    DIR* dir;
    uint32_t gen;
    int append;
    int writable;
} srv_hnd_t;

typedef struct {
    ssh_chan_t* ch;
    JSContext* ctx;
    JSRuntime* rt;
    int rootfd;
    int read_only;
    int max_handles;
    uint32_t max_read;
    JSValue allow, res;
    srv_hnd_t* h;
    uint32_t gen;
    sshb_t in;
    size_t off;
    sshb_t pk;
    sshb_t tmp;
    uint8_t* rbuf;
    int inited, paused, decided, decision, eof, done, busy, again;
    char* pkey;
    uint64_t requests, rbytes, wbytes, denied;
    char err[160];
} sftp_srv_t;

typedef struct {
    int fd[SFTP_DEPTH + 1];
    size_t mark[SFTP_DEPTH + 1];
    int depth;
    char vp[PATH_MAX + 2];
    size_t vlen;
    char last[NAME_MAX + 1];
} srv_path_t;

static void srv_drain(sftp_srv_t* s);

static void rp_free(srv_path_t* rp)
{
    while (rp->depth > 0)
        close(rp->fd[rp->depth--]);
}

static void rp_pop(srv_path_t* rp)
{
    if (rp->depth <= 0)
        return;
    close(rp->fd[rp->depth]);
    rp->vlen = rp->mark[rp->depth];
    rp->vp[rp->vlen] = 0;
    rp->depth--;
}

static int resolve(sftp_srv_t* s, const uint8_t* path, size_t pl, int follow, srv_path_t* rp)
{
    char lnk[PATH_MAX + 1];
    char* work;
    size_t wl, pos = 0;
    int links = 0, rc = 0;
    rp->depth = 0;
    rp->fd[0] = s->rootfd;
    rp->vlen = 0;
    rp->vp[0] = 0;
    rp->last[0] = 0x2e; rp->last[1] = 0;
    if (pl > PATH_MAX)
        return -ENAMETOOLONG;
    if (memchr(path, 0, pl))
        return -EINVAL;
    work = (char*)malloc(2 * PATH_MAX + 2);
    if (!work)
        return -ENOMEM;
    memcpy(work, path, pl);
    wl = pl;
    for (;;) {
        size_t st, cl, q;
        int last;
        char c[NAME_MAX + 1];
        struct stat sb;
        int fd;
        while (pos < wl && work[pos] == '/')
            pos++;
        if (pos >= wl) {
            rp->last[0] = 0x2e; rp->last[1] = 0;
            break;
        }
        st = pos;
        while (pos < wl && work[pos] != '/')
            pos++;
        cl = pos - st;
        q = pos;
        while (q < wl && work[q] == '/')
            q++;
        last = q >= wl;
        if (cl > NAME_MAX) {
            rc = -ENAMETOOLONG;
            break;
        }
        memcpy(c, work + st, cl);
        c[cl] = 0;
        if (cl == 1 && c[0] == '.') {
            if (last) {
                rp->last[0] = 0x2e; rp->last[1] = 0;
                break;
            }
            continue;
        }
        if (cl == 2 && c[0] == '.' && c[1] == '.') {
            rp_pop(rp);
            if (last) {
                rp->last[0] = 0x2e; rp->last[1] = 0;
                break;
            }
            continue;
        }
        if (last && !follow) {
            memcpy(rp->last, c, cl + 1);
            break;
        }
        if (fstatat(rp->fd[rp->depth], c, &sb, AT_SYMLINK_NOFOLLOW) < 0) {
            if (last && errno == ENOENT) {
                memcpy(rp->last, c, cl + 1);
                break;
            }
            rc = -errno;
            break;
        }
        if (S_ISLNK(sb.st_mode)) {
            ssize_t n;
            size_t rest = wl - pos;
            if (++links > 40) {
                rc = -ELOOP;
                break;
            }
            n = readlinkat(rp->fd[rp->depth], c, lnk, sizeof lnk - 1);
            if (n <= 0) {
                rc = n < 0 ? -errno : -ENOENT;
                break;
            }
            if ((size_t)n + 1 + rest > 2 * PATH_MAX) {
                rc = -ENAMETOOLONG;
                break;
            }
            memmove(work + n + 1, work + pos, rest);
            memcpy(work, lnk, (size_t)n);
            work[n] = '/';
            wl = (size_t)n + 1 + rest;
            pos = 0;
            if (lnk[0] == '/')
                rp_free(rp), rp->vlen = 0, rp->vp[0] = 0;
            continue;
        }
        if (last) {
            memcpy(rp->last, c, cl + 1);
            break;
        }
        if (!S_ISDIR(sb.st_mode)) {
            rc = -ENOTDIR;
            break;
        }
        if (rp->depth >= SFTP_DEPTH || rp->vlen + 1 + cl > PATH_MAX) {
            rc = -ENAMETOOLONG;
            break;
        }
        fd = openat(rp->fd[rp->depth], c, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            rc = -errno;
            break;
        }
        rp->depth++;
        rp->fd[rp->depth] = fd;
        rp->mark[rp->depth] = rp->vlen;
        rp->vp[rp->vlen++] = '/';
        memcpy(rp->vp + rp->vlen, c, cl);
        rp->vlen += cl;
        rp->vp[rp->vlen] = 0;
    }
    free(work);
    if (rc)
        rp_free(rp);
    return rc;
}

static int rp_dir(const srv_path_t* rp)
{
    return rp->fd[rp->depth];
}

static int rp_isdot(const srv_path_t* rp)
{
    return rp->last[0] == '.' && rp->last[1] == 0;
}

static void rp_full(const srv_path_t* rp, char* out, size_t cap)
{
    if (rp_isdot(rp))
        snprintf(out, cap, "%s", rp->vlen ? rp->vp : "/");
    else
        snprintf(out, cap, "%s/%s", rp->vp, rp->last);
}

static uint32_t errno_status(int e)
{
    if (e == ENOENT || e == ENOTDIR)
        return SFTP_NO_SUCH_FILE;
    if (e == EACCES || e == EPERM || e == EROFS)
        return SFTP_PERMISSION_DENIED;
    return SFTP_FAILURE;
}

static int reply_status(sftp_srv_t* s, uint32_t id, uint32_t code, const char* msg)
{
    sshb_t* b = &s->pk;
    if (sftp_pkt_begin(b, SFTP_STATUS) || sshb_u32(b, id) || sshb_u32(b, code) || sshb_cstr(b, msg) || sshb_cstr(b, "en"))
        return -1;
    return sftp_send(s->ch, b);
}

static int reply_errno(sftp_srv_t* s, uint32_t id, int e)
{
    return reply_status(s, id, errno_status(e), strerror(e));
}

static int reply_attrs(sftp_srv_t* s, uint32_t id, const struct stat* st)
{
    sftp_attr_t a;
    sshb_t* b = &s->pk;
    sftp_attr_stat(st, &a);
    if (sftp_pkt_begin(b, SFTP_ATTRS) || sshb_u32(b, id) || sftp_attr_put(b, &a))
        return -1;
    return sftp_send(s->ch, b);
}

static int reply_name1(sftp_srv_t* s, uint32_t id, const char* name, size_t nl)
{
    sshb_t* b = &s->pk;
    if (sftp_pkt_begin(b, SFTP_NAME) || sshb_u32(b, id) || sshb_u32(b, 1) || sshb_str(b, name, nl) || sshb_str(b, name, nl) || sshb_u32(b, 0))
        return -1;
    return sftp_send(s->ch, b);
}

static JSValue srv_settle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data);

static char* gate_key(const char* op, const char* path, const char* target)
{
    size_t a = strlen(op), b = strlen(path), c = target ? strlen(target) : 0;
    char* k = (char*)malloc(a + b + c + 4);
    if (!k)
        return NULL;
    memcpy(k, op, a);
    k[a] = 1;
    memcpy(k + a + 1, path, b);
    k[a + 1 + b] = 2;
    if (c)
        memcpy(k + a + b + 2, target, c);
    k[a + b + 2 + c] = 0;
    return k;
}

static int gate(sftp_srv_t* s, const char* op, const char* path, const char* target, int write)
{
    JSContext* ctx = s->ctx;
    JSValue req, ret;
    if (write && s->read_only)
        return 0;
    if (JS_IsUndefined(s->allow))
        return 1;
    if (s->decided) {
        char* k = gate_key(op, path, target);
        int same = k && s->pkey && strcmp(k, s->pkey) == 0;
        free(k);
        free(s->pkey);
        s->pkey = NULL;
        s->decided = 0;
        return same ? s->decision : 0;
    }
    if (ssh_js_off() || !s->ch->conn)
        return 0;
    req = JS_NewObject(ctx);
    if (JS_IsException(req)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    JS_SetPropertyStr(ctx, req, "op", JS_NewString(ctx, op));
    JS_SetPropertyStr(ctx, req, "path", JS_NewString(ctx, path));
    if (target)
        JS_SetPropertyStr(ctx, req, "target", JS_NewString(ctx, target));
    JS_SetPropertyStr(ctx, req, "user", JS_NewString(ctx, s->ch->conn->user ? s->ch->conn->user : ""));
    JS_SetPropertyStr(ctx, req, "write", JS_NewBool(ctx, write));
    ret = JS_Call(ctx, s->allow, JS_UNDEFINED, 1, (JSValueConst*)&req);
    JS_FreeValue(ctx, req);
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    if (JS_IsBool(ret))
        return JS_ToBool(ctx, ret);
    if (JS_IsObject(ret)) {
        JSValueConst d[1] = { s->ch->jsobj };
        int rc = ssh_then2(ctx, ret, srv_settle, 1, d);
        JS_FreeValue(ctx, ret);
        if (rc < 0)
            return 0;
        free(s->pkey);
        s->pkey = gate_key(op, path, target);
        s->paused = 1;
        return 2;
    }
    JS_FreeValue(ctx, ret);
    return 0;
}

static int gated(sftp_srv_t* s, uint32_t id, int g)
{
    if (g == 2)
        return 2;
    if (g == 0) {
        s->denied++;
        return reply_status(s, id, SFTP_PERMISSION_DENIED, "denied by the server policy") < 0 ? -1 : 0;
    }
    return 1;
}

static srv_hnd_t* hnd_get(sftp_srv_t* s, sshr_t* r, int kind)
{
    size_t n;
    const uint8_t* p = sshr_str(r, &n);
    uint32_t i, g;
    if (r->err || n != 8)
        return NULL;
    i = ssh_be32(p);
    g = ssh_be32(p + 4);
    if (i >= (uint32_t)s->max_handles || s->h[i].kind != kind || s->h[i].gen != g)
        return NULL;
    return &s->h[i];
}

static int hnd_new(sftp_srv_t* s, uint32_t id, int kind, int fd, DIR* d, int append, int writable)
{
    int i;
    uint8_t hb[8];
    sshb_t* b = &s->pk;
    for (i = 0; i < s->max_handles; i++)
        if (!s->h[i].kind)
            break;
    if (i == s->max_handles) {
        if (d)
            closedir(d);
        else
            close(fd);
        return reply_status(s, id, SFTP_FAILURE, "too many open handles");
    }
    s->h[i].kind = kind;
    s->h[i].fd = fd;
    s->h[i].dir = d;
    s->h[i].gen = ++s->gen;
    s->h[i].append = append;
    s->h[i].writable = writable;
    ssh_put_be32(hb, (uint32_t)i);
    ssh_put_be32(hb + 4, s->h[i].gen);
    if (sftp_pkt_begin(b, SFTP_HANDLE) || sshb_u32(b, id) || sshb_str(b, hb, 8))
        return -1;
    return sftp_send(s->ch, b);
}

static void hnd_close(srv_hnd_t* h)
{
    if (h->kind == 2 && h->dir)
        closedir(h->dir);
    else if (h->kind == 1 && h->fd >= 0)
        close(h->fd);
    h->kind = 0;
    h->fd = -1;
    h->dir = NULL;
}

static int apply_attrs(int fd, const sftp_attr_t* a)
{
    if ((a->flags & SFTP_A_SIZE) && ftruncate(fd, (off_t)a->size) < 0)
        return -errno;
    if ((a->flags & SFTP_A_PERM) && fchmod(fd, (mode_t)(a->perm & 07777)) < 0)
        return -errno;
    if ((a->flags & SFTP_A_UIDGID) && fchown(fd, (uid_t)a->uid, (gid_t)a->gid) < 0)
        return -errno;
    if (a->flags & SFTP_A_TIME) {
        struct timespec ts[2];
        ts[0].tv_sec = (time_t)a->atime;
        ts[0].tv_nsec = 0;
        ts[1].tv_sec = (time_t)a->mtime;
        ts[1].tv_nsec = 0;
        if (futimens(fd, ts) < 0)
            return -errno;
    }
    return 0;
}

static int path_attrs(int dir, const char* name, const sftp_attr_t* a)
{
    if (a->flags & SFTP_A_PERM) {
        if (fchmodat(dir, name, (mode_t)(a->perm & 07777), AT_SYMLINK_NOFOLLOW) < 0) {
            int fd, e = errno;
            if (e != ENOTSUP && e != EOPNOTSUPP)
                return -e;
            fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                return -errno;
            e = fchmod(fd, (mode_t)(a->perm & 07777)) < 0 ? errno : 0;
            close(fd);
            if (e)
                return -e;
        }
    }
    if ((a->flags & SFTP_A_UIDGID) && fchownat(dir, name, (uid_t)a->uid, (gid_t)a->gid, AT_SYMLINK_NOFOLLOW) < 0)
        return -errno;
    if (a->flags & SFTP_A_SIZE) {
        int fd = openat(dir, name, O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC), rc;
        sftp_attr_t sz;
        if (fd < 0)
            return -errno;
        memset(&sz, 0, sizeof sz);
        sz.flags = SFTP_A_SIZE;
        sz.size = a->size;
        rc = apply_attrs(fd, &sz);
        close(fd);
        if (rc)
            return rc;
    }
    if (a->flags & SFTP_A_TIME) {
        struct timespec ts[2];
        ts[0].tv_sec = (time_t)a->atime;
        ts[0].tv_nsec = 0;
        ts[1].tv_sec = (time_t)a->mtime;
        ts[1].tv_nsec = 0;
        if (utimensat(dir, name, ts, AT_SYMLINK_NOFOLLOW) < 0)
            return -errno;
    }
    return 0;
}

static int op_open(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    size_t pl;
    const uint8_t* p = sshr_str(r, &pl);
    uint32_t pf = sshr_u32(r);
    sftp_attr_t a;
    srv_path_t rp;
    char full[PATH_MAX + NAME_MAX + 4];
    int rc, g, fl = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, fd;
    int wr = (pf & (SFTP_FXF_WRITE | SFTP_FXF_APPEND | SFTP_FXF_CREAT | SFTP_FXF_TRUNC)) != 0;
    struct stat st;
    if (sftp_attr_get(r, &a) < 0)
        return -1;
    if ((rc = resolve(s, p, pl, 1, &rp)) < 0)
        return reply_errno(s, id, -rc);
    rp_full(&rp, full, sizeof full);
    g = gated(s, id, gate(s, "open", full, NULL, wr));
    if (g != 1) {
        rp_free(&rp);
        return g;
    }
    if ((pf & SFTP_FXF_READ) && (pf & (SFTP_FXF_WRITE | SFTP_FXF_APPEND)))
        fl |= O_RDWR;
    else if (pf & (SFTP_FXF_WRITE | SFTP_FXF_APPEND))
        fl |= O_WRONLY;
    else
        fl |= O_RDONLY;
    if (pf & SFTP_FXF_APPEND)
        fl |= O_APPEND;
    if (pf & SFTP_FXF_CREAT)
        fl |= O_CREAT;
    if (pf & SFTP_FXF_TRUNC)
        fl |= O_TRUNC;
    if (pf & SFTP_FXF_EXCL)
        fl |= O_EXCL;
    if (rp_isdot(&rp)) {
        rp_free(&rp);
        return reply_errno(s, id, EISDIR);
    }
    fd = openat(rp_dir(&rp), rp.last, fl, (a.flags & SFTP_A_PERM) ? (mode_t)(a.perm & 07777) : (mode_t)0666);
    rc = errno;
    rp_free(&rp);
    if (fd < 0)
        return reply_errno(s, id, rc);
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return reply_status(s, id, SFTP_FAILURE, "not a regular file");
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return hnd_new(s, id, 1, fd, NULL, (pf & SFTP_FXF_APPEND) != 0, wr);
}

static int op_read(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    srv_hnd_t* h = hnd_get(s, r, 1);
    uint32_t hi = sshr_u32(r), lo = sshr_u32(r), len = sshr_u32(r);
    uint64_t off = ((uint64_t)hi << 32) | lo;
    ssize_t n;
    sshb_t* b = &s->pk;
    if (r->err)
        return -1;
    if (!h)
        return reply_status(s, id, SFTP_FAILURE, "invalid handle");
    if (len > s->max_read)
        len = s->max_read;
    if (off > (uint64_t)INT64_MAX)
        return reply_status(s, id, SFTP_EOF, "end of file");
    if (sftp_pkt_begin(b, SFTP_DATA) || sshb_u32(b, id) || sshb_u32(b, 0) || sshb_reserve(b, len))
        return -1;
    do
        n = pread(h->fd, b->p + b->len, len, (off_t)off);
    while (n < 0 && errno == EINTR);
    if (n < 0)
        return reply_errno(s, id, errno);
    if (n == 0)
        return reply_status(s, id, SFTP_EOF, "end of file");
    ssh_put_be32(b->p + 9, (uint32_t)n);
    b->len += (size_t)n;
    s->rbytes += (uint64_t)n;
    return sftp_send(s->ch, b);
}

static int op_write(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    srv_hnd_t* h = hnd_get(s, r, 1);
    uint32_t hi = sshr_u32(r), lo = sshr_u32(r);
    uint64_t off = ((uint64_t)hi << 32) | lo;
    size_t n, done = 0;
    const uint8_t* d = sshr_str(r, &n);
    if (r->err)
        return -1;
    if (!h)
        return reply_status(s, id, SFTP_FAILURE, "invalid handle");
    if (off > (uint64_t)INT64_MAX - n)
        return reply_errno(s, id, EFBIG);
    while (done < n) {
        ssize_t w = h->append ? write(h->fd, d + done, n - done) : pwrite(h->fd, d + done, n - done, (off_t)(off + done));
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return reply_errno(s, id, w < 0 ? errno : EIO);
        done += (size_t)w;
    }
    s->wbytes += n;
    return reply_status(s, id, SFTP_OK, "");
}

static int op_stat(sftp_srv_t* s, uint32_t id, sshr_t* r, int follow)
{
    size_t pl;
    const uint8_t* p = sshr_str(r, &pl);
    srv_path_t rp;
    char full[PATH_MAX + NAME_MAX + 4];
    struct stat st;
    int rc, g;
    if (r->err)
        return -1;
    if ((rc = resolve(s, p, pl, follow, &rp)) < 0)
        return reply_errno(s, id, -rc);
    rp_full(&rp, full, sizeof full);
    g = gated(s, id, gate(s, follow ? "stat" : "lstat", full, NULL, 0));
    if (g != 1) {
        rp_free(&rp);
        return g;
    }
    rc = fstatat(rp_dir(&rp), rp.last, &st, AT_SYMLINK_NOFOLLOW) < 0 ? errno : 0;
    rp_free(&rp);
    return rc ? reply_errno(s, id, rc) : reply_attrs(s, id, &st);
}

static int op_fstat(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    size_t save = r->off;
    srv_hnd_t* h = hnd_get(s, r, 1);
    struct stat st;
    if (!h) {
        r->off = save;
        r->err = 0;
        h = hnd_get(s, r, 2);
    }
    if (!h)
        return reply_status(s, id, SFTP_FAILURE, "invalid handle");
    if (fstat(h->kind == 2 ? dirfd(h->dir) : h->fd, &st) < 0)
        return reply_errno(s, id, errno);
    return reply_attrs(s, id, &st);
}

static int op_setstat(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    size_t pl;
    const uint8_t* p = sshr_str(r, &pl);
    sftp_attr_t a;
    srv_path_t rp;
    char full[PATH_MAX + NAME_MAX + 4];
    int rc, g;
    if (sftp_attr_get(r, &a) < 0)
        return -1;
    if ((rc = resolve(s, p, pl, 1, &rp)) < 0)
        return reply_errno(s, id, -rc);
    rp_full(&rp, full, sizeof full);
    g = gated(s, id, gate(s, "setstat", full, NULL, 1));
    if (g != 1) {
        rp_free(&rp);
        return g;
    }
    rc = path_attrs(rp_dir(&rp), rp.last, &a);
    rp_free(&rp);
    return rc ? reply_errno(s, id, -rc) : reply_status(s, id, SFTP_OK, "");
}

static int op_fsetstat(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    srv_hnd_t* h = hnd_get(s, r, 1);
    sftp_attr_t a;
    int rc;
    if (sftp_attr_get(r, &a) < 0)
        return -1;
    if (!h)
        return reply_status(s, id, SFTP_FAILURE, "invalid handle");
    if (s->read_only || !h->writable) {
        s->denied++;
        return reply_status(s, id, SFTP_PERMISSION_DENIED, s->read_only ? "denied by the server policy" : "the handle was not opened for writing");
    }
    rc = apply_attrs(h->fd, &a);
    return rc ? reply_errno(s, id, -rc) : reply_status(s, id, SFTP_OK, "");
}

static int op_opendir(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    size_t pl;
    const uint8_t* p = sshr_str(r, &pl);
    srv_path_t rp;
    char full[PATH_MAX + NAME_MAX + 4];
    int rc, g, fd;
    DIR* d;
    if (r->err)
        return -1;
    if ((rc = resolve(s, p, pl, 1, &rp)) < 0)
        return reply_errno(s, id, -rc);
    rp_full(&rp, full, sizeof full);
    g = gated(s, id, gate(s, "opendir", full, NULL, 0));
    if (g != 1) {
        rp_free(&rp);
        return g;
    }
    fd = openat(rp_dir(&rp), rp.last, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    rc = errno;
    rp_free(&rp);
    if (fd < 0)
        return reply_errno(s, id, rc);
    d = fdopendir(fd);
    if (!d) {
        rc = errno;
        close(fd);
        return reply_errno(s, id, rc);
    }
    return hnd_new(s, id, 2, -1, d, 0, 0);
}

static int op_readdir(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    srv_hnd_t* h = hnd_get(s, r, 2);
    sshb_t* b = &s->pk;
    uint32_t count = 0;
    if (!h)
        return reply_status(s, id, SFTP_FAILURE, "invalid handle");
    if (sftp_pkt_begin(b, SFTP_NAME) || sshb_u32(b, id) || sshb_u32(b, 0))
        return -1;
    while (count < SFTP_DIR_BATCH && b->len < SFTP_MAX_MSG / 2) {
        struct dirent* e;
        struct stat st;
        sftp_attr_t a;
        size_t nl;
        errno = 0;
        e = readdir(h->dir);
        if (!e)
            break;
        nl = strlen(e->d_name);
        if (fstatat(dirfd(h->dir), e->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
            memset(&st, 0, sizeof st);
        sftp_attr_stat(&st, &a);
        sftp_longname(&a, e->d_name, nl, &s->tmp);
        if (sshb_str(b, e->d_name, nl) || sshb_str(b, s->tmp.p, s->tmp.len) || sftp_attr_put(b, &a))
            return -1;
        count++;
    }
    if (!count)
        return reply_status(s, id, SFTP_EOF, "end of directory");
    ssh_put_be32(b->p + 9, count);
    return sftp_send(s->ch, b);
}

static int op_path(sftp_srv_t* s, uint32_t id, sshr_t* r, int type)
{
    size_t pl;
    const uint8_t* p = sshr_str(r, &pl);
    sftp_attr_t a;
    srv_path_t rp;
    char full[PATH_MAX + NAME_MAX + 4];
    const char* op = type == SFTP_REMOVE ? "remove" : type == SFTP_MKDIR ? "mkdir" : type == SFTP_RMDIR ? "rmdir" : "readlink";
    int rc, g;
    memset(&a, 0, sizeof a);
    if (r->err || (type == SFTP_MKDIR && sftp_attr_get(r, &a) < 0))
        return -1;
    if ((rc = resolve(s, p, pl, 0, &rp)) < 0)
        return reply_errno(s, id, -rc);
    rp_full(&rp, full, sizeof full);
    g = gated(s, id, gate(s, op, full, NULL, type != SFTP_READLINK));
    if (g != 1) {
        rp_free(&rp);
        return g;
    }
    if (rp_isdot(&rp)) {
        rp_free(&rp);
        return reply_errno(s, id, type == SFTP_MKDIR ? EEXIST : EINVAL);
    }
    if (type == SFTP_READLINK) {
        char t[PATH_MAX + 1];
        ssize_t n = readlinkat(rp_dir(&rp), rp.last, t, sizeof t - 1);
        rc = errno;
        rp_free(&rp);
        if (n < 0)
            return reply_errno(s, id, rc);
        return reply_name1(s, id, t, (size_t)n);
    }
    if (type == SFTP_REMOVE)
        rc = unlinkat(rp_dir(&rp), rp.last, 0);
    else if (type == SFTP_RMDIR)
        rc = unlinkat(rp_dir(&rp), rp.last, AT_REMOVEDIR);
    else
        rc = mkdirat(rp_dir(&rp), rp.last, (a.flags & SFTP_A_PERM) ? (mode_t)(a.perm & 07777) : (mode_t)0777);
    rc = rc < 0 ? errno : 0;
    rp_free(&rp);
    return rc ? reply_errno(s, id, rc) : reply_status(s, id, SFTP_OK, "");
}

static int op_realpath(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    size_t pl;
    const uint8_t* p = sshr_str(r, &pl);
    srv_path_t rp;
    char full[PATH_MAX + NAME_MAX + 4];
    int rc;
    if (r->err)
        return -1;
    if ((rc = resolve(s, p, pl, 1, &rp)) < 0)
        return reply_errno(s, id, -rc);
    rp_full(&rp, full, sizeof full);
    rp_free(&rp);
    return reply_name1(s, id, full, strlen(full));
}

static int op_rename(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    size_t on, nn;
    const uint8_t* o = sshr_str(r, &on);
    const uint8_t* n = sshr_str(r, &nn);
    srv_path_t a, b;
    char fa[PATH_MAX + NAME_MAX + 4], fb[PATH_MAX + NAME_MAX + 4];
    struct stat st;
    int rc, g;
    if (r->err)
        return -1;
    if ((rc = resolve(s, o, on, 0, &a)) < 0)
        return reply_errno(s, id, -rc);
    if ((rc = resolve(s, n, nn, 0, &b)) < 0) {
        rp_free(&a);
        return reply_errno(s, id, -rc);
    }
    rp_full(&a, fa, sizeof fa);
    rp_full(&b, fb, sizeof fb);
    g = gated(s, id, gate(s, "rename", fa, fb, 1));
    if (g != 1 || rp_isdot(&a) || rp_isdot(&b)) {
        rp_free(&a);
        rp_free(&b);
        return g != 1 ? g : reply_errno(s, id, EINVAL);
    }
    if (fstatat(rp_dir(&b), b.last, &st, AT_SYMLINK_NOFOLLOW) == 0)
        rc = EEXIST;
    else
        rc = renameat(rp_dir(&a), a.last, rp_dir(&b), b.last) < 0 ? errno : 0;
    rp_free(&a);
    rp_free(&b);
    if (rc == EEXIST)
        return reply_status(s, id, SFTP_FAILURE, "the target exists");
    return rc ? reply_errno(s, id, rc) : reply_status(s, id, SFTP_OK, "");
}

static int op_symlink(sftp_srv_t* s, uint32_t id, sshr_t* r)
{
    size_t tn, ln;
    const uint8_t* t = sshr_str(r, &tn);
    const uint8_t* l = sshr_str(r, &ln);
    srv_path_t rp;
    char full[PATH_MAX + NAME_MAX + 4], target[PATH_MAX + 1];
    int rc, g;
    if (r->err)
        return -1;
    if (tn == 0 || tn > PATH_MAX || memchr(t, 0, tn))
        return reply_errno(s, id, EINVAL);
    memcpy(target, t, tn);
    target[tn] = 0;
    if ((rc = resolve(s, l, ln, 0, &rp)) < 0)
        return reply_errno(s, id, -rc);
    rp_full(&rp, full, sizeof full);
    g = gated(s, id, gate(s, "symlink", full, target, 1));
    if (g != 1 || rp_isdot(&rp)) {
        rp_free(&rp);
        return g != 1 ? g : reply_errno(s, id, EEXIST);
    }
    rc = symlinkat(target, rp_dir(&rp), rp.last) < 0 ? errno : 0;
    rp_free(&rp);
    return rc ? reply_errno(s, id, rc) : reply_status(s, id, SFTP_OK, "");
}

static int srv_one(sftp_srv_t* s, const uint8_t* p, size_t n)
{
    sshr_t r;
    uint8_t t;
    uint32_t id;
    sshr_init(&r, p, n);
    t = sshr_u8(&r);
    if (!s->inited) {
        sshb_t* b = &s->pk;
        (void)sshr_u32(&r);
        if (t != SFTP_INIT || r.err)
            return -1;
        s->inited = 1;
        if (sftp_pkt_begin(b, SFTP_VERSION) || sshb_u32(b, 3))
            return -1;
        return sftp_send(s->ch, b);
    }
    id = sshr_u32(&r);
    if (r.err)
        return -1;
    switch (t) {
    case SFTP_OPEN:
        return op_open(s, id, &r);
    case SFTP_CLOSE: {
        size_t save = r.off;
        srv_hnd_t* h = hnd_get(s, &r, 1);
        if (!h) {
            r.off = save;
            r.err = 0;
            h = hnd_get(s, &r, 2);
        }
        if (!h)
            return reply_status(s, id, SFTP_FAILURE, "invalid handle");
        hnd_close(h);
        return reply_status(s, id, SFTP_OK, "");
    }
    case SFTP_READ:
        return op_read(s, id, &r);
    case SFTP_WRITE:
        return op_write(s, id, &r);
    case SFTP_LSTAT:
        return op_stat(s, id, &r, 0);
    case SFTP_STAT:
        return op_stat(s, id, &r, 1);
    case SFTP_FSTAT:
        return op_fstat(s, id, &r);
    case SFTP_SETSTAT:
        return op_setstat(s, id, &r);
    case SFTP_FSETSTAT:
        return op_fsetstat(s, id, &r);
    case SFTP_OPENDIR:
        return op_opendir(s, id, &r);
    case SFTP_READDIR:
        return op_readdir(s, id, &r);
    case SFTP_REMOVE:
    case SFTP_MKDIR:
    case SFTP_RMDIR:
    case SFTP_READLINK:
        return op_path(s, id, &r, t);
    case SFTP_REALPATH:
        return op_realpath(s, id, &r);
    case SFTP_RENAME:
        return op_rename(s, id, &r);
    case SFTP_SYMLINK:
        return op_symlink(s, id, &r);
    default:
        return reply_status(s, id, SFTP_OP_UNSUPPORTED, "operation not supported");
    }
}

static JSValue srv_stats(sftp_srv_t* s)
{
    JSContext* ctx = s->ctx;
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "requests", JS_NewFloat64(ctx, (double)s->requests));
    JS_SetPropertyStr(ctx, o, "bytesRead", JS_NewFloat64(ctx, (double)s->rbytes));
    JS_SetPropertyStr(ctx, o, "bytesWritten", JS_NewFloat64(ctx, (double)s->wbytes));
    JS_SetPropertyStr(ctx, o, "denied", JS_NewFloat64(ctx, (double)s->denied));
    JS_SetPropertyStr(ctx, o, "error", s->err[0] ? JS_NewString(ctx, s->err) : JS_NULL);
    return o;
}

static void srv_finish(sftp_srv_t* s, const char* err)
{
    int i;
    if (s->done)
        return;
    s->done = 1;
    if (err && !s->err[0])
        snprintf(s->err, sizeof s->err, "%s", err);
    for (i = 0; i < s->max_handles; i++)
        if (s->h[i].kind)
            hnd_close(&s->h[i]);
    if (s->rootfd >= 0)
        close(s->rootfd);
    s->rootfd = -1;
    if (s->ctx && !ssh_js_off() && JS_IsFunction(s->ctx, s->res)) {
        JSValue st = srv_stats(s);
        JSValue r = JS_Call(s->ctx, s->res, JS_UNDEFINED, 1, (JSValueConst*)&st);
        JS_FreeValue(s->ctx, st);
        if (JS_IsException(r))
            JS_FreeValue(s->ctx, JS_GetException(s->ctx));
        JS_FreeValue(s->ctx, r);
    }
    JS_FreeValueRT(s->rt, s->res);
    JS_FreeValueRT(s->rt, s->allow);
    s->res = s->allow = JS_UNDEFINED;
    sshb_free(&s->in);
    s->off = 0;
    free(s->pkey);
    s->pkey = NULL;
}

static void srv_drain(sftp_srv_t* s)
{
    ssh_chan_t* ch = s->ch;
    if (s->busy) {
        s->again = 1;
        return;
    }
    s->busy = 1;
    do {
        s->again = 0;
        while (!s->paused && !s->done && ch->conn) {
            size_t avail = s->in.len - s->off;
            uint32_t L;
            int rc;
            if (ch->inq.len - ch->inq_off > SFTP_OUT_HWM)
                break;
            if (avail < 4)
                break;
            L = ssh_be32(s->in.p + s->off);
            if (L < 5 || L > SFTP_MAX_MSG) {
                srv_finish(s, "a malformed or oversized SFTP packet");
                ssh_chan_close(ch);
                break;
            }
            if (avail < 4 + (size_t)L)
                break;
            rc = srv_one(s, s->in.p + s->off + 4, L);
            if (rc < 0) {
                srv_finish(s, "a malformed SFTP request");
                ssh_chan_close(ch);
                break;
            }
            if (s->paused)
                break;
            s->off += 4 + (size_t)L;
            s->requests++;
            s->decided = 0;
            free(s->pkey);
            s->pkey = NULL;
        }
        if (!s->done) {
            if (s->off && s->off == s->in.len)
                s->in.len = s->off = 0;
            else if (s->off > (64u << 10)) {
                memmove(s->in.p, s->in.p + s->off, s->in.len - s->off);
                s->in.len -= s->off;
                s->off = 0;
            }
            ch->queued[0] = s->in.len - s->off;
            if (s->eof && !s->paused && s->in.len - s->off < 4)
                srv_finish(s, NULL);
        } else
            ch->queued[0] = 0;
        if (ch->conn)
            ssh_chan_kick(ch);
    } while (s->again && ch->conn);
    s->busy = 0;
}

static void srv_on_data(ssh_chan_t* ch, int which, const uint8_t* p, size_t n)
{
    sftp_srv_t* s = (sftp_srv_t*)ch->opq;
    if (!s || s->done || which)
        return;
    if (sshb_put(&s->in, p, n)) {
        srv_finish(s, "out of memory");
        ssh_chan_close(ch);
        return;
    }
    srv_drain(s);
}

static void srv_on_eof(ssh_chan_t* ch)
{
    sftp_srv_t* s = (sftp_srv_t*)ch->opq;
    if (!s || s->done)
        return;
    s->eof = 1;
    srv_drain(s);
}

static void srv_on_written(ssh_chan_t* ch)
{
    sftp_srv_t* s = (sftp_srv_t*)ch->opq;
    size_t avail;
    if (!s || s->done || s->paused || ch->inq.len - ch->inq_off > SFTP_OUT_HWM)
        return;
    avail = s->in.len - s->off;
    if (avail < 4 || avail < 4 + (size_t)ssh_be32(s->in.p + s->off))
        return;
    if (s->busy)
        s->again = 1;
    else
        srv_drain(s);
}

static void srv_on_closed(ssh_chan_t* ch, const char* err)
{
    sftp_srv_t* s = (sftp_srv_t*)ch->opq;
    if (s)
        srv_finish(s, err);
}

static void srv_release(JSRuntime* rt, ssh_chan_t* ch)
{
    sftp_srv_t* s = (sftp_srv_t*)ch->opq;
    (void)rt;
    if (!s)
        return;
    s->ctx = NULL;
    srv_finish(s, NULL);
    sshb_free(&s->pk);
    sshb_free(&s->tmp);
    free(s->h);
    free(s);
    ch->opq = NULL;
    ch->ops = NULL;
}

static const ssh_chan_ops_t srv_ops = { NULL, srv_on_data, srv_on_eof, srv_on_written, srv_on_closed, srv_release };

static JSValue srv_settle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue* data)
{
    ssh_chan_t* ch = ssh_session_of(data[0]);
    sftp_srv_t* s = ch && ch->ops == &srv_ops ? (sftp_srv_t*)ch->opq : NULL;
    (void)this_val;
    if (!s || s->done || !s->paused)
        return JS_UNDEFINED;
    s->paused = 0;
    s->decided = 1;
    s->decision = magic && argc > 0 && JS_IsBool(argv[0]) && JS_ToBool(ctx, argv[0]);
    srv_drain(s);
    return JS_UNDEFINED;
}

static const char* const serve_keys[] = { "root", "readOnly", "allow", "dyna" };
static const char* const serve_meta_keys[] = { "maxHandles", "maxReadBytes" };

static int meta_int(JSContext* ctx, JSValueConst o, const char* k, double lo, double hi, int* out)
{
    JSValue m = JS_GetPropertyStr(ctx, o, k);
    double d;
    if (JS_IsException(m))
        return -1;
    if (JS_IsUndefined(m))
        return 0;
    if (!JS_IsNumber(m)) {
        JS_FreeValue(ctx, m);
        JS_ThrowTypeError(ctx, "SFTP: dyna.%s must be a number", k);
        return -1;
    }
    JS_ToFloat64(ctx, &d, m);
    JS_FreeValue(ctx, m);
    if (!(d >= lo && d <= hi) || d != (double)(int)d) {
        JS_ThrowRangeError(ctx, "SFTP: dyna.%s must be an integer in %.0f..%.0f", k, lo, hi);
        return -1;
    }
    *out = (int)d;
    return 0;
}

static JSValue sftp_serve(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    ssh_chan_t* ch;
    JSValueConst o;
    JSValue v, p, f[2];
    sftp_srv_t* s;
    const char* root;
    int rootfd, ro = 0, maxh = 64, maxr = (int)SFTP_MAX_READ;
    (void)this_val;
    ch = argc > 0 ? ssh_session_of(argv[0]) : NULL;
    if (!ch)
        return JS_ThrowTypeError(ctx, "SFTP: serve expects an SSHSession");
    o = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!JS_IsObject(o))
        return JS_ThrowTypeError(ctx, "SFTP: serve expects an options object with root");
    if (dyn_opts_strict(ctx, o, serve_keys, (int)countof(serve_keys)))
        return JS_EXCEPTION;
    if (!ch->conn || ch->ops || ch->got_data || ch->want_exit || ch->sent_close)
        return JS_ThrowTypeError(ctx, "SFTP: serve must be called synchronously from onSession, once, on an open session");
    v = JS_GetPropertyStr(ctx, o, "readOnly");
    if (!JS_IsUndefined(v)) {
        if (!JS_IsBool(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx, "SFTP: readOnly must be a boolean");
        }
        ro = JS_ToBool(ctx, v);
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "dyna");
    if (!JS_IsUndefined(v)) {
        if (!JS_IsObject(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx, "SFTP: dyna must be an object");
        }
        if (dyn_opts_strict(ctx, v, serve_meta_keys, (int)countof(serve_meta_keys)) || meta_int(ctx, v, "maxHandles", 1, 4096, &maxh)
            || meta_int(ctx, v, "maxReadBytes", 1024, SFTP_MAX_READ, &maxr)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, o, "allow");
    if (!JS_IsUndefined(v) && !JS_IsFunction(ctx, v)) {
        JS_FreeValue(ctx, v);
        return JS_ThrowTypeError(ctx, "SFTP: allow must be a function");
    }
    p = JS_GetPropertyStr(ctx, o, "root");
    root = JS_IsString(p) ? JS_ToCString(ctx, p) : NULL;
    JS_FreeValue(ctx, p);
    if (!root) {
        JS_FreeValue(ctx, v);
        return JS_ThrowTypeError(ctx, "SFTP: root must be a directory path string");
    }
    rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) {
        int e = errno;
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "SFTP: cannot open root %s: %s", root, strerror(e));
        JS_FreeCString(ctx, root);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, root);
    s = (sftp_srv_t*)calloc(1, sizeof *s);
    if (s)
        s->h = (srv_hnd_t*)calloc((size_t)maxh, sizeof *s->h);
    if (!s || !s->h) {
        if (s)
            free(s);
        close(rootfd);
        JS_FreeValue(ctx, v);
        return JS_ThrowOutOfMemory(ctx);
    }
    p = JS_NewPromiseCapability(ctx, f);
    if (JS_IsException(p)) {
        free(s->h);
        free(s);
        close(rootfd);
        JS_FreeValue(ctx, v);
        return p;
    }
    JS_FreeValue(ctx, f[1]);
    s->ch = ch;
    s->ctx = ctx;
    s->rt = JS_GetRuntime(ctx);
    s->rootfd = rootfd;
    s->read_only = ro;
    s->max_handles = maxh;
    s->max_read = (uint32_t)maxr;
    s->allow = v;
    s->res = f[0];
    ch->ops = &srv_ops;
    ch->opq = s;
    return p;
}

static const JSCFunctionListEntry sftp_server_static[] = {
    JS_CFUNC_DEF("serve", 2, sftp_serve),
};

int sftp_server_init(JSContext* ctx, JSModuleDef* m)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return -1;
    JS_SetPropertyFunctionList(ctx, o, sftp_server_static, (int)countof(sftp_server_static));
    return JS_SetModuleExport(ctx, m, "SFTPServer", o);
}

#endif
