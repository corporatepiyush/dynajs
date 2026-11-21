// covers: c:src/dyna-tls.c -- RFC 5929 s.4.1 tls-server-end-point hash chosen by the certificate signature algorithm
// MD5/SHA-1 map to SHA-256, other hashes are kept, Ed25519 (no hash) is -2; expected digests come from openssl x509 -outform DER | openssl dgst.
// Usage: test_tls_end_point (OPENSSL=path overrides the CLI); run from the repo root.
#include "dyna-tls.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
static char dir[256];
static const char* ossl;

static long slurp(const char* path, uint8_t* buf, size_t cap)
{
    FILE* f = fopen(path, "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

static void fail(const char* name, const char* why, int rc)
{
    fails++;
    printf("  FAIL: %s: %s (rc %d)\n", name, why, rc);
}

static void one(const char* name, const char* newkey, const char* md, const char* dgst, int want_rc, int required)
{
    char cmd[2048], p[512];
    uint8_t der[16384], want[64], got[64];
    long dn, wn = 0;
    int rc;
    snprintf(cmd, sizeof cmd, "'%s' req -x509 %s -nodes %s -subj /CN=ep -days 1 -keyout '%s/%s.key' -out '%s/%s.pem' >/dev/null 2>&1 && '%s' x509 -in '%s/%s.pem' -outform DER -out '%s/%s.der'",
        ossl, newkey, md, dir, name, dir, name, ossl, dir, name, dir, name);
    if (system(cmd) != 0) {
        if (required)
            fail(name, "openssl could not make the certificate", 0);
        else
            printf("  skip %s: openssl refused this signature\n", name);
        return;
    }
    if (dgst) {
        snprintf(cmd, sizeof cmd, "'%s' dgst -%s -binary '%s/%s.der' > '%s/%s.h'", ossl, dgst, dir, name, dir, name);
        snprintf(p, sizeof p, "%s/%s.h", dir, name);
        if (system(cmd) != 0 || (wn = slurp(p, want, sizeof want)) <= 0) {
            fail(name, "openssl dgst failed", 0);
            return;
        }
    }
    snprintf(p, sizeof p, "%s/%s.der", dir, name);
    dn = slurp(p, der, sizeof der);
    if (dn <= 0) {
        fail(name, "no DER", 0);
        return;
    }
    checks++;
    rc = dyn_tls_der_end_point_hash(der, (size_t)dn, got, sizeof got);
    if (want_rc ? rc != want_rc : (rc != wn || memcmp(got, want, (size_t)wn))) {
        fail(name, want_rc ? "wrong verdict" : "hash differs from openssl dgst", rc);
        return;
    }
    printf("  ok %s: %s\n", name, want_rc ? "no usable hash (-2)" : dgst);
    if (want_rc)
        return;
    checks++;
    rc = dyn_tls_der_end_point_hash(der, (size_t)dn, got, (size_t)wn - 1);
    if (rc >= 0)
        fail(name, "output one byte short accepted", rc);
    checks++;
    rc = dyn_tls_der_end_point_hash(der, (size_t)dn - 1, got, sizeof got);
    if (rc >= 0)
        fail(name, "truncated DER accepted", rc);
}

int main(void)
{
    static const uint8_t junk[] = { 0x30, 0x03, 0x02, 0x01, 0x00, 0x00 };
    uint8_t got[64];
    char cmd[512];
    int rc;
    ossl = getenv("OPENSSL") ? getenv("OPENSSL") : "openssl";
    snprintf(dir, sizeof dir, ".obj/tls-ep-%d", (int)getpid());
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    if (system(cmd) != 0) {
        printf("FAIL: cannot create %s\n", dir);
        return 1;
    }
    one("rsa-sha1", "-newkey rsa:2048", "-sha1", "sha256", 0, 1);
    one("rsa-md5", "-newkey rsa:2048", "-md5", "sha256", 0, 0);
    one("rsa-sha256", "-newkey rsa:2048", "-sha256", "sha256", 0, 1);
    one("rsa-sha384", "-newkey rsa:2048", "-sha384", "sha384", 0, 1);
    one("rsa-pss-sha512", "-newkey rsa-pss -pkeyopt rsa_keygen_bits:2048", "-sha512", "sha512", 0, 1);
    one("ec-p256-sha224", "-newkey ec -pkeyopt ec_paramgen_curve:P-256", "-sha224", "sha224", 0, 1);
    one("ed25519", "-newkey ed25519", "", NULL, -2, 1);
    checks += 3;
    if ((rc = dyn_tls_der_end_point_hash(junk, sizeof junk, got, sizeof got)) >= 0)
        fail("junk DER", "accepted", rc);
    if ((rc = dyn_tls_der_end_point_hash(junk, 0, got, sizeof got)) >= 0)
        fail("empty DER", "accepted", rc);
    if ((rc = dyn_tls_der_end_point_hash(NULL, 10, got, sizeof got)) >= 0)
        fail("NULL DER", "accepted", rc);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fails++;
    if (checks < 20) {
        printf("  FAIL: only %d checks ran\n", checks);
        fails++;
    }
    printf("tls_end_point: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
