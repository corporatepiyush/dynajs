#ifndef CONFIG_TAILCALL_INTERP
#define CONFIG_TAILCALL_INTERP 0
#endif

#if !CONFIG_TAILCALL_INTERP
#define JS_TC_REDIRECTS
#define JS_TC_ENTER_BLOCK
#define JS_TC_FAST_ENTRY(x) (x)

int js_call_prep_begin(JSContext* ctx, JSCallPrep* cp, JSValueConst func, JSValueConst this_obj, int argc)
{
    (void)ctx;
    cp->fr = NULL;
    cp->func = func;
    cp->this_obj = this_obj;
    cp->argc = argc;
    cp->nargs = argc;
    return 0;
}

JSValue js_call_prep(JSContext* ctx, JSCallPrep* cp, JSValue* argv)
{
    return js_call_owned(ctx, cp->func, cp->this_obj, cp->argc, argv);
}

JSValue js_call_prep_dup(JSContext* ctx, JSCallPrep* cp, JSValueConst* argv)
{
    return JS_Call(ctx, cp->func, cp->this_obj, cp->argc, argv);
}

void js_call_prep_end(JSContext* ctx, JSCallPrep* cp)
{
    (void)ctx;
    cp->fr = NULL;
}
#else

static JSValue js_typeof_string(JSContext* ctx, JSAtom atom);
static int js_using_add(JSContext* ctx, JSValueConst recs_val, JSValueConst value, int hint);
static JSValue js_using_dispose_value(JSContext* ctx, JSValueConst recs_val, JSValue pending, int flags);
static JSValue js_using_new_capability(JSContext* ctx);
static JSValue js_resolve_ctor_proto(JSContext* ctx, JSValueConst ctor, int class_id);

typedef struct JSTCState {
    JSStackFrame* sf;
    const uint8_t* pc;
    JSValue* sp;
    JSValue rel;
    JSValue rel2;
    JSValue ret;
    JSFunctionBytecode* b;
    JSValue* cpool;
    JSVarRef** var_refs;
    JSValue this_obj;
    JSContext* caller_ctx;
    JSValue new_target;
    JSValue* argv;
    JSValue* local_buf;
    JSValue* stack_buf;
    JSObject* p;
    size_t alloca_capacity;
    int argc;
    int tc_argc;
    int tc_arg_alloc;
    JSValue tc_func;
    JSValue tc_this;
    JSValue* tc_argv;
    struct JSTCState* up;
    JSValue own_func;
    JSValue own_this;
    JSValue own_nt;
    JSValue ctor_obj;
    int fr_flags;
#if CONFIG_LITERAL_BOILERPLATE
    JSObject* bb_rec_obj;
    JSObjectPresizeSite* bb_rec_site;
    uint32_t bb_rec_off;
    int bb_rec_nf;
#endif
} JSTCState;

#define TC_FR_NT 2
#define TC_FR_CTOR 4
#define TC_FR_ENTRY 8
#define TC_FR_KEEP 16

#if CONFIG_LITERAL_BOILERPLATE
#define TC_BB_REC_START(val, off_)             \
    do {                                       \
        S->bb_rec_obj = JS_VALUE_GET_OBJ(val); \
        S->bb_rec_off = (off_);                \
        S->bb_rec_site = NULL;                 \
        S->bb_rec_nf = 0;                      \
    } while (0)
#define TC_BB_REC_CLEAR()      \
    do {                       \
        S->bb_rec_obj = NULL;  \
        S->bb_rec_site = NULL; \
    } while (0)
#define TC_BB_REC_DROP(v)                                                                             \
    do {                                                                                              \
        if (S->bb_rec_obj && JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(v) == S->bb_rec_obj) \
            TC_BB_REC_CLEAR();                                                                        \
    } while (0)
#else
#define TC_BB_REC_DROP(v) ((void)0)
#endif

struct JSTCDisp;
#define TC_ARGS JSTCState *S, const uint8_t *pc, JSValue *sp, JSValue *lb, JSValue *ab, JSContext *ctx, const struct JSTCDisp *tbl
#define TC_PASS S, pc, sp, lb, ab, ctx, tbl
#define TC_HANDLER(name) static __attribute__((preserve_none, noinline)) int name(TC_ARGS)

typedef __attribute__((preserve_none)) int (*js_tc_handler)(TC_ARGS);

typedef struct JSTCDisp {
    js_tc_handler h;
} JSTCDisp;

static const JSTCDisp js_tc_table[256];

#define TC_DISPATCH() __attribute__((musttail)) return tbl[*pc].h(TC_PASS)
#define TC_JUMP(h) __attribute__((musttail)) return h(TC_PASS)
#define TC_THROW() TC_JUMP(js_tc_throw)
#define TC_NEXT(n)     \
    do {               \
        pc += (n);     \
        TC_DISPATCH(); \
    } while (0)

static __attribute__((preserve_none, noinline)) int js_tc_throw_inline(TC_ARGS);

TC_HANDLER(js_tc_throw)
{
    if (unlikely(S->up != NULL || (S->fr_flags & TC_FR_ENTRY)))
        TC_JUMP(js_tc_throw_inline);
    S->pc = pc;
    S->sp = sp;
    return 1;
}

TC_HANDLER(js_tc_poll)
{
    if (__js_poll_interrupts(ctx))
        TC_THROW();
    TC_DISPATCH();
}

#define TC_POLL_DISPATCH()                           \
    do {                                             \
        if (unlikely(--ctx->interrupt_counter <= 0)) \
            TC_JUMP(js_tc_poll);                     \
        TC_DISPATCH();                               \
    } while (0)

TC_HANDLER(js_tc_release)
{
    __JS_FreeValue(ctx, S->rel);
    TC_DISPATCH();
}

#define TC_FREE_DISPATCH(v)                                                                          \
    do {                                                                                             \
        if (JS_VALUE_HAS_REF_COUNT(v) && unlikely(--__js_rc(JS_VALUE_GET_PTR(v))->ref_count <= 0)) { \
            S->rel = (v);                                                                            \
            TC_JUMP(js_tc_release);                                                                  \
        }                                                                                            \
        TC_DISPATCH();                                                                               \
    } while (0)

static force_inline BOOL js_tc_num2(JSValue a, JSValue b, double* d1, double* d2)
{
    uint32_t t1 = JS_VALUE_GET_TAG(a), t2 = JS_VALUE_GET_TAG(b);
    if (JS_TAG_IS_FLOAT64(t1))
        *d1 = JS_VALUE_GET_FLOAT64(a);
    else if (t1 == JS_TAG_INT)
        *d1 = JS_VALUE_GET_INT(a);
    else
        return FALSE;
    if (JS_TAG_IS_FLOAT64(t2))
        *d2 = JS_VALUE_GET_FLOAT64(b);
    else if (t2 == JS_TAG_INT)
        *d2 = JS_VALUE_GET_INT(b);
    else
        return FALSE;
    return TRUE;
}

typedef unsigned __int128 __attribute__((may_alias, aligned(8))) js_tc_raw;
#define TC_RAW(i) (*(js_tc_raw*)&sp[i])

#define TC_PUSH(name, size, val) \
    TC_HANDLER(name)             \
    {                            \
        *sp++ = (val);           \
        TC_NEXT(size);           \
    }

TC_PUSH(js_tc_push_small, 1, JS_NewInt32(ctx, *pc - OP_push_0))
TC_PUSH(js_tc_push_i8, 2, JS_NewInt32(ctx, get_i8(pc + 1)))
TC_PUSH(js_tc_push_i16, 3, JS_NewInt32(ctx, get_i16(pc + 1)))
TC_PUSH(js_tc_push_i32, 5, JS_NewInt32(ctx, get_u32(pc + 1)))
TC_PUSH(js_tc_push_bigint_i32, 5, __JS_NewShortBigInt(ctx, (int)get_u32(pc + 1)))
TC_PUSH(js_tc_push_const8, 2, JS_DupValue(ctx, S->cpool[pc[1]]))
TC_PUSH(js_tc_push_const, 5, JS_DupValue(ctx, S->cpool[get_u32(pc + 1)]))
TC_PUSH(js_tc_push_atom_value, 5, js_atom_value_fast(ctx, get_u32(pc + 1)))
TC_PUSH(js_tc_push_empty_string, 1, JS_AtomToString(ctx, JS_ATOM_empty_string))
TC_PUSH(js_tc_undefined, 1, JS_UNDEFINED)
TC_PUSH(js_tc_null, 1, JS_NULL)
TC_PUSH(js_tc_push_false, 1, JS_FALSE)
TC_PUSH(js_tc_push_true, 1, JS_TRUE)

TC_HANDLER(js_tc_push_this_slow)
{
    JSValue val;
    uint32_t tag = JS_VALUE_GET_TAG(S->this_obj);
    pc += 1;
    if (tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED) {
        val = JS_DupValue(ctx, ctx->global_obj);
    } else {
        val = JS_ToObject(ctx, S->this_obj);
        if (JS_IsException(val))
            TC_THROW();
    }
    *sp++ = val;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_push_this)
{
    if (!(S->b->js_mode & JS_MODE_STRICT) && JS_VALUE_GET_TAG(S->this_obj) != JS_TAG_OBJECT)
        TC_JUMP(js_tc_push_this_slow);
    *sp++ = JS_DupValue(ctx, S->this_obj);
    TC_NEXT(1);
}

TC_HANDLER(js_tc_drop)
{
    JSValue v = *--sp;
    pc += 1;
    TC_FREE_DISPATCH(v);
}

TC_HANDLER(js_tc_nip)
{
    JSValue v = sp[-2];
    sp[-2] = sp[-1];
    sp--;
    pc += 1;
    TC_FREE_DISPATCH(v);
}

TC_HANDLER(js_tc_nip1)
{
    JSValue v = sp[-3];
    sp[-3] = sp[-2];
    sp[-2] = sp[-1];
    sp--;
    pc += 1;
    TC_FREE_DISPATCH(v);
}

TC_HANDLER(js_tc_dup)
{
    sp[0] = JS_DupValue(ctx, sp[-1]);
    sp++;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_dup1)
{
    sp[0] = sp[-1];
    sp[-1] = JS_DupValue(ctx, sp[-2]);
    sp++;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_dup2)
{
    sp[0] = JS_DupValue(ctx, sp[-2]);
    sp[1] = JS_DupValue(ctx, sp[-1]);
    sp += 2;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_dup3)
{
    sp[0] = JS_DupValue(ctx, sp[-3]);
    sp[1] = JS_DupValue(ctx, sp[-2]);
    sp[2] = JS_DupValue(ctx, sp[-1]);
    sp += 3;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_insert2)
{
    sp[0] = sp[-1];
    sp[-1] = sp[-2];
    sp[-2] = JS_DupValue(ctx, sp[0]);
    sp++;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_insert3)
{
    sp[0] = sp[-1];
    sp[-1] = sp[-2];
    sp[-2] = sp[-3];
    sp[-3] = JS_DupValue(ctx, sp[0]);
    sp++;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_insert4)
{
    sp[0] = sp[-1];
    sp[-1] = sp[-2];
    sp[-2] = sp[-3];
    sp[-3] = sp[-4];
    sp[-4] = JS_DupValue(ctx, sp[0]);
    sp++;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_perm3)
{
    js_tc_raw v2 = TC_RAW(-2), v3 = TC_RAW(-3);
    TC_RAW(-2) = v3;
    TC_RAW(-3) = v2;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_perm4)
{
    js_tc_raw v2 = TC_RAW(-2), v3 = TC_RAW(-3), v4 = TC_RAW(-4);
    TC_RAW(-2) = v3;
    TC_RAW(-3) = v4;
    TC_RAW(-4) = v2;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_perm5)
{
    js_tc_raw v2 = TC_RAW(-2), v3 = TC_RAW(-3), v4 = TC_RAW(-4), v5 = TC_RAW(-5);
    TC_RAW(-2) = v3;
    TC_RAW(-3) = v4;
    TC_RAW(-4) = v5;
    TC_RAW(-5) = v2;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_rot3l)
{
    js_tc_raw v1 = TC_RAW(-1), v2 = TC_RAW(-2), v3 = TC_RAW(-3);
    TC_RAW(-1) = v3;
    TC_RAW(-2) = v1;
    TC_RAW(-3) = v2;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_rot4l)
{
    js_tc_raw v1 = TC_RAW(-1), v2 = TC_RAW(-2), v3 = TC_RAW(-3), v4 = TC_RAW(-4);
    TC_RAW(-1) = v4;
    TC_RAW(-2) = v1;
    TC_RAW(-3) = v2;
    TC_RAW(-4) = v3;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_rot5l)
{
    js_tc_raw v1 = TC_RAW(-1), v2 = TC_RAW(-2), v3 = TC_RAW(-3), v4 = TC_RAW(-4), v5 = TC_RAW(-5);
    TC_RAW(-1) = v5;
    TC_RAW(-2) = v1;
    TC_RAW(-3) = v2;
    TC_RAW(-4) = v3;
    TC_RAW(-5) = v4;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_rot3r)
{
    js_tc_raw v1 = TC_RAW(-1), v2 = TC_RAW(-2), v3 = TC_RAW(-3);
    TC_RAW(-1) = v2;
    TC_RAW(-2) = v3;
    TC_RAW(-3) = v1;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_swap)
{
    js_tc_raw v1 = TC_RAW(-1), v2 = TC_RAW(-2);
    TC_RAW(-1) = v2;
    TC_RAW(-2) = v1;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_swap2)
{
    js_tc_raw v1 = TC_RAW(-1), v2 = TC_RAW(-2), v3 = TC_RAW(-3), v4 = TC_RAW(-4);
    TC_RAW(-1) = v3;
    TC_RAW(-2) = v4;
    TC_RAW(-3) = v1;
    TC_RAW(-4) = v2;
    TC_NEXT(1);
}

#define TC_GETV(name, size, slot)         \
    TC_HANDLER(name)                      \
    {                                     \
        *sp++ = JS_DupValue(ctx, (slot)); \
        TC_NEXT(size);                    \
    }
#define TC_PUTV(name, size, ref) \
    TC_HANDLER(name)             \
    {                            \
        JSValue* pv = (ref);     \
        JSValue old = *pv;       \
        *pv = *--sp;             \
        pc += (size);            \
        TC_FREE_DISPATCH(old);   \
    }
#define TC_SETV(name, size, ref)               \
    TC_HANDLER(name)                           \
    {                                          \
        JSValue* pv = (ref);                   \
        JSValue nv = JS_DupValue(ctx, sp[-1]); \
        JSValue old = *pv;                     \
        *pv = nv;                              \
        pc += (size);                          \
        TC_FREE_DISPATCH(old);                 \
    }
