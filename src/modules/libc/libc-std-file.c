#include "libc-internal.h"
#include "core/dyn-snappend.h"

#define DYN_PRINTF_FIELD_MAX 1000000

static inline BOOL my_isdigit(int c)
{
    return (c >= '0' && c <= '9');
}

JSValue js_printf_internal(JSContext* ctx,
    int argc, JSValueConst* argv, FILE* fp)
{
    char fmtbuf[32];
    if (fp)
        dyn_stdio_sync();
    uint8_t cbuf[UTF8_CHAR_LEN_MAX + 1];
    JSValue res;
    DynBuf dbuf;
    const char* fmt_str = NULL;
    const uint8_t *fmt, *fmt_end;
    const uint8_t* p;
    char* q;
    int i, c, len, mod;
    size_t fmt_len;
    int32_t int32_arg;
    int64_t int64_arg;
    double double_arg;
    const char* string_arg;
    int (*dbuf_printf_fun)(DynBuf* s, const char* fmt, ...) = (void*)dbuf_printf;

    js_std_dbuf_init(ctx, &dbuf);

    if (argc > 0) {
        fmt_str = JS_ToCStringLen(ctx, &fmt_len, argv[0]);
        if (!fmt_str)
            goto fail;

        i = 1;
        fmt = (const uint8_t*)fmt_str;
        fmt_end = fmt + fmt_len;
        while (fmt < fmt_end) {
            for (p = fmt; fmt < fmt_end && *fmt != '%'; fmt++)
                continue;
            dbuf_put(&dbuf, p, fmt - p);
            if (fmt >= fmt_end)
                break;
            q = fmtbuf;
            *q++ = *fmt++;

            for (;;) {
                c = *fmt;
                if (c == '0' || c == '#' || c == '+' || c == '-' || c == ' ' || c == '\'') {
                    if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                        goto invalid;
                    *q++ = c;
                    fmt++;
                } else {
                    break;
                }
            }
            if (*fmt == '*') {
                if (i >= argc)
                    goto missing;
                if (JS_ToInt32(ctx, &int32_arg, argv[i++]))
                    goto fail;
                if (int32_arg > DYN_PRINTF_FIELD_MAX || int32_arg < -DYN_PRINTF_FIELD_MAX)
                    goto field;
                q = fmtbuf + dyn_snappend(fmtbuf, sizeof(fmtbuf), (size_t)(q - fmtbuf), "%d", int32_arg);
                if (unlikely(q >= fmtbuf + sizeof(fmtbuf) - 1))
                    goto invalid;
                fmt++;
            } else {
                unsigned long long fw = 0;
                while (my_isdigit(*fmt)) {
                    if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                        goto invalid;
                    fw = fw * 10 + (unsigned)(*fmt - '0');
                    if (fw > DYN_PRINTF_FIELD_MAX)
                        goto field;
                    *q++ = *fmt++;
                }
            }
            if (*fmt == '.') {
                if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                    goto invalid;
                *q++ = *fmt++;
                if (*fmt == '*') {
                    if (i >= argc)
                        goto missing;
                    if (JS_ToInt32(ctx, &int32_arg, argv[i++]))
                        goto fail;
                    if (int32_arg > DYN_PRINTF_FIELD_MAX || int32_arg < -DYN_PRINTF_FIELD_MAX)
                        goto field;
                    q = fmtbuf + dyn_snappend(fmtbuf, sizeof(fmtbuf), (size_t)(q - fmtbuf), "%d", int32_arg);
                    if (unlikely(q >= fmtbuf + sizeof(fmtbuf) - 1))
                        goto invalid;
                    fmt++;
                } else {
                    unsigned long long prec = 0;
                    while (my_isdigit(*fmt)) {
                        if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                            goto invalid;
                        prec = prec * 10 + (unsigned)(*fmt - '0');
                        if (prec > DYN_PRINTF_FIELD_MAX)
                            goto field;
                        *q++ = *fmt++;
                    }
                }
            }

            mod = ' ';
            if (*fmt == 'l') {
                mod = *fmt++;
            }

            c = *fmt++;
            if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                goto invalid;
            *q++ = c;
            *q = '\0';

            switch (c) {
            case 'c':
                if (mod == 'l')
                    goto invalid;
                if (i >= argc)
                    goto missing;
                if (JS_IsString(argv[i])) {
                    string_arg = JS_ToCString(ctx, argv[i++]);
                    if (!string_arg)
                        goto fail;
                    int32_arg = unicode_from_utf8((const uint8_t*)string_arg, UTF8_CHAR_LEN_MAX, &p);
                    JS_FreeCString(ctx, string_arg);
                } else {
                    if (JS_ToInt32(ctx, &int32_arg, argv[i++]))
                        goto fail;
                }
                if ((unsigned)int32_arg > 0x10FFFF)
                    int32_arg = 0xFFFD;
                len = unicode_to_utf8(cbuf, int32_arg);
                if (q - fmtbuf > 2) {
                    cbuf[len] = '\0';
                    q[-1] = 's';
                    if (dbuf_printf_fun(&dbuf, fmtbuf, (char*)cbuf) < 0)
                        goto fail;
                } else {
                    dbuf_put(&dbuf, cbuf, len);
                }
                break;

            case 'd':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
            case 'X':
                if (i >= argc)
                    goto missing;
                if (JS_ToInt64Ext(ctx, &int64_arg, argv[i++]))
                    goto fail;
                if (mod == 'l') {
#if defined(_WIN32)
                    if (q >= fmtbuf + sizeof(fmtbuf) - 3)
                        goto invalid;
                    q[2] = q[-1];
                    q[-1] = 'I';
                    q[0] = '6';
                    q[1] = '4';
                    q[3] = '\0';
                    dbuf_printf_fun(&dbuf, fmtbuf, (int64_t)int64_arg);
#else
                    if (q >= fmtbuf + sizeof(fmtbuf) - 2)
                        goto invalid;
                    q[1] = q[-1];
                    q[-1] = q[0] = 'l';
                    q[2] = '\0';
                    dbuf_printf_fun(&dbuf, fmtbuf, (long long)int64_arg);
#endif
                } else {
                    dbuf_printf_fun(&dbuf, fmtbuf, (int)int64_arg);
                }
                break;

            case 's':
                if (mod == 'l')
                    goto invalid;
                if (i >= argc)
                    goto missing;
                string_arg = JS_ToCString(ctx, argv[i++]);
                if (!string_arg)
                    goto fail;
                dbuf_printf_fun(&dbuf, fmtbuf, string_arg);
                JS_FreeCString(ctx, string_arg);
                break;

            case 'e':
            case 'f':
            case 'g':
            case 'a':
            case 'E':
            case 'F':
            case 'G':
            case 'A':
                if (i >= argc)
                    goto missing;
                if (JS_ToFloat64(ctx, &double_arg, argv[i++]))
                    goto fail;
                dbuf_printf_fun(&dbuf, fmtbuf, double_arg);
                break;

            case '%':
                dbuf_putc(&dbuf, '%');
                break;

            default:
            invalid:
                JS_ThrowTypeError(ctx, "std: invalid conversion specifier in format string");
                goto fail;
            missing:
                JS_ThrowReferenceError(ctx, "std: missing argument for conversion specifier");
                goto fail;
            field:
                JS_ThrowRangeError(ctx, "std: width/precision exceeds 1000000");
                goto fail;
            }
        }
        JS_FreeCString(ctx, fmt_str);
    }
    if (dbuf.error) {
        res = JS_ThrowOutOfMemory(ctx);
    } else {
        if (fp) {
            len = fwrite(dbuf.buf, 1, dbuf.size, fp);
            res = JS_NewInt32(ctx, len);
        } else {
            res = JS_NewStringLen(ctx, (char*)dbuf.buf, dbuf.size);
        }
    }
    dbuf_free(&dbuf);
    return res;

fail:
    JS_FreeCString(ctx, fmt_str);
    dbuf_free(&dbuf);
    return JS_EXCEPTION;
}

