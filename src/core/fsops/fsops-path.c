#include "fsops.h"

int dyn_fs_path_open(const char* path, int flags, mode_t mode)
{
    return open(path, flags | O_CLOEXEC, mode);
}

char* dyn_join(const char* a, const char* b)
{
    size_t la = strlen(a), lb = strlen(b);
    int sep = (la > 0 && a[la - 1] != '/');
    char* r;

    if (la == 0) {
        r = (char*)malloc(lb + 1);
        if (!r)
            return NULL;
        memcpy(r, b, lb + 1);
        return r;
    }
    r = (char*)malloc(la + (size_t)sep + lb + 1);
    if (!r)
        return NULL;
    memcpy(r, a, la);
    if (sep)
        r[la] = '/';
    memcpy(r + la + (size_t)sep, b, lb);
    r[la + (size_t)sep + lb] = '\0';
    return r;
}

const char* dyn_fs_errno_code(int e)
{
    switch (e) {
    case ENOENT:
        return "ENOENT";
    case EBADF:
        return "EBADF";
    case EACCES:
        return "EACCES";
    case EWOULDBLOCK:
        return "EWOULDBLOCK";
    case EEXIST:
        return "EEXIST";
    case ENOTDIR:
        return "ENOTDIR";
    case EISDIR:
        return "EISDIR";
    case ENOTEMPTY:
        return "ENOTEMPTY";
    case EPERM:
        return "EPERM";
    case ELOOP:
        return "ELOOP";
    case ENAMETOOLONG:
        return "ENAMETOOLONG";
    case EXDEV:
        return "EXDEV";
    case EINVAL:
        return "EINVAL";
    case ENOSPC:
        return "ENOSPC";
    case EROFS:
        return "EROFS";
    case EBUSY:
        return "EBUSY";
    case EMFILE:
        return "EMFILE";
    case ENFILE:
        return "ENFILE";
    case ENOMEM:
        return "ENOMEM";
    case EIO:
        return "EIO";
    case ENOTSOCK:
        return "ENOTSOCK";
    default:
        return NULL;
    }
}

int dyn_fs_mkdirp(const char* path, mode_t mode)
{
    struct stat st;
    size_t len;
    char* parent;
    int r;

    if (mkdir(path, mode) == 0)
        return 0;
    if (errno == EEXIST) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT)
        return -1;

    len = strlen(path);
    while (len > 0 && path[len - 1] == '/')
        len--;
    while (len > 0 && path[len - 1] != '/')
        len--;
    while (len > 0 && path[len - 1] == '/')
        len--;
    if (len == 0) {
        errno = ENOENT;
        return -1;
    }
    parent = (char*)malloc(len + 1);
    if (!parent) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(parent, path, len);
    parent[len] = '\0';
    r = dyn_fs_mkdirp(parent, mode);
    free(parent);
    if (r != 0)
        return -1;

    if (mkdir(path, mode) == 0)
        return 0;
    if (errno == EEXIST) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = EEXIST;
    }
    return -1;
}

const char* dyn_fs_tempdir_str(void)
{
    const char* t = getenv("TMPDIR");
    if (!t || !*t)
        t = "/tmp";
    return t;
}

int dyn_fs_temp_prefix_ok(const char* prefix, size_t len)
{
    size_t i;
    if (len > 200 || (len && prefix[0] == '.') || strlen(prefix) != len)
        return 0;
    for (i = 0; i < len; i++)
        if (prefix[i] == '/' || prefix[i] == '\\')
            return 0;
    return 1;
}

char* dyn_fs_temp_template(const char* prefix)
{
    const char* t = dyn_fs_tempdir_str();
    size_t tl = strlen(t), pl = prefix ? strlen(prefix) : 0;
    char* tpl;
    while (tl > 1 && t[tl - 1] == '/')
        tl--;
    tpl = (char*)malloc(tl + 1 + pl + 6 + 1);
    if (!tpl)
        return NULL;
    memcpy(tpl, t, tl);
    tpl[tl] = '/';
    if (pl)
        memcpy(tpl + tl + 1, prefix, pl);
    memcpy(tpl + tl + 1 + pl, "XXXXXX", 6);
    tpl[tl + 1 + pl + 6] = '\0';
    return tpl;
}

static char* dyn_fs_parent_dir(const char* p)
{
    const char* slash = strrchr(p, '/');
    size_t n;
    char* d;
    if (!slash)
        return strdup(".");
    n = slash == p ? 1u : (size_t)(slash - p);
    d = (char*)malloc(n + 1);
    if (!d)
        return NULL;
    memcpy(d, p, n);
    d[n] = '\0';
    return d;
}

int dyn_fs_write_atomic(const char* path, const uint8_t* data, size_t len)
{
    static unsigned long seq;
    char *dest, *tmp = NULL, *dir;
    struct stat st;
    int fd = -1, e = 0, have_mode = 0, tries;
    size_t dl, tl, off = 0;

    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        dest = realpath(path, NULL);
        if (!dest)
            return errno;
    } else {
        dest = strdup(path);
        if (!dest)
            return ENOMEM;
    }
    if (stat(dest, &st) == 0)
        have_mode = 1;
    dl = strlen(dest);
    tl = dl + 64;
    tmp = (char*)malloc(tl);
    if (!tmp) {
        free(dest);
        return ENOMEM;
    }
    for (tries = 0; tries < 64; tries++) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        snprintf(tmp, tl, "%s.tmp-%ld-%lx-%lx", dest, (long)getpid(),
            (unsigned long)ts.tv_nsec ^ (unsigned long)(uintptr_t)&ts, __atomic_add_fetch(&seq, 1UL, __ATOMIC_RELAXED));
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    if (fd < 0) {
        e = errno;
        goto out;
    }
    if (have_mode && fchmod(fd, st.st_mode & 07777) < 0) {
        e = errno;
        goto fail;
    }
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            e = errno;
            goto fail;
        }
        off += (size_t)n;
    }
    if (fsync(fd) < 0) {
        e = errno;
        goto fail;
    }
    if (close(fd) < 0) {
        fd = -1;
        e = errno;
        goto unlink_tmp;
    }
    fd = -1;
    if (rename(tmp, dest) < 0) {
        e = errno;
        goto unlink_tmp;
    }
    dir = dyn_fs_parent_dir(dest);
    if (dir) {
        int dfd = open(dir, O_RDONLY | O_CLOEXEC);
        if (dfd >= 0) {
            (void)fsync(dfd);
            close(dfd);
        }
        free(dir);
    }
    goto out;
fail:
    close(fd);
unlink_tmp:
    unlink(tmp);
out:
    free(tmp);
    free(dest);
    return e;
}