#define TC_VAR_FAMILY(kind, base)                        \
    TC_GETV(js_tc_get_##kind##0, 1, base[0])             \
    TC_GETV(js_tc_get_##kind##1, 1, base[1])             \
    TC_GETV(js_tc_get_##kind##2, 1, base[2])             \
    TC_GETV(js_tc_get_##kind##3, 1, base[3])             \
    TC_PUTV(js_tc_put_##kind##0, 1, &base[0])            \
    TC_PUTV(js_tc_put_##kind##1, 1, &base[1])            \
    TC_PUTV(js_tc_put_##kind##2, 1, &base[2])            \
    TC_PUTV(js_tc_put_##kind##3, 1, &base[3])            \
    TC_SETV(js_tc_set_##kind##0, 1, &base[0])            \
    TC_SETV(js_tc_set_##kind##1, 1, &base[1])            \
    TC_SETV(js_tc_set_##kind##2, 1, &base[2])            \
    TC_SETV(js_tc_set_##kind##3, 1, &base[3])            \
    TC_GETV(js_tc_get_##kind, 3, base[get_u16(pc + 1)])  \
    TC_PUTV(js_tc_put_##kind, 3, &base[get_u16(pc + 1)]) \
    TC_SETV(js_tc_set_##kind, 3, &base[get_u16(pc + 1)])

TC_VAR_FAMILY(loc, lb)
TC_VAR_FAMILY(arg, ab)
TC_GETV(js_tc_get_loc8, 2, lb[pc[1]])
TC_PUTV(js_tc_put_loc8, 2, &lb[pc[1]])
TC_SETV(js_tc_set_loc8, 2, &lb[pc[1]])

#define TC_VREF(i) (S->var_refs[i]->pvalue)
TC_GETV(js_tc_get_var_ref0, 1, *TC_VREF(0))
TC_GETV(js_tc_get_var_ref1, 1, *TC_VREF(1))
TC_GETV(js_tc_get_var_ref2, 1, *TC_VREF(2))
TC_GETV(js_tc_get_var_ref3, 1, *TC_VREF(3))
TC_PUTV(js_tc_put_var_ref0, 1, TC_VREF(0))
TC_PUTV(js_tc_put_var_ref1, 1, TC_VREF(1))
TC_PUTV(js_tc_put_var_ref2, 1, TC_VREF(2))
TC_PUTV(js_tc_put_var_ref3, 1, TC_VREF(3))
TC_SETV(js_tc_set_var_ref0, 1, TC_VREF(0))
TC_SETV(js_tc_set_var_ref1, 1, TC_VREF(1))
TC_SETV(js_tc_set_var_ref2, 1, TC_VREF(2))
TC_SETV(js_tc_set_var_ref3, 1, TC_VREF(3))
TC_GETV(js_tc_get_var_ref, 3, *TC_VREF(get_u16(pc + 1)))
TC_PUTV(js_tc_put_var_ref, 3, TC_VREF(get_u16(pc + 1)))
TC_SETV(js_tc_set_var_ref, 3, TC_VREF(get_u16(pc + 1)))

TC_HANDLER(js_tc_uninit_loc)
{
    pc += 3;
    JS_ThrowReferenceErrorUninitialized2(ctx, S->b, get_u16(pc - 2), FALSE);
    TC_THROW();
}

TC_HANDLER(js_tc_uninit_this)
{
    pc += 3;
    JS_ThrowReferenceErrorUninitialized2(S->caller_ctx, S->b, get_u16(pc - 2), FALSE);
    TC_THROW();
}

TC_HANDLER(js_tc_uninit_ref)
{
    pc += 3;
    JS_ThrowReferenceErrorUninitialized2(ctx, S->b, get_u16(pc - 2), TRUE);
    TC_THROW();
}

TC_HANDLER(js_tc_this_twice)
{
    pc += 3;
    JS_ThrowReferenceError(ctx, "'this' can be initialized only once");
    TC_THROW();
}

TC_HANDLER(js_tc_get_loc_check)
{
    JSValue* pv = &lb[get_u16(pc + 1)];
    if (unlikely(JS_IsUninitialized(*pv)))
        TC_JUMP(js_tc_uninit_loc);
    *sp++ = JS_DupValue(ctx, *pv);
    TC_NEXT(3);
}

TC_HANDLER(js_tc_get_loc_checkthis)
{
    JSValue* pv = &lb[get_u16(pc + 1)];
    if (unlikely(JS_IsUninitialized(*pv)))
        TC_JUMP(js_tc_uninit_this);
    *sp++ = JS_DupValue(ctx, *pv);
    TC_NEXT(3);
}

TC_HANDLER(js_tc_put_loc_check)
{
    JSValue* pv = &lb[get_u16(pc + 1)];
    JSValue old = *pv;
    if (unlikely(JS_IsUninitialized(old)))
        TC_JUMP(js_tc_uninit_loc);
    *pv = *--sp;
    pc += 3;
    TC_FREE_DISPATCH(old);
}

TC_HANDLER(js_tc_set_loc_check)
{
    JSValue* pv = &lb[get_u16(pc + 1)];
    JSValue old = *pv;
    if (unlikely(JS_IsUninitialized(old)))
        TC_JUMP(js_tc_uninit_loc);
    *pv = JS_DupValue(ctx, sp[-1]);
    pc += 3;
    TC_FREE_DISPATCH(old);
}

TC_HANDLER(js_tc_put_loc_check_init)
{
    JSValue* pv = &lb[get_u16(pc + 1)];
    JSValue old = *pv;
    if (unlikely(!JS_IsUninitialized(old)))
        TC_JUMP(js_tc_this_twice);
    *pv = *--sp;
    pc += 3;
    TC_FREE_DISPATCH(old);
}

TC_HANDLER(js_tc_set_loc_uninitialized)
{
    JSValue* pv = &lb[get_u16(pc + 1)];
    JSValue old = *pv;
    *pv = JS_UNINITIALIZED;
    pc += 3;
    TC_FREE_DISPATCH(old);
}

TC_HANDLER(js_tc_get_var_ref_check)
{
    JSValue val = *TC_VREF(get_u16(pc + 1));
    if (unlikely(JS_IsUninitialized(val)))
        TC_JUMP(js_tc_uninit_ref);
    *sp++ = JS_DupValue(ctx, val);
    TC_NEXT(3);
}

TC_HANDLER(js_tc_put_var_ref_check)
{
    JSValue* pv = TC_VREF(get_u16(pc + 1));
    JSValue old = *pv;
    if (unlikely(JS_IsUninitialized(old)))
        TC_JUMP(js_tc_uninit_ref);
    *pv = *--sp;
    pc += 3;
    TC_FREE_DISPATCH(old);
}

TC_HANDLER(js_tc_put_var_ref_check_init)
{
    JSValue* pv = TC_VREF(get_u16(pc + 1));
    JSValue old = *pv;
    if (unlikely(!JS_IsUninitialized(old)))
        TC_JUMP(js_tc_uninit_ref);
    *pv = *--sp;
    pc += 3;
    TC_FREE_DISPATCH(old);
}

TC_HANDLER(js_tc_close_loc)
{
    pc += 3;
    close_lexical_var(ctx, S->b, S->sf, get_u16(pc - 2));
    TC_DISPATCH();
}

TC_HANDLER(js_tc_goto)
{
    pc += 1 + (int32_t)get_u32(pc + 1);
    TC_POLL_DISPATCH();
}

TC_HANDLER(js_tc_goto16)
{
    pc += 1 + (int16_t)get_u16(pc + 1);
    TC_POLL_DISPATCH();
}

TC_HANDLER(js_tc_goto8)
{
    pc += 1 + (int8_t)pc[1];
    TC_POLL_DISPATCH();
}

#define TC_IF_TAIL(size, rd, test)                     \
    pc += (size);                                      \
    sp--;                                              \
    if (test) {                                        \
        int32_t br_off = (int32_t)(rd) - ((size) - 1); \
        pc += br_off;                                  \
        if (br_off <= 0)                               \
            TC_POLL_DISPATCH();                        \
    }                                                  \
    TC_DISPATCH();

#define TC_IF(name, size, rd, test)                                       \
    TC_HANDLER(name##_slow)                                               \
    {                                                                     \
        int res = JS_ToBoolFree(ctx, sp[-1]);                             \
        TC_IF_TAIL(size, rd, test)                                        \
    }                                                                     \
    TC_HANDLER(name)                                                      \
    {                                                                     \
        JSValue op1 = sp[-1];                                             \
        int res;                                                          \
        if (unlikely((uint32_t)JS_VALUE_GET_TAG(op1) > JS_TAG_UNDEFINED)) \
            TC_JUMP(name##_slow);                                         \
        res = JS_VALUE_GET_INT(op1);                                      \
        TC_IF_TAIL(size, rd, test)                                        \
    }

TC_IF(js_tc_if_true, 5, get_u32(pc - 4), res)
TC_IF(js_tc_if_false, 5, get_u32(pc - 4), !res)
TC_IF(js_tc_if_true8, 2, (int8_t)pc[-1], res)
TC_IF(js_tc_if_false8, 2, (int8_t)pc[-1], !res)

TC_HANDLER(js_tc_lnot_slow)
{
    int res = JS_ToBoolFree(ctx, sp[-1]);
    sp[-1] = JS_NewBool(ctx, !res);
    TC_NEXT(1);
}

TC_HANDLER(js_tc_lnot)
{
    JSValue op1 = sp[-1];
    if (unlikely((uint32_t)JS_VALUE_GET_TAG(op1) > JS_TAG_UNDEFINED))
        TC_JUMP(js_tc_lnot_slow);
    sp[-1] = JS_NewBool(ctx, !(JS_VALUE_GET_INT(op1) != 0));
    TC_NEXT(1);
}

TC_HANDLER(js_tc_arith_slow)
{
    pc += 1;
    S->sf->cur_pc = pc;
    if (js_binary_arith_slow(ctx, sp, (OPCodeEnum)pc[-1]))
        TC_THROW();
    sp--;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_add_slow)
{
    JSValue op1 = sp[-2], op2 = sp[-1];
    pc += 1;
    if (JS_IsString(op1) && JS_IsString(op2)) {
        int ar = js_add_string_to_slot(ctx, sp, pc, lb, ab, S->var_refs);
        if (ar < 0)
            TC_THROW();
        if (ar == 0)
            sp[-2] = JS_ConcatString(ctx, op1, op2);
        sp--;
        if (JS_IsException(sp[-1]))
            TC_THROW();
        TC_DISPATCH();
    }
    S->sf->cur_pc = pc;
    if (js_add_slow(ctx, sp))
        TC_THROW();
    sp--;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_add)
{
    JSValue op1 = sp[-2], op2 = sp[-1];
    double d1, d2;
    if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
        int64_t r = (int64_t)JS_VALUE_GET_INT(op1) + JS_VALUE_GET_INT(op2);
        if (unlikely((int)r != r))
            sp[-2] = __JS_NewFloat64(ctx, (double)r);
        else
            sp[-2] = JS_NewInt32(ctx, r);
        sp--;
        TC_NEXT(1);
    }
    if (js_tc_num2(op1, op2, &d1, &d2)) {
        sp[-2] = __JS_NewFloat64(ctx, d1 + d2);
        sp--;
        TC_NEXT(1);
    }
    TC_JUMP(js_tc_add_slow);
}

TC_HANDLER(js_tc_sub)
{
    JSValue op1 = sp[-2], op2 = sp[-1];
    double d1, d2;
    if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
        int64_t r = (int64_t)JS_VALUE_GET_INT(op1) - JS_VALUE_GET_INT(op2);
        if (unlikely((int)r != r))
            sp[-2] = __JS_NewFloat64(ctx, (double)r);
        else
            sp[-2] = JS_NewInt32(ctx, r);
        sp--;
        TC_NEXT(1);
    }
    if (js_tc_num2(op1, op2, &d1, &d2)) {
        sp[-2] = __JS_NewFloat64(ctx, d1 - d2);
        sp--;
        TC_NEXT(1);
    }
    TC_JUMP(js_tc_arith_slow);
}

TC_HANDLER(js_tc_mul)
{
    JSValue op1 = sp[-2], op2 = sp[-1];
    double d1, d2;
    if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
        int32_t v1 = JS_VALUE_GET_INT(op1), v2 = JS_VALUE_GET_INT(op2);
        int64_t r = (int64_t)v1 * v2;
        if (unlikely((int)r != r))
            sp[-2] = __JS_NewFloat64(ctx, (double)r);
        else if (unlikely(r == 0 && (v1 | v2) < 0))
            sp[-2] = __JS_NewFloat64(ctx, -0.0);
        else
            sp[-2] = JS_NewInt32(ctx, r);
        sp--;
        TC_NEXT(1);
    }
    if (js_tc_num2(op1, op2, &d1, &d2)) {
        sp[-2] = __JS_NewFloat64(ctx, d1 * d2);
        sp--;
        TC_NEXT(1);
    }
    TC_JUMP(js_tc_arith_slow);
}

TC_HANDLER(js_tc_div)
{
    JSValue op1 = sp[-2], op2 = sp[-1];
    double d1, d2;
    if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
        sp[-2] = JS_NewFloat64(ctx, (double)JS_VALUE_GET_INT(op1) / (double)JS_VALUE_GET_INT(op2));
        sp--;
        TC_NEXT(1);
    }
    if (js_tc_num2(op1, op2, &d1, &d2)) {
        sp[-2] = __JS_NewFloat64(ctx, d1 / d2);
        sp--;
        TC_NEXT(1);
    }
    TC_JUMP(js_tc_arith_slow);
}

TC_HANDLER(js_tc_mod_slow)
{
    JSValue op1 = sp[-2], op2 = sp[-1];
    double d1, d2;
    if (!JS_VALUE_IS_BOTH_INT(op1, op2) && js_tc_num2(op1, op2, &d1, &d2)) {
        sp[-2] = __JS_NewFloat64(ctx, js_fmod(d1, d2));
        sp--;
        TC_NEXT(1);
    }
    TC_JUMP(js_tc_arith_slow);
}

TC_HANDLER(js_tc_mod)
{
    JSValue op1 = sp[-2], op2 = sp[-1];
    if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
        int v1 = JS_VALUE_GET_INT(op1), v2 = JS_VALUE_GET_INT(op2);
        if (likely(v1 >= 0 && v2 > 0)) {
            sp[-2] = JS_NewInt32(ctx, v1 % v2);
            sp--;
            TC_NEXT(1);
        }
    }
    TC_JUMP(js_tc_mod_slow);
}

TC_HANDLER(js_tc_pow)
{
    JSValue op1 = sp[-2], op2 = sp[-1];
    double d1, d2;
    if (JS_VALUE_IS_BOTH_INT(op1, op2)) {
        sp[-2] = JS_NewFloat64(ctx, js_pow(JS_VALUE_GET_INT(op1), JS_VALUE_GET_INT(op2)));
        sp--;
        TC_NEXT(1);
    }
    if (js_tc_num2(op1, op2, &d1, &d2)) {
        sp[-2] = __JS_NewFloat64(ctx, js_pow(d1, d2));
        sp--;
        TC_NEXT(1);
    }
    TC_JUMP(js_tc_arith_slow);
}

TC_HANDLER(js_tc_add_loc_slow)
{
    JSValue op2 = sp[-1];
    JSValue* pv = &lb[pc[1]];
    pc += 2;
    if (JS_VALUE_GET_TAG(*pv) == JS_TAG_STRING && JS_VALUE_GET_TAG(op2) == JS_TAG_STRING) {
        sp--;
        S->sf->cur_pc = pc;
        JSValue hr;
        if (JS_ConcatStringInPlace(ctx, JS_VALUE_GET_STRING(*pv), op2)) {
            JS_FreeValue(ctx, op2);
        } else if (!JS_IsUndefined(hr = js_concat_headroom(ctx, JS_VALUE_GET_STRING(*pv), op2))) {
            JS_FreeValue(ctx, op2);
            if (JS_IsException(hr))
                TC_THROW();
            set_value(ctx, pv, hr);
        } else {
            op2 = JS_ConcatString(ctx, JS_DupValue(ctx, *pv), op2);
            if (JS_IsException(op2))
                TC_THROW();
            set_value(ctx, pv, op2);
        }
    } else if (JS_VALUE_GET_TAG(*pv) == JS_TAG_STRING_ROPE && js_rope_append_in_place(ctx, *pv, op2)) {
        sp--;
        JS_FreeValue(ctx, op2);
    } else {
        JSValue ops[2];
        S->sf->cur_pc = pc;
        ops[0] = JS_DupValue(ctx, *pv);
        ops[1] = op2;
        sp--;
        if (js_add_slow(ctx, ops + 2))
            TC_THROW();
        set_value(ctx, pv, ops[0]);
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_add_loc)
{
    JSValue op2 = sp[-1];
    JSValue* pv = &lb[pc[1]];
    double d1, d2;
    if (likely(JS_VALUE_IS_BOTH_INT(*pv, op2))) {
        int64_t r = (int64_t)JS_VALUE_GET_INT(*pv) + JS_VALUE_GET_INT(op2);
        if (unlikely((int)r != r))
            *pv = __JS_NewFloat64(ctx, (double)r);
        else
            *pv = JS_NewInt32(ctx, r);
        sp--;
        TC_NEXT(2);
    }
    if (js_tc_num2(*pv, op2, &d1, &d2)) {
        *pv = __JS_NewFloat64(ctx, d1 + d2);
        sp--;
        TC_NEXT(2);
    }
    TC_JUMP(js_tc_add_loc_slow);
}

TC_HANDLER(js_tc_unary_slow)
{
    pc += 1;
    S->sf->cur_pc = pc;
    if (js_unary_arith_slow(ctx, sp, (OPCodeEnum)pc[-1]))
        TC_THROW();
    TC_DISPATCH();
}

TC_HANDLER(js_tc_plus)
{
    uint32_t tag = JS_VALUE_GET_TAG(sp[-1]);
    if (tag == JS_TAG_INT || JS_TAG_IS_FLOAT64(tag))
        TC_NEXT(1);
    if (tag == JS_TAG_NULL || tag == JS_TAG_BOOL) {
        sp[-1] = JS_NewInt32(ctx, JS_VALUE_GET_INT(sp[-1]));
        TC_NEXT(1);
    }
    TC_JUMP(js_tc_unary_slow);
}

TC_HANDLER(js_tc_neg)
{
    JSValue op1 = sp[-1];
    uint32_t tag = JS_VALUE_GET_TAG(op1);
    if (tag == JS_TAG_INT || tag == JS_TAG_BOOL || tag == JS_TAG_NULL) {
        int val = JS_VALUE_GET_INT(op1);
        if (unlikely(val == 0))
            sp[-1] = __JS_NewFloat64(ctx, -0.0);
        else if (unlikely(val == INT32_MIN))
            sp[-1] = __JS_NewFloat64(ctx, -(double)val);
        else
            sp[-1] = JS_NewInt32(ctx, -val);
        TC_NEXT(1);
    }
    if (JS_TAG_IS_FLOAT64(tag)) {
        sp[-1] = __JS_NewFloat64(ctx, -JS_VALUE_GET_FLOAT64(op1));
        TC_NEXT(1);
    }
    TC_JUMP(js_tc_unary_slow);
}

TC_HANDLER(js_tc_post_slow)
{
    pc += 1;
    S->sf->cur_pc = pc;
    if (js_post_inc_slow(ctx, sp, (OPCodeEnum)pc[-1]))
        TC_THROW();
    sp++;
    TC_DISPATCH();
}

#define TC_INCDEC(name, delta, limit, slow, post)                                            \
    TC_HANDLER(name)                                                                         \
    {                                                                                        \
        JSValue op1 = sp[-1];                                                                \
        if (likely(JS_VALUE_GET_TAG(op1) == JS_TAG_INT && JS_VALUE_GET_INT(op1) != limit)) { \
            sp[post - 1] = JS_NewInt32(ctx, JS_VALUE_GET_INT(op1) + delta);                  \
            sp += post;                                                                      \
            TC_NEXT(1);                                                                      \
        }                                                                                    \
        TC_JUMP(slow);                                                                       \
    }

TC_INCDEC(js_tc_inc, 1, INT32_MAX, js_tc_unary_slow, 0)
TC_INCDEC(js_tc_dec, -1, INT32_MIN, js_tc_unary_slow, 0)
TC_INCDEC(js_tc_post_inc, 1, INT32_MAX, js_tc_post_slow, 1)
TC_INCDEC(js_tc_post_dec, -1, INT32_MIN, js_tc_post_slow, 1)

TC_HANDLER(js_tc_incdec_loc_slow)
{
    JSValue tmp[1];
    int idx = pc[1];
    OPCodeEnum op = pc[0] == OP_inc_loc ? OP_inc : OP_dec;
    pc += 2;
    S->sf->cur_pc = pc;
    tmp[0] = JS_DupValue(ctx, lb[idx]);
    if (js_unary_arith_slow(ctx, tmp + 1, op))
        TC_THROW();
    set_value(ctx, &lb[idx], tmp[0]);
    TC_DISPATCH();
}

#define TC_INCDEC_LOC(name, delta, limit)                                                    \
    TC_HANDLER(name)                                                                         \
    {                                                                                        \
        JSValue* pv = &lb[pc[1]];                                                            \
        if (likely(JS_VALUE_GET_TAG(*pv) == JS_TAG_INT && JS_VALUE_GET_INT(*pv) != limit)) { \
            *pv = JS_NewInt32(ctx, JS_VALUE_GET_INT(*pv) + delta);                           \
            TC_NEXT(2);                                                                      \
        }                                                                                    \
        TC_JUMP(js_tc_incdec_loc_slow);                                                      \
    }

TC_INCDEC_LOC(js_tc_inc_loc, 1, INT32_MAX)
TC_INCDEC_LOC(js_tc_dec_loc, -1, INT32_MIN)

TC_HANDLER(js_tc_not_slow)
{
    pc += 1;
    S->sf->cur_pc = pc;
    if (js_not_slow(ctx, sp))
        TC_THROW();
    TC_DISPATCH();
}

TC_HANDLER(js_tc_not)
{
    if (likely(JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_INT)) {
        sp[-1] = JS_NewInt32(ctx, ~JS_VALUE_GET_INT(sp[-1]));
        TC_NEXT(1);
    }
    TC_JUMP(js_tc_not_slow);
}

TC_HANDLER(js_tc_logic_slow)
{
    JSValue op1 = sp[-2], op2 = sp[-1];
    int32_t ia, ib;
    OPCodeEnum op = (OPCodeEnum)pc[0];
    pc += 1;
    if (js_num_i32(op1, &ia) && js_num_i32(op2, &ib)) {
        switch ((int)op) {
        case OP_shl:
            sp[-2] = JS_NewInt32(ctx, (int32_t)((uint32_t)ia << (ib & 0x1f)));
            break;
        case OP_shr:
            sp[-2] = JS_NewUint32(ctx, (uint32_t)ia >> (ib & 0x1f));
            break;
        case OP_sar:
            sp[-2] = JS_NewInt32(ctx, ia >> (ib & 0x1f));
            break;
        case OP_and:
            sp[-2] = JS_NewInt32(ctx, ia & ib);
            break;
        case OP_or:
            sp[-2] = JS_NewInt32(ctx, ia | ib);
            break;
        default:
            sp[-2] = JS_NewInt32(ctx, ia ^ ib);
            break;
        }
        sp--;
        TC_DISPATCH();
    }
    S->sf->cur_pc = pc;
    if (op == OP_shr ? js_shr_slow(ctx, sp) : js_binary_logic_slow(ctx, sp, op))
        TC_THROW();
    sp--;
    TC_DISPATCH();
}

#define TC_LOGIC(name, mk, expr)                      \
    TC_HANDLER(name)                                  \
    {                                                 \
        JSValue op1 = sp[-2], op2 = sp[-1];           \
        if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) { \
            uint32_t a = JS_VALUE_GET_INT(op1);       \
            uint32_t b = JS_VALUE_GET_INT(op2);       \
            sp[-2] = mk(ctx, expr);                   \
            sp--;                                     \
            TC_NEXT(1);                               \
        }                                             \
        TC_JUMP(js_tc_logic_slow);                    \
    }

TC_LOGIC(js_tc_shl, JS_NewInt32, a << (b & 0x1f))
TC_LOGIC(js_tc_shr, JS_NewUint32, a >> (b & 0x1f))
TC_LOGIC(js_tc_sar, JS_NewInt32, (int)a >> (b & 0x1f))
TC_LOGIC(js_tc_and, JS_NewInt32, a& b)
TC_LOGIC(js_tc_or, JS_NewInt32, a | b)
TC_LOGIC(js_tc_xor, JS_NewInt32, a ^ b)

TC_HANDLER(js_tc_rel_slow)
{
    pc += 1;
    S->sf->cur_pc = pc;
    if (js_relational_slow(ctx, sp, (OPCodeEnum)pc[-1]))
        TC_THROW();
    sp--;
    TC_DISPATCH();
}

#define TC_CMP(name, cop)                                                              \
    TC_HANDLER(name)                                                                   \
    {                                                                                  \
        JSValue op1 = sp[-2], op2 = sp[-1];                                            \
        double d1, d2;                                                                 \
        if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {                                  \
            sp[-2] = JS_NewBool(ctx, JS_VALUE_GET_INT(op1) cop JS_VALUE_GET_INT(op2)); \
            sp--;                                                                      \
            TC_NEXT(1);                                                                \
        }                                                                              \
        if (js_tc_num2(op1, op2, &d1, &d2)) {                                          \
            sp[-2] = JS_NewBool(ctx, d1 cop d2);                                       \
            sp--;                                                                      \
            TC_NEXT(1);                                                                \
        }                                                                              \
        TC_JUMP(js_tc_rel_slow);                                                       \
    }

TC_CMP(js_tc_lt, <)
TC_CMP(js_tc_lte, <=)
TC_CMP(js_tc_gt, >)
TC_CMP(js_tc_gte, >=)

#define TC_BR_TAIL(test)                            \
    sp -= 2;                                        \
    if (test) {                                     \
        int32_t br_diff = (int32_t)get_u32(pc - 4); \
        pc += br_diff - 4;                          \
        if (br_diff <= 0)                           \
            TC_POLL_DISPATCH();                     \
    }                                               \
    TC_DISPATCH();

#define TC_CMP_BRANCH(name, base, cop)                             \
    TC_HANDLER(name##_slow)                                        \
    {                                                              \
        int res;                                                   \
        pc += 5;                                                   \
        S->sf->cur_pc = pc;                                        \
        if (js_relational_slow(ctx, sp, base))                     \
            TC_THROW();                                            \
        res = JS_VALUE_GET_INT(sp[-2]);                            \
        TC_BR_TAIL(!res)                                           \
    }                                                              \
    TC_HANDLER(name)                                               \
    {                                                              \
        JSValue op1 = sp[-2], op2 = sp[-1];                        \
        double d1, d2;                                             \
        int res;                                                   \
        if (likely(JS_VALUE_IS_BOTH_INT(op1, op2)))                \
            res = JS_VALUE_GET_INT(op1) cop JS_VALUE_GET_INT(op2); \
        else if (js_tc_num2(op1, op2, &d1, &d2))                   \
            res = d1 cop d2;                                       \
        else                                                       \
            TC_JUMP(name##_slow);                                  \
        pc += 5;                                                   \
        TC_BR_TAIL(!res)                                           \
    }

TC_CMP_BRANCH(js_tc_lt_if_false, OP_lt, <)
TC_CMP_BRANCH(js_tc_lte_if_false, OP_lte, <=)
TC_CMP_BRANCH(js_tc_gt_if_false, OP_gt, >)
TC_CMP_BRANCH(js_tc_gte_if_false, OP_gte, >=)

static force_inline int js_tc_strict_eq_free(JSContext* ctx, JSValue op1, JSValue op2)
{
    int res;
    uint32_t tag1 = JS_VALUE_GET_TAG(op1), tag2 = JS_VALUE_GET_TAG(op2);
    if (likely(tag1 == JS_TAG_INT)) {
        if (tag2 == JS_TAG_INT) {
            res = JS_VALUE_GET_INT(op1) == JS_VALUE_GET_INT(op2);
        } else if (JS_TAG_IS_FLOAT64(tag2)) {
            res = (JS_VALUE_GET_INT(op1) == JS_VALUE_GET_FLOAT64(op2));
        } else {
            JS_FreeValue(ctx, op2);
            res = FALSE;
        }
    } else if (JS_TAG_IS_FLOAT64(tag1)) {
        if (tag2 == JS_TAG_INT) {
            res = JS_VALUE_GET_FLOAT64(op1) == JS_VALUE_GET_INT(op2);
        } else if (JS_TAG_IS_FLOAT64(tag2)) {
            res = (JS_VALUE_GET_FLOAT64(op1) == JS_VALUE_GET_FLOAT64(op2));
        } else {
            JS_FreeValue(ctx, op2);
            res = FALSE;
        }
    } else if (tag1 == JS_TAG_OBJECT) {
        if (tag2 == JS_TAG_OBJECT)
            res = JS_VALUE_GET_OBJ(op1) == JS_VALUE_GET_OBJ(op2);
        else
            res = FALSE;
        JS_FreeValue(ctx, op1);
        JS_FreeValue(ctx, op2);
    } else if (tag1 == JS_TAG_NULL || tag1 == JS_TAG_UNDEFINED) {
        res = (tag1 == tag2);
        JS_FreeValue(ctx, op2);
    } else if (tag1 == JS_TAG_STRING && tag2 == JS_TAG_STRING) {
        res = js_string_eq_quick(ctx, JS_VALUE_GET_STRING(op1), JS_VALUE_GET_STRING(op2));
        JS_FreeValue(ctx, op1);
        JS_FreeValue(ctx, op2);
    } else {
        res = js_strict_eq2(ctx, op1, op2, JS_EQ_STRICT);
        JS_FreeValue(ctx, op1);
        JS_FreeValue(ctx, op2);
    }
    return res;
}

static force_inline int js_tc_strict_eq_str(JSContext* ctx, JSValue op1, JSValue op2)
{
    int res;
    if (JS_VALUE_GET_TAG(op1) != JS_TAG_STRING || JS_VALUE_GET_TAG(op2) != JS_TAG_STRING)
        return js_tc_strict_eq_free(ctx, op1, op2);
    res = js_string_eq_quick(ctx, JS_VALUE_GET_STRING(op1), JS_VALUE_GET_STRING(op2));
    JS_FreeValue(ctx, op1);
    JS_FreeValue(ctx, op2);
    return res;
}

#define TC_SEQ_BRANCH(name, test)                                   \
    TC_HANDLER(name##_slow)                                         \
    {                                                               \
        int res = js_tc_strict_eq_str(ctx, sp[-2], sp[-1]);         \
        pc += 5;                                                    \
        TC_BR_TAIL(test)                                            \
    }                                                               \
    TC_HANDLER(name)                                                \
    {                                                               \
        int res;                                                    \
        if (unlikely(!JS_VALUE_IS_BOTH_INT(sp[-2], sp[-1])))        \
            TC_JUMP(name##_slow);                                   \
        res = JS_VALUE_GET_INT(sp[-2]) == JS_VALUE_GET_INT(sp[-1]); \
        pc += 5;                                                    \
        TC_BR_TAIL(test)                                            \
    }

TC_SEQ_BRANCH(js_tc_strict_eq_if_false, !(res ^ 0))
TC_SEQ_BRANCH(js_tc_strict_neq_if_false, !(res ^ 1))
TC_SEQ_BRANCH(js_tc_strict_eq_if_true, (res ^ 0))
TC_SEQ_BRANCH(js_tc_strict_neq_if_true, (res ^ 1))

#define TC_SEQ(name, inv)                                                                       \
    TC_HANDLER(name##_slow)                                                                     \
    {                                                                                           \
        int res = js_tc_strict_eq_free(ctx, sp[-2], sp[-1]);                                    \
        sp[-2] = JS_NewBool(ctx, res ^ inv);                                                    \
        sp--;                                                                                   \
        TC_NEXT(1);                                                                             \
    }                                                                                           \
    TC_HANDLER(name)                                                                            \
    {                                                                                           \
        if (unlikely(!JS_VALUE_IS_BOTH_INT(sp[-2], sp[-1])))                                    \
            TC_JUMP(name##_slow);                                                               \
        sp[-2] = JS_NewBool(ctx, (JS_VALUE_GET_INT(sp[-2]) == JS_VALUE_GET_INT(sp[-1])) ^ inv); \
        sp--;                                                                                   \
        TC_NEXT(1);                                                                             \
    }

TC_SEQ(js_tc_strict_eq, 0)
TC_SEQ(js_tc_strict_neq, 1)

static int js_tc_loose_eq_free(JSContext* ctx, JSValue op1, JSValue op2)
{
    uint32_t tag1 = JS_VALUE_GET_TAG(op1), tag2 = JS_VALUE_GET_TAG(op2);
    int res;
    if (likely(tag1 == JS_TAG_INT)) {
        if (tag2 == JS_TAG_INT)
            return JS_VALUE_GET_INT(op1) == JS_VALUE_GET_INT(op2);
        if (JS_TAG_IS_FLOAT64(tag2))
            return JS_VALUE_GET_INT(op1) == JS_VALUE_GET_FLOAT64(op2);
        return -1;
    }
    if (JS_TAG_IS_FLOAT64(tag1)) {
        if (tag2 == JS_TAG_INT)
            return JS_VALUE_GET_FLOAT64(op1) == JS_VALUE_GET_INT(op2);
        if (JS_TAG_IS_FLOAT64(tag2))
            return JS_VALUE_GET_FLOAT64(op1) == JS_VALUE_GET_FLOAT64(op2);
        return -1;
    }
    if (tag1 == JS_TAG_OBJECT) {
        if (tag2 == JS_TAG_NULL || tag2 == JS_TAG_UNDEFINED) {
            res = JS_VALUE_GET_OBJ(op1)->is_HTMLDDA;
            JS_FreeValue(ctx, op1);
            return res;
        }
        if (tag2 == JS_TAG_OBJECT) {
            res = JS_VALUE_GET_OBJ(op1) == JS_VALUE_GET_OBJ(op2);
            JS_FreeValue(ctx, op1);
            JS_FreeValue(ctx, op2);
            return res;
        }
        return -1;
    }
    if (tag1 == JS_TAG_NULL || tag1 == JS_TAG_UNDEFINED) {
        if (tag2 == JS_TAG_NULL || tag2 == JS_TAG_UNDEFINED)
            return TRUE;
        if (tag2 == JS_TAG_OBJECT) {
            res = JS_VALUE_GET_OBJ(op2)->is_HTMLDDA;
            JS_FreeValue(ctx, op2);
            return res;
        }
        return -1;
    }
    if (tag1 == JS_TAG_STRING && tag2 == JS_TAG_STRING) {
        res = js_string_eq_quick(ctx, JS_VALUE_GET_STRING(op1), JS_VALUE_GET_STRING(op2));
        JS_FreeValue(ctx, op1);
        JS_FreeValue(ctx, op2);
        return res;
    }
    return -1;
}

#define TC_EQ(name, inv)                                                                        \
    TC_HANDLER(name##_slow)                                                                     \
    {                                                                                           \
        int res = js_tc_loose_eq_free(ctx, sp[-2], sp[-1]);                                     \
        pc += 1;                                                                                \
        if (res < 0) {                                                                          \
            S->sf->cur_pc = pc;                                                                 \
            if (js_eq_slow(ctx, sp, inv))                                                       \
                TC_THROW();                                                                     \
            sp--;                                                                               \
            TC_DISPATCH();                                                                      \
        }                                                                                       \
        sp[-2] = JS_NewBool(ctx, res ^ inv);                                                    \
        sp--;                                                                                   \
        TC_DISPATCH();                                                                          \
    }                                                                                           \
    TC_HANDLER(name)                                                                            \
    {                                                                                           \
        if (unlikely(!JS_VALUE_IS_BOTH_INT(sp[-2], sp[-1])))                                    \
            TC_JUMP(name##_slow);                                                               \
        sp[-2] = JS_NewBool(ctx, (JS_VALUE_GET_INT(sp[-2]) == JS_VALUE_GET_INT(sp[-1])) ^ inv); \
        sp--;                                                                                   \
        TC_NEXT(1);                                                                             \
    }

TC_EQ(js_tc_eq, 0)
TC_EQ(js_tc_neq, 1)

TC_HANDLER(js_tc_release2)
{
    __JS_FreeValue(ctx, S->rel);
    JS_FreeValue(ctx, S->rel2);
    TC_DISPATCH();
}

#define TC_FREE2_DISPATCH(v1, v2)                                                                      \
    do {                                                                                               \
        if (JS_VALUE_HAS_REF_COUNT(v1) && unlikely(--__js_rc(JS_VALUE_GET_PTR(v1))->ref_count <= 0)) { \
            S->rel = (v1);                                                                             \
            S->rel2 = (v2);                                                                            \
            TC_JUMP(js_tc_release2);                                                                   \
        }                                                                                              \
        TC_FREE_DISPATCH(v2);                                                                          \
    } while (0)

#define TC_GET_EL_TAIL(keep) \
    pc += 1;                 \
    if (keep) {              \
        sp[-1] = val;        \
        TC_DISPATCH();       \
    }                        \
    sp[-2] = val;            \
    sp--;                    \
    TC_FREE_DISPATCH(obj);

static force_inline JSValue js_tc_get_sym_prop(JSContext* ctx, JSValueConst obj, JSAtom atom)
{
    JSObject* p = JS_VALUE_GET_OBJ(obj);
    JSProperty* pr;
    JSShapeProperty* prs;
    int depth = 0;
    for (;;) {
        if (unlikely(p->is_exotic || depth++ >= 8))
            break;
        prs = find_own_property(&pr, p, atom);
        if (prs) {
            if (unlikely(prs->flags & JS_PROP_TMASK))
                break;
            return JS_DupValue(ctx, pr->u.value);
        }
        p = p->shape->proto;
        if (!p)
            return JS_UNDEFINED;
    }
    return JS_GetPropertyInternal(ctx, obj, atom, obj, FALSE);
}

#define TC_GET_EL(name, keep)                                                                         \
    TC_HANDLER(name);                                                                                 \
    TC_HANDLER(name##_slow)                                                                           \
    {                                                                                                 \
        JSValue obj = sp[-2], prop, val;                                                              \
        int sres_;                                                                                    \
        if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && js_int_key_from_float(&sp[-1]))                 \
            TC_JUMP(name);                                                                            \
        prop = sp[-1];                                                                                \
        pc += 1;                                                                                      \
        S->sf->cur_pc = pc;                                                                           \
        if (JS_VALUE_GET_TAG(prop) == JS_TAG_SYMBOL && JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {      \
            val = js_tc_get_sym_prop(ctx, obj, js_get_atom_index(ctx->rt, JS_VALUE_GET_PTR(prop)));    \
            JS_FreeValue(ctx, prop);                                                                  \
        } else {                                                                                      \
            sres_ = js_string_index_get(ctx, obj, prop, &val);                                        \
            if (sres_ == 0)                                                                           \
                val = JS_GetPropertyValue(ctx, obj, prop);                                            \
            else if (sres_ < 0)                                                                       \
                val = JS_EXCEPTION;                                                                   \
        }                                                                                             \
        if (unlikely(JS_IsException(val))) {                                                          \
            if (keep)                                                                                 \
                sp[-1] = JS_UNDEFINED;                                                                \
            else                                                                                      \
                sp--;                                                                                 \
            TC_THROW();                                                                               \
        }                                                                                             \
        if (keep) {                                                                                   \
            sp[-1] = val;                                                                             \
            TC_DISPATCH();                                                                            \
        }                                                                                             \
        sp[-2] = val;                                                                                 \
        sp--;                                                                                         \
        TC_FREE_DISPATCH(obj);                                                                        \
    }                                                                                                 \
    TC_HANDLER(name##_ta)                                                                             \
    {                                                                                                 \
        JSValue obj = sp[-2], prop = sp[-1], val;                                                     \
        if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT || JS_VALUE_GET_TAG(prop) != JS_TAG_INT            \
            || !js_fast_array_get(ctx, JS_VALUE_GET_OBJ(obj), JS_VALUE_GET_INT(prop), &val, TRUE))    \
            TC_JUMP(name##_slow);                                                                     \
        TC_GET_EL_TAIL(keep)                                                                          \
    }                                                                                                 \
    TC_HANDLER(name)                                                                                  \
    {                                                                                                 \
        JSValue obj = sp[-2], prop = sp[-1], val;                                                     \
        JSObject* p;                                                                                  \
        uint32_t idx;                                                                                 \
        if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT || JS_VALUE_GET_TAG(prop) != JS_TAG_INT)) \
            TC_JUMP(name##_slow);                                                                     \
        p = JS_VALUE_GET_OBJ(obj);                                                                    \
        idx = JS_VALUE_GET_INT(prop);                                                                 \
        if (likely(p->class_id == JS_CLASS_ARRAY && idx < p->u.array.count))                          \
            val = JS_DupValue(ctx, p->u.array.u.values[idx]);                                         \
        else if (p->class_id == JS_CLASS_FLOAT64_ARRAY && idx < p->u.array.count)                     \
            val = __JS_NewFloat64(ctx, p->u.array.u.double_ptr[idx]);                                 \
        else                                                                                          \
            TC_JUMP(name##_ta);                                                                       \
        TC_GET_EL_TAIL(keep)                                                                          \
    }

TC_GET_EL(js_tc_get_array_el, 0)
TC_GET_EL(js_tc_get_array_el2, 1)

TC_HANDLER(js_tc_get_array_el3);

TC_HANDLER(js_tc_get_array_el3_slow)
{
    JSValue val, key;
    if (JS_VALUE_GET_TAG(sp[-2]) == JS_TAG_OBJECT && js_int_key_from_float(&sp[-1]))
        TC_JUMP(js_tc_get_array_el3);
    pc += 1;
    switch (JS_VALUE_GET_TAG(sp[-1])) {
    case JS_TAG_INT:
    case JS_TAG_STRING:
    case JS_TAG_SYMBOL:
        break;
    default:
        if (unlikely(JS_IsUndefined(sp[-2]) || JS_IsNull(sp[-2]))) {
            JS_ThrowTypeError(ctx, "value has no property");
            TC_THROW();
        }
        S->sf->cur_pc = pc;
        key = JS_ToPropertyKey(ctx, sp[-1]);
        if (JS_IsException(key))
            TC_THROW();
        JS_FreeValue(ctx, sp[-1]);
        sp[-1] = key;
        break;
    }
    S->sf->cur_pc = pc;
    val = JS_GetPropertyValue(ctx, sp[-2], JS_DupValue(ctx, sp[-1]));
    if (unlikely(JS_IsException(val)))
        TC_THROW();
    *sp++ = val;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_get_array_el3)
{
    JSObject* p;
    uint32_t idx;
    if (unlikely(JS_VALUE_GET_TAG(sp[-2]) != JS_TAG_OBJECT || JS_VALUE_GET_TAG(sp[-1]) != JS_TAG_INT))
        TC_JUMP(js_tc_get_array_el3_slow);
    p = JS_VALUE_GET_OBJ(sp[-2]);
    idx = JS_VALUE_GET_INT(sp[-1]);
    if (likely(p->class_id == JS_CLASS_ARRAY && idx < p->u.array.count))
        *sp = JS_DupValue(ctx, p->u.array.u.values[idx]);
    else if (!js_fast_array_get(ctx, p, idx, sp, TRUE))
        TC_JUMP(js_tc_get_array_el3_slow);
    sp++;
    TC_NEXT(1);
}

TC_HANDLER(js_tc_put_array_el);

TC_HANDLER(js_tc_put_array_el_slow)
{
    int ret;
    JSObject* arr_obj;
    uint32_t idx;
    if (JS_VALUE_GET_TAG(sp[-3]) == JS_TAG_OBJECT && js_int_key_from_float(&sp[-2]))
        TC_JUMP(js_tc_put_array_el);
    pc += 1;
    if (likely(JS_VALUE_GET_TAG(sp[-3]) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(sp[-2]) == JS_TAG_INT)) {
        arr_obj = JS_VALUE_GET_OBJ(sp[-3]);
        idx = JS_VALUE_GET_INT(sp[-2]);
        if (likely(arr_obj->class_id == JS_CLASS_ARRAY))
            goto put_array_el_plain;
        if (arr_obj->class_id >= JS_CLASS_UINT8C_ARRAY && arr_obj->class_id <= JS_CLASS_FLOAT64_ARRAY) {
            int vtag = JS_VALUE_GET_TAG(sp[-1]);
            double d;
            if (idx < arr_obj->u.array.count && likely(!arr_obj->ta_immutable)) {
                switch (arr_obj->class_id) {
                case JS_CLASS_INT8_ARRAY:
                case JS_CLASS_UINT8_ARRAY:
                    if (vtag != JS_TAG_INT)
                        goto put_array_el_slow_path;
                    arr_obj->u.array.u.uint8_ptr[idx] = (uint8_t)JS_VALUE_GET_INT(sp[-1]);
                    break;
                case JS_CLASS_UINT8C_ARRAY: {
                    int v;
                    if (vtag != JS_TAG_INT)
                        goto put_array_el_slow_path;
                    v = JS_VALUE_GET_INT(sp[-1]);
                    arr_obj->u.array.u.uint8_ptr[idx] = v < 0 ? 0 : v > 255 ? 255
                                                                            : (uint8_t)v;
                    break;
                }
                case JS_CLASS_INT16_ARRAY:
                case JS_CLASS_UINT16_ARRAY:
                    if (vtag != JS_TAG_INT)
                        goto put_array_el_slow_path;
                    arr_obj->u.array.u.uint16_ptr[idx] = (uint16_t)JS_VALUE_GET_INT(sp[-1]);
                    break;
                case JS_CLASS_INT32_ARRAY:
                case JS_CLASS_UINT32_ARRAY:
                    if (vtag != JS_TAG_INT)
                        goto put_array_el_slow_path;
                    arr_obj->u.array.u.uint32_ptr[idx] = (uint32_t)JS_VALUE_GET_INT(sp[-1]);
                    break;
                case JS_CLASS_FLOAT32_ARRAY:
                    if (vtag == JS_TAG_INT)
                        d = JS_VALUE_GET_INT(sp[-1]);
                    else if (vtag == JS_TAG_FLOAT64)
                        d = JS_VALUE_GET_FLOAT64(sp[-1]);
                    else
                        goto put_array_el_slow_path;
                    arr_obj->u.array.u.float_ptr[idx] = (float)d;
                    break;
                case JS_CLASS_FLOAT64_ARRAY:
                    if (vtag == JS_TAG_INT)
                        d = JS_VALUE_GET_INT(sp[-1]);
                    else if (vtag == JS_TAG_FLOAT64)
                        d = JS_VALUE_GET_FLOAT64(sp[-1]);
                    else
                        goto put_array_el_slow_path;
                    arr_obj->u.array.u.double_ptr[idx] = d;
                    break;
                default:
                    goto put_array_el_slow_path;
                }
            } else if (arr_obj->ta_immutable || (vtag != JS_TAG_INT && vtag != JS_TAG_FLOAT64) || arr_obj->class_id == JS_CLASS_BIG_INT64_ARRAY || arr_obj->class_id == JS_CLASS_BIG_UINT64_ARRAY) {
                goto put_array_el_slow_path;
            }
            JS_FreeValue(ctx, sp[-3]);
            sp -= 3;
            TC_DISPATCH();
        }
        if (unlikely(arr_obj->class_id != JS_CLASS_ARRAY))
            goto put_array_el_slow_path;
    put_array_el_plain:
        if (unlikely(idx >= (uint32_t)arr_obj->u.array.count)) {
            uint32_t new_len, array_len;
            if (unlikely(idx != (uint32_t)arr_obj->u.array.count || !arr_obj->fast_array || !can_extend_fast_array(arr_obj)))
                goto put_array_el_slow_path;
            if (unlikely(JS_VALUE_GET_TAG(arr_obj->prop[0].u.value) != JS_TAG_INT))
                goto put_array_el_slow_path;
            new_len = idx + 1;
            if (unlikely(new_len > arr_obj->u.array.u1.size))
                goto put_array_el_slow_path;
            array_len = JS_VALUE_GET_INT(arr_obj->prop[0].u.value);
            if (new_len > array_len) {
                if (unlikely(!(get_shape_prop(arr_obj->shape)->flags & JS_PROP_WRITABLE)))
                    goto put_array_el_slow_path;
                arr_obj->prop[0].u.value = JS_NewInt32(ctx, new_len);
            }
            arr_obj->u.array.count = new_len;
            arr_obj->u.array.u.values[idx] = sp[-1];
        } else {
            set_value(ctx, &arr_obj->u.array.u.values[idx], sp[-1]);
        }
        JS_FreeValue(ctx, sp[-3]);
        sp -= 3;
        TC_DISPATCH();
    }
put_array_el_slow_path:
    S->sf->cur_pc = pc;
    ret = JS_SetPropertyValue(ctx, sp[-3], sp[-2], sp[-1], JS_PROP_THROW_STRICT);
    JS_FreeValue(ctx, sp[-3]);
    sp -= 3;
    if (unlikely(ret < 0))
        TC_THROW();
    TC_DISPATCH();
}

TC_HANDLER(js_tc_put_array_el)
{
    JSValue obj = sp[-3], old;
    JSObject* p;
    uint32_t idx;
    if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT || JS_VALUE_GET_TAG(sp[-2]) != JS_TAG_INT))
        TC_JUMP(js_tc_put_array_el_slow);
    p = JS_VALUE_GET_OBJ(obj);
    idx = JS_VALUE_GET_INT(sp[-2]);
    if (unlikely(p->class_id != JS_CLASS_ARRAY || idx >= (uint32_t)p->u.array.count))
        TC_JUMP(js_tc_put_array_el_slow);
    old = p->u.array.u.values[idx];
    p->u.array.u.values[idx] = sp[-1];
    sp -= 3;
    pc += 1;
    TC_FREE2_DISPATCH(old, obj);
}

#define TC_GET_FIELD(name, keep, is_length, size)                                      \
    TC_HANDLER(name##_slow)                                                            \
    {                                                                                  \
        JSAtom atom = is_length ? JS_ATOM_length : get_u32(pc + 1);                    \
        JSValue val;                                                                   \
        pc += (size);                                                                  \
        S->sf->cur_pc = pc;                                                            \
        val = JS_GetPropertyInternal(ctx, S->rel, atom, sp[-1], 0);                    \
        if (unlikely(JS_IsException(val)))                                             \
            TC_THROW();                                                                \
        if (keep) {                                                                    \
            *sp++ = val;                                                               \
            TC_DISPATCH();                                                             \
        } else {                                                                       \
            JSValue old = sp[-1];                                                      \
            sp[-1] = val;                                                              \
            TC_FREE_DISPATCH(old);                                                     \
        }                                                                              \
    }                                                                                  \
    TC_HANDLER(name)                                                                   \
    {                                                                                  \
        JSAtom atom = is_length ? JS_ATOM_length : get_u32(pc + 1);                    \
        JSValue obj = sp[-1], val;                                                     \
        JSObject* p = js_fast_lookup_start(ctx, obj, atom);                              \
        if (likely(p != NULL)) {                          \
            JSProperty* pr;                                                            \
            JSShapeProperty* prs;                                                      \
            for (;;) {                                                                 \
                prs = find_own_property(&pr, p, atom);                                 \
                if (prs) {                                                             \
                    if (unlikely(prs->flags & JS_PROP_TMASK)) {                        \
                        S->rel = obj;                                                  \
                        TC_JUMP(name##_slow);                                          \
                    }                                                                  \
                    val = JS_DupValue(ctx, pr->u.value);                               \
                    break;                                                             \
                }                                                                      \
                if (unlikely(p->is_exotic)) {                                          \
                    if (is_length && p->class_id >= JS_CLASS_UINT8C_ARRAY              \
                        && p->class_id <= JS_CLASS_FLOAT64_ARRAY                       \
                        && p == JS_VALUE_GET_OBJ(obj) && js_ta_length_fast(p, &val))   \
                        break;                                                         \
                    if (p->class_id != JS_CLASS_ARRAY || __JS_AtomIsTaggedInt(atom)) { \
                        S->rel = JS_MKPTR(JS_TAG_OBJECT, p);                           \
                        TC_JUMP(name##_slow);                                          \
                    }                                                                  \
                }                                                                      \
                p = p->shape->proto;                                                   \
                if (!p) {                                                              \
                    val = JS_UNDEFINED;                                                \
                    break;                                                             \
                }                                                                      \
            }                                                                          \
        } else if (is_length && JS_VALUE_GET_TAG(obj) == JS_TAG_STRING) {              \
            val = JS_NewInt32(ctx, JS_VALUE_GET_STRING(obj)->len);                     \
        } else if (is_length && JS_VALUE_GET_TAG(obj) == JS_TAG_STRING_ROPE) {         \
            val = JS_NewInt32(ctx, JS_VALUE_GET_STRING_ROPE(obj)->len);                \
        } else {                                                                       \
            S->rel = obj;                                                              \
            TC_JUMP(name##_slow);                                                      \
        }                                                                              \
        pc += (size);                                                                  \
        if (keep) {                                                                    \
            *sp++ = val;                                                               \
            TC_DISPATCH();                                                             \
        }                                                                              \
        sp[-1] = val;                                                                  \
        TC_FREE_DISPATCH(obj);                                                         \
    }

TC_GET_FIELD(js_tc_get_field, 0, 0, 5)
TC_GET_FIELD(js_tc_get_field2, 1, 0, 5)
TC_GET_FIELD(js_tc_get_length, 0, 1, 1)

TC_HANDLER(js_tc_put_field_slow)
{
    JSValue obj = sp[-2];
    JSAtom atom = get_u32(pc + 1);
    int ret;
    pc += 5;
    S->sf->cur_pc = pc;
    ret = JS_SetPropertyInternal(ctx, obj, atom, sp[-1], obj, JS_PROP_THROW_STRICT);
    JS_FreeValue(ctx, obj);
    sp -= 2;
    if (unlikely(ret < 0))
        TC_THROW();
    TC_DISPATCH();
}

TC_HANDLER(js_tc_put_field)
{
    JSValue obj = sp[-2], old;
    JSProperty* pr;
    JSShapeProperty* prs;
    if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT))
        TC_JUMP(js_tc_put_field_slow);
    prs = find_own_property(&pr, JS_VALUE_GET_OBJ(obj), get_u32(pc + 1));
    if (!prs || unlikely((prs->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE | JS_PROP_LENGTH)) != JS_PROP_WRITABLE))
        TC_JUMP(js_tc_put_field_slow);
    old = pr->u.value;
    pr->u.value = sp[-1];
    sp -= 2;
    pc += 5;
    TC_FREE2_DISPATCH(old, obj);
}

TC_HANDLER(js_tc_get_var_slow)
{
    int idx = get_u16(pc + 1);
    int undef_flag = pc[0] - OP_get_var_undef;
    JSClosureVar* cv = &S->b->closure_var[idx];
    pc += 3;
    if (cv->is_lexical) {
        JS_ThrowReferenceErrorUninitialized(ctx, cv->var_name);
        TC_THROW();
    }
    S->sf->cur_pc = pc;
    sp[0] = JS_GetPropertyInternal(ctx, ctx->global_obj, cv->var_name, ctx->global_obj, undef_flag);
    if (JS_IsException(sp[0]))
        TC_THROW();
    sp++;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_get_var)
{
    JSValue val = *TC_VREF(get_u16(pc + 1));
    if (unlikely(JS_IsUninitialized(val)))
        TC_JUMP(js_tc_get_var_slow);
    *sp++ = JS_DupValue(ctx, val);
    TC_NEXT(3);
}

TC_HANDLER(js_tc_put_var_slow)
{
    int idx = get_u16(pc + 1), ret;
    BOOL is_init = pc[0] == OP_put_var_init;
    JSVarRef* var_ref = S->var_refs[idx];
    JSClosureVar* cv = &S->b->closure_var[idx];
    pc += 3;
    if (var_ref->is_lexical) {
        if (is_init) {
            set_value(ctx, var_ref->pvalue, sp[-1]);
            sp--;
            TC_DISPATCH();
        }
        if (JS_IsUninitialized(*var_ref->pvalue))
            JS_ThrowReferenceErrorUninitialized(ctx, cv->var_name);
        else
            JS_ThrowTypeErrorReadOnly(ctx, JS_PROP_THROW, cv->var_name);
        TC_THROW();
    }
    S->sf->cur_pc = pc;
    ret = JS_HasProperty(ctx, ctx->global_obj, cv->var_name);
    if (ret < 0)
        TC_THROW();
    if (ret == 0 && is_strict_mode(ctx)) {
        JS_ThrowReferenceErrorNotDefined(ctx, cv->var_name);
        TC_THROW();
    }
    ret = JS_SetPropertyInternal(ctx, ctx->global_obj, cv->var_name, sp[-1], ctx->global_obj, JS_PROP_THROW_STRICT);
    sp--;
    if (ret < 0)
        TC_THROW();
    TC_DISPATCH();
}

TC_HANDLER(js_tc_put_var)
{
    JSVarRef* var_ref = S->var_refs[get_u16(pc + 1)];
    JSValue old = *var_ref->pvalue;
    if (unlikely(JS_IsUninitialized(old) || var_ref->is_const))
        TC_JUMP(js_tc_put_var_slow);
    *var_ref->pvalue = *--sp;
    pc += 3;
    TC_FREE_DISPATCH(old);
}

TC_HANDLER(js_tc_fclosure)
{
    JSValue bfunc = JS_DupValue(ctx, S->cpool[(*pc == OP_fclosure8) ? pc[1] : get_u32(pc + 1)]);
    pc += (*pc == OP_fclosure8) ? 2 : 5;
    *sp++ = js_closure(ctx, bfunc, S->var_refs, S->sf, FALSE);
    if (unlikely(JS_IsException(sp[-1])))
        TC_THROW();
    TC_DISPATCH();
}

typedef struct JSTCVChunk {
    struct JSTCVChunk* next;
    uint8_t* prev_ptr;
    uint8_t* prev_end;
    uint8_t* prev_base;
    size_t prev_base_used;
    size_t size;
    JSValue data[];
} JSTCVChunk;

typedef struct JSTCFrame {
    JSTCState s;
    JSStackFrame sf;
} JSTCFrame;

#define JS_TC_VCHUNK_SIZE ((size_t)128 << 10)
#define JS_TC_VCHUNK(base) ((JSTCVChunk*)((uint8_t*)(base) - offsetof(JSTCVChunk, data)))
#define JS_TC_FRAME_HDR ((sizeof(JSTCFrame) + 15) & ~(size_t)15)
#define JS_TC_VBOTTOM_PAD ((size_t)16)

_Static_assert(offsetof(JSTCVChunk, data) % 16 == 0, "vstack chunk data is 16-byte aligned");
_Static_assert(offsetof(JSTCVChunk, next) == 0, "JS_TC_RT_FREE walks chunks through their first word");

static void js_tcv_clamp(JSRuntime* rt, JSTCVChunk* c)
{
    uint8_t* end = (uint8_t*)c->data + c->size;
    if (rt->stack_limit) {
        uintptr_t avail = js_get_stack_pointer() - rt->stack_limit;
        uintptr_t lim = avail > rt->vs_base_used ? avail - rt->vs_base_used : 0;
        if (lim < c->size)
            end = (uint8_t*)c->data + lim;
    }
    rt->vs_end = end;
}

static force_inline void js_tcv_entry_clamp(JSRuntime* rt)
{
    if ((uintptr_t)rt->vs_ptr - (uintptr_t)rt->vs_base == JS_TC_VBOTTOM_PAD)
        js_tcv_clamp(rt, JS_TC_VCHUNK(rt->vs_base));
}

static void js_tcv_free_from(JSRuntime* rt, JSTCVChunk* c)
{
    JSTCVChunk* n;
    for (; c; c = n) {
        n = c->next;
        js_free_rt(rt, c);
    }
}

static no_inline uint8_t* js_tcv_grow(JSContext* ctx, size_t n)
{
    JSRuntime* rt = ctx->rt;
    JSTCVChunk *cur, *c, **link;
    uint8_t* base;
    size_t need;
    if (js_check_stack_overflow(rt, n)) {
        JS_ThrowStackOverflow(ctx);
        return NULL;
    }
    cur = rt->vs_base ? JS_TC_VCHUNK(rt->vs_base) : NULL;
    if (cur && rt->vs_ptr + n <= (uint8_t*)cur->data + cur->size) {
        base = rt->vs_ptr;
        rt->vs_ptr = base + n;
        js_tcv_clamp(rt, cur);
        return base;
    }
    need = n + (cur ? 0 : JS_TC_VBOTTOM_PAD);
    link = cur ? &cur->next : (JSTCVChunk**)&rt->vs_chunks;
    c = *link;
    if (!c || c->size < need) {
        size_t size = need > JS_TC_VCHUNK_SIZE ? need : JS_TC_VCHUNK_SIZE;
        JSTCVChunk* nc = js_malloc(ctx, sizeof(JSTCVChunk) + size);
        if (!nc)
            return NULL;
        nc->size = size;
        nc->next = c;
        *link = nc;
        c = nc;
    }
    c->prev_ptr = rt->vs_ptr;
    c->prev_end = rt->vs_end;
    c->prev_base = rt->vs_base;
    c->prev_base_used = rt->vs_base_used;
    rt->vs_base_used += (size_t)(rt->vs_ptr - rt->vs_base);
    rt->vs_base = (uint8_t*)c->data;
    rt->vs_ptr = (uint8_t*)c->data + need;
    base = rt->vs_ptr - n;
    js_tcv_clamp(rt, c);
    return base;
}

static no_inline void js_tcv_pop_chunk(JSRuntime* rt)
{
    JSTCVChunk* c = JS_TC_VCHUNK(rt->vs_base);
    rt->vs_ptr = c->prev_ptr;
    rt->vs_end = c->prev_end;
    rt->vs_base = c->prev_base;
    rt->vs_base_used = c->prev_base_used;
    js_tcv_free_from(rt, c->next);
    c->next = NULL;
}

static force_inline uint8_t* js_tcv_push(JSContext* ctx, size_t n)
{
    JSRuntime* rt = ctx->rt;
    uint8_t* base = rt->vs_ptr;
    if (likely(n <= (size_t)((uintptr_t)rt->vs_end - (uintptr_t)base))) {
        rt->vs_ptr = base + n;
        return base;
    }
    return js_tcv_grow(ctx, n);
}

static force_inline size_t js_tcv_frame_size(JSFunctionBytecode* nb, int nargs)
{
    return (JS_TC_FRAME_HDR + sizeof(JSValue) * (nargs + nb->var_count + nb->stack_size) + sizeof(JSVarRef*) * nb->var_ref_count + 15) & ~(size_t)15;
}

static force_inline JSTCState* js_tcv_frame_init(JSRuntime* rt, JSContext* ctx, uint8_t* base, int flags, JSTCState* up, JSValue f, JSValue th,
    const JSValue* args, int argc, int nargs, size_t capacity)
{
    JSTCFrame* fr = (JSTCFrame*)base;
    JSTCState* T = &fr->s;
    JSStackFrame* sf = &fr->sf;
    JSObject* fp = JS_VALUE_GET_OBJ(f);
    JSFunctionBytecode* nb = fp->u.func.function_bytecode;
    JSValue* lbuf = (JSValue*)(base + JS_TC_FRAME_HDR);
    JSValue *vbuf, *sbuf;
    JSVarRef** vr;
    int i;
    if (likely(argc <= 3)) {
        if (argc > 0)
            lbuf[0] = args[0];
        if (argc > 1)
            lbuf[1] = args[1];
        if (argc > 2)
            lbuf[2] = args[2];
    } else {
        memmove(lbuf, args, sizeof(JSValue) * argc);
    }
    for (i = argc; i < nargs; i++)
        lbuf[i] = JS_UNDEFINED;
    vbuf = lbuf + nargs;
    for (i = 0; i < nb->var_count; i++)
        vbuf[i] = JS_UNDEFINED;
    sbuf = vbuf + nb->var_count;
    vr = (JSVarRef**)(sbuf + nb->stack_size);
    for (i = 0; i < nb->var_ref_count; i++)
        vr[i] = NULL;
    T->up = up;
    T->fr_flags = flags;
    T->own_func = f;
    T->own_this = th;
    T->sf = sf;
    T->b = nb;
    T->cpool = nb->cpool;
    T->var_refs = fp->u.func.var_refs;
    T->this_obj = th;
    T->caller_ctx = ctx;
    T->new_target = JS_UNDEFINED;
    T->argc = argc;
    T->argv = lbuf;
    T->local_buf = lbuf;
    T->stack_buf = sbuf;
    T->p = fp;
    T->alloca_capacity = capacity;
#if CONFIG_LITERAL_BOILERPLATE
    T->bb_rec_obj = NULL;
    T->bb_rec_site = NULL;
#endif
    sf->js_mode = nb->js_mode;
    sf->arg_count = nargs;
    sf->cur_func = f;
    sf->arg_buf = lbuf;
    sf->var_buf = vbuf;
    sf->var_refs = vr;
    sf->prev_frame = rt->current_stack_frame;
    rt->current_stack_frame = sf;
    return T;
}

static force_inline void js_tcv_frame_drop(JSRuntime* rt, JSContext* ctx, JSTCState* T, JSValue* sp)
{
    JSValue* pv;
    if (unlikely(T->b->var_ref_count != 0))
        close_var_refs(rt, T->b, T->sf);
    for (pv = T->local_buf; pv < sp; pv++)
        JS_FreeValue(ctx, *pv);
    rt->current_stack_frame = T->sf->prev_frame;
    JS_FreeValue(ctx, T->own_func);
    JS_FreeValue(ctx, T->own_this);
    if (unlikely(T->fr_flags)) {
        if (T->fr_flags & TC_FR_NT)
            JS_FreeValue(ctx, T->own_nt);
        if (T->fr_flags & TC_FR_CTOR)
            JS_FreeValue(ctx, T->ctor_obj);
        if (T->fr_flags & TC_FR_KEEP)
            return;
    }
    if (unlikely((uint8_t*)T == rt->vs_base))
        js_tcv_pop_chunk(rt);
    else
        rt->vs_ptr = (uint8_t*)T;
}

static no_inline JSTCState* js_tcv_push_frame(JSContext* ctx, JSTCState* S, JSValue f, JSValue th, const JSValue* args, int argc, int flags)
{
    JSFunctionBytecode* nb = JS_VALUE_GET_OBJ(f)->u.func.function_bytecode;
    int nargs = argc < nb->arg_count ? nb->arg_count : argc;
    uint8_t* base;
    if (unlikely(--ctx->interrupt_counter <= 0) && __js_poll_interrupts(ctx))
        return NULL;
    base = js_tcv_push(ctx, js_tcv_frame_size(nb, nargs));
    if (unlikely(!base))
        return NULL;
    return js_tcv_frame_init(ctx->rt, ctx, base, flags, S, f, th, args, argc, nargs,
        sizeof(JSValue) * ((argc < nb->arg_count ? nb->arg_count : 0) + nb->var_count + nb->stack_size) + sizeof(JSVarRef*) * nb->var_ref_count);
}

#define TC_RESUME_CALLER()   \
    do {                     \
        S = S->up;           \
        pc = S->pc;          \
        sp = S->sp;          \
        lb = S->sf->var_buf; \
        ab = S->sf->arg_buf; \
        ctx = S->b->realm;   \
    } while (0)

#define TC_INLINE_ENTER(T)        \
    do {                          \
        S = (T);                  \
        ctx = S->b->realm;        \
        pc = S->b->byte_code_buf; \
        sp = S->stack_buf;        \
        lb = S->sf->var_buf;      \
        ab = S->sf->arg_buf;      \
        TC_DISPATCH();            \
    } while (0)

static force_inline BOOL js_tc_inlinable(JSValue f)
{
    JSObject* fp;
    return JS_VALUE_GET_TAG(f) == JS_TAG_OBJECT && (fp = JS_VALUE_GET_OBJ(f))->class_id == JS_CLASS_BYTECODE_FUNCTION
        && fp->u.func.function_bytecode->func_kind == JS_FUNC_NORMAL;
}

TC_HANDLER(js_tc_leave)
{
    JSValue ret = S->ret;
    if (unlikely(S->fr_flags & TC_FR_CTOR) && JS_VALUE_GET_TAG(ret) != JS_TAG_OBJECT) {
        JS_FreeValue(ctx, ret);
        ret = JS_DupValue(ctx, S->ctor_obj);
#if CONFIG_PRESIZE_CTOR
        if (unlikely(S->b->ctor_presize != NULL) && !S->b->ctor_presize->learned)
            js_ctor_presize_learn(ctx, S->b->ctor_presize, JS_VALUE_GET_OBJ(ret));
#endif
    }
    js_tcv_frame_drop(ctx->rt, ctx, S, sp);
    TC_RESUME_CALLER();
    *sp++ = ret;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_call_inline)
{
    int op = *pc, argc, nthis, nargs;
    JSValue *call_argv, th;
    JSFunctionBytecode* nb;
    uint8_t* base;
    size_t n;
    if (op >= OP_call0 && op <= OP_call3) {
        argc = op - OP_call0;
        nthis = 1;
        pc += 1;
    } else {
        argc = get_u16(pc + 1);
        nthis = op == OP_call_method ? 2 : 1;
        pc += 3;
    }
    S->sf->cur_pc = pc;
    if (unlikely(--ctx->interrupt_counter <= 0) && __js_poll_interrupts(ctx))
        TC_THROW();
    call_argv = sp - argc;
    nb = JS_VALUE_GET_OBJ(call_argv[-1])->u.func.function_bytecode;
    nargs = argc < nb->arg_count ? nb->arg_count : argc;
    n = js_tcv_frame_size(nb, nargs);
    base = js_tcv_push(ctx, n);
    if (unlikely(!base))
        TC_THROW();
    th = nthis == 2 ? call_argv[-2] : JS_UNDEFINED;
    S->pc = pc;
    S->sp = call_argv - nthis;
    TC_INLINE_ENTER(js_tcv_frame_init(ctx->rt, ctx, base, 0, S, call_argv[-1], th, call_argv, argc, nargs,
        sizeof(JSValue) * ((argc < nb->arg_count ? nb->arg_count : 0) + nb->var_count + nb->stack_size) + sizeof(JSVarRef*) * nb->var_ref_count));
}

TC_HANDLER(js_tc_tail_inline)
{
    JSRuntime* rt = ctx->rt;
    int nthis = pc[-3] == OP_tail_call_method ? 2 : 1, argc = get_u16(pc - 2), nargs;
    JSValue *call_argv = sp - argc, f, th, *pv;
    JSTCState* up = S->up;
    size_t capacity = S->alloca_capacity;
    int carry = S->fr_flags & (TC_FR_NT | TC_FR_CTOR | TC_FR_ENTRY);
    JSValue own_nt = S->own_nt, ctor_obj = S->ctor_obj;
    JSFunctionBytecode* nb;
    uint8_t* base;
    if (unlikely(--ctx->interrupt_counter <= 0) && __js_poll_interrupts(ctx))
        TC_THROW();
    f = call_argv[-1];
    th = nthis == 2 ? call_argv[-2] : JS_UNDEFINED;
    if (unlikely(S->b->var_ref_count != 0))
        close_var_refs(rt, S->b, S->sf);
    for (pv = S->local_buf; pv < call_argv - nthis; pv++)
        JS_FreeValue(ctx, *pv);
    rt->current_stack_frame = S->sf->prev_frame;
    JS_FreeValue(ctx, S->own_func);
    JS_FreeValue(ctx, S->own_this);
    if (unlikely((uint8_t*)S == rt->vs_base))
        js_tcv_pop_chunk(rt);
    else
        rt->vs_ptr = (uint8_t*)S;
    nb = JS_VALUE_GET_OBJ(f)->u.func.function_bytecode;
    nargs = argc < nb->arg_count ? nb->arg_count : argc;
    base = js_tcv_push(ctx, js_tcv_frame_size(nb, nargs));
    if (unlikely(!base)) {
        for (pv = call_argv; pv < call_argv + argc; pv++)
            JS_FreeValue(ctx, *pv);
        JS_FreeValue(ctx, f);
        JS_FreeValue(ctx, th);
        if (carry & TC_FR_NT)
            JS_FreeValue(ctx, own_nt);
        if (carry & TC_FR_CTOR)
            JS_FreeValue(ctx, ctor_obj);
        if (!up)
            return 1;
        S = up;
        pc = S->pc;
        sp = S->sp;
        lb = S->sf->var_buf;
        ab = S->sf->arg_buf;
        ctx = S->b->realm;
        TC_THROW();
    }
    {
        JSTCState* T = js_tcv_frame_init(rt, ctx, base, carry, up, f, th, call_argv, argc, nargs, capacity);
        T->own_nt = own_nt;
        T->ctor_obj = ctor_obj;
        TC_INLINE_ENTER(T);
    }
}

TC_HANDLER(js_tc_throw_inline)
{
    JSRuntime* rt = ctx->rt;
#if CONFIG_LITERAL_BOILERPLATE
    TC_BB_REC_CLEAR();
#endif
    if (unlikely(JS_IsUninitialized(rt->current_exception)))
        js_exception_lost(ctx);
    if (is_backtrace_needed(ctx, rt->current_exception)) {
        S->sf->cur_pc = pc;
        build_backtrace(ctx, rt->current_exception, NULL, 0, 0, 0);
    }
    if (!rt->current_exception_is_uncatchable) {
        while (sp > S->stack_buf) {
            JSValue val = *--sp;
            JS_FreeValue(ctx, val);
            if (JS_VALUE_GET_TAG(val) == JS_TAG_CATCH_OFFSET) {
                int pos = JS_VALUE_GET_INT(val);
                if (pos == 0) {
                    JSValue next = sp[-1];
                    sp--;
                    js_for_of_close(ctx, sp[-1], next, TRUE);
                    JS_FreeValue(ctx, next);
                } else if (pos < 0) {
                    if (sp > S->stack_buf) {
                        JS_FreeValue(ctx, sp[-1]);
                        sp--;
                    }
                    if (sp > S->stack_buf) {
                        JS_FreeValue(ctx, sp[-1]);
                        sp--;
                    }
                } else if (pos < S->b->byte_code_len) {
                    *sp++ = rt->current_exception;
                    rt->current_exception = JS_UNINITIALIZED;
                    S->sf->js_mode = (S->sf->js_mode & ~JS_MODE_STRICT) | (S->b->js_mode & JS_MODE_STRICT);
                    pc = S->b->byte_code_buf + pos;
                    TC_DISPATCH();
                }
            }
        }
    }
    if (!S->up) {
        js_tcv_frame_drop(rt, ctx, S, sp);
        return 1;
    }
    js_tcv_frame_drop(rt, ctx, S, sp);
    TC_RESUME_CALLER();
    TC_THROW();
}

TC_HANDLER(js_tc_exit_keep)
{
    JSRuntime* rt = ctx->rt;
    JSValue* pv;
    if (unlikely(S->b->var_ref_count != 0))
        close_var_refs(rt, S->b, S->sf);
    for (pv = S->local_buf; pv < sp; pv++)
        JS_FreeValue(ctx, *pv);
    rt->current_stack_frame = S->sf->prev_frame;
    rt->vs_ret = S->ret;
    return 2;
}

TC_HANDLER(js_tc_exit_entry)
{
    JSValue ret = S->ret;
    if (S->fr_flags & TC_FR_KEEP)
        TC_JUMP(js_tc_exit_keep);
    js_tcv_frame_drop(ctx->rt, ctx, S, sp);
    ctx->rt->vs_ret = ret;
    return 2;
}

static no_inline int js_tc_enter(JSTCState* S, const uint8_t* pc, JSValue* sp, JSValue* lb, JSValue* ab, JSContext* ctx);

static no_inline JSValue js_call_tc_entry(JSContext* caller_ctx, JSValueConst func_obj, JSValueConst this_obj, JSValueConst new_target,
    int argc, JSValue* argv, int flags)
{
    JSRuntime* rt = caller_ctx->rt;
    JSFunctionBytecode* nb = JS_VALUE_GET_OBJ(func_obj)->u.func.function_bytecode;
    int nargs = argc < nb->arg_count ? nb->arg_count : argc, i;
    int arg_alloc = (argc < nb->arg_count || (flags & JS_CALL_FLAG_COPY_ARGV)) ? nb->arg_count : 0;
    size_t cap = sizeof(JSValue) * (arg_alloc + nb->var_count + nb->stack_size) + sizeof(JSVarRef*) * nb->var_ref_count;
    uint8_t* base;
    JSTCState* T;
    if (js_check_stack_overflow(rt, cap))
        return JS_ThrowStackOverflow(caller_ctx);
    js_tcv_entry_clamp(rt);
    base = js_tcv_push(caller_ctx, js_tcv_frame_size(nb, nargs));
    if (unlikely(!base))
        return JS_EXCEPTION;
    T = js_tcv_frame_init(rt, caller_ctx, base, TC_FR_ENTRY, NULL, func_obj, this_obj, argv, argc, nargs, cap);
    for (i = 0; i < argc; i++)
        T->argv[i] = JS_DupValue(caller_ctx, T->argv[i]);
    T->own_func = JS_UNDEFINED;
    T->own_this = JS_UNDEFINED;
    T->new_target = new_target;
    if (js_tc_enter(T, nb->byte_code_buf, T->stack_buf, T->sf->var_buf, T->sf->arg_buf, nb->realm) == 2)
        return rt->vs_ret;
    return JS_EXCEPTION;
}

int js_call_prep_begin(JSContext* ctx, JSCallPrep* cp, JSValueConst func, JSValueConst this_obj, int argc)
{
    JSRuntime* rt = ctx->rt;
    JSFunctionBytecode* nb;
    uint8_t* base;
    size_t cap;
    cp->fr = NULL;
    cp->func = func;
    cp->this_obj = this_obj;
    cp->argc = argc;
    if (!js_tc_inlinable(func))
        return 0;
    nb = JS_VALUE_GET_OBJ(func)->u.func.function_bytecode;
    cp->nargs = argc < nb->arg_count ? nb->arg_count : argc;
    cap = sizeof(JSValue) * ((argc < nb->arg_count ? nb->arg_count : 0) + nb->var_count + nb->stack_size) + sizeof(JSVarRef*) * nb->var_ref_count;
    if (js_check_stack_overflow(rt, cap))
        return 0;
    js_tcv_entry_clamp(rt);
    base = js_tcv_push(ctx, js_tcv_frame_size(nb, cp->nargs));
    if (unlikely(!base))
        return -1;
    js_tcv_frame_init(rt, ctx, base, TC_FR_ENTRY | TC_FR_KEEP, NULL, func, this_obj, NULL, 0, cp->nargs, cap);
    rt->current_stack_frame = ((JSTCFrame*)base)->sf.prev_frame;
    ((JSTCFrame*)base)->s.own_func = JS_UNDEFINED;
    ((JSTCFrame*)base)->s.own_this = JS_UNDEFINED;
    ((JSTCFrame*)base)->s.argc = argc;
    cp->fr = base;
    return 0;
}

static force_inline JSValue js_call_prep_core(JSContext* ctx, JSCallPrep* cp, JSValue* argv, BOOL dup)
{
    JSRuntime* rt = ctx->rt;
    JSTCState* T;
    JSFunctionBytecode* nb;
    JSValue* lb;
    int i, n;
    if (unlikely(!cp->fr))
        return dup ? JS_Call(ctx, cp->func, cp->this_obj, cp->argc, argv) : js_call_owned(ctx, cp->func, cp->this_obj, cp->argc, argv);
    if (js_poll_interrupts(ctx)) {
        if (!dup) {
            for (i = 0; i < cp->argc; i++)
                JS_FreeValue(ctx, argv[i]);
        }
        return JS_EXCEPTION;
    }
    T = &((JSTCFrame*)cp->fr)->s;
    nb = T->b;
    lb = T->local_buf;
    for (i = 0; i < cp->argc; i++)
        lb[i] = dup ? JS_DupValue(ctx, argv[i]) : argv[i];
    n = cp->nargs + nb->var_count;
    for (; i < n; i++)
        lb[i] = JS_UNDEFINED;
    if (unlikely(nb->var_ref_count))
        memset(T->sf->var_refs, 0, sizeof(JSVarRef*) * nb->var_ref_count);
#if CONFIG_LITERAL_BOILERPLATE
    T->bb_rec_obj = NULL;
    T->bb_rec_site = NULL;
#endif
    T->sf->prev_frame = rt->current_stack_frame;
    rt->current_stack_frame = T->sf;
    if (js_tc_table[*nb->byte_code_buf].h(T, nb->byte_code_buf, T->stack_buf, T->sf->var_buf, T->sf->arg_buf, nb->realm, js_tc_table) == 2)
        return rt->vs_ret;
    return JS_EXCEPTION;
}

JSValue js_call_prep(JSContext* ctx, JSCallPrep* cp, JSValue* argv)
{
    return js_call_prep_core(ctx, cp, argv, FALSE);
}

JSValue js_call_prep_dup(JSContext* ctx, JSCallPrep* cp, JSValueConst* argv)
{
    return js_call_prep_core(ctx, cp, (JSValue*)argv, TRUE);
}

void js_call_prep_end(JSContext* ctx, JSCallPrep* cp)
{
    JSRuntime* rt = ctx->rt;
    uint8_t* base = cp->fr;
    if (!base)
        return;
    cp->fr = NULL;
    if (unlikely(base == rt->vs_base))
        js_tcv_pop_chunk(rt);
    else
        rt->vs_ptr = base;
}

TC_HANDLER(js_tc_iter_call);

#define TC_CALL(name, argc_expr, size, nthis)                                                     \
    TC_HANDLER(name)                                                                              \
    {                                                                                             \
        int call_argc = (argc_expr), i;                                                           \
        JSValue* call_argv = sp - call_argc;                                                      \
        JSValue ret;                                                                              \
        if (js_tc_inlinable(call_argv[-1]))                                                       \
            TC_JUMP(js_tc_call_inline);                                                           \
        if ((nthis) == 2 && JS_VALUE_GET_TAG(call_argv[-1]) == JS_TAG_OBJECT                      \
            && JS_VALUE_GET_OBJ(call_argv[-1])->class_id == JS_CLASS_C_FUNCTION                   \
            && unlikely(JS_VALUE_GET_OBJ(call_argv[-1])->u.cfunc.tc_iter))                        \
            TC_JUMP(js_tc_iter_call);                                                             \
        pc += (size);                                                                             \
        S->sf->cur_pc = pc;                                                                       \
        if (!js_call_numeric_cfunc(ctx, call_argv[-1], call_argc, call_argv, &ret))               \
            ret = js_call_dispatch(ctx, call_argv[-1], nthis == 2 ? call_argv[-2] : JS_UNDEFINED, \
                JS_UNDEFINED, call_argc, call_argv, 0);                                           \
        if (unlikely(JS_IsException(ret)))                                                        \
            TC_THROW();                                                                           \
        for (i = -(nthis); i < call_argc; i++)                                                    \
            JS_FreeValue(ctx, call_argv[i]);                                                      \
        sp -= call_argc + (nthis);                                                                \
        *sp++ = ret;                                                                              \
        TC_DISPATCH();                                                                            \
    }

TC_CALL(js_tc_call_small, *pc - OP_call0, 1, 1)
TC_CALL(js_tc_call, get_u16(pc + 1), 3, 1)
TC_CALL(js_tc_call_method, get_u16(pc + 1), 3, 2)

#define TC_OP_ITER 255
_Static_assert(OP_COUNT <= TC_OP_ITER, "the array-iteration trampoline byte must not collide with a real opcode");

typedef struct JSTCIter {
    int64_t k;
    int64_t len;
    int64_t n;
    int kind;
} JSTCIter;

#define JS_TCI_NV 4
#define JS_TCI_HDR (JS_TC_FRAME_HDR + ((sizeof(JSTCIter) + 15) & ~(size_t)15))
#define JS_TCI_SIZE ((JS_TCI_HDR + sizeof(JSValue) * (JS_TCI_NV + 1) + 15) & ~(size_t)15)
#define TC_ITER(T) ((JSTCIter*)((uint8_t*)(T) + JS_TC_FRAME_HDR))

static const uint8_t js_tc_iter_code[1] = { TC_OP_ITER };

static no_inline JSFunctionBytecode* js_tcv_iter_bc(JSContext* ctx)
{
    JSFunctionBytecode* b = js_mallocz(ctx, sizeof(JSFunctionBytecode));
    if (!b)
        return NULL;
    b->realm = ctx;
    b->func_kind = JS_FUNC_NORMAL;
    b->read_only_bytecode = 1;
    b->byte_code_buf = DYN_UNCONST(js_tc_iter_code);
    b->byte_code_len = 1;
    ctx->tc_iter_b = b;
    return b;
}

static force_inline BOOL js_tc_iter_args_ok(JSValueConst th, int argc, const JSValue* argv)
{
    return argc >= 1 && JS_VALUE_GET_TAG(th) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(th)->class_id == JS_CLASS_ARRAY && js_tc_inlinable(argv[0]);
}

static no_inline JSTCState* js_tcv_iter_push(JSContext* caller_ctx, JSTCState* S, JSValue* call_argv, int argc)
{
    JSObject* cf = JS_VALUE_GET_OBJ(call_argv[-1]);
    JSContext* ctx = cf->u.cfunc.realm;
    JSRuntime* rt = ctx->rt;
    JSFunctionBytecode* ib = ctx->tc_iter_b;
    JSValue obj = call_argv[-2], acc = JS_UNDEFINED;
    JSTCFrame* fr;
    JSTCState* T;
    JSTCIter* it;
    JSStackFrame* sf;
    JSValue* lbuf;
    uint8_t* base;
    int64_t len;
    int i;
    if (js_poll_interrupts(caller_ctx))
        return NULL;
    if (unlikely(!ib)) {
        ib = js_tcv_iter_bc(ctx);
        if (!ib)
            return NULL;
    }
    base = js_tcv_push(ctx, JS_TCI_SIZE);
    if (unlikely(!base))
        return NULL;
    fr = (JSTCFrame*)base;
    T = &fr->s;
    sf = &fr->sf;
    it = TC_ITER(T);
    lbuf = (JSValue*)(base + JS_TCI_HDR);
    for (i = 0; i < JS_TCI_NV; i++)
        lbuf[i] = JS_UNDEFINED;
    T->up = S;
    T->fr_flags = 0;
    T->own_func = JS_UNDEFINED;
    T->own_this = JS_UNDEFINED;
    T->sf = sf;
    T->b = ib;
    T->cpool = NULL;
    T->var_refs = NULL;
    T->this_obj = obj;
    T->caller_ctx = caller_ctx;
    T->new_target = JS_UNDEFINED;
    T->argc = 0;
    T->argv = lbuf;
    T->local_buf = lbuf;
    T->stack_buf = lbuf + JS_TCI_NV;
    T->p = cf;
    T->alloca_capacity = 0;
#if CONFIG_LITERAL_BOILERPLATE
    T->bb_rec_obj = NULL;
    T->bb_rec_site = NULL;
#endif
    sf->js_mode = 0;
    sf->arg_count = 0;
    sf->cur_func = call_argv[-1];
    sf->arg_buf = lbuf;
    sf->var_buf = lbuf;
    sf->borrow_epoch = 0;
    sf->cur_pc = ib->byte_code_buf;
    sf->cur_sp = NULL;
    sf->prev_frame = rt->current_stack_frame;
    rt->current_stack_frame = sf;
    it->kind = cf->u.cfunc.tc_iter;
    it->k = 0;
    it->n = 0;
    if (js_get_length64(ctx, &len, obj))
        goto fail;
    it->len = len;
    switch (it->kind) {
    case JS_TCI_MAP:
        lbuf[2] = JS_ArraySpeciesCreate(ctx, obj, len);
        if (JS_IsException(lbuf[2]))
            goto fail;
        break;
    case JS_TCI_FILTER:
        lbuf[2] = JS_ArraySpeciesCreate(ctx, obj, 0);
        if (JS_IsException(lbuf[2]))
            goto fail;
        break;
    case JS_TCI_REDUCE:
        if (argc > 1) {
            acc = JS_DupValue(ctx, call_argv[1]);
            break;
        }
        for (;;) {
            int present;
            if (it->k >= len) {
                JS_ThrowTypeError(ctx, "empty array");
                goto fail;
            }
            present = JS_TryGetPropertyInt64(ctx, obj, it->k++, &acc);
            if (present < 0)
                goto fail;
            if (present)
                break;
        }
        break;
    default:
        break;
    }
    T->own_func = call_argv[-1];
    T->own_this = obj;
    lbuf[0] = call_argv[0];
    if (it->kind == JS_TCI_REDUCE) {
        lbuf[1] = acc;
        for (i = 1; i < argc; i++)
            JS_FreeValue(ctx, call_argv[i]);
    } else {
        lbuf[1] = argc > 1 ? call_argv[1] : JS_UNDEFINED;
        for (i = 2; i < argc; i++)
            JS_FreeValue(ctx, call_argv[i]);
    }
    return T;
fail:
    if (JS_IsException(lbuf[2]))
        lbuf[2] = JS_UNDEFINED;
    js_tcv_frame_drop(rt, ctx, T, T->stack_buf);
    return NULL;
}

TC_HANDLER(js_tc_iter_step);

#define TC_ITER_ENTER(T)          \
    do {                          \
        S = (T);                  \
        ctx = S->b->realm;        \
        pc = S->b->byte_code_buf; \
        sp = S->stack_buf;        \
        lb = S->sf->var_buf;      \
        ab = S->sf->arg_buf;      \
        TC_JUMP(js_tc_iter_step); \
    } while (0)

#define TC_ITER_CALL_BODY(call_argc)                                                                        \
    {                                                                                                       \
        JSValue* call_argv = sp - (call_argc);                                                              \
        JSValue ret;                                                                                        \
        int i;                                                                                              \
        S->sf->cur_pc = pc;                                                                                 \
        if (js_tc_iter_args_ok(call_argv[-2], (call_argc), call_argv)) {                                    \
            JSTCState* T = js_tcv_iter_push(ctx, S, call_argv, (call_argc));                                \
            if (unlikely(!T))                                                                               \
                TC_THROW();                                                                                 \
            S->pc = pc;                                                                                     \
            S->sp = call_argv - 2;                                                                          \
            TC_ITER_ENTER(T);                                                                               \
        }                                                                                                   \
        ret = js_call_dispatch(ctx, call_argv[-1], call_argv[-2], JS_UNDEFINED, (call_argc), call_argv, 0); \
        if (unlikely(JS_IsException(ret)))                                                                  \
            TC_THROW();                                                                                     \
        for (i = -2; i < (call_argc); i++)                                                                  \
            JS_FreeValue(ctx, call_argv[i]);                                                                \
        sp -= (call_argc) + 2;                                                                              \
        *sp++ = ret;                                                                                        \
        TC_DISPATCH();                                                                                      \
    }

TC_HANDLER(js_tc_iter_call)
{
    int call_argc = get_u16(pc + 1);
    pc += 3;
    TC_ITER_CALL_BODY(call_argc)
}

TC_HANDLER(js_tc_iter_done)
{
    JSTCIter* it = TC_ITER(S);
    JSValue ret;
    switch (it->kind) {
    case JS_TCI_EVERY:
        ret = JS_TRUE;
        break;
    case JS_TCI_SOME:
        ret = JS_FALSE;
        break;
    case JS_TCI_FINDINDEX:
        ret = JS_NewInt32(ctx, -1);
        break;
    case JS_TCI_MAP:
    case JS_TCI_FILTER:
        ret = lb[2];
        lb[2] = JS_UNDEFINED;
        break;
    case JS_TCI_REDUCE:
        ret = lb[1];
        lb[1] = JS_UNDEFINED;
        break;
    default:
        ret = JS_UNDEFINED;
        break;
    }
    S->ret = ret;
    TC_JUMP(js_tc_leave);
}

TC_HANDLER(js_tc_iter_step)
{
    JSTCIter* it = TC_ITER(S);
    JSValue obj = S->own_this, f = lb[0], val, th, a[4];
    JSObject* p = JS_VALUE_GET_OBJ(obj);
    JSFunctionBytecode* nb;
    int64_t k = it->k;
    int kind = it->kind, argc, nargs;
    uint8_t* base;
    for (;;) {
        int present;
        if (k >= it->len)
            TC_JUMP(js_tc_iter_done);
        if (likely((uint64_t)k < p->u.array.count)) {
            val = JS_DupValue(ctx, p->u.array.u.values[k]);
            break;
        }
        it->k = k;
        if (kind == JS_TCI_FIND || kind == JS_TCI_FINDINDEX) {
            val = JS_GetPropertyInt64(ctx, obj, k);
            if (unlikely(JS_IsException(val)))
                TC_THROW();
            break;
        }
        present = JS_TryGetPropertyInt64(ctx, obj, k, &val);
        if (unlikely(present < 0))
            TC_THROW();
        if (present)
            break;
        k++;
    }
    it->k = k;
    if (kind == JS_TCI_FILTER || kind == JS_TCI_FIND)
        lb[3] = JS_DupValue(ctx, val);
    if (kind == JS_TCI_REDUCE) {
        a[0] = JS_DupValue(ctx, lb[1]);
        a[1] = val;
        a[2] = JS_NewInt64(ctx, k);
        a[3] = JS_DupValue(ctx, obj);
        argc = 4;
        th = JS_UNDEFINED;
    } else {
        a[0] = val;
        a[1] = JS_NewInt64(ctx, k);
        a[2] = JS_DupValue(ctx, obj);
        argc = 3;
        th = JS_DupValue(ctx, lb[1]);
    }
    nb = JS_VALUE_GET_OBJ(f)->u.func.function_bytecode;
    nargs = argc < nb->arg_count ? nb->arg_count : argc;
    base = NULL;
    if (likely(--ctx->interrupt_counter > 0) || !__js_poll_interrupts(ctx))
        base = js_tcv_push(ctx, js_tcv_frame_size(nb, nargs));
    if (unlikely(!base)) {
        int i;
        for (i = 0; i < argc; i++)
            JS_FreeValue(ctx, a[i]);
        JS_FreeValue(ctx, th);
        TC_THROW();
    }
    S->pc = pc;
    S->sp = sp;
    TC_INLINE_ENTER(js_tcv_frame_init(ctx->rt, ctx, base, 0, S, JS_DupValue(ctx, f), th, a, argc, nargs,
        sizeof(JSValue) * ((argc < nb->arg_count ? nb->arg_count : 0) + nb->var_count + nb->stack_size) + sizeof(JSVarRef*) * nb->var_ref_count));
}

TC_HANDLER(js_tc_iter_next)
{
    JSTCIter* it = TC_ITER(S);
    JSValue res = *--sp, v;
    switch (it->kind) {
    case JS_TCI_EVERY:
        if (!JS_ToBoolFree(ctx, res)) {
            S->ret = JS_FALSE;
            TC_JUMP(js_tc_leave);
        }
        break;
    case JS_TCI_SOME:
        if (JS_ToBoolFree(ctx, res)) {
            S->ret = JS_TRUE;
            TC_JUMP(js_tc_leave);
        }
        break;
    case JS_TCI_MAP:
        if (JS_DefinePropertyValueInt64(ctx, lb[2], it->k, res, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            TC_THROW();
        break;
    case JS_TCI_FILTER:
        v = lb[3];
        lb[3] = JS_UNDEFINED;
        if (JS_ToBoolFree(ctx, res)) {
            if (JS_DefinePropertyValueInt64(ctx, lb[2], it->n++, v, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                TC_THROW();
        } else {
            JS_FreeValue(ctx, v);
        }
        break;
    case JS_TCI_FIND:
        v = lb[3];
        lb[3] = JS_UNDEFINED;
        if (JS_ToBoolFree(ctx, res)) {
            S->ret = v;
            TC_JUMP(js_tc_leave);
        }
        JS_FreeValue(ctx, v);
        break;
    case JS_TCI_FINDINDEX:
        if (JS_ToBoolFree(ctx, res)) {
            S->ret = JS_NewInt64(ctx, it->k);
            TC_JUMP(js_tc_leave);
        }
        break;
    case JS_TCI_REDUCE:
        JS_FreeValue(ctx, lb[1]);
        lb[1] = res;
        break;
    default:
        JS_FreeValue(ctx, res);
        break;
    }
    it->k++;
    TC_JUMP(js_tc_iter_step);
}

static no_inline JSTCState* js_tcv_push_ctor(JSContext* ctx, JSTCState* S, JSValue* call_argv, int argc)
{
    JSValue f = call_argv[-2], nt = call_argv[-1], obj = JS_UNDEFINED;
    JSFunctionBytecode* nb = JS_VALUE_GET_OBJ(f)->u.func.function_bytecode;
    JSTCState* T;
    if (!nb->is_derived_class_constructor) {
#if CONFIG_PRESIZE_CTOR
        if (nb->ctor_presize) {
            JSValue proto = js_resolve_ctor_proto(ctx, nt, JS_CLASS_OBJECT);
            if (JS_IsException(proto))
                return NULL;
            obj = js_ctor_presize_new_this(ctx, nb->ctor_presize, proto);
        } else
#endif
            obj = js_create_from_ctor(ctx, nt, JS_CLASS_OBJECT);
        if (JS_IsException(obj))
            return NULL;
    }
    T = js_tcv_push_frame(ctx, S, f, obj, call_argv, argc, nb->is_derived_class_constructor ? TC_FR_NT : TC_FR_NT | TC_FR_CTOR);
    if (unlikely(!T)) {
        JS_FreeValue(ctx, obj);
        return NULL;
    }
    T->own_this = JS_UNDEFINED;
    T->ctor_obj = obj;
    T->own_nt = nt;
    T->new_target = nt;
    return T;
}

TC_HANDLER(js_tc_call_constructor)
{
    int call_argc = get_u16(pc + 1), i;
    JSValue* call_argv = sp - call_argc;
    JSValue ret;
    JSObject* fp;
    pc += 3;
    S->sf->cur_pc = pc;
    if (JS_VALUE_GET_TAG(call_argv[-2]) == JS_TAG_OBJECT && (fp = JS_VALUE_GET_OBJ(call_argv[-2]))->class_id == JS_CLASS_BYTECODE_FUNCTION
        && fp->is_constructor && fp->u.func.function_bytecode->func_kind == JS_FUNC_NORMAL) {
        JSTCState* T;
        S->pc = pc;
        S->sp = call_argv - 2;
        T = js_tcv_push_ctor(ctx, S, call_argv, call_argc);
        if (unlikely(!T))
            TC_THROW();
        TC_INLINE_ENTER(T);
    }
    ret = JS_CallConstructorInternal(ctx, call_argv[-2], call_argv[-1], call_argc, call_argv, 0);
    if (unlikely(JS_IsException(ret)))
        TC_THROW();
    for (i = -2; i < call_argc; i++)
        JS_FreeValue(ctx, call_argv[i]);
    sp -= call_argc + 2;
    *sp++ = ret;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_array_from)
{
    int call_argc = get_u16(pc + 1);
    JSValue ret;
    pc += 3;
    ret = js_create_array_free(ctx, call_argc, sp - call_argc);
    sp -= call_argc;
    if (unlikely(JS_IsException(ret)))
        TC_THROW();
    *sp++ = ret;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_apply)
{
    int magic = get_u16(pc + 1);
    JSValue ret;
    pc += 3;
    S->sf->cur_pc = pc;
    ret = js_function_apply(ctx, sp[-3], 2, (JSValueConst*)&sp[-2], magic);
    if (unlikely(JS_IsException(ret)))
        TC_THROW();
    JS_FreeValue(ctx, sp[-3]);
    JS_FreeValue(ctx, sp[-2]);
    JS_FreeValue(ctx, sp[-1]);
    sp -= 3;
    *sp++ = ret;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_return)
{
    S->ret = *--sp;
    if (S->up)
        TC_JUMP(js_tc_leave);
    if (S->fr_flags & TC_FR_ENTRY) {
        if (S->fr_flags & TC_FR_KEEP)
            TC_JUMP(js_tc_exit_keep);
        TC_JUMP(js_tc_exit_entry);
    }
    S->pc = pc + 1;
    S->sp = sp;
    return 2;
}

TC_HANDLER(js_tc_return_undef)
{
    S->ret = JS_UNDEFINED;
    if (S->up)
        TC_JUMP(js_tc_leave);
    if (S->fr_flags & TC_FR_ENTRY) {
        if (S->fr_flags & TC_FR_KEEP)
            TC_JUMP(js_tc_exit_keep);
        TC_JUMP(js_tc_exit_entry);
    }
    S->pc = pc + 1;
    S->sp = sp;
    return 2;
}

#define TC_EXIT_CODE(code) \
    do {                   \
        S->pc = pc;        \
        S->sp = sp;        \
        return (code);     \
    } while (0)

#define TC_SUSPEND(v)    \
    do {                 \
        S->ret = (v);    \
        TC_EXIT_CODE(3); \
    } while (0)

TC_HANDLER(js_tc_op_await)
{
    pc++;
    TC_SUSPEND(JS_NewInt32(ctx, FUNC_RET_AWAIT));
}

TC_HANDLER(js_tc_op_yield)
{
    pc++;
    TC_SUSPEND(JS_NewInt32(ctx, FUNC_RET_YIELD));
}

TC_HANDLER(js_tc_op_yield_star)
{
    pc++;
    TC_SUSPEND(JS_NewInt32(ctx, FUNC_RET_YIELD_STAR));
}

TC_HANDLER(js_tc_op_return_async)
{
    pc++;
    TC_SUSPEND(JS_UNDEFINED);
}

TC_HANDLER(js_tc_op_initial_yield)
{
    pc++;
    TC_SUSPEND(JS_NewInt32(ctx, FUNC_RET_INITIAL_YIELD));
}

TC_HANDLER(js_tc_op_nop)
{
    TC_NEXT(1);
}

TC_HANDLER(js_tc_op_invalid)
{
    int opcode = *pc;
    pc++;
    JS_ThrowInternalError(ctx, "invalid opcode: pc=%u opcode=0x%02x", (int)(pc - S->b->byte_code_buf - 1), opcode);
    TC_THROW();
}

TC_HANDLER(js_tc_op2_invalid)
{
    pc += 2;
    JS_ThrowInternalError(ctx, "invalid opcode: pc=%u opcode=0x%02x", (int)(pc - S->b->byte_code_buf - 1), OP_ext);
    TC_THROW();
}

#define TC_SET_BOOL(free_old, v)           \
    do {                                   \
        int tc_res_ = (v);                 \
        if (free_old)                      \
            JS_FreeValue(ctx, sp[-1]);     \
        sp[-1] = JS_NewBool(ctx, tc_res_); \
        TC_NEXT(1);                        \
    } while (0)

TC_HANDLER(js_tc_op_is_undefined_or_null)
{
    if (JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_UNDEFINED || JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_NULL)
        TC_SET_BOOL(0, 1);
    TC_SET_BOOL(1, 0);
}

TC_HANDLER(js_tc_op_is_undefined)
{
    if (JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_UNDEFINED)
        TC_SET_BOOL(0, 1);
    TC_SET_BOOL(1, 0);
}

TC_HANDLER(js_tc_op_is_null)
{
    if (JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_NULL)
        TC_SET_BOOL(0, 1);
    TC_SET_BOOL(1, 0);
}

TC_HANDLER(js_tc_op_typeof_is_undefined)
{
    TC_SET_BOOL(1, js_operator_typeof(ctx, sp[-1]) == JS_ATOM_undefined);
}

TC_HANDLER(js_tc_op_typeof_is_function)
{
    TC_SET_BOOL(1, js_operator_typeof(ctx, sp[-1]) == JS_ATOM_function);
}

#define TC_TAIL_CALL(name, nthis)                                                                                                               \
    TC_HANDLER(name)                                                                                                                            \
    {                                                                                                                                           \
        int call_argc = get_u16(pc + 1);                                                                                                        \
        JSValue* call_argv = sp - call_argc;                                                                                                    \
        JSValue ret_val;                                                                                                                        \
        pc += 3;                                                                                                                                \
        S->sf->cur_pc = pc;                                                                                                                     \
        if (js_tc_inlinable(call_argv[-1]) && !(S->fr_flags & TC_FR_KEEP)) {                                                                    \
            JSFunctionBytecode* nb = JS_VALUE_GET_OBJ(call_argv[-1])->u.func.function_bytecode;                                                 \
            int arg_alloc = (call_argc < nb->arg_count) ? nb->arg_count : 0;                                                                    \
            if (sizeof(JSValue) * (arg_alloc + nb->var_count + nb->stack_size) + sizeof(JSVarRef*) * nb->var_ref_count <= S->alloca_capacity) { \
                if (S->up || (S->fr_flags & TC_FR_ENTRY))                                                                                       \
                    TC_JUMP(js_tc_tail_inline);                                                                                                 \
                if (S->b->func_kind == JS_FUNC_NORMAL) {                                                                                        \
                    JSValue* nargv = js_malloc(ctx, sizeof(JSValue) * call_argc);                                                               \
                    if (nargv) {                                                                                                                \
                        int j;                                                                                                                  \
                        memcpy(nargv, call_argv, sizeof(JSValue) * call_argc);                                                                  \
                        S->tc_func = call_argv[-1];                                                                                             \
                        S->tc_argv = nargv;                                                                                                     \
                        S->tc_argc = call_argc;                                                                                                 \
                        S->tc_arg_alloc = arg_alloc;                                                                                            \
                        if (nthis == 2) {                                                                                                       \
                            S->tc_this = call_argv[-2];                                                                                         \
                            call_argv[-2] = JS_UNDEFINED;                                                                                       \
                        } else {                                                                                                                \
                            S->tc_this = JS_UNDEFINED;                                                                                          \
                        }                                                                                                                       \
                        call_argv[-1] = JS_UNDEFINED;                                                                                           \
                        for (j = 0; j < call_argc; j++)                                                                                         \
                            call_argv[j] = JS_UNDEFINED;                                                                                        \
                        TC_EXIT_CODE(4);                                                                                                        \
                    }                                                                                                                           \
                }                                                                                                                               \
            }                                                                                                                                   \
        }                                                                                                                                       \
        if (!js_call_numeric_cfunc(ctx, call_argv[-1], call_argc, call_argv, &ret_val))                                                         \
            ret_val = js_call_dispatch(ctx, call_argv[-1], nthis == 2 ? call_argv[-2] : JS_UNDEFINED, JS_UNDEFINED, call_argc, call_argv, 0);   \
        if (unlikely(JS_IsException(ret_val)))                                                                                                  \
            TC_THROW();                                                                                                                         \
        S->ret = ret_val;                                                                                                                       \
        if (S->up)                                                                                                                              \
            TC_JUMP(js_tc_leave);                                                                                                               \
        if (S->fr_flags & TC_FR_ENTRY)                                                                                                          \
            TC_JUMP(js_tc_exit_entry);                                                                                                          \
        TC_EXIT_CODE(2);                                                                                                                        \
    }

TC_TAIL_CALL(js_tc_op_tail_call, 1)
TC_TAIL_CALL(js_tc_op_tail_call_method, 2)

static const JSTCDisp js_tc_table2[256];

TC_HANDLER(js_tc_op_ext)
{
    __attribute__((musttail)) return js_tc_table2[pc[1]].h(TC_PASS);
}

#define TC_OP2_READ_LOC_LOC(v1, v2)                                 \
    int ai = get_u16(pc), bi = get_u16(pc + 2);                     \
    JSValue v1 = lb[ai], v2 = lb[bi];                               \
    if (unlikely(JS_IsUninitialized(v1))) {                         \
        JS_ThrowReferenceErrorUninitialized2(ctx, S->b, ai, FALSE); \
        TC_THROW();                                                 \
    }                                                               \
    if (unlikely(JS_IsUninitialized(v2))) {                         \
        JS_ThrowReferenceErrorUninitialized2(ctx, S->b, bi, FALSE); \
        TC_THROW();                                                 \
    }

#define TC_OP2_PROBE_STRICT_EQ(o1, o2)                                                     \
    {                                                                                      \
        uint32_t tag1 = JS_VALUE_GET_TAG(o1);                                              \
        uint32_t tag2 = JS_VALUE_GET_TAG(o2);                                              \
        if (likely(tag1 == JS_TAG_INT)) {                                                  \
            if (tag2 == JS_TAG_INT) {                                                      \
                res = JS_VALUE_GET_INT(o1) == JS_VALUE_GET_INT(o2);                        \
            } else if (JS_TAG_IS_FLOAT64(tag2)) {                                          \
                res = (JS_VALUE_GET_INT(o1) == JS_VALUE_GET_FLOAT64(o2));                  \
            } else {                                                                       \
                res = FALSE;                                                               \
            }                                                                              \
        } else if (JS_TAG_IS_FLOAT64(tag1)) {                                              \
            if (tag2 == JS_TAG_INT) {                                                      \
                res = (JS_VALUE_GET_FLOAT64(o1) == JS_VALUE_GET_INT(o2));                  \
            } else if (JS_TAG_IS_FLOAT64(tag2)) {                                          \
                res = (JS_VALUE_GET_FLOAT64(o1) == JS_VALUE_GET_FLOAT64(o2));              \
            } else {                                                                       \
                res = FALSE;                                                               \
            }                                                                              \
        } else if (tag1 == JS_TAG_OBJECT) {                                                \
            res = (tag2 == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(o1) == JS_VALUE_GET_OBJ(o2)); \
        } else if (tag1 == JS_TAG_NULL || tag1 == JS_TAG_UNDEFINED) {                      \
            res = (tag1 == tag2);                                                          \
        } else if (tag1 == JS_TAG_STRING && tag2 == JS_TAG_STRING) {                       \
            res = js_string_eq_quick(ctx, JS_VALUE_GET_STRING(o1),                         \
                JS_VALUE_GET_STRING(o2));                                                  \
        } else {                                                                           \
            res = js_strict_eq2(ctx, o1, o2, JS_EQ_STRICT);                                \
        }                                                                                  \
    }

#define TC_OP2_CALL_METHOD(argc_, kind_, load_d_, adv_)                                                                                \
    {                                                                                                                                  \
        JSAtom mf_atom;                                                                                                                \
        JSValue mf_this, mf_obj, mf_func, mf_arg;                                                                                      \
        JSObject* mf_p;                                                                                                                \
        JSProperty* mf_pr;                                                                                                             \
        JSShapeProperty* mf_prs;                                                                                                       \
        int mf_loaded, mf_form, mf_idx;                                                                                                \
        int mf_argc = (argc_);                                                                                                         \
        JSValue mf_argv1[2];                                                                                                           \
        mf_atom = get_u32(pc);                                                                                                         \
        mf_this = sp[-1];                                                                                                              \
        mf_obj = mf_this;                                                                                                              \
        mf_loaded = FALSE;                                                                                                             \
        mf_p = js_fast_lookup_start(ctx, mf_this, mf_atom);                                                                                          \
        if (likely(mf_p != NULL)) {                                                                      \
            for (;;) {                                                                                                                 \
                mf_prs = find_own_property(&mf_pr, mf_p, mf_atom);                                                                     \
                if (mf_prs) {                                                                                                          \
                    if (unlikely(mf_prs->flags & JS_PROP_TMASK))                                                                       \
                        break;                                                                                                         \
                    mf_func = JS_DupValue(ctx, mf_pr->u.value);                                                                        \
                    mf_loaded = TRUE;                                                                                                  \
                    break;                                                                                                             \
                }                                                                                                                      \
                if (unlikely(mf_p->is_exotic)) {                                                                                       \
                                                                                                                                       \
                    if (mf_p->class_id != JS_CLASS_ARRAY || __JS_AtomIsTaggedInt(mf_atom)) {                                           \
                        mf_obj = JS_MKPTR(JS_TAG_OBJECT, mf_p);                                                                        \
                        break;                                                                                                         \
                    }                                                                                                                  \
                }                                                                                                                      \
                mf_p = mf_p->shape->proto;                                                                                             \
                if (!mf_p) {                                                                                                           \
                    mf_func = JS_UNDEFINED;                                                                                            \
                    mf_loaded = TRUE;                                                                                                  \
                    break;                                                                                                             \
                }                                                                                                                      \
            }                                                                                                                          \
        }                                                                                                                              \
        if (!mf_loaded) {                                                                                                              \
            S->sf->cur_pc = pc + (load_d_);                                                                                            \
            mf_func = JS_GetPropertyInternal(ctx, mf_obj, mf_atom,                                                                     \
                sp[-1], 0);                                                                                                            \
            if (unlikely(JS_IsException(mf_func)))                                                                                     \
                TC_THROW();                                                                                                            \
        }                                                                                                                              \
                                                                                                                                       \
        mf_arg = JS_UNDEFINED;                                                                                                         \
        switch ((kind_)) {                                                                                                             \
        case 1:                                                                                                                        \
            mf_form = pc[4];                                                                                                           \
            mf_idx = get_u16(pc + 5);                                                                                                  \
            if (mf_form == 3) {                                                                                                        \
                mf_arg = JS_NewInt32(ctx, get_i16(pc + 5));                                                                            \
                break;                                                                                                                 \
            }                                                                                                                          \
            if (mf_form == 1) {                                                                                                        \
                if (unlikely(JS_IsUninitialized(lb[mf_idx]))) {                                                                        \
                    JS_FreeValue(ctx, mf_func);                                                                                        \
                    JS_ThrowReferenceErrorUninitialized2(ctx, S->b,                                                                    \
                        mf_idx,                                                                                                        \
                        FALSE);                                                                                                        \
                    TC_THROW();                                                                                                        \
                }                                                                                                                      \
            }                                                                                                                          \
            mf_arg = JS_DupValue(ctx, mf_form == 2 ? ab[mf_idx] : lb[mf_idx]);                                                         \
            break;                                                                                                                     \
        case 2:                                                                                                                        \
            mf_form = pc[4];                                                                                                           \
            if (mf_form == 0)                                                                                                          \
                mf_arg = JS_NewInt32(ctx, get_i8(pc + 5));                                                                             \
            else                                                                                                                       \
                mf_arg = JS_DupValue(ctx, S->cpool[pc[5]]);                                                                            \
            break;                                                                                                                     \
        case 3:                                                                                                                        \
            mf_arg = JS_DupValue(ctx, S->cpool[get_u32(pc + 4)]);                                                                      \
            break;                                                                                                                     \
        default:                                                                                                                       \
            break;                                                                                                                     \
        }                                                                                                                              \
                                                                                                                                       \
        S->sf->cur_pc = pc + (adv_);                                                                                                   \
        mf_argv1[0] = mf_arg;                                                                                                          \
        mf_argv1[1] = JS_UNDEFINED;                                                                                                    \
        if (likely(JS_VALUE_GET_TAG(mf_func) == JS_TAG_OBJECT) && JS_VALUE_GET_OBJ(mf_func)->class_id == JS_CLASS_C_FUNCTION) {        \
                                                                                                                                       \
            JSObject* mf_cf = JS_VALUE_GET_OBJ(mf_func);                                                                               \
            JSCFunctionEnum mf_cp = mf_cf->u.cfunc.cproto;                                                                             \
            if (unlikely(mf_cf->u.cfunc.tc_iter) && mf_argc == 1 && js_tc_iter_args_ok(mf_this, 1, mf_argv1)) {                        \
                JSValue mf_cv[3];                                                                                                      \
                JSTCState* mf_T;                                                                                                       \
                mf_cv[0] = mf_this;                                                                                                    \
                mf_cv[1] = mf_func;                                                                                                    \
                mf_cv[2] = mf_arg;                                                                                                     \
                mf_T = js_tcv_iter_push(ctx, S, mf_cv + 2, 1);                                                                         \
                if (unlikely(!mf_T)) {                                                                                                 \
                    JS_FreeValue(ctx, mf_func);                                                                                        \
                    JS_FreeValue(ctx, mf_arg);                                                                                         \
                    TC_THROW();                                                                                                        \
                }                                                                                                                      \
                S->pc = pc + (adv_);                                                                                                   \
                S->sp = sp - 1;                                                                                                        \
                TC_ITER_ENTER(mf_T);                                                                                                   \
            }                                                                                                                          \
            if (mf_cf->u.cfunc.c_function.generic == js_string_charCodeAt && mf_cp == JS_CFUNC_generic && js_string_char_code_fast(mf_this, mf_arg, &ret_val)) { \
            } else if (unlikely(mf_argc < mf_cf->u.cfunc.length) || unlikely(mf_cp != JS_CFUNC_generic && mf_cp != JS_CFUNC_generic_magic)) { \
                if (!js_call_numeric_cfunc(ctx, mf_func, mf_argc, (JSValueConst*)mf_argv1, &ret_val))                                  \
                    ret_val = js_call_c_function(ctx, mf_func, mf_this,                                                                \
                        mf_argc,                                                                                                       \
                        (JSValueConst*)                                                                                                \
                            mf_argv1,                                                                                                  \
                        0);                                                                                                            \
            } else if (unlikely(js_check_stack_overflow(rt,                                                                            \
                           sizeof(JSValue) * 2))) {                                                                                    \
                ret_val = JS_ThrowStackOverflow(ctx);                                                                                  \
            } else {                                                                                                                   \
                                                                                                                                       \
                JSStackFrame mf_csf, *mf_prev_sf;                                                                                      \
                JSContext* mf_realm = mf_cf->u.cfunc.realm;                                                                            \
                mf_prev_sf = rt->current_stack_frame;                                                                                  \
                mf_csf.prev_frame = mf_prev_sf;                                                                                        \
                rt->current_stack_frame = &mf_csf;                                                                                     \
                mf_csf.js_mode = 0;                                                                                                    \
                mf_csf.borrow_epoch = 0;                                                                                               \
                mf_csf.cur_func = (JSValue)mf_func;                                                                                    \
                mf_csf.arg_count = mf_argc;                                                                                            \
                mf_csf.arg_buf = mf_argv1;                                                                                             \
                uint64_t mf_taken0 = rt->uncatchable_events;                                                                           \
                JS_BOUNDARY_ENTER(ctx);                                                                                                \
                if (mf_cp == JS_CFUNC_generic)                                                                                         \
                    ret_val = mf_cf->u.cfunc.c_function.generic(                                                                       \
                        mf_realm, mf_this, mf_argc,                                                                                    \
                        (JSValueConst*)mf_argv1);                                                                                      \
                else                                                                                                                   \
                    ret_val = mf_cf->u.cfunc.c_function.generic_magic(mf_realm, mf_this,                                               \
                        mf_argc,                                                                                                       \
                        (JSValueConst*)mf_argv1,                                                                                       \
                        mf_cf->u.cfunc.magic);                                                                                         \
                rt->current_stack_frame = mf_prev_sf;                                                                                  \
                if (unlikely(mf_csf.borrow_epoch != 0))                                                                                \
                    rt->native_borrow_depth--;                                                                                         \
                if (unlikely(rt->uncatchable_events != mf_taken0))                                                                                         \
                    ret_val = js_interrupt_swallowed(ctx, ret_val);                                                                                         \
                JS_BOUNDARY_CHECK(ctx, ret_val); \
            }                                                                                                                          \
        } else if (js_tc_inlinable(mf_func)) {                                                                                         \
            JSTCState* mf_T;                                                                                                           \
            S->pc = pc + (adv_);                                                                                                       \
            S->sp = sp - 1;                                                                                                            \
            mf_T = js_tcv_push_frame(ctx, S, mf_func, sp[-1], mf_argv1, mf_argc, 0);                                                   \
            if (unlikely(!mf_T)) {                                                                                                     \
                JS_FreeValue(ctx, mf_func);                                                                                            \
                if (mf_argc)                                                                                                           \
                    JS_FreeValue(ctx, mf_argv1[0]);                                                                                    \
                TC_THROW();                                                                                                            \
            }                                                                                                                          \
            TC_INLINE_ENTER(mf_T);                                                                                                     \
        } else {                                                                                                                       \
                                                                                                                                       \
            ret_val = js_call_dispatch(ctx, mf_func, mf_this,                                                                          \
                JS_UNDEFINED, mf_argc,                                                                                                 \
                mf_argv1, 0);                                                                                                          \
        }                                                                                                                              \
        if (unlikely(JS_IsException(ret_val))) {                                                                                       \
                                                                                                                                       \
            JS_FreeValue(ctx, mf_func);                                                                                                \
            if (mf_argc)                                                                                                               \
                JS_FreeValue(ctx, mf_argv1[0]);                                                                                        \
            TC_THROW();                                                                                                                \
        }                                                                                                                              \
                                                                                                                                       \
        JS_FreeValue(ctx, sp[-1]);                                                                                                     \
        JS_FreeValue(ctx, mf_func);                                                                                                    \
        if (mf_argc)                                                                                                                   \
            JS_FreeValue(ctx, mf_argv1[0]);                                                                                            \
        sp--;                                                                                                                          \
        *sp++ = ret_val;                                                                                                               \
        pc += (adv_);                                                                                                                  \
    }

TC_HANDLER(js_tc_op_object)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
#if CONFIG_PRESIZE_LITERAL
    {
        JSObjectPresize* ps = S->b->obj_presize;
        if (unlikely(ps != NULL)) {
            uint32_t off = (uint32_t)(pc - 1 - S->b->byte_code_buf);
            int si;

            for (si = 0; si < ps->site_count; si++) {
                if (ps->sites[si].bc_offset == off) {
                    *sp++ = js_object_presize_new(ctx,
                        &ps->sites[si]);
                    if (unlikely(JS_IsException(sp[-1])))
                        TC_THROW();
#if CONFIG_LITERAL_BOILERPLATE
                    S->bb_rec_obj = JS_VALUE_GET_OBJ(sp[-1]);
                    S->bb_rec_off = off;
                    S->bb_rec_site = &ps->sites[si];
                    S->bb_rec_nf = 0;
#endif
                    TC_DISPATCH();
                }
            }
        }
    }
#endif
    *sp++ = JS_NewObject(ctx);
    if (unlikely(JS_IsException(sp[-1])))
        TC_THROW();
#if CONFIG_LITERAL_BOILERPLATE
    TC_BB_REC_START(sp[-1], (uint32_t)(pc - 1 - S->b->byte_code_buf));
#endif
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_special_object)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int arg = *pc++;
        switch (arg) {
        case OP_SPECIAL_OBJECT_ARGUMENTS:
            *sp++ = js_build_arguments(ctx, S->argc, (JSValueConst*)S->argv);
            if (unlikely(JS_IsException(sp[-1])))
                TC_THROW();
            break;
        case OP_SPECIAL_OBJECT_MAPPED_ARGUMENTS:
            *sp++ = js_build_mapped_arguments(ctx, S->argc, (JSValueConst*)S->argv,
                S->sf, min_int(S->argc, S->b->arg_count));
            if (unlikely(JS_IsException(sp[-1])))
                TC_THROW();
            break;
        case OP_SPECIAL_OBJECT_THIS_FUNC:
            *sp++ = JS_DupValue(ctx, S->sf->cur_func);
            break;
        case OP_SPECIAL_OBJECT_NEW_TARGET:
            *sp++ = JS_DupValue(ctx, S->new_target);
            break;
        case OP_SPECIAL_OBJECT_HOME_OBJECT: {
            JSObject* p1;
            p1 = S->p->u.func.home_object;
            if (unlikely(!p1))
                *sp++ = JS_UNDEFINED;
            else
                *sp++ = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p1));
        } break;
        case OP_SPECIAL_OBJECT_VAR_OBJECT:
            *sp++ = JS_NewObjectProto(ctx, JS_NULL);
            if (unlikely(JS_IsException(sp[-1])))
                TC_THROW();
            break;
        case OP_SPECIAL_OBJECT_IMPORT_META:
            *sp++ = js_import_meta(ctx);
            if (unlikely(JS_IsException(sp[-1])))
                TC_THROW();
            break;
        default:
            abort();
        }
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_rest)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int first = get_u16(pc);
        pc += 2;
        first = min_int(first, S->argc);
        *sp++ = js_create_array(ctx, S->argc - first, (JSValueConst*)(S->argv + first));
        if (unlikely(JS_IsException(sp[-1])))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_check_ctor_return)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    if (!JS_IsObject(sp[-1])) {
        if (!JS_IsUndefined(sp[-1])) {
            JS_ThrowTypeError(S->caller_ctx, "derived class constructor must return an object or undefined");
            TC_THROW();
        }
        sp[0] = JS_TRUE;
    } else {
        sp[0] = JS_FALSE;
    }
    sp++;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_check_ctor)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    if (JS_IsUndefined(S->new_target)) {
        JS_ThrowTypeError(ctx, "class constructors must be invoked with 'new'");
        TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_init_ctor)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue super, ret;
        S->sf->cur_pc = pc;
        if (JS_IsUndefined(S->new_target)) {
            JS_ThrowTypeError(ctx, "class constructors must be invoked with 'new'");
            TC_THROW();
        }
        super = JS_GetPrototype(ctx, S->sf->cur_func);
        if (JS_IsException(super))
            TC_THROW();
        ret = JS_CallConstructor2(ctx, super, S->new_target, S->argc, (JSValueConst*)S->argv);
        JS_FreeValue(ctx, super);
        if (JS_IsException(ret))
            TC_THROW();
        *sp++ = ret;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_check_brand)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int ret = JS_CheckBrand(ctx, sp[-2], sp[-1]);
        if (ret < 0)
            TC_THROW();
        if (!ret) {
            JS_ThrowTypeError(ctx, "invalid brand on object");
            TC_THROW();
        }
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_add_brand)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    if (JS_AddBrand(ctx, sp[-2], sp[-1]) < 0)
        TC_THROW();
    JS_FreeValue(ctx, sp[-2]);
    JS_FreeValue(ctx, sp[-1]);
    sp -= 2;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_throw)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    JS_Throw(ctx, *--sp);
    TC_THROW();
}

TC_HANDLER(js_tc_op_throw_error)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSAtom atom;
        int type;
        atom = get_u32(pc);
        type = pc[4];
        pc += 5;
        if (type == JS_THROW_VAR_RO)
            JS_ThrowTypeErrorReadOnly(ctx, JS_PROP_THROW, atom);
        else if (type == JS_THROW_VAR_REDECL)
            JS_ThrowSyntaxErrorVarRedeclaration(ctx, atom);
        else if (type == JS_THROW_VAR_UNINITIALIZED)
            JS_ThrowReferenceErrorUninitialized(ctx, atom);
        else if (type == JS_THROW_ERROR_DELETE_SUPER)
            JS_ThrowReferenceError(ctx, "unsupported reference to 'super'");
        else if (type == JS_THROW_ERROR_ITERATOR_THROW)
            JS_ThrowTypeError(ctx, "iterator does not have a throw method");
        else if (type == JS_THROW_ERROR_INVALID_LVALUE)
            JS_ThrowReferenceError(ctx, "invalid assignment target");
        else
            JS_ThrowInternalError(ctx, "invalid throw var type %d", type);
    }
    TC_THROW();
}

TC_HANDLER(js_tc_op_eval)
{
    int opcode = *pc;
    JSValue ret_val;
    int call_argc;
    JSValue* call_argv;
    int i;
    pc++;
    (void)opcode;
    {
        JSValueConst obj;
        int scope_idx;
        call_argc = get_u16(pc);
        scope_idx = get_u16(pc + 2) + ARG_SCOPE_END;
        pc += 4;
        call_argv = sp - call_argc;
        S->sf->cur_pc = pc;
        if (js_same_value(ctx, call_argv[-1], ctx->eval_obj)) {
            if (call_argc >= 1)
                obj = call_argv[0];
            else
                obj = JS_UNDEFINED;
            ret_val = JS_EvalObject(ctx, JS_UNDEFINED, obj,
                JS_EVAL_TYPE_DIRECT, scope_idx);
        } else {
            ret_val = js_call_dispatch(ctx, call_argv[-1], JS_UNDEFINED,
                JS_UNDEFINED, call_argc, call_argv, 0);
        }
        if (unlikely(JS_IsException(ret_val)))
            TC_THROW();
        for (i = -1; i < call_argc; i++)
            JS_FreeValue(ctx, call_argv[i]);
        sp -= call_argc + 1;
        *sp++ = ret_val;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_apply_eval)
{
    int opcode = *pc;
    JSValue ret_val;
    pc++;
    (void)opcode;
    {
        int scope_idx;
        uint32_t len;
        JSValue* tab;
        JSValueConst obj;

        scope_idx = get_u16(pc) + ARG_SCOPE_END;
        pc += 2;
        S->sf->cur_pc = pc;
        tab = build_arg_list(ctx, &len, sp[-1]);
        if (!tab)
            TC_THROW();
        if (js_same_value(ctx, sp[-2], ctx->eval_obj)) {
            if (len >= 1)
                obj = tab[0];
            else
                obj = JS_UNDEFINED;
            ret_val = JS_EvalObject(ctx, JS_UNDEFINED, obj,
                JS_EVAL_TYPE_DIRECT, scope_idx);
        } else {
            ret_val = JS_Call(ctx, sp[-2], JS_UNDEFINED, len,
                (JSValueConst*)tab);
        }
        free_arg_list(ctx, tab, len);
        if (unlikely(JS_IsException(ret_val)))
            TC_THROW();
        JS_FreeValue(ctx, sp[-2]);
        JS_FreeValue(ctx, sp[-1]);
        sp -= 2;
        *sp++ = ret_val;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_regexp)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        sp[-2] = JS_NewRegexp(ctx, sp[-2], sp[-1]);
        sp--;
        if (JS_IsException(sp[-1]))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_get_super)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue proto;
        S->sf->cur_pc = pc;
        proto = JS_GetPrototype(ctx, sp[-1]);
        if (JS_IsException(proto))
            TC_THROW();
        JS_FreeValue(ctx, sp[-1]);
        sp[-1] = proto;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_import)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue val;
        S->sf->cur_pc = pc;
        val = js_dynamic_import(ctx, sp[-2], sp[-1]);
        if (JS_IsException(val))
            TC_THROW();
        JS_FreeValue(ctx, sp[-2]);
        JS_FreeValue(ctx, sp[-1]);
        sp--;
        sp[-1] = val;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_make_var_ref_ref)
{
    int opcode = *pc;
    JSRuntime* rt = ctx->rt;
    pc++;
    {
        JSVarRef* var_ref;
        JSProperty* pr;
        JSAtom atom;
        int idx;
        atom = get_u32(pc);
        idx = get_u16(pc + 4);
        pc += 6;
        *sp++ = JS_NewObjectProto(ctx, JS_NULL);
        if (unlikely(JS_IsException(sp[-1])))
            TC_THROW();
        if (opcode == OP_make_var_ref_ref) {
            var_ref = S->var_refs[idx];
            js_rc(var_ref)->ref_count++;
        } else {
            var_ref = get_var_ref(ctx, S->sf, idx, opcode == OP_make_arg_ref);
            if (!var_ref)
                TC_THROW();
        }
        pr = add_property(ctx, JS_VALUE_GET_OBJ(sp[-1]), atom,
            JS_PROP_WRITABLE | JS_PROP_VARREF);
        if (!pr) {
            free_var_ref(rt, var_ref);
            TC_THROW();
        }
        pr->u.var_ref = var_ref;
        *sp++ = JS_AtomToValue(ctx, atom);
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_make_var_ref)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSAtom atom;
        atom = get_u32(pc);
        pc += 4;
        S->sf->cur_pc = pc;

        if (JS_GetGlobalVarRef(ctx, atom, sp))
            TC_THROW();
        sp += 2;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_switch)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        const uint8_t* pc_op = pc - 1;
        uint32_t cp_idx = get_u32(pc);
        pc += 4;
        if (JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_INT) {
            const uint8_t* t = JS_VALUE_GET_STRING(S->cpool[cp_idx])->u.str8;
            int32_t min = (int32_t)switch_tbl_get(t);
            uint32_t count = switch_tbl_get(t + 4);
            uint32_t idx = (uint32_t)(JS_VALUE_GET_INT(sp[-1]) - min);
            if (idx < count) {
                int32_t off = (int32_t)switch_tbl_get(t + 8 + idx * 4);
                if (off != 0)
                    pc = pc_op + off;
            }
        }
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_catch)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int32_t diff;
        diff = get_u32(pc);
        sp[0] = JS_NewCatchOffset(ctx, pc + diff - S->b->byte_code_buf);
        sp++;
        pc += 4;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_for_await_catch_body)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int32_t diff;
        diff = get_u32(pc);
        sp[-3] = JS_NewCatchOffset(ctx, pc + diff - S->b->byte_code_buf);
        pc += 4;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_gosub)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int32_t diff;
        diff = get_u32(pc);
        sp[0] = JS_NewInt32(ctx, pc + 4 - S->b->byte_code_buf);
        sp++;
        pc += diff;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_ret)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue op1;
        uint32_t pos;
        op1 = sp[-1];
        if (unlikely(JS_VALUE_GET_TAG(op1) != JS_TAG_INT))
            goto ret_fail;
        pos = JS_VALUE_GET_INT(op1);
        if (unlikely(pos >= S->b->byte_code_len)) {
        ret_fail:
            JS_ThrowInternalError(ctx, "invalid ret value");
            TC_THROW();
        }
        sp--;
        pc = S->b->byte_code_buf + pos;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_for_in_start)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_for_in_start(ctx, sp))
        TC_THROW();
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_for_in_next)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_for_in_next(ctx, sp))
        TC_THROW();
    sp += 2;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_for_of_start)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_for_of_start_mark(ctx, sp, *pc))
        TC_THROW();
    sp += 1;
    *sp++ = JS_NewCatchOffset(ctx, 0);
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_for_of_next_slow)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int offset = -3 - pc[0];
        pc += 1;
        S->sf->cur_pc = pc;
        if (js_for_of_next(ctx, sp, offset))
            TC_THROW();
        sp += 2;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_for_of_next)
{
    int offset = -3 - pc[1];
    JSValue iter = sp[offset], next = sp[offset + 1], arr;
    JSObject *p, *np, *ap;
    JSArrayIteratorData* it;
    JSCFunctionType ft;
    uint32_t idx;
    if (JS_VALUE_GET_TAG(next) == JS_TAG_UNINITIALIZED && JS_VALUE_GET_TAG(iter) == JS_TAG_OBJECT) {
        ap = JS_VALUE_GET_OBJ(iter);
        idx = (uint32_t)JS_VALUE_GET_INT(next);
        if (unlikely(!ap->fast_array || idx >= ap->u.array.count))
            TC_JUMP(js_tc_op_for_of_next_slow);
        sp[offset + 1] = JS_MKVAL(JS_TAG_UNINITIALIZED, (int32_t)(idx + 1));
        sp[0] = JS_DupValue(ctx, ap->u.array.u.values[idx]);
        sp[1] = JS_FALSE;
        sp += 2;
        pc += 2;
        TC_DISPATCH();
    }
    if (unlikely(JS_VALUE_GET_TAG(iter) != JS_TAG_OBJECT || JS_VALUE_GET_TAG(next) != JS_TAG_OBJECT))
        TC_JUMP(js_tc_op_for_of_next_slow);
    p = JS_VALUE_GET_OBJ(iter);
    np = JS_VALUE_GET_OBJ(next);
    ft.iterator_next = js_array_iterator_next;
    if (unlikely(p->class_id != JS_CLASS_ARRAY_ITERATOR || !(it = p->u.array_iterator_data) || it->kind != JS_ITERATOR_KIND_VALUE
            || np->class_id != JS_CLASS_C_FUNCTION || np->u.cfunc.c_function.generic != ft.generic || np->u.cfunc.magic != 0))
        TC_JUMP(js_tc_op_for_of_next_slow);
    arr = it->obj;
    if (unlikely(JS_VALUE_GET_TAG(arr) != JS_TAG_OBJECT))
        TC_JUMP(js_tc_op_for_of_next_slow);
    ap = JS_VALUE_GET_OBJ(arr);
    idx = it->idx;
    if (unlikely(ap->class_id != JS_CLASS_ARRAY || !ap->fast_array || idx >= ap->u.array.count))
        TC_JUMP(js_tc_op_for_of_next_slow);
    it->idx = idx + 1;
    sp[0] = JS_DupValue(ctx, ap->u.array.u.values[idx]);
    sp[1] = JS_FALSE;
    sp += 2;
    pc += 2;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_for_await_of_next)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_for_await_of_next(ctx, sp))
        TC_THROW();
    sp++;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_for_await_of_start)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_for_of_start(ctx, sp, TRUE))
        TC_THROW();
    sp += 1;
    *sp++ = JS_NewCatchOffset(ctx, 0);
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_iterator_get_value_done)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_iterator_get_value_done(ctx, sp))
        TC_THROW();
    sp += 1;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_iterator_check_object)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    if (unlikely(!JS_IsObject(sp[-1]))) {
        JS_ThrowTypeError(ctx, "iterator must return an object");
        TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_iterator_close)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    JSValue next;
    sp--;
    next = sp[-1];
    sp--;
    if (!JS_IsUndefined(sp[-1])) {
        S->sf->cur_pc = pc;
        if (js_for_of_close(ctx, sp[-1], next, FALSE)) {
            JS_FreeValue(ctx, next);
            TC_THROW();
        }
        JS_FreeValue(ctx, sp[-1]);
    }
    JS_FreeValue(ctx, next);
    sp--;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_nip_catch)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue catch_ret;
        catch_ret = *--sp;
        while (sp > S->stack_buf && JS_VALUE_GET_TAG(sp[-1]) != JS_TAG_CATCH_OFFSET) {
            JS_FreeValue(ctx, *--sp);
        }
        if (unlikely(sp == S->stack_buf)) {
            JS_ThrowInternalError(ctx, "nip_catch");
            JS_FreeValue(ctx, catch_ret);
            TC_THROW();
        }
        sp[-1] = catch_ret;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_iterator_next)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue ret;
        S->sf->cur_pc = pc;
        ret = JS_Call(ctx, sp[-3], sp[-4],
            1, (JSValueConst*)(sp - 1));
        if (JS_IsException(ret))
            TC_THROW();
        JS_FreeValue(ctx, sp[-1]);
        sp[-1] = ret;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_iterator_call)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue method, ret;
        BOOL ret_flag;
        int throw_flags;
        throw_flags = *pc++;
        S->sf->cur_pc = pc;
        method = JS_GetProperty(ctx, sp[-4], (throw_flags & 1) ? JS_ATOM_throw : JS_ATOM_return);
        if (JS_IsException(method))
            TC_THROW();
        if (JS_IsUndefined(method) || JS_IsNull(method)) {
            ret_flag = TRUE;
        } else {
            if (throw_flags & 2) {
                ret = JS_CallFree(ctx, method, sp[-4],
                    0, NULL);
            } else {
                ret = JS_CallFree(ctx, method, sp[-4],
                    1, (JSValueConst*)(sp - 1));
            }
            if (JS_IsException(ret))
                TC_THROW();
            JS_FreeValue(ctx, sp[-1]);
            sp[-1] = ret;
            ret_flag = FALSE;
        }
        sp[0] = JS_NewBool(ctx, ret_flag);
        sp += 1;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_private_symbol)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSAtom atom;
        JSValue val;

        atom = get_u32(pc);
        pc += 4;
        val = JS_NewSymbolFromAtom(ctx, atom, JS_ATOM_TYPE_PRIVATE);
        if (JS_IsException(val))
            TC_THROW();
        *sp++ = val;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_get_private_field)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue val;

        val = JS_GetPrivateField(ctx, sp[-2], sp[-1]);
        JS_FreeValue(ctx, sp[-1]);
        JS_FreeValue(ctx, sp[-2]);
        sp[-2] = val;
        sp--;
        if (unlikely(JS_IsException(val)))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_put_private_field)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int ret;
        ret = JS_SetPrivateField(ctx, sp[-3], sp[-1], sp[-2]);
        JS_FreeValue(ctx, sp[-3]);
        JS_FreeValue(ctx, sp[-1]);
        sp -= 3;
        if (unlikely(ret < 0))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_define_private_field)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int ret;
        ret = JS_DefinePrivateField(ctx, sp[-3], sp[-2], sp[-1]);
        JS_FreeValue(ctx, sp[-2]);
        sp -= 2;
        if (unlikely(ret < 0))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_define_field)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int ret;
        JSAtom atom;
        atom = get_u32(pc);
        pc += 4;