void js_std_file_finalizer(JSRuntime* rt, JSValue val)
{
    JSSTDFile* s = JS_GetOpaque(val, js_std_file_class_id);
    if (s) {
        if (s->f && s->close_in_finalizer)
            fclose(s->f);
        js_free_rt(rt, s);
    }
}

JSValue js_std_printf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return js_printf_internal(ctx, argc, argv, stdout);
}

static FILE* js_std_file_get(JSContext* ctx, JSValueConst obj)
{
    JSSTDFile* s = JS_GetOpaque2(ctx, obj, js_std_file_class_id);
    if (!s)
        return NULL;
    if (!s->f) {
        JS_ThrowTypeError(ctx, "std.file: invalid file handle");
        return NULL;
    }
    if (s->f == stdout || s->f == stderr)
        dyn_stdio_sync();
    return s->f;
}

JSValue js_std_file_puts(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    FILE* f;
    int i;
    const char* str;
    size_t len;

    if (magic == 0) {
        dyn_stdio_sync();
        f = stdout;
    } else {
        f = js_std_file_get(ctx, this_val);
        if (!f)
            return JS_EXCEPTION;
    }

    for (i = 0; i < argc; i++) {
        str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        if (magic != 0 && !(f = js_std_file_get(ctx, this_val))) {
            JS_FreeCString(ctx, str);
            return JS_EXCEPTION;
        }
        fwrite(str, 1, len, f);
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

JSValue js_std_file_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSSTDFile* s = JS_GetOpaque2(ctx, this_val, js_std_file_class_id);
    int err;
    if (!s)
        return JS_EXCEPTION;
    if (!s->f)
        return JS_ThrowTypeError(ctx, "std.file: invalid file handle");
    err = js_get_errno(fclose(s->f));
    s->f = NULL;
    return JS_NewInt32(ctx, err);
}

JSValue js_std_file_printf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue text;
    const char* p;
    size_t n;
    FILE* f;

    if (!js_std_file_get(ctx, this_val))
        return JS_EXCEPTION;
    text = js_printf_internal(ctx, argc, argv, NULL);
    if (JS_IsException(text))
        return text;
    f = js_std_file_get(ctx, this_val);
    p = f ? JS_ToCStringLen(ctx, &n, text) : NULL;
    JS_FreeValue(ctx, text);
    if (!p)
        return JS_EXCEPTION;
    n = fwrite(p, 1, n, f);
    JS_FreeCString(ctx, p);
    return JS_NewInt32(ctx, (int32_t)n);
}

