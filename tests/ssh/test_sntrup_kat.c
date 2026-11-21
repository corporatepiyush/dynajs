/*
 * test_sntrup_kat.c -- dyna:ssh sntrup761 (src/modules/ssh/ssh-sntrup.c) against the NIST round-3 KAT file
 * tests/corpus/sntrup761/kat_kem.rsp (100 vectors, from ntruprime-20201007.tar.gz; sha256 of the .rsp
 * e720b256c12c50fa4281708342aae2c6a47b01ae83b152f3fd03d7680e376172). Links ssh-sntrup.c alone and supplies
 * its ssh_random() from the NIST AES-256-CTR DRBG (rng.c of the NIST harness, re-implemented here on OpenSSL's
 * AES-256-ECB; test-only, never shipped). For every vector:
 *  1. randombytes_init(seed), then keypair: pk and sk byte-equal to the KAT;
 *  2. enc from the same DRBG stream: ct and ss byte-equal;
 *  3. dec(KAT ct, KAT sk): ss equal (decapsulation from the file alone, independent of the DRBG);
 *  4. implicit rejection: dec of ct with one flipped byte (a different position per vector, both halves of ct)
 *     returns HashSession(0, rho, ct') = SHA-512(0 || SHA-512(3 || rho)[0..32] || ct')[0..32] computed here
 *     from the round-3 specification; the KAT has no expected value for this path.
 *  5. Seeded properties (tests/lib/dyn-fuzzkit.h; DYN_FUZZ_SEED replays, DYN_FUZZ_ITERS scales): round trip from random
 *     DRBG seeds, 1-bit and multi-byte ct corruption and uniformly random ct all give HashSession(0, rho, ct).
 * A missing corpus file prints SKIP and exits 0 (the file is gitignored; sync-work links it).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>

#include "../lib/dyn-fuzzkit.h"

#define PK_BYTES 1158
#define SK_BYTES 1763
#define CT_BYTES 1039
#define SMALL_BYTES 191

int ssh_sntrup_keypair(uint8_t pk[PK_BYTES], uint8_t sk[SK_BYTES]);
int ssh_sntrup_enc(uint8_t ct[CT_BYTES], uint8_t ss[32], const uint8_t pk[PK_BYTES]);
int ssh_sntrup_dec(uint8_t ss[32], const uint8_t ct[CT_BYTES], const uint8_t sk[SK_BYTES]);
void ssh_random(void* p, size_t n);

static uint8_t drbg_key[32], drbg_v[16];

static void aes_ecb(const uint8_t key[32], const uint8_t in[16], uint8_t out[16])
{
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int n = 0;
    if (!c || EVP_EncryptInit_ex(c, EVP_aes_256_ecb(), NULL, key, NULL) != 1 || EVP_CIPHER_CTX_set_padding(c, 0) != 1
        || EVP_EncryptUpdate(c, out, &n, in, 16) != 1 || n != 16) {
        fprintf(stderr, "FAIL: AES-256-ECB unavailable\n");
        exit(1);
    }
    EVP_CIPHER_CTX_free(c);
}

static void v_inc(void)
{
    for (int j = 15; j >= 0; j--) {
        if (drbg_v[j] == 0xff)
            drbg_v[j] = 0;
        else {
            drbg_v[j]++;
            break;
        }
    }
}

static void drbg_update(const uint8_t* provided)
{
    uint8_t temp[48];
    for (int i = 0; i < 3; i++) {
        v_inc();
        aes_ecb(drbg_key, drbg_v, temp + 16 * i);
    }
    if (provided)
        for (int i = 0; i < 48; i++)
            temp[i] ^= provided[i];
    memcpy(drbg_key, temp, 32);
    memcpy(drbg_v, temp + 32, 16);
}

static void drbg_init(const uint8_t seed[48])
{
    memset(drbg_key, 0, sizeof drbg_key);
    memset(drbg_v, 0, sizeof drbg_v);
    drbg_update(seed);
}

static size_t drbg_bytes;

void ssh_random(void* p, size_t n)
{
    uint8_t* x = (uint8_t*)p;
    uint8_t block[16];
    drbg_bytes += n;
    while (n > 0) {
        v_inc();
        aes_ecb(drbg_key, drbg_v, block);
        size_t k = n > 16 ? 16 : n;
        memcpy(x, block, k);
        x += k;
        n -= k;
    }
    drbg_update(NULL);
}

static void sha512_prefixed(uint8_t out[64], uint8_t b, const uint8_t* a, size_t an, const uint8_t* c, size_t cn)
{
    unsigned int n = 0;
    EVP_MD_CTX* m = EVP_MD_CTX_new();
    if (!m || EVP_DigestInit_ex(m, EVP_sha512(), NULL) != 1 || EVP_DigestUpdate(m, &b, 1) != 1 || EVP_DigestUpdate(m, a, an) != 1
        || (cn && EVP_DigestUpdate(m, c, cn) != 1) || EVP_DigestFinal_ex(m, out, &n) != 1) {
        fprintf(stderr, "FAIL: SHA-512 unavailable\n");
        exit(1);
    }
    EVP_MD_CTX_free(m);
}

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static int field(const char* text, const char* name, int count, uint8_t* out, size_t len)
{
    char key[64];
    const char* p = text;
    snprintf(key, sizeof key, "\ncount = %d\n", count);
    p = strstr(p, key);
    if (!p)
        return -1;
    snprintf(key, sizeof key, "\n%s = ", name);
    p = strstr(p + 1, key);
    if (!p)
        return -1;
    p += strlen(key);
    for (size_t i = 0; i < len; i++) {
        int h = hexval((unsigned char)p[2 * i]), l = hexval((unsigned char)p[2 * i + 1]);
        if (h < 0 || l < 0)
            return -1;
        out[i] = (uint8_t)(h << 4 | l);
    }
    return hexval((unsigned char)p[2 * len]) < 0 ? 0 : -1;
}

static int fails;

static void check(int ok, int count, const char* what)
{
    if (!ok) {
        fails++;
        if (fails <= 20)
            fprintf(stderr, "FAIL: vector %d: %s\n", count, what);
    }
}

static void p_sntrup(void)
{
    static uint8_t pk[PK_BYTES], sk[SK_BYTES], ct[CT_BYTES], ct2[CT_BYTES], h3[64], hs[64];
    uint8_t seed[48], ss[32], ss2[32], ss3[32];
    fk_rng_t r;
    unsigned i, n, bad_rt = 0, bad_rej = 0, bad_rand = 0;
    if (!fk_only("sntrup-props"))
        return;
    r = fk_stream("sntrup-props");
    n = (unsigned)fk_iters(12);
    for (i = 0; i < n; i++) {
        fk_bytes(&r, seed, sizeof seed);
        drbg_init(seed);
        ssh_sntrup_keypair(pk, sk);
        ssh_sntrup_enc(ct, ss, pk);
        memset(ss2, 0, 32);
        ssh_sntrup_dec(ss2, ct, sk);
        ssh_sntrup_dec(ss3, ct, sk);
        bad_rt += memcmp(ss, ss2, 32) != 0 || memcmp(ss2, ss3, 32) != 0;
        sha512_prefixed(h3, 3, sk + 2 * SMALL_BYTES + PK_BYTES, SMALL_BYTES, NULL, 0);
        memcpy(ct2, ct, CT_BYTES);
        if (fk_chance(&r, 50))
            ct2[fk_below(&r, CT_BYTES)] ^= (uint8_t)(1u << fk_below(&r, 8));
        else
            fk_mutate(&r, ct2, CT_BYTES, 1 + fk_below(&r, 8));
        if (memcmp(ct2, ct, CT_BYTES) == 0)
            ct2[0] ^= 1;
        ssh_sntrup_dec(ss2, ct2, sk);
        sha512_prefixed(hs, 0, h3, 32, ct2, CT_BYTES);
        bad_rej += memcmp(ss2, hs, 32) != 0 || memcmp(ss2, ss, 32) == 0;
        fk_bytes(&r, ct2, CT_BYTES);
        ssh_sntrup_dec(ss2, ct2, sk);
        sha512_prefixed(hs, 0, h3, 32, ct2, CT_BYTES);
        bad_rand += memcmp(ss2, hs, 32) != 0;
    }
    FK_CHECK(bad_rt == 0, "sntrup-props: keypair/enc/dec round trip from random DRBG seeds, dec deterministic (%u bad)", bad_rt);
    FK_CHECK(bad_rej == 0, "sntrup-props: 1-bit and multi-byte ct corruption gives HashSession(0, rho, ct') (%u bad)", bad_rej);
    FK_CHECK(bad_rand == 0, "sntrup-props: a uniformly random ct is implicitly rejected (%u bad)", bad_rand);
    fails += bad_rt + bad_rej + bad_rand;
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "tests/corpus/sntrup761/kat_kem.rsp";
    fk_init("test_sntrup_kat");
    p_sntrup();
    if (!fk_legacy())
        return fails ? 1 : 0;
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("SKIP: test-ssh-sntrup-kat: %s is missing (gitignored corpus; run sync-work)\n", path);
        return fails ? 1 : 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = (char*)malloc((size_t)sz + 2);
    if (!text || fread(text + 1, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "FAIL: cannot read %s\n", path);
        return 1;
    }
    fclose(f);
    text[0] = '\n';
    text[sz + 1] = 0;
    static uint8_t seed[48], kpk[PK_BYTES], ksk[SK_BYTES], kct[CT_BYTES], kss[32];
    static uint8_t pk[PK_BYTES], sk[SK_BYTES], ct[CT_BYTES], ss[32], ct2[CT_BYTES], h3[64], hs[64];
    int n = 0;
    size_t min_rand = (size_t)-1, max_rand = 0;
    for (int count = 0;; count++) {
        if (field(text, "seed", count, seed, 48) < 0)
            break;
        if (field(text, "pk", count, kpk, PK_BYTES) < 0 || field(text, "sk", count, ksk, SK_BYTES) < 0
            || field(text, "ct", count, kct, CT_BYTES) < 0 || field(text, "ss", count, kss, 32) < 0) {
            fprintf(stderr, "FAIL: vector %d is malformed\n", count);
            return 1;
        }
        drbg_init(seed);
        drbg_bytes = 0;
        ssh_sntrup_keypair(pk, sk);
        check(memcmp(pk, kpk, PK_BYTES) == 0, count, "keypair pk");
        check(memcmp(sk, ksk, SK_BYTES) == 0, count, "keypair sk");
        ssh_sntrup_enc(ct, ss, pk);
        check(memcmp(ct, kct, CT_BYTES) == 0, count, "enc ct");
        check(memcmp(ss, kss, 32) == 0, count, "enc ss");
        if (drbg_bytes < min_rand)
            min_rand = drbg_bytes;
        if (drbg_bytes > max_rand)
            max_rand = drbg_bytes;
        memset(ss, 0, 32);
        ssh_sntrup_dec(ss, kct, ksk);
        check(memcmp(ss, kss, 32) == 0, count, "dec ss from the KAT sk/ct");
        memcpy(ct2, kct, CT_BYTES);
        size_t pos = (size_t)(count * 104729) % CT_BYTES;
        ct2[pos] ^= (uint8_t)(1u << (count % 8));
        ssh_sntrup_dec(ss, ct2, ksk);
        sha512_prefixed(h3, 3, ksk + 2 * SMALL_BYTES + PK_BYTES, SMALL_BYTES, NULL, 0);
        sha512_prefixed(hs, 0, h3, 32, ct2, CT_BYTES);
        check(memcmp(ss, hs, 32) == 0, count, "implicit rejection ss = HashSession(0, rho, ct')");
        check(memcmp(ss, kss, 32) != 0, count, "a tampered ct does not yield the real ss");
        n++;
    }
    free(text);
    if (n != 100) {
        fprintf(stderr, "FAIL: expected 100 vectors, parsed %d\n", n);
        return 1;
    }
    printf("test-ssh-sntrup-kat: %d vectors, %d failures; DRBG bytes per keypair+enc %zu..%zu\n", n, fails, min_rand, max_rand);
    return fails ? 1 : 0;
}
