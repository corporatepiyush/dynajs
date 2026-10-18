/* src/core/dyn-common.h is the shared layer every module trusts, so each helper is checked against an
   INDEPENDENT reference (libc, memcpy + byte swap, a strlen-based compare) rather than against itself:
   byte-order loads/stores, ASCII classification over all 256 byte values in the C locale, hex digits,
   case-insensitive literal compare, overflow-checked size arithmetic, UTF-8 over every scalar value plus
   the malformed forms a decoder must refuse, and the fd wrappers observed through fcntl. */
#include "dyn-common.h"

#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...)                               \
    do {                                            \
        if (!(c)) {                                 \
            fails++;                                \
            if (fails < 20) {                       \
                printf("FAIL line %d: ", __LINE__); \
                printf(__VA_ARGS__);                \
                printf("\n");                       \
            }                                       \
        }                                           \
    } while (0)

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static int host_le(void)
{
    uint16_t v = 1;
    uint8_t b;
    memcpy(&b, &v, 1);
    return b == 1;
}

static void test_byte_order(void)
{
    uint8_t buf[16];
    int t, off;
    for (t = 0; t < 20000; t++) {
        uint32_t r32;
        uint64_t r64;
        uint16_t r16;
        for (off = 0; off < 16; off++)
            buf[off] = (uint8_t)rnd();
        off = (int)(rnd() % 8);
        memcpy(&r16, buf + off, 2);
        memcpy(&r32, buf + off, 4);
        memcpy(&r64, buf + off, 8);
        if (!host_le()) {
            r16 = __builtin_bswap16(r16);
            r32 = __builtin_bswap32(r32);
            r64 = __builtin_bswap64(r64);
        }
        CHECK(dyn_ld16le(buf + off) == r16, "ld16le");
        CHECK(dyn_ld32le(buf + off) == r32, "ld32le");
        CHECK(dyn_ld64le(buf + off) == r64, "ld64le");
        CHECK(dyn_ld16be(buf + off) == __builtin_bswap16(r16), "ld16be");
        CHECK(dyn_ld32be(buf + off) == __builtin_bswap32(r32), "ld32be");
        CHECK(dyn_ld64be(buf + off) == __builtin_bswap64(r64), "ld64be");
        {
            uint8_t o[8];
            uint64_t v = rnd();
            dyn_st64le(o, v);
            CHECK(dyn_ld64le(o) == v && o[0] == (uint8_t)v, "st64le");
            dyn_st64be(o, v);
            CHECK(dyn_ld64be(o) == v && o[7] == (uint8_t)v, "st64be");
            dyn_st32le(o, (uint32_t)v);
            CHECK(dyn_ld32le(o) == (uint32_t)v && o[0] == (uint8_t)v, "st32le");
            dyn_st32be(o, (uint32_t)v);
            CHECK(dyn_ld32be(o) == (uint32_t)v && o[3] == (uint8_t)v, "st32be");
            dyn_st16le(o, (uint16_t)v);
            CHECK(dyn_ld16le(o) == (uint16_t)v && o[0] == (uint8_t)v, "st16le");
            dyn_st16be(o, (uint16_t)v);
            CHECK(dyn_ld16be(o) == (uint16_t)v && o[1] == (uint8_t)v, "st16be");
        }
    }
    CHECK(dyn_rotl64(0x8000000000000001ull, 0) == 0x8000000000000001ull, "rotl64 by 0");
    CHECK(dyn_rotl64(0x8000000000000001ull, 1) == 0x0000000000000003ull, "rotl64 by 1");
    CHECK(dyn_rotl64(0x8000000000000001ull, 64) == 0x8000000000000001ull, "rotl64 by 64");
}

