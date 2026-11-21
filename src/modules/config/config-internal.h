#ifndef CONFIG_INTERNAL_H
#define CONFIG_INTERNAL_H

#include "dyna-nat.h"
#include "dtoa.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CONFIG)

#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "cutils.h"
#include "core/dyn-sb.h"
#include "core/dyn-cfgscan.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

int dyn_def(JSContext* ctx, JSValueConst obj, const char* k, size_t klen,
    JSValue v);

JSValue dyn_tml_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static inline size_t dyn_cfg_dtoa(char* buf, double d)
{
    JSDTOATempMem tmp;
    int n = js_dtoa(buf, d, 10, 0, JS_DTOA_FORMAT_FREE, &tmp);
    buf[n] = 0;
    return (size_t)n;
}

JSValue dyn_tml_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dyn_ini_ns[2];

extern const JSCFunctionListEntry dyn_env_ns[3];

extern const JSCFunctionListEntry dyn_fm_ns[1];

#endif

#endif
