/* test_bc_read_safety.c -- hostile-bytecode rejection (sweep findings F1/F2,
 * then the fuzz_bceval discovery run).
 *
 * Hand-built blobs that pre-fix crashed the shipped lib (SIGSEGV). Modes 1-5
 * are the M30-03 sweep set: closure vars whose var_idx/var_ref_idx index
 * outside their frames, and a module export claiming var_idx 0x10000. Modes
 * 6-9 are the fuzz_bceval classes: a module whose "function" is a forged
 * int32 (js_closure2 dereferenced it), an OP_fclosure8 whose cpool entry is
 * an int32 (js_closure dereferenced func_kind off it), a zero-length
 * byte_code_len body (SWITCH(pc) read off the function allocation), and a
 * module function claiming func_kind=NORMAL (async machinery completion
 * contract broken; async_func_resume read sf->cur_sp[-1]). Post-fix
 * every mode is rejected by the reader (modes 1-3 fail closed in the closure
 * machinery). The assertion is the EXIT CODE: pre-fix binaries exit 139;
 * post-fix exits 0 with per-mode "read rejected"/"eval threw" lines.
 *
 * Build: cc -I src -o test_bc_read_safety test_bc_read_safety.c libdynajs.a
 * Run:   ./test_bc_read_safety   (each mode prints its own lines)
 */
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
/* leb128 of a small non-negative value (all we need) */
static void putleb(uint32_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        put8(v ? (b | 0x80) : b);
    } while (v);
}
/* sleb128 for a signed value (var_idx etc.) */
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
    /* BC_VERSION is 13 since the OP_for_await_catch_body opcode-table edit
     * (parser.inc.c). This header used to write 12, which made the reader
     * reject every blob below at the version check and turned the whole
     * suite vacuous ("invalid version" != the checks these modes exist to
     * exercise); keep it in lockstep with BC_VERSION. */
    put8(13);        /* BC_VERSION */
    putleb(0);       /* idx_to_atom_count = 0 */
}

/* Minimal function: everything zero except closure_var_count.
 * `async_kind` builds the body/flags the way the compiler does for a module
 * function (JS_FUNC_ASYNC, body [OP_undefined, OP_return_async]) instead of
 * a plain NORMAL function with OP_return_undef: post-fuzz_bceval the reader
 * pins kind and terminator together, so module-function modes need the async
 * shape to reach the checks they exist to exercise. Mode 8 builds its empty
 * body inline (below).
 * OP_undefined=6, OP_return_undef=41, OP_return_async=47; func_kind
 * occupies flag bits 4-5, so JS_FUNC_ASYNC -> flags 0x20. */
