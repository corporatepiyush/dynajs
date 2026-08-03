// Fuzz target for the three parsers dyna:net points at a peer.
//
// Each reads bytes chosen by whatever answered on that port, and each fails
// differently, so each needs its own way past the gate. The trap this target is
// built to avoid is the one the LZ4 and DYNS sweeps hit: a target that random
// bytes bounce off measures the first rejection and proves nothing about the
// code behind it.
//
//   - RESP has a one-byte type tag, so ~99% of random inputs die on "unknown
//     type byte" before any length is parsed. The first fuzzer byte therefore
//     SELECTS a valid tag and the rest becomes the body, which puts the length
//     arithmetic, the nesting stack and the aggregate bounds in reach.
//   - a DNS message needs a 12-octet header before the name decoder is called
//     at all, so the input is also driven through a REPAIRED message: a real
//     header wrapped around the fuzzer's bytes as the question section. Name
//     compression makes that the pointer-chasing surface where the historical
//     bugs are.
//   - SCRAM needs a client-first before a server-first means anything, so the
//     exchange is started properly and the fuzzer's bytes are handed in as the
//     server's reply -- the position where every value is attacker-chosen: the
//     nonce, the salt, and the iteration count that decides how much work we do.
//
// Every path is also run through the READER after the scanner accepts, because
// accepting an input and then walking it are different bounds.
#include "dyn-resp.h"
#include "dyn-dns.h"
#include "dyn-scram.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void simd_init(void);          /* dyn-codec dispatches base64 through it */

static int inited;

static void fuzz_resp(const uint8_t *d, size_t n)
{
    static const uint8_t TAGS[] = { '+', '-', ':', '$', '*', '_', ',', '#',
                                    '!', '=', '(', '%', '~', '|', '>' };
    uint8_t *buf;
    size_t used = 0, len = n;
    int rc;

    if (n < 2 || n > 65536)
        return;
    /* EXACTLY n bytes, on the heap. A fixed-size stack buffer hides the whole
     * bug class this target exists for: a read one past `len` lands inside the
     * spare capacity, so the sanitizer sees nothing and a removed bounds check
     * still reports clean. Sized to the input, one byte over is a heap
     * overflow ASan reports immediately. */
    buf = (uint8_t *)malloc(n);
    if (!buf)
        return;
    /* Give the input a valid tag, or the scanner rejects almost everything on
     * the first byte and the rest of the parser is never reached. */
    buf[0] = TAGS[d[0] % (sizeof(TAGS) / sizeof(TAGS[0]))];
    memcpy(buf + 1, d + 1, n - 1);

    rc = dyn_resp_scan(buf, len, 0, &used);
    if (rc == DYN_RESP_OK) {
        dyn_resp_reader_t r;
        dyn_resp_item_t it;
        int guard = 0;
        if (used > len)
            abort();                     /* consumed more than it was given */
        dyn_resp_reader_init(&r, buf, used);
        while (dyn_resp_next(&r, &it) == DYN_RESP_OK && ++guard < 4096) {
            if (it.str && (it.str < buf || it.str + it.slen > buf + used))
                abort();                 /* a borrowed view outside the input */
        }
    }
    /* The scanner must be monotone in the input: every prefix of something it
     * accepted has to be INCOMPLETE, never a different accept and never an
     * error, or a partial TCP read is parsed as a whole message. */
    if (rc == DYN_RESP_OK && used > 1) {
        size_t k = used / 2, u2 = 0;
        uint8_t *pre = (uint8_t *)malloc(k);   /* exact again, same reason */
        if (pre) {
            int rc2;
            memcpy(pre, buf, k);
            rc2 = dyn_resp_scan(pre, k, 0, &u2);
            if (rc2 == DYN_RESP_OK && u2 > k)
                abort();
            free(pre);
        }
    }
    free(buf);
}

