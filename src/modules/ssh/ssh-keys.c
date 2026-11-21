#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/crypto.h>
#include "core/cryptok/cryptok.h"

#define SSH_KEY_MAX (1u << 16)
#define SSH_KDF_ROUNDS_MAX 4096
#define SSH_KH_MAX (16u << 20)

static int bcrypt_pbkdf(const char* pass, size_t passlen, const uint8_t* salt, size_t saltlen,
    uint8_t* key, size_t keylen, unsigned rounds)
{
    uint8_t sha2pass[64], sha2salt[64], out[32], tmp[32], cnt[4];
    size_t stride, amt, origlen = keylen, i;
    uint32_t count;
    unsigned r;
    if (rounds < 1 || passlen == 0 || saltlen == 0 || keylen == 0 || keylen > 1024)
        return -1;
    stride = (keylen + 31) / 32;
    amt = (keylen + stride - 1) / stride;
    if (ssh_hash(1, pass, passlen, sha2pass) < 0)
        return -1;
    for (count = 1; keylen > 0; count++) {
        ssh_put_be32(cnt, count);
        if (ssh_hash2(1, salt, saltlen, cnt, 4, sha2salt) < 0)
            return -1;
        dyn_bcrypt_pbkdf_hash(sha2pass, sha2salt, tmp);
        memcpy(out, tmp, 32);
        for (r = 1; r < rounds; r++) {
            if (ssh_hash(1, tmp, 32, sha2salt) < 0)
                return -1;
            dyn_bcrypt_pbkdf_hash(sha2pass, sha2salt, tmp);
            for (i = 0; i < 32; i++)
                out[i] ^= tmp[i];
        }
        if (amt > keylen)
            amt = keylen;
        for (i = 0; i < amt; i++) {
            size_t dest = i * stride + (count - 1);
            if (dest >= origlen)
                break;
            key[dest] = out[i];
        }
        keylen -= i;
    }
    OPENSSL_cleanse(out, sizeof out);
    OPENSSL_cleanse(tmp, sizeof tmp);
    OPENSSL_cleanse(sha2pass, sizeof sha2pass);
    return 0;
}

static const char* find_mem(const uint8_t* h, size_t hn, const char* needle)
{
    size_t nl = strlen(needle), i;
    if (hn < nl)
        return NULL;
    for (i = 0; i + nl <= hn; i++)
        if (memcmp(h + i, needle, nl) == 0)
            return (const char*)h + i;
    return NULL;
}

static int str_eq(const uint8_t* p, size_t n, const char* s)
{
    return p && n == strlen(s) && memcmp(p, s, n) == 0;
}

static int key_decrypt(const uint8_t* ciph, size_t cn, const uint8_t* kdfo, size_t kdfon,
    const char* pass, size_t passlen, uint8_t* data, size_t dn, const uint8_t* tag,
    char* why, size_t whylen)
{
    int gcm = str_eq(ciph, cn, "aes256-gcm@openssh.com");
    sshr_t r;
    const uint8_t* salt;
    size_t saltn;
    uint32_t rounds;
    uint8_t km[48];
    EVP_CIPHER_CTX* c = NULL;
    int ol, ok = 0;
    sshr_init(&r, kdfo, kdfon);
    salt = sshr_str(&r, &saltn);
    rounds = sshr_u32(&r);
    if (r.err || r.off != r.len || saltn == 0 || saltn > 1024) {
        snprintf(why, whylen, "malformed bcrypt KDF options in the private key");
        return -1;
    }
    if (rounds == 0 || rounds > SSH_KDF_ROUNDS_MAX) {
        snprintf(why, whylen, "bcrypt KDF rounds %u outside 1..%u", (unsigned)rounds, SSH_KDF_ROUNDS_MAX);
        return -1;
    }
    if (!pass) {
        snprintf(why, whylen, "the private key is encrypted: pass `passphrase`");
        return -1;
    }
    if (bcrypt_pbkdf(pass, passlen, salt, saltn, km, gcm ? 44 : 48, rounds) < 0) {
        snprintf(why, whylen, "bcrypt KDF failed");
        return -1;
    }
    c = EVP_CIPHER_CTX_new();
    if (gcm) {
        ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1
            && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1
            && EVP_DecryptInit_ex(c, NULL, NULL, km, km + 32) == 1
            && EVP_DecryptUpdate(c, data, &ol, data, (int)dn) == 1
            && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void*)(uintptr_t)tag) == 1
            && EVP_DecryptFinal_ex(c, km, &ol) == 1;
    } else {
        ok = c && EVP_DecryptInit_ex(c, EVP_aes_256_ctr(), NULL, km, km + 32) == 1
            && EVP_DecryptUpdate(c, data, &ol, data, (int)dn) == 1;
    }
    EVP_CIPHER_CTX_free(c);
    OPENSSL_cleanse(km, sizeof km);
    if (!ok) {
        snprintf(why, whylen, "wrong passphrase for the private key");
        return -1;
    }
    return 0;
}