static void function_tag(int closure_var_count, int cv_type, int32_t var_idx,
                         int async_kind)
{
    put8(12);        /* BC_TAG_FUNCTION_BYTECODE */
    put16(async_kind ? 0x20 : 0); /* flags: func_kind = ASYNC ? */
    put8(0);         /* js_mode */
    putleb(0);       /* func_name atom: idx 0 -> JS_ATOM_NULL */
    putleb(0);       /* arg_count */
    putleb(0);       /* var_count */
    putleb(0);       /* defined_arg_count */
    putleb(1);       /* stack_size (the return pushes/pops 1) */
    putleb(0);       /* var_ref_count */
    putleb((uint32_t)closure_var_count); /* closure_var_count */
    putleb(0);       /* cpool_count */
    putleb(async_kind ? 2 : 1);          /* byte_code_len */
    putleb(0);       /* local_count (0 == arg+var) */
    /* closure vars */
    for (int i = 0; i < closure_var_count; i++) {
        putleb(0);   /* var_name atom -> NULL */
        putsleb(var_idx);
        put16((uint16_t)cv_type); /* closure_type 3 bits, rest 0 */
    }
    if (async_kind) {
        put8(6);     /* OP_undefined */
        put8(47);    /* OP_return_async */
    } else {
        put8(41);    /* OP_return_undef */
    }
    /* no debug, no cpool */
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
        /* fuzz_bceval class B: byte_code_len=0. Pre-fix the reader accepted
         * it, JS_CallInternal fetched the first opcode at the tail of the
         * JSFunctionBytecode allocation -- heap-buffer-overflow READ.
         * Built inline: a NORMAL function with an EMPTY body. */
        put8(12);        /* BC_TAG_FUNCTION_BYTECODE */
        put16(0);        /* flags: func_kind NORMAL */
        put8(0);         /* js_mode */
        putleb(0);       /* func_name */
        putleb(0);       /* arg_count */
        putleb(0);       /* var_count */
        putleb(0);       /* defined_arg_count */
        putleb(0);       /* stack_size */
        putleb(0);       /* var_ref_count */
        putleb(0);       /* closure_var_count */
        putleb(0);       /* cpool_count */
        putleb(0);       /* byte_code_len == 0 -- the whole point */
        putleb(0);       /* local_count */
    }
    if (mode == 7) {
        /* fuzz_bceval class A2: OP_fclosure8 whose cpool entry is an int32.
         * js_closure() read func_kind_to_class_id[b->func_kind] with b =
         * JS_VALUE_GET_PTR(int) -- SEGV at ~0x11. Post-fix the read-time
         * validator demands a FUNCTION_BYTECODE tag for closure cpool
         * entries. */
        put8(12);        /* BC_TAG_FUNCTION_BYTECODE (F) */
        put16(0);        /* flags */
        put8(0);         /* js_mode */
        putleb(0);       /* func_name */
        putleb(0);       /* arg_count */
        putleb(0);       /* var_count */
        putleb(0);       /* defined_arg_count */
        putleb(1);       /* stack_size */
        putleb(0);       /* var_ref_count */
        putleb(0);       /* closure_var_count */
        putleb(1);       /* cpool_count */
        putleb(3);       /* byte_code_len */
        putleb(0);       /* local_count */
        /* bytecode: */
        put8(200);       /* OP_fclosure8 */
        put8(0);         /*   cpool index 0 */
        put8(41);        /* OP_return_undef (readable body) */
        /* cpool[0] = a forged int32, not a function: */
        put8(5);         /* BC_TAG_INT32 */
        putsleb(0);
    }
    if (mode == 6) {
        /* fuzz_bceval class A1: a module whose trailing "function" is an
         * int32. js_create_module_bytecode_function() did b =
         * JS_VALUE_GET_PTR(m->func_obj) and js_closure2() dereferenced
         * b->closure_var_count -- SEGV at 0x5c/0x5d. Post-fix the reader
         * requires the module function to deserialize as bytecode. */
        put8(13);        /* BC_TAG_MODULE */
        putleb(0);       /* module_name atom -> NULL */
        putleb(0);       /* req_module_entries_count */
        putleb(0);       /* export_entries_count */
        putleb(0);       /* star_export_entries_count */
        putleb(0);       /* import_entries_count */
        put8(0);         /* has_tla */
        put8(5);         /* BC_TAG_INT32 -- NOT a function bytecode */
        putsleb(0);
    }
    if (mode == 9) {
        /* fuzz_bceval sustained run #2: a module function claiming
         * func_kind = NORMAL. js_execute_sync_module() runs every module body
         * through the async machinery, whose completion contract only holds
         * for JS_FUNC_ASYNC bodies; a NORMAL body that reaches a sync return
         * completes via done: with sf->cur_sp left NULL and
         * async_func_resume() read sf->cur_sp[-1] -- SEGV at 0x...fff0. The
         * compiler always compiles module functions as JS_FUNC_ASYNC
         * (JS_EvalInternal), so the reader pins the kind. */
        put8(13);        /* BC_TAG_MODULE */
        putleb(0);       /* module_name atom -> NULL */
        putleb(0);       /* req_module_entries_count */
        putleb(0);       /* export_entries_count */
        putleb(0);       /* star_export_entries_count */
        putleb(0);       /* import_entries_count */
        put8(0);         /* has_tla */
        put8(12);        /* BC_TAG_FUNCTION_BYTECODE */
        put16(0);        /* flags: func_kind = NORMAL -- NOT what the writer emits */
        put8(0);         /* js_mode */
        putleb(0);       /* func_name */
        putleb(0);       /* arg_count */
        putleb(0);       /* var_count */
        putleb(0);       /* defined_arg_count */
        putleb(1);       /* stack_size */
        putleb(0);       /* var_ref_count */
        putleb(0);       /* closure_var_count */
        putleb(0);       /* cpool_count */
        putleb(1);       /* byte_code_len */
        putleb(0);       /* local_count */
        put8(41);        /* OP_return_undef */
    }
    if (mode == 5) {
        /* Heap/stack-OOB variant: executing parent F with var_ref_count=1 but a
         * vardef claiming var_ref_idx=100; child G (cpool[0]) closes over
         * F's local 0, so get_var_ref does sf->var_refs[100] (read) and, when
         * that reads NULL, sf->var_refs[100] = var_ref (write) -- both far
         * outside the 1-entry array at the end of F's alloca. */
        put8(12);        /* BC_TAG_FUNCTION_BYTECODE (F) */
        put16(0);        /* flags */
        put8(0);         /* js_mode */
        putleb(0);       /* func_name */
        putleb(0);       /* arg_count */
        putleb(1);       /* var_count */
        putleb(0);       /* defined_arg_count */
        putleb(2);       /* stack_size */
        putleb(1);       /* var_ref_count (array really has 1 slot) */
        putleb(0);       /* closure_var_count */
        putleb(1);       /* cpool_count */
        putleb(3);       /* byte_code_len */
        putleb(1);       /* local_count == arg+var */
        /* vardefs[0]: */
        putleb(0);       /* var_name atom -> NULL */
        putleb(1);       /* scope_next (stored +1) */
        putleb(100);     /* var_ref_idx -- LIES: array holds 1 entry */
        put8(0x40);      /* is_captured = 1 (bit 6) */
        /* bytecode: */
        put8(200);       /* OP_fclosure8 */
        put8(0);         /*   cpool index 0 */
        put8(41);        /* OP_return_undef */
        /* cpool[0] = child G: */
        put8(12);        /* BC_TAG_FUNCTION_BYTECODE */
        put16(0);
        put8(0);         /* js_mode */
        putleb(0);       /* name */
        putleb(0);       /* arg */
        putleb(0);       /* var */
        putleb(0);       /* defined_arg */
        putleb(1);       /* stack */
        putleb(0);       /* var_ref_count */
        putleb(1);       /* closure_var_count */
        putleb(0);       /* cpool_count */
        putleb(1);       /* bc_len */
        putleb(0);       /* local_count */
        /* G closure_var[0]: JS_CLOSURE_LOCAL(0), var_idx 0 */
        putleb(0);       /* var_name */
        putsleb(0);      /* var_idx -> F's var 0 */
        put16(0);        /* closure_type LOCAL */
        put8(41);        /* OP_return_undef */
    }
    if (mode == 4) {
        /* module: name atom, 0 reqs, 1 LOCAL export w/ huge var_idx, 0 star,
         * 0 imports, has_tla=0, then the module function */
        put8(13);        /* BC_TAG_MODULE */
        putleb(0);       /* module_name atom -> NULL */
        putleb(0);       /* req_module_entries_count */
        putleb(1);       /* export_entries_count */
        put8(0);         /* export_type = JS_EXPORT_TYPE_LOCAL */
        putsleb(0x10000);/* u.local.var_idx (unvalidated) */
        putleb(0);       /* export_name atom */
        putleb(0);       /* star_export_entries_count */
        putleb(0);       /* import_entries_count */
        put8(0);         /* has_tla */
        function_tag(0, 0, 0, 1); /* module function: ASYNC kind, as the
                                     compiler builds it; no closure vars */
    } else if (mode >= 1 && mode <= 3) {
        int type = mode == 1 ? 2 /* JS_CLOSURE_REF */ :
                  mode == 2 ? 0 /* JS_CLOSURE_LOCAL */ : 1 /* JS_CLOSURE_ARG */;
        function_tag(1, type, 0, 0); /* plain NORMAL eval function */
    }

    fprintf(stderr, "[%s] reading %zu bytes\n", name, n);
    obj = JS_ReadObject(ctx, buf, n, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj)) {
        fprintf(stderr, "[%s] read rejected (no crash)\n", name);
        { JSValue e = JS_GetException(ctx); print_str(ctx, e); JS_FreeValue(ctx, e); }
    } else {
        fprintf(stderr, "[%s] read ok; evaluating\n", name);
        fflush(stderr);
        val = JS_EvalFunction(ctx, obj);   /* the crash should be in here */
        fprintf(stderr, "[%s] eval returned (no crash)\n", name);
        if (JS_IsException(val))
            { JSValue e = JS_GetException(ctx); print_str(ctx, e); JS_FreeValue(ctx, e); }
        JS_FreeValue(ctx, val);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* ------------------------------------------------------------------ */
/* fuzz-lane finding F-1 (ws-bcfix): a hostile MODULE blob whose read
 * fails mid-structure leaked the JSModuleDef and every GC object only
 * it referenced. JS_ReadModule's fail path released the reference
 * js_new_module_def created but not the second one JS_NewModuleValue
 * had taken for 'obj', so the module survived JS_FreeContext's
 * js_free_modules() with ref_count==1 and JS_FreeRuntime aborted on
 * assert(list_empty(&rt->gc_obj_list)). The saved fuzz inputs below are
 * fed through the same flag sequence as src/fuzz/fuzz_bytecode.c; the
 * assertion is JS_FreeRuntime's, i.e. the exit code plus the per-blob
 * "rejected" lines. */

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
    /* Pre-fix this teardown asserted: the rejected module read stranded a
     * JSModuleDef on rt->gc_obj_list. */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    fprintf(stderr, "[%s] rejected, teardown clean\n", name);
}