#if CONFIG_LITERAL_BOILERPLATE
        if (S->bb_rec_obj != NULL && JS_VALUE_GET_TAG(sp[-2]) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(sp[-2]) == S->bb_rec_obj
            && js_object_presize_define_fast(ctx, S->bb_rec_obj, S->bb_rec_site, S->bb_rec_nf, atom, sp[-1])) {
            sp--;
            S->bb_rec_nf++;
            TC_DISPATCH();
        }
#endif
        ret = JS_DefinePropertyValue(ctx, sp[-2], atom, sp[-1],
            JS_PROP_C_W_E | JS_PROP_THROW);
        sp--;
        if (unlikely(ret < 0))
            TC_THROW();
#if CONFIG_LITERAL_BOILERPLATE
        if (unlikely(S->bb_rec_obj != NULL)) {
            if (JS_VALUE_GET_OBJ(sp[-1]) == S->bb_rec_obj && !__JS_AtomIsTaggedInt(atom)) {
                JSObject* po = S->bb_rec_obj;
                JSShape* sh = po->shape;
                int prefix = S->bb_rec_site ? S->bb_rec_site->field_count : 0;
                int k = S->bb_rec_nf + 1;
                int expect = k > prefix ? k : prefix;

                if (sh->prop_count == expect && po->class_id == JS_CLASS_OBJECT && !po->is_exotic && !po->fast_array && sh->proto == get_proto_obj(ctx->class_proto[JS_CLASS_OBJECT])) {
                    JSShapeProperty* pr = get_shape_prop(sh);
                    if (pr[k - 1].atom == atom && pr[k - 1].flags == JS_PROP_C_W_E) {
                        JSObjectPresizeSite* bb_st_;
                        S->bb_rec_nf = k;
                        bb_st_ = js_object_presize_record_site(
                            ctx, S->b, S->bb_rec_site, S->bb_rec_off, po, k);
                        if (bb_st_ != NULL)
                            S->bb_rec_site = bb_st_;
                    } else {
                        TC_BB_REC_CLEAR();
                    }
                } else {
                    TC_BB_REC_CLEAR();
                }
            } else {
                TC_BB_REC_CLEAR();
            }
        }
