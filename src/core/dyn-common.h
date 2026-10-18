#ifndef DYN_COMMON_H
#define DYN_COMMON_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dyn-cdefs.h"
#include "dyn-sb.h"

static inline uint16_t dyn_ld16le(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)(b[0] | (uint16_t)b[1] << 8);
}

static inline uint32_t dyn_ld32le(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static inline uint64_t dyn_ld64le(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint64_t)dyn_ld32le(b) | (uint64_t)dyn_ld32le(b + 4) << 32;
}

static inline uint16_t dyn_ld16be(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)((uint16_t)b[0] << 8 | b[1]);
}

static inline uint32_t dyn_ld32be(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
}

static inline uint64_t dyn_ld64be(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint64_t)dyn_ld32be(b) << 32 | (uint64_t)dyn_ld32be(b + 4);
}

static inline void dyn_st16le(void* p, uint16_t v)
{
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static inline void dyn_st32le(void* p, uint32_t v)
{
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static inline void dyn_st64le(void* p, uint64_t v)
{
    uint8_t* b = (uint8_t*)p;
    dyn_st32le(b, (uint32_t)v);
    dyn_st32le(b + 4, (uint32_t)(v >> 32));
}

static inline void dyn_st16be(void* p, uint16_t v)
{
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

static inline void dyn_st32be(void* p, uint32_t v)
{
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

static inline void dyn_st64be(void* p, uint64_t v)
{
    uint8_t* b = (uint8_t*)p;
    dyn_st32be(b, (uint32_t)(v >> 32));
    dyn_st32be(b + 4, (uint32_t)v);
}

static inline uint32_t dyn_rotl32(uint32_t x, int n)
{
    unsigned s = (unsigned)n & 31u;
    return s ? (x << s) | (x >> (32 - s)) : x;
}

static inline uint32_t dyn_rotr32(uint32_t x, int n)
{
    unsigned s = (unsigned)n & 31u;
    return s ? (x >> s) | (x << (32 - s)) : x;
}

static inline uint64_t dyn_rotl64(uint64_t x, int n)
{
    unsigned s = (unsigned)n & 63u;
    return s ? (x << s) | (x >> (64 - s)) : x;
}

static inline uint64_t dyn_rotr64(uint64_t x, int n)
{
    unsigned s = (unsigned)n & 63u;
    return s ? (x >> s) | (x << (64 - s)) : x;
}

static inline int dyn_ascii_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int dyn_ascii_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int dyn_ascii_islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int dyn_ascii_isalpha(int c) { return dyn_ascii_isupper(c) || dyn_ascii_islower(c); }
static inline int dyn_ascii_isalnum(int c) { return dyn_ascii_isalpha(c) || dyn_ascii_isdigit(c); }
static inline int dyn_ascii_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int dyn_ascii_tolower(int c) { return dyn_ascii_isupper(c) ? c + ('a' - 'A') : c; }
static inline int dyn_ascii_toupper(int c) { return dyn_ascii_islower(c) ? c - ('a' - 'A') : c; }

static inline int dyn_hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static inline int dyn_ascii_ieq_lower_lit(const char* a, size_t an, const char* lower_lit)
{
    size_t i;
    for (i = 0; i < an; i++)
        if (lower_lit[i] == '\0' || dyn_ascii_tolower((unsigned char)a[i]) != lower_lit[i])
            return 0;
    return lower_lit[an] == '\0';
}

static inline int dyn_ascii_ieq(const char* a, size_t an, const char* b, size_t bn)
{
    size_t i;
    if (an != bn)
        return 0;
    for (i = 0; i < an; i++)
        if (dyn_ascii_tolower((unsigned char)a[i]) != dyn_ascii_tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

static inline int dyn_size_mul(size_t a, size_t b, size_t* out)
{
    return __builtin_mul_overflow(a, b, out) ? -1 : 0;
}

static inline int dyn_size_add(size_t a, size_t b, size_t* out)
{
    return __builtin_add_overflow(a, b, out) ? -1 : 0;
}

static inline int dyn_utf8_encode(uint32_t cp, uint8_t out[4])
{
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

static inline int dyn_utf8_decode(const uint8_t* s, size_t n, uint32_t* cp)
{
    uint32_t c, v;
    int len, k;
    if (n == 0)
        return 0;
    c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        v = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        v = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        v = c & 0x07;
    } else
        return -1;
    if (n < (size_t)len)
        return -1;
    for (k = 1; k < len; k++) {
        if ((s[k] & 0xC0) != 0x80)
            return -1;
        v = (v << 6) | (s[k] & 0x3F);
    }
    if ((len == 3 && (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF))) || (len == 4 && (v < 0x10000 || v > 0x10FFFF)))
        return -1;
    *cp = v;
    return len;
}

static inline int dyn_fd_set_cloexec(int fd)
{
    return fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ? -1 : 0;
}

static inline int dyn_fd_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    if (fl & O_NONBLOCK)
        return 0;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0;
}

static inline int dyn_pipe_cloexec(int fds[2])
{
#if defined(__linux__) && defined(_GNU_SOURCE) && defined(O_CLOEXEC)
    if (pipe2(fds, O_CLOEXEC) == 0)
        return 0;
    if (errno != ENOSYS && errno != EINVAL)
        return -1;
#endif
    if (pipe(fds) < 0)
        return -1;
    if (dyn_fd_set_cloexec(fds[0]) < 0 || dyn_fd_set_cloexec(fds[1]) < 0) {
        int e = errno;
        close(fds[0]);
        close(fds[1]);
        errno = e;
        return -1;
    }
    return 0;
}

static inline ssize_t dyn_read_eintr(int fd, void* buf, size_t n)
{
    ssize_t r;
    do
        r = read(fd, buf, n);
    while (r < 0 && errno == EINTR);
    return r;
}

static inline ssize_t dyn_write_eintr(int fd, const void* buf, size_t n)
{
    ssize_t r;
    do
        r = write(fd, buf, n);
    while (r < 0 && errno == EINTR);
    return r;
}

static inline ssize_t dyn_recv_eintr(int fd, void* buf, size_t n, int flags)
{
    ssize_t r;
    do
        r = recv(fd, buf, n, flags);
    while (r < 0 && errno == EINTR);
    return r;
}

static inline ssize_t dyn_send_eintr(int fd, const void* buf, size_t n, const struct sockaddr* peer, socklen_t peerlen)
{
    ssize_t r;
    do
        r = peer ? sendto(fd, buf, n, 0, peer, peerlen) : send(fd, buf, n, 0);
    while (r < 0 && errno == EINTR);
    return r;
}

static inline pid_t dyn_waitpid_eintr(pid_t pid, int* status, int options)
{
    pid_t r;
    do
        r = waitpid(pid, status, options);
    while (r < 0 && errno == EINTR);
    return r;
}

static inline int dyn_write_all(int fd, const void* buf, size_t n)
{
    const uint8_t* p = (const uint8_t*)buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

#endif
