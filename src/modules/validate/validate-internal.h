#ifndef VALIDATE_INTERNAL_H
#define VALIDATE_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VALIDATE)

#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-codec.h"
#include "core/dyn-prng.h"
#include "core/dyn-validk.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

enum { V_ALPHA,
    V_ALNUM,
    V_ASCII,
    V_EMAIL,
    V_LUHN,
    V_IBAN,
    V_IPV4,
    V_IPV6,
    V_IP,
    V_PORT,
    V_BASE64,
    V_BASE64URL,
    V_HEX,
    V_HEXCOLOR,
    V_NUMERIC,
    V_DATESTRING,
    V_RFC3339,
    V_MIME };

JSValue dyn_v_check(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_v_cache_find(JSContext* ctx, const char* module,
    const char* fnname);

void dyn_v_cache_store(JSContext* ctx, const char* module,
    const char* fnname, JSValue fn);

int dyn_v_jwt(JSContext* ctx, const char* s, size_t n, JSValueConst algs);

enum { VX_URL,
    VX_DOMAIN,
    VX_SLUG,
    VX_UUID,
    VX_JWT,
    VX_SEMVER,
    VX_E164,
    VX_JSON,
    VX_STRONGPW };

JSValue dyn_v_check_ext(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

#endif

#endif