EVP_PKEY* ssh_parse_private_key(const uint8_t* text, size_t n,
    const char* pass, size_t passlen, uint8_t pub[32], char* why, size_t whylen)
{
    static const char begin[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
    static const char end[] = "-----END OPENSSH PRIVATE KEY-----";
    const char *b, *e;
    uint8_t* raw = NULL;
    size_t rn;
    sshr_t r, p;
    const uint8_t *ciph, *kdf, *kdfo, *pubblob, *priv, *tag = NULL;
    size_t cn, kn, kon, pbn, prn;
    uint32_t nkeys;
    uint8_t* data = NULL;
    EVP_PKEY* key = NULL;
    int enc;

    if (n > SSH_KEY_MAX) {
        snprintf(why, whylen, "the private key text is larger than %u bytes", SSH_KEY_MAX);
        return NULL;
    }
    b = find_mem(text, n, begin);
    e = b ? find_mem((const uint8_t*)b, n - (size_t)(b - (const char*)text), end) : NULL;
    if (!b || !e) {
        snprintf(why, whylen, "not an OpenSSH private key (openssh-key-v1 PEM block expected)");
        return NULL;
    }
    b += sizeof begin - 1;
    raw = (uint8_t*)malloc((size_t)(e - b) + 1);
    if (!raw) {
        snprintf(why, whylen, "out of memory");
        return NULL;
    }
    rn = ssh_b64_decode(b, (size_t)(e - b), raw, (size_t)(e - b));
    if (rn == (size_t)-1 || rn < 15 || memcmp(raw, "openssh-key-v1", 15) != 0) {
        snprintf(why, whylen, "malformed openssh-key-v1 private key");
        goto out;
    }
    sshr_init(&r, raw + 15, rn - 15);
    ciph = sshr_str(&r, &cn);
    kdf = sshr_str(&r, &kn);
    kdfo = sshr_str(&r, &kon);
    nkeys = sshr_u32(&r);
    pubblob = sshr_str(&r, &pbn);
    priv = sshr_str(&r, &prn);
    if (r.err || nkeys != 1) {
        snprintf(why, whylen, "malformed openssh-key-v1 private key");
        goto out;
    }
    enc = !str_eq(ciph, cn, "none");
    if (enc) {
        if (!str_eq(kdf, kn, "bcrypt")
            || (!str_eq(ciph, cn, "aes256-ctr") && !str_eq(ciph, cn, "aes256-gcm@openssh.com"))) {
            snprintf(why, whylen, "unsupported private key encryption %.*s/%.*s (aes256-ctr or aes256-gcm@openssh.com with bcrypt)",
                (int)(cn > 40 ? 40 : cn), ciph, (int)(kn > 40 ? 40 : kn), kdf);
            goto out;
        }
        if (str_eq(ciph, cn, "aes256-gcm@openssh.com")) {
            if (r.len - r.off < 16) {
                snprintf(why, whylen, "malformed openssh-key-v1 private key");
                goto out;
            }
            tag = r.p + r.off;
            r.off += 16;
        }
        if (prn % 16) {
            snprintf(why, whylen, "malformed openssh-key-v1 private key");
            goto out;
        }
    } else if (!str_eq(kdf, kn, "none")) {
        snprintf(why, whylen, "malformed openssh-key-v1 private key");
        goto out;
    }
    if (r.off != r.len) {
        snprintf(why, whylen, "malformed openssh-key-v1 private key");
        goto out;
    }
    data = (uint8_t*)malloc(prn ? prn : 1);
    if (!data) {
        snprintf(why, whylen, "out of memory");
        goto out;
    }
    memcpy(data, priv, prn);
    if (enc && key_decrypt(ciph, cn, kdfo, kon, pass, passlen, data, prn, tag, why, whylen) < 0)
        goto out;
    sshr_init(&p, data, prn);
    {
        uint32_t c1 = sshr_u32(&p), c2 = sshr_u32(&p);
        const uint8_t *kt, *pk, *sk, *cm;
        size_t ktn, pkn, skn, cmn, i;
        sshr_t pb;
        const uint8_t *pbt, *pbk;
        size_t pbtn, pbkn;
        if (p.err || c1 != c2) {
            snprintf(why, whylen, enc ? "wrong passphrase for the private key" : "malformed openssh-key-v1 private key");
            goto out;
        }
        kt = sshr_str(&p, &ktn);
        if (p.err || !str_eq(kt, ktn, "ssh-ed25519")) {
            snprintf(why, whylen, "unsupported private key type %.*s (only ssh-ed25519)",
                (int)(ktn > 40 ? 40 : ktn), kt ? (const char*)kt : "");
            goto out;
        }
        pk = sshr_str(&p, &pkn);
        sk = sshr_str(&p, &skn);
        cm = sshr_str(&p, &cmn);
        (void)cm;
        if (p.err || pkn != 32 || skn != 64 || memcmp(sk + 32, pk, 32) != 0) {
            snprintf(why, whylen, "malformed ssh-ed25519 private key");
            goto out;
        }
        for (i = 0; p.off < p.len; i++)
            if (p.p[p.off++] != (uint8_t)(i + 1)) {
                snprintf(why, whylen, "malformed openssh-key-v1 private key padding");
                goto out;
            }
        sshr_init(&pb, pubblob, pbn);
        pbt = sshr_str(&pb, &pbtn);
        pbk = sshr_str(&pb, &pbkn);
        if (pb.err || !str_eq(pbt, pbtn, "ssh-ed25519") || pbkn != 32 || memcmp(pbk, pk, 32) != 0) {
            snprintf(why, whylen, "the private key's public half does not match");
            goto out;
        }
        key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, sk, 32);
        if (!key) {
            snprintf(why, whylen, "cannot load the ssh-ed25519 private key");
            goto out;
        }
        memcpy(pub, pk, 32);
    }
out:
    if (data) {
        OPENSSL_cleanse(data, prn);
        free(data);
    }
    if (raw) {
        OPENSSL_cleanse(raw, rn == (size_t)-1 ? 0 : rn);
        free(raw);
    }
    return key;
}

