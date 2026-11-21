#include "fsops.h"

#define DYN_FS_RMRF_MAX_DEPTH 512
#define DYN_FS_RMRF_FD_FALLBACK 16

typedef struct {
    size_t fd_budget;
    int root_fd;
    char** names;
    size_t ncomp;
    size_t names_cap;
} dyn_fs_rmrf_t;

static size_t dyn_fs_rmrf_fd_budget(void)
{
    size_t b = DYN_FS_RMRF_FD_FALLBACK * 4;
#ifndef _WIN32
    struct rlimit rl;

    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        if (rl.rlim_cur > 40)
            b = (size_t)rl.rlim_cur - 24;
        else
            b = rl.rlim_cur > 8 ? (size_t)rl.rlim_cur / 2 : 4;
    }
#endif
    if (b > DYN_FS_RMRF_MAX_DEPTH + 2)
        b = DYN_FS_RMRF_MAX_DEPTH + 2;
    if (b < 4)
        b = 4;
    return b;
}

static int dyn_fs_rmrf_push(dyn_fs_rmrf_t* p, const char* name)
{
    char* d;

    if (p->ncomp == p->names_cap) {
        size_t nc = p->names_cap ? p->names_cap * 2 : 16;
        char** nn = (char**)realloc(p->names, nc * sizeof(*nn));
        if (!nn) {
            errno = ENOMEM;
            return -1;
        }
        p->names = nn;
        p->names_cap = nc;
    }
    d = strdup(name);
    if (!d) {
        errno = ENOMEM;
        return -1;
    }
    p->names[p->ncomp++] = d;
    return 0;
}

static void dyn_fs_rmrf_pop(dyn_fs_rmrf_t* p)
{
    if (p->ncomp)
        free(p->names[--p->ncomp]);
}

static void dyn_fs_rmrf_clear(dyn_fs_rmrf_t* p)
{
    while (p->ncomp)
        dyn_fs_rmrf_pop(p);
    free(p->names);
    p->names = NULL;
    p->names_cap = 0;
}

static int dyn_fs_rmrf_reach(dyn_fs_rmrf_t* p, size_t ncomp, int* out_fd)
{
    int fd, i;

    if (p->root_fd < 0) {
        errno = EBADF;
        return -1;
    }
    fd = dup(p->root_fd);
    if (fd < 0)
        return -1;
    for (i = 0; i < (int)ncomp; i++) {
        int nfd = openat(fd, p->names[i],
            O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        int e = errno;
        close(fd);
        if (nfd < 0) {
            errno = e;
            return -1;
        }
        fd = nfd;
    }
    *out_fd = fd;
    return 0;
}

static int dyn_fs_rmrf_deep(dyn_fs_rmrf_t* p, int depth)
{
    DIR* d;
    struct dirent* e;
    char** dirs = NULL;
    size_t nd = 0, dc = 0, i;
    size_t base = p->ncomp;
    int rc = 0, fd = -1;

    if (depth > DYN_FS_RMRF_MAX_DEPTH) {
        errno = ELOOP;
        return -1;
    }
    if (dyn_fs_rmrf_reach(p, p->ncomp, &fd) != 0)
        return -1;
    d = fdopendir(fd);
    if (!d) {
        int e2 = errno;
        close(fd);
        errno = e2;
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (dyn_is_dot_or_dotdot(e->d_name))
            continue;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = -1;
            break;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (unlinkat(dirfd(d), e->d_name, 0) != 0) {
                rc = -1;
                break;
            }
            continue;
        }
        if (nd == dc) {
            size_t nc2 = dc ? dc * 2 : 16;
            char** np = (char**)realloc(dirs, nc2 * sizeof(*np));
            if (!np) {
                errno = ENOMEM;
                rc = -1;
                break;
            }
            dirs = np;
            dc = nc2;
        }
        dirs[nd] = strdup(e->d_name);
        if (!dirs[nd]) {
            errno = ENOMEM;
            rc = -1;
            break;
        }
        nd++;
    }
    {
        int saved = errno;
        closedir(d);
        if (rc)
            errno = saved;
    }
    for (i = 0; rc == 0 && i < nd; i++) {
        int pfd = -1;
        if (dyn_fs_rmrf_push(p, dirs[i]) < 0) {
            rc = -1;
            break;
        }
        if (dyn_fs_rmrf_deep(p, depth + 1) != 0) {
            rc = -1;
            dyn_fs_rmrf_pop(p);
            break;
        }
        dyn_fs_rmrf_pop(p);
        p->ncomp = base;
        if (dyn_fs_rmrf_reach(p, base, &pfd) != 0) {
            rc = -1;
            break;
        }
        if (unlinkat(pfd, dirs[i], AT_REMOVEDIR) != 0) {
            int e2 = errno;
            close(pfd);
            errno = e2;
            rc = -1;
            break;
        }
        close(pfd);
    }
    p->ncomp = base;
    for (i = 0; i < nd; i++)
        free(dirs[i]);
    free(dirs);
    return rc;
}