void js_print_value_write(void* opaque, const char* buf, size_t len)
{
    FILE* fo = opaque;
    fwrite(buf, 1, len, fo);
}

JSValue js_std_file_printObject(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_stdio_sync();
    JS_PrintValue(ctx, js_print_value_write, stdout, argv[0], NULL);
    return JS_UNDEFINED;
}

JSValue js_std_file_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    fflush(f);
    return JS_UNDEFINED;
}

JSValue js_std_file_tell(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int is_bigint)
{
    FILE* f = js_std_file_get(ctx, this_val);
    int64_t pos;
    if (!f)
        return JS_EXCEPTION;
#if defined(__linux__) || defined(__GLIBC__)
    pos = ftello(f);
#else
    pos = ftell(f);
#endif
    if (is_bigint)
        return JS_NewBigInt64(ctx, pos);
    else
        return JS_NewInt64(ctx, pos);
}

JSValue js_std_file_seek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f;
    int64_t pos;
    int whence, ret;
    if (JS_ToInt64Ext(ctx, &pos, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &whence, argv[1]))
        return JS_EXCEPTION;
    f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
#if defined(__linux__) || defined(__GLIBC__)
    ret = fseeko(f, pos, whence);
#else
    ret = fseek(f, pos, whence);
#endif
    if (ret < 0)
        ret = -errno;
    return JS_NewInt32(ctx, ret);
}

JSValue js_std_file_eof(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, feof(f));
}

JSValue js_std_file_error(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, ferror(f));
}

JSValue js_std_file_clearerr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    clearerr(f);
    return JS_UNDEFINED;
}

JSValue js_std_file_fileno(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, fileno(f));
}

JSValue js_std_file_read_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    FILE* f;
    uint64_t pos, len;
    size_t size, ret;
    uint8_t* buf;

    if (JS_ToIndex(ctx, &pos, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToIndex(ctx, &len, argv[2]))
        return JS_EXCEPTION;
    f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    if (!magic && JS_CheckWritableBuffer(ctx, argv[0]) < 0)
        return JS_EXCEPTION;
    buf = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!buf)
        return JS_EXCEPTION;
    if (pos + len > size)
        return JS_ThrowRangeError(ctx, "std.file: read/write array buffer overflow");
    if (magic)
        ret = fwrite(buf + pos, 1, len, f);
    else
        ret = fread(buf + pos, 1, len, f);
    return JS_NewInt64(ctx, ret);
}

JSValue js_std_file_write_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    JSValue abuf;
    uint8_t* base;
    size_t size, bpe, off, len;

    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "std.file: writeBytes requires a byte view");
    abuf = JS_GetArrayBufferView(ctx, argv[0], &off, &len, &bpe);
    if (JS_IsException(abuf) || bpe != 1) {
        if (!JS_IsException(abuf))
            JS_FreeValue(ctx, abuf);
        return JS_ThrowTypeError(ctx, "std.file: writeBytes requires a Uint8Array");
    }
    base = JS_GetArrayBuffer(ctx, &size, abuf);
    JS_FreeValue(ctx, abuf);
    if (!base)
        return JS_EXCEPTION;
    if (off > size || len > size - off)
        return JS_ThrowRangeError(ctx, "std.file: writeBytes array buffer overflow");
    return JS_NewInt64(ctx, fwrite(base + off, 1, len, f));
}

