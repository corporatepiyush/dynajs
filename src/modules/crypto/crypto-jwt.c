#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#ifdef CONFIG_OPENSSL
static int jwt_es_coord(const char* alg)
{
    if (!strcmp(alg, "ES256"))
        return 32;
    if (!strcmp(alg, "ES384"))
        return 48;
    if (!strcmp(alg, "ES512"))
        return 66;
    return 0;
}

static const EVP_MD* jwt_md_for(const char* alg)
{
    if (!strncmp(alg + 2, "256", 3))
        return EVP_sha256();
    if (!strncmp(alg + 2, "384", 3))
        return EVP_sha384();
    if (!strncmp(alg + 2, "512", 3))
        return EVP_sha512();
    return NULL;
}

static int jwt_is_asym(const char* alg)
{
    return alg && strlen(alg) == 5 && (alg[0] == 'R' || alg[0] == 'E') && alg[1] == 'S' && jwt_md_for(alg) != NULL;
}

static int jwt_key_matches_alg(const char* alg, const EVP_PKEY* k)
{
    if (alg[0] == 'R')
        return EVP_PKEY_is_a(k, "RSA");
    {
        const char* want = NULL;
        char group[64];
        size_t group_len = 0;
        if (!strcmp(alg, "ES256"))
            want = "prime256v1";
        else if (!strcmp(alg, "ES384"))
            want = "secp384r1";
        else if (!strcmp(alg, "ES512"))
            want = "secp521r1";
        if (!want || !EVP_PKEY_is_a(k, "EC"))
            return 0;
        if (EVP_PKEY_get_utf8_string_param(k, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof group, &group_len) != 1)
            return 0;
        return !strcmp(group, want);
    }
}

EVP_PKEY* jwt_pem_key(const char* pem, size_t pemlen, int want_private)
{
    BIO* b = BIO_new_mem_buf(pem, (int)pemlen);
    EVP_PKEY* k = NULL;
    if (!b)
        return NULL;
    k = want_private ? PEM_read_bio_PrivateKey(b, NULL, dyn_pem_no_passphrase, NULL)
                     : PEM_read_bio_PUBKEY(b, NULL, dyn_pem_no_passphrase, NULL);
    BIO_free(b);
    if (!k)
        ERR_clear_error();
    return k;
}

int jwt_der_to_raw(const uint8_t* der, size_t derlen, int coord,
    uint8_t* out)
{
    const uint8_t* p = der;
    ECDSA_SIG* sig = d2i_ECDSA_SIG(NULL, &p, (long)derlen);
    const BIGNUM *r, *s;
    int ok;
    if (!sig) {
        ERR_clear_error();
        return -1;
    }
    ECDSA_SIG_get0(sig, &r, &s);
    ok = BN_bn2binpad(r, out, coord) == coord && BN_bn2binpad(s, out + coord, coord) == coord;
    ECDSA_SIG_free(sig);
    return ok ? 0 : -1;
}

int jwt_raw_to_der(const uint8_t* raw, int coord, uint8_t** der,
    int* derlen)
{
    ECDSA_SIG* sig = ECDSA_SIG_new();
    BIGNUM* r = BN_bin2bn(raw, coord, NULL);
    BIGNUM* s = BN_bin2bn(raw + coord, coord, NULL);
    int n;
    *der = NULL;
    if (!sig || !r || !s) {
        if (r)
            BN_free(r);
        if (s)
            BN_free(s);
        if (sig)
            ECDSA_SIG_free(sig);
        ERR_clear_error();
        return -1;
    }
    ECDSA_SIG_set0(sig, r, s);
    n = i2d_ECDSA_SIG(sig, der);
    ECDSA_SIG_free(sig);
    if (n <= 0) {
        ERR_clear_error();
        return -1;
    }
    *derlen = n;
    return 0;
}

