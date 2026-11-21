#ifndef OTP_INTERNAL_H
#define OTP_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OTP)

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/dyn-hash.h"
#include "core/dyn-prng.h"
#include "core/dyn-cryptok.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_OTP_MAX_WINDOW 4096
#define DYN_OTP_MAX_SAFE 9007199254740991.0

int dyn_otp_secret(JSContext* ctx, JSValueConst v, const uint8_t** pdata,
    size_t* plen, const char** powned);

int dyn_otp_compute(const char* algo, const uint8_t* key, size_t keylen,
    uint64_t counter, int digits, char* out, size_t outsz);

int dyn_otp_wellformed(const char* code, size_t codelen, int digits);

int dyn_otp_int(JSContext* ctx, JSValueConst v, const char* fn, const char* name,
    double lo, double hi, int64_t* out);

int dyn_otp_opt_int(JSContext* ctx, JSValueConst o, const char* fn, const char* name,
    double lo, double hi, int64_t* out);

int dyn_otp_opt_algo(JSContext* ctx, JSValueConst o, const char* fn, const char** out);

size_t dyn_otp_b32_encode(const uint8_t* in, size_t n, char* out);

int dyn_otp_b32_decode(const char* s, size_t n, uint8_t* out, size_t* outn);

JSValue dyn_otp_u8(JSContext* ctx, const uint8_t* data, size_t len);

JSValue dyn_otp_hotp(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_otp_totp(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_otp_hotp_verify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_otp_totp_verify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_otp_hotp_match(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_otp_totp_match(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_otp_uri_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_otp_uri_build(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_otp_enroll(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_otp_throttle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
