#include "sftp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SFTP) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <stdio.h>
#include <string.h>
#include <time.h>

static int u64_get(sshr_t* r, uint64_t* v)
{
    uint32_t hi = sshr_u32(r), lo = sshr_u32(r);
    *v = ((uint64_t)hi << 32) | lo;
    return r->err ? -1 : 0;
}

static int u64_put(sshb_t* b, uint64_t v)
{
    return sshb_u32(b, (uint32_t)(v >> 32)) || sshb_u32(b, (uint32_t)v) ? -1 : 0;
}

int sftp_attr_get(sshr_t* r, sftp_attr_t* a)
{
    memset(a, 0, sizeof *a);
    a->flags = sshr_u32(r);
    if (a->flags & SFTP_A_SIZE)
        u64_get(r, &a->size);
    if (a->flags & SFTP_A_UIDGID) {
        a->uid = sshr_u32(r);
        a->gid = sshr_u32(r);
    }
    if (a->flags & SFTP_A_PERM)
        a->perm = sshr_u32(r);
    if (a->flags & SFTP_A_TIME) {
        a->atime = sshr_u32(r);
        a->mtime = sshr_u32(r);
    }
    if (a->flags & SFTP_A_EXT) {
        uint32_t i, n = sshr_u32(r);
        size_t l;
        if (n > 64)
            r->err = 1;
        for (i = 0; i < n && !r->err; i++) {
            sshr_str(r, &l);
            sshr_str(r, &l);
        }
    }
    a->flags &= SFTP_A_SIZE | SFTP_A_UIDGID | SFTP_A_PERM | SFTP_A_TIME;
    return r->err ? -1 : 0;
}

int sftp_attr_put(sshb_t* b, const sftp_attr_t* a)
{
    uint32_t f = a->flags & (SFTP_A_SIZE | SFTP_A_UIDGID | SFTP_A_PERM | SFTP_A_TIME);
    if (sshb_u32(b, f))
        return -1;
    if ((f & SFTP_A_SIZE) && u64_put(b, a->size))
        return -1;
    if ((f & SFTP_A_UIDGID) && (sshb_u32(b, a->uid) || sshb_u32(b, a->gid)))
        return -1;
    if ((f & SFTP_A_PERM) && sshb_u32(b, a->perm))
        return -1;
    if ((f & SFTP_A_TIME) && (sshb_u32(b, a->atime) || sshb_u32(b, a->mtime)))
        return -1;
    return 0;
}

void sftp_attr_stat(const struct stat* st, sftp_attr_t* a)
{
    a->flags = SFTP_A_SIZE | SFTP_A_UIDGID | SFTP_A_PERM | SFTP_A_TIME;
    a->size = (uint64_t)st->st_size;
    a->uid = (uint32_t)st->st_uid;
    a->gid = (uint32_t)st->st_gid;
    a->perm = (uint32_t)st->st_mode;
    a->atime = (uint32_t)st->st_atime;
    a->mtime = (uint32_t)st->st_mtime;
}

void sftp_longname(const sftp_attr_t* a, const char* name, size_t nl, sshb_t* out)
{
    char mode[11], line[160], when[32];
    uint32_t p = a->perm;
    time_t t = (time_t)a->mtime;
    struct tm tmv;
    uint32_t ty = p & S_IFMT;
    int n;
    mode[0] = ty == S_IFDIR ? 'd' : ty == S_IFLNK ? 'l' : ty == S_IFCHR ? 'c' : ty == S_IFBLK ? 'b' : ty == S_IFIFO ? 'p' : ty == S_IFSOCK ? 's' : '-';
    mode[1] = p & 0400 ? 'r' : '-';
    mode[2] = p & 0200 ? 'w' : '-';
    mode[3] = p & 04000 ? (p & 0100 ? 's' : 'S') : (p & 0100 ? 'x' : '-');
    mode[4] = p & 040 ? 'r' : '-';
    mode[5] = p & 020 ? 'w' : '-';
    mode[6] = p & 02000 ? (p & 010 ? 's' : 'S') : (p & 010 ? 'x' : '-');
    mode[7] = p & 04 ? 'r' : '-';
    mode[8] = p & 02 ? 'w' : '-';
    mode[9] = p & 01000 ? (p & 01 ? 't' : 'T') : (p & 01 ? 'x' : '-');
    mode[10] = 0;
    if (!gmtime_r(&t, &tmv) || !strftime(when, sizeof when, "%b %e %H:%M", &tmv))
        snprintf(when, sizeof when, "?");
    n = snprintf(line, sizeof line, "%s    1 %-8u %-8u %8llu %s ", mode, (unsigned)a->uid, (unsigned)a->gid, (unsigned long long)a->size, when);
    if (n < 0 || (size_t)n >= sizeof line)
        n = 0;
    out->len = 0;
    (void)(sshb_put(out, line, (size_t)n) || sshb_put(out, name, nl));
}

const char* sftp_status_name(uint32_t code)
{
    static const char* const names[] = { "OK", "EOF", "NO_SUCH_FILE", "PERMISSION_DENIED", "FAILURE", "BAD_MESSAGE",
        "NO_CONNECTION", "CONNECTION_LOST", "OP_UNSUPPORTED" };
    return code < countof(names) ? names[code] : "FAILURE";
}

int sftp_pkt_begin(sshb_t* b, uint8_t type)
{
    b->len = 0;
    return sshb_u32(b, 0) || sshb_u8(b, type) ? -1 : 0;
}

void sftp_pkt_end(sshb_t* b)
{
    ssh_put_be32(b->p, (uint32_t)(b->len - 4));
}

#endif