static int jwt_asym_sign(const char* alg, const char* pem, size_t pemlen,
    const uint8_t* msg, size_t msglen,
    uint8_t** sigout, size_t* sigoutlen)
{
    EVP_PKEY* k = jwt_pem_key(pem, pemlen, 1);
    EVP_MD_CTX* md = NULL;
    uint8_t* buf = NULL;
    size_t n = 0;
    int coord = jwt_es_coord(alg), rc = -1;

    *sigout = NULL;
    *sigoutlen = 0;
    if (!k)
        return -1;
    if (dyn_rsa_too_small(k)) {
        EVP_PKEY_free(k);
        return -2;
    }
    if (!jwt_key_matches_alg(alg, k)) {
        EVP_PKEY_free(k);
        return -3;
    }
    md = EVP_MD_CTX_new();
    if (!md || EVP_DigestSignInit(md, NULL, jwt_md_for(alg), NULL, k) <= 0)
        goto done;
    if (EVP_DigestSign(md, NULL, &n, msg, msglen) <= 0)
        goto done;
    buf = (uint8_t*)malloc(n ? n : 1);
    if (!buf)
        goto done;
    if (EVP_DigestSign(md, buf, &n, msg, msglen) <= 0)
        goto done;
    if (coord) {
        uint8_t* raw = (uint8_t*)malloc((size_t)coord * 2);
        if (!raw || jwt_der_to_raw(buf, n, coord, raw) != 0) {
            free(raw);
            goto done;
        }
        free(buf);
        buf = raw;
        n = (size_t)coord * 2;
    }
    *sigout = buf;
    *sigoutlen = n;
    buf = NULL;
    rc = 0;
done:
    ERR_clear_error();
    free(buf);
    if (md)
        EVP_MD_CTX_free(md);
    EVP_PKEY_free(k);
    return rc;
}

static int jwt_asym_verify(const char* alg, const char* pem, size_t pemlen,
    const uint8_t* msg, size_t msglen,
    const uint8_t* sig, size_t siglen)
{
    EVP_PKEY* k = jwt_pem_key(pem, pemlen, 0);
    EVP_MD_CTX* md = NULL;
    uint8_t* der = NULL;
    int coord = jwt_es_coord(alg), derlen = 0, rc = 0;

    if (!k)
        return -1;
    if (dyn_rsa_too_small(k)) {
        EVP_PKEY_free(k);
        return -2;
    }
    if (!jwt_key_matches_alg(alg, k)) {
        EVP_PKEY_free(k);
        return -3;
    }
    if (coord) {
        if (siglen != (size_t)coord * 2)
            goto done;
        if (jwt_raw_to_der(sig, coord, &der, &derlen) != 0)
            goto done;
        sig = der;
        siglen = (size_t)derlen;
    }
    md = EVP_MD_CTX_new();
    if (md && EVP_DigestVerifyInit(md, NULL, jwt_md_for(alg), NULL, k) > 0)
        rc = EVP_DigestVerify(md, sig, siglen, msg, msglen) == 1;
done:
    ERR_clear_error();
    if (der)
        OPENSSL_free(der);
    if (md)
        EVP_MD_CTX_free(md);
    EVP_PKEY_free(k);
    return rc;
}
#endif

static int jwt_b64url_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '-')
        return 62;
    if (c == '_')
        return 63;
    return -1;
}

static int jwt_b64url_canonical(const char* seg, size_t n)
{
    size_t i;
    int last;
    if (n % 4 == 1)
        return 0;
    for (i = 0; i < n; i++)
        if (jwt_b64url_value((unsigned char)seg[i]) < 0)
            return 0;
    if (n == 0 || n % 4 == 0)
        return 1;
    last = jwt_b64url_value((unsigned char)seg[n - 1]);
    return n % 4 == 2 ? (last & 0x0F) == 0 : (last & 0x03) == 0;
}

static int jwt_key_looks_like_pem(const uint8_t* key, size_t keylen)
{
    static const char marker[] = "-----BEGIN ";
    size_t i = 0, m = sizeof(marker) - 1;
    while (i < keylen && (key[i] == ' ' || key[i] == '\t' || key[i] == '\r' || key[i] == '\n'))
        i++;
    return keylen - i >= m && memcmp(key + i, marker, m) == 0;
}

static size_t jwt_b64_len(size_t n)
{
    size_t r = n % 4;
    return n / 4 * 3 + (r == 2 ? 1 : r == 3 ? 2
                                            : 0);
}

static const char* jwt_hash_for(const char* alg)
{
    if (!strcmp(alg, "HS256"))
        return "sha256";
    if (!strcmp(alg, "HS384"))
        return "sha384";
    if (!strcmp(alg, "HS512"))
        return "sha512";
    return NULL;
}

static int jwt_b64url_json(JSContext* ctx, JSValueConst obj, char** out,
    size_t* outn)
{
    JSValue j = JS_JSONStringify(ctx, obj, JS_UNDEFINED, JS_UNDEFINED);
    const char* t;
    size_t tn;
    char* b;
    if (JS_IsException(j))
        return -1;
    t = JS_ToCStringLen(ctx, &tn, j);
    JS_FreeValue(ctx, j);
    if (!t)
        return -1;
    b = (char*)js_malloc(ctx, tn * 4 / 3 + 8);
    if (!b) {
        JS_FreeCString(ctx, t);
        return -1;
    }
    *outn = dyn_codec_base64url_encode((const uint8_t*)t, tn, b);
    JS_FreeCString(ctx, t);
    *out = b;
    return 0;
}