/* 10 saved trigger inputs from the ws-fuzz campaign (one dedupe group:
 * every one aborted JS_FreeRuntime's gc_obj_list assert pre-fix). */
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

/* General statement of the F-1 class: a module blob that carries a
 * req-module entry whose attributes object graph was ALREADY read when a
 * later section fails must free the module AND that graph, at ANY of the
 * reader's mid-module rejection points. fail_where picks the section: */
enum {
    MW_FAIL_EXPORT_REQ_IDX,   /* export entry req_module_idx out of range */
    MW_FAIL_STAR_REQ_IDX,     /* star-export req_module_idx out of range */
    MW_FAIL_IMPORT_REQ_IDX,   /* import req_module_idx out of range */
    MW_FAIL_FUNC_NOT_BC,      /* module "function" is not function bytecode */
};

static void module_with_attrs(int fail_where)
{
    /* header() (version + empty atom table), then module shape:
     * name=NULL, 1 req entry (name NULL + attributes = nested object with
     * one property, so objects/shapes hang off the module), then the
     * chosen failing section, then the trailing function (only reached
     * when the failure is elsewhere). */
    header();
    put8(13);        /* BC_TAG_MODULE */
    putleb(0);       /* module_name atom -> NULL */
    putleb(1);       /* req_module_entries_count = 1 */
    putleb(0);       /*   req[0].module_name atom -> NULL */
    put8(8);         /*   BC_TAG_OBJECT (attributes) */
    putleb(1);       /*     one property */
    putleb(0);       /*     key atom -> NULL */
    put8(5);         /*     BC_TAG_INT32 */
    putsleb(7);      /*       value 7 */
    switch (fail_where) {
    case MW_FAIL_EXPORT_REQ_IDX:
        putleb(1);   /* export_entries_count = 1 */
        put8(1);     /* export_type = JS_EXPORT_TYPE_INDIRECT */
        putsleb(9);  /* u.req_module_idx = 9 >= 1 -> rejected */
        putleb(0);   /* local_name */
        putleb(0);   /* export_name */
        break;
    case MW_FAIL_STAR_REQ_IDX:
        putleb(0);   /* export_entries_count = 0 */
        putleb(1);   /* star_export_entries_count = 1 */
        putsleb(9);  /* req_module_idx = 9 >= 1 -> rejected */
        break;
    case MW_FAIL_IMPORT_REQ_IDX:
        putleb(0);   /* export_entries_count = 0 */
        putleb(0);   /* star_export_entries_count = 0 */
        putleb(1);   /* import_entries_count = 1 */
        putsleb(0);  /* var_idx */
        put8(0);     /* is_star = 0 */
        putleb(0);   /* import_name atom */
        putsleb(9);  /* req_module_idx = 9 >= 1 -> rejected */
        break;
    case MW_FAIL_FUNC_NOT_BC:
        putleb(0);   /* export_entries_count = 0 */
        putleb(0);   /* star_export_entries_count = 0 */
        putleb(0);   /* import_entries_count = 0 */
        put8(0);     /* has_tla */
        put8(5);     /* BC_TAG_INT32 instead of function bytecode */
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

/* Positive control: VALID module bytecode must still compile -> serialize ->
 * deserialize -> evaluate, and a valid script bytecode roundtrip must keep
 * working; both teardowns must stay clean. Guards the F-1 fix against
 * over-freeing on the success path. */
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

    /* module: compile-only -> write BYTECODE -> read BYTECODE -> eval */
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

    /* plain function bytecode roundtrip */
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

    /* valid-bytecode controls first: the F-1 fix must not have touched the
     * success path, and these blobs keep the suite honest about it */
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