static int glob_match(const char* p, size_t pn, const char* s, size_t sn)
{
    size_t pi = 0, si = 0, star = (size_t)-1, mark = 0;
    while (si < sn) {
        if (pi < pn && (p[pi] == '?' || tolower((unsigned char)p[pi]) == tolower((unsigned char)s[si]))) {
            pi++;
            si++;
        } else if (pi < pn && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star != (size_t)-1) {
            pi = star + 1;
            si = ++mark;
        } else
            return 0;
    }
    while (pi < pn && p[pi] == '*')
        pi++;
    return pi == pn;
}

static int host_match(const char* pats, size_t pn, const char* host)
{
    size_t hl = strlen(host), i = 0;
    int pos = 0;
    if (pn > 3 && memcmp(pats, "|1|", 3) == 0) {
        const char* bar = memchr(pats + 3, '|', pn - 3);
        uint8_t salt[64], want[64], got[EVP_MAX_MD_SIZE];
        size_t sl, wl, gl = 0;
        if (!bar)
            return 0;
        sl = ssh_b64_decode(pats + 3, (size_t)(bar - pats - 3), salt, sizeof salt);
        wl = ssh_b64_decode(bar + 1, pn - (size_t)(bar + 1 - pats), want, sizeof want);
        if (sl == (size_t)-1 || wl != 20)
            return 0;
        if (!EVP_Q_mac(NULL, "HMAC", NULL, "SHA1", NULL, salt, sl, (const unsigned char*)host, hl,
                got, sizeof got, &gl)
            || gl != 20)
            return 0;
        return CRYPTO_memcmp(got, want, 20) == 0;
    }
    while (i < pn) {
        size_t j = i;
        int neg = 0;
        while (j < pn && pats[j] != ',')
            j++;
        if (pats[i] == '!') {
            neg = 1;
            i++;
        }
        if (j > i && glob_match(pats + i, j - i, host, hl)) {
            if (neg)
                return 0;
            pos = 1;
        }
        i = j + 1;
    }
    return pos;
}