#endif
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_set_name)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int ret;
        JSAtom atom;
        atom = get_u32(pc);
        pc += 4;

        ret = JS_DefineObjectName(ctx, sp[-1], atom, JS_PROP_CONFIGURABLE);
        if (unlikely(ret < 0))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_set_name_computed)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int ret;
        ret = JS_DefineObjectNameComputed(ctx, sp[-1], sp[-2], JS_PROP_CONFIGURABLE);
        if (unlikely(ret < 0))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_set_proto)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue proto;
        S->sf->cur_pc = pc;
        proto = sp[-1];
        if (JS_IsObject(proto) || JS_IsNull(proto)) {
            if (JS_SetPrototypeInternal(ctx, sp[-2], proto, TRUE) < 0)
                TC_THROW();
        }
        JS_FreeValue(ctx, proto);
        sp--;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_set_home_object)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    js_method_set_home_object(ctx, sp[-1], sp[-2]);
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_define_method_computed)
{
    int opcode = *pc;
    pc++;
    {
        JSValue getter, setter, value;
        JSValueConst obj;
        JSAtom atom;
        int def_flags, ret, op_flags;
        BOOL is_computed;

        is_computed = (opcode == OP_define_method_computed);
        if (is_computed) {
            atom = JS_ValueToAtom(ctx, sp[-2]);
            if (unlikely(atom == JS_ATOM_NULL))
                TC_THROW();
            opcode += OP_define_method - OP_define_method_computed;
        } else {
            atom = get_u32(pc);
            pc += 4;
        }
        op_flags = *pc++;

        obj = sp[-2 - is_computed];
        if (is_computed)
            TC_BB_REC_DROP(obj);
        def_flags = JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_THROW;
        if (op_flags & OP_DEFINE_METHOD_ENUMERABLE)
            def_flags |= JS_PROP_ENUMERABLE;
        op_flags &= 3;
        value = JS_UNDEFINED;
        getter = JS_UNDEFINED;
        setter = JS_UNDEFINED;
        if (op_flags == OP_DEFINE_METHOD_METHOD) {
            value = sp[-1];
            def_flags |= JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE;
        } else if (op_flags == OP_DEFINE_METHOD_GETTER) {
            getter = sp[-1];
            def_flags |= JS_PROP_HAS_GET;
        } else {
            setter = sp[-1];
            def_flags |= JS_PROP_HAS_SET;
        }
        ret = js_method_set_properties(ctx, sp[-1], atom, def_flags, obj);
        if (ret >= 0) {
            ret = JS_DefineProperty(ctx, obj, atom, value,
                getter, setter, def_flags);
        }
        JS_FreeValue(ctx, sp[-1]);
        if (is_computed) {
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, sp[-2]);
        }
        sp -= 1 + is_computed;
        if (unlikely(ret < 0))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_define_class_computed)
{
    int opcode = *pc;
    pc++;
    {
        int class_flags;
        JSAtom atom;

        atom = get_u32(pc);
        class_flags = pc[4];
        pc += 5;
        if (js_op_define_class(ctx, sp, atom, class_flags,
                S->var_refs, S->sf,
                (opcode == OP_define_class_computed))
            < 0)
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_get_ref_value)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue val;
        JSAtom atom;
        int ret;

        S->sf->cur_pc = pc;
        atom = JS_ValueToAtom(ctx, sp[-1]);
        if (atom == JS_ATOM_NULL)
            TC_THROW();
        if (unlikely(JS_IsUndefined(sp[-2]))) {
            JS_ThrowReferenceErrorNotDefined(ctx, atom);
            JS_FreeAtom(ctx, atom);
            TC_THROW();
        }
        ret = JS_HasProperty(ctx, sp[-2], atom);
        if (unlikely(ret <= 0)) {
            if (ret < 0) {
                JS_FreeAtom(ctx, atom);
                TC_THROW();
            }
            if (is_strict_mode(ctx)) {
                JS_ThrowReferenceErrorNotDefined(ctx, atom);
                JS_FreeAtom(ctx, atom);
                TC_THROW();
            }
            val = JS_UNDEFINED;
        } else {
            val = JS_GetProperty(ctx, sp[-2], atom);
        }
        JS_FreeAtom(ctx, atom);
        if (unlikely(JS_IsException(val)))
            TC_THROW();
        sp[0] = val;
        sp++;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_get_super_value)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue val;
        JSAtom atom;
        S->sf->cur_pc = pc;
        atom = JS_ValueToAtom(ctx, sp[-1]);
        if (unlikely(atom == JS_ATOM_NULL))
            TC_THROW();
        val = JS_GetPropertyInternal(ctx, sp[-2], atom, sp[-3], FALSE);
        JS_FreeAtom(ctx, atom);
        if (unlikely(JS_IsException(val)))
            TC_THROW();
        JS_FreeValue(ctx, sp[-1]);
        JS_FreeValue(ctx, sp[-2]);
        JS_FreeValue(ctx, sp[-3]);
        sp[-3] = val;
        sp -= 2;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_put_ref_value)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int ret;
        JSAtom atom;
        S->sf->cur_pc = pc;
        atom = JS_ValueToAtom(ctx, sp[-2]);
        if (unlikely(atom == JS_ATOM_NULL))
            TC_THROW();
        if (unlikely(JS_IsUndefined(sp[-3]))) {
            if (is_strict_mode(ctx)) {
                JS_ThrowReferenceErrorNotDefined(ctx, atom);
                JS_FreeAtom(ctx, atom);
                TC_THROW();
            } else {
                sp[-3] = JS_DupValue(ctx, ctx->global_obj);
            }
        }
        ret = JS_HasProperty(ctx, sp[-3], atom);
        if (unlikely(ret <= 0)) {
            if (unlikely(ret < 0)) {
                JS_FreeAtom(ctx, atom);
                TC_THROW();
            }
            if (is_strict_mode(ctx)) {
                JS_ThrowReferenceErrorNotDefined(ctx, atom);
                JS_FreeAtom(ctx, atom);
                TC_THROW();
            }
        }
        ret = JS_SetPropertyInternal(ctx, sp[-3], atom, sp[-1], sp[-3], JS_PROP_THROW_STRICT);
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, sp[-2]);
        JS_FreeValue(ctx, sp[-3]);
        sp -= 3;
        if (unlikely(ret < 0))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_put_super_value)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int ret;
        JSAtom atom;
        S->sf->cur_pc = pc;
        if (JS_VALUE_GET_TAG(sp[-3]) != JS_TAG_OBJECT) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            TC_THROW();
        }
        atom = JS_ValueToAtom(ctx, sp[-2]);
        if (unlikely(atom == JS_ATOM_NULL))
            TC_THROW();
        ret = JS_SetPropertyInternal(ctx, sp[-3], atom, sp[-1], sp[-4],
            JS_PROP_THROW_STRICT);
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, sp[-4]);
        JS_FreeValue(ctx, sp[-3]);
        JS_FreeValue(ctx, sp[-2]);
        sp -= 4;
        if (ret < 0)
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_define_array_el)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int ret;
        TC_BB_REC_DROP(sp[-3]);
        ret = JS_DefinePropertyValueValue(ctx, sp[-3], JS_DupValue(ctx, sp[-2]), sp[-1],
            JS_PROP_C_W_E | JS_PROP_THROW);
        sp -= 1;
        if (unlikely(ret < 0))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_append)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        S->sf->cur_pc = pc;
        if (js_append_enumerate(ctx, sp))
            TC_THROW();
        JS_FreeValue(ctx, *--sp);
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_copy_data_properties)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        int mask;

        mask = *pc++;
        S->sf->cur_pc = pc;
        TC_BB_REC_DROP(sp[-1 - (mask & 3)]);
        if (JS_CopyDataProperties(ctx, sp[-1 - (mask & 3)],
                sp[-1 - ((mask >> 2) & 7)],
                sp[-1 - ((mask >> 5) & 7)], 0))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_add_loc_loc)
{
    int opcode = OP_ext;
    pc += 2;
    (void)opcode;
    {
        TC_OP2_READ_LOC_LOC(op1, op2)
        if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
            int64_t r = (int64_t)JS_VALUE_GET_INT(op1) + JS_VALUE_GET_INT(op2);
            pc += 4;
            if (unlikely((int)r != r))
                *sp++ = __JS_NewFloat64(ctx, (double)r);
            else
                *sp++ = JS_NewInt32(ctx, r);
        } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
            double d1, d2;
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)))
                d1 = JS_VALUE_GET_FLOAT64(op1);
            else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT)
                d1 = JS_VALUE_GET_INT(op1);
            else
                goto add_ll_slow;
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2)))
                d2 = JS_VALUE_GET_FLOAT64(op2);
            else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT)
                d2 = JS_VALUE_GET_INT(op2);
            else
                goto add_ll_slow;
            pc += 4;
            *sp++ = __JS_NewFloat64(ctx, d1 + d2);
        } else {
        add_ll_slow:
            pc += 4;
            {
                JSValue ops[2];
                ops[0] = JS_DupValue(ctx, op1);
                ops[1] = JS_DupValue(ctx, op2);
                S->sf->cur_pc = pc;
                if (js_add_slow(ctx, ops + 2))
                    TC_THROW();
                *sp++ = ops[0];
            }
        }
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_sub_loc_loc)
{
    int opcode = OP_ext;
    pc += 2;
    (void)opcode;
    {
        TC_OP2_READ_LOC_LOC(op1, op2)
        if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
            int64_t r = (int64_t)JS_VALUE_GET_INT(op1) - JS_VALUE_GET_INT(op2);
            pc += 4;
            if (unlikely((int)r != r))
                *sp++ = __JS_NewFloat64(ctx, (double)r);
            else
                *sp++ = JS_NewInt32(ctx, r);
        } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
            double d1, d2;
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)))
                d1 = JS_VALUE_GET_FLOAT64(op1);
            else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT)
                d1 = JS_VALUE_GET_INT(op1);
            else
                goto sub_ll_slow;
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2)))
                d2 = JS_VALUE_GET_FLOAT64(op2);
            else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT)
                d2 = JS_VALUE_GET_INT(op2);
            else
                goto sub_ll_slow;
            pc += 4;
            *sp++ = __JS_NewFloat64(ctx, d1 - d2);
        } else {
        sub_ll_slow:
            pc += 4;
            {
                JSValue ops[2];
                ops[0] = JS_DupValue(ctx, op1);
                ops[1] = JS_DupValue(ctx, op2);
                S->sf->cur_pc = pc;
                if (js_binary_arith_slow(ctx, ops + 2, OP_sub))
                    TC_THROW();
                *sp++ = ops[0];
            }
        }
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_mul_loc_loc)
{
    int opcode = OP_ext;
    pc += 2;
    (void)opcode;
    {
        TC_OP2_READ_LOC_LOC(op1, op2)
        double d;
        if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
            int32_t v1 = JS_VALUE_GET_INT(op1);
            int32_t v2 = JS_VALUE_GET_INT(op2);
            int64_t r = (int64_t)v1 * v2;
            if (unlikely((int)r != r)) {
                d = (double)r;
                goto mul_ll_fp;
            }
            if (unlikely(r == 0 && (v1 | v2) < 0)) {
                d = -0.0;
                goto mul_ll_fp;
            }
            pc += 4;
            *sp++ = JS_NewInt32(ctx, r);
        } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
            double d1, d2;
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)))
                d1 = JS_VALUE_GET_FLOAT64(op1);
            else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT)
                d1 = JS_VALUE_GET_INT(op1);
            else
                goto mul_ll_slow;
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2)))
                d2 = JS_VALUE_GET_FLOAT64(op2);
            else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT)
                d2 = JS_VALUE_GET_INT(op2);
            else
                goto mul_ll_slow;
            d = d1 * d2;
        mul_ll_fp:
            pc += 4;
            *sp++ = __JS_NewFloat64(ctx, d);
        } else {
        mul_ll_slow:
            pc += 4;
            {
                JSValue ops[2];
                ops[0] = JS_DupValue(ctx, op1);
                ops[1] = JS_DupValue(ctx, op2);
                S->sf->cur_pc = pc;
                if (js_binary_arith_slow(ctx, ops + 2, OP_mul))
                    TC_THROW();
                *sp++ = ops[0];
            }
        }
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_streq_const_if_false)
{
    int opcode = OP_ext;
    pc += 2;
    (void)opcode;
    {
        JSValue op1 = sp[-1];
        const JSValue op2 = S->cpool[pc[4]];
        int res;
        int32_t diff = (int32_t)get_u32(pc);
        pc += 5;
        TC_OP2_PROBE_STRICT_EQ(op1, op2)
        if (unlikely(!res)) {
            pc += diff - 5;
            if (unlikely(diff <= 0) && unlikely(js_poll_interrupts(ctx)))
                TC_THROW();
        }
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_streq_varprop_if_false_slow)
{
    int opcode = OP_ext;
    pc += 2;
    (void)opcode;
    {
        JSValue op1 = sp[-1], obj, val;
        JSObject* po;
        JSProperty* pr;
        JSShapeProperty* prs;
        JSAtom atom;
        int res;
        BOOL obj_owned = FALSE;
        int32_t diff = (int32_t)get_u32(pc);
        {
            int vr = get_u16(pc + 4);
            atom = get_u32(pc + 6);
            pc += 10;
            obj = *S->var_refs[vr]->pvalue;
            if (unlikely(JS_IsUninitialized(obj))) {
                JSClosureVar* cv = &S->b->closure_var[vr];
                if (cv->is_lexical) {
                    JS_ThrowReferenceErrorUninitialized(ctx,
                        cv->var_name);
                    TC_THROW();
                }
                S->sf->cur_pc = pc;
                obj = JS_GetPropertyInternal(ctx,
                    ctx->global_obj,
                    cv->var_name,
                    ctx->global_obj,
                    1);
                if (unlikely(JS_IsException(obj)))
                    TC_THROW();
                obj_owned = TRUE;
            }
        }
        if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT)) {
            JSCaseIC* ic = &ctx->rt->case_ic[((uintptr_t)pc >> 2) & (JS_CASE_IC_SIZE - 1)];
            JSShape* sh;
            po = JS_VALUE_GET_OBJ(obj);
            sh = po->shape;
            if (ic->pc == pc && ic->idx < (uint32_t)sh->prop_count) {
                prs = &get_shape_prop(sh)[ic->idx];
                if (likely(prs->atom == atom && !(prs->flags & JS_PROP_TMASK))) {
                    val = po->prop[ic->idx].u.value;
                    goto varprop_cmp;
                }
            }
            for (;;) {
                prs = find_own_property(&pr, po, atom);
                if (prs) {
                    if (unlikely(prs->flags & JS_PROP_TMASK))
                        goto varprop_slow;
                    if (po == JS_VALUE_GET_OBJ(obj)) {
                        ic->pc = pc;
                        ic->idx = (uint32_t)(prs - get_shape_prop(po->shape));
                    }
                    val = pr->u.value;
                    break;
                }
                if (unlikely(po->is_exotic)) {
                    if (po->class_id != JS_CLASS_ARRAY || __JS_AtomIsTaggedInt(atom)) {
                        goto varprop_slow;
                    }
                }
                po = po->shape->proto;
                if (!po) {
                    val = JS_UNDEFINED;
                    break;
                }
            }
        varprop_cmp:
            TC_OP2_PROBE_STRICT_EQ(op1, val)
        } else {
        varprop_slow:
            if (!obj_owned) {
                obj = JS_DupValue(ctx, obj);
                obj_owned = TRUE;
            }
            S->sf->cur_pc = pc;
            val = JS_GetPropertyInternal(ctx, obj, atom, obj, 0);
            if (unlikely(JS_IsException(val))) {
                JS_FreeValue(ctx, obj);
                TC_THROW();
            }
            TC_OP2_PROBE_STRICT_EQ(op1, val)
            JS_FreeValue(ctx, val);
        }
        if (obj_owned)
            JS_FreeValue(ctx, obj);
        if (unlikely(!res)) {
            pc += diff - 10;
            if (unlikely(diff <= 0) && unlikely(js_poll_interrupts(ctx)))
                TC_THROW();
        }
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_streq_varprop_if_false)
{
    const uint8_t* np = pc + 12;
    JSValue op1 = sp[-1];
    JSValue obj = *S->var_refs[get_u16(pc + 6)]->pvalue;
    if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT && (JS_VALUE_GET_TAG(op1) == JS_TAG_INT || JS_VALUE_GET_TAG(op1) == JS_TAG_STRING))) {
        JSObject* po = JS_VALUE_GET_OBJ(obj);
        JSShape* sh = po->shape;
        JSCaseIC* ic = &ctx->rt->case_ic[((uintptr_t)np >> 2) & (JS_CASE_IC_SIZE - 1)];
        uint32_t idx = ic->idx;
        if (likely(ic->pc == np && idx < (uint32_t)sh->prop_count)) {
            JSShapeProperty* prs = &get_shape_prop(sh)[idx];
            JSValue val = po->prop[idx].u.value;
            int32_t diff = (int32_t)get_u32(pc + 2);
            if (likely(prs->atom == get_u32(pc + 8) && !(prs->flags & JS_PROP_TMASK) && JS_VALUE_GET_TAG(val) == JS_VALUE_GET_TAG(op1) && diff > 0)) {
                if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT ? JS_VALUE_GET_INT(op1) == JS_VALUE_GET_INT(val) : js_string_eq_quick(ctx, JS_VALUE_GET_STRING(op1), JS_VALUE_GET_STRING(val))) {
                    pc = np;
                    TC_DISPATCH();
                }
                pc += 2 + diff;
                TC_DISPATCH();
            }
        }
    }
    TC_JUMP(js_tc_op2_streq_varprop_if_false_slow);
}