JSValue js_std_file_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f;
    uint64_t max64 = 0;
    uint8_t* buf;
    size_t got;
    JSValue ab, args[3], out;

    if (argc < 1 || JS_ToIndex(ctx, &max64, argv[0]))
        return JS_ThrowTypeError(ctx, "std.file: readBytes requires a length");
    f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    if (max64 > (uint64_t)1 << 31)
        return JS_ThrowRangeError(ctx, "std.file: readBytes max is out of range");
    buf = (uint8_t*)js_malloc(ctx, max64 ? (size_t)max64 : 1);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    got = fread(buf, 1, (size_t)max64, f);
    ab = JS_NewArrayBufferCopy(ctx, buf, got);
    js_free(ctx, buf);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

JSValue js_std_file_getline(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    JSValue obj;

    if (!f)
        return JS_EXCEPTION;

#if !defined(_WIN32)
    {
        char* line = NULL;
        size_t cap = 0;
        ssize_t n = getdelim(&line, &cap, '\n', f);
        if (n < 0) {
            free(line);
            return JS_NULL;
        }
        if (n > 0 && line[n - 1] == '\n')
            n--;
        obj = JS_NewStringLen(ctx, line, n);
        free(line);
        return obj;
    }
#else
    {
        int c;
        DynBuf dbuf;
        js_std_dbuf_init(ctx, &dbuf);
        for (;;) {
            c = fgetc(f);
            if (c == EOF) {
                if (dbuf.size == 0) {
                    dbuf_free(&dbuf);
                    return JS_NULL;
                } else {
                    break;
                }
            }
            if (c == '\n')
                break;
            if (dbuf_putc(&dbuf, c)) {
                dbuf_free(&dbuf);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        obj = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
        dbuf_free(&dbuf);
        return obj;
    }
#endif
}

JSValue js_std_file_readAsString(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f;
    DynBuf dbuf;
    JSValue obj;
    uint64_t max_size64;
    size_t max_size;
    JSValueConst max_size_val;

    if (argc >= 1)
        max_size_val = argv[0];
    else
        max_size_val = JS_UNDEFINED;
    max_size = (size_t)-1;
    if (!JS_IsUndefined(max_size_val)) {
        if (JS_ToIndex(ctx, &max_size64, max_size_val))
            return JS_EXCEPTION;
        if (max_size64 < max_size)
            max_size = max_size64;
    }
    f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;

    js_std_dbuf_init(ctx, &dbuf);

    {
        struct stat st;
        int fd = fileno(f);
        if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            int64_t pos = ftello(f);
            if (pos >= 0 && st.st_size > pos) {
                uint64_t want = (uint64_t)(st.st_size - pos);
                if (want > max_size)
                    want = max_size;
                if (want <= (UINT64_C(1) << 31))
                    dbuf_claim(&dbuf, (size_t)want);
            }
        }
    }

    while (max_size != 0) {
        char buf[65536] DYN_IO_SCRATCH;
        size_t want = max_size < sizeof(buf) ? max_size : sizeof(buf);
        size_t n = fread(buf, 1, want, f);
        if (n == 0)
            break;
        if (dbuf_put(&dbuf, (const uint8_t*)buf, n)) {
            dbuf_free(&dbuf);
            return JS_EXCEPTION;
        }
        max_size -= n;
    }
    obj = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
    dbuf_free(&dbuf);
    return obj;
}

JSValue js_std_file_getByte(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, fgetc(f));
}

JSValue js_std_file_putByte(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f;
    int c;
    if (JS_ToInt32(ctx, &c, argv[0]))
        return JS_EXCEPTION;
    f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    c = fputc(c, f);
    return JS_NewInt32(ctx, c);
}

JSValue js_print_internal(JSContext* ctx, int argc, JSValueConst* argv,
    FILE* fp)
{
    int i;
    JSValueConst v;

    dyn_stdio_sync();
    for (i = 0; i < argc; i++) {
        if (i != 0)
            fputc(' ', fp);
        v = argv[i];
        if (JS_IsString(v)) {
            const char* str;
            size_t len;
            str = JS_ToCStringLen(ctx, &len, v);
            if (!str)
                return JS_EXCEPTION;
            fwrite(str, 1, len, fp);
            JS_FreeCString(ctx, str);
        } else {
            JS_PrintValue(ctx, js_print_value_write, fp, v, NULL);
        }
    }
    fputc('\n', fp);
    return JS_UNDEFINED;
}

JSValue js_print(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return js_print_internal(ctx, argc, argv, stdout);
}
