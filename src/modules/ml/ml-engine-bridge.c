#define DYN_ML_RAW_ENGINE
#include "ml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ML)

DynResource* dyn_ml_res_get(JSContext* ctx, JSValueConst v,
    JSClassID class_id)
{
    DynResource* r;
    dyn_ml_job_t* job;
    int i, excl;

    dyn_fiber_onjs();
    r = dyn_res_get(ctx, v, class_id);
    if (!r)
        return NULL;
    job = dyn_ml_job();
    if (!job) {
        if (r->async_users < 0) {
            JS_ThrowTypeError(ctx, "dyna:ml this object is being fitted by a call that has not settled; await it first");
            return NULL;
        }
        return r;
    }
    for (i = 0; i < job->n_holds; i++)
        if (job->holds[i].r == r)
            return r;
    excl = job->excl_this && JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT
        && JS_VALUE_GET_TAG(job->this_val) == JS_TAG_OBJECT
        && JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(job->this_val);
    if (excl ? r->async_users != 0 : r->async_users < 0) {
        JS_ThrowTypeError(ctx, excl ? "dyna:ml fit() on an object that another call is still using; await that call first" : "dyna:ml this object is being fitted by a call that has not settled; await it first");
        return NULL;
    }
    if (job->n_holds == job->cap_holds) {
        int cap = job->cap_holds ? job->cap_holds * 2 : 4;
        dyn_ml_hold_t* n = (dyn_ml_hold_t*)realloc(job->holds,
            (size_t)cap * sizeof(*n));
        if (!n) {
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        job->holds = n;
        job->cap_holds = cap;
    }
    job->holds[job->n_holds].r = r;
    job->holds[job->n_holds].excl = excl;
    job->holds[job->n_holds].keep = JS_DupValue(ctx, v);
    job->n_holds++;
    if (excl)
        r->async_users = -1;
    else
        r->async_users++;
    return r;
}

int dyn_ml_interrupt(JSContext* ctx)
{
    int st = dyn_fiber_cancel_state();
    if (st == 0)
        return 0;
    dyn_fiber_onjs();
    if (st > 0) {
        JS_ThrowInternalError(ctx, "interrupted");
        return 1;
    }
    return JS_CheckInterrupt(ctx);
}

#endif
