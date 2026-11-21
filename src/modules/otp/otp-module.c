#include "otp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_OTP)

static const JSCFunctionListEntry dyn_otp_funcs[] = {
    JS_CFUNC_DEF("HOTPGenerate", 2, dyn_otp_hotp),
    JS_CFUNC_DEF("TOTPGenerate", 1, dyn_otp_totp),
    JS_CFUNC_DEF("HOTPVerify", 3, dyn_otp_hotp_verify),
    JS_CFUNC_DEF("TOTPVerify", 2, dyn_otp_totp_verify),
    JS_CFUNC_DEF("HOTPMatch", 3, dyn_otp_hotp_match),
    JS_CFUNC_DEF("TOTPMatch", 2, dyn_otp_totp_match),
    JS_CFUNC_DEF("KeyURIParse", 1, dyn_otp_uri_parse),
    JS_CFUNC_DEF("KeyURIBuild", 1, dyn_otp_uri_build),
    JS_CFUNC_DEF("Enroll", 1, dyn_otp_enroll),
    JS_CFUNC_DEF("Throttle", 2, dyn_otp_throttle),
};

static int dyn_otp_init_module(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, dyn_otp_funcs, countof(dyn_otp_funcs));
}

int js_nat_init_otp(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:otp", dyn_otp_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_otp_funcs, countof(dyn_otp_funcs));
}

#endif