static const char* const jwt_sign_keys[] = { "alg" };
static const char* const jwt_verify_keys[] = { "algorithms" };

JSValue dyn_crypto_jwt_sign(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *alg = NULL, *hname, *kown = NULL;
    const uint8_t* key;
    size_t keylen;
    const dyn_hash_algo_t* a;
    char *ph = NULL, *pp = NULL, hdr[128], *signing = NULL;
    size_t phn = 0, ppn = 0, sn, hn;
    uint8_t mac[DYN_HASH_MAX_DIGEST];
    JSValue hv = JS_UNDEFINED, hobj, r = JS_EXCEPTION;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "JWTSign(payload, key[, { alg }])");
    if (argc > 2 && JS_IsObject(argv[2])) {
        if (dyn_opts_strict(ctx, argv[2], jwt_sign_keys, countof(jwt_sign_keys)) < 0)
            return JS_EXCEPTION;
        hv = JS_GetPropertyStr(ctx, argv[2], "alg");
        if (!JS_IsUndefined(hv))
            alg = JS_ToCString(ctx, hv);
    }
    hname = jwt_hash_for(alg ? alg : "HS256");
#ifdef CONFIG_OPENSSL
    if (!hname && jwt_is_asym(alg)) {
        a = NULL;
    } else
#endif
    {
        if (!hname) {
            JS_ThrowTypeError(ctx, "JWTSign: alg must be HS256, HS384 or HS512"
#ifdef CONFIG_OPENSSL
                                   ", RS256/384/512 or ES256/384/512"
#endif
            );
            goto done;
        }
        a = dyn_hash_algo_by_name(hname);
    }
    hn = (size_t)snprintf(hdr, sizeof(hdr), "{\"alg\":\"%.5s\",\"typ\":\"JWT\"}",
        alg ? alg : "HS256");
    if (hn >= sizeof(hdr))
        hn = sizeof(hdr) - 1;
    ph = (char*)js_malloc(ctx, hn * 4 / 3 + 8);
    if (!ph)
        goto done;
    phn = dyn_codec_base64url_encode((const uint8_t*)hdr, hn, ph);
    if (jwt_b64url_json(ctx, argv[1 - 1 + 0], &pp, &ppn) < 0)
        goto done;

    signing = (char*)js_malloc(ctx, phn + 1 + ppn + 1);
    if (!signing)
        goto done;
    memcpy(signing, ph, phn);
    signing[phn] = '.';
    memcpy(signing + phn + 1, pp, ppn);
    sn = phn + 1 + ppn;
    signing[sn] = 0;

    if (dyn_crypto_secret(ctx, argv[1], &key, &keylen, &kown))
        goto done;
    {
        const uint8_t* rawsig;
        size_t rawlen;
        uint8_t* heapsig = NULL;
        char* b64;
        size_t bn;
        char* tok;
#ifdef CONFIG_OPENSSL
        if (!a) {
            int s = jwt_asym_sign(alg, (const char*)key, keylen,
                (const uint8_t*)signing, sn,
                &heapsig, &rawlen);
            if (s == -2) {
                JS_ThrowRangeError(ctx,
                    "JWTSign: RSA private key under 2048 bits refused");
                goto done;
            }
            if (s == -3) {
                JS_ThrowTypeError(ctx,
                    "JWTSign: the private key does not match alg %s", alg);
                goto done;
            }
            if (s != 0) {
                JS_ThrowTypeError(ctx,
                    "JWTSign: could not sign with that PEM private key");
                goto done;
            }
            rawsig = heapsig;
        } else
#endif
        {
            dyn_hmac(a, key, keylen, (const uint8_t*)signing, sn, mac);
            rawsig = mac;
            rawlen = a->digest_size;
        }
        b64 = (char*)js_malloc(ctx, rawlen * 4 / 3 + 8);
        if (!b64) {
            free(heapsig);
            goto done;
        }
        bn = dyn_codec_base64url_encode(rawsig, rawlen, b64);
        free(heapsig);
        tok = (char*)js_malloc(ctx, sn + 1 + bn + 1);
        if (tok) {
            memcpy(tok, signing, sn);
            tok[sn] = '.';
            memcpy(tok + sn + 1, b64, bn);
            tok[sn + 1 + bn] = 0;
            r = JS_NewString(ctx, tok);
            js_free(ctx, tok);
        }
        js_free(ctx, b64);
    }
