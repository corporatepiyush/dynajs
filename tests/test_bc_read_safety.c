#include "dynajs.h"
#include "dyna-libc.h"

#include <stdint.h>

static void print_str(JSContext *ctx, JSValue v) {
    const char *m = JS_ToCString(ctx, v);
    fprintf(stderr, "  exception: %s\n", m ? m : "(?)");
    if (m) JS_FreeCString(ctx, m);
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static uint8_t buf[512];
static size_t n;

static void put8(uint8_t v)  { buf[n++] = v; }
static void put16(uint16_t v){ put8(v & 0xff); put8(v >> 8); }
static void putleb(uint32_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        put8(v ? (b | 0x80) : b);
    } while (v);
}
static void putsleb(int32_t v)
{
    uint32_t z = (uint32_t)v;
    for (;;) {
        uint8_t b = z & 0x7f;
        z >>= 7;
        int done = ((z == 0 && !(b & 0x40)) || (z == 0xffffffff && (b & 0x40)));
        put8(done ? b : (b | 0x80));
        if (done) break;
    }
}

static void header(void)
{
    n = 0;
    put8(13);
    putleb(0);
}

static void function_tag(int closure_var_count, int cv_type, int32_t var_idx,
                         int async_kind)
{
    put8(12);
    put16(async_kind ? 0x20 : 0);
    put8(0);
    putleb(0);
    putleb(0);
    putleb(0);
    putleb(0);
    putleb(1);
    putleb(0);
    putleb((uint32_t)closure_var_count);
    putleb(0);
    putleb(async_kind ? 2 : 1);
    putleb(0);
    for (int i = 0; i < closure_var_count; i++) {
        putleb(0);
        putsleb(var_idx);
        put16((uint16_t)cv_type);
    }
    if (async_kind) {
        put8(6);
        put8(47);
    } else {
        put8(41);
    }
}

static JSContext *mkctx(JSRuntime *rt)
{
    JSContext *ctx = JS_NewContextRaw(rt);
    if (ctx) {
        JS_AddIntrinsicBaseObjects(ctx);
        JS_AddIntrinsicWeakRef(ctx);
    }
    return ctx;
}

