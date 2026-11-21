/*
 * test_webrtc_dtls_openssl.c -- the dyna:webrtc DTLS layer (src/modules/webrtc/webrtc-dtls.c) against the OpenSSL
 * command line peer over real loopback UDP: `openssl s_server -dtls1_2` with dynajs as the DTLS client, and
 * `openssl s_client -dtls1_2` with dynajs as the DTLS server. The peer runs a different OpenSSL build than the one
 * linked here when the PATH one differs (Homebrew openssl 4.x vs the 3.5 series the build links).
 *
 * Oracles, none derived from the code under test:
 * - the peer's certificate is made by `openssl req`; its SHA-256 fingerprint comes from `openssl x509 -fingerprint`;
 *   a connection only counts when our fingerprint check (RFC 8122 / RFC 8827 5.5) accepts exactly that certificate,
 *   and a one-bit-off fingerprint must fail the handshake;
 * - use_srtp (RFC 5764 4.1, RFC 7714 14.2): the peer prints the negotiated profile; each of SRTP_AEAD_AES_128_GCM,
 *   SRTP_AEAD_AES_256_GCM, SRTP_AES128_CM_SHA1_80 and SRTP_AES128_CM_SHA1_32 is forced from the peer side in turn (P2
 *   offers all four, so a peer that supports only one of them still gets media); wrt_dtls_srtp_name must return the
 *   RFC 5764 4.1.2 / RFC 7714 14.2 name of the negotiated profile (what getStats reports as srtpCipher);
 * - exported keying material (RFC 5705, label EXTRACTOR-dtls_srtp, RFC 5764 4.2): the peer prints its own export
 *   of the profile's key length (56 bytes for AES-128-GCM, 88 for AES-256-GCM, 60 for AES-CM); ours must match it byte for byte;
 * - R3-9: after one record the read loop stops unless SSL_has_pending; two records packed into one datagram (RFC 6347
 *   4.1.1 allows several per datagram) must still both be delivered by that one input (in-memory pair, no openssl CLI).
 * - M14: the cipher list is AEAD only (GCM, ChaCha20-Poly1305); an s_server limited to the CBC suite
 *   ECDHE-ECDSA-AES128-SHA256 must fail the handshake with us as client.
 * - application data both ways, then close_notify both ways (dynajs closes as client; the peer closes as client).
 * Prints SKIP and exits 0 when no openssl binary is on PATH (or in $OPENSSL_BIN).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "modules/webrtc/webrtc-internal.h"

extern char** environ;

static int fails, checks;

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static int g_fd = -1;
static struct sockaddr_in g_peer;
static int g_have_peer;
static char g_rx[65536];
static size_t g_rx_n;
static int g_state_calls;
static const char* g_ossl = "openssl";
static long g_pc_tag;
#define FAKE_PC ((wrt_pc*)(void*)&g_pc_tag)
static char g_dir[512];

static long g_mem_a, g_mem_b;
#define MEM_A ((wrt_pc*)(void*)&g_mem_a)
#define MEM_B ((wrt_pc*)(void*)&g_mem_b)
static uint8_t g_mq[2][64][1500];
static size_t g_ml[2][64];
static int g_mn[2];

void wrt_pc_send_dtls(wrt_pc* pc, const uint8_t* p, size_t n)
{
    if (pc == MEM_A || pc == MEM_B) {
        int to = pc == MEM_A ? 1 : 0;
        if (g_mn[to] < 64 && n <= 1500) {
            memcpy(g_mq[to][g_mn[to]], p, n);
            g_ml[to][g_mn[to]++] = n;
        }
        return;
    }
    if (g_fd >= 0 && g_have_peer)
        sendto(g_fd, p, n, 0, (struct sockaddr*)&g_peer, sizeof(g_peer));
}

void wrt_pc_dtls_data(wrt_pc* pc, const uint8_t* p, size_t n)
{
    (void)pc;
    if (g_rx_n + n < sizeof(g_rx)) {
        memcpy(g_rx + g_rx_n, p, n);
        g_rx_n += n;
        g_rx[g_rx_n] = 0;
    }
}

void wrt_pc_dtls_state(wrt_pc* pc)
{
    (void)pc;
    g_state_calls++;
}

typedef struct {
    pid_t pid;
    int in_fd;
    int out_fd;
    char out[131072];
    size_t out_n;
    int status;
    int exited;
} child;

static int run_cmd(const char* const* argv, char* out, size_t cap)
{
    int pfd[2];
    pid_t pid;
    posix_spawn_file_actions_t fa;
    size_t n = 0;
    int st = -1;
    if (pipe(pfd) < 0)
        return -1;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    if (posix_spawnp(&pid, argv[0], &fa, NULL, (char* const*)argv, environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    for (;;) {
        ssize_t r = read(pfd[0], out + n, cap - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n >= cap - 1)
            break;
    }
    out[n] = 0;
    close(pfd[0]);
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int child_start(child* c, const char* const* argv)
{
    int in[2], out[2];
    posix_spawn_file_actions_t fa;
    memset(c, 0, sizeof(*c));
    if (pipe(in) < 0 || pipe(out) < 0)
        return -1;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    if (posix_spawnp(&c->pid, argv[0], &fa, NULL, (char* const*)argv, environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return -1;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    c->in_fd = in[1];
    c->out_fd = out[0];
    fcntl(c->out_fd, F_SETFL, fcntl(c->out_fd, F_GETFL) | O_NONBLOCK);
    return 0;
}

static void child_drain(child* c)
{
    for (;;) {
        ssize_t r;
        if (c->out_fd < 0 || c->out_n + 1 >= sizeof(c->out))
            return;
        r = read(c->out_fd, c->out + c->out_n, sizeof(c->out) - 1 - c->out_n);
        if (r > 0) {
            c->out_n += (size_t)r;
            c->out[c->out_n] = 0;
            continue;
        }
        if (r == 0) {
            close(c->out_fd);
            c->out_fd = -1;
        }
        return;
    }
}

static void child_poll_exit(child* c)
{
    int st;
    if (!c->exited && waitpid(c->pid, &st, WNOHANG) == c->pid) {
        c->exited = 1;
        c->status = st;
    }
}

static void child_stop(child* c)
{
    int i;
    if (c->in_fd >= 0)
        close(c->in_fd);
    c->in_fd = -1;
    for (i = 0; i < 100 && !c->exited; i++) {
        child_drain(c);
        child_poll_exit(c);
        if (!c->exited)
            usleep(20000);
    }
    if (!c->exited) {
        kill(c->pid, SIGKILL);
        waitpid(c->pid, &c->status, 0);
        c->exited = 1;
    }
    child_drain(c);
    if (c->out_fd >= 0)
        close(c->out_fd);
    c->out_fd = -1;
}

typedef int (*until_fn)(wrt_dtls* d, child* c);

static int pump(wrt_dtls* d, child* c, until_fn done, int ms)
{
    uint64_t end = wrt_now_us() + (uint64_t)ms * 1000u;
    uint8_t buf[2048];
    while (wrt_now_us() < end) {
        struct pollfd pf[2];
        int np = 1, to = 20;
        uint64_t now = wrt_now_us();
        uint64_t dl = d ? wrt_dtls_deadline(d, now) : UINT64_MAX;
        if (done(d, c))
            return 1;
        if (dl != UINT64_MAX) {
            if (dl <= now)
                wrt_dtls_timeout(d);
            else if ((dl - now) / 1000u < (uint64_t)to)
                to = (int)((dl - now) / 1000u) + 1;
        }
        pf[0].fd = g_fd;
        pf[0].events = POLLIN;
        if (c->out_fd >= 0) {
            pf[1].fd = c->out_fd;
            pf[1].events = POLLIN;
            np = 2;
        }
        poll(pf, (nfds_t)np, to);
        for (;;) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t r = recvfrom(g_fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&from, &fl);
            if (r <= 0)
                break;
            if (!g_have_peer) {
                g_peer = from;
                g_have_peer = 1;
            }
            if (d)
                wrt_dtls_input(d, buf, (size_t)r);
        }
        child_drain(c);
        child_poll_exit(c);
    }
    return done(d, c);
}

static int udp_bind(uint16_t* port)
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0)
        return -1;
    getsockname(fd, (struct sockaddr*)&a, &al);
    *port = ntohs(a.sin_port);
    return fd;
}

static size_t parse_hex(const char* s, uint8_t* out, size_t cap)
{
    size_t n = 0;
    while (*s && n < cap) {
        int hi, lo;
        while (*s == ':' || *s == ' ')
            s++;
        if (!s[0] || !s[1])
            break;
        hi = s[0] <= '9' && s[0] >= '0' ? s[0] - '0' : (s[0] | 32) >= 'a' && (s[0] | 32) <= 'f' ? (s[0] | 32) - 'a' + 10 : -1;
        lo = s[1] <= '9' && s[1] >= '0' ? s[1] - '0' : (s[1] | 32) >= 'a' && (s[1] | 32) <= 'f' ? (s[1] | 32) - 'a' + 10 : -1;
        if (hi < 0 || lo < 0)
            break;
        out[n++] = (uint8_t)(hi * 16 + lo);
        s += 2;
    }
    return n;
}

typedef struct {
    char cert[600];
    char key[600];
    uint8_t fp[32];
    int ok;
} peer_cert;

static void make_peer_cert(peer_cert* pc, const char* tag, int rsa)
{
    char out[8192];
    const char* fpl;
    snprintf(pc->cert, sizeof(pc->cert), "%s/%s-cert.pem", g_dir, tag);
    snprintf(pc->key, sizeof(pc->key), "%s/%s-key.pem", g_dir, tag);
    {
        const char* ec[] = { g_ossl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-subj", "/CN=openssl-peer",
            "-days", "2", "-keyout", pc->key, "-out", pc->cert, NULL };
        const char* rs[] = { g_ossl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=openssl-peer", "-days", "2", "-keyout",
            pc->key, "-out", pc->cert, NULL };
        pc->ok = run_cmd(rsa ? rs : ec, out, sizeof(out)) == 0;
    }
    if (pc->ok) {
        const char* fa[] = { g_ossl, "x509", "-noout", "-fingerprint", "-sha256", "-in", pc->cert, NULL };
        pc->ok = run_cmd(fa, out, sizeof(out)) == 0;
        fpl = strchr(out, '=');
        pc->ok = pc->ok && fpl && parse_hex(fpl + 1, pc->fp, 32) == 32;
    }
}

static void write_file(const char* path, const char* s)
{
    FILE* f = fopen(path, "w");
    if (!f)
        return;
    fputs(s, f);
    fclose(f);
}

static int st_connected(wrt_dtls* d, child* c)
{
    (void)c;
    return d->state == WRT_DTLS_CONNECTED || d->state == WRT_DTLS_FAILED;
}

static const char* g_want_out;
static int out_has(wrt_dtls* d, child* c)
{
    (void)d;
    return strstr(c->out, g_want_out) != NULL;
}

static const char* g_want_rx;
static int rx_has(wrt_dtls* d, child* c)
{
    (void)d;
    (void)c;
    return strstr(g_rx, g_want_rx) != NULL;
}

static int child_gone(wrt_dtls* d, child* c)
{
    (void)d;
    return c->exited;
}

static int closed_or_gone(wrt_dtls* d, child* c)
{
    return d->state == WRT_DTLS_CLOSED || c->exited;
}

static int keys_match(const wrt_dtls* d, const char* out, uint32_t want_len)
{
    const char* k = strstr(out, "Keying material: ");
    uint8_t theirs[128];
    size_t n;
    if (!k || d->keys_len != want_len)
        return 0;
    n = parse_hex(k + 17, theirs, sizeof(theirs));
    return n == want_len && memcmp(theirs, d->keys, n) == 0;
}

static const char* g_srv_cipher;

static void case_we_client(wrt_cert* ours, const peer_cert* pk, const char* profile, uint32_t klen, int corrupt_fp)
{
    char port_s[32], accept_s[64], klen_s[16], line[256];
    uint16_t port = 0;
    int probe;
    child c;
    wrt_dtls d;
    char ourc[600], ourk[600];
    const char* label = corrupt_fp ? "client/bad-fp" : g_srv_cipher ? "client/cbc-only-peer" : "client";
    char *cp = NULL, *kp = NULL;
    snprintf(ourc, sizeof(ourc), "%s/ours-cert.pem", g_dir);
    snprintf(ourk, sizeof(ourk), "%s/ours-key.pem", g_dir);
    wrt_cert_pem(ours, &cp, &kp);
    write_file(ourc, cp ? cp : "");
    write_file(ourk, kp ? kp : "");
    free(cp);
    free(kp);
    probe = udp_bind(&port);
    close(probe);
    snprintf(port_s, sizeof(port_s), "%u", port);
    snprintf(accept_s, sizeof(accept_s), "127.0.0.1:%u", port);
    snprintf(klen_s, sizeof(klen_s), "%u", klen);
    {
        const char* argv[] = { g_ossl, "s_server", "-dtls1_2", "-accept", accept_s, "-cert", pk->cert, "-key", pk->key, "-verify", "1",
            "-use_srtp", profile, "-keymatexport", "EXTRACTOR-dtls_srtp", "-keymatexportlen", klen_s, "-naccept", "1", NULL, NULL, NULL };
        if (g_srv_cipher) {
            argv[19] = "-cipher";
            argv[20] = g_srv_cipher;
        }
        CHECK(child_start(&c, argv) == 0, "%s: spawn s_server", label);
    }
    g_want_out = "ACCEPT";
    CHECK(pump(NULL, &c, out_has, 5000), "%s: s_server listening (%s)", label, c.out);
    g_fd = udp_bind(&port);
    memset(&g_peer, 0, sizeof(g_peer));
    g_peer.sin_family = AF_INET;
    g_peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_peer.sin_port = htons((uint16_t)atoi(port_s));
    g_have_peer = 1;
    g_rx_n = 0;
    g_rx[0] = 0;
    memset(&d, 0, sizeof(d));
    CHECK(wrt_dtls_init(&d, FAKE_PC, ours, 0) == 0, "%s: init", label);
    memcpy(d.remote_fp, pk->fp, 32);
    d.remote_fp_len = 32;
    d.remote_fp_alg = RTC_FP_SHA256;
    if (corrupt_fp)
        d.remote_fp[31] ^= 1;
    g_state_calls = 0;
    wrt_dtls_start(&d);
    pump(&d, &c, st_connected, 8000);
    if (g_srv_cipher) {
        CHECK(d.state == WRT_DTLS_FAILED, "%s: a CBC-only peer fails the handshake (state %d, cipher %s)", label, d.state,
            wrt_dtls_cipher(&d) ? wrt_dtls_cipher(&d) : "none");
        CHECK(d.keys_len == 0, "%s: no keys exported", label);
    } else if (corrupt_fp) {
        CHECK(d.state == WRT_DTLS_FAILED, "%s: a one-bit-off fingerprint fails the handshake (state %d)", label, d.state);
        CHECK(d.keys_len == 0, "%s: no keys exported for a rejected peer", label);
    } else {
        CHECK(d.state == WRT_DTLS_CONNECTED, "%s: handshake with s_server (state %d) out=%s", label, d.state, c.out);
        CHECK(g_state_calls == 2, "%s: state callback ran for connecting and connected (%d)", label, g_state_calls);
        g_want_out = "Keying material: ";
        pump(&d, &c, out_has, 3000);
        snprintf(line, sizeof(line), "SRTP Extension negotiated, profile=%s", profile);
        CHECK(strstr(c.out, line) != NULL, "%s: peer negotiated %s", label, profile);
        {
            const char* want = !strcmp(profile, "SRTP_AEAD_AES_128_GCM") ? "AEAD_AES_128_GCM"
                : !strcmp(profile, "SRTP_AEAD_AES_256_GCM")              ? "AEAD_AES_256_GCM"
                : !strcmp(profile, "SRTP_AES128_CM_SHA1_80")             ? "AES_CM_128_HMAC_SHA1_80"
                                                                         : "AES_CM_128_HMAC_SHA1_32";
            const char* got = wrt_dtls_srtp_name(&d);
            CHECK(got && !strcmp(got, want), "%s: srtp profile name is the RFC 5764/7714 name %s (got %s)", label, want,
                got ? got : "none");
        }
        CHECK(keys_match(&d, c.out, klen), "%s: exported keying material equals the peer's (%u bytes)", label, klen);
        CHECK(wrt_dtls_cipher(&d) && strstr(wrt_dtls_cipher(&d), "GCM"), "%s: AEAD cipher suite (%s)", label,
            wrt_dtls_cipher(&d) ? wrt_dtls_cipher(&d) : "none");
        CHECK(wrt_dtls_write(&d, (const uint8_t*)"hello-from-dynajs\n", 18) == 0, "%s: write", label);
        g_want_out = "hello-from-dynajs";
        CHECK(pump(&d, &c, out_has, 3000), "%s: s_server received our record", label);
        CHECK(write(c.in_fd, "hello-from-openssl\n", 19) == 19, "%s: feed s_server stdin", label);
        g_want_rx = "hello-from-openssl";
        CHECK(pump(&d, &c, rx_has, 3000), "%s: we received the peer's record (%s)", label, g_rx);
        wrt_dtls_close(&d);
        pump(&d, &c, child_gone, 3000);
        CHECK(c.exited && WIFEXITED(c.status) && WEXITSTATUS(c.status) == 0, "%s: s_server saw close_notify and exited 0 (%d)", label,
            c.status);
    }
    child_stop(&c);
    wrt_dtls_free(&d);
    close(g_fd);
    g_fd = -1;
    g_have_peer = 0;
}

static void case_we_server(wrt_cert* ours, const peer_cert* pk, const char* profile, uint32_t klen)
{
    char connect_s[64], klen_s[16], line[256], ourc[600], ourk[600];
    uint16_t port = 0;
    child c;
    wrt_dtls d;
    char *cp = NULL, *kp = NULL;
    snprintf(ourc, sizeof(ourc), "%s/ours-cert.pem", g_dir);
    snprintf(ourk, sizeof(ourk), "%s/ours-key.pem", g_dir);
    wrt_cert_pem(ours, &cp, &kp);
    write_file(ourc, cp ? cp : "");
    write_file(ourk, kp ? kp : "");
    free(cp);
    free(kp);
    g_fd = udp_bind(&port);
    g_have_peer = 0;
    g_rx_n = 0;
    g_rx[0] = 0;
    memset(&d, 0, sizeof(d));
    CHECK(wrt_dtls_init(&d, FAKE_PC, ours, 1) == 0, "server: init");
    memcpy(d.remote_fp, pk->fp, 32);
    d.remote_fp_len = 32;
    d.remote_fp_alg = RTC_FP_SHA256;
    wrt_dtls_start(&d);
    snprintf(connect_s, sizeof(connect_s), "127.0.0.1:%u", port);
    snprintf(klen_s, sizeof(klen_s), "%u", klen);
    {
        const char* argv[] = { g_ossl, "s_client", "-dtls1_2", "-connect", connect_s, "-cert", pk->cert, "-key", pk->key, "-use_srtp", profile,
            "-keymatexport", "EXTRACTOR-dtls_srtp", "-keymatexportlen", klen_s, NULL };
        CHECK(child_start(&c, argv) == 0, "server: spawn s_client");
    }
    pump(&d, &c, st_connected, 8000);
    CHECK(d.state == WRT_DTLS_CONNECTED, "server: handshake with s_client (state %d) out=%s", d.state, c.out);
    g_want_out = "Keying material: ";
    pump(&d, &c, out_has, 3000);
    snprintf(line, sizeof(line), "SRTP Extension negotiated, profile=%s", profile);
    CHECK(strstr(c.out, line) != NULL, "server: peer negotiated %s", profile);
    CHECK(keys_match(&d, c.out, klen), "server: exported keying material equals the peer's (%u bytes)", klen);
    CHECK(strstr(c.out, "Server certificate") != NULL, "server: s_client received our certificate");
    CHECK(write(c.in_fd, "hello-from-openssl\n", 19) == 19, "server: feed s_client stdin");
    g_want_rx = "hello-from-openssl";
    CHECK(pump(&d, &c, rx_has, 3000), "server: we received the peer's record (%s)", g_rx);
    CHECK(wrt_dtls_write(&d, (const uint8_t*)"hello-from-dynajs\n", 18) == 0, "server: write");
    g_want_out = "hello-from-dynajs";
    CHECK(pump(&d, &c, out_has, 3000), "server: s_client received our record");
    close(c.in_fd);
    c.in_fd = -1;
    pump(&d, &c, closed_or_gone, 3000);
    pump(&d, &c, closed_or_gone, 200);
    CHECK(d.state == WRT_DTLS_CLOSED, "server: the peer's close_notify closes us (state %d)", d.state);
    child_stop(&c);
    wrt_dtls_free(&d);
    close(g_fd);
    g_fd = -1;
}

static void mem_pump(wrt_dtls* d[2], int rounds)
{
    static uint8_t q[64][1500];
    static size_t l[64];
    int r, side, i, k;
    for (r = 0; r < rounds; r++)
        for (side = 0; side < 2; side++) {
            k = g_mn[side];
            for (i = 0; i < k; i++) {
                memcpy(q[i], g_mq[side][i], g_ml[side][i]);
                l[i] = g_ml[side][i];
            }
            g_mn[side] = 0;
            for (i = 0; i < k; i++)
                wrt_dtls_input(d[side], q[i], l[i]);
        }
}

static void case_two_records(wrt_cert* ca, wrt_cert* cb)
{
    wrt_dtls c, s;
    wrt_dtls* d[2];
    uint8_t one[3000];
    size_t n;
    memset(&c, 0, sizeof(c));
    memset(&s, 0, sizeof(s));
    g_mn[0] = g_mn[1] = 0;
    CHECK(wrt_dtls_init(&c, MEM_A, ca, 0) == 0 && wrt_dtls_init(&s, MEM_B, cb, 1) == 0, "two-records: init");
    memcpy(c.remote_fp, cb->fp, 32);
    c.remote_fp_len = 32;
    c.remote_fp_alg = RTC_FP_SHA256;
    memcpy(s.remote_fp, ca->fp, 32);
    s.remote_fp_len = 32;
    s.remote_fp_alg = RTC_FP_SHA256;
    d[0] = &c;
    d[1] = &s;
    wrt_dtls_start(&s);
    wrt_dtls_start(&c);
    mem_pump(d, 20);
    CHECK(c.state == WRT_DTLS_CONNECTED && s.state == WRT_DTLS_CONNECTED, "two-records: in-memory handshake (%d %d)", c.state, s.state);
    g_mn[0] = g_mn[1] = 0;
    CHECK(wrt_dtls_write(&c, (const uint8_t*)"first-record;", 13) == 0 && wrt_dtls_write(&c, (const uint8_t*)"second-record;", 14) == 0,
        "two-records: two writes");
    CHECK(g_mn[1] == 2, "two-records: two datagrams (%d)", g_mn[1]);
    if (g_mn[1] == 2) {
        memcpy(one, g_mq[1][0], g_ml[1][0]);
        memcpy(one + g_ml[1][0], g_mq[1][1], g_ml[1][1]);
        n = g_ml[1][0] + g_ml[1][1];
        g_mn[1] = 0;
        g_rx_n = 0;
        g_rx[0] = 0;
        wrt_dtls_input(&s, one, n);
        CHECK(strcmp(g_rx, "first-record;second-record;") == 0, "two-records: both records of one datagram are delivered at once (%s)", g_rx);
    }
    wrt_dtls_free(&c);
    wrt_dtls_free(&s);
}

int main(int argc, char** argv)
{
    const char* e = getenv("OPENSSL_BIN");
    char out[4096];
    peer_cert ec, rsa;
    wrt_cert *ours_ec, *ours_rsa;
    const char* err = NULL;
    signal(SIGPIPE, SIG_IGN);
    if (e && *e)
        g_ossl = e;
    {
        const char* v[] = { g_ossl, "version", NULL };
        if (run_cmd(v, out, sizeof(out)) != 0) {
            printf("SKIP test_webrtc_dtls_openssl: no openssl binary\n");
            return 0;
        }
        printf("peer: %s", out);
    }
    snprintf(g_dir, sizeof(g_dir), "%s", argc > 1 ? argv[1] : ".obj/webrtc-dtls-openssl");
    mkdir(g_dir, 0700);
    make_peer_cert(&ec, "peer-ec", 0);
    make_peer_cert(&rsa, "peer-rsa", 1);
    CHECK(ec.ok && rsa.ok, "openssl req/x509 made the peer certificates");
    ours_ec = wrt_cert_generate(0, 0, 86400000.0, &err);
    ours_rsa = wrt_cert_generate(1, 2048, 86400000.0, &err);
    CHECK(ours_ec && ours_rsa, "our certificates (%s)", err ? err : "");
    if (!ec.ok || !rsa.ok || !ours_ec || !ours_rsa) {
        printf("test_webrtc_dtls_openssl: %d checks, %d failures\n", checks, fails);
        return 1;
    }
    case_two_records(ours_ec, ours_rsa);
    case_we_client(ours_ec, &ec, "SRTP_AEAD_AES_128_GCM", 56, 0);
    case_we_client(ours_ec, &ec, "SRTP_AES128_CM_SHA1_80", 60, 0);
    case_we_client(ours_ec, &ec, "SRTP_AEAD_AES_256_GCM", 88, 0);
    case_we_client(ours_ec, &ec, "SRTP_AES128_CM_SHA1_32", 60, 0);
    case_we_client(ours_rsa, &rsa, "SRTP_AEAD_AES_128_GCM", 56, 0);
    case_we_client(ours_ec, &ec, "SRTP_AEAD_AES_128_GCM", 56, 1);
    g_srv_cipher = "ECDHE-ECDSA-AES128-SHA256";
    case_we_client(ours_ec, &ec, "SRTP_AEAD_AES_128_GCM", 56, 0);
    g_srv_cipher = NULL;
    case_we_server(ours_ec, &ec, "SRTP_AEAD_AES_128_GCM", 56);
    case_we_server(ours_ec, &ec, "SRTP_AES128_CM_SHA1_80", 60);
    case_we_server(ours_ec, &ec, "SRTP_AEAD_AES_256_GCM", 88);
    case_we_server(ours_ec, &ec, "SRTP_AES128_CM_SHA1_32", 60);
    case_we_server(ours_rsa, &rsa, "SRTP_AES128_CM_SHA1_80", 60);
    wrt_cert_unref(ours_ec);
    wrt_cert_unref(ours_rsa);
    printf("test_webrtc_dtls_openssl: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
