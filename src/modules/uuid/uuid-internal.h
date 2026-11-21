#ifndef UUID_INTERNAL_H
#define UUID_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_UUID)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include "core/dyn-hash.h"
#include "core/dyn-prng.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_UUID_STRLEN 36

extern const char dyn_uuid_hex[17];

static inline void dyn_uuid_format(const uint8_t b[16], char out[DYN_UUID_STRLEN])
{
    int i, j = 0;
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[j++] = '-';
        out[j++] = dyn_uuid_hex[b[i] >> 4];
        out[j++] = dyn_uuid_hex[b[i] & 0x0f];
    }
}

int dyn_uuid_parse_bytes(const char* s, size_t len, uint8_t b[16]);

int dyn_uuid_view(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn);

JSValue dyn_uuid_new_u8array(JSContext* ctx, const uint8_t* data, size_t len);

int dyn_uuid_opts_check(JSContext* ctx, JSValueConst opts,
    const char* const* allowed, size_t n_allowed,
    const char* who);

JSValue dyn_uuid_v4(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_uuid_v7(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_uuid_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_uuid_v8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_uuid_named(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_uuid_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_uuid_variant(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_uuid_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_uuid_from_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_nanoid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_nanoid_alphabet(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_ulid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_ulid_time(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
