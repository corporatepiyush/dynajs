#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#ifdef CONFIG_OPENSSL
static int dyn_asn1_len(const ASN1_STRING* s)
{
#if OPENSSL_VERSION_NUMBER >= 0x40100000L
    size_t n = ASN1_STRING_get_length(s);
    return n > INT_MAX ? -1 : (int)n;
#else
    return ASN1_STRING_length(s);
#endif
}

JSValue dyn_x509_parse(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const uint8_t* data = NULL;
    size_t len = 0;
    const char* owned = NULL;
    X509* x = NULL;
    BIO* bio = NULL;
    JSValue res = JS_EXCEPTION;
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "X509.parse(cert): certificate (PEM string or DER bytes) is required");

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned) < 0)
        return JS_EXCEPTION;

    if (len >= 10 && !memcmp(data, "-----BEGIN", 10)) {
        bio = BIO_new_mem_buf(data, (int)len);
        if (bio) {
            x = PEM_read_bio_X509(bio, NULL, NULL, NULL);
            BIO_free(bio);
        }
    }
    if (!x) {
        const unsigned char* p = (const unsigned char*)data;
        x = d2i_X509(NULL, &p, (long)len);
        if (x && p != (const unsigned char*)data + len) {
            X509_free(x);
            x = NULL;
        }
    }
    if (owned)
        JS_FreeCString(ctx, owned);

    if (!x)
        return JS_ThrowTypeError(ctx, "X509.parse: invalid X.509 certificate (cannot decode PEM or DER)");

    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        X509_free(x);
        return res;
    }

    {
        char* s_str = X509_NAME_oneline(X509_get_subject_name(x), NULL, 0);
        if (s_str) {
            JS_DefinePropertyValueStr(ctx, res, "subject", JS_NewString(ctx, s_str), JS_PROP_C_W_E);
            OPENSSL_free(s_str);
        }
        char* i_str = X509_NAME_oneline(X509_get_issuer_name(x), NULL, 0);
        if (i_str) {
            JS_DefinePropertyValueStr(ctx, res, "issuer", JS_NewString(ctx, i_str), JS_PROP_C_W_E);
            OPENSSL_free(i_str);
        }
    }

    {
        ASN1_INTEGER* serial = X509_get_serialNumber(x);
        BIGNUM* bn = ASN1_INTEGER_to_BN(serial, NULL);
        if (bn) {
            char* hex = BN_bn2hex(bn);
            if (hex) {
                JS_DefinePropertyValueStr(ctx, res, "serialNumber", JS_NewString(ctx, hex), JS_PROP_C_W_E);
                OPENSSL_free(hex);
            }
            BN_free(bn);
        }
    }

    JS_DefinePropertyValueStr(ctx, res, "version", JS_NewInt32(ctx, (int)X509_get_version(x) + 1), JS_PROP_C_W_E);

    {
        const ASN1_TIME* nb = X509_get0_notBefore(x);
        const ASN1_TIME* na = X509_get0_notAfter(x);
        BIO* b_nb = BIO_new(BIO_s_mem());
        BIO* b_na = BIO_new(BIO_s_mem());
        if (b_nb && nb) {
            ASN1_TIME_print(b_nb, nb);
            BUF_MEM* bm;
            BIO_get_mem_ptr(b_nb, &bm);
            JS_DefinePropertyValueStr(ctx, res, "notBefore", JS_NewStringLen(ctx, bm->data, bm->length), JS_PROP_C_W_E);
            BIO_free(b_nb);
        }
        if (b_na && na) {
            ASN1_TIME_print(b_na, na);
            BUF_MEM* bm;
            BIO_get_mem_ptr(b_na, &bm);
            JS_DefinePropertyValueStr(ctx, res, "notAfter", JS_NewStringLen(ctx, bm->data, bm->length), JS_PROP_C_W_E);
            BIO_free(b_na);
        }
    }

    {
        unsigned char md[32];
        unsigned int md_len = 0;
        if (X509_digest(x, EVP_sha256(), md, &md_len) && md_len == 32) {
            char hex_fp[65];
            size_t k;
            for (k = 0; k < 32; k++)
                snprintf(hex_fp + k * 2, 3, "%02x", md[k]);
            hex_fp[64] = 0;
            JS_DefinePropertyValueStr(ctx, res, "fingerprint", JS_NewString(ctx, hex_fp), JS_PROP_C_W_E);
        }
    }

    {
        JSValue sans_obj = JS_NewObject(ctx);
        JSValue dns_arr = JS_NewArray(ctx);
        JSValue ip_arr = JS_NewArray(ctx);
        JSValue email_arr = JS_NewArray(ctx);
        uint32_t dns_idx = 0, ip_idx = 0, email_idx = 0;
        GENERAL_NAMES* gens = (GENERAL_NAMES*)X509_get_ext_d2i(x, NID_subject_alt_name, NULL, NULL);
        if (gens) {
            int num = sk_GENERAL_NAME_num(gens);
            int i;
            for (i = 0; i < num; i++) {
                const GENERAL_NAME* gn = sk_GENERAL_NAME_value(gens, i);
                if (gn->type == GEN_DNS) {
                    const unsigned char* d = ASN1_STRING_get0_data(gn->d.dNSName);
                    int dlen = dyn_asn1_len(gn->d.dNSName);
                    if (d && dlen > 0) {
                        JS_DefinePropertyValueUint32(ctx, dns_arr, dns_idx++, JS_NewStringLen(ctx, (const char*)d, (size_t)dlen), JS_PROP_C_W_E);
                    }
                } else if (gn->type == GEN_IPADD) {
                    const unsigned char* d = ASN1_STRING_get0_data(gn->d.iPAddress);
                    int dlen = dyn_asn1_len(gn->d.iPAddress);
                    if (d && dlen == 4) {
                        char ipbuf[32];
                        snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", d[0], d[1], d[2], d[3]);
                        JS_DefinePropertyValueUint32(ctx, ip_arr, ip_idx++, JS_NewString(ctx, ipbuf), JS_PROP_C_W_E);
                    } else if (d && dlen == 16) {
                        char ipbuf[64];
                        snprintf(ipbuf, sizeof(ipbuf), "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                            d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                            d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
                        JS_DefinePropertyValueUint32(ctx, ip_arr, ip_idx++, JS_NewString(ctx, ipbuf), JS_PROP_C_W_E);
                    }
                } else if (gn->type == GEN_EMAIL) {
                    const unsigned char* d = ASN1_STRING_get0_data(gn->d.rfc822Name);
                    int dlen = dyn_asn1_len(gn->d.rfc822Name);
                    if (d && dlen > 0) {
                        JS_DefinePropertyValueUint32(ctx, email_arr, email_idx++, JS_NewStringLen(ctx, (const char*)d, (size_t)dlen), JS_PROP_C_W_E);
                    }
                }
            }
            GENERAL_NAMES_free(gens);
        }
        JS_DefinePropertyValueStr(ctx, sans_obj, "dns", dns_arr, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, sans_obj, "ip", ip_arr, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, sans_obj, "email", email_arr, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, res, "sans", sans_obj, JS_PROP_C_W_E);
    }

    X509_free(x);
    return res;
}