TC_HANDLER(js_tc_op2_call_method0)
{
    int opcode = OP_ext;
    JSValue ret_val;
    JSRuntime* rt = ctx->rt;
    pc += 2;
    (void)opcode;
    TC_OP2_CALL_METHOD(0, 0, 4, 4)
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_call_method1_loc)
{
    int opcode = OP_ext;
    JSValue ret_val;
    JSRuntime* rt = ctx->rt;
    pc += 2;
    (void)opcode;
    TC_OP2_CALL_METHOD(1, 1, 3, 7)
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_call_method1_imm8)
{
    int opcode = OP_ext;
    JSValue ret_val;
    JSRuntime* rt = ctx->rt;
    pc += 2;
    (void)opcode;
    TC_OP2_CALL_METHOD(1, 2, 3, 6)
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_call_method1_const)
{
    int opcode = OP_ext;
    JSValue ret_val;
    JSRuntime* rt = ctx->rt;
    pc += 2;
    (void)opcode;
    TC_OP2_CALL_METHOD(1, 3, 3, 8)
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_check_var)
{
    int idx, ret;
    pc += 2;
    idx = get_u16(pc);
    pc += 2;
    S->sf->cur_pc = pc;
    ret = js_check_global_var(ctx, S->var_refs[idx], S->b->closure_var[idx].var_name);
    if (ret < 0)
        TC_THROW();
    *sp++ = JS_NewBool(ctx, ret);
    *sp++ = JS_UNDEFINED;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_put_var_strict)
{
    int idx, ret;
    pc += 2;
    idx = get_u16(pc);
    pc += 2;
    S->sf->cur_pc = pc;
    ret = js_put_var_strict(ctx, S->var_refs[idx], S->b->closure_var[idx].var_name, sp[-3], sp[-1]);
    sp -= 3;
    if (ret)
        TC_THROW();
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_enter_strict)
{
    pc += 2;
    S->sf->js_mode |= JS_MODE_STRICT;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_leave_strict)
{
    pc += 2;
    S->sf->js_mode = (S->sf->js_mode & ~JS_MODE_STRICT) | (S->b->js_mode & JS_MODE_STRICT);
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_using_new)
{
    int opcode = OP_ext;
    pc += 2;
    (void)opcode;
    {
        JSValue cap = js_using_new_capability(ctx);
        if (JS_IsException(cap))
            TC_THROW();
        *sp++ = cap;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_using_add)
{
    int opcode = OP_ext;
    pc += 2;
    (void)opcode;
    {
        int hint = get_u16(pc);
        pc += 4;
        S->sf->cur_pc = pc;
        if (js_using_add(ctx, sp[-2], sp[-1], hint))
            TC_THROW();
        JS_FreeValue(ctx, sp[-2]);
        sp[-2] = sp[-1];
        sp--;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op2_using_dispose)
{
    int opcode = OP_ext;
    pc += 2;
    (void)opcode;
    {
        int disp_flags = get_u16(pc);
        JSValue ret;
        pc += 4;
        S->sf->cur_pc = pc;
        ret = js_using_dispose_value(ctx, sp[-2],
            (disp_flags & 2) ? sp[-1]
                             : JS_UNINITIALIZED,
            disp_flags);
        JS_FreeValue(ctx, sp[-2]);
        if (!(disp_flags & 2))
            JS_FreeValue(ctx, sp[-1]);
        sp[-2] = ret;
        sp--;
        if (JS_IsException(ret))
            TC_THROW();
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_in)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_operator_in(ctx, sp))
        TC_THROW();
    sp--;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_private_in)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_operator_private_in(ctx, sp))
        TC_THROW();
    sp--;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_instanceof)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_operator_instanceof(ctx, sp))
        TC_THROW();
    sp--;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_typeof)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSValue op1;
        JSAtom atom;

        op1 = sp[-1];
        atom = js_operator_typeof(ctx, op1);
        JS_FreeValue(ctx, op1);
        sp[-1] = js_typeof_string(ctx, atom);
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_delete)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    S->sf->cur_pc = pc;
    if (js_operator_delete(ctx, sp))
        TC_THROW();
    sp--;
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_delete_var)
{
    int opcode = *pc;
    pc++;
    (void)opcode;
    {
        JSAtom atom;
        int ret;

        atom = get_u32(pc);
        pc += 4;
        S->sf->cur_pc = pc;

        ret = JS_DeleteGlobalVar(ctx, atom);
        if (unlikely(ret < 0))
            TC_THROW();
        *sp++ = JS_NewBool(ctx, ret);
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_to_object)
{
    int opcode = *pc;
    JSValue ret_val;
    pc++;
    (void)opcode;
    if (JS_VALUE_GET_TAG(sp[-1]) != JS_TAG_OBJECT) {
        S->sf->cur_pc = pc;
        ret_val = JS_ToObject(ctx, sp[-1]);
        if (JS_IsException(ret_val))
            TC_THROW();
        JS_FreeValue(ctx, sp[-1]);
        sp[-1] = ret_val;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_to_propkey)
{
    int opcode = *pc;
    JSValue ret_val;
    pc++;
    (void)opcode;
    switch (JS_VALUE_GET_TAG(sp[-1])) {
    case JS_TAG_INT:
    case JS_TAG_STRING:
    case JS_TAG_SYMBOL:
        break;
    default:
        S->sf->cur_pc = pc;
        ret_val = JS_ToPropertyKey(ctx, sp[-1]);
        if (JS_IsException(ret_val))
            TC_THROW();
        JS_FreeValue(ctx, sp[-1]);
        sp[-1] = ret_val;
        break;
    }
    TC_DISPATCH();
}

TC_HANDLER(js_tc_op_with_get_ref)
{
    int opcode = *pc;
    pc++;
    {
        JSAtom atom;
        int32_t diff;
        JSValue obj, val;
        int ret, is_with;
        atom = get_u32(pc);
        diff = get_u32(pc + 4);
        is_with = pc[8];
        pc += 9;
        S->sf->cur_pc = pc;

        obj = sp[-1];
        ret = JS_HasProperty(ctx, obj, atom);
        if (unlikely(ret < 0))
            TC_THROW();
        if (ret) {
            if (is_with) {
                ret = js_has_unscopable(ctx, obj, atom);
                if (unlikely(ret < 0))
                    TC_THROW();
                if (ret)
                    goto no_with;
            }
            switch (opcode) {
            case OP_with_get_var:
                ret = JS_HasProperty(ctx, obj, atom);
                if (unlikely(ret <= 0)) {
                    if (ret < 0)
                        TC_THROW();
                    if (is_strict_mode(ctx)) {
                        JS_ThrowReferenceErrorNotDefined(ctx, atom);
                        TC_THROW();
                    }
                    val = JS_UNDEFINED;
                } else {
                    val = JS_GetProperty(ctx, obj, atom);
                    if (unlikely(JS_IsException(val)))
                        TC_THROW();
                }
                set_value(ctx, &sp[-1], val);
                break;
            case OP_with_put_var:
                ret = JS_HasProperty(ctx, obj, atom);
                if (unlikely(ret <= 0)) {
                    if (ret < 0)
                        TC_THROW();
                    if (is_strict_mode(ctx)) {
                        JS_ThrowReferenceErrorNotDefined(ctx, atom);
                        TC_THROW();
                    }
                }
                ret = JS_SetPropertyInternal(ctx, obj, atom, sp[-2], obj,
                    JS_PROP_THROW_STRICT);
                JS_FreeValue(ctx, sp[-1]);
                sp -= 2;
                if (unlikely(ret < 0))
                    TC_THROW();
                break;
            case OP_with_delete_var:
                ret = JS_DeleteProperty(ctx, obj, atom, 0);
                if (unlikely(ret < 0))
                    TC_THROW();
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = JS_NewBool(ctx, ret);
                break;
            case OP_with_make_ref:
                *sp++ = JS_AtomToValue(ctx, atom);
                break;
            case OP_with_get_ref:
                ret = JS_HasProperty(ctx, obj, atom);
                if (unlikely(ret < 0))
                    TC_THROW();
                if (!ret) {
                    val = JS_UNDEFINED;
                } else {
                    val = JS_GetProperty(ctx, obj, atom);
                    if (unlikely(JS_IsException(val)))
                        TC_THROW();
                }
                if (!is_with)
                    set_value(ctx, &sp[-1], JS_UNDEFINED);
                *sp++ = val;
                break;
            }
            pc += diff - 5;
        } else {
        no_with:
            JS_FreeValue(ctx, sp[-1]);
            sp--;
        }
    }
    TC_DISPATCH();
}

#define JS_TC_OPS(X)                                                \
    X(OP_push_minus1, js_tc_push_small)                             \
    X(OP_push_0, js_tc_push_small)                                  \
    X(OP_push_1, js_tc_push_small)                                  \
    X(OP_push_2, js_tc_push_small)                                  \
    X(OP_push_3, js_tc_push_small)                                  \
    X(OP_push_4, js_tc_push_small)                                  \
    X(OP_push_5, js_tc_push_small)                                  \
    X(OP_push_6, js_tc_push_small)                                  \
    X(OP_push_7, js_tc_push_small)                                  \
    X(OP_push_i8, js_tc_push_i8)                                    \
    X(OP_push_i16, js_tc_push_i16)                                  \
    X(OP_push_i32, js_tc_push_i32)                                  \
    X(OP_push_bigint_i32, js_tc_push_bigint_i32)                    \
    X(OP_push_const8, js_tc_push_const8)                            \
    X(OP_push_const, js_tc_push_const)                              \
    X(OP_push_atom_value, js_tc_push_atom_value)                    \
    X(OP_push_empty_string, js_tc_push_empty_string)                \
    X(OP_undefined, js_tc_undefined)                                \
    X(OP_null, js_tc_null)                                          \
    X(OP_push_false, js_tc_push_false)                              \
    X(OP_push_true, js_tc_push_true)                                \
    X(OP_push_this, js_tc_push_this)                                \
    X(OP_drop, js_tc_drop)                                          \
    X(OP_nip, js_tc_nip)                                            \
    X(OP_nip1, js_tc_nip1)                                          \
    X(OP_dup, js_tc_dup)                                            \
    X(OP_dup1, js_tc_dup1)                                          \
    X(OP_dup2, js_tc_dup2)                                          \
    X(OP_dup3, js_tc_dup3)                                          \
    X(OP_insert2, js_tc_insert2)                                    \
    X(OP_insert3, js_tc_insert3)                                    \
    X(OP_insert4, js_tc_insert4)                                    \
    X(OP_perm3, js_tc_perm3)                                        \
    X(OP_perm4, js_tc_perm4)                                        \
    X(OP_perm5, js_tc_perm5)                                        \
    X(OP_rot3l, js_tc_rot3l)                                        \
    X(OP_rot4l, js_tc_rot4l)                                        \
    X(OP_rot5l, js_tc_rot5l)                                        \
    X(OP_rot3r, js_tc_rot3r)                                        \
    X(OP_swap, js_tc_swap)                                          \
    X(OP_swap2, js_tc_swap2)                                        \
    X(OP_get_loc0, js_tc_get_loc0)                                  \
    X(OP_get_loc1, js_tc_get_loc1)                                  \
    X(OP_get_loc2, js_tc_get_loc2)                                  \
    X(OP_get_loc3, js_tc_get_loc3)                                  \
    X(OP_get_loc, js_tc_get_loc)                                    \
    X(OP_put_loc0, js_tc_put_loc0)                                  \
    X(OP_put_loc1, js_tc_put_loc1)                                  \
    X(OP_put_loc2, js_tc_put_loc2)                                  \
    X(OP_put_loc3, js_tc_put_loc3)                                  \
    X(OP_put_loc, js_tc_put_loc)                                    \
    X(OP_set_loc0, js_tc_set_loc0)                                  \
    X(OP_set_loc1, js_tc_set_loc1)                                  \
    X(OP_set_loc2, js_tc_set_loc2)                                  \
    X(OP_set_loc3, js_tc_set_loc3)                                  \
    X(OP_set_loc, js_tc_set_loc)                                    \
    X(OP_get_arg0, js_tc_get_arg0)                                  \
    X(OP_get_arg1, js_tc_get_arg1)                                  \
    X(OP_get_arg2, js_tc_get_arg2)                                  \
    X(OP_get_arg3, js_tc_get_arg3)                                  \
    X(OP_get_arg, js_tc_get_arg)                                    \
    X(OP_put_arg0, js_tc_put_arg0)                                  \
    X(OP_put_arg1, js_tc_put_arg1)                                  \
    X(OP_put_arg2, js_tc_put_arg2)                                  \
    X(OP_put_arg3, js_tc_put_arg3)                                  \
    X(OP_put_arg, js_tc_put_arg)                                    \
    X(OP_set_arg0, js_tc_set_arg0)                                  \
    X(OP_set_arg1, js_tc_set_arg1)                                  \
    X(OP_set_arg2, js_tc_set_arg2)                                  \
    X(OP_set_arg3, js_tc_set_arg3)                                  \
    X(OP_set_arg, js_tc_set_arg)                                    \
    X(OP_get_loc8, js_tc_get_loc8)                                  \
    X(OP_get_var_ref0, js_tc_get_var_ref0)                          \
    X(OP_get_var_ref1, js_tc_get_var_ref1)                          \
    X(OP_get_var_ref2, js_tc_get_var_ref2)                          \
    X(OP_get_var_ref3, js_tc_get_var_ref3)                          \
    X(OP_get_var_ref, js_tc_get_var_ref)                            \
    X(OP_put_loc8, js_tc_put_loc8)                                  \
    X(OP_put_var_ref0, js_tc_put_var_ref0)                          \
    X(OP_put_var_ref1, js_tc_put_var_ref1)                          \
    X(OP_put_var_ref2, js_tc_put_var_ref2)                          \
    X(OP_put_var_ref3, js_tc_put_var_ref3)                          \
    X(OP_put_var_ref, js_tc_put_var_ref)                            \
    X(OP_set_loc8, js_tc_set_loc8)                                  \
    X(OP_set_var_ref0, js_tc_set_var_ref0)                          \
    X(OP_set_var_ref1, js_tc_set_var_ref1)                          \
    X(OP_set_var_ref2, js_tc_set_var_ref2)                          \
    X(OP_set_var_ref3, js_tc_set_var_ref3)                          \
    X(OP_set_var_ref, js_tc_set_var_ref)                            \
    X(OP_get_loc_check, js_tc_get_loc_check)                        \
    X(OP_get_loc_checkthis, js_tc_get_loc_checkthis)                \
    X(OP_put_loc_check, js_tc_put_loc_check)                        \
    X(OP_set_loc_check, js_tc_set_loc_check)                        \
    X(OP_put_loc_check_init, js_tc_put_loc_check_init)              \
    X(OP_set_loc_uninitialized, js_tc_set_loc_uninitialized)        \
    X(OP_get_var_ref_check, js_tc_get_var_ref_check)                \
    X(OP_put_var_ref_check, js_tc_put_var_ref_check)                \
    X(OP_put_var_ref_check_init, js_tc_put_var_ref_check_init)      \
    X(OP_close_loc, js_tc_close_loc)                                \
    X(OP_goto, js_tc_goto)                                          \
    X(OP_goto16, js_tc_goto16)                                      \
    X(OP_goto8, js_tc_goto8)                                        \
    X(OP_if_true, js_tc_if_true)                                    \
    X(OP_if_false, js_tc_if_false)                                  \
    X(OP_if_true8, js_tc_if_true8)                                  \
    X(OP_if_false8, js_tc_if_false8)                                \
    X(OP_lnot, js_tc_lnot)                                          \
    X(OP_add, js_tc_add)                                            \
    X(OP_sub, js_tc_sub)                                            \
    X(OP_mul, js_tc_mul)                                            \
    X(OP_div, js_tc_div)                                            \
    X(OP_mod, js_tc_mod)                                            \
    X(OP_pow, js_tc_pow)                                            \
    X(OP_add_loc, js_tc_add_loc)                                    \
    X(OP_plus, js_tc_plus)                                          \
    X(OP_neg, js_tc_neg)                                            \
    X(OP_inc, js_tc_inc)                                            \
    X(OP_dec, js_tc_dec)                                            \
    X(OP_post_inc, js_tc_post_inc)                                  \
    X(OP_post_dec, js_tc_post_dec)                                  \
    X(OP_inc_loc, js_tc_inc_loc)                                    \
    X(OP_dec_loc, js_tc_dec_loc)                                    \
    X(OP_not, js_tc_not)                                            \
    X(OP_shl, js_tc_shl)                                            \
    X(OP_shr, js_tc_shr)                                            \
    X(OP_sar, js_tc_sar)                                            \
    X(OP_and, js_tc_and)                                            \
    X(OP_or, js_tc_or)                                              \
    X(OP_xor, js_tc_xor)                                            \
    X(OP_lt, js_tc_lt)                                              \
    X(OP_lte, js_tc_lte)                                            \
    X(OP_gt, js_tc_gt)                                              \
    X(OP_gte, js_tc_gte)                                            \
    X(OP_lt_if_false, js_tc_lt_if_false)                            \
    X(OP_lte_if_false, js_tc_lte_if_false)                          \
    X(OP_gt_if_false, js_tc_gt_if_false)                            \
    X(OP_gte_if_false, js_tc_gte_if_false)                          \
    X(OP_strict_eq_if_false, js_tc_strict_eq_if_false)              \
    X(OP_strict_neq_if_false, js_tc_strict_neq_if_false)            \
    X(OP_strict_eq_if_true, js_tc_strict_eq_if_true)                \
    X(OP_strict_neq_if_true, js_tc_strict_neq_if_true)              \
    X(OP_strict_eq, js_tc_strict_eq)                                \
    X(OP_strict_neq, js_tc_strict_neq)                              \
    X(OP_eq, js_tc_eq)                                              \
    X(OP_neq, js_tc_neq)                                            \
    X(OP_get_array_el, js_tc_get_array_el)                          \
    X(OP_get_array_el2, js_tc_get_array_el2)                        \
    X(OP_get_array_el3, js_tc_get_array_el3)                        \
    X(OP_put_array_el, js_tc_put_array_el)                          \
    X(OP_get_field, js_tc_get_field)                                \
    X(OP_get_field2, js_tc_get_field2)                              \
    X(OP_get_length, js_tc_get_length)                              \
    X(OP_put_field, js_tc_put_field)                                \
    X(OP_get_var, js_tc_get_var)                                    \
    X(OP_get_var_undef, js_tc_get_var)                              \
    X(OP_put_var, js_tc_put_var)                                    \
    X(OP_put_var_init, js_tc_put_var)                               \
    X(OP_fclosure, js_tc_fclosure)                                  \
    X(OP_fclosure8, js_tc_fclosure)                                 \
    X(OP_call0, js_tc_call_small)                                   \
    X(OP_call1, js_tc_call_small)                                   \
    X(OP_call2, js_tc_call_small)                                   \
    X(OP_call3, js_tc_call_small)                                   \
    X(OP_call, js_tc_call)                                          \
    X(OP_call_method, js_tc_call_method)                            \
    X(OP_call_constructor, js_tc_call_constructor)                  \
    X(OP_array_from, js_tc_array_from)                              \
    X(OP_apply, js_tc_apply)                                        \
    X(OP_return, js_tc_return)                                      \
    X(OP_return_undef, js_tc_return_undef)                          \
    X(OP_object, js_tc_op_object)                                   \
    X(OP_special_object, js_tc_op_special_object)                   \
    X(OP_rest, js_tc_op_rest)                                       \
    X(OP_check_ctor_return, js_tc_op_check_ctor_return)             \
    X(OP_check_ctor, js_tc_op_check_ctor)                           \
    X(OP_init_ctor, js_tc_op_init_ctor)                             \
    X(OP_check_brand, js_tc_op_check_brand)                         \
    X(OP_add_brand, js_tc_op_add_brand)                             \
    X(OP_throw, js_tc_op_throw)                                     \
    X(OP_throw_error, js_tc_op_throw_error)                         \
    X(OP_eval, js_tc_op_eval)                                       \
    X(OP_apply_eval, js_tc_op_apply_eval)                           \
    X(OP_regexp, js_tc_op_regexp)                                   \
    X(OP_get_super, js_tc_op_get_super)                             \
    X(OP_import, js_tc_op_import)                                   \
    X(OP_make_var_ref_ref, js_tc_op_make_var_ref_ref)               \
    X(OP_make_var_ref, js_tc_op_make_var_ref)                       \
    X(OP_switch, js_tc_op_switch)                                   \
    X(OP_catch, js_tc_op_catch)                                     \
    X(OP_for_await_catch_body, js_tc_op_for_await_catch_body)       \
    X(OP_gosub, js_tc_op_gosub)                                     \
    X(OP_ret, js_tc_op_ret)                                         \
    X(OP_for_in_start, js_tc_op_for_in_start)                       \
    X(OP_for_in_next, js_tc_op_for_in_next)                         \
    X(OP_for_of_start, js_tc_op_for_of_start)                       \
    X(OP_for_of_next, js_tc_op_for_of_next)                         \
    X(OP_for_await_of_next, js_tc_op_for_await_of_next)             \
    X(OP_for_await_of_start, js_tc_op_for_await_of_start)           \
    X(OP_iterator_get_value_done, js_tc_op_iterator_get_value_done) \
    X(OP_iterator_check_object, js_tc_op_iterator_check_object)     \
    X(OP_iterator_close, js_tc_op_iterator_close)                   \
    X(OP_nip_catch, js_tc_op_nip_catch)                             \
    X(OP_iterator_next, js_tc_op_iterator_next)                     \
    X(OP_iterator_call, js_tc_op_iterator_call)                     \
    X(OP_private_symbol, js_tc_op_private_symbol)                   \
    X(OP_get_private_field, js_tc_op_get_private_field)             \
    X(OP_put_private_field, js_tc_op_put_private_field)             \
    X(OP_define_private_field, js_tc_op_define_private_field)       \
    X(OP_define_field, js_tc_op_define_field)                       \
    X(OP_set_name, js_tc_op_set_name)                               \
    X(OP_set_name_computed, js_tc_op_set_name_computed)             \
    X(OP_set_proto, js_tc_op_set_proto)                             \
    X(OP_set_home_object, js_tc_op_set_home_object)                 \
    X(OP_define_method_computed, js_tc_op_define_method_computed)   \
    X(OP_define_class_computed, js_tc_op_define_class_computed)     \
    X(OP_get_ref_value, js_tc_op_get_ref_value)                     \
    X(OP_get_super_value, js_tc_op_get_super_value)                 \
    X(OP_put_ref_value, js_tc_op_put_ref_value)                     \
    X(OP_put_super_value, js_tc_op_put_super_value)                 \
    X(OP_define_array_el, js_tc_op_define_array_el)                 \
    X(OP_append, js_tc_op_append)                                   \
    X(OP_copy_data_properties, js_tc_op_copy_data_properties)       \
    X(OP_in, js_tc_op_in)                                           \
    X(OP_private_in, js_tc_op_private_in)                           \
    X(OP_instanceof, js_tc_op_instanceof)                           \
    X(OP_typeof, js_tc_op_typeof)                                   \
    X(OP_delete, js_tc_op_delete)                                   \
    X(OP_delete_var, js_tc_op_delete_var)                           \
    X(OP_to_object, js_tc_op_to_object)                             \
    X(OP_to_propkey, js_tc_op_to_propkey)                           \
    X(OP_with_get_ref, js_tc_op_with_get_ref)                       \
    X(OP_make_loc_ref, js_tc_op_make_var_ref_ref)                   \
    X(OP_make_arg_ref, js_tc_op_make_var_ref_ref)                   \
    X(OP_define_method, js_tc_op_define_method_computed)            \
    X(OP_define_class, js_tc_op_define_class_computed)              \
    X(OP_with_get_var, js_tc_op_with_get_ref)                       \
    X(OP_with_put_var, js_tc_op_with_get_ref)                       \
    X(OP_with_delete_var, js_tc_op_with_get_ref)                    \
    X(OP_with_make_ref, js_tc_op_with_get_ref)                      \
    X(OP_await, js_tc_op_await)                                     \
    X(OP_yield, js_tc_op_yield)                                     \
    X(OP_yield_star, js_tc_op_yield_star)                           \
    X(OP_async_yield_star, js_tc_op_yield_star)                     \
    X(OP_return_async, js_tc_op_return_async)                       \
    X(OP_initial_yield, js_tc_op_initial_yield)                     \
    X(OP_nop, js_tc_op_nop)                                         \
    X(OP_invalid, js_tc_op_invalid)                                 \
    X(OP_is_undefined_or_null, js_tc_op_is_undefined_or_null)       \
    X(OP_is_undefined, js_tc_op_is_undefined)                       \
    X(OP_is_null, js_tc_op_is_null)                                 \
    X(OP_typeof_is_undefined, js_tc_op_typeof_is_undefined)         \
    X(OP_typeof_is_function, js_tc_op_typeof_is_function)           \
    X(OP_tail_call, js_tc_op_tail_call)                             \
    X(OP_tail_call_method, js_tc_op_tail_call_method)               \
    X(OP_ext, js_tc_op_ext)

#define JS_TC_OPS2(X)                                               \
    X(OP2_add_loc_loc, js_tc_op2_add_loc_loc)                       \
    X(OP2_sub_loc_loc, js_tc_op2_sub_loc_loc)                       \
    X(OP2_mul_loc_loc, js_tc_op2_mul_loc_loc)                       \
    X(OP2_streq_const_if_false, js_tc_op2_streq_const_if_false)     \
    X(OP2_streq_varprop_if_false, js_tc_op2_streq_varprop_if_false) \
    X(OP2_call_method0, js_tc_op2_call_method0)                     \
    X(OP2_call_method1_loc, js_tc_op2_call_method1_loc)             \
    X(OP2_call_method1_imm8, js_tc_op2_call_method1_imm8)           \
    X(OP2_call_method1_const, js_tc_op2_call_method1_const)         \
    X(OP2_using_new, js_tc_op2_using_new)                           \
    X(OP2_using_add, js_tc_op2_using_add)                           \
    X(OP2_using_dispose, js_tc_op2_using_dispose)                   \
    X(OP2_check_var, js_tc_op2_check_var)                           \
    X(OP2_put_var_strict, js_tc_op2_put_var_strict) \
    X(OP2_enter_strict, js_tc_op2_enter_strict) \
    X(OP2_leave_strict, js_tc_op2_leave_strict)

#define JS_TC_TABLE_ENTRY(op, h) [op] = { h },
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winitializer-overrides"
static const JSTCDisp js_tc_table[256] = {
    [0 ... 255] = { js_tc_op_invalid },
    JS_TC_OPS(JS_TC_TABLE_ENTRY)
        [TC_OP_ITER] = { js_tc_iter_next },
};
static const JSTCDisp js_tc_table2[256] = {
    [0 ... 255] = { js_tc_op2_invalid },
    JS_TC_OPS2(JS_TC_TABLE_ENTRY)
};
#pragma clang diagnostic pop

static no_inline int js_tc_enter(JSTCState* S, const uint8_t* pc, JSValue* sp, JSValue* lb, JSValue* ab, JSContext* ctx)
{
    return js_tc_table[*pc].h(S, pc, sp, lb, ab, ctx, js_tc_table);
}

static no_inline int js_tc_enter_block(JSTCState* S, const uint8_t* pc, JSValue* sp, JSValue* lb, JSValue* ab, JSContext* ctx)
{
    js_tcv_entry_clamp(ctx->rt);
    return js_tc_table[*pc].h(S, pc, sp, lb, ab, ctx, js_tc_table);
}

#pragma clang diagnostic ignored "-Winitializer-overrides"
#pragma clang diagnostic ignored "-Wunused-label"
#define JS_TC_REDIRECTS , [0 ... 255] = &&tc_enter
#define JS_TC_FAST_ENTRY(x)                                                                         \
    ({                                                                                              \
        JSFunctionBytecode* js_tc_b_ = (x);                                                         \
        if (likely(js_tc_b_->func_kind == JS_FUNC_NORMAL))                                          \
            return js_call_tc_entry(caller_ctx, func_obj, this_obj, new_target, argc, argv, flags); \
        js_tc_b_;                                                                                   \
    })
#if CONFIG_LITERAL_BOILERPLATE
#define JS_TC_BB_IN(t)           \
    t.bb_rec_obj = bb_rec_obj;   \
    t.bb_rec_site = bb_rec_site; \
    t.bb_rec_off = bb_rec_off;   \
    t.bb_rec_nf = bb_rec_nf;
#define JS_TC_BB_OUT(t)          \
    bb_rec_obj = t.bb_rec_obj;   \
    bb_rec_site = t.bb_rec_site; \
    bb_rec_off = t.bb_rec_off;   \
    bb_rec_nf = t.bb_rec_nf;
#else
#define JS_TC_BB_IN(t)
#define JS_TC_BB_OUT(t)
#endif
#define JS_TC_ENTER_BLOCK                                           \
    tc_enter:                                                       \
    {                                                               \
        JSTCState tcs;                                              \
        int tcr;                                                    \
        tcs.sf = sf;                                                \
        tcs.b = b;                                                  \
        tcs.cpool = cpool;                                          \
        tcs.var_refs = var_refs;                                    \
        tcs.this_obj = this_obj;                                    \
        tcs.caller_ctx = caller_ctx;                                \
        tcs.new_target = new_target;                                \
        tcs.argc = argc;                                            \
        tcs.argv = argv;                                            \
        tcs.local_buf = local_buf;                                  \
        tcs.stack_buf = stack_buf;                                  \
        tcs.p = p;                                                  \
        tcs.alloca_capacity = alloca_capacity;                      \
        tcs.up = NULL;                                              \
        tcs.fr_flags = 0;                                           \
        JS_TC_BB_IN(tcs)                                            \
        tcr = js_tc_enter_block(&tcs, pc - 1, sp, var_buf, arg_buf, ctx); \
        pc = tcs.pc;                                                \
        sp = tcs.sp;                                                \
        JS_TC_BB_OUT(tcs)                                           \
        switch (tcr) {                                              \
        case 2:                                                     \
            ret_val = tcs.ret;                                      \
            goto done;                                              \
        case 3:                                                     \
            ret_val = tcs.ret;                                      \
            goto done_generator;                                    \
        case 4:                                                     \
            tc_func = tcs.tc_func;                                  \
            tc_this = tcs.tc_this;                                  \
            tc_argv = tcs.tc_argv;                                  \
            tc_argc = tcs.tc_argc;                                  \
            tc_arg_alloc = tcs.tc_arg_alloc;                        \
            tc_pending = TRUE;                                      \
            goto done;                                              \
        default:                                                    \
            goto exception;                                         \
        }                                                           \
    }

#endif