static void host_string(char* out, size_t cap, const char* host, uint16_t port)
{
    if (port == 22)
        snprintf(out, cap, "%s", host);
    else
        snprintf(out, cap, "[%s]:%u", host, (unsigned)port);
}

static size_t next_tok(const char* s, size_t n, size_t* i, const char** tok)
{
    size_t a;
    while (*i < n && (s[*i] == ' ' || s[*i] == '\t'))
        (*i)++;
    a = *i;
    while (*i < n && s[*i] != ' ' && s[*i] != '\t')
        (*i)++;
    *tok = s + a;
    return *i - a;
}

static int kh_load(const char* path, char** out, size_t* outn)
{
    FILE* f = fopen(path, "rb");
    char* buf;
    size_t n;
    if (!f)
        return errno == ENOENT ? SSH_KH_UNKNOWN : SSH_KH_ERROR;
    buf = (char*)malloc(SSH_KH_MAX + 1);
    if (!buf) {
        fclose(f);
        return SSH_KH_ERROR;
    }
    n = fread(buf, 1, SSH_KH_MAX + 1, f);
    fclose(f);
    if (n > SSH_KH_MAX) {
        free(buf);
        return SSH_KH_ERROR;
    }
    *out = buf;
    *outn = n;
    return SSH_KH_OK;
}

typedef struct {
    int marker;
    const char* hp;
    size_t hl;
    const char* kt;
    size_t kl;
    const char* kd;
    size_t dl;
} kh_line_t;

static int kh_next(const char* buf, size_t n, size_t* off, kh_line_t* L)
{
    while (*off < n) {
        size_t e = *off, i = 0, ln, tl;
        const char *line, *tok;
        while (e < n && buf[e] != '\n')
            e++;
        line = buf + *off;
        ln = e - *off;
        *off = e + 1;
        if (ln && line[ln - 1] == '\r')
            ln--;
        tl = next_tok(line, ln, &i, &tok);
        if (tl == 0 || tok[0] == '#')
            continue;
        L->marker = 0;
        if (tok[0] == '@') {
            if (tl == 8 && memcmp(tok, "@revoked", 8) == 0)
                L->marker = 1;
            else if (tl == 15 && memcmp(tok, "@cert-authority", 15) == 0)
                L->marker = 2;
            else
                continue;
            tl = next_tok(line, ln, &i, &tok);
        }
        L->hp = tok;
        L->hl = tl;
        L->kl = next_tok(line, ln, &i, &L->kt);
        L->dl = next_tok(line, ln, &i, &L->kd);
        if (!L->hl || !L->kl || !L->dl)
            continue;
        return 1;
    }
    return 0;
}

static int kh_blob_is(const kh_line_t* L, uint8_t* kb, const uint8_t* blob, size_t bloblen)
{
    size_t bn = ssh_b64_decode(L->kd, L->dl, kb, 8192);
    return bn != (size_t)-1 && bn == bloblen && memcmp(kb, blob, bn) == 0;
}

