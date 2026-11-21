#ifndef LIBC_URL_H
#define LIBC_URL_H

#include "dyna-nat.h"
#include "core/dyn-pct.h"
#include "core/dyn-sb.h"
#include "core/dyn-urlrec.h"
#include "core/dyn-idna.h"
#include "cutils.h"
#include "libunicode.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    dyn_url_t u;
    JSValue sp_wrapper;
} dyn_urlobj_t;

static inline dyn_urlobj_t* dyn_url_obj(dyn_url_t* u)
{
    return (dyn_urlobj_t*)u;
}

extern JSClassID dyn_url_class_id;

JSValue dyn_url_href(JSContext* ctx, const dyn_url_t* u);

static inline dyn_url_t* dyn_url_of(JSContext* ctx, JSValueConst v)
{
    return (dyn_url_t*)dyn_plain_get(ctx, v, dyn_url_class_id);
}

JSValue dyn_url_build(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv, int quiet);

JSValue js_url_parse_static(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { U_HREF,
    U_PROTOCOL,
    U_USERNAME,
    U_PASSWORD,
    U_HOST,
    U_HOSTNAME,
    U_PORT,
    U_PATHNAME,
    U_SEARCH,
    U_HASH,
    U_ORIGIN,
    U_SEARCHPARAMS };

JSValue dyn_url_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue dyn_url_set(JSContext* ctx, JSValueConst this_val,
    JSValueConst val, int magic);

JSValue dyn_enc_strict(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_url_constructor(JSContext* ctx);

void dyn_url_install(JSContext* ctx, JSValueConst global_obj);

#endif