static int dyn_fs_rmrf_children(int dirfd, int depth, dyn_fs_rmrf_t* p)
{
    DIR* d;
    struct dirent* e;
    size_t base = p->ncomp;
    int rc = 0;

    if (depth > DYN_FS_RMRF_MAX_DEPTH) {
        close(dirfd);
        errno = ELOOP;
        return -1;
    }
    d = fdopendir(dirfd);
    if (!d) {
        int e2 = errno;
        close(dirfd);
        errno = e2;
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (dyn_is_dot_or_dotdot(e->d_name))
            continue;
        if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            int cfd;
            if (dyn_fs_rmrf_push(p, e->d_name) < 0) {
                rc = -1;
                break;
            }
            if ((size_t)(depth + 1) >= p->fd_budget) {
                if (dyn_fs_rmrf_deep(p, depth + 1) != 0) {
                    rc = -1;
                    dyn_fs_rmrf_pop(p);
                    break;
                }
            } else {
                cfd = openat(dirfd, e->d_name,
                    O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
                if (cfd < 0) {
                    if (errno == EMFILE || errno == ENFILE) {
                        if (dyn_fs_rmrf_deep(p, depth + 1) != 0) {
                            rc = -1;
                            dyn_fs_rmrf_pop(p);
                            break;
                        }
                    } else {
                        rc = -1;
                        dyn_fs_rmrf_pop(p);
                        break;
                    }
                } else {
                    if (dyn_fs_rmrf_children(cfd, depth + 1, p) != 0) {
                        rc = -1;
                        dyn_fs_rmrf_pop(p);
                        break;
                    }
                }
            }
            dyn_fs_rmrf_pop(p);
            p->ncomp = base;
            if (unlinkat(dirfd, e->d_name, AT_REMOVEDIR) != 0) {
                rc = -1;
                break;
            }
        } else {
            if (unlinkat(dirfd, e->d_name, 0) != 0) {
                rc = -1;
                break;
            }
        }
    }
    {
        int saved = errno;
        closedir(d);
        if (rc)
            errno = saved;
    }
    p->ncomp = base;
    return rc;
}

int dyn_fs_remove_tree(const char* path, int* perr, const char** pwhat)
{
    struct stat st;

    *pwhat = NULL;
    if (lstat(path, &st) != 0) {
        int e = errno;
        if (e == ENOENT)
            return 0;
        *perr = e;
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        int fd = dyn_fs_path_open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
        if (fd < 0) {
            *perr = errno;
            *pwhat = path;
            return -1;
        }
        {
            int pin_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
            dyn_fs_rmrf_t p;
            if (pin_fd < 0) {
                close(fd);
                *perr = errno;
                return -1;
            }
            p.fd_budget = dyn_fs_rmrf_fd_budget();
            p.root_fd = pin_fd;
            p.names = NULL;
            p.ncomp = 0;
            p.names_cap = 0;
            if (dyn_fs_rmrf_children(fd, 0, &p) != 0) {
                int e = errno;
                dyn_fs_rmrf_clear(&p);
                close(pin_fd);
                *perr = e;
                *pwhat = path;
                return -1;
            }
            dyn_fs_rmrf_clear(&p);
            {
                struct stat pin, now;
                int bad = fstat(pin_fd, &pin) != 0 || stat(path, &now) != 0
                    || now.st_dev != pin.st_dev || now.st_ino != pin.st_ino;
                close(pin_fd);
                if (bad) {
                    *perr = EXDEV;
                    *pwhat = "directory changed during removal";
                    return -1;
                }
            }
        }
        if (rmdir(path) != 0 && errno != ENOENT) {
            *perr = errno;
            *pwhat = path;
            return -1;
        }
    } else {
        if (unlink(path) != 0 && errno != ENOENT) {
            *perr = errno;
            *pwhat = path;
            return -1;
        }
    }
    return 0;
}