static int dyn_js_length(JSContext* ctx, JSValueConst arrv)
{
    JSValue v = JS_GetPropertyStr(ctx, arrv, "length");
    int32_t n;
    if (JS_IsException(v))
        return -1;
    if (JS_ToInt32(ctx, &n, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    return (int)n;
}

static const struct {
    const char* name;
    int nid;
} x509_ext_table[] = {
    { "subjectAltName", NID_subject_alt_name },
    { "keyUsage", NID_key_usage },
    { "extendedKeyUsage", NID_ext_key_usage },
    { "basicConstraints", NID_basic_constraints },
    { "subjectKeyIdentifier", NID_subject_key_identifier },
    { "authorityKeyIdentifier", NID_authority_key_identifier },
};

static int x509_apply_ext(JSContext* ctx, X509* x, int nid,
    const char* valstr, int critical)
{
    X509V3_CTX v3ctx;
    X509_EXTENSION* ex;
    char buf[512];

    X509V3_set_ctx(&v3ctx, x, x, NULL, NULL, 0);
    if (critical) {
        if (snprintf(buf, sizeof buf, "critical,%s", valstr) >= (int)sizeof buf) {
            JS_ThrowRangeError(ctx, "X509.generateSelfSigned: extension value too long");
            return -1;
        }
        valstr = buf;
    }
    ex = X509V3_EXT_nconf_nid(NULL, &v3ctx, nid, DYN_UNCONST(valstr));
    if (!ex) {
        ERR_clear_error();
        JS_ThrowTypeError(ctx,
            "X509.generateSelfSigned: extension value rejected for %s (value: \"%s\")",
            OBJ_nid2sn(nid), valstr);
        return -1;
    }
    if (X509_add_ext(x, ex, -1) != 1) {
        X509_EXTENSION_free(ex);
        ERR_clear_error();
        JS_ThrowInternalError(ctx, "X509.generateSelfSigned: could not add extension");
        return -1;
    }
    X509_EXTENSION_free(ex);
    return 0;
}

static int x509_host_shaped(const char* s)
{
    size_t n = strlen(s), i;
    if (n == 0 || n > 253 || s[0] == '.' || s[0] == '-' || s[n - 1] == '.' || s[n - 1] == '-')
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'
                || (c == '*' && i == 0 && n > 2 && s[1] == '.')))
            return 0;
    }
    return 1;
}