int ssh_known_hosts_check(const char* path, const char* host, uint16_t port,
    const char* type, const uint8_t* blob, size_t bloblen)
{
    char hs[600];
    char* buf = NULL;
    size_t n = 0, off = 0;
    int ok = 0, changed = 0, revoked = 0, rc;
    uint8_t* kb;
    kh_line_t L;
    host_string(hs, sizeof hs, host, port);
    rc = kh_load(path, &buf, &n);
    if (rc != SSH_KH_OK)
        return rc;
    kb = (uint8_t*)malloc(8192);
    if (!kb) {
        free(buf);
        return SSH_KH_ERROR;
    }
    while (kh_next(buf, n, &off, &L)) {
        int same, same_type;
        if (L.marker == 2 || !host_match(L.hp, L.hl, hs))
            continue;
        same_type = L.kl == strlen(type) && memcmp(L.kt, type, L.kl) == 0;
        same = kh_blob_is(&L, kb, blob, bloblen);
        if (L.marker == 1) {
            if (same)
                revoked = 1;
            continue;
        }
        if (same && same_type)
            ok = 1;
        else if (same_type)
            changed = 1;
    }
    free(buf);
    free(kb);
    if (revoked)
        return SSH_KH_REVOKED;
    if (ok)
        return SSH_KH_OK;
    return changed ? SSH_KH_CHANGED : SSH_KH_UNKNOWN;
}

int ssh_known_hosts_has_ca(const char* path, const char* host, uint16_t port)
{
    char hs[600];
    char* buf = NULL;
    size_t n = 0, off = 0;
    int found = 0;
    kh_line_t L;
    host_string(hs, sizeof hs, host, port);
    if (kh_load(path, &buf, &n) != SSH_KH_OK)
        return 0;
    while (!found && kh_next(buf, n, &off, &L))
        if (L.marker == 2 && host_match(L.hp, L.hl, hs))
            found = 1;
    free(buf);
    return found;
}

int ssh_known_hosts_check_ca(const char* path, const char* host, uint16_t port,
    const uint8_t* ca, size_t calen, const uint8_t* key, size_t keylen)
{
    char hs[600];
    char* buf = NULL;
    size_t n = 0, off = 0;
    int trusted = 0, revoked = 0, rc;
    uint8_t* kb;
    kh_line_t L;
    host_string(hs, sizeof hs, host, port);
    rc = kh_load(path, &buf, &n);
    if (rc != SSH_KH_OK)
        return rc;
    kb = (uint8_t*)malloc(8192);
    if (!kb) {
        free(buf);
        return SSH_KH_ERROR;
    }
    while (kh_next(buf, n, &off, &L)) {
        if (L.marker == 1) {
            if (kh_blob_is(&L, kb, ca, calen) || kh_blob_is(&L, kb, key, keylen))
                revoked = 1;
            continue;
        }
        if (L.marker == 2 && host_match(L.hp, L.hl, hs) && kh_blob_is(&L, kb, ca, calen))
            trusted = 1;
    }
    free(buf);
    free(kb);
    if (revoked)
        return SSH_KH_REVOKED;
    return trusted ? SSH_KH_OK : SSH_KH_UNKNOWN;
}

int ssh_known_hosts_add(const char* path, const char* host, uint16_t port,
    const char* type, const uint8_t* blob, size_t bloblen)
{
    char hs[600];
    char* line;
    uint8_t salt[20], mac[EVP_MAX_MD_SIZE];
    size_t cap = strlen(type) + bloblen * 2 + 200, n, ml = 0;
    int fd, rc = 0;
    ssize_t w;
    if (bloblen > 8192)
        return -1;
    line = (char*)malloc(cap);
    if (!line)
        return -1;
    host_string(hs, sizeof hs, host, port);
    ssh_random(salt, sizeof salt);
    if (!EVP_Q_mac(NULL, "HMAC", NULL, "SHA1", NULL, salt, sizeof salt, (const unsigned char*)hs, strlen(hs),
            mac, sizeof mac, &ml)
        || ml != 20) {
        free(line);
        return -1;
    }
    memcpy(line, "|1|", 3);
    n = 3;
    n += ssh_b64_encode(salt, sizeof salt, line + n, 1);
    line[n++] = '|';
    n += ssh_b64_encode(mac, 20, line + n, 1);
    n += (size_t)snprintf(line + n, cap - n, " %s ", type);
    n += ssh_b64_encode(blob, bloblen, line + n, 1);
    line[n++] = '\n';
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        free(line);
        return -1;
    }
    w = write(fd, line, n);
    if (w < 0 || (size_t)w != n)
        rc = -1;
    if (close(fd) < 0)
        rc = -1;
    free(line);
    return rc;
}

#endif