done:
    if (kown)
        JS_FreeCString(ctx, kown);
    if (alg)
        JS_FreeCString(ctx, alg);
    JS_FreeValue(ctx, hv);
    js_free(ctx, ph);
    js_free(ctx, pp);
    js_free(ctx, signing);
    (void)hobj;
    return r;
}

JSValue dyn_crypto_jwt_verify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *tok = NULL, *kown = NULL;
    const uint8_t* key;
    size_t keylen, tn = 0;
    const char *d1, *d2;
    char alg[16];
    const dyn_hash_algo_t* a;
    uint8_t mac[DYN_HASH_MAX_DIGEST], got[DYN_HASH_MAX_DIGEST];
    JSValue algs = JS_UNDEFINED, r = JS_EXCEPTION, hdrv = JS_UNDEFINED;
    size_t hn, bn, i;
    uint32_t nalg = 0;
    int allowed = 0;
    (void)this_val;

    if (argc < 3 || !JS_IsObject(argv[2]))
        return JS_ThrowTypeError(ctx,
            "JWTVerify(token, key, { algorithms }) -- algorithms is REQUIRED");
    if (dyn_opts_strict(ctx, argv[2], jwt_verify_keys, countof(jwt_verify_keys)) < 0)
        return JS_EXCEPTION;
    algs = JS_GetPropertyStr(ctx, argv[2], "algorithms");
    if (!JS_IsArray(ctx, algs)) {
        JS_ThrowTypeError(ctx, "JWTVerify: `algorithms` must be an array; "
                               "without it alg:none and HS/RS confusion are open");
        goto done;
    }
    tok = JS_ToCStringLen(ctx, &tn, argv[0]);
    if (!tok)
        goto done;
    d1 = memchr(tok, '.', tn);
    if (!d1) {
        JS_ThrowTypeError(ctx, "JWTVerify: malformed token");
        goto done;
    }
    d2 = memchr(d1 + 1, '.', tn - (size_t)(d1 + 1 - tok));
    if (!d2) {
        JS_ThrowTypeError(ctx, "JWTVerify: malformed token");
        goto done;
    }

    {
        uint8_t hb[512];
        char hs[520];
        size_t hlen = (size_t)(d1 - tok);
        size_t hl = (hlen > 500) ? 0
                                 : dyn_codec_base64url_decode(tok, hlen, hb, hs);
        if (hl == DYN_CODEC_BAD)
            hl = 0;
        if (hl > jwt_b64_len(hlen))
            hl = jwt_b64_len(hlen);
        hb[hl] = 0;
        JSValue h;
        const char* av;
        if (hl == 0) {
            JS_ThrowTypeError(ctx, "JWTVerify: bad header");
            goto done;
        }
        h = JS_ParseJSON(ctx, (const char*)hb, hl, "<jwt>");
        if (JS_IsException(h))
            goto done;
        {
            JSValue critv = JS_GetPropertyStr(ctx, h, "crit");
            int has_crit = !JS_IsUndefined(critv);
            JS_FreeValue(ctx, critv);
            if (has_crit) {
                JS_FreeValue(ctx, h);
                JS_ThrowTypeError(ctx, "JWTVerify: the header carries `crit`, and no critical extension is supported");
                goto done;
            }
        }
        hdrv = JS_GetPropertyStr(ctx, h, "alg");
        JS_FreeValue(ctx, h);
        {
            size_t avn = 0;
            av = JS_IsString(hdrv) ? JS_ToCStringLen(ctx, &avn, hdrv) : NULL;
            if (!av) {
                JS_ThrowTypeError(ctx, "JWTVerify: header has no alg");
                goto done;
            }
            if (avn == 0 || avn >= sizeof(alg) || memchr(av, 0, avn)) {
                JS_FreeCString(ctx, av);
                JS_ThrowTypeError(ctx, "JWTVerify: header alg is malformed");
                goto done;
            }
            memcpy(alg, av, avn);
            alg[avn] = 0;
            JS_FreeCString(ctx, av);
        }
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, algs, "length");
        int bad = JS_IsException(lv) || JS_ToUint32(ctx, &nalg, lv);
        JS_FreeValue(ctx, lv);
        if (bad)
            goto done;
    }
    for (i = 0; i < nalg; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, algs, (uint32_t)i);
        const char* es;
        if (JS_IsException(e))
            goto done;
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            JS_ThrowTypeError(ctx, "JWTVerify: algorithms[%u] must be a string", (unsigned)i);
            goto done;
        }
        es = JS_ToCString(ctx, e);
        JS_FreeValue(ctx, e);
        if (!es)
            goto done;
        if (!allowed && !strcmp(es, alg))
            allowed = 1;
        JS_FreeCString(ctx, es);
    }
    if (!allowed) {
        JS_ThrowTypeError(ctx, "JWTVerify: alg %s is not in the allowlist", alg);
        goto done;
    }
    if (!jwt_hash_for(alg)
#ifdef CONFIG_OPENSSL
        && !jwt_is_asym(alg)
#endif
    ) {
        JS_ThrowTypeError(ctx, "JWTVerify: alg %s is not supported here", alg);
        goto done;
    }
    if (dyn_crypto_secret(ctx, argv[1], &key, &keylen, &kown))
        goto done;
    hn = (size_t)(d2 - tok);
    if (!jwt_b64url_canonical(d2 + 1, tn - (size_t)(d2 + 1 - tok))) {
        JS_ThrowTypeError(ctx, "JWTVerify: signature is not canonical base64url");
        goto done;
    }
