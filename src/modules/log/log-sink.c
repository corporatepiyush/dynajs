#include "log-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

#define DYN_LOG_FILE_MODE 0640
#define DYN_LOG_PATH_MAX 4096

static dyn_log_sink_t* dyn_sinks;

static void dyn_sink_write_failed(dyn_log_sink_t* s, int fd)
{
    char msg[192];
    int w;
    s->write_errors++;
    if (s->write_errors != 1 || fd == STDERR_FILENO)
        return;
    w = snprintf(msg, sizeof msg,
        "dyna:log: write to %s failed (%s); log lines are being lost\n",
        s->active ? s->active : (s->path ? s->path : "?"),
        strerror(errno));
    if (w > 0)
        (void)dyn_write_all(STDERR_FILENO, msg, (size_t)w);
}

static int dyn_mkdir_p(const char* path)
{
    char tmp[DYN_LOG_PATH_MAX];
    size_t n = strlen(path);
    char* p;
    if (n == 0 || n >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static const char* dyn_split_dir(const char* path, char* dir, size_t cap)
{
    const char* slash = strrchr(path, '/');
    size_t n;
    if (!slash) {
        dir[0] = '\0';
        return path;
    }
    n = (size_t)(slash - path);
    if (n >= cap)
        n = cap - 1;
    memcpy(dir, path, n);
    dir[n] = '\0';
    return slash + 1;
}

static int64_t dyn_period_of(int64_t freq_ms, int64_t now)
{
    return now - now % freq_ms;
}

static void dyn_fmt_date(int64_t period_ms, int hourly, char* out, size_t cap)
{
    time_t t = (time_t)(period_ms / 1000);
    struct tm tmv;
    if (!gmtime_r(&t, &tmv))
        memset(&tmv, 0, sizeof tmv);
    if (hourly)
        snprintf(out, cap, "%04d-%02d-%02dT%02d",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour);
    else
        snprintf(out, cap, "%04d-%02d-%02d",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

static int dyn_sink_name(const dyn_log_sink_t* s, unsigned n, char* out,
    size_t cap)
{
    char date[32];
    int w;
    date[0] = '\0';
    if (s->named_date)
        dyn_fmt_date(s->period_ms, s->freq_ms == 3600000, date, sizeof date);
    if (s->named_date && s->numbered)
        w = snprintf(out, cap, "%s.%s.%u%s", s->base, date, n, s->ext);
    else if (s->named_date)
        w = snprintf(out, cap, "%s.%s%s", s->base, date, s->ext);
    else if (s->numbered)
        w = snprintf(out, cap, "%s.%u%s", s->base, n, s->ext);
    else
        w = snprintf(out, cap, "%s%s", s->base, s->ext);
    return (w < 0 || (size_t)w >= cap) ? -1 : 0;
}

static unsigned dyn_sink_scan_max_n(const dyn_log_sink_t* s)
{
    char dir[DYN_LOG_PATH_MAX];
    char date[32];
    const char* bn = dyn_split_dir(s->base, dir, sizeof dir);
    size_t bnl = strlen(bn), dl = 0;
    unsigned maxn = 0;
    DIR* d = opendir(dir[0] ? dir : ".");
    struct dirent* de;

    if (s->named_date) {
        dyn_fmt_date(s->period_ms, s->freq_ms == 3600000, date, sizeof date);
        dl = strlen(date);
    }
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        const char* mid = de->d_name + bnl;
        unsigned v = 0;
        int digits = 0;
        if (strncmp(de->d_name, bn, bnl) != 0 || *mid != '.')
            continue;
        mid++;
        if (dl) {
            if (strncmp(mid, date, dl) != 0 || mid[dl] != '.')
                continue;
            mid += dl + 1;
        }
        while (*mid >= '0' && *mid <= '9') {
            v = v * 10u + (unsigned)(*mid - '0');
            mid++;
            digits++;
        }
        if (digits && strcmp(mid, s->ext) == 0 && v > maxn)
            maxn = v;
    }
    closedir(d);
    return maxn;
}

static int dyn_sink_open_numbered(dyn_log_sink_t* s, char* out, size_t cap)
{
    unsigned start = dyn_sink_scan_max_n(s);
    unsigned n;
    for (n = start + 1; n < start + 4096; n++) {
        int fd;
        if (dyn_sink_name(s, n, out, cap) < 0)
            return -1;
        fd = open(out, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, DYN_LOG_FILE_MODE);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

void dyn_sink_flush(dyn_log_sink_t* s)
{
    if (s->buf && s->bufn) {
        int rc;
        if (s->fd >= 0) {
            rc = dyn_write_all(s->fd, s->buf, s->bufn);
        } else {
            rc = dyn_write_all(STDERR_FILENO, s->buf, s->bufn);
        }
        if (rc < 0)
            dyn_sink_write_failed(s, s->fd >= 0 ? s->fd : STDERR_FILENO);
        s->bufn = 0;
    }
}

static void dyn_sink_prune(dyn_log_sink_t* s);

static void dyn_sink_rotate(dyn_log_sink_t* s, int64_t now)
{
    char path[DYN_LOG_PATH_MAX];
    int fd;
    struct stat st;

    dyn_sink_flush(s);
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    free(s->active);
    s->active = NULL;
    s->written = 0;
    if (s->freq_ms)
        s->period_ms = dyn_period_of(s->freq_ms, now);
    if (s->numbered)
        fd = dyn_sink_open_numbered(s, path, sizeof path);
    else {
        if (dyn_sink_name(s, 0, path, sizeof path) < 0) {
            s->last_retry = now;
            return;
        }
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, DYN_LOG_FILE_MODE);
    }
    if (fd < 0) {
        s->last_retry = now;
        return;
    }
    s->fd = fd;
    if (fstat(fd, &st) == 0)
        s->written = (uint64_t)st.st_size;
    s->active = strdup(path);
    if (s->use_symlink && s->active) {
        const char* abn = strrchr(s->active, '/');
        char tmp[DYN_LOG_PATH_MAX];
        abn = abn ? abn + 1 : s->active;
        if (snprintf(tmp, sizeof tmp, "%s.lnk", s->path) < (int)sizeof tmp) {
            unlink(tmp);
            if (symlink(abn, tmp) == 0)
                rename(tmp, s->path);
        }
    }
    dyn_sink_prune(s);
}

static void dyn_sink_prune(dyn_log_sink_t* s)
{
    char dir[DYN_LOG_PATH_MAX];
    const char* bn;
    size_t bnl, extl;
    DIR* d;
    typedef struct {
        char* name;
        char date[32];
        long num;
    } dyn_rot_t;
    dyn_rot_t* ents = NULL;
    int cnt = 0, cap = 256, i;
    struct dirent* de;
    const char* abn2 = s->active ? strrchr(s->active, '/') : NULL;

    if (!s->keep_count || !s->base)
        return;
    bn = dyn_split_dir(s->base, dir, sizeof dir);
    bnl = strlen(bn);
    extl = strlen(s->ext);
    abn2 = s->active ? (abn2 ? abn2 + 1 : s->active) : NULL;
    d = opendir(dir[0] ? dir : ".");
    if (d) {
        ents = (dyn_rot_t*)malloc((size_t)cap * sizeof *ents);
        while ((de = readdir(d)) != NULL) {
            const char *mid, *dot;
            size_t mlen, k;
            if (ents && cnt >= cap) {
                dyn_rot_t* ne;
                if (cap >= 4096)
                    continue;
                ne = (dyn_rot_t*)realloc(ents, (size_t)(cap * 2) * sizeof *ne);
                if (!ne)
                    continue;
                ents = ne;
                cap *= 2;
            }
            if (abn2 && strcmp(de->d_name, abn2) == 0)
                continue;
            if (strncmp(de->d_name, bn, bnl) != 0 || de->d_name[bnl] != '.')
                continue;
            mlen = strlen(de->d_name);
            if (mlen < bnl + 1 + extl || strcmp(de->d_name + mlen - extl, s->ext) != 0)
                continue;
            mid = de->d_name + bnl + 1;
            mlen -= bnl + 1 + extl;
            if (!mlen)
                continue;
            for (k = 0; k < mlen; k++) {
                char c = mid[k];
                if (!((c >= '0' && c <= '9') || c == '-' || c == 'T' || c == '.'))
                    break;
            }
            if (k != mlen)
                continue;
            if (!ents)
                continue;
            dot = memchr(mid, '.', mlen);
            ents[cnt].name = strdup(de->d_name);
            if (!ents[cnt].name)
                continue;
            if (s->named_date && !s->numbered) {
                size_t dl = mlen;
                if (dl >= sizeof ents[cnt].date)
                    dl = sizeof ents[cnt].date - 1;
                memcpy(ents[cnt].date, mid, dl);
                ents[cnt].date[dl] = '\0';
                ents[cnt].num = 0;
            } else if (dot) {
                size_t dl = (size_t)(dot - mid);
                if (dl >= sizeof ents[cnt].date)
                    dl = sizeof ents[cnt].date - 1;
                memcpy(ents[cnt].date, mid, dl);
                ents[cnt].date[dl] = '\0';
                ents[cnt].num = strtol(dot + 1, NULL, 10);
            } else {
                ents[cnt].date[0] = '\0';
                ents[cnt].num = strtol(mid, NULL, 10);
            }
            cnt++;
        }
        closedir(d);
    }
    if (ents && cnt > s->keep_count) {
        int over = cnt - s->keep_count;
        for (i = 1; i < cnt; i++) {
            dyn_rot_t key = ents[i];
            int j = i - 1;
            while (j >= 0 && (strcmp(ents[j].date, key.date) > 0 || (strcmp(ents[j].date, key.date) == 0 && ents[j].num > key.num))) {
                ents[j + 1] = ents[j];
                j--;
            }
            ents[j + 1] = key;
        }
        for (i = 0; i < over; i++) {
            char full[DYN_LOG_PATH_MAX];
            if (snprintf(full, sizeof full, "%s/%s",
                    dir[0] ? dir : ".", ents[i].name)
                < (int)sizeof full)
                unlink(full);
        }
    }
    if (ents) {
        for (i = 0; i < cnt; i++)
            free(ents[i].name);
        free(ents);
    }
}

static void dyn_sink_maybe_rotate(dyn_log_sink_t* s, int64_t now, size_t n)
{
    if (s->fd < 0) {
        if (now >= s->last_retry && now - s->last_retry >= 1000)
            dyn_sink_rotate(s, now);
        return;
    }
    if ((s->freq_ms && now >= s->period_ms + s->freq_ms) || (s->max_size && s->written > 0 && s->written + n > s->max_size))
        dyn_sink_rotate(s, now);
}

static void dyn_sink_write_locked(dyn_log_sink_t* s, int64_t now,
    const char* data, size_t n)
{
    if (!s) {
        (void)dyn_write_all(STDERR_FILENO, data, n);
        return;
    }
    if (s->max_size || s->freq_ms)
        dyn_sink_maybe_rotate(s, now, n);
    if (s->fd < 0) {
        if (dyn_write_all(STDERR_FILENO, data, n) < 0)
            dyn_sink_write_failed(s, STDERR_FILENO);
        return;
    }
    if (!s->buf) {
        if (dyn_write_all(s->fd, data, n) < 0)
            dyn_sink_write_failed(s, s->fd);
        s->written += n;
        return;
    }
    if (s->bufn + n > s->bufcap)
        dyn_sink_flush(s);
    if (n > s->bufcap) {
        if (dyn_write_all(s->fd, data, n) < 0)
            dyn_sink_write_failed(s, s->fd);
        s->written += n;
        return;
    }
    memcpy(s->buf + s->bufn, data, n);
    s->bufn += n;
    s->written += n;
    if (s->bufn == s->bufcap)
        dyn_sink_flush(s);
}

void dyn_sink_write(dyn_log_sink_t* s, int64_t now,
    const char* data, size_t n)
{
    pthread_mutex_lock(&dyn_log_mu);
    dyn_sink_write_locked(s, now, data, n);
    pthread_mutex_unlock(&dyn_log_mu);
}

static void dyn_sink_free(dyn_log_sink_t* s)
{
    dyn_log_sink_t** pp;
    for (pp = &dyn_sinks; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    dyn_sink_flush(s);
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    free(s->path);
    free(s->base);
    free(s->ext);
    free(s->active);
    free(s);
}

void dyn_sink_unref(dyn_log_sink_t* s)
{
    if (!s)
        return;
    pthread_mutex_lock(&dyn_log_mu);
    if (--s->refs <= 0)
        dyn_sink_free(s);
    pthread_mutex_unlock(&dyn_log_mu);
}

static void dyn_sinks_atexit(void)
{
    dyn_log_sink_t* s;
    pthread_mutex_lock(&dyn_log_mu);
    for (s = dyn_sinks; s; s = s->next)
        dyn_sink_flush(s);
    pthread_mutex_unlock(&dyn_log_mu);
}

static dyn_log_sink_t* dyn_sink_new_locked(JSContext* ctx, const char* path,
    uint64_t max_size, int64_t freq_ms,
    int named_date, int keep_count,
    int use_symlink, size_t bufcap, int do_mkdir);

dyn_log_sink_t* dyn_sink_new(JSContext* ctx, const char* path,
    uint64_t max_size, int64_t freq_ms,
    int named_date, int keep_count,
    int use_symlink, size_t bufcap, int do_mkdir)
{
    dyn_log_sink_t* s;
    pthread_mutex_lock(&dyn_log_mu);
    s = dyn_sink_new_locked(ctx, path, max_size, freq_ms, named_date,
        keep_count, use_symlink, bufcap, do_mkdir);
    pthread_mutex_unlock(&dyn_log_mu);
    return s;
}

static dyn_log_sink_t* dyn_sink_new_locked(JSContext* ctx, const char* path,
    uint64_t max_size, int64_t freq_ms,
    int named_date, int keep_count,
    int use_symlink, size_t bufcap, int do_mkdir)
{
    dyn_log_sink_t *s, *found;
    int rotation;
    int64_t now;

    for (found = dyn_sinks; found; found = found->next) {
        if (strcmp(found->path, path) == 0)
            break;
    }
    if (found) {
        const char* which = NULL;
        if (found->max_size != max_size)
            which = "rollover.size";
        else if (found->freq_ms != freq_ms)
            which = "rollover.frequency";
        else if (found->named_date != named_date)
            which = "rollover.frequency";
        else if (found->keep_count != keep_count)
            which = "rollover.count";
        else if (found->use_symlink != use_symlink)
            which = "rollover.symlink";
        else if (found->bufcap != bufcap)
            which = "buffer";
        if (which) {
            JS_ThrowRangeError(ctx,
                "Logger: dest is already open with a different %s", which);
            return NULL;
        }
        found->refs++;
        return found;
    }

    s = (dyn_log_sink_t*)calloc(1, sizeof *s);
    if (!s) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    s->path = strdup(path);
    if (!s->path) {
        free(s);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    s->max_size = max_size;
    s->freq_ms = freq_ms;
    s->named_date = named_date;
    s->numbered = max_size != 0 || (freq_ms != 0 && !named_date);
    s->keep_count = keep_count;
    s->use_symlink = use_symlink;
    s->fd = -1;
    rotation = max_size != 0 || freq_ms != 0;

    if (do_mkdir) {
        char dir[DYN_LOG_PATH_MAX];
        dyn_split_dir(path, dir, sizeof dir);
        if (dir[0] && dyn_mkdir_p(dir) != 0) {
            JS_ThrowRangeError(ctx, "Logger: cannot mkdir %s: %s", dir,
                strerror(errno));
            free(s->path);
            free(s);
            return NULL;
        }
    }

    now = log_now_ms();
    if (rotation) {
        const char* slash = strrchr(path, '/');
        const char* dot = strrchr(slash ? slash : path, '.');
        const char* bn0 = slash ? slash + 1 : path;
        size_t bl;
        if (dot && dot > bn0) {
            bl = (size_t)(dot - path);
            s->ext = strdup(dot);
        } else {
            bl = strlen(path);
            s->ext = strdup(".log");
        }
        s->base = (char*)malloc(bl + 1);
        if (!s->base || !s->ext) {
            int oom = !s->base || !s->ext;
            free(s->base);
            free(s->ext);
            free(s->path);
            free(s);
            if (oom)
                JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        memcpy(s->base, path, bl);
        s->base[bl] = '\0';
        if (freq_ms)
            s->period_ms = dyn_period_of(freq_ms, now);
        if (s->numbered) {
            char p0[DYN_LOG_PATH_MAX];
            int fd = dyn_sink_open_numbered(s, p0, sizeof p0);
            if (fd < 0) {
                JS_ThrowRangeError(ctx, "Logger: cannot open %s: %s", p0,
                    strerror(errno));
                free(s->base);
                free(s->ext);
                free(s->path);
                free(s);
                return NULL;
            }
            s->fd = fd;
            s->active = strdup(p0);
        } else {
            char p0[DYN_LOG_PATH_MAX];
            if (dyn_sink_name(s, 0, p0, sizeof p0) < 0) {
                JS_ThrowRangeError(ctx, "Logger: dest path too long");
                free(s->base);
                free(s->ext);
                free(s->path);
                free(s);
                return NULL;
            }
            s->fd = open(p0, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, DYN_LOG_FILE_MODE);
            if (s->fd < 0) {
                JS_ThrowRangeError(ctx, "Logger: cannot open %s: %s", p0,
                    strerror(errno));
                free(s->base);
                free(s->ext);
                free(s->path);
                free(s);
                return NULL;
            }
            s->active = strdup(p0);
        }
        if (!s->active) {
            close(s->fd);
            s->fd = -1;
            free(s->base);
            free(s->ext);
            free(s->path);
            free(s);
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
    } else {
        s->base = strdup(path);
        s->fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, DYN_LOG_FILE_MODE);
        if (s->fd < 0) {
            JS_ThrowRangeError(ctx, "Logger: cannot open %s: %s", path,
                strerror(errno));
            free(s->base);
            free(s->path);
            free(s);
            return NULL;
        }
        s->active = strdup(path);
        if (!s->active) {
            close(s->fd);
            s->fd = -1;
            free(s->base);
            free(s->path);
            free(s);
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
    }
    {
        struct stat st;
        if (fstat(s->fd, &st) == 0)
            s->written = (uint64_t)st.st_size;
    }
    if (rotation) {
        if (s->use_symlink && s->active) {
            const char* abn = strrchr(s->active, '/');
            char tmp[DYN_LOG_PATH_MAX];
            abn = abn ? abn + 1 : s->active;
            if (snprintf(tmp, sizeof tmp, "%s.lnk", s->path) < (int)sizeof tmp) {
                unlink(tmp);
                if (symlink(abn, tmp) == 0)
                    rename(tmp, s->path);
            }
        }
        dyn_sink_prune(s);
    }
    if (bufcap) {
        s->buf = (char*)malloc(bufcap);
        if (!s->buf) {
            dyn_sink_free(s);
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        s->bufcap = bufcap;
    }
    s->refs = 1;
    s->next = dyn_sinks;
    dyn_sinks = s;
    {
        static int atexit_done;
        if (!atexit_done) {
            atexit_done = 1;
            atexit(dyn_sinks_atexit);
        }
    }
    return s;
}

#endif