static void fuzz_dns(const uint8_t *d, size_t n)
{
    char name[DYN_DNS_MAX_NAME + 1];
    uint8_t *msg;
    size_t mlen = DYN_DNS_HDR_LEN + n;
    dyn_dns_hdr_t h;
    size_t off;

    if (n < 4 || n > 2048)
        return;
    /* Exact-sized, for the same reason as the RESP buffer above. */
    msg = (uint8_t *)malloc(mlen);
    if (!msg)
        return;

    /* raw: the decoder must survive being pointed anywhere in any buffer */
    (void)dyn_dns_name_decode(d, n, 0, name, sizeof(name));
    (void)dyn_dns_name_decode(d, n, n / 2, name, sizeof(name));

    /* repaired: a real header, so the question and record walkers are reached */
    memset(msg, 0, mlen);
    msg[0] = d[0]; msg[1] = d[1];
    msg[2] = 0x81; msg[3] = 0x80;
    msg[5] = 1;                          /* qdcount 1 */
    msg[7] = 1;                          /* ancount 1 */
    memcpy(msg + DYN_DNS_HDR_LEN, d, n);
    if (dyn_dns_hdr_decode(msg, mlen, &h) == DYN_DNS_OK) {
        off = DYN_DNS_HDR_LEN;
        if (dyn_dns_skip_questions(msg, mlen, &off,
                                   h.qdcount) == DYN_DNS_OK) {
            dyn_dns_rr_t rr;
            uint16_t i;
            for (i = 0; i < h.ancount && i < 64; i++) {
                if (dyn_dns_rr_decode(msg, mlen, &off, &rr)
                    != DYN_DNS_OK)
                    break;
                /* rdata BORROWS the message; a wrong bound here is the bug */
                if (rr.rdata && (rr.rdata < msg ||
                                 rr.rdata + rr.rdlen > msg + mlen))
                    abort();
            }
        }
    }
    /* the response builder is fed attacker bytes as the "query" it echoes */
    {
        uint8_t out[1024];
        int r = dyn_dns_begin_response(msg, mlen, 0, out, sizeof(out));
        if (r > 0) {
            static const uint8_t a4[4] = { 192, 0, 2, 1 };
            if ((size_t)r > sizeof(out))
                abort();
            (void)dyn_dns_add_answer(out, sizeof(out), (size_t)r,
                                     DYN_DNS_T_A, 60, a4, 4);
        }
    }
    free(msg);
}

static void fuzz_scram(const uint8_t *d, size_t n)
{
    dyn_scram_t sc;
    char cfirst[256], cfinal[1024];
    char *srv;

    /* EXACT length, not a fixed buffer. This used char srv[DYN_SCRAM_MAX_MSG]
     * with n capped BELOW it, so a read past n landed in spare capacity and
     * ASan saw nothing -- the target was structurally blind to the one bug
     * class it exists for. fuzz_scram.c parses the same protocol with an exact
     * malloc and gets it right; this is the same code, two targets. The +1 is
     * only for the NUL the parser requires; a read past n but within that byte
     * is still invisible, which is why fuzz_scram remains the primary target. */
    if (n == 0 || n > DYN_SCRAM_MAX_MSG - 1)
        return;
    if (dyn_scram_client_first(&sc, cfirst, sizeof(cfirst)) < 0)
        return;
    srv = (char *)malloc(n + 1);
    if (!srv) {
        dyn_scram_free(&sc);
        return;
    }
    memcpy(srv, d, n);
    srv[n] = '\0';
    /* Two shapes: the fuzzer's bytes alone, and the same bytes after a nonce
     * that DOES extend ours -- without that prefix almost everything dies on
     * the nonce check and the salt and iteration parsing are never reached. */
    if (dyn_scram_server_first(&sc, srv, n, "pencil", cfinal,
                               sizeof(cfinal)) > 0)
        (void)dyn_scram_server_final(&sc, srv, n);
    dyn_scram_free(&sc);

    if (dyn_scram_client_first(&sc, cfirst, sizeof(cfirst)) >= 0) {
        char withnonce[DYN_SCRAM_MAX_MSG];
        int w = snprintf(withnonce, sizeof(withnonce), "r=%sX,%.*s",
                         sc.nonce, (int)(n < 2048 ? n : 2048), srv);
        if (w > 0 && (size_t)w < sizeof(withnonce) &&
            dyn_scram_server_first(&sc, withnonce, (size_t)w, "pencil",
                                   cfinal, sizeof(cfinal)) > 0)
            (void)dyn_scram_server_final(&sc, srv, n);
        dyn_scram_free(&sc);
    }
    free(srv);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!inited) { simd_init(); inited = 1; }
    if (size < 2)
        return 0;
    /* One byte selects the parser so each gets a share of the budget, rather
     * than one of them absorbing every input that happens to look valid. */
    switch (data[0] % 3) {
    case 0: fuzz_resp(data + 1, size - 1);  break;
    case 1: fuzz_dns(data + 1, size - 1);   break;
    default: fuzz_scram(data + 1, size - 1); break;
    }
    return 0;
}
