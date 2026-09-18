/* Date timezone helpers (getTimezoneOffset and friends).
 *
 * Unity-build fragment: #included into src/dynajs.c, never compiled alone.
 * Split out of the former object_array_iterator.inc.c (byte-identical token
 * stream preserved). */
/* Date */

/* Refill counter for the (second -> offset) memo in getTimezoneOffset().
   File-scope so the per-Date field-breakdown cache in date.inc.c can
   validate its cached local breakdowns cheaply: unchanged epoch == no
   refill == the memo still maps our second to the offset we cached. */
static _Atomic uint32_t tz_memo_epoch;

/* OS dependent. d = argv[0] is in ms from 1970. Return the difference
   between UTC time and local time 'd' in minutes */
static int getTimezoneOffset(int64_t time)
{
    time_t ti;
    int res;

    time /= 1000; /* convert to seconds */
    if (sizeof(time_t) == 4) {
        /* on 32-bit systems, we need to clamp the time value to the
           range of `time_t`. This is better than truncating values to
           32 bits and hopefully provides the same result as 64-bit
           implementation of localtime_r.
         */
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
    /* Memoize the last two (second -> offset) pairs. localtime_r consults
       the process timezone state on every call (glibc takes a lock and walks
       tzdata), and the dominant Date usage patterns -- several getters
       against the same Date object, or formatting loops over nearby
       timestamps -- ask for the SAME second repeatedly.  A single memo
       entry thrashed on the second pattern the audit found hottest:
       get_date_fields() keys the memo with the UTC second of the time
       value while set_date_fields() (every local setter) keys it with the
       LOCAL-WALL second of the rebuilt timestamp -- two different, but
       each stable, keys alternating once per setter call.  Two slots with
       a pseudo-LRU bit keep both residents, so setters and getters stop
       evicting each other.

       Concurrency: runtimes may live on different threads, so each pair is
       packed into a single atomic 64-bit word (48-bit biased second,
       16-bit signed offset in minutes; real offsets are within +-14h and
       tzdata transitions land on whole seconds, so second granularity is
       exact).  Relaxed torn-free loads/stores are all that is needed: any
       thread either sees a complete pair or misses and recomputes.

       Staleness: a process that changes TZ via setenv+tzset can be served
       the pre-change offset FOR THE SAME SECOND it had already resolved;
       the window is one wall-clock second of keys and matches the caching
       other engines do.

       tz_memo_epoch counts memo refills.  It lets the per-Date field
       breakdown cache (date.inc.c) serve a cached local breakdown without
       re-resolving the offset, while still noticing that the timezone
       state moved under it: if the epoch is unchanged no refill happened,
       so the memo still maps our second to our offset; if it changed, the
       cache revalidates the offset for its own second (below) before
       trusting the cached fields. */
    {
        static _Atomic uint64_t tz_memo[2]; /* 0 = empty */
        static _Atomic uint8_t tz_memo_mru; /* most recently used slot */
        uint64_t key_hi = ((uint64_t)(ti + ((int64_t)1 << 44))) & ((((uint64_t)1) << 48) - 1);
        uint64_t v;
        int mru = atomic_load_explicit(&tz_memo_mru, memory_order_relaxed) & 1;
        int i, slot;

        for (i = 0; i < 2; i++) {
            slot = mru ^ i; /* MRU slot first */
            v = atomic_load_explicit(&tz_memo[slot], memory_order_relaxed);
            if ((v >> 16) == key_hi) {
                if (slot != mru)
                    atomic_store_explicit(&tz_memo_mru, slot,
                                          memory_order_relaxed);
                return (int16_t)(v & 0xffff);
            }
        }
        slot = mru ^ 1; /* victim = least recently used slot */
        /* miss: compute, then publish into the least recently used slot */
        {
#if defined(_WIN32)
            struct tm *tm;
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

/* Current tz-memo refill epoch (see date.inc.c: per-Date field cache
   validation).  Relaxed like the memo itself. */
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

/* create a new date object */
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

