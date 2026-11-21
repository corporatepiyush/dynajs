#include "fsops.h"

void dyn_flock_sleep_ms(int64_t ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

int dyn_flock_try(dyn_flock_t* l, const char* path)
{
    int fd;

#if defined(_WIN32)
    HANDLE h;
    OVERLAPPED ov;
    (void)path;
    if (l->fd < 0) {
        l->fd = _open(path, _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (l->fd < 0)
            return -1;
    }
    h = (HANDLE)_get_osfhandle(l->fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EINVAL;
        return -1;
    }
    memset(&ov, 0, sizeof(ov));
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0, 0, 0xFFFFFFFF, &ov))
        return 0;
    if (GetLastError() == ERROR_LOCK_VIOLATION) {
        errno = EWOULDBLOCK;
        return 1;
    }
    errno = EACCES;
    return -1;
#else
    if (l->excl) {
        fd = open(l->lockfile, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0) {
            l->fd = fd;
            return 0;
        }
        if (errno == EEXIST) {
            errno = EWOULDBLOCK;
            return 1;
        }
        return -1;
    }
    if (l->fd < 0) {
        l->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (l->fd < 0 && errno == EACCES) {
            l->fd = open(path, O_RDONLY | O_CLOEXEC);
        }
        if (l->fd < 0)
            return -1;
    }
    if (flock(l->fd, LOCK_EX | LOCK_NB) == 0)
        return 0;
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        errno = EWOULDBLOCK;
        return 1;
    }
    if (errno == EOPNOTSUPP || errno == ENOLCK || errno == ENOSYS) {
        size_t pl = strlen(path);
        close(l->fd);
        l->fd = -1;
        l->excl = 1;
        l->lockfile = (char*)malloc(pl + 6);
        if (!l->lockfile) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(l->lockfile, path, pl);
        memcpy(l->lockfile + pl, ".lock", 6);
        return dyn_flock_try(l, path);
    }
    return -1;
#endif
}

void dyn_flock_release(dyn_flock_t* l)
{
    if (l->fd < 0)
        return;
#if defined(_WIN32)
    {
        HANDLE h = (HANDLE)_get_osfhandle(l->fd);
        OVERLAPPED ov;
        if (h != INVALID_HANDLE_VALUE) {
            memset(&ov, 0, sizeof(ov));
            UnlockFileEx(h, 0, 0xFFFFFFFF, 0, &ov);
        }
        _close(l->fd);
    }
#else
    if (l->excl) {
        close(l->fd);
        unlink(l->lockfile);
    } else {
        flock(l->fd, LOCK_UN);
        close(l->fd);
    }
#endif
    l->fd = -1;
}