#ifdef CONFIG_OPENSSL
    if (jwt_is_asym(alg)) {
        size_t slen = tn - (size_t)(d2 + 1 - tok), sl;
        uint8_t* sig = (uint8_t*)js_malloc(ctx, slen + 4);
        char* scratch = (char*)js_malloc(ctx, slen + 8);
        int v;
        if (!sig || !scratch) {
            js_free(ctx, sig);
            js_free(ctx, scratch);
            goto done;
        }
        sl = dyn_codec_base64url_decode(d2 + 1, slen, sig, scratch);
        js_free(ctx, scratch);
        if (sl == DYN_CODEC_BAD)
            sl = 0;
        v = jwt_asym_verify(alg, (const char*)key, keylen,
            (const uint8_t*)tok, hn, sig, sl);
        js_free(ctx, sig);
        if (v == -2) {
            JS_ThrowRangeError(ctx,
                "JWTVerify: RSA key under 2048 bits refused");
            goto done;
        }
        if (v == -3) {
            JS_ThrowTypeError(ctx,
                "JWTVerify: the public key does not match alg %s", alg);
            goto done;
        }
        if (v < 0) {
            JS_ThrowTypeError(ctx, "JWTVerify: could not read that PEM public key");
            goto done;
        }
        if (v == 0) {
            JS_ThrowTypeError(ctx, "JWTVerify: signature does not verify");
            goto done;
        }
    } else
#endif
    {
        if (keylen == 0) {
            JS_ThrowTypeError(ctx, "JWTVerify: an HMAC key must not be empty");
            goto done;
        }
        if (jwt_key_looks_like_pem(key, keylen)) {
            JS_ThrowTypeError(ctx, "JWTVerify: a PEM key cannot verify an HS* token (key family does not match alg %s)", alg);
            goto done;
        }
        a = dyn_hash_algo_by_name(jwt_hash_for(alg));
        dyn_hmac(a, key, keylen, (const uint8_t*)tok, hn, mac);
        {
            char ss[192];
            size_t slen = tn - (size_t)(d2 + 1 - tok);
            if (slen > 180 || jwt_b64_len(slen) > sizeof(got))
                bn = 0;
            else {
                bn = dyn_codec_base64url_decode(d2 + 1, slen, got, ss);
                if (bn == DYN_CODEC_BAD)
                    bn = 0;
                if (bn > jwt_b64_len(slen))
                    bn = jwt_b64_len(slen);
            }
        }
        if (bn != a->digest_size || dyn_ct_equal(mac, got, bn) != 1) {
            JS_ThrowTypeError(ctx, "JWTVerify: signature does not verify");
            goto done;
        }
    }
    {
        uint8_t* pb = (uint8_t*)js_malloc(ctx, tn + 1);
        char* ps = (char*)js_malloc(ctx, tn + 8);
        size_t pl;
        if (!pb || !ps) {
            js_free(ctx, pb);
            js_free(ctx, ps);
            goto done;
        }
        pl = dyn_codec_base64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), pb, ps);
        js_free(ctx, ps);
        if (pl != DYN_CODEC_BAD) {
            size_t want = jwt_b64_len((size_t)(d2 - d1 - 1));
            if (pl > want)
                pl = want;
            pb[pl] = 0;
        }
        if (pl == DYN_CODEC_BAD) {
            js_free(ctx, pb);
            JS_ThrowTypeError(ctx, "JWTVerify: bad payload encoding");
            goto done;
        }
        r = JS_ParseJSON(ctx, (const char*)pb, pl, "<jwt>");
        js_free(ctx, pb);
    }
done:
    if (kown)
        JS_FreeCString(ctx, kown);
    if (tok)
        JS_FreeCString(ctx, tok);
    JS_FreeValue(ctx, hdrv);
    JS_FreeValue(ctx, algs);
    return r;
}

#endif
