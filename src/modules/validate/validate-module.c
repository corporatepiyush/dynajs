#include "validate-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VALIDATE)

static const JSCFunctionListEntry dyn_v_funcs_ext[] = {
    JS_CFUNC_MAGIC_DEF("IsURL", 1, dyn_v_check_ext, VX_URL),
    JS_CFUNC_MAGIC_DEF("IsDomain", 1, dyn_v_check_ext, VX_DOMAIN),
    JS_CFUNC_MAGIC_DEF("IsSlug", 1, dyn_v_check_ext, VX_SLUG),
    JS_CFUNC_MAGIC_DEF("IsUUID", 1, dyn_v_check_ext, VX_UUID),
    JS_CFUNC_MAGIC_DEF("IsJWT", 1, dyn_v_check_ext, VX_JWT),
    JS_CFUNC_MAGIC_DEF("IsSemver", 1, dyn_v_check_ext, VX_SEMVER),
    JS_CFUNC_MAGIC_DEF("IsE164", 1, dyn_v_check_ext, VX_E164),
    JS_CFUNC_MAGIC_DEF("IsJSON", 1, dyn_v_check_ext, VX_JSON),
    JS_CFUNC_MAGIC_DEF("IsStrongPassword", 1, dyn_v_check_ext, VX_STRONGPW),
};

static const JSCFunctionListEntry dyn_v_funcs[] = {
    JS_CFUNC_MAGIC_DEF("IsAlpha", 1, dyn_v_check, V_ALPHA),
    JS_CFUNC_MAGIC_DEF("IsAlphanumeric", 1, dyn_v_check, V_ALNUM),
    JS_CFUNC_MAGIC_DEF("IsAscii", 1, dyn_v_check, V_ASCII),
    JS_CFUNC_MAGIC_DEF("IsEmail", 1, dyn_v_check, V_EMAIL),
    JS_CFUNC_MAGIC_DEF("IsCreditCard", 1, dyn_v_check, V_LUHN),
    JS_CFUNC_MAGIC_DEF("IsIBAN", 1, dyn_v_check, V_IBAN),
    JS_CFUNC_MAGIC_DEF("IsIPv4", 1, dyn_v_check, V_IPV4),
    JS_CFUNC_MAGIC_DEF("IsIPv6", 1, dyn_v_check, V_IPV6),
    JS_CFUNC_MAGIC_DEF("IsIP", 1, dyn_v_check, V_IP),
    JS_CFUNC_MAGIC_DEF("IsPort", 1, dyn_v_check, V_PORT),
    JS_CFUNC_MAGIC_DEF("IsBase64", 1, dyn_v_check, V_BASE64),
    JS_CFUNC_MAGIC_DEF("IsBase64Url", 1, dyn_v_check, V_BASE64URL),
    JS_CFUNC_MAGIC_DEF("IsHex", 1, dyn_v_check, V_HEX),
    JS_CFUNC_MAGIC_DEF("IsHexColor", 1, dyn_v_check, V_HEXCOLOR),
    JS_CFUNC_MAGIC_DEF("IsNumeric", 1, dyn_v_check, V_NUMERIC),
    JS_CFUNC_MAGIC_DEF("IsDateString", 1, dyn_v_check, V_DATESTRING),
    JS_CFUNC_MAGIC_DEF("IsRFC3339", 1, dyn_v_check, V_RFC3339),
    JS_CFUNC_MAGIC_DEF("IsMimeType", 1, dyn_v_check, V_MIME),
};

static int dyn_v_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (JS_SetModuleExportList(ctx, m, dyn_v_funcs, countof(dyn_v_funcs)) < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_v_funcs_ext,
        countof(dyn_v_funcs_ext));
}

int js_nat_init_validate(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:validate", dyn_v_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_v_funcs, countof(dyn_v_funcs)) < 0)
        return -1;
    return JS_AddModuleExportList(ctx, m, dyn_v_funcs_ext,
        countof(dyn_v_funcs_ext));
}

#endif
