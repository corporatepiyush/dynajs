#include "libc-internal.h"

#if !defined(_WIN32)
#define DYN_SPAWN_EXTRACT_BEGIN
static int my_execvpe(const char* filename, char** argv, char** envp)
{
    const char *path, *p, *p_next, *p1;
    char buf[PATH_MAX];
    size_t filename_len, path_len;
    int eacces_error;

    filename_len = strlen(filename);
    if (filename_len == 0) {
        errno = ENOENT;
        return -1;
    }
    if (strchr(filename, '/'))
        return execve(filename, argv, envp);

    path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin";
    eacces_error = 0;
    p = path;
    for (p = path; p != NULL; p = p_next) {
        p1 = p;
        while (*p1 && *p1 != ':')
            p1++;
        path_len = p1 - p;
        if (*p1) {
            p_next = p1 + 1;
        } else {
            p_next = NULL;
            p1 = NULL;
        }
        if ((path_len + 1 + filename_len + 1) > PATH_MAX)
            continue;
        memcpy(buf, p, path_len);
        buf[path_len] = '/';
        memcpy(buf + path_len + 1, filename, filename_len);
        buf[path_len + 1 + filename_len] = '\0';

        execve(buf, argv, envp);

        switch (errno) {
        case EACCES:
            eacces_error = 1;
            break;
        case ENOENT:
        case ENOTDIR:
            break;
        default:
            return -1;
        }
    }
    if (eacces_error)
        errno = EACCES;
    return -1;
}

_Thread_local int os_exec_own_group;

static int os_exec_fork_child(const char* file, char** argv, char** envp,
    int std_fds[3], const char* cwd,
    uid_t uid, gid_t gid, int use_path, pid_t* out)
{
    pid_t pid;
    int i;

    pid = fork();
    if (pid < 0)
        return errno;
    if (pid == 0) {
        if (os_exec_own_group)
            setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        for (i = 0; i < 3; i++) {
            if (std_fds[i] != i) {
                if (dup2(std_fds[i], i) < 0)
                    _exit(127);
            }
        }
        dyn_close_fds_from(3);
        if (cwd) {
            if (chdir(cwd) < 0)
                _exit(127);
        }
        if (gid != (gid_t)-1) {
            if (setgid(gid) < 0)
                _exit(127);
        }
        if (uid != (uid_t)-1) {
            if (setuid(uid) < 0)
                _exit(127);
        }

        if (use_path)
            my_execvpe(file, argv, envp);
        else
            execve(file, argv, envp);
        _exit(127);
    }
    *out = pid;
    return 0;
}

int os_exec_spawn_child(const char* file, char** argv, char** envp,
    int std_fds[3], const char* cwd,
    uid_t uid, gid_t gid, int use_path, pid_t* out)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t sa;
    short flags = 0;
    int i, rc;
    pid_t pid = 0;

#if defined(POSIX_SPAWN_SETUID) && defined(POSIX_SPAWN_SETGID)
#else
    if (uid != (uid_t)-1 || gid != (gid_t)-1)
        return os_exec_fork_child(file, argv, envp, std_fds, cwd,
            uid, gid, use_path, out);
#endif
#ifndef DYN_SPAWN_HAVE_ADDCHDIR
    if (cwd)
        return os_exec_fork_child(file, argv, envp, std_fds, cwd,
            uid, gid, use_path, out);
#endif
#if !(defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)) && !defined(DYN_SPAWN_HAVE_ADDCLOSEFROM)
    return os_exec_fork_child(file, argv, envp, std_fds, cwd, uid, gid, use_path, out);
#endif

    for (i = 0; i < 3; i++) {
        if (std_fds[i] != i && fcntl(std_fds[i], F_GETFD) < 0)
            return errno ? errno : EBADF;
    }

    rc = posix_spawn_file_actions_init(&fa);
    if (rc == 0)
        rc = posix_spawnattr_init(&sa);
    if (rc == 0) {
        sigset_t dfl;
        sigemptyset(&dfl);
        sigaddset(&dfl, SIGPIPE);
        rc = posix_spawnattr_setsigdefault(&sa, &dfl);
        flags |= POSIX_SPAWN_SETSIGDEF;
    }
#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    for (i = 0; rc == 0 && i < 3; i++) {
        if (std_fds[i] != i)
            rc = posix_spawn_file_actions_adddup2(&fa, std_fds[i], i);
        else
            rc = posix_spawn_file_actions_adddup2(&fa, i, i);
    }
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
    for (i = 0; rc == 0 && i < 3; i++) {
        if (std_fds[i] != i)
            rc = posix_spawn_file_actions_adddup2(&fa, std_fds[i], i);
    }
#ifdef DYN_SPAWN_HAVE_ADDCLOSEFROM
    if (rc == 0)
        rc = posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif
#endif
#ifdef DYN_SPAWN_HAVE_ADDCHDIR
    if (rc == 0 && cwd) {
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        rc = posix_spawn_file_actions_addchdir_np(&fa, cwd);
#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
    }
#endif
#ifdef POSIX_SPAWN_USEVFORK
    flags |= POSIX_SPAWN_USEVFORK;
#endif
#if defined(POSIX_SPAWN_SETUID) && defined(POSIX_SPAWN_SETGID)
    if (rc == 0 && gid != (gid_t)-1) {
        flags |= POSIX_SPAWN_SETGID;
        rc = posix_spawnattr_setgid(&sa, gid);
    }
    if (rc == 0 && uid != (uid_t)-1) {
        flags |= POSIX_SPAWN_SETUID;
        rc = posix_spawnattr_setuid(&sa, uid);
    }
#endif
    if (rc == 0 && os_exec_own_group) {
        flags |= POSIX_SPAWN_SETPGROUP;
        rc = posix_spawnattr_setpgroup(&sa, 0);
    }
    if (rc == 0)
        rc = posix_spawnattr_setflags(&sa, flags);
    if (rc == 0) {
        if (use_path)
            rc = posix_spawnp(&pid, file, &fa, &sa, argv, envp);
        else
            rc = posix_spawn(&pid, file, &fa, &sa, argv, envp);
    }
    posix_spawnattr_destroy(&sa);
    posix_spawn_file_actions_destroy(&fa);
    if (rc)
        return rc;
    *out = pid;
    return 0;
}
#define DYN_SPAWN_EXTRACT_END
#endif
