#include "dyn-resp.h"
#include "dyn-dns.h"
#include "dyn-scram.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void simd_init(void);

static int inited;

static void fuzz_resp(const uint8_t* d, size_t n)
{
    static const uint8_t TAGS[] = { '+', '-', ':', '$', '*', '_', ',', '#',
        '!', '=', '(', '%', '~', '|', '>' };
    uint8_t* buf;
    size_t used = 0, len = n;
    int rc;

    if (n < 2 || n > 65536)
        return;
    buf = (uint8_t*)malloc(n);
    if (!buf)
        return;
    buf[0] = TAGS[d[0] % (sizeof(TAGS) / sizeof(TAGS[0]))];
    memcpy(buf + 1, d + 1, n - 1);

    rc = dyn_resp_scan(buf, len, 0, &used);
    if (rc == DYN_RESP_OK) {
        dyn_resp_reader_t r;
        dyn_resp_item_t it;
        int guard = 0;
        if (used > len)
            abort();
        dyn_resp_reader_init(&r, buf, used);
        while (dyn_resp_next(&r, &it) == DYN_RESP_OK && ++guard < 4096) {
            if (it.str && (it.str < buf || it.str + it.slen > buf + used))
                abort();
        }
    }
    if (rc == DYN_RESP_OK && used > 1) {
        size_t k = used / 2, u2 = 0;
        uint8_t* pre = (uint8_t*)malloc(k);
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

static void fuzz_dns(const uint8_t* d, size_t n)
{
    char name[DYN_DNS_MAX_NAME + 1];
    uint8_t* msg;
    size_t mlen = DYN_DNS_HDR_LEN + n;
    dyn_dns_hdr_t h;
    size_t off;

    if (n < 4 || n > 2048)
        return;
    msg = (uint8_t*)malloc(mlen);
    if (!msg)
        return;

    (void)dyn_dns_name_decode(d, n, 0, name, sizeof(name));
    (void)dyn_dns_name_decode(d, n, n / 2, name, sizeof(name));

    memset(msg, 0, mlen);
    msg[0] = d[0];
    msg[1] = d[1];
    msg[2] = 0x81;
    msg[3] = 0x80;
    msg[5] = 1;
    msg[7] = 1;
    memcpy(msg + DYN_DNS_HDR_LEN, d, n);
    if (dyn_dns_hdr_decode(msg, mlen, &h) == DYN_DNS_OK) {
        off = DYN_DNS_HDR_LEN;
        if (dyn_dns_skip_questions(msg, mlen, &off,
                h.qdcount)
            == DYN_DNS_OK) {
            dyn_dns_rr_t rr;
            uint16_t i;
            for (i = 0; i < h.ancount && i < 64; i++) {
                if (dyn_dns_rr_decode(msg, mlen, &off, &rr)
                    != DYN_DNS_OK)
                    break;
                if (rr.rdata && (rr.rdata < msg || rr.rdata + rr.rdlen > msg + mlen))
                    abort();
            }
        }
    }
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

static void fuzz_scram(const uint8_t* d, size_t n)
{
    dyn_scram_t sc;
    char cfirst[256], cfinal[1024];
    char* srv;

    if (n == 0 || n > DYN_SCRAM_MAX_MSG - 1)
        return;
    memset(&sc, 0, sizeof sc);
    if (dyn_scram_client_first(&sc, cfirst, sizeof(cfirst)) < 0)
        return;
    srv = (char*)malloc(n + 1);
    if (!srv) {
        dyn_scram_free(&sc);
        return;
    }
    memcpy(srv, d, n);
    srv[n] = '\0';
    if (dyn_scram_server_first(&sc, srv, n, "pencil", cfinal,
            sizeof(cfinal))
        > 0)
        (void)dyn_scram_server_final(&sc, srv, n);
    dyn_scram_free(&sc);
    memset(&sc, 0, sizeof sc);

    if (dyn_scram_client_first(&sc, cfirst, sizeof(cfirst)) >= 0) {
        char withnonce[DYN_SCRAM_MAX_MSG];
        int w = snprintf(withnonce, sizeof(withnonce), "r=%sX,%.*s",
            sc.nonce, (int)(n < 2048 ? n : 2048), srv);
        if (w > 0 && (size_t)w < sizeof(withnonce) && dyn_scram_server_first(&sc, withnonce, (size_t)w, "pencil", cfinal, sizeof(cfinal)) > 0)
            (void)dyn_scram_server_final(&sc, srv, n);
        dyn_scram_free(&sc);
    }
    free(srv);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!inited) {
        simd_init();
        inited = 1;
    }
    if (size < 2)
        return 0;
    switch (data[0] % 3) {
    case 0:
        fuzz_resp(data + 1, size - 1);
        break;
    case 1:
        fuzz_dns(data + 1, size - 1);
        break;
    default:
        fuzz_scram(data + 1, size - 1);
        break;
    }
    return 0;
}
