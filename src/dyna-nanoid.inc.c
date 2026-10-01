static const char DYN_NANOID_ALPHA[] = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLFGQZbfghjklqvwyzrict";

#define DYN_NANOID_MAX 4096

static int dyn_nanoid_fill(char* out, size_t size, const char* alpha,
    size_t n_alpha)
{
    size_t mask = 1, produced = 0;
    uint8_t buf[256];

    while (mask < n_alpha - 1)
        mask = (mask << 1) | 1;
    while (produced < size) {
        size_t want = size - produced, i;
        if (want > sizeof buf)
            want = sizeof buf;
        if (dyn_os_entropy(buf, want) < 0)
            return -1;
        for (i = 0; i < want && produced < size; i++) {
            size_t idx = (size_t)buf[i] & mask;
            if (idx < n_alpha)
                out[produced++] = alpha[idx];
        }
    }
    return 0;
}

static JSValue dyn_nanoid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char out[DYN_NANOID_MAX];
    int32_t size = 21;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        double dv;
        if (JS_ToFloat64(ctx, &dv, argv[0]))
            return JS_EXCEPTION;
        if (!(dv >= 1 && dv <= DYN_NANOID_MAX) || dv != floor(dv))
            return JS_ThrowRangeError(ctx,
                "NanoID(size): size must be in [1, %d]", DYN_NANOID_MAX);
        size = (int32_t)dv;
    }
    if (dyn_nanoid_fill(out, (size_t)size, DYN_NANOID_ALPHA,
            sizeof(DYN_NANOID_ALPHA) - 1)
        < 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");
    return JS_NewStringLen(ctx, out, (size_t)size);
}

static JSValue dyn_nanoid_alphabet(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char out[DYN_NANOID_MAX];
    const char* alpha;
    size_t n_alpha, i;
    int32_t size = 21;
    JSValue ret;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "NanoIDAlphabet(alphabet, size): alphabet must be a string");
    alpha = JS_ToCStringLen(ctx, &n_alpha, argv[0]);
    if (!alpha)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        double dv;
        if (JS_ToFloat64(ctx, &dv, argv[1])) {
            JS_FreeCString(ctx, alpha);
            return JS_EXCEPTION;
        }
        if (!(dv >= 1 && dv <= DYN_NANOID_MAX) || dv != floor(dv)) {
            JS_FreeCString(ctx, alpha);
            return JS_ThrowRangeError(ctx,
                "NanoIDAlphabet(alphabet, size): size must be in [1, %d]", DYN_NANOID_MAX);
        }
        size = (int32_t)dv;
    }
    if (n_alpha < 2 || n_alpha > 256) {
        JS_FreeCString(ctx, alpha);
        return JS_ThrowRangeError(ctx,
            "NanoIDAlphabet(alphabet, size): alphabet must hold 2..256 symbols");
    }
    for (i = 0; i < n_alpha; i++)
        if ((uint8_t)alpha[i] >= 0x80) {
            JS_FreeCString(ctx, alpha);
            return JS_ThrowTypeError(ctx,
                "NanoIDAlphabet(alphabet, size): alphabet must be ASCII");
        }
    if (size < 1 || size > DYN_NANOID_MAX) {
        JS_FreeCString(ctx, alpha);
        return JS_ThrowRangeError(ctx,
            "NanoIDAlphabet(alphabet, size): size must be in [1, %d]", DYN_NANOID_MAX);
    }
    if (dyn_nanoid_fill(out, (size_t)size, alpha, n_alpha) < 0) {
        JS_FreeCString(ctx, alpha);
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");
    }
    ret = JS_NewStringLen(ctx, out, (size_t)size);
    JS_FreeCString(ctx, alpha);
    return ret;
}

