static _Atomic uint32_t tz_memo_epoch;

static int getTimezoneOffset(int64_t time)
{
    time_t ti;
    int res;

    time /= 1000;
    if (sizeof(time_t) == 4) {
        if ((time_t)-1 < 0) {
            if (time < INT32_MIN) {
                time = INT32_MIN;
            } else if (time > INT32_MAX) {
                time = INT32_MAX;
            }
        } else {
            if (time < 0) {
                time = 0;
            } else if (time > UINT32_MAX) {
                time = UINT32_MAX;
            }
        }
    }
    ti = time;
    {
        static _Atomic uint64_t tz_memo[2];
        static _Atomic uint8_t tz_memo_mru;
        uint64_t key_hi = ((uint64_t)(ti + ((int64_t)1 << 44))) & ((((uint64_t)1) << 48) - 1);
        uint64_t v;
        int mru = atomic_load_explicit(&tz_memo_mru, memory_order_relaxed) & 1;
        int i, slot;

        for (i = 0; i < 2; i++) {
            slot = mru ^ i;
            v = atomic_load_explicit(&tz_memo[slot], memory_order_relaxed);
            if ((v >> 16) == key_hi) {
                if (slot != mru)
                    atomic_store_explicit(&tz_memo_mru, slot,
                        memory_order_relaxed);
                return (int16_t)(v & 0xffff);
            }
        }
        slot = mru ^ 1;
        {
#if defined(_WIN32)
            struct tm* tm;
            time_t gm_ti, loc_ti;

            tm = gmtime(&ti);
            if (!tm)
                return 0;
            gm_ti = mktime(tm);

            tm = localtime(&ti);
            if (!tm)
                return 0;
            loc_ti = mktime(tm);

            res = (gm_ti - loc_ti) / 60;
#else
            struct tm tm;
            localtime_r(&ti, &tm);
            res = -tm.tm_gmtoff / 60;
#endif
        }
        atomic_store_explicit(&tz_memo[slot],
            (key_hi << 16) | (uint16_t)(int16_t)res,
            memory_order_relaxed);
        atomic_store_explicit(&tz_memo_mru, slot, memory_order_relaxed);
        atomic_fetch_add_explicit(&tz_memo_epoch, 1, memory_order_relaxed);
        return res;
    }
}

static uint32_t js_date_tz_epoch(void)
{
    return atomic_load_explicit(&tz_memo_epoch, memory_order_relaxed);
}

#if 0
static JSValue js___date_getTimezoneOffset(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    double dd;

    if (JS_ToFloat64(ctx, &dd, argv[0]))
        return JS_EXCEPTION;
    if (isnan(dd))
        return __JS_NewFloat64(ctx, dd);
    else
        return JS_NewInt32(ctx, getTimezoneOffset((int64_t)dd));
}

static JSValue js_get_prototype_from_ctor(JSContext *ctx, JSValueConst ctor,
                                          JSValueConst def_proto)
{
    JSValue proto;
    proto = JS_GetProperty(ctx, ctor, JS_ATOM_prototype);
    if (JS_IsException(proto))
        return proto;
    if (!JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        proto = JS_DupValue(ctx, def_proto);
    }
    return proto;
}

static JSValue js___date_create(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue obj, proto;
    proto = js_get_prototype_from_ctor(ctx, argv[0], argv[1]);
    if (JS_IsException(proto))
        return proto;
    obj = JS_NewObjectProtoClass(ctx, proto, JS_CLASS_DATE);
    JS_FreeValue(ctx, proto);
    if (!JS_IsException(obj))
        JS_SetObjectData(ctx, obj, JS_DupValue(ctx, argv[2]));
    return obj;
}
#endif