static const char* x509_san_kind(const char* sv)
{
    struct in_addr a4;
    struct in6_addr a6;
    if (strchr(sv, '@'))
        return "email";
    if (inet_pton(AF_INET, sv, &a4) == 1 || inet_pton(AF_INET6, sv, &a6) == 1)
        return "IP";
    return "DNS";
}

JSValue dyn_x509_generate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char *subj = "localhost", *key_str = NULL;
    int64_t days = 365;
    size_t key_len = 0;
    int subj_owned = 0;
    EVP_PKEY* pkey = NULL;
    X509* x = NULL;
    BIO* bio = NULL;
    JSValue sansv = JS_UNDEFINED, extv = JS_UNDEFINED;
    int ext_san = 0;
    JSValue ret = JS_EXCEPTION, v;
    (void)this_val;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "X509.generateSelfSigned({ key, subject?, days?, sans?, extensions? })");

    {
        JSPropertyEnum* props = NULL;
        uint32_t nprops = 0, i;
        static const char* const ok_keys[] = { "key", "subject", "days", "sans", "extensions" };
        if (JS_GetOwnPropertyNames(ctx, &props, &nprops, argv[0],
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            return JS_EXCEPTION;
        for (i = 0; i < nprops; i++) {
            const char* k = JS_AtomToCString(ctx, props[i].atom);
            int j, known = 0;
            if (!k) {
                JS_FreePropertyEnum(ctx, props, nprops);
                return JS_EXCEPTION;
            }
            for (j = 0; j < (int)countof(ok_keys); j++)
                if (!strcmp(k, ok_keys[j]))
                    known = 1;
            if (!known)
                JS_ThrowTypeError(ctx,
                    "X509.generateSelfSigned: unknown option \"%s\" (valid: key, subject, days, sans, extensions)", k);
            JS_FreeCString(ctx, k);
            if (JS_HasException(ctx))
                break;
        }
        JS_FreePropertyEnum(ctx, props, nprops);
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
    }

    v = JS_GetPropertyStr(ctx, argv[0], "key");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    key_str = JS_ToCStringLen(ctx, &key_len, v);
    JS_FreeValue(ctx, v);
    if (!key_str)
        return JS_EXCEPTION;

    v = JS_GetPropertyStr(ctx, argv[0], "subject");
    if (JS_IsException(v))
        goto optfail;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        subj = JS_ToCString(ctx, v);
        if (!subj)
            goto optfail;
        JS_FreeValue(ctx, v);
        subj_owned = 1;
    } else {
        JS_FreeValue(ctx, v);
    }
    v = JS_GetPropertyStr(ctx, argv[0], "days");
    if (JS_IsException(v))
        goto optfail;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        int rc = JS_ToInt64(ctx, &days, v);
        if (rc)
            goto optfail;
        JS_FreeValue(ctx, v);
        if (days < 1)
            days = 1;
    } else {
        JS_FreeValue(ctx, v);
    }
    v = JS_GetPropertyStr(ctx, argv[0], "sans");
    if (JS_IsException(v))
        goto optfail;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (!JS_IsArray(ctx, v)) {
            JS_ThrowTypeError(ctx, "X509.generateSelfSigned: sans must be an array of strings");
            goto optfail;
        }
        sansv = v;
        v = JS_UNDEFINED;
    } else {
        v = JS_UNDEFINED;
    }
    v = JS_GetPropertyStr(ctx, argv[0], "extensions");
    if (JS_IsException(v))
        goto optfail;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (!JS_IsArray(ctx, v)) {
            JS_ThrowTypeError(ctx, "X509.generateSelfSigned: extensions must be an array");
            goto optfail;
        }
        extv = v;
        v = JS_UNDEFINED;
    } else {
        v = JS_UNDEFINED;
    }

    bio = BIO_new_mem_buf(key_str, (int)key_len);
    if (bio) {
        pkey = PEM_read_bio_PrivateKey(bio, NULL, dyn_pem_no_passphrase, NULL);
        BIO_free(bio);
    }
    JS_FreeCString(ctx, key_str);
    key_str = NULL;

    if (!pkey) {
        if (subj_owned)
            JS_FreeCString(ctx, subj);
        JS_FreeValue(ctx, sansv);
        JS_FreeValue(ctx, extv);
        return JS_ThrowTypeError(ctx, "X509.generateSelfSigned: invalid private key PEM");
    }

    x = X509_new();
    if (!x) {
        EVP_PKEY_free(pkey);
        if (subj_owned)
            JS_FreeCString(ctx, subj);
        return JS_ThrowOutOfMemory(ctx);
    }

    {
        uint8_t serial_bytes[16];
        BIGNUM* serial_bn;
        if (dyn_os_entropy(serial_bytes, sizeof serial_bytes) < 0) {
            JS_ThrowInternalError(ctx, "X509.generateSelfSigned: no entropy for the serial number");
            goto extfail;
        }
        serial_bytes[0] = (uint8_t)((serial_bytes[0] & 0x7F) | 0x40);
        serial_bn = BN_bin2bn(serial_bytes, (int)sizeof serial_bytes, NULL);
        if (!serial_bn || !BN_to_ASN1_INTEGER(serial_bn, X509_get_serialNumber(x))) {
            BN_free(serial_bn);
            ERR_clear_error();
            JS_ThrowOutOfMemory(ctx);
            goto extfail;
        }
        BN_free(serial_bn);
    }
    X509_set_version(x, 2);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    if (days > (int64_t)(LONG_MAX / 86400L)) {
        JS_ThrowRangeError(ctx,
            "X509.generateSelfSigned: days overflows the certificate validity");
        goto extfail;
    }
    if (!X509_gmtime_adj(X509_getm_notAfter(x), (long)days * 86400L)) {
        ERR_clear_error();
        JS_ThrowRangeError(ctx,
            "X509.generateSelfSigned: days is beyond the representable notAfter range");
        goto extfail;
    }
    X509_set_pubkey(x, pkey);

    {
        X509_NAME* name = X509_NAME_new();
        int named = name
            && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)(subj ? subj : "localhost"), -1, -1, 0)
            && X509_set_subject_name(x, name) && X509_set_issuer_name(x, name);
        X509_NAME_free(name);
        if (!named) {
            JS_ThrowInternalError(ctx, "X509.generateSelfSigned: could not set the certificate name");
            goto extfail;
        }
    }
    {
        char sanbuf[2048];
        size_t sanlen = 0;
        int i, n;

        if (!JS_IsUndefined(sansv)) {
            n = dyn_js_length(ctx, sansv);
            if (n < 0)
                goto extfail;
            if (n > 256) {
                JS_ThrowRangeError(ctx, "X509.generateSelfSigned: at most 256 sans");
                goto extfail;
            }
            for (i = 0; i < n; i++) {
                JSValue ev = JS_GetPropertyUint32(ctx, sansv, (uint32_t)i);
                const char* sv;
                const char* kind;
                size_t klen;
                if (JS_IsException(ev))
                    goto extfail;
                sv = JS_ToCStringLen(ctx, &klen, ev);
                JS_FreeValue(ctx, ev);
                if (!sv)
                    goto extfail;
                if (klen == 0 || klen > 1000) {
                    JS_ThrowRangeError(ctx,
                        "X509.generateSelfSigned: san entries are 1..1000 characters");
                    JS_FreeCString(ctx, sv);
                    goto extfail;
                }
                if (memchr(sv, ',', klen) || memchr(sv, '\n', klen) || memchr(sv, '\r', klen) || memchr(sv, 0, klen)) {
                    JS_ThrowTypeError(ctx,
                        "X509.generateSelfSigned: a san entry must not contain a comma, a line break or NUL");
                    JS_FreeCString(ctx, sv);
                    goto extfail;
                }
                kind = x509_san_kind(sv);
                if (sanlen + klen + 16 >= sizeof sanbuf) {
                    JS_ThrowRangeError(ctx, "X509.generateSelfSigned: sans exceed the 2 KiB extension limit");
                    JS_FreeCString(ctx, sv);
                    goto extfail;
                }
                if (sanlen)
                    sanbuf[sanlen++] = ',';
                sanlen += (size_t)snprintf(sanbuf + sanlen, sizeof sanbuf - sanlen,
                    "%s:%s", kind, sv);
                JS_FreeCString(ctx, sv);
            }
        }

        if (!JS_IsUndefined(extv)) {
            n = dyn_js_length(ctx, extv);
            if (n < 0)
                goto extfail;
            if (n > 64) {
                JS_ThrowRangeError(ctx, "X509.generateSelfSigned: at most 64 extensions");
                goto extfail;
            }
            for (i = 0; i < n; i++) {
                JSValue ev = JS_GetPropertyUint32(ctx, extv, (uint32_t)i);
                JSValue namev = JS_UNDEFINED, valv = JS_UNDEFINED, criv = JS_UNDEFINED;
                const char *name = NULL, *val = NULL;
                int nid = 0, j, critical = 0, found = 0;
                if (JS_IsException(ev))
                    goto extfail;
                if (!JS_IsObject(ev)) {
                    JS_FreeValue(ctx, ev);
                    JS_ThrowTypeError(ctx,
                        "X509.generateSelfSigned: extensions entries must be { name, value, critical? } objects");
                    goto extfail;
                }
                namev = JS_GetPropertyStr(ctx, ev, "name");
                if (JS_IsException(namev)) {
                    JS_FreeValue(ctx, ev);
                    goto extfail;
                }
                valv = JS_GetPropertyStr(ctx, ev, "value");
                if (JS_IsException(valv)) {
                    JS_FreeValue(ctx, namev);
                    JS_FreeValue(ctx, ev);
                    goto extfail;
                }
                criv = JS_GetPropertyStr(ctx, ev, "critical");
                if (JS_IsException(criv)) {
                    JS_FreeValue(ctx, valv);
                    JS_FreeValue(ctx, namev);
                    JS_FreeValue(ctx, ev);
                    goto extfail;
                }
                if (!JS_IsUndefined(criv) && !JS_IsNull(criv)) {
                    critical = JS_ToBool(ctx, criv);
                    if (critical == -1)
                        critical = -1;
                }
                if (!JS_IsString(namev) || !JS_IsString(valv)) {
                    JS_ThrowTypeError(ctx,
                        "X509.generateSelfSigned: extension entry needs string name and value");
                } else {
                    name = JS_ToCString(ctx, namev);
                    if (name)
                        val = JS_ToCString(ctx, valv);
                    if (name && val) {
                        for (j = 0; j < (int)countof(x509_ext_table); j++) {
                            if (!strcmp(name, x509_ext_table[j].name)) {
                                nid = x509_ext_table[j].nid;
                                found = 1;
                                break;
                            }
                        }
                        if (found && nid == NID_subject_alt_name && sanlen) {
                            JS_ThrowTypeError(ctx,
                                "X509.generateSelfSigned: extensions names subjectAltName while sans is also set -- ambiguous, use one");
                        } else if (!found) {
                            JS_ThrowTypeError(ctx,
                                "X509.generateSelfSigned: unsupported extension \"%s\" (supported: subjectAltName, keyUsage, extendedKeyUsage, basicConstraints, subjectKeyIdentifier, authorityKeyIdentifier)",
                                name);
                        } else if (critical == -1) {
                            if (!JS_HasException(ctx))
                                JS_ThrowTypeError(ctx,
                                    "X509.generateSelfSigned: extension entry needs string name and value");
                        } else if (x509_apply_ext(ctx, x, nid, val, critical) != 0) {
                        } else {
                            if (nid == NID_subject_alt_name)
                                ext_san = 1;
                            nid = -nid;
                        }
                        if (name)
                            JS_FreeCString(ctx, name);
                        if (val)
                            JS_FreeCString(ctx, val);
                    } else {
                        if (name)
                            JS_FreeCString(ctx, name);
                    }
                }
                JS_FreeValue(ctx, criv);
                JS_FreeValue(ctx, valv);
                JS_FreeValue(ctx, namev);
                JS_FreeValue(ctx, ev);
                if (JS_HasException(ctx) || nid >= 0)
                    goto extfail;
            }
        }

        if (!sanlen && !ext_san && JS_IsUndefined(sansv)) {
            const char* dsubj = subj ? subj : "localhost";
            const char* kind = x509_san_kind(dsubj);
            if (!strcmp(kind, "IP") || (!strcmp(kind, "DNS") && x509_host_shaped(dsubj)))
                sanlen = (size_t)snprintf(sanbuf, sizeof sanbuf, "%s:%s", kind, dsubj);
            if (sanlen >= sizeof sanbuf)
                sanlen = 0;
        }
        if (sanlen) {
            if (x509_apply_ext(ctx, x, NID_subject_alt_name, sanbuf, 0) != 0)
                goto extfail;
        }
    }
    JS_FreeValue(ctx, sansv);
    sansv = JS_UNDEFINED;
    JS_FreeValue(ctx, extv);
    extv = JS_UNDEFINED;

    {
        int ed = EVP_PKEY_is_a(pkey, "ED25519") || EVP_PKEY_is_a(pkey, "ED448");
        if (!X509_sign(x, pkey, ed ? NULL : EVP_sha256())) {
            X509_free(x);
            EVP_PKEY_free(pkey);
            if (subj_owned)
                JS_FreeCString(ctx, subj);
            return JS_ThrowInternalError(ctx, "X509.generateSelfSigned: certificate signing failed");
        }
    }

    bio = BIO_new(BIO_s_mem());
    if (bio && PEM_write_bio_X509(bio, x)) {
        BUF_MEM* bm;
        BIO_get_mem_ptr(bio, &bm);
        ret = JS_NewStringLen(ctx, bm->data, bm->length);
    } else {
        ret = JS_ThrowOutOfMemory(ctx);
    }

    if (bio)
        BIO_free(bio);
    X509_free(x);
    EVP_PKEY_free(pkey);
    if (subj_owned)
        JS_FreeCString(ctx, subj);
    return ret;

extfail:
    X509_free(x);
    EVP_PKEY_free(pkey);
    JS_FreeValue(ctx, sansv);
    JS_FreeValue(ctx, extv);
    if (subj_owned)
        JS_FreeCString(ctx, subj);
    return JS_EXCEPTION;

optfail:
    JS_FreeValue(ctx, v);
    JS_FreeCString(ctx, key_str);
    JS_FreeValue(ctx, sansv);
    JS_FreeValue(ctx, extv);
    if (subj_owned)
        JS_FreeCString(ctx, subj);
    return JS_EXCEPTION;
}
#endif

#endif
