/* NanoID and ULID for dyna:uuid; included by dyna-uuid.c so both share the OS
   entropy helper. Neither is a UUID, but both answer the same question and a
   second module for two ID generators would be a second thing to keep correct. */

/* NanoID's default alphabet, verbatim: URL-safe, 64 symbols, so 21 characters
   carry 126 bits. Order is part of the spec's identity -- do not sort it. */
static const char DYN_NANOID_ALPHA[] =
    "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLFGQZbfghjklqvwyzrict";

#define DYN_NANOID_MAX 4096          /* a generated id is not a document */

/* Rejection sampling, the same way nanoid does it: take `bits` low bits of each
   random byte and discard values past the alphabet. Modulo would bias toward the
   first (256 % n) symbols, which is exactly what an ID must not do. */
static int dyn_nanoid_fill(char *out, size_t size, const char *alpha,
                           size_t n_alpha)
{
    size_t mask = 1, produced = 0;
    uint8_t buf[256];

    while (mask < n_alpha - 1)       /* smallest 2^k-1 >= n_alpha-1 */
        mask = (mask << 1) | 1;
    while (produced < size) {
        size_t want = size - produced, i;
        if (want > sizeof buf)
            want = sizeof buf;
        if (dyn_uuid_entropy(buf, want) < 0)
            return -1;               /* fail closed: no bytes, no ID */
        for (i = 0; i < want && produced < size; i++) {
            size_t idx = (size_t)buf[i] & mask;
            if (idx < n_alpha)
                out[produced++] = alpha[idx];
        }
    }
    return 0;
}

/* NanoID(size = 21) -> string over the default 64-symbol alphabet. */
static JSValue dyn_nanoid(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    char out[DYN_NANOID_MAX];
    int32_t size = 21;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt32(ctx, &size, argv[0]))
            return JS_EXCEPTION;
        if (size < 1 || size > DYN_NANOID_MAX)
            return JS_ThrowRangeError(ctx,
                "NanoID(size): size must be in [1, %d]", DYN_NANOID_MAX);
    }
    if (dyn_nanoid_fill(out, (size_t)size, DYN_NANOID_ALPHA,
                        sizeof(DYN_NANOID_ALPHA) - 1) < 0)
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");
    return JS_NewStringLen(ctx, out, (size_t)size);
}

/* NanoIDAlphabet(alphabet, size) -> string over a caller-supplied alphabet.
   The alphabet must be ASCII: the output is indexed by byte, and a multi-byte
   symbol would be cut in half. */
static JSValue dyn_nanoid_alphabet(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    char out[DYN_NANOID_MAX];
    const char *alpha;
    size_t n_alpha, i;
    int32_t size = 21;
    JSValue ret;

    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "NanoIDAlphabet(alphabet, size): alphabet must be a string");
    alpha = JS_ToCStringLen(ctx, &n_alpha, argv[0]);
    if (!alpha)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt32(ctx, &size, argv[1])) { JS_FreeCString(ctx, alpha); return JS_EXCEPTION; }
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

/* ------------------------------------------------------------------ ULID */

/* Crockford base32: no I, L, O or U, so a transcription cannot ambiguously
   decode. Sorted-by-value, which is what makes a ULID lexicographically
   ordered by its timestamp. */
static const char DYN_ULID_ALPHA[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* ULID(atMillis?) -> 26 Crockford base32 chars: 48-bit big-endian millisecond
   timestamp then 80 bits of entropy. */
static JSValue dyn_ulid(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    uint8_t raw[16];
    char out[26];
    uint64_t ms;
    int i;

    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        int64_t v;
        if (JS_ToInt64(ctx, &v, argv[0]))
            return JS_EXCEPTION;
        if (v < 0 || (uint64_t)v > 0xFFFFFFFFFFFFull)
            return JS_ThrowRangeError(ctx,
                "ULID(atMillis): the timestamp must fit 48 bits");
        ms = (uint64_t)v;
    } else {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
            return JS_ThrowInternalError(ctx, "ULID(): clock_gettime failed");
        ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
    }
    for (i = 0; i < 6; i++)                       /* 48-bit time, big-endian */
        raw[i] = (uint8_t)(ms >> (40 - 8 * i));
    if (dyn_uuid_entropy(raw + 6, 10) < 0)        /* 80 bits of randomness */
        return JS_ThrowInternalError(ctx, "dyna:uuid: OS entropy unavailable");

    /* 128 bits into 26 base32 characters: the first character carries only the
       top 2 bits (26 * 5 = 130), which is why a ULID never starts above '7'. */
    for (i = 0; i < 26; i++) {
        int bit = i * 5 - 2;                      /* -2 pads the leading 2 bits */
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

/* ULIDTime(ulid) -> the millisecond timestamp encoded in the first 10 chars. */
static JSValue dyn_ulid_time(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    const char *s;
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
    /* Validate ALL 26 symbols, not just the 10 that carry the time: a string
       with a bad character anywhere is not a ULID. */
    for (i = 0; i < 26; i++) {
        char c = s[i];
        const char *p;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');       /* not toupper: that reads the locale */
        p = strchr(DYN_ULID_ALPHA, c);
        if (!p || c == 0) {
            char bad = s[i];          /* read BEFORE the free, not after */
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