static void run(const char *name, int mode)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = mkctx(rt);
    JSValue obj, val;

    header();
    if (mode == 8) {
        put8(12);
        put16(0);
        put8(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
    }
    if (mode == 7) {
        put8(12);
        put16(0);
        put8(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(1);
        putleb(0);
        putleb(0);
        putleb(1);
        putleb(3);
        putleb(0);
        put8(200);
        put8(0);
        put8(41);
        put8(5);
        putsleb(0);
    }
    if (mode == 6) {
        put8(13);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        put8(0);
        put8(5);
        putsleb(0);
    }
    if (mode == 9) {
        put8(13);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        put8(0);
        put8(12);
        put16(0);
        put8(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(1);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(1);
        putleb(0);
        put8(41);
    }
    if (mode == 5) {
        put8(12);
        put16(0);
        put8(0);
        putleb(0);
        putleb(0);
        putleb(1);
        putleb(0);
        putleb(2);
        putleb(1);
        putleb(0);
        putleb(1);
        putleb(3);
        putleb(1);
        putleb(0);
        putleb(1);
        putleb(100);
        put8(0x40);
        put8(200);
        put8(0);
        put8(41);
        put8(12);
        put16(0);
        put8(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(0);
        putleb(1);
        putleb(0);
        putleb(1);
        putleb(0);
        putleb(1);
        putleb(0);
        putleb(0);
        putsleb(0);
        put16(0);
        put8(41);
    }
    if (mode == 4) {
        put8(13);
        putleb(0);
        putleb(0);
        putleb(1);
        put8(0);
        putsleb(0x10000);
        putleb(0);
        putleb(0);
        putleb(0);
        put8(0);
        function_tag(0, 0, 0, 1);
    } else if (mode >= 1 && mode <= 3) {
        int type = mode == 1 ? 2  :
                  mode == 2 ? 0  : 1  ;
        function_tag(1, type, 0, 0);
    }

    fprintf(stderr, "[%s] reading %zu bytes\n", name, n);
    obj = JS_ReadObject(ctx, buf, n, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) {
        fprintf(stderr, "[%s] read rejected (no crash)\n", name);
        { JSValue e = JS_GetException(ctx); print_str(ctx, e); JS_FreeValue(ctx, e); }
    } else {
        fprintf(stderr, "[%s] read ok; evaluating\n", name);
        fflush(stderr);
        val = JS_EvalFunction(ctx, obj);
        fprintf(stderr, "[%s] eval returned (no crash)\n", name);
        if (JS_IsException(val))
            { JSValue e = JS_GetException(ctx); print_str(ctx, e); JS_FreeValue(ctx, e); }
        JS_FreeValue(ctx, val);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void run_blob(const char *name, const uint8_t *blob, size_t len)
{
    static const int flag_sets[] = {
        JS_READ_OBJ_BYTECODE,
        JS_READ_OBJ_BYTECODE | JS_READ_OBJ_REFERENCE,
        JS_READ_OBJ_BYTECODE | JS_READ_OBJ_ROM_DATA,
        JS_READ_OBJ_SAB,
        0,
    };
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = mkctx(rt);
    unsigned rejected = 0;

    fprintf(stderr, "[%s] reading %zu bytes under %u flag sets\n",
            name, len, (unsigned)countof(flag_sets));
    for (unsigned i = 0; i < countof(flag_sets); i++) {
        JSValue obj = JS_ReadObject(ctx, blob, len, flag_sets[i]);
        if (JS_IsException(obj)) {
            rejected++;
            { JSValue e = JS_GetException(ctx); print_str(ctx, e); JS_FreeValue(ctx, e); }
        } else {
            fprintf(stderr, "[%s] FAIL: flag set %u accepted invalid bytecode\n",
                    name, flag_sets[i]);
            JS_FreeValue(ctx, obj);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            exit(1);
        }
    }
    if (rejected != countof(flag_sets)) {
        fprintf(stderr, "[%s] FAIL: expected %u refusals, got %u\n",
                name, (unsigned)countof(flag_sets), rejected);
        exit(1);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    fprintf(stderr, "[%s] rejected, teardown clean\n", name);
}

static const uint8_t blob_f1_bytecode_20[] = {
    0x0d,0x00,0x0d,0x00,0x01,0x00,0x08,0x01,0x00,0x08,
    0x00,0x02,0xff,0xb4,0x29,0xff,0xb4,0xb4,0xf6,0x3a,
};
static const uint8_t blob_f1_bceval_23[] = {
    0x0d,0x00,0x0d,0x04,0x0d,0x04,0x01,0x00,0x08,0x01,
    0x00,0x08,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x0d,
    0x00,0x00,0x04,
};
static const uint8_t blob_f1_bc2_0222_23[] = {
    0x0d,0x00,0x09,0x0d,0x08,0x35,0x00,0x13,0x01,0x03,
    0x0c,0x00,0x03,0x26,0xfc,0xff,0xff,0xff,0xff,0xff,
    0xde,0xff,0x03,
};
static const uint8_t blob_f1_bc2_0e61_33[] = {
    0x0d,0x00,0x09,0x0d,0x04,0x08,0x36,0x00,0x13,0x01,
    0x0b,0x0d,0x07,0x01,0x00,0x13,0x00,0x00,0xf3,0xff,
    0x2f,0x03,0x00,0x8d,0x43,0x43,0x43,0x43,0xe0,0x00,
    0x00,0x00,0x0b,
};
static const uint8_t blob_f1_bc2_2942_27[] = {
    0x0d,0x00,0x09,0x0d,0x04,0x08,0x03,0x0d,0x01,0x00,
    0x13,0x01,0x0d,0x03,0x0c,0x2b,0x04,0x00,0x00,0x00,
    0x00,0x00,0x00,0x03,0x08,0x01,0x5d,
};
static const uint8_t blob_f1_bc2_2b6a_32[] = {
    0x0d,0x00,0x09,0x0d,0x04,0x08,0x03,0x0d,0x01,0x00,
    0x13,0x01,0xff,0xfb,0x0c,0x2b,0x41,0x04,0x00,0x00,
    0x00,0x00,0x2b,0x04,0x00,0x00,0x00,0x08,0x00,0x34,
    0x01,0x0b,
};
static const uint8_t blob_f1_bc2_48f3_33[] = {
    0x0d,0x00,0x09,0x0d,0x04,0x08,0x03,0x0d,0x01,0x00,
    0x13,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x03,0x0c,0x2b,0x04,0x00,0x00,0x00,0x00,0x00,
    0x08,0x01,0x0b,
};
static const uint8_t blob_f1_bc2_7aaa_33[] = {
    0x0d,0x00,0x09,0x0d,0x04,0x08,0x03,0x0d,0x01,0x00,
    0x13,0x01,0x01,0x03,0x0c,0x2b,0x04,0x00,0x00,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,0x00,
    0x08,0x01,0x0b,
};
static const uint8_t blob_f1_bc2_a159_33[] = {
    0x0d,0x00,0x09,0x0d,0x04,0x08,0x03,0x0d,0x13,0x01,
    0x00,0x0b,0x00,0x03,0x0c,0x26,0x04,0xee,0xff,0xfc,
    0x1e,0xff,0x00,0x01,0x00,0x0b,0x00,0x00,0xff,0xff,
    0xfc,0x1e,0xfd,
};
static const uint8_t blob_f1_bc2_a1c2_27[] = {
    0x0d,0x00,0x09,0x0d,0x04,0x08,0x03,0x0d,0x01,0x00,
    0x13,0x01,0x00,0x1b,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x03,0x08,0x01,0x0b,
};

enum {
    MW_FAIL_EXPORT_REQ_IDX,
    MW_FAIL_STAR_REQ_IDX,
    MW_FAIL_IMPORT_REQ_IDX,
    MW_FAIL_FUNC_NOT_BC,
};

static void module_with_attrs(int fail_where)
{
    header();
    put8(13);
    putleb(0);
    putleb(1);
    putleb(0);
    put8(8);
    putleb(1);
    putleb(0);
    put8(5);
    putsleb(7);
    switch (fail_where) {
    case MW_FAIL_EXPORT_REQ_IDX:
        putleb(1);
        put8(1);
        putsleb(9);
        putleb(0);
        putleb(0);
        break;
    case MW_FAIL_STAR_REQ_IDX:
        putleb(0);
        putleb(1);
        putsleb(9);
        break;
    case MW_FAIL_IMPORT_REQ_IDX:
        putleb(0);
        putleb(0);
        putleb(1);
        putsleb(0);
        put8(0);
        putleb(0);
        putsleb(9);
        break;
    case MW_FAIL_FUNC_NOT_BC:
        putleb(0);
        putleb(0);
        putleb(0);
        put8(0);
        put8(5);
        putsleb(0);
        break;
    }
    {
        char name[64];
        static const char *const where[] = {
            "module_attrs_fail_export_req_idx",
            "module_attrs_fail_star_req_idx",
            "module_attrs_fail_import_req_idx",
            "module_attrs_fail_func_not_bc",
        };
        snprintf(name, sizeof(name), "f1-%s", where[fail_where]);
        run_blob(name, buf, n);
    }
}

static void roundtrip_controls(void)
{
    static const char mod_src[] =
        "export const conf = { a: 1, b: \"x\" };\n"
        "export function f(n) { return conf.a + n; }\n"
        "export default 42;\n";
    static const char fun_src[] = "(function (n) { return n * 3; })\n";
    JSRuntime *rt;
    JSContext *ctx;
    JSValue obj, res;
    uint8_t *bc;
    size_t bc_len;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

    obj = JS_Eval(ctx, mod_src, strlen(mod_src), "<f1-roundtrip-module>",
                  JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(obj))
        goto cfail;
    bc = JS_WriteObject(ctx, &bc_len, obj, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, obj);
    if (!bc)
        goto cfail;
    obj = JS_ReadObject(ctx, bc, bc_len, JS_READ_OBJ_BYTECODE);
    js_free(ctx, bc);
    if (JS_IsException(obj))
        goto cfail;
    res = JS_EvalFunction(ctx, obj);
    if (JS_IsException(res))
        goto cfail;
    JS_FreeValue(ctx, res);
    fprintf(stderr, "[f1-roundtrip-module] ok, teardown clean\n");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    obj = JS_Eval(ctx, fun_src, strlen(fun_src), "<f1-roundtrip-func>",
                  JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(obj))
        goto cfail;
    bc = JS_WriteObject(ctx, &bc_len, obj, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, obj);
    if (!bc)
        goto cfail;
    obj = JS_ReadObject(ctx, bc, bc_len, JS_READ_OBJ_BYTECODE);
    js_free(ctx, bc);
    if (JS_IsException(obj))
        goto cfail;
    res = JS_EvalFunction(ctx, obj);
    if (JS_IsException(res))
        goto cfail;
    JS_FreeValue(ctx, res);
    fprintf(stderr, "[f1-roundtrip-func] ok, teardown clean\n");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return;
cfail:
    { JSValue e = JS_GetException(ctx); print_str(ctx, e); JS_FreeValue(ctx, e); }
    fprintf(stderr, "[f1-roundtrip] FAIL: valid bytecode did not survive\n");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    exit(1);
}

int main(int argc, char **argv)
{
    static const struct { const char *name; int mode; } cases[] = {
        { "closure_ref",   1 },
        { "closure_local", 2 },
        { "closure_arg",   3 },
        { "module_varidx", 4 },
        { "oob_var_ref_idx", 5 },
        { "module_func_not_function", 6 },
        { "cpool_closure_not_function", 7 },
        { "empty_byte_code_len", 8 },
        { "module_func_kind", 9 },
    };
    int only = argc > 1 ? atoi(argv[1]) : 0;
    unsigned i;
    static const struct {
        const char *name;
        const uint8_t *blob;
        size_t len;
    } blobs[] = {
        { "f1-bytecode-20",  blob_f1_bytecode_20,  sizeof(blob_f1_bytecode_20) },
        { "f1-bceval-23",    blob_f1_bceval_23,    sizeof(blob_f1_bceval_23) },
        { "f1-bc2-0222-23",  blob_f1_bc2_0222_23,  sizeof(blob_f1_bc2_0222_23) },
        { "f1-bc2-0e61-33",  blob_f1_bc2_0e61_33,  sizeof(blob_f1_bc2_0e61_33) },
        { "f1-bc2-2942-27",  blob_f1_bc2_2942_27,  sizeof(blob_f1_bc2_2942_27) },
        { "f1-bc2-2b6a-32",  blob_f1_bc2_2b6a_32,  sizeof(blob_f1_bc2_2b6a_32) },
        { "f1-bc2-48f3-33",  blob_f1_bc2_48f3_33,  sizeof(blob_f1_bc2_48f3_33) },
        { "f1-bc2-7aaa-33",  blob_f1_bc2_7aaa_33,  sizeof(blob_f1_bc2_7aaa_33) },
        { "f1-bc2-a159-33",  blob_f1_bc2_a159_33,  sizeof(blob_f1_bc2_a159_33) },
        { "f1-bc2-a1c2-27",  blob_f1_bc2_a1c2_27,  sizeof(blob_f1_bc2_a1c2_27) },
    };

    roundtrip_controls();
    for (i = 0; i < countof(blobs); i++)
        run_blob(blobs[i].name, blobs[i].blob, blobs[i].len);
    for (i = 0; i < 4; i++)
        module_with_attrs((int)i);
    for (i = 0; i < countof(cases); i++)
        if (!only || only == cases[i].mode)
            run(cases[i].name, cases[i].mode);
    fprintf(stderr, "all done\n");
    return 0;
}