static void test_ascii(void)
{
    int c;
    const char* hex = "0123456789abcdef";
    setlocale(LC_ALL, "C");
    for (c = 0; c < 256; c++) {
        const char* h = c ? strchr(hex, tolower(c)) : NULL;
        int want = h ? (int)(h - hex) : -1;
        CHECK(dyn_ascii_isdigit(c) == !!isdigit(c), "isdigit %d", c);
        CHECK(dyn_ascii_isalpha(c) == !!isalpha(c), "isalpha %d", c);
        CHECK(dyn_ascii_isalnum(c) == !!isalnum(c), "isalnum %d", c);
        CHECK(dyn_ascii_isupper(c) == !!isupper(c), "isupper %d", c);
        CHECK(dyn_ascii_islower(c) == !!islower(c), "islower %d", c);
        CHECK(dyn_ascii_isspace(c) == !!isspace(c), "isspace %d", c);
        CHECK(dyn_ascii_tolower(c) == tolower(c), "tolower %d", c);
        CHECK(dyn_ascii_toupper(c) == toupper(c), "toupper %d", c);
        CHECK(dyn_hexval(c) == want, "hexval %d -> %d want %d", c, dyn_hexval(c), want);
    }
    CHECK(dyn_hexval(-1) == -1, "hexval(EOF)");
}

static int ref_ieq_lower_lit(const char* a, size_t an, const char* lit)
{
    size_t i;
    if (an != strlen(lit))
        return 0;
    for (i = 0; i < an; i++)
        if (tolower((unsigned char)a[i]) != lit[i])
            return 0;
    return 1;
}

static void test_compare(void)
{
    static const char* lits[] = { "", "p", "li", "script", "blockquote", "a\x80" };
    static const char* inputs[] = { "", "P", "Li", "LI", "lI", "li", "lix", "l", "SCRIPT", "Script ", "BlockQuote", "a\x80", "A\x80", "p\0x" };
    size_t i, j;
    for (i = 0; i < sizeof lits / sizeof *lits; i++)
        for (j = 0; j < sizeof inputs / sizeof *inputs; j++) {
            size_t n = strlen(inputs[j]) + (j == 13 ? 2 : 0);
            CHECK(dyn_ascii_ieq_lower_lit(inputs[j], n, lits[i]) == ref_ieq_lower_lit(inputs[j], n, lits[i]),
                "ieq_lower_lit '%s' vs '%s'", inputs[j], lits[i]);
        }
    CHECK(dyn_ascii_ieq("HeLLo", 5, "hello", 5) && !dyn_ascii_ieq("hello", 5, "hell", 4) && !dyn_ascii_ieq("a[", 2, "a{", 2),
        "ascii_ieq");
}

static void test_size(void)
{
    size_t out = 0;
    CHECK(dyn_size_mul(SIZE_MAX / 2, 2, &out) == 0 && out == SIZE_MAX - 1, "mul fits");
    CHECK(dyn_size_mul(SIZE_MAX / 2 + 1, 2, &out) == -1, "mul overflows");
    CHECK(dyn_size_mul(0, SIZE_MAX, &out) == 0 && out == 0, "mul by zero");
    CHECK(dyn_size_add(SIZE_MAX - 1, 1, &out) == 0 && out == SIZE_MAX, "add fits");
    CHECK(dyn_size_add(SIZE_MAX, 1, &out) == -1, "add overflows");
}

static void test_utf8(void)
{
    uint32_t cp, got;
    uint8_t b[4];
    static const struct {
        const char* s;
        size_t n;
    } bad[] = {
        { "\x80", 1 },
        { "\xBF", 1 },
        { "\xC0\x80", 2 },
        { "\xC1\xBF", 2 },
        { "\xE0\x80\x80", 3 },
        { "\xE0\x9F\xBF", 3 },
        { "\xED\xA0\x80", 3 },
        { "\xED\xBF\xBF", 3 },
        { "\xF0\x80\x80\x80", 4 },
        { "\xF0\x8F\xBF\xBF", 4 },
        { "\xF4\x90\x80\x80", 4 },
        { "\xF5\x80\x80\x80", 4 },
        { "\xFF", 1 },
        { "\xC2", 1 },
        { "\xE2\x82", 2 },
        { "\xF0\x9F\x98", 3 },
        { "\xC2\x41", 2 },
        { "\xE2\x28\xA1", 3 },
    };
    size_t k;
    for (cp = 0; cp <= 0x10FFFF; cp++) {
        int n, m;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            continue;
        n = dyn_utf8_encode(cp, b);
        CHECK(n == (cp < 0x80 ? 1 : cp < 0x800 ? 2
                      : cp < 0x10000           ? 3
                                               : 4),
            "encode length U+%X", cp);
        m = dyn_utf8_decode(b, (size_t)n, &got);
        CHECK(m == n && got == cp, "round trip U+%X (%d %d %X)", cp, n, m, got);
    }
    for (k = 0; k < sizeof bad / sizeof *bad; k++)
        CHECK(dyn_utf8_decode((const uint8_t*)bad[k].s, bad[k].n, &got) == -1, "refuses malformed #%zu", k);
    CHECK(dyn_utf8_decode((const uint8_t*)"", 0, &got) == 0, "empty input");
}