static const char DYN_ULID_ALPHA[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static pthread_mutex_t dyn_ulid_mono_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t dyn_ulid_mono_ms;
static uint64_t dyn_ulid_mono_hi;
static uint64_t dyn_ulid_mono_lo;

static JSValue dyn_ulid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t raw[16];
    char out[26];
    uint64_t ms;
    double dv;
    int i;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        int64_t v;
        if (JS_IsBigInt(ctx, argv[0])) {
            if (JS_ToBigInt64(ctx, &v, argv[0]))
                return JS_EXCEPTION;
            {
                JSValue back = JS_NewBigInt64(ctx, v);
                int same;
                if (JS_IsException(back))
                    return JS_EXCEPTION;
                same = JS_SameValue(ctx, back, argv[0]);
                JS_FreeValue(ctx, back);
                if (same < 0)
                    return JS_EXCEPTION;
                if (!same)
                    return JS_ThrowRangeError(ctx,
                        "ULID(atMillis): the timestamp must fit 48 bits");
            }
        } else {
            if (JS_ToFloat64(ctx, &dv, argv[0]))
                return JS_EXCEPTION;
            if (!(dv >= 0 && dv <= 0xFFFFFFFFFFFFull))
                return JS_ThrowRangeError(ctx,
                    "ULID(atMillis): the timestamp must fit 48 bits");
            if (dv != floor(dv))
                return JS_ThrowRangeError(ctx,
                    "ULID(atMillis): the timestamp must be an integer number of milliseconds");
            if (JS_ToInt64(ctx, &v, argv[0]))
                return JS_EXCEPTION;
        }
        if (v < 0 || (uint64_t)v > 0xFFFFFFFFFFFFull)
            return JS_ThrowRangeError(ctx,
                "ULID(atMillis): the timestamp must fit 48 bits");
        ms = (uint64_t)v;
    } else {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
            return JS_ThrowInternalError(ctx, "ULID(): clock_gettime failed");
        ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
        if (ms > 0xFFFFFFFFFFFFull)
            return JS_ThrowRangeError(ctx,
                "ULID(): the clock exceeds the 48-bit timestamp field");
    }
    for (i = 0; i < 6; i++)
        raw[i] = (uint8_t)(ms >> (40 - 8 * i));
    if (dyn_os_entropy(raw + 6, 10) < 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");

    {
        static const char* const allowed[] = { "monotonic" };
        int monotonic = 0;

        if (argc > 1 && !JS_IsUndefined(argv[1])) {
            JSValue mv;
            if (dyn_uuid_opts_check(ctx, argv[1], allowed, 1, "ULID(opts)"))
                return JS_EXCEPTION;
            if (!JS_IsObject(argv[1]))
                return JS_ThrowTypeError(ctx, "ULID(opts): opts must be an object");
            mv = JS_GetPropertyStr(ctx, argv[1], "monotonic");
            if (JS_IsException(mv))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(mv))
                monotonic = JS_ToBool(ctx, mv);
            JS_FreeValue(ctx, mv);
        }
        if (monotonic) {
            pthread_mutex_lock(&dyn_ulid_mono_lock);
            if (ms < dyn_ulid_mono_ms)
                ms = dyn_ulid_mono_ms;
            for (i = 0; i < 6; i++)
                raw[i] = (uint8_t)(ms >> (40 - 8 * i));
            if (ms == dyn_ulid_mono_ms) {
                uint64_t lo = dyn_ulid_mono_lo + 1;
                uint64_t hi = dyn_ulid_mono_hi;
                if (lo > 0xFFFF) {
                    lo = 0;
                    hi += 1;
                }
                dyn_ulid_mono_lo = lo;
                dyn_ulid_mono_hi = hi;
            } else {
                uint64_t hi = 0, lo = 0;
                int k;
                for (k = 0; k < 8; k++)
                    hi = (hi << 8) | raw[6 + k];
                lo = ((uint64_t)raw[14] << 8) | raw[15];
                dyn_ulid_mono_ms = ms;
                dyn_ulid_mono_hi = hi;
                dyn_ulid_mono_lo = lo;
            }
            for (i = 0; i < 8; i++)
                raw[6 + i] = (uint8_t)(dyn_ulid_mono_hi >> (56 - 8 * i));
            raw[14] = (uint8_t)(dyn_ulid_mono_lo >> 8);
            raw[15] = (uint8_t)dyn_ulid_mono_lo;
            pthread_mutex_unlock(&dyn_ulid_mono_lock);
        }
    }

    for (i = 0; i < 26; i++) {
        int bit = i * 5 - 2;
        uint32_t acc = 0;
        int b;
        for (b = 0; b < 5; b++) {
            int pos = bit + b;
            int v = (pos < 0) ? 0
                              : (raw[pos >> 3] >> (7 - (pos & 7))) & 1;
            acc = (acc << 1) | (uint32_t)v;
        }
        out[i] = DYN_ULID_ALPHA[acc];
    }
    return JS_NewStringLen(ctx, out, 26);
}

static JSValue dyn_ulid_time(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    uint64_t ms = 0;
    int i;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "ULIDTime(ulid): argument must be a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (n != 26) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "ULIDTime(ulid): a ULID is 26 characters");
    }
    for (i = 0; i < 26; i++) {
        char c = s[i];
        const char* p;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        p = strchr(DYN_ULID_ALPHA, c);
        if (!p || c == 0) {
            char bad = s[i];
            JS_FreeCString(ctx, s);
            return JS_ThrowTypeError(ctx,
                "ULIDTime(ulid): '%c' is not a Crockford base32 symbol", bad);
        }
        if (i < 10)
            ms = (ms << 5) | (uint64_t)(p - DYN_ULID_ALPHA);
    }
    JS_FreeCString(ctx, s);
    return JS_NewInt64(ctx, (int64_t)(ms & 0xFFFFFFFFFFFFull));
}
