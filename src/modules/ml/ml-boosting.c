#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

JSValue dyn_xgb_best(JSContext* ctx, JSValueConst this_val,
    JSClassID class_id)
{
    dyn_forest_t* m = (dyn_forest_t*)dyn_res_native(ctx, this_val, class_id);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)(m->fitted ? m->best_rounds : 0));
}

#endif
