#ifndef CRYPTO_INTERNAL_H
#define CRYPTO_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/dyn-hash.h"
#include "core/dyn-codec.h"
#include "core/dyn-prng.h"
#include "core/dyn-par.h"
#include "core/dyn-cryptok.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

int dyn_crypto_data(JSContext* ctx, JSValueConst v, const uint8_t** pdata,
    size_t* plen, const char** powned);

int dyn_kdf_meta(JSContext* ctx, JSValueConst opts, const char* who, uint32_t* cap);
int dyn_kdf_begin(JSContext* ctx, int memhard, uint32_t cap);
void dyn_kdf_end(JSContext* ctx, int memhard);

int dyn_crypto_secret(JSContext* ctx, JSValueConst v, const uint8_t** pdata,
    size_t* plen, const char** powned);

static inline int dyn_crypto_private(int* live, const uint8_t** p, size_t n,
    const char* owned, uint8_t** hold)
{
    *hold = NULL;
    *live = dyn_fiber_any();
    if (__builtin_expect(!*live, 1) || owned || !n || !dyn_fiber_can_offload_slow())
        return 0;
    *hold = (uint8_t*)malloc(n);
    if (!*hold)
        return -1;
    memcpy(*hold, *p, n);
    *p = *hold;
    return 0;
}

#ifdef CONFIG_OPENSSL
int dyn_pem_no_passphrase(char* buf, int size, int rwflag, void* userdata);
#endif


JSValue dyn_crypto_hex(JSContext* ctx, const uint8_t* data, size_t len);

JSValue dyn_crypto_hash(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_crypto_hash_hex(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_crypto_hmac(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_sha3(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

#define K3_MAX_RATE 168

typedef struct {
    uint64_t a[25];
    uint8_t buf[K3_MAX_RATE];
    size_t buflen;
    size_t rate;
    uint8_t pad;
} k3_stream_t;

void k3_stream_init(k3_stream_t* s, size_t rate, uint8_t pad);

void k3_stream_update(k3_stream_t* s, const uint8_t* p, size_t n);

void k3_stream_digest(const k3_stream_t* s0, uint8_t* out, size_t out_len);

JSValue dyn_hash_cdc(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);


JSValue dyn_blake(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_hash_xx(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_crypto_kdf(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

extern JSClassID dyn_hmac_class_id;

JSValue dyn_hmac_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_hmac_sign(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_hmac_update_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_hmac_digest(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_hmac_reset_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_hmac_verify(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_hmac_get_algorithm(JSContext* ctx, JSValueConst this_val);

JSValue dyn_hmac_get_size(JSContext* ctx, JSValueConst this_val);

extern const JSCFunctionListEntry dyn_hash_funcs[41];

#ifdef CONFIG_OPENSSL
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>

int dyn_rsa_too_small(const EVP_PKEY* k);

EVP_PKEY* jwt_pem_key(const char* pem, size_t pemlen, int want_private);

int jwt_der_to_raw(const uint8_t* der, size_t derlen, int coord,
    uint8_t* out);

int jwt_raw_to_der(const uint8_t* raw, int coord, uint8_t** der,
    int* derlen);
#endif

JSValue dyn_crypto_jwt_sign(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_crypto_jwt_verify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#ifdef CONFIG_OPENSSL
#include <openssl/evp.h>
#include <openssl/err.h>

extern JSClassID dyn_aesgcm_class_id;

extern JSClassID dyn_chapoly_class_id;

JSValue dyn_aead_ctor_common(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv, int chacha);

JSValue dyn_aead_op(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_aead_seal_random(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_aead_op_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_ed25519_generate(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_ed25519_pem_to_raw(JSContext* ctx, JSValueConst t,
    int argc, JSValueConst* argv);

JSValue dyn_ed25519_pem_from_raw(JSContext* ctx, JSValueConst t,
    int argc, JSValueConst* argv);

JSValue dyn_x25519_pem_to_raw(JSContext* ctx, JSValueConst t,
    int argc, JSValueConst* argv);

JSValue dyn_x25519_pem_from_raw(JSContext* ctx, JSValueConst t,
    int argc, JSValueConst* argv);

JSValue dyn_x25519_generate(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_ed25519_sign(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_ed25519_verify(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);

JSValue dyn_x25519_derive(JSContext* ctx, JSValueConst t, int argc,
    JSValueConst* argv);
#endif


JSValue dyn_bcrypt_hash_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_bcrypt_verify_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_argon2_hash_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_argon2_verify_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#ifdef CONFIG_OPENSSL
#include <openssl/rand.h>
#include <openssl/kdf.h>

const EVP_MD* dyn_standalone_md(const char* name);

int dyn_crypto_copy(JSContext* ctx, JSValueConst v, uint8_t** out, size_t* outlen);

int dyn_curve_nid(const char* curve, int* coord);

JSValue dyn_pk_export_pem(JSContext* ctx, EVP_PKEY* k);

JSValue dyn_rsa_generate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_rsa_sign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_rsa_verify(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_rsa_sign_pss(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_rsa_verify_pss(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_rsa_oaep_seal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_rsa_oaep_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_ecdsa_generate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_ecdsa_sign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_ecdsa_verify(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_ecdh_generate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_ecdh_derive(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_scrypt_js(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_x509_parse(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue dyn_x509_generate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
#endif

#endif

#endif