static void test_fd(void)
{
    int p[2], fl;
    char buf[8];
    CHECK(dyn_pipe_cloexec(p) == 0, "pipe");
    CHECK((fcntl(p[0], F_GETFD) & FD_CLOEXEC) && (fcntl(p[1], F_GETFD) & FD_CLOEXEC), "pipe ends are FD_CLOEXEC");
    CHECK(!(fcntl(p[0], F_GETFL) & O_NONBLOCK), "pipe starts blocking");
    CHECK(dyn_fd_set_nonblock(p[0]) == 0 && (fcntl(p[0], F_GETFL) & O_NONBLOCK), "set_nonblock");
    CHECK(dyn_fd_set_nonblock(p[0]) == 0 && (fcntl(p[0], F_GETFL) & O_NONBLOCK), "set_nonblock is idempotent");
    CHECK(dyn_read_eintr(p[0], buf, sizeof buf) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK), "empty nonblocking read");
    CHECK(dyn_write_all(p[1], "abcdef", 6) == 0, "write_all");
    CHECK(dyn_read_eintr(p[0], buf, sizeof buf) == 6 && memcmp(buf, "abcdef", 6) == 0, "read back");
    close(p[0]);
    close(p[1]);
    CHECK(dyn_fd_set_nonblock(p[0]) == -1 && errno == EBADF, "set_nonblock on a closed fd fails, sets nothing");
    CHECK(dyn_fd_set_cloexec(p[0]) == -1 && errno == EBADF, "set_cloexec on a closed fd fails");
    fl = fcntl(0, F_GETFD);
    CHECK(fl >= 0, "stdin is valid");
}

static void test_eintr_wrappers(void)
{
    int sv[2], status = -1;
    char buf[8];
    pid_t pid;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    CHECK(dyn_send_eintr(sv[0], "hello", 5, NULL, 0) == 5, "send without a peer is send()");
    CHECK(dyn_recv_eintr(sv[1], buf, sizeof buf, 0) == 5 && memcmp(buf, "hello", 5) == 0, "recv gets the bytes");
    CHECK(dyn_write_eintr(sv[1], "xy", 2) == 2, "write");
    CHECK(dyn_read_eintr(sv[0], buf, sizeof buf) == 2 && memcmp(buf, "xy", 2) == 0, "read back");
    CHECK(dyn_recv_eintr(sv[0], buf, sizeof buf, MSG_DONTWAIT) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK), "recv passes flags through");
    close(sv[0]);
    close(sv[1]);
    CHECK(dyn_send_eintr(sv[0], "z", 1, NULL, 0) == -1 && errno == EBADF, "send on a closed fd reports the error");
    pid = fork();
    if (pid == 0)
        _exit(42);
    CHECK(pid > 0, "fork");
    CHECK(dyn_waitpid_eintr(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42, "waitpid reaps the child with its status");
    CHECK(dyn_waitpid_eintr(pid, &status, WNOHANG) == -1 && errno == ECHILD, "a reaped child is gone");
}

int main(void)
{
    test_byte_order();
    test_ascii();
    test_compare();
    test_size();
    test_utf8();
    test_fd();
    test_eintr_wrappers();
    printf("test_dyn_common: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
