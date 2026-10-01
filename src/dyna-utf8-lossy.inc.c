

static JSValue dyn_utf8_lossy_string(JSContext* ctx, const uint8_t* p, size_t n)
{
    size_t i = 0, o, ascii = 0;
    uint8_t* out;
    JSValue ret;

    while (ascii + 8 <= n) {
        uint64_t word;
        memcpy(&word, p + ascii, 8);
        if (word & 0x8080808080808080ULL)
            goto multibyte_tail;
        ascii += 8;
    }
    while (ascii < n && p[ascii] < 0x80)
        ascii++;
    if (ascii == n)
        return JS_NewStringLen(ctx, (const char*)p, n);
multibyte_tail:
#ifdef DYN_UTF8_LOSSY_HAS_SIMD
    if (simd.validate_utf8(p, n) == n)
        return JS_NewStringLen(ctx, (const char*)p, n);
#endif

    out = (uint8_t*)js_malloc(ctx, n * 3 + 1);
    if (!out)
        return JS_EXCEPTION;
    o = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t len, lo = 0x80, hi = 0xBF, j, bad;

        if (c < 0x80) {
            out[o++] = c;
            i++;
            continue;
        }
        if (c < 0xC2)
            bad = 1;
        else if (c < 0xE0) {
            len = 2;
            bad = 0;
        } else if (c < 0xF0) {
            len = 3;
            lo = (c == 0xE0) ? 0xA0 : 0x80;
            hi = (c == 0xED) ? 0x9F : 0xBF;
            bad = 0;
        } else if (c < 0xF5) {
            len = 4;
            lo = (c == 0xF0) ? 0x90 : 0x80;
            hi = (c == 0xF4) ? 0x8F : 0xBF;
            bad = 0;
        } else
            bad = 1;
        if (!bad) {
            for (j = 1; j < len; j++) {
                uint8_t jlo = (j == 1) ? lo : 0x80, jhi = (j == 1) ? hi : 0xBF;
                if (i + j >= n || p[i + j] < jlo || p[i + j] > jhi) {
                    bad = j;
                    break;
                }
            }
        }
        if (!bad) {
            memcpy(out + o, p + i, len);
            o += len;
            i += len;
        } else {
            out[o++] = 0xEF;
            out[o++] = 0xBF;
            out[o++] = 0xBD;
            i += bad;
        }
    }
    ret = JS_NewStringLen(ctx, (const char*)out, o);
    js_free(ctx, out);
    return ret;
}
