static JSValue js_typeof_string(JSContext* ctx, JSAtom atom)
{
    JSRuntime* rt = ctx->rt;
    int idx;

    switch (atom) {
    case JS_ATOM_undefined:
        idx = 0;
        break;
    case JS_ATOM_function:
        idx = 1;
        break;
    case JS_ATOM_object:
        idx = 2;
        break;
    case JS_ATOM_string:
        idx = 3;
        break;
    case JS_ATOM_number:
        idx = 4;
        break;
    case JS_ATOM_boolean:
        idx = 5;
        break;
    case JS_ATOM_symbol:
        idx = 6;
        break;
    case JS_ATOM_bigint:
        idx = 7;
        break;
    default:
        return JS_AtomToString(ctx, atom);
    }
    if (unlikely(JS_VALUE_GET_TAG(rt->typeof_strings[idx]) != JS_TAG_STRING)) {
        JSValue str = JS_AtomToString(ctx, atom);
        if (JS_IsException(str))
            return str;
        rt->typeof_strings[idx] = str;
    }
    return JS_DupValue(ctx, rt->typeof_strings[idx]);
}

static int js_using_add(JSContext* ctx, JSValueConst recs_val,
    JSValueConst value, int hint);
static JSValue js_using_dispose_value(JSContext* ctx, JSValueConst recs_val,
    JSValue pending, int async);
static JSValue js_using_new_capability(JSContext* ctx);

static JSValue JS_CallInternal(JSContext* caller_ctx, JSValueConst func_obj,
    JSValueConst this_obj, JSValueConst new_target,
    int argc, JSValue* argv, int flags)
{
    JSRuntime* rt = caller_ctx->rt;
    JSContext* ctx;
    JSObject* p;
    JSFunctionBytecode* b;
    JSStackFrame sf_s, *sf = &sf_s;
    const uint8_t* pc;
    int opcode, arg_allocated_size, i;
    JSValue *local_buf, *stack_buf, *var_buf, *arg_buf, *sp, ret_val, *pval;
    JSVarRef** var_refs;
    size_t alloca_size;
    size_t alloca_capacity = 0;
    JSValue tc_func, tc_this, tc_owner_func = JS_UNDEFINED,
                              tc_owner_this = JS_UNDEFINED, *tc_argv;
    int tc_argc, tc_arg_alloc, tc_pending = FALSE;
    BOOL tc_entry = FALSE, tc_argv_live = FALSE;
#if CONFIG_LITERAL_BOILERPLATE
    JSObject* bb_rec_obj = NULL;
    JSObjectPresizeSite* bb_rec_site = NULL;
    uint32_t bb_rec_off = 0;
    int bb_rec_nf = 0;
#define BB_REC_START(val, off_)             \
    do {                                    \
        bb_rec_obj = JS_VALUE_GET_OBJ(val); \
        bb_rec_off = (off_);                \
        bb_rec_site = NULL;                 \
        bb_rec_nf = 0;                      \
    } while (0)
#define BB_REC_CLEAR()      \
    do {                    \
        bb_rec_obj = NULL;  \
        bb_rec_site = NULL; \
    } while (0)
#endif

#if !DIRECT_DISPATCH
#define SWITCH(pc) switch (opcode = *pc++)
#define CASE(op) case op
#define DEFAULT default
#define BREAK break
#define SWITCH2(op2) switch (op2)
#define CASE2(op) case op
#else
    static const void* const dispatch_table[256] = {
#define DEF(id, size, n_pop, n_push, f) &&case_OP_##id,
#if SHORT_OPCODES
#define def(id, size, n_pop, n_push, f)
#else
#define def(id, size, n_pop, n_push, f) &&case_default,
#endif
#include "dyna-opcode.h"
        [OP_COUNT... 255] = &&case_default
    };
    static const void* const dispatch_table2[256] = {
#define DEF2(id, size, n_pop, n_push, f) &&case_OP2_##id,
#include "dyna-opcode2.h"
#undef DEF2
        [OP2_COUNT... 255] = &&case_default
    };
#define SWITCH(pc) goto* dispatch_table[opcode = *pc++];
#define SWITCH2(op2) goto* dispatch_table2[op2];
#ifdef OPCODE_ASM_LABEL
#define CASE(op)                                                       \
    case_##op : __asm__ volatile("label_" #op ":\n.globl label_" #op); \
    dummy_case_##op
#else
#define CASE(op) case_##op
#endif
#define DEFAULT case_default
#define CASE2(op) case_##op
#define BREAK SWITCH(pc)
#endif

    if (js_poll_interrupts(caller_ctx))
        return JS_EXCEPTION;
    if (unlikely(JS_VALUE_GET_TAG(func_obj) != JS_TAG_OBJECT)) {
        if (flags & JS_CALL_FLAG_GENERATOR) {
            JSAsyncFunctionState* s = JS_VALUE_GET_PTR(func_obj);
            sf = &s->frame;
            p = JS_VALUE_GET_OBJ(sf->cur_func);
            b = p->u.func.function_bytecode;
            ctx = b->realm;
            var_refs = p->u.func.var_refs;
            local_buf = arg_buf = sf->arg_buf;
            var_buf = sf->var_buf;
            stack_buf = sf->var_buf + b->var_count;
            sp = sf->cur_sp;
            sf->cur_sp = NULL;
            pc = sf->cur_pc;
            sf->prev_frame = rt->current_stack_frame;
            rt->current_stack_frame = sf;
            if (s->throw_flag)
                goto exception;
            else
                goto restart;
        } else {
            goto not_a_function;
        }
    }
    p = JS_VALUE_GET_OBJ(func_obj);
    if (unlikely(p->class_id != JS_CLASS_BYTECODE_FUNCTION)) {
        JSClassCall* call_func;
        call_func = rt->class_array[p->class_id].call;
        if (!call_func) {
        not_a_function:
            return JS_ThrowTypeError(caller_ctx, "not a function");
        }
        return call_func(caller_ctx, func_obj, this_obj, argc,
            (JSValueConst*)argv, flags);
    }
    b = p->u.func.function_bytecode;

    if (unlikely(argc < b->arg_count || (flags & JS_CALL_FLAG_COPY_ARGV))) {
        arg_allocated_size = b->arg_count;
    } else {
        arg_allocated_size = 0;
    }

    alloca_size = sizeof(JSValue) * (arg_allocated_size + b->var_count + b->stack_size) + sizeof(JSVarRef*) * b->var_ref_count;
    if (js_check_stack_overflow(rt, alloca_size))
        return JS_ThrowStackOverflow(caller_ctx);

    local_buf = alloca(alloca_size);
    alloca_capacity = alloca_size;
restart_call:
    sf->js_mode = b->js_mode;
    arg_buf = argv;
    sf->arg_count = argc;
    sf->cur_func = (JSValue)func_obj;
    var_refs = p->u.func.var_refs;

    if (unlikely(tc_entry)) {
        tc_entry = FALSE;
        if (arg_allocated_size == 0) {
            arg_buf = tc_argv;
            tc_argv_live = TRUE;
        } else {
            arg_buf = local_buf;
            for (i = 0; i < argc; i++)
                arg_buf[i] = argv[i];
            for (; i < arg_allocated_size; i++)
                arg_buf[i] = JS_UNDEFINED;
            argv = arg_buf;
            js_free(ctx, tc_argv);
        }
    } else if (unlikely(arg_allocated_size)) {
        int n = min_int(argc, b->arg_count);
        arg_buf = local_buf;
        {
            for (i = 0; i < n; i++)
                arg_buf[i] = JS_DupValue(caller_ctx, argv[i]);
            for (; i < b->arg_count; i++)
                arg_buf[i] = JS_UNDEFINED;
            sf->arg_count = b->arg_count;
        }
    }
    var_buf = local_buf + arg_allocated_size;
    sf->var_buf = var_buf;
    sf->arg_buf = arg_buf;

    for (i = 0; i < b->var_count; i++)
        var_buf[i] = JS_UNDEFINED;

    stack_buf = var_buf + b->var_count;
    sf->var_refs = (JSVarRef**)(stack_buf + b->stack_size);
    for (i = 0; i < b->var_ref_count; i++)
        sf->var_refs[i] = NULL;
    sp = stack_buf;
    pc = b->byte_code_buf;
    sf->prev_frame = rt->current_stack_frame;
    rt->current_stack_frame = sf;
    ctx = b->realm;

restart:
    for (;;) {
        int call_argc;
        JSValue* call_argv;

        SWITCH(pc)
        {
            CASE(OP_push_i32)
                : * sp++ = JS_NewInt32(ctx, get_u32(pc));
            pc += 4;
            BREAK;
            CASE(OP_push_bigint_i32)
                : * sp++ = __JS_NewShortBigInt(ctx, (int)get_u32(pc));
            pc += 4;
            BREAK;
            CASE(OP_push_const)
                : * sp++ = JS_DupValue(ctx, b->cpool[get_u32(pc)]);
            pc += 4;
            BREAK;
#if SHORT_OPCODES
            CASE(OP_push_minus1)
                : CASE(OP_push_0)
                : CASE(OP_push_1)
                : CASE(OP_push_2)
                : CASE(OP_push_3)
                : CASE(OP_push_4)
                : CASE(OP_push_5)
                : CASE(OP_push_6)
                : CASE(OP_push_7)
                : * sp++ = JS_NewInt32(ctx, opcode - OP_push_0);
            BREAK;
            CASE(OP_push_i8)
                : * sp++ = JS_NewInt32(ctx, get_i8(pc));
            pc += 1;
            BREAK;
            CASE(OP_push_i16)
                : * sp++ = JS_NewInt32(ctx, get_i16(pc));
            pc += 2;
            BREAK;
            CASE(OP_push_const8)
                : * sp++ = JS_DupValue(ctx, b->cpool[*pc++]);
            BREAK;
            CASE(OP_fclosure8)
                : * sp++ = js_closure(ctx, JS_DupValue(ctx, b->cpool[*pc++]), var_refs, sf, FALSE);
            if (unlikely(JS_IsException(sp[-1])))
                goto exception;
            BREAK;
            CASE(OP_push_empty_string)
                : * sp++ = JS_AtomToString(ctx, JS_ATOM_empty_string);
            BREAK;
#endif
            CASE(OP_push_atom_value)
                : * sp++ = JS_AtomToValue(ctx, get_u32(pc));
            pc += 4;
            BREAK;
            CASE(OP_undefined)
                : * sp++ = JS_UNDEFINED;
            BREAK;
            CASE(OP_null)
                : * sp++ = JS_NULL;
            BREAK;
            CASE(OP_push_this)
                :
            {
                JSValue val;
                if (!(b->js_mode & JS_MODE_STRICT)) {
                    uint32_t tag = JS_VALUE_GET_TAG(this_obj);
                    if (likely(tag == JS_TAG_OBJECT))
                        goto normal_this;
                    if (tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED) {
                        val = JS_DupValue(ctx, ctx->global_obj);
                    } else {
                        val = JS_ToObject(ctx, this_obj);
                        if (JS_IsException(val))
                            goto exception;
                    }
                } else {
                normal_this:
                    val = JS_DupValue(ctx, this_obj);
                }
                *sp++ = val;
            }
            BREAK;
            CASE(OP_push_false)
                : * sp++ = JS_FALSE;
            BREAK;
            CASE(OP_push_true)
                : * sp++ = JS_TRUE;
            BREAK;
            CASE(OP_object)
                :
#if CONFIG_PRESIZE_LITERAL
            {
                JSObjectPresize* ps = b->obj_presize;
                if (unlikely(ps != NULL)) {
                    uint32_t off = (uint32_t)(pc - 1 - b->byte_code_buf);
                    int si;

                    for (si = 0; si < ps->site_count; si++) {
                        if (ps->sites[si].bc_offset == off) {
                            *sp++ = js_object_presize_new(ctx,
                                &ps->sites[si]);
                            if (unlikely(JS_IsException(sp[-1])))
                                goto exception;
#if CONFIG_LITERAL_BOILERPLATE
                            bb_rec_obj = JS_VALUE_GET_OBJ(sp[-1]);
                            bb_rec_off = off;
                            bb_rec_site = &ps->sites[si];
                            bb_rec_nf = 0;
#endif
                            BREAK;
                        }
                    }
                }
            }
#endif
            *sp++ = JS_NewObject(ctx);
            if (unlikely(JS_IsException(sp[-1])))
                goto exception;
#if CONFIG_LITERAL_BOILERPLATE
            BB_REC_START(sp[-1], (uint32_t)(pc - 1 - b->byte_code_buf));
#endif
            BREAK;
            CASE(OP_special_object)
                :
            {
                int arg = *pc++;
                switch (arg) {
                case OP_SPECIAL_OBJECT_ARGUMENTS:
                    *sp++ = js_build_arguments(ctx, argc, (JSValueConst*)argv);
                    if (unlikely(JS_IsException(sp[-1])))
                        goto exception;
                    break;
                case OP_SPECIAL_OBJECT_MAPPED_ARGUMENTS:
                    *sp++ = js_build_mapped_arguments(ctx, argc, (JSValueConst*)argv,
                        sf, min_int(argc, b->arg_count));
                    if (unlikely(JS_IsException(sp[-1])))
                        goto exception;
                    break;
                case OP_SPECIAL_OBJECT_THIS_FUNC:
                    *sp++ = JS_DupValue(ctx, sf->cur_func);
                    break;
                case OP_SPECIAL_OBJECT_NEW_TARGET:
                    *sp++ = JS_DupValue(ctx, new_target);
                    break;
                case OP_SPECIAL_OBJECT_HOME_OBJECT: {
                    JSObject* p1;
                    p1 = p->u.func.home_object;
                    if (unlikely(!p1))
                        *sp++ = JS_UNDEFINED;
                    else
                        *sp++ = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p1));
                } break;
                case OP_SPECIAL_OBJECT_VAR_OBJECT:
                    *sp++ = JS_NewObjectProto(ctx, JS_NULL);
                    if (unlikely(JS_IsException(sp[-1])))
                        goto exception;
                    break;
                case OP_SPECIAL_OBJECT_IMPORT_META:
                    *sp++ = js_import_meta(ctx);
                    if (unlikely(JS_IsException(sp[-1])))
                        goto exception;
                    break;
                default:
                    abort();
                }
            }
            BREAK;
            CASE(OP_rest)
                :
            {
                int first = get_u16(pc);
                pc += 2;
                first = min_int(first, argc);
                *sp++ = js_create_array(ctx, argc - first, (JSValueConst*)(argv + first));
                if (unlikely(JS_IsException(sp[-1])))
                    goto exception;
            }
            BREAK;

            CASE(OP_drop)
                : JS_FreeValue(ctx, sp[-1]);
            sp--;
            BREAK;
            CASE(OP_nip)
                : JS_FreeValue(ctx, sp[-2]);
            sp[-2] = sp[-1];
            sp--;
            BREAK;
            CASE(OP_nip1)
                : JS_FreeValue(ctx, sp[-3]);
            sp[-3] = sp[-2];
            sp[-2] = sp[-1];
            sp--;
            BREAK;
            CASE(OP_dup)
                : sp[0] = JS_DupValue(ctx, sp[-1]);
            sp++;
            BREAK;
            CASE(OP_dup2)
                : sp[0] = JS_DupValue(ctx, sp[-2]);
            sp[1] = JS_DupValue(ctx, sp[-1]);
            sp += 2;
            BREAK;
            CASE(OP_dup3)
                : sp[0] = JS_DupValue(ctx, sp[-3]);
            sp[1] = JS_DupValue(ctx, sp[-2]);
            sp[2] = JS_DupValue(ctx, sp[-1]);
            sp += 3;
            BREAK;
            CASE(OP_dup1)
                : sp[0] = sp[-1];
            sp[-1] = JS_DupValue(ctx, sp[-2]);
            sp++;
            BREAK;
            CASE(OP_insert2)
                : sp[0] = sp[-1];
            sp[-1] = sp[-2];
            sp[-2] = JS_DupValue(ctx, sp[0]);
            sp++;
            BREAK;
            CASE(OP_insert3)
                : sp[0] = sp[-1];
            sp[-1] = sp[-2];
            sp[-2] = sp[-3];
            sp[-3] = JS_DupValue(ctx, sp[0]);
            sp++;
            BREAK;
            CASE(OP_insert4)
                : sp[0] = sp[-1];
            sp[-1] = sp[-2];
            sp[-2] = sp[-3];
            sp[-3] = sp[-4];
            sp[-4] = JS_DupValue(ctx, sp[0]);
            sp++;
            BREAK;
            CASE(OP_perm3)
                :
            {
                JSValue tmp;
                tmp = sp[-2];
                sp[-2] = sp[-3];
                sp[-3] = tmp;
            }
            BREAK;
            CASE(OP_rot3l)
                :
            {
                JSValue tmp;
                tmp = sp[-3];
                sp[-3] = sp[-2];
                sp[-2] = sp[-1];
                sp[-1] = tmp;
            }
            BREAK;
            CASE(OP_rot4l)
                :
            {
                JSValue tmp;
                tmp = sp[-4];
                sp[-4] = sp[-3];
                sp[-3] = sp[-2];
                sp[-2] = sp[-1];
                sp[-1] = tmp;
            }
            BREAK;
            CASE(OP_rot5l)
                :
            {
                JSValue tmp;
                tmp = sp[-5];
                sp[-5] = sp[-4];
                sp[-4] = sp[-3];
                sp[-3] = sp[-2];
                sp[-2] = sp[-1];
                sp[-1] = tmp;
            }
            BREAK;
            CASE(OP_rot3r)
                :
            {
                JSValue tmp;
                tmp = sp[-1];
                sp[-1] = sp[-2];
                sp[-2] = sp[-3];
                sp[-3] = tmp;
            }
            BREAK;
            CASE(OP_perm4)
                :
            {
                JSValue tmp;
                tmp = sp[-2];
                sp[-2] = sp[-3];
                sp[-3] = sp[-4];
                sp[-4] = tmp;
            }
            BREAK;
            CASE(OP_perm5)
                :
            {
                JSValue tmp;
                tmp = sp[-2];
                sp[-2] = sp[-3];
                sp[-3] = sp[-4];
                sp[-4] = sp[-5];
                sp[-5] = tmp;
            }
            BREAK;
            CASE(OP_swap)
                :
            {
                JSValue tmp;
                tmp = sp[-2];
                sp[-2] = sp[-1];
                sp[-1] = tmp;
            }
            BREAK;
            CASE(OP_swap2)
                :
            {
                JSValue tmp1, tmp2;
                tmp1 = sp[-4];
                tmp2 = sp[-3];
                sp[-4] = sp[-2];
                sp[-3] = sp[-1];
                sp[-2] = tmp1;
                sp[-1] = tmp2;
            }
            BREAK;

            CASE(OP_fclosure)
                :
            {
                JSValue bfunc = JS_DupValue(ctx, b->cpool[get_u32(pc)]);
                pc += 4;
                *sp++ = js_closure(ctx, bfunc, var_refs, sf, FALSE);
                if (unlikely(JS_IsException(sp[-1])))
                    goto exception;
            }
            BREAK;
#if SHORT_OPCODES
            CASE(OP_call0)
                : CASE(OP_call1)
                : CASE(OP_call2)
                : CASE(OP_call3)
                : call_argc = opcode - OP_call0;
            goto has_call_argc;
#endif
            CASE(OP_call)
                : CASE(OP_tail_call)
                :
            {
                call_argc = get_u16(pc);
                pc += 2;
                goto has_call_argc;
            has_call_argc:
                call_argv = sp - call_argc;
                sf->cur_pc = pc;
                if (unlikely(opcode == OP_tail_call) && b->func_kind == JS_FUNC_NORMAL) {
                    JSObject* np;
                    JSFunctionBytecode* nb;
                    tc_func = call_argv[-1];
                    if (JS_VALUE_GET_TAG(tc_func) == JS_TAG_OBJECT && (np = JS_VALUE_GET_OBJ(tc_func))->class_id == JS_CLASS_BYTECODE_FUNCTION && (nb = np->u.func.function_bytecode)->func_kind == JS_FUNC_NORMAL) {
                        tc_arg_alloc = (call_argc < nb->arg_count) ? nb->arg_count : 0;
                        if (sizeof(JSValue) * (tc_arg_alloc + nb->var_count + nb->stack_size) + sizeof(JSVarRef*) * nb->var_ref_count <= alloca_capacity) {
                            tc_argv = js_malloc(ctx, sizeof(JSValue) * call_argc);
                            if (tc_argv) {
                                int j;
                                memcpy(tc_argv, call_argv,
                                    sizeof(JSValue) * call_argc);
                                call_argv[-1] = JS_UNDEFINED;
                                for (j = 0; j < call_argc; j++)
                                    call_argv[j] = JS_UNDEFINED;
                                tc_argc = call_argc;
                                tc_this = JS_UNDEFINED;
                                tc_pending = TRUE;
                                goto done;
                            }
                        }
                    }
                }
                ret_val = JS_CallInternal(ctx, call_argv[-1], JS_UNDEFINED,
                    JS_UNDEFINED, call_argc, call_argv, 0);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                if (opcode == OP_tail_call)
                    goto done;
                for (i = -1; i < call_argc; i++)
                    JS_FreeValue(ctx, call_argv[i]);
                sp -= call_argc + 1;
                *sp++ = ret_val;
            }
            BREAK;
            CASE(OP_call_constructor)
                :
            {
                call_argc = get_u16(pc);
                pc += 2;
                call_argv = sp - call_argc;
                sf->cur_pc = pc;
                ret_val = JS_CallConstructorInternal(ctx, call_argv[-2],
                    call_argv[-1],
                    call_argc, call_argv, 0);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                for (i = -2; i < call_argc; i++)
                    JS_FreeValue(ctx, call_argv[i]);
                sp -= call_argc + 2;
                *sp++ = ret_val;
            }
            BREAK;
            CASE(OP_call_method)
                : CASE(OP_tail_call_method)
                :
            {
                call_argc = get_u16(pc);
                pc += 2;
                call_argv = sp - call_argc;
                sf->cur_pc = pc;
                if (unlikely(opcode == OP_tail_call_method) && b->func_kind == JS_FUNC_NORMAL) {
                    JSObject* np;
                    JSFunctionBytecode* nb;
                    tc_func = call_argv[-1];
                    if (JS_VALUE_GET_TAG(tc_func) == JS_TAG_OBJECT && (np = JS_VALUE_GET_OBJ(tc_func))->class_id == JS_CLASS_BYTECODE_FUNCTION && (nb = np->u.func.function_bytecode)->func_kind == JS_FUNC_NORMAL) {
                        tc_arg_alloc = (call_argc < nb->arg_count) ? nb->arg_count : 0;
                        if (sizeof(JSValue) * (tc_arg_alloc + nb->var_count + nb->stack_size) + sizeof(JSVarRef*) * nb->var_ref_count <= alloca_capacity) {
                            tc_argv = js_malloc(ctx, sizeof(JSValue) * call_argc);
                            if (tc_argv) {
                                int j;
                                memcpy(tc_argv, call_argv,
                                    sizeof(JSValue) * call_argc);
                                tc_argc = call_argc;
                                tc_this = call_argv[-2];
                                call_argv[-2] = JS_UNDEFINED;
                                call_argv[-1] = JS_UNDEFINED;
                                for (j = 0; j < call_argc; j++)
                                    call_argv[j] = JS_UNDEFINED;
                                tc_pending = TRUE;
                                goto done;
                            }
                        }
                    }
                }
                ret_val = JS_CallInternal(ctx, call_argv[-1], call_argv[-2],
                    JS_UNDEFINED, call_argc, call_argv, 0);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                if (opcode == OP_tail_call_method)
                    goto done;
                for (i = -2; i < call_argc; i++)
                    JS_FreeValue(ctx, call_argv[i]);
                sp -= call_argc + 2;
                *sp++ = ret_val;
            }
            BREAK;
            CASE(OP_array_from)
                : call_argc = get_u16(pc);
            pc += 2;
            ret_val = js_create_array_free(ctx, call_argc, sp - call_argc);
            sp -= call_argc;
            if (unlikely(JS_IsException(ret_val)))
                goto exception;
            *sp++ = ret_val;
            BREAK;

            CASE(OP_apply)
                :
            {
                int magic;
                magic = get_u16(pc);
                pc += 2;
                sf->cur_pc = pc;

                ret_val = js_function_apply(ctx, sp[-3], 2, (JSValueConst*)&sp[-2], magic);
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-3]);
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp -= 3;
                *sp++ = ret_val;
            }
            BREAK;
            CASE(OP_return)
                : ret_val = *--sp;
            goto done;
            CASE(OP_return_undef)
                : ret_val = JS_UNDEFINED;
            goto done;

            CASE(OP_check_ctor_return)
                : if (!JS_IsObject(sp[-1]))
            {
                if (!JS_IsUndefined(sp[-1])) {
                    JS_ThrowTypeError(caller_ctx, "derived class constructor must return an object or undefined");
                    goto exception;
                }
                sp[0] = JS_TRUE;
            }
            else
            {
                sp[0] = JS_FALSE;
            }
            sp++;
            BREAK;
            CASE(OP_check_ctor)
                : if (JS_IsUndefined(new_target))
            {
            non_ctor_call:
                JS_ThrowTypeError(ctx, "class constructors must be invoked with 'new'");
                goto exception;
            }
            BREAK;
            CASE(OP_init_ctor)
                :
            {
                JSValue super, ret;
                sf->cur_pc = pc;
                if (JS_IsUndefined(new_target))
                    goto non_ctor_call;
                super = JS_GetPrototype(ctx, func_obj);
                if (JS_IsException(super))
                    goto exception;
                ret = JS_CallConstructor2(ctx, super, new_target, argc, (JSValueConst*)argv);
                JS_FreeValue(ctx, super);
                if (JS_IsException(ret))
                    goto exception;
                *sp++ = ret;
            }
            BREAK;
            CASE(OP_check_brand)
                :
            {
                int ret = JS_CheckBrand(ctx, sp[-2], sp[-1]);
                if (ret < 0)
                    goto exception;
                if (!ret) {
                    JS_ThrowTypeError(ctx, "invalid brand on object");
                    goto exception;
                }
            }
            BREAK;
            CASE(OP_add_brand)
                : if (JS_AddBrand(ctx, sp[-2], sp[-1]) < 0) goto exception;
            JS_FreeValue(ctx, sp[-2]);
            JS_FreeValue(ctx, sp[-1]);
            sp -= 2;
            BREAK;

            CASE(OP_throw)
                : JS_Throw(ctx, *--sp);
            goto exception;

            CASE(OP_throw_error)
                :
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
                else
                    JS_ThrowInternalError(ctx, "invalid throw var type %d", type);
            }
            goto exception;

            CASE(OP_eval)
                :
            {
                JSValueConst obj;
                int scope_idx;
                call_argc = get_u16(pc);
                scope_idx = get_u16(pc + 2) + ARG_SCOPE_END;
                pc += 4;
                call_argv = sp - call_argc;
                sf->cur_pc = pc;
                if (js_same_value(ctx, call_argv[-1], ctx->eval_obj)) {
                    if (call_argc >= 1)
                        obj = call_argv[0];
                    else
                        obj = JS_UNDEFINED;
                    ret_val = JS_EvalObject(ctx, JS_UNDEFINED, obj,
                        JS_EVAL_TYPE_DIRECT, scope_idx);
                } else {
                    ret_val = JS_CallInternal(ctx, call_argv[-1], JS_UNDEFINED,
                        JS_UNDEFINED, call_argc, call_argv, 0);
                }
                if (unlikely(JS_IsException(ret_val)))
                    goto exception;
                for (i = -1; i < call_argc; i++)
                    JS_FreeValue(ctx, call_argv[i]);
                sp -= call_argc + 1;
                *sp++ = ret_val;
            }
            BREAK;
            CASE(OP_apply_eval)
                :
            {
                int scope_idx;
                uint32_t len;
                JSValue* tab;
                JSValueConst obj;

                scope_idx = get_u16(pc) + ARG_SCOPE_END;
                pc += 2;
                sf->cur_pc = pc;
                tab = build_arg_list(ctx, &len, sp[-1]);
                if (!tab)
                    goto exception;
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
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp -= 2;
                *sp++ = ret_val;
            }
            BREAK;

            CASE(OP_regexp)
                :
            {
                sp[-2] = JS_NewRegexp(ctx, sp[-2], sp[-1]);
                sp--;
                if (JS_IsException(sp[-1]))
                    goto exception;
            }
            BREAK;

            CASE(OP_get_super)
                :
            {
                JSValue proto;
                sf->cur_pc = pc;
                proto = JS_GetPrototype(ctx, sp[-1]);
                if (JS_IsException(proto))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = proto;
            }
            BREAK;

            CASE(OP_import)
                :
            {
                JSValue val;
                sf->cur_pc = pc;
                val = js_dynamic_import(ctx, sp[-2], sp[-1]);
                if (JS_IsException(val))
                    goto exception;
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-1]);
                sp--;
                sp[-1] = val;
            }
            BREAK;

            CASE(OP_get_var_undef)
                : CASE(OP_get_var)
                :
            {
                int idx;
                JSValue val;
                idx = get_u16(pc);
                pc += 2;
                val = *var_refs[idx]->pvalue;
                if (unlikely(JS_IsUninitialized(val))) {
                    JSClosureVar* cv = &b->closure_var[idx];
                    if (cv->is_lexical) {
                        JS_ThrowReferenceErrorUninitialized(ctx, cv->var_name);
                        goto exception;
                    } else {
                        sf->cur_pc = pc;
                        sp[0] = JS_GetPropertyInternal(ctx, ctx->global_obj,
                            cv->var_name,
                            ctx->global_obj,
                            opcode - OP_get_var_undef);
                        if (JS_IsException(sp[0]))
                            goto exception;
                    }
                } else {
                    sp[0] = JS_DupValue(ctx, val);
                }
                sp++;
            }
            BREAK;

            CASE(OP_put_var)
                : CASE(OP_put_var_init)
                :
            {
                int idx, ret;
                JSVarRef* var_ref;
                idx = get_u16(pc);
                pc += 2;
                var_ref = var_refs[idx];
                if (unlikely(JS_IsUninitialized(*var_ref->pvalue) || var_ref->is_const)) {
                    JSClosureVar* cv = &b->closure_var[idx];
                    if (var_ref->is_lexical) {
                        if (opcode == OP_put_var_init)
                            goto put_var_ok;
                        if (JS_IsUninitialized(*var_ref->pvalue))
                            JS_ThrowReferenceErrorUninitialized(ctx, cv->var_name);
                        else
                            JS_ThrowTypeErrorReadOnly(ctx, JS_PROP_THROW, cv->var_name);
                        goto exception;
                    } else {
                        sf->cur_pc = pc;
                        ret = JS_HasProperty(ctx, ctx->global_obj, cv->var_name);
                        if (ret < 0)
                            goto exception;
                        if (ret == 0 && is_strict_mode(ctx)) {
                            JS_ThrowReferenceErrorNotDefined(ctx, cv->var_name);
                            goto exception;
                        }
                        ret = JS_SetPropertyInternal(ctx, ctx->global_obj, cv->var_name, sp[-1],
                            ctx->global_obj, JS_PROP_THROW_STRICT);
                        sp--;
                        if (ret < 0)
                            goto exception;
                    }
                } else {
                put_var_ok:
                    set_value(ctx, var_ref->pvalue, sp[-1]);
                    sp--;
                }
            }
            BREAK;
            CASE(OP_get_loc)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                sp[0] = JS_DupValue(ctx, var_buf[idx]);
                sp++;
            }
            BREAK;
            CASE(OP_put_loc)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                set_value(ctx, &var_buf[idx], sp[-1]);
                sp--;
            }
            BREAK;
            CASE(OP_set_loc)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                set_value(ctx, &var_buf[idx], JS_DupValue(ctx, sp[-1]));
            }
            BREAK;
            CASE(OP_get_arg)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                sp[0] = JS_DupValue(ctx, arg_buf[idx]);
                sp++;
            }
            BREAK;
            CASE(OP_put_arg)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                set_value(ctx, &arg_buf[idx], sp[-1]);
                sp--;
            }
            BREAK;
            CASE(OP_set_arg)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                set_value(ctx, &arg_buf[idx], JS_DupValue(ctx, sp[-1]));
            }
            BREAK;

#if SHORT_OPCODES
            CASE(OP_get_loc8)
                : * sp++ = JS_DupValue(ctx, var_buf[*pc++]);
            BREAK;
            CASE(OP_put_loc8)
                : set_value(ctx, &var_buf[*pc++], *--sp);
            BREAK;
            CASE(OP_set_loc8)
                : set_value(ctx, &var_buf[*pc++], JS_DupValue(ctx, sp[-1]));
            BREAK;

            CASE(OP_get_loc0)
                : * sp++ = JS_DupValue(ctx, var_buf[0]);
            BREAK;
            CASE(OP_get_loc1)
                : * sp++ = JS_DupValue(ctx, var_buf[1]);
            BREAK;
            CASE(OP_get_loc2)
                : * sp++ = JS_DupValue(ctx, var_buf[2]);
            BREAK;
            CASE(OP_get_loc3)
                : * sp++ = JS_DupValue(ctx, var_buf[3]);
            BREAK;
            CASE(OP_put_loc0)
                : set_value(ctx, &var_buf[0], *--sp);
            BREAK;
            CASE(OP_put_loc1)
                : set_value(ctx, &var_buf[1], *--sp);
            BREAK;
            CASE(OP_put_loc2)
                : set_value(ctx, &var_buf[2], *--sp);
            BREAK;
            CASE(OP_put_loc3)
                : set_value(ctx, &var_buf[3], *--sp);
            BREAK;
            CASE(OP_set_loc0)
                : set_value(ctx, &var_buf[0], JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_set_loc1)
                : set_value(ctx, &var_buf[1], JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_set_loc2)
                : set_value(ctx, &var_buf[2], JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_set_loc3)
                : set_value(ctx, &var_buf[3], JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_get_arg0)
                : * sp++ = JS_DupValue(ctx, arg_buf[0]);
            BREAK;
            CASE(OP_get_arg1)
                : * sp++ = JS_DupValue(ctx, arg_buf[1]);
            BREAK;
            CASE(OP_get_arg2)
                : * sp++ = JS_DupValue(ctx, arg_buf[2]);
            BREAK;
            CASE(OP_get_arg3)
                : * sp++ = JS_DupValue(ctx, arg_buf[3]);
            BREAK;
            CASE(OP_put_arg0)
                : set_value(ctx, &arg_buf[0], *--sp);
            BREAK;
            CASE(OP_put_arg1)
                : set_value(ctx, &arg_buf[1], *--sp);
            BREAK;
            CASE(OP_put_arg2)
                : set_value(ctx, &arg_buf[2], *--sp);
            BREAK;
            CASE(OP_put_arg3)
                : set_value(ctx, &arg_buf[3], *--sp);
            BREAK;
            CASE(OP_set_arg0)
                : set_value(ctx, &arg_buf[0], JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_set_arg1)
                : set_value(ctx, &arg_buf[1], JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_set_arg2)
                : set_value(ctx, &arg_buf[2], JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_set_arg3)
                : set_value(ctx, &arg_buf[3], JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_get_var_ref0)
                : * sp++ = JS_DupValue(ctx, *var_refs[0]->pvalue);
            BREAK;
            CASE(OP_get_var_ref1)
                : * sp++ = JS_DupValue(ctx, *var_refs[1]->pvalue);
            BREAK;
            CASE(OP_get_var_ref2)
                : * sp++ = JS_DupValue(ctx, *var_refs[2]->pvalue);
            BREAK;
            CASE(OP_get_var_ref3)
                : * sp++ = JS_DupValue(ctx, *var_refs[3]->pvalue);
            BREAK;
            CASE(OP_put_var_ref0)
                : set_value(ctx, var_refs[0]->pvalue, *--sp);
            BREAK;
            CASE(OP_put_var_ref1)
                : set_value(ctx, var_refs[1]->pvalue, *--sp);
            BREAK;
            CASE(OP_put_var_ref2)
                : set_value(ctx, var_refs[2]->pvalue, *--sp);
            BREAK;
            CASE(OP_put_var_ref3)
                : set_value(ctx, var_refs[3]->pvalue, *--sp);
            BREAK;
            CASE(OP_set_var_ref0)
                : set_value(ctx, var_refs[0]->pvalue, JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_set_var_ref1)
                : set_value(ctx, var_refs[1]->pvalue, JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_set_var_ref2)
                : set_value(ctx, var_refs[2]->pvalue, JS_DupValue(ctx, sp[-1]));
            BREAK;
            CASE(OP_set_var_ref3)
                : set_value(ctx, var_refs[3]->pvalue, JS_DupValue(ctx, sp[-1]));
            BREAK;
#endif

            CASE(OP_get_var_ref)
                :
            {
                int idx;
                JSValue val;
                idx = get_u16(pc);
                pc += 2;
                val = *var_refs[idx]->pvalue;
                sp[0] = JS_DupValue(ctx, val);
                sp++;
            }
            BREAK;
            CASE(OP_put_var_ref)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                set_value(ctx, var_refs[idx]->pvalue, sp[-1]);
                sp--;
            }
            BREAK;
            CASE(OP_set_var_ref)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                set_value(ctx, var_refs[idx]->pvalue, JS_DupValue(ctx, sp[-1]));
            }
            BREAK;
            CASE(OP_get_var_ref_check)
                :
            {
                int idx;
                JSValue val;
                idx = get_u16(pc);
                pc += 2;
                val = *var_refs[idx]->pvalue;
                if (unlikely(JS_IsUninitialized(val))) {
                    JS_ThrowReferenceErrorUninitialized2(ctx, b, idx, TRUE);
                    goto exception;
                }
                sp[0] = JS_DupValue(ctx, val);
                sp++;
            }
            BREAK;
            CASE(OP_put_var_ref_check)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                if (unlikely(JS_IsUninitialized(*var_refs[idx]->pvalue))) {
                    JS_ThrowReferenceErrorUninitialized2(ctx, b, idx, TRUE);
                    goto exception;
                }
                set_value(ctx, var_refs[idx]->pvalue, sp[-1]);
                sp--;
            }
            BREAK;
            CASE(OP_put_var_ref_check_init)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                if (unlikely(!JS_IsUninitialized(*var_refs[idx]->pvalue))) {
                    JS_ThrowReferenceErrorUninitialized2(ctx, b, idx, TRUE);
                    goto exception;
                }
                set_value(ctx, var_refs[idx]->pvalue, sp[-1]);
                sp--;
            }
            BREAK;
            CASE(OP_set_loc_uninitialized)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                set_value(ctx, &var_buf[idx], JS_UNINITIALIZED);
            }
            BREAK;
            CASE(OP_get_loc_check)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                if (unlikely(JS_IsUninitialized(var_buf[idx]))) {
                    JS_ThrowReferenceErrorUninitialized2(ctx, b, idx, FALSE);
                    goto exception;
                }
                sp[0] = JS_DupValue(ctx, var_buf[idx]);
                sp++;
            }
            BREAK;
            CASE(OP_get_loc_checkthis)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                if (unlikely(JS_IsUninitialized(var_buf[idx]))) {
                    JS_ThrowReferenceErrorUninitialized2(caller_ctx, b, idx, FALSE);
                    goto exception;
                }
                sp[0] = JS_DupValue(ctx, var_buf[idx]);
                sp++;
            }
            BREAK;
            CASE(OP_put_loc_check)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                if (unlikely(JS_IsUninitialized(var_buf[idx]))) {
                    JS_ThrowReferenceErrorUninitialized2(ctx, b, idx, FALSE);
                    goto exception;
                }
                set_value(ctx, &var_buf[idx], sp[-1]);
                sp--;
            }
            BREAK;
            CASE(OP_set_loc_check)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                if (unlikely(JS_IsUninitialized(var_buf[idx]))) {
                    JS_ThrowReferenceErrorUninitialized2(ctx, b, idx, FALSE);
                    goto exception;
                }
                set_value(ctx, &var_buf[idx], JS_DupValue(ctx, sp[-1]));
            }
            BREAK;
            CASE(OP_put_loc_check_init)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                if (unlikely(!JS_IsUninitialized(var_buf[idx]))) {
                    JS_ThrowReferenceError(ctx, "'this' can be initialized only once");
                    goto exception;
                }
                set_value(ctx, &var_buf[idx], sp[-1]);
                sp--;
            }
            BREAK;
            CASE(OP_close_loc)
                :
            {
                int idx;
                idx = get_u16(pc);
                pc += 2;
                close_lexical_var(ctx, b, sf, idx);
            }
            BREAK;

            CASE(OP_make_loc_ref)
                : CASE(OP_make_arg_ref)
                : CASE(OP_make_var_ref_ref)
                :
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
                    goto exception;
                if (opcode == OP_make_var_ref_ref) {
                    var_ref = var_refs[idx];
                    js_rc(var_ref)->ref_count++;
                } else {
                    var_ref = get_var_ref(ctx, sf, idx, opcode == OP_make_arg_ref);
                    if (!var_ref)
                        goto exception;
                }
                pr = add_property(ctx, JS_VALUE_GET_OBJ(sp[-1]), atom,
                    JS_PROP_WRITABLE | JS_PROP_VARREF);
                if (!pr) {
                    free_var_ref(rt, var_ref);
                    goto exception;
                }
                pr->u.var_ref = var_ref;
                *sp++ = JS_AtomToValue(ctx, atom);
            }
            BREAK;
            CASE(OP_make_var_ref)
                :
            {
                JSAtom atom;
                atom = get_u32(pc);
                pc += 4;
                sf->cur_pc = pc;

                if (JS_GetGlobalVarRef(ctx, atom, sp))
                    goto exception;
                sp += 2;
            }
            BREAK;

            CASE(OP_goto)
                : pc += (int32_t)get_u32(pc);
            if (unlikely(js_poll_interrupts(ctx)))
                goto exception;
            BREAK;
#if SHORT_OPCODES
            CASE(OP_goto16)
                : pc += (int16_t)get_u16(pc);
            if (unlikely(js_poll_interrupts(ctx)))
                goto exception;
            BREAK;
            CASE(OP_goto8)
                : pc += (int8_t)pc[0];
            if (unlikely(js_poll_interrupts(ctx)))
                goto exception;
            BREAK;
#endif
            CASE(OP_if_true)
                :
            {
                int res;
                JSValue op1;

                op1 = sp[-1];
                pc += 4;
                if ((uint32_t)JS_VALUE_GET_TAG(op1) <= JS_TAG_UNDEFINED) {
                    res = JS_VALUE_GET_INT(op1);
                } else {
                    res = JS_ToBoolFree(ctx, op1);
                }
                sp--;
                if (res) {
                    int32_t br_off = (int32_t)get_u32(pc - 4) - 4;
                    pc += br_off;
                    if (unlikely(br_off <= 0) && unlikely(js_poll_interrupts(ctx)))
                        goto exception;
                }
            }
            BREAK;
            CASE(OP_if_false)
                :
            {
                int res;
                JSValue op1;

                op1 = sp[-1];
                pc += 4;
                if ((uint32_t)JS_VALUE_GET_TAG(op1) <= JS_TAG_UNDEFINED) {
                    res = JS_VALUE_GET_INT(op1);
                } else {
                    res = JS_ToBoolFree(ctx, op1);
                }
                sp--;
                if (!res) {
                    int32_t br_off = (int32_t)get_u32(pc - 4) - 4;
                    pc += br_off;
                    if (unlikely(br_off <= 0) && unlikely(js_poll_interrupts(ctx)))
                        goto exception;
                }
            }
            BREAK;
            CASE(OP_switch)
                :
            {
                const uint8_t* pc_op = pc - 1;
                uint32_t cp_idx = get_u32(pc);
                pc += 4;
                if (JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_INT) {
                    const uint8_t* t = JS_VALUE_GET_STRING(b->cpool[cp_idx])->u.str8;
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
            BREAK;
#if SHORT_OPCODES
            CASE(OP_if_true8)
                :
            {
                int res;
                JSValue op1;

                op1 = sp[-1];
                pc += 1;
                if ((uint32_t)JS_VALUE_GET_TAG(op1) <= JS_TAG_UNDEFINED) {
                    res = JS_VALUE_GET_INT(op1);
                } else {
                    res = JS_ToBoolFree(ctx, op1);
                }
                sp--;
                if (res) {
                    int br_off = (int8_t)pc[-1] - 1;
                    pc += br_off;
                    if (unlikely(br_off <= 0) && unlikely(js_poll_interrupts(ctx)))
                        goto exception;
                }
            }
            BREAK;
            CASE(OP_if_false8)
                :
            {
                int res;
                JSValue op1;

                op1 = sp[-1];
                pc += 1;
                if ((uint32_t)JS_VALUE_GET_TAG(op1) <= JS_TAG_UNDEFINED) {
                    res = JS_VALUE_GET_INT(op1);
                } else {
                    res = JS_ToBoolFree(ctx, op1);
                }
                sp--;
                if (!res) {
                    int br_off = (int8_t)pc[-1] - 1;
                    pc += br_off;
                    if (unlikely(br_off <= 0) && unlikely(js_poll_interrupts(ctx)))
                        goto exception;
                }
            }
            BREAK;
#endif
            CASE(OP_catch)
                :
            {
                int32_t diff;
                diff = get_u32(pc);
                sp[0] = JS_NewCatchOffset(ctx, pc + diff - b->byte_code_buf);
                sp++;
                pc += 4;
            }
            BREAK;
            CASE(OP_for_await_catch_body)
                :
            {
                int32_t diff;
                diff = get_u32(pc);
                sp[-3] = JS_NewCatchOffset(ctx, pc + diff - b->byte_code_buf);
                pc += 4;
            }
            BREAK;
            CASE(OP_gosub)
                :
            {
                int32_t diff;
                diff = get_u32(pc);
                sp[0] = JS_NewInt32(ctx, pc + 4 - b->byte_code_buf);
                sp++;
                pc += diff;
            }
            BREAK;
            CASE(OP_ret)
                :
            {
                JSValue op1;
                uint32_t pos;
                op1 = sp[-1];
                if (unlikely(JS_VALUE_GET_TAG(op1) != JS_TAG_INT))
                    goto ret_fail;
                pos = JS_VALUE_GET_INT(op1);
                if (unlikely(pos >= b->byte_code_len)) {
                ret_fail:
                    JS_ThrowInternalError(ctx, "invalid ret value");
                    goto exception;
                }
                sp--;
                pc = b->byte_code_buf + pos;
            }
            BREAK;

            CASE(OP_for_in_start)
                : sf->cur_pc = pc;
            if (js_for_in_start(ctx, sp))
                goto exception;
            BREAK;
            CASE(OP_for_in_next)
                : sf->cur_pc = pc;
            if (js_for_in_next(ctx, sp))
                goto exception;
            sp += 2;
            BREAK;
            CASE(OP_for_of_start)
                : sf->cur_pc = pc;
            if (js_for_of_start(ctx, sp, FALSE))
                goto exception;
            sp += 1;
            *sp++ = JS_NewCatchOffset(ctx, 0);
            BREAK;
            CASE(OP_for_of_next)
                :
            {
                int offset = -3 - pc[0];
                pc += 1;
                sf->cur_pc = pc;
                if (js_for_of_next(ctx, sp, offset))
                    goto exception;
                sp += 2;
            }
            BREAK;
            CASE(OP_for_await_of_next)
                : sf->cur_pc = pc;
            if (js_for_await_of_next(ctx, sp))
                goto exception;
            sp++;
            BREAK;
            CASE(OP_for_await_of_start)
                : sf->cur_pc = pc;
            if (js_for_of_start(ctx, sp, TRUE))
                goto exception;
            sp += 1;
            *sp++ = JS_NewCatchOffset(ctx, 0);
            BREAK;
            CASE(OP_iterator_get_value_done)
                : sf->cur_pc = pc;
            if (js_iterator_get_value_done(ctx, sp))
                goto exception;
            sp += 1;
            BREAK;
            CASE(OP_iterator_check_object)
                : if (unlikely(!JS_IsObject(sp[-1])))
            {
                JS_ThrowTypeError(ctx, "iterator must return an object");
                goto exception;
            }
            BREAK;

            CASE(OP_iterator_close)
                : sp--;
            JS_FreeValue(ctx, sp[-1]);
            sp--;
            if (!JS_IsUndefined(sp[-1])) {
                sf->cur_pc = pc;
                if (JS_IteratorClose(ctx, sp[-1], FALSE))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
            }
            sp--;
            BREAK;
            CASE(OP_nip_catch)
                :
            {
                JSValue catch_ret;
                catch_ret = *--sp;
                while (sp > stack_buf && JS_VALUE_GET_TAG(sp[-1]) != JS_TAG_CATCH_OFFSET) {
                    JS_FreeValue(ctx, *--sp);
                }
                if (unlikely(sp == stack_buf)) {
                    JS_ThrowInternalError(ctx, "nip_catch");
                    JS_FreeValue(ctx, catch_ret);
                    goto exception;
                }
                sp[-1] = catch_ret;
            }
            BREAK;

            CASE(OP_iterator_next)
                :
            {
                JSValue ret;
                sf->cur_pc = pc;
                ret = JS_Call(ctx, sp[-3], sp[-4],
                    1, (JSValueConst*)(sp - 1));
                if (JS_IsException(ret))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = ret;
            }
            BREAK;

            CASE(OP_iterator_call)
                :
            {
                JSValue method, ret;
                BOOL ret_flag;
                int throw_flags;
                throw_flags = *pc++;
                sf->cur_pc = pc;
                method = JS_GetProperty(ctx, sp[-4], (throw_flags & 1) ? JS_ATOM_throw : JS_ATOM_return);
                if (JS_IsException(method))
                    goto exception;
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
                        goto exception;
                    JS_FreeValue(ctx, sp[-1]);
                    sp[-1] = ret;
                    ret_flag = FALSE;
                }
                sp[0] = JS_NewBool(ctx, ret_flag);
                sp += 1;
            }
            BREAK;

            CASE(OP_lnot)
                :
            {
                int res;
                JSValue op1;

                op1 = sp[-1];
                if ((uint32_t)JS_VALUE_GET_TAG(op1) <= JS_TAG_UNDEFINED) {
                    res = JS_VALUE_GET_INT(op1) != 0;
                } else {
                    res = JS_ToBoolFree(ctx, op1);
                }
                sp[-1] = JS_NewBool(ctx, !res);
            }
            BREAK;

#define GET_FIELD_INLINE(name, keep, is_length)                                              \
    {                                                                                        \
        JSValue fi_val, fi_obj;                                                              \
        JSAtom fi_atom;                                                                      \
        JSObject* fi_p;                                                                      \
        JSProperty* fi_pr;                                                                   \
        JSShapeProperty* fi_prs;                                                             \
                                                                                             \
        if (is_length) {                                                                     \
            fi_atom = JS_ATOM_length;                                                        \
        } else {                                                                             \
            fi_atom = get_u32(pc);                                                           \
            pc += 4;                                                                         \
        }                                                                                    \
                                                                                             \
        fi_obj = sp[-1];                                                                     \
        if (likely(JS_VALUE_GET_TAG(fi_obj) == JS_TAG_OBJECT)) {                             \
            fi_p = JS_VALUE_GET_OBJ(fi_obj);                                                 \
            for (;;) {                                                                       \
                fi_prs = find_own_property(&fi_pr, fi_p, fi_atom);                           \
                if (fi_prs) {                                                                \
                                                                                             \
                    if (unlikely(fi_prs->flags & JS_PROP_TMASK))                             \
                        goto name##_slow_path;                                               \
                    fi_val = JS_DupValue(ctx, fi_pr->u.value);                               \
                    break;                                                                   \
                }                                                                            \
                if (unlikely(fi_p->is_exotic)) {                                             \
                                                                                             \
                    if (fi_p->class_id != JS_CLASS_ARRAY || __JS_AtomIsTaggedInt(fi_atom)) { \
                        fi_obj = JS_MKPTR(JS_TAG_OBJECT, fi_p);                              \
                        goto name##_slow_path;                                               \
                    }                                                                        \
                }                                                                            \
                fi_p = fi_p->shape->proto;                                                   \
                if (!fi_p) {                                                                 \
                    fi_val = JS_UNDEFINED;                                                   \
                    break;                                                                   \
                }                                                                            \
            }                                                                                \
        } else if (is_length && JS_VALUE_GET_TAG(fi_obj) == JS_TAG_STRING) {                 \
            fi_val = JS_NewInt32(ctx, JS_VALUE_GET_STRING(fi_obj)->len);                     \
        } else if (is_length && JS_VALUE_GET_TAG(fi_obj) == JS_TAG_STRING_ROPE) {            \
            fi_val = JS_NewInt32(ctx,                                                        \
                JS_VALUE_GET_STRING_ROPE(fi_obj)->len);                                      \
        } else {                                                                             \
            name##_slow_path : sf->cur_pc = pc;                                              \
            fi_val = JS_GetPropertyInternal(ctx, fi_obj, fi_atom, sp[-1], 0);                \
            if (unlikely(JS_IsException(fi_val)))                                            \
                goto exception;                                                              \
        }                                                                                    \
        if (keep) {                                                                          \
            *sp++ = fi_val;                                                                  \
        } else {                                                                             \
            JS_FreeValue(ctx, sp[-1]);                                                       \
            sp[-1] = fi_val;                                                                 \
        }                                                                                    \
    }

            CASE(OP_get_field)
                :
            GET_FIELD_INLINE(get_field, 0, 0);
            BREAK;

            CASE(OP_get_field2)
                : GET_FIELD_INLINE(get_field2, 1, 0);
            BREAK;

#if SHORT_OPCODES
            CASE(OP_get_length)
                : GET_FIELD_INLINE(get_length, 0, 1);
            BREAK;
#endif

            CASE(OP_put_field)
                :
            {
                int ret;
                JSValue obj;
                JSAtom atom;
                JSObject* dp;
                JSProperty* pr;
                JSShapeProperty* prs;

                atom = get_u32(pc);
                pc += 4;

                obj = sp[-2];
                if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT)) {
                    dp = JS_VALUE_GET_OBJ(obj);
                    prs = find_own_property(&pr, dp, atom);
                    if (!prs)
                        goto put_field_slow_path;
                    if (likely((prs->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE | JS_PROP_LENGTH)) == JS_PROP_WRITABLE)) {
                        set_value(ctx, &pr->u.value, sp[-1]);
                    } else {
                        goto put_field_slow_path;
                    }
                    JS_FreeValue(ctx, obj);
                    sp -= 2;
                } else {
                put_field_slow_path:
                    sf->cur_pc = pc;
                    ret = JS_SetPropertyInternal(ctx, obj, atom, sp[-1], obj,
                        JS_PROP_THROW_STRICT);
                    JS_FreeValue(ctx, obj);
                    sp -= 2;
                    if (unlikely(ret < 0))
                        goto exception;
                }
            }
            BREAK;

            CASE(OP_private_symbol)
                :
            {
                JSAtom atom;
                JSValue val;

                atom = get_u32(pc);
                pc += 4;
                val = JS_NewSymbolFromAtom(ctx, atom, JS_ATOM_TYPE_PRIVATE);
                if (JS_IsException(val))
                    goto exception;
                *sp++ = val;
            }
            BREAK;

            CASE(OP_get_private_field)
                :
            {
                JSValue val;

                val = JS_GetPrivateField(ctx, sp[-2], sp[-1]);
                JS_FreeValue(ctx, sp[-1]);
                JS_FreeValue(ctx, sp[-2]);
                sp[-2] = val;
                sp--;
                if (unlikely(JS_IsException(val)))
                    goto exception;
            }
            BREAK;

            CASE(OP_put_private_field)
                :
            {
                int ret;
                ret = JS_SetPrivateField(ctx, sp[-3], sp[-1], sp[-2]);
                JS_FreeValue(ctx, sp[-3]);
                JS_FreeValue(ctx, sp[-1]);
                sp -= 3;
                if (unlikely(ret < 0))
                    goto exception;
            }
            BREAK;

            CASE(OP_define_private_field)
                :
            {
                int ret;
                ret = JS_DefinePrivateField(ctx, sp[-3], sp[-2], sp[-1]);
                JS_FreeValue(ctx, sp[-2]);
                sp -= 2;
                if (unlikely(ret < 0))
                    goto exception;
            }
            BREAK;

            CASE(OP_define_field)
                :
            {
                int ret;
                JSAtom atom;
                atom = get_u32(pc);
                pc += 4;

                ret = JS_DefinePropertyValue(ctx, sp[-2], atom, sp[-1],
                    JS_PROP_C_W_E | JS_PROP_THROW);
                sp--;
                if (unlikely(ret < 0))
                    goto exception;
#if CONFIG_LITERAL_BOILERPLATE
                if (unlikely(bb_rec_obj != NULL)) {
                    if (JS_VALUE_GET_OBJ(sp[-1]) == bb_rec_obj && !__JS_AtomIsTaggedInt(atom)) {
                        JSObject* po = bb_rec_obj;
                        JSShape* sh = po->shape;
                        int prefix = bb_rec_site ? bb_rec_site->field_count : 0;
                        int k = bb_rec_nf + 1;
                        int expect = k > prefix ? k : prefix;

                        if (sh->prop_count == expect && po->class_id == JS_CLASS_OBJECT && !po->is_exotic && !po->fast_array && sh->proto == get_proto_obj(ctx->class_proto[JS_CLASS_OBJECT])) {
                            JSShapeProperty* pr = get_shape_prop(sh);
                            if (pr[k - 1].atom == atom && pr[k - 1].flags == JS_PROP_C_W_E) {
                                JSObjectPresizeSite* bb_st_;
                                bb_rec_nf = k;
                                bb_st_ = js_object_presize_record_site(
                                    ctx, b, bb_rec_site, bb_rec_off, po, k);
                                if (bb_st_ != NULL)
                                    bb_rec_site = bb_st_;
                            } else {
                                BB_REC_CLEAR();
                            }
                        } else {
                            BB_REC_CLEAR();
                        }
                    } else {
                        BB_REC_CLEAR();
                    }
                }
#endif
            }
            BREAK;

            CASE(OP_set_name)
                :
            {
                int ret;
                JSAtom atom;
                atom = get_u32(pc);
                pc += 4;

                ret = JS_DefineObjectName(ctx, sp[-1], atom, JS_PROP_CONFIGURABLE);
                if (unlikely(ret < 0))
                    goto exception;
            }
            BREAK;
            CASE(OP_set_name_computed)
                :
            {
                int ret;
                ret = JS_DefineObjectNameComputed(ctx, sp[-1], sp[-2], JS_PROP_CONFIGURABLE);
                if (unlikely(ret < 0))
                    goto exception;
            }
            BREAK;
            CASE(OP_set_proto)
                :
            {
                JSValue proto;
                sf->cur_pc = pc;
                proto = sp[-1];
                if (JS_IsObject(proto) || JS_IsNull(proto)) {
                    if (JS_SetPrototypeInternal(ctx, sp[-2], proto, TRUE) < 0)
                        goto exception;
                }
                JS_FreeValue(ctx, proto);
                sp--;
            }
            BREAK;
            CASE(OP_set_home_object)
                : js_method_set_home_object(ctx, sp[-1], sp[-2]);
            BREAK;
            CASE(OP_define_method)
                : CASE(OP_define_method_computed)
                :
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
                        goto exception;
                    opcode += OP_define_method - OP_define_method_computed;
                } else {
                    atom = get_u32(pc);
                    pc += 4;
                }
                op_flags = *pc++;

                obj = sp[-2 - is_computed];
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
                    goto exception;
            }
            BREAK;

            CASE(OP_define_class)
                : CASE(OP_define_class_computed)
                :
            {
                int class_flags;
                JSAtom atom;

                atom = get_u32(pc);
                class_flags = pc[4];
                pc += 5;
                if (js_op_define_class(ctx, sp, atom, class_flags,
                        var_refs, sf,
                        (opcode == OP_define_class_computed))
                    < 0)
                    goto exception;
            }
            BREAK;

#define GET_ARRAY_EL_INLINE(name, keep)                                                                     \
    {                                                                                                       \
        JSValue ae_val, ae_obj, ae_prop;                                                                    \
        JSObject* ae_p;                                                                                     \
        uint32_t ae_idx;                                                                                    \
                                                                                                            \
        ae_obj = sp[-2];                                                                                    \
        ae_prop = sp[-1];                                                                                   \
        if (likely(JS_VALUE_GET_TAG(ae_obj) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(ae_prop) == JS_TAG_INT)) { \
            ae_p = JS_VALUE_GET_OBJ(ae_obj);                                                                \
            ae_idx = JS_VALUE_GET_INT(ae_prop);                                                             \
                                                                                                            \
            switch (ae_p->class_id) {                                                                       \
            case JS_CLASS_ARRAY:                                                                            \
            case JS_CLASS_ARGUMENTS:                                                                        \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = JS_DupValue(ctx, ae_p->u.array.u.values[ae_idx]);                                  \
                break;                                                                                      \
            case JS_CLASS_INT8_ARRAY:                                                                       \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = JS_NewInt32(ctx, ae_p->u.array.u.int8_ptr[ae_idx]);                                \
                break;                                                                                      \
            case JS_CLASS_UINT8C_ARRAY:                                                                     \
            case JS_CLASS_UINT8_ARRAY:                                                                      \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = JS_NewInt32(ctx, ae_p->u.array.u.uint8_ptr[ae_idx]);                               \
                break;                                                                                      \
            case JS_CLASS_INT16_ARRAY:                                                                      \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = JS_NewInt32(ctx, ae_p->u.array.u.int16_ptr[ae_idx]);                               \
                break;                                                                                      \
            case JS_CLASS_UINT16_ARRAY:                                                                     \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = JS_NewInt32(ctx, ae_p->u.array.u.uint16_ptr[ae_idx]);                              \
                break;                                                                                      \
            case JS_CLASS_INT32_ARRAY:                                                                      \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = JS_NewInt32(ctx, ae_p->u.array.u.int32_ptr[ae_idx]);                               \
                break;                                                                                      \
            case JS_CLASS_UINT32_ARRAY:                                                                     \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = JS_NewUint32(ctx, ae_p->u.array.u.uint32_ptr[ae_idx]);                             \
                break;                                                                                      \
            case JS_CLASS_FLOAT16_ARRAY:                                                                    \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = __JS_NewFloat64(ctx,                                                               \
                    fromfp16(ae_p->u.array.u.fp16_ptr[ae_idx]));                                            \
                break;                                                                                      \
            case JS_CLASS_FLOAT32_ARRAY:                                                                    \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = __JS_NewFloat64(ctx, (double)ae_p->u.array.u.float_ptr[ae_idx]);                   \
                break;                                                                                      \
            case JS_CLASS_FLOAT64_ARRAY:                                                                    \
                if (unlikely(ae_idx >= ae_p->u.array.count))                                                \
                    goto name##_slow_path;                                                                  \
                ae_val = __JS_NewFloat64(ctx, ae_p->u.array.u.double_ptr[ae_idx]);                          \
                break;                                                                                      \
            default:                                                                                        \
                goto name##_slow_path;                                                                      \
            }                                                                                               \
        } else {                                                                                            \
            name##_slow_path : sf->cur_pc = pc;                                                             \
            ae_val = JS_GetPropertyValue(ctx, ae_obj, ae_prop);                                             \
            if (unlikely(JS_IsException(ae_val))) {                                                         \
                if (keep)                                                                                   \
                    sp[-1] = JS_UNDEFINED;                                                                  \
                else                                                                                        \
                    sp--;                                                                                   \
                goto exception;                                                                             \
            }                                                                                               \
        }                                                                                                   \
        if (keep) {                                                                                         \
            sp[-1] = ae_val;                                                                                \
        } else {                                                                                            \
            JS_FreeValue(ctx, ae_obj);                                                                      \
            sp[-2] = ae_val;                                                                                \
            sp--;                                                                                           \
        }                                                                                                   \
    }

            CASE(OP_get_array_el)
                : GET_ARRAY_EL_INLINE(get_array_el, 0);
            BREAK;

            CASE(OP_get_array_el2)
                : GET_ARRAY_EL_INLINE(get_array_el2, 1);
            BREAK;

            CASE(OP_get_array_el3)
                :
            {
                JSValue val;
                JSObject* arr_obj;
                uint32_t idx;

                if (likely(JS_VALUE_GET_TAG(sp[-2]) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_INT)) {
                    arr_obj = JS_VALUE_GET_OBJ(sp[-2]);
                    idx = JS_VALUE_GET_INT(sp[-1]);
                    if (unlikely(arr_obj->class_id != JS_CLASS_ARRAY))
                        goto get_array_el3_slow_path;
                    if (unlikely(idx >= arr_obj->u.array.count))
                        goto get_array_el3_slow_path;
                    val = JS_DupValue(ctx, arr_obj->u.array.u.values[idx]);
                } else {
                get_array_el3_slow_path:
                    switch (JS_VALUE_GET_TAG(sp[-1])) {
                    case JS_TAG_INT:
                    case JS_TAG_STRING:
                    case JS_TAG_SYMBOL:
                        break;
                    default:
                        if (unlikely(JS_IsUndefined(sp[-2]) || JS_IsNull(sp[-2]))) {
                            JS_ThrowTypeError(ctx, "value has no property");
                            goto exception;
                        }
                        sf->cur_pc = pc;
                        ret_val = JS_ToPropertyKey(ctx, sp[-1]);
                        if (JS_IsException(ret_val))
                            goto exception;
                        JS_FreeValue(ctx, sp[-1]);
                        sp[-1] = ret_val;
                        break;
                    }
                    sf->cur_pc = pc;
                    val = JS_GetPropertyValue(ctx, sp[-2], JS_DupValue(ctx, sp[-1]));
                    if (unlikely(JS_IsException(val)))
                        goto exception;
                }
                *sp++ = val;
            }
            BREAK;

            CASE(OP_get_ref_value)
                :
            {
                JSValue val;
                JSAtom atom;
                int ret;

                sf->cur_pc = pc;
                atom = JS_ValueToAtom(ctx, sp[-1]);
                if (atom == JS_ATOM_NULL)
                    goto exception;
                if (unlikely(JS_IsUndefined(sp[-2]))) {
                    JS_ThrowReferenceErrorNotDefined(ctx, atom);
                    JS_FreeAtom(ctx, atom);
                    goto exception;
                }
                ret = JS_HasProperty(ctx, sp[-2], atom);
                if (unlikely(ret <= 0)) {
                    if (ret < 0) {
                        JS_FreeAtom(ctx, atom);
                        goto exception;
                    }
                    if (is_strict_mode(ctx)) {
                        JS_ThrowReferenceErrorNotDefined(ctx, atom);
                        JS_FreeAtom(ctx, atom);
                        goto exception;
                    }
                    val = JS_UNDEFINED;
                } else {
                    val = JS_GetProperty(ctx, sp[-2], atom);
                }
                JS_FreeAtom(ctx, atom);
                if (unlikely(JS_IsException(val)))
                    goto exception;
                sp[0] = val;
                sp++;
            }
            BREAK;

            CASE(OP_get_super_value)
                :
            {
                JSValue val;
                JSAtom atom;
                sf->cur_pc = pc;
                atom = JS_ValueToAtom(ctx, sp[-1]);
                if (unlikely(atom == JS_ATOM_NULL))
                    goto exception;
                val = JS_GetPropertyInternal(ctx, sp[-2], atom, sp[-3], FALSE);
                JS_FreeAtom(ctx, atom);
                if (unlikely(JS_IsException(val)))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-3]);
                sp[-3] = val;
                sp -= 2;
            }
            BREAK;

            CASE(OP_put_array_el)
                :
            {
                int ret;
                JSObject* arr_obj;
                uint32_t idx;

                if (likely(JS_VALUE_GET_TAG(sp[-3]) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(sp[-2]) == JS_TAG_INT)) {
                    arr_obj = JS_VALUE_GET_OBJ(sp[-3]);
                    idx = JS_VALUE_GET_INT(sp[-2]);
                    if (likely(arr_obj->class_id == JS_CLASS_ARRAY))
                        goto put_array_el_plain;
                    if (arr_obj->class_id >= JS_CLASS_UINT8C_ARRAY && arr_obj->class_id <= JS_CLASS_FLOAT64_ARRAY) {
                        int vtag = JS_VALUE_GET_TAG(sp[-1]);
                        double d;
                        if (idx < arr_obj->u.array.count) {
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
                        } else if ((vtag != JS_TAG_INT && vtag != JS_TAG_FLOAT64) || arr_obj->class_id == JS_CLASS_BIG_INT64_ARRAY || arr_obj->class_id == JS_CLASS_BIG_UINT64_ARRAY) {
                            goto put_array_el_slow_path;
                        }
                        JS_FreeValue(ctx, sp[-3]);
                        sp -= 3;
                        BREAK;
                    }
                    if (unlikely(arr_obj->class_id != JS_CLASS_ARRAY))
                        goto put_array_el_slow_path;
                put_array_el_plain:
                    if (unlikely(idx >= (uint32_t)arr_obj->u.array.count)) {
                        uint32_t new_len, array_len;
                        if (unlikely(idx != (uint32_t)arr_obj->u.array.count || !arr_obj->fast_array || !can_extend_fast_array(arr_obj))) {
                            goto put_array_el_slow_path;
                        }
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
                } else {
                put_array_el_slow_path:
                    sf->cur_pc = pc;
                    ret = JS_SetPropertyValue(ctx, sp[-3], sp[-2], sp[-1], JS_PROP_THROW_STRICT);
                    JS_FreeValue(ctx, sp[-3]);
                    sp -= 3;
                    if (unlikely(ret < 0))
                        goto exception;
                }
            }
            BREAK;

            CASE(OP_put_ref_value)
                :
            {
                int ret;
                JSAtom atom;
                sf->cur_pc = pc;
                atom = JS_ValueToAtom(ctx, sp[-2]);
                if (unlikely(atom == JS_ATOM_NULL))
                    goto exception;
                if (unlikely(JS_IsUndefined(sp[-3]))) {
                    if (is_strict_mode(ctx)) {
                        JS_ThrowReferenceErrorNotDefined(ctx, atom);
                        JS_FreeAtom(ctx, atom);
                        goto exception;
                    } else {
                        sp[-3] = JS_DupValue(ctx, ctx->global_obj);
                    }
                }
                ret = JS_HasProperty(ctx, sp[-3], atom);
                if (unlikely(ret <= 0)) {
                    if (unlikely(ret < 0)) {
                        JS_FreeAtom(ctx, atom);
                        goto exception;
                    }
                    if (is_strict_mode(ctx)) {
                        JS_ThrowReferenceErrorNotDefined(ctx, atom);
                        JS_FreeAtom(ctx, atom);
                        goto exception;
                    }
                }
                ret = JS_SetPropertyInternal(ctx, sp[-3], atom, sp[-1], sp[-3], JS_PROP_THROW_STRICT);
                JS_FreeAtom(ctx, atom);
                JS_FreeValue(ctx, sp[-2]);
                JS_FreeValue(ctx, sp[-3]);
                sp -= 3;
                if (unlikely(ret < 0))
                    goto exception;
            }
            BREAK;

            CASE(OP_put_super_value)
                :
            {
                int ret;
                JSAtom atom;
                sf->cur_pc = pc;
                if (JS_VALUE_GET_TAG(sp[-3]) != JS_TAG_OBJECT) {
                    JS_ThrowTypeErrorNotAnObject(ctx);
                    goto exception;
                }
                atom = JS_ValueToAtom(ctx, sp[-2]);
                if (unlikely(atom == JS_ATOM_NULL))
                    goto exception;
                ret = JS_SetPropertyInternal(ctx, sp[-3], atom, sp[-1], sp[-4],
                    JS_PROP_THROW_STRICT);
                JS_FreeAtom(ctx, atom);
                JS_FreeValue(ctx, sp[-4]);
                JS_FreeValue(ctx, sp[-3]);
                JS_FreeValue(ctx, sp[-2]);
                sp -= 4;
                if (ret < 0)
                    goto exception;
            }
            BREAK;

            CASE(OP_define_array_el)
                :
            {
                int ret;
                ret = JS_DefinePropertyValueValue(ctx, sp[-3], JS_DupValue(ctx, sp[-2]), sp[-1],
                    JS_PROP_C_W_E | JS_PROP_THROW);
                sp -= 1;
                if (unlikely(ret < 0))
                    goto exception;
            }
            BREAK;

            CASE(OP_append)
                :
            {
                sf->cur_pc = pc;
                if (js_append_enumerate(ctx, sp))
                    goto exception;
                JS_FreeValue(ctx, *--sp);
            }
            BREAK;

            CASE(OP_copy_data_properties)
                :
            {
                int mask;

                mask = *pc++;
                sf->cur_pc = pc;
                if (JS_CopyDataProperties(ctx, sp[-1 - (mask & 3)],
                        sp[-1 - ((mask >> 2) & 7)],
                        sp[-1 - ((mask >> 5) & 7)], 0))
                    goto exception;
            }
            BREAK;

            CASE(OP_add)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    int64_t r;
                    r = (int64_t)JS_VALUE_GET_INT(op1) + JS_VALUE_GET_INT(op2);
                    if (unlikely((int)r != r)) {
                        sp[-2] = __JS_NewFloat64(ctx, (double)r);
                    } else {
                        sp[-2] = JS_NewInt32(ctx, r);
                    }
                    sp--;
                } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                    double d1, d2;
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1))) {
                        d1 = JS_VALUE_GET_FLOAT64(op1);
                    } else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                        d1 = JS_VALUE_GET_INT(op1);
                    } else {
                        goto add_slow_case;
                    }
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                        d2 = JS_VALUE_GET_FLOAT64(op2);
                    } else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT) {
                        d2 = JS_VALUE_GET_INT(op2);
                    } else {
                        goto add_slow_case;
                    }
                    sp[-2] = __JS_NewFloat64(ctx, d1 + d2);
                    sp--;
                } else if (JS_IsString(op1) && JS_IsString(op2)) {
                    sp[-2] = JS_ConcatString(ctx, op1, op2);
                    sp--;
                    if (JS_IsException(sp[-1]))
                        goto exception;
                } else {
                add_slow_case:
                    sf->cur_pc = pc;
                    if (js_add_slow(ctx, sp))
                        goto exception;
                    sp--;
                }
            }
            BREAK;
            CASE(OP_add_loc)
                :
            {
                JSValue op2;
                JSValue* pv;
                int idx;
                idx = *pc;
                pc += 1;

                op2 = sp[-1];
                pv = &var_buf[idx];
                if (likely(JS_VALUE_IS_BOTH_INT(*pv, op2))) {
                    int64_t r;
                    r = (int64_t)JS_VALUE_GET_INT(*pv) + JS_VALUE_GET_INT(op2);
                    if (unlikely((int)r != r)) {
                        *pv = __JS_NewFloat64(ctx, (double)r);
                    } else {
                        *pv = JS_NewInt32(ctx, r);
                    }
                    sp--;
                } else if (JS_VALUE_IS_BOTH_FLOAT(*pv, op2)) {
                    *pv = __JS_NewFloat64(ctx, JS_VALUE_GET_FLOAT64(*pv) + JS_VALUE_GET_FLOAT64(op2));
                    sp--;
                } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(*pv)) && JS_VALUE_GET_TAG(op2) == JS_TAG_INT) {
                    *pv = __JS_NewFloat64(ctx, JS_VALUE_GET_FLOAT64(*pv) + JS_VALUE_GET_INT(op2));
                    sp--;
                } else if (JS_VALUE_GET_TAG(*pv) == JS_TAG_INT && JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                    *pv = __JS_NewFloat64(ctx, JS_VALUE_GET_INT(*pv) + JS_VALUE_GET_FLOAT64(op2));
                    sp--;
                } else if (JS_VALUE_GET_TAG(*pv) == JS_TAG_STRING && JS_VALUE_GET_TAG(op2) == JS_TAG_STRING) {
                    sp--;
                    sf->cur_pc = pc;
                    if (JS_ConcatStringInPlace(ctx, JS_VALUE_GET_STRING(*pv), op2)) {
                        JS_FreeValue(ctx, op2);
                    } else {
                        op2 = JS_ConcatString(ctx, JS_DupValue(ctx, *pv), op2);
                        if (JS_IsException(op2))
                            goto exception;
                        set_value(ctx, pv, op2);
                    }
                } else {
                    JSValue ops[2];
                    sf->cur_pc = pc;
                    ops[0] = JS_DupValue(ctx, *pv);
                    ops[1] = op2;
                    sp--;
                    if (js_add_slow(ctx, ops + 2))
                        goto exception;
                    set_value(ctx, pv, ops[0]);
                }
            }
            BREAK;
            CASE(OP_sub)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    int64_t r;
                    r = (int64_t)JS_VALUE_GET_INT(op1) - JS_VALUE_GET_INT(op2);
                    if (unlikely((int)r != r)) {
                        sp[-2] = __JS_NewFloat64(ctx, (double)r);
                    } else {
                        sp[-2] = JS_NewInt32(ctx, r);
                    }
                    sp--;
                } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                    double d1, d2;
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1))) {
                        d1 = JS_VALUE_GET_FLOAT64(op1);
                    } else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                        d1 = JS_VALUE_GET_INT(op1);
                    } else {
                        goto binary_arith_slow;
                    }
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                        d2 = JS_VALUE_GET_FLOAT64(op2);
                    } else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT) {
                        d2 = JS_VALUE_GET_INT(op2);
                    } else {
                        goto binary_arith_slow;
                    }
                    sp[-2] = __JS_NewFloat64(ctx, d1 - d2);
                    sp--;
                } else {
                    goto binary_arith_slow;
                }
            }
            BREAK;
            CASE(OP_mul)
                :
            {
                JSValue op1, op2;
                double d;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    int32_t v1, v2;
                    int64_t r;
                    v1 = JS_VALUE_GET_INT(op1);
                    v2 = JS_VALUE_GET_INT(op2);
                    r = (int64_t)v1 * v2;
                    if (unlikely((int)r != r)) {
                        d = (double)r;
                        goto mul_fp_res;
                    }
                    if (unlikely(r == 0 && (v1 | v2) < 0)) {
                        d = -0.0;
                        goto mul_fp_res;
                    }
                    sp[-2] = JS_NewInt32(ctx, r);
                    sp--;
                } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                    double d1, d2;
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1))) {
                        d1 = JS_VALUE_GET_FLOAT64(op1);
                    } else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                        d1 = JS_VALUE_GET_INT(op1);
                    } else {
                        goto binary_arith_slow;
                    }
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                        d2 = JS_VALUE_GET_FLOAT64(op2);
                    } else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT) {
                        d2 = JS_VALUE_GET_INT(op2);
                    } else {
                        goto binary_arith_slow;
                    }
                    d = d1 * d2;
                mul_fp_res:
                    sp[-2] = __JS_NewFloat64(ctx, d);
                    sp--;
                } else {
                    goto binary_arith_slow;
                }
            }
            BREAK;
            CASE(OP_div)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    int v1, v2;
                    v1 = JS_VALUE_GET_INT(op1);
                    v2 = JS_VALUE_GET_INT(op2);
                    sp[-2] = JS_NewFloat64(ctx, (double)v1 / (double)v2);
                    sp--;
                } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                    double d1, d2;
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1))) {
                        d1 = JS_VALUE_GET_FLOAT64(op1);
                    } else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                        d1 = JS_VALUE_GET_INT(op1);
                    } else {
                        goto binary_arith_slow;
                    }
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                        d2 = JS_VALUE_GET_FLOAT64(op2);
                    } else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT) {
                        d2 = JS_VALUE_GET_INT(op2);
                    } else {
                        goto binary_arith_slow;
                    }
                    sp[-2] = __JS_NewFloat64(ctx, d1 / d2);
                    sp--;
                } else {
                    goto binary_arith_slow;
                }
            }
            BREAK;
            CASE(OP_mod)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    int v1, v2, r;
                    v1 = JS_VALUE_GET_INT(op1);
                    v2 = JS_VALUE_GET_INT(op2);
                    if (unlikely(v1 < 0 || v2 <= 0))
                        goto binary_arith_slow;
                    r = v1 % v2;
                    sp[-2] = JS_NewInt32(ctx, r);
                    sp--;
                } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                    double d1, d2;
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1))) {
                        d1 = JS_VALUE_GET_FLOAT64(op1);
                    } else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                        d1 = JS_VALUE_GET_INT(op1);
                    } else {
                        goto binary_arith_slow;
                    }
                    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {
                        d2 = JS_VALUE_GET_FLOAT64(op2);
                    } else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT) {
                        d2 = JS_VALUE_GET_INT(op2);
                    } else {
                        goto binary_arith_slow;
                    }
                    sp[-2] = __JS_NewFloat64(ctx, fmod(d1, d2));
                    sp--;
                } else {
                    goto binary_arith_slow;
                }
            }
            BREAK;
            CASE(OP_pow)
                : binary_arith_slow : sf->cur_pc = pc;
            if (js_binary_arith_slow(ctx, sp, opcode))
                goto exception;
            sp--;
            BREAK;

            CASE(OP_plus)
                :
            {
                JSValue op1;
                uint32_t tag;
                op1 = sp[-1];
                tag = JS_VALUE_GET_TAG(op1);
                if (tag == JS_TAG_INT || JS_TAG_IS_FLOAT64(tag)) {
                } else if (tag == JS_TAG_NULL || tag == JS_TAG_BOOL) {
                    sp[-1] = JS_NewInt32(ctx, JS_VALUE_GET_INT(op1));
                } else {
                    sf->cur_pc = pc;
                    if (js_unary_arith_slow(ctx, sp, opcode))
                        goto exception;
                }
            }
            BREAK;
            CASE(OP_neg)
                :
            {
                JSValue op1;
                uint32_t tag;
                int val;
                double d;
                op1 = sp[-1];
                tag = JS_VALUE_GET_TAG(op1);
                if (tag == JS_TAG_INT || tag == JS_TAG_BOOL || tag == JS_TAG_NULL) {
                    val = JS_VALUE_GET_INT(op1);
                    if (unlikely(val == 0)) {
                        d = -0.0;
                        goto neg_fp_res;
                    }
                    if (unlikely(val == INT32_MIN)) {
                        d = -(double)val;
                        goto neg_fp_res;
                    }
                    sp[-1] = JS_NewInt32(ctx, -val);
                } else if (JS_TAG_IS_FLOAT64(tag)) {
                    d = -JS_VALUE_GET_FLOAT64(op1);
                neg_fp_res:
                    sp[-1] = __JS_NewFloat64(ctx, d);
                } else {
                    sf->cur_pc = pc;
                    if (js_unary_arith_slow(ctx, sp, opcode))
                        goto exception;
                }
            }
            BREAK;
            CASE(OP_inc)
                :
            {
                JSValue op1;
                int val;
                op1 = sp[-1];
                if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                    val = JS_VALUE_GET_INT(op1);
                    if (unlikely(val == INT32_MAX))
                        goto inc_slow;
                    sp[-1] = JS_NewInt32(ctx, val + 1);
                } else {
                inc_slow:
                    sf->cur_pc = pc;
                    if (js_unary_arith_slow(ctx, sp, opcode))
                        goto exception;
                }
            }
            BREAK;
            CASE(OP_dec)
                :
            {
                JSValue op1;
                int val;
                op1 = sp[-1];
                if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                    val = JS_VALUE_GET_INT(op1);
                    if (unlikely(val == INT32_MIN))
                        goto dec_slow;
                    sp[-1] = JS_NewInt32(ctx, val - 1);
                } else {
                dec_slow:
                    sf->cur_pc = pc;
                    if (js_unary_arith_slow(ctx, sp, opcode))
                        goto exception;
                }
            }
            BREAK;
            CASE(OP_post_inc)
                :
            {
                JSValue op1;
                int val;
                op1 = sp[-1];
                if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                    val = JS_VALUE_GET_INT(op1);
                    if (unlikely(val == INT32_MAX))
                        goto post_inc_slow;
                    sp[0] = JS_NewInt32(ctx, val + 1);
                } else {
                post_inc_slow:
                    sf->cur_pc = pc;
                    if (js_post_inc_slow(ctx, sp, opcode))
                        goto exception;
                }
                sp++;
            }
            BREAK;
            CASE(OP_post_dec)
                :
            {
                JSValue op1;
                int val;
                op1 = sp[-1];
                if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                    val = JS_VALUE_GET_INT(op1);
                    if (unlikely(val == INT32_MIN))
                        goto post_dec_slow;
                    sp[0] = JS_NewInt32(ctx, val - 1);
                } else {
                post_dec_slow:
                    sf->cur_pc = pc;
                    if (js_post_inc_slow(ctx, sp, opcode))
                        goto exception;
                }
                sp++;
            }
            BREAK;
            CASE(OP_inc_loc)
                :
            {
                JSValue op1;
                int val;
                int idx;
                idx = *pc;
                pc += 1;

                op1 = var_buf[idx];
                if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                    val = JS_VALUE_GET_INT(op1);
                    if (unlikely(val == INT32_MAX))
                        goto inc_loc_slow;
                    var_buf[idx] = JS_NewInt32(ctx, val + 1);
                } else {
                inc_loc_slow:
                    sf->cur_pc = pc;
                    op1 = JS_DupValue(ctx, op1);
                    if (js_unary_arith_slow(ctx, &op1 + 1, OP_inc))
                        goto exception;
                    set_value(ctx, &var_buf[idx], op1);
                }
            }
            BREAK;
            CASE(OP_dec_loc)
                :
            {
                JSValue op1;
                int val;
                int idx;
                idx = *pc;
                pc += 1;

                op1 = var_buf[idx];
                if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                    val = JS_VALUE_GET_INT(op1);
                    if (unlikely(val == INT32_MIN))
                        goto dec_loc_slow;
                    var_buf[idx] = JS_NewInt32(ctx, val - 1);
                } else {
                dec_loc_slow:
                    sf->cur_pc = pc;
                    op1 = JS_DupValue(ctx, op1);
                    if (js_unary_arith_slow(ctx, &op1 + 1, OP_dec))
                        goto exception;
                    set_value(ctx, &var_buf[idx], op1);
                }
            }
            BREAK;
            CASE(OP_not)
                :
            {
                JSValue op1;
                op1 = sp[-1];
                if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {
                    sp[-1] = JS_NewInt32(ctx, ~JS_VALUE_GET_INT(op1));
                } else {
                    sf->cur_pc = pc;
                    if (js_not_slow(ctx, sp))
                        goto exception;
                }
            }
            BREAK;

            CASE(OP_shl)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    uint32_t v1, v2;
                    v1 = JS_VALUE_GET_INT(op1);
                    v2 = JS_VALUE_GET_INT(op2);
                    v2 &= 0x1f;
                    sp[-2] = JS_NewInt32(ctx, v1 << v2);
                    sp--;
                } else {
                    sf->cur_pc = pc;
                    if (js_binary_logic_slow(ctx, sp, opcode))
                        goto exception;
                    sp--;
                }
            }
            BREAK;
            CASE(OP_shr)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    uint32_t v2;
                    v2 = JS_VALUE_GET_INT(op2);
                    v2 &= 0x1f;
                    sp[-2] = JS_NewUint32(ctx,
                        (uint32_t)JS_VALUE_GET_INT(op1) >> v2);
                    sp--;
                } else {
                    sf->cur_pc = pc;
                    if (js_shr_slow(ctx, sp))
                        goto exception;
                    sp--;
                }
            }
            BREAK;
            CASE(OP_sar)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    uint32_t v2;
                    v2 = JS_VALUE_GET_INT(op2);
                    v2 &= 0x1f;
                    sp[-2] = JS_NewInt32(ctx,
                        (int)JS_VALUE_GET_INT(op1) >> v2);
                    sp--;
                } else {
                    sf->cur_pc = pc;
                    if (js_binary_logic_slow(ctx, sp, opcode))
                        goto exception;
                    sp--;
                }
            }
            BREAK;
            CASE(OP_and)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    sp[-2] = JS_NewInt32(ctx,
                        JS_VALUE_GET_INT(op1) & JS_VALUE_GET_INT(op2));
                    sp--;
                } else {
                    sf->cur_pc = pc;
                    if (js_binary_logic_slow(ctx, sp, opcode))
                        goto exception;
                    sp--;
                }
            }
            BREAK;
            CASE(OP_or)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    sp[-2] = JS_NewInt32(ctx,
                        JS_VALUE_GET_INT(op1) | JS_VALUE_GET_INT(op2));
                    sp--;
                } else {
                    sf->cur_pc = pc;
                    if (js_binary_logic_slow(ctx, sp, opcode))
                        goto exception;
                    sp--;
                }
            }
            BREAK;
            CASE(OP_xor)
                :
            {
                JSValue op1, op2;
                op1 = sp[-2];
                op2 = sp[-1];
                if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {
                    sp[-2] = JS_NewInt32(ctx,
                        JS_VALUE_GET_INT(op1) ^ JS_VALUE_GET_INT(op2));
                    sp--;
                } else {
                    sf->cur_pc = pc;
                    if (js_binary_logic_slow(ctx, sp, opcode))
                        goto exception;
                    sp--;
                }
            }
            BREAK;

#define OP_CMP(opcode, binary_op, slow_call)                                                               \
    CASE(opcode)                                                                                           \
        :                                                                                                  \
    {                                                                                                      \
        JSValue op1, op2;                                                                                  \
        op1 = sp[-2];                                                                                      \
        op2 = sp[-1];                                                                                      \
        if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {                                                      \
            sp[-2] = JS_NewBool(ctx, JS_VALUE_GET_INT(op1) binary_op JS_VALUE_GET_INT(op2));               \
            sp--;                                                                                          \
        } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) { \
            double d1, d2;                                                                                 \
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1))) {                                                \
                d1 = JS_VALUE_GET_FLOAT64(op1);                                                            \
            } else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {                                              \
                d1 = JS_VALUE_GET_INT(op1);                                                                \
            } else {                                                                                       \
                goto opcode##_slow_case;                                                                   \
            }                                                                                              \
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {                                                \
                d2 = JS_VALUE_GET_FLOAT64(op2);                                                            \
            } else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT) {                                              \
                d2 = JS_VALUE_GET_INT(op2);                                                                \
            } else {                                                                                       \
                goto opcode##_slow_case;                                                                   \
            }                                                                                              \
            sp[-2] = JS_NewBool(ctx, d1 binary_op d2);                                                     \
            sp--;                                                                                          \
        } else {                                                                                           \
            opcode##_slow_case : sf->cur_pc = pc;                                                          \
            if (slow_call)                                                                                 \
                goto exception;                                                                            \
            sp--;                                                                                          \
        }                                                                                                  \
    }                                                                                                      \
    BREAK

            OP_CMP(OP_lt, <, js_relational_slow(ctx, sp, opcode));
            OP_CMP(OP_lte, <=, js_relational_slow(ctx, sp, opcode));
            OP_CMP(OP_gt, >, js_relational_slow(ctx, sp, opcode));
            OP_CMP(OP_gte, >=, js_relational_slow(ctx, sp, opcode));

#define OP_CMP_BRANCH(fused_op, base_op, binary_op, take)                                                  \
    CASE(fused_op)                                                                                         \
        :                                                                                                  \
    {                                                                                                      \
        JSValue op1, op2;                                                                                  \
        int res;                                                                                           \
        op1 = sp[-2];                                                                                      \
        op2 = sp[-1];                                                                                      \
        pc += 4;                                                                                           \
        if (likely(JS_VALUE_IS_BOTH_INT(op1, op2))) {                                                      \
            res = JS_VALUE_GET_INT(op1) binary_op JS_VALUE_GET_INT(op2);                                   \
            sp -= 2;                                                                                       \
        } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1)) || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) { \
            double d1, d2;                                                                                 \
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op1))) {                                                \
                d1 = JS_VALUE_GET_FLOAT64(op1);                                                            \
            } else if (JS_VALUE_GET_TAG(op1) == JS_TAG_INT) {                                              \
                d1 = JS_VALUE_GET_INT(op1);                                                                \
            } else {                                                                                       \
                goto fused_op##_slow;                                                                      \
            }                                                                                              \
            if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(op2))) {                                                \
                d2 = JS_VALUE_GET_FLOAT64(op2);                                                            \
            } else if (JS_VALUE_GET_TAG(op2) == JS_TAG_INT) {                                              \
                d2 = JS_VALUE_GET_INT(op2);                                                                \
            } else {                                                                                       \
                goto fused_op##_slow;                                                                      \
            }                                                                                              \
            res = d1 binary_op d2;                                                                         \
            sp -= 2;                                                                                       \
        } else {                                                                                           \
            fused_op##_slow : sf->cur_pc = pc;                                                             \
            if (js_relational_slow(ctx, sp, base_op))                                                      \
                goto exception;                                                                            \
            res = JS_VALUE_GET_INT(sp[-2]);                                                                \
            sp -= 2;                                                                                       \
        }                                                                                                  \
        if (take) {                                                                                        \
            int32_t br_diff = (int32_t)get_u32(pc - 4);                                                    \
            pc += br_diff - 4;                                                                             \
            if (unlikely(br_diff <= 0) && unlikely(js_poll_interrupts(ctx)))                               \
                goto exception;                                                                            \
        }                                                                                                  \
    }                                                                                                      \
    BREAK

            OP_CMP_BRANCH(OP_lt_if_false, OP_lt, <, !res);
            OP_CMP_BRANCH(OP_lte_if_false, OP_lte, <=, !res);
            OP_CMP_BRANCH(OP_gt_if_false, OP_gt, >, !res);
            OP_CMP_BRANCH(OP_gte_if_false, OP_gte, >=, !res);

#define OP_STRICT_EQ_BRANCH(fused_op, inv, take)                                \
    CASE(fused_op)                                                              \
        :                                                                       \
    {                                                                           \
        JSValue op1, op2;                                                       \
        int res;                                                                \
        uint32_t tag1, tag2;                                                    \
        op1 = sp[-2];                                                           \
        op2 = sp[-1];                                                           \
        pc += 4;                                                                \
        tag1 = JS_VALUE_GET_TAG(op1);                                           \
        tag2 = JS_VALUE_GET_TAG(op2);                                           \
        if (likely(tag1 == JS_TAG_INT)) {                                       \
            if (tag2 == JS_TAG_INT) {                                           \
                res = JS_VALUE_GET_INT(op1) == JS_VALUE_GET_INT(op2);           \
            } else if (JS_TAG_IS_FLOAT64(tag2)) {                               \
                res = (JS_VALUE_GET_INT(op1) == JS_VALUE_GET_FLOAT64(op2));     \
            } else {                                                            \
                JS_FreeValue(ctx, op2);                                         \
                res = FALSE;                                                    \
            }                                                                   \
        } else if (JS_TAG_IS_FLOAT64(tag1)) {                                   \
            if (tag2 == JS_TAG_INT) {                                           \
                res = JS_VALUE_GET_FLOAT64(op1) == JS_VALUE_GET_INT(op2);       \
            } else if (JS_TAG_IS_FLOAT64(tag2)) {                               \
                res = (JS_VALUE_GET_FLOAT64(op1) == JS_VALUE_GET_FLOAT64(op2)); \
            } else {                                                            \
                JS_FreeValue(ctx, op2);                                         \
                res = FALSE;                                                    \
            }                                                                   \
        } else if (tag1 == JS_TAG_OBJECT) {                                     \
            if (tag2 == JS_TAG_OBJECT) {                                        \
                res = JS_VALUE_GET_OBJ(op1) == JS_VALUE_GET_OBJ(op2);           \
            } else {                                                            \
                res = FALSE;                                                    \
            }                                                                   \
            JS_FreeValue(ctx, op1);                                             \
            JS_FreeValue(ctx, op2);                                             \
        } else if (tag1 == JS_TAG_NULL || tag1 == JS_TAG_UNDEFINED) {           \
            res = (tag1 == tag2);                                               \
            JS_FreeValue(ctx, op2);                                             \
        } else if (tag1 == JS_TAG_STRING && tag2 == JS_TAG_STRING) {            \
            res = js_string_eq(ctx, JS_VALUE_GET_STRING(op1),                   \
                JS_VALUE_GET_STRING(op2));                                      \
            JS_FreeValue(ctx, op1);                                             \
            JS_FreeValue(ctx, op2);                                             \
        } else {                                                                \
            res = js_strict_eq2(ctx, op1, op2, JS_EQ_STRICT);                   \
            JS_FreeValue(ctx, op1);                                             \
            JS_FreeValue(ctx, op2);                                             \
        }                                                                       \
        sp -= 2;                                                                \
        if (take) {                                                             \
            int32_t br_diff = (int32_t)get_u32(pc - 4);                         \
            pc += br_diff - 4;                                                  \
            if (unlikely(br_diff <= 0) && unlikely(js_poll_interrupts(ctx)))    \
                goto exception;                                                 \
        }                                                                       \
    }                                                                           \
    BREAK

            OP_STRICT_EQ_BRANCH(OP_strict_eq_if_false, 0, !(res ^ 0));
            OP_STRICT_EQ_BRANCH(OP_strict_neq_if_false, 1, !(res ^ 1));
            OP_STRICT_EQ_BRANCH(OP_strict_eq_if_true, 0, (res ^ 0));
            OP_STRICT_EQ_BRANCH(OP_strict_neq_if_true, 1, (res ^ 1));

            CASE(OP_ext)
                :
#define OP2_READ_LOC_LOC(v1, v2)                                 \
    int ai = get_u16(pc), bi = get_u16(pc + 2);                  \
    JSValue v1 = var_buf[ai], v2 = var_buf[bi];                  \
    if (unlikely(JS_IsUninitialized(v1))) {                      \
        JS_ThrowReferenceErrorUninitialized2(ctx, b, ai, FALSE); \
        goto exception;                                          \
    }                                                            \
    if (unlikely(JS_IsUninitialized(v2))) {                      \
        JS_ThrowReferenceErrorUninitialized2(ctx, b, bi, FALSE); \
        goto exception;                                          \
    }
                SWITCH2(*pc++)
            {
                CASE2(OP2_add_loc_loc)
                    :
                {
                    OP2_READ_LOC_LOC(op1, op2)
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
                            sf->cur_pc = pc;
                            if (js_add_slow(ctx, ops + 2))
                                goto exception;
                            *sp++ = ops[0];
                        }
                    }
                }
                BREAK;
                CASE2(OP2_sub_loc_loc)
                    :
                {
                    OP2_READ_LOC_LOC(op1, op2)
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
                            sf->cur_pc = pc;
                            if (js_binary_arith_slow(ctx, ops + 2, OP_sub))
                                goto exception;
                            *sp++ = ops[0];
                        }
                    }
                }
                BREAK;
                CASE2(OP2_mul_loc_loc)
                    :
                {
                    OP2_READ_LOC_LOC(op1, op2)
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
                            sf->cur_pc = pc;
                            if (js_binary_arith_slow(ctx, ops + 2, OP_mul))
                                goto exception;
                            *sp++ = ops[0];
                        }
                    }
                }
                BREAK;
#define OP2_PROBE_STRICT_EQ(o1, o2)                                                        \
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
            res = js_string_eq(ctx, JS_VALUE_GET_STRING(o1),                               \
                JS_VALUE_GET_STRING(o2));                                                  \
        } else {                                                                           \
            res = js_strict_eq2(ctx, o1, o2, JS_EQ_STRICT);                                \
        }                                                                                  \
    }
                CASE2(OP2_streq_const_if_false)
                    :
                {
                    JSValue op1 = sp[-1];
                    const JSValue op2 = b->cpool[pc[4]];
                    int res;
                    int32_t diff = (int32_t)get_u32(pc);
                    pc += 5;
                    OP2_PROBE_STRICT_EQ(op1, op2)
                    if (unlikely(!res)) {
                        pc += diff - 5;
                        if (unlikely(diff <= 0) && unlikely(js_poll_interrupts(ctx)))
                            goto exception;
                    }
                }
                BREAK;
                CASE2(OP2_streq_varprop_if_false)
                    :
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
                        obj = JS_DupValue(ctx, *var_refs[vr]->pvalue);
                        obj_owned = TRUE;
                        if (unlikely(JS_IsUninitialized(obj))) {
                            JSClosureVar* cv = &b->closure_var[vr];
                            if (cv->is_lexical) {
                                JS_ThrowReferenceErrorUninitialized(ctx,
                                    cv->var_name);
                                goto exception;
                            }
                            sf->cur_pc = pc;
                            obj = JS_GetPropertyInternal(ctx,
                                ctx->global_obj,
                                cv->var_name,
                                ctx->global_obj,
                                1);
                            if (unlikely(JS_IsException(obj)))
                                goto exception;
                            obj_owned = TRUE;
                        }
                    }
                    if (likely(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT)) {
                        po = JS_VALUE_GET_OBJ(obj);
                        for (;;) {
                            prs = find_own_property(&pr, po, atom);
                            if (prs) {
                                if (unlikely(prs->flags & JS_PROP_TMASK))
                                    goto varprop_slow;
                                val = JS_DupValue(ctx, pr->u.value);
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
                    } else {
                    varprop_slow:
                        sf->cur_pc = pc;
                        val = JS_GetPropertyInternal(ctx, obj, atom, obj, 0);
                        if (unlikely(JS_IsException(val))) {
                            if (obj_owned)
                                JS_FreeValue(ctx, obj);
                            goto exception;
                        }
                    }
                    OP2_PROBE_STRICT_EQ(op1, val)
                    JS_FreeValue(ctx, val);
                    if (obj_owned)
                        JS_FreeValue(ctx, obj);
                    if (unlikely(!res)) {
                        pc += diff - 10;
                        if (unlikely(diff <= 0) && unlikely(js_poll_interrupts(ctx)))
                            goto exception;
                    }
                }
                BREAK;

#define OP2_CALL_METHOD(argc_, kind_, load_d_, adv_)                                                                                   \
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
        if (likely(JS_VALUE_GET_TAG(mf_this) == JS_TAG_OBJECT)) {                                                                      \
            mf_p = JS_VALUE_GET_OBJ(mf_this);                                                                                          \
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
            sf->cur_pc = pc + (load_d_);                                                                                               \
            mf_func = JS_GetPropertyInternal(ctx, mf_obj, mf_atom,                                                                     \
                sp[-1], 0);                                                                                                            \
            if (unlikely(JS_IsException(mf_func)))                                                                                     \
                goto exception;                                                                                                        \
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
                if (unlikely(JS_IsUninitialized(var_buf[mf_idx]))) {                                                                   \
                    JS_FreeValue(ctx, mf_func);                                                                                        \
                    JS_ThrowReferenceErrorUninitialized2(ctx, b,                                                                       \
                        mf_idx,                                                                                                        \
                        FALSE);                                                                                                        \
                    goto exception;                                                                                                    \
                }                                                                                                                      \
            }                                                                                                                          \
            mf_arg = JS_DupValue(ctx, mf_form == 2 ? arg_buf[mf_idx] : var_buf[mf_idx]);                                               \
            break;                                                                                                                     \
        case 2:                                                                                                                        \
            mf_form = pc[4];                                                                                                           \
            if (mf_form == 0)                                                                                                          \
                mf_arg = JS_NewInt32(ctx, get_i8(pc + 5));                                                                             \
            else                                                                                                                       \
                mf_arg = JS_DupValue(ctx, b->cpool[pc[5]]);                                                                            \
            break;                                                                                                                     \
        case 3:                                                                                                                        \
            mf_arg = JS_DupValue(ctx, b->cpool[get_u32(pc + 4)]);                                                                      \
            break;                                                                                                                     \
        default:                                                                                                                       \
            break;                                                                                                                     \
        }                                                                                                                              \
                                                                                                                                       \
        sf->cur_pc = pc + (adv_);                                                                                                      \
        mf_argv1[0] = mf_arg;                                                                                                          \
        mf_argv1[1] = JS_UNDEFINED;                                                                                                    \
        if (likely(JS_VALUE_GET_TAG(mf_func) == JS_TAG_OBJECT) && JS_VALUE_GET_OBJ(mf_func)->class_id == JS_CLASS_C_FUNCTION) {        \
                                                                                                                                       \
            JSObject* mf_cf = JS_VALUE_GET_OBJ(mf_func);                                                                               \
            JSCFunctionEnum mf_cp = mf_cf->u.cfunc.cproto;                                                                             \
            if (unlikely(mf_argc < mf_cf->u.cfunc.length) || unlikely(mf_cp != JS_CFUNC_generic && mf_cp != JS_CFUNC_generic_magic)) { \
                ret_val = js_call_c_function(ctx, mf_func, mf_this,                                                                    \
                    mf_argc,                                                                                                           \
                    (JSValueConst*)                                                                                                    \
                        mf_argv1,                                                                                                      \
                    0);                                                                                                                \
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
            }                                                                                                                          \
        } else {                                                                                                                       \
                                                                                                                                       \
            ret_val = JS_CallInternal(ctx, mf_func, mf_this,                                                                           \
                JS_UNDEFINED, mf_argc,                                                                                                 \
                mf_argv1, 0);                                                                                                          \
        }                                                                                                                              \
        if (unlikely(JS_IsException(ret_val))) {                                                                                       \
                                                                                                                                       \
            JS_FreeValue(ctx, mf_func);                                                                                                \
            if (mf_argc)                                                                                                               \
                JS_FreeValue(ctx, mf_argv1[0]);                                                                                        \
            goto exception;                                                                                                            \
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
                CASE2(OP2_call_method0)
                    : OP2_CALL_METHOD(0, 0, 4, 4)
                          BREAK;
                CASE2(OP2_call_method1_loc)
                    : OP2_CALL_METHOD(1, 1, 3, 7)
                          BREAK;
                CASE2(OP2_call_method1_imm8)
                    : OP2_CALL_METHOD(1, 2, 3, 6)
                          BREAK;
                CASE2(OP2_call_method1_const)
                    : OP2_CALL_METHOD(1, 3, 3, 8)
                          BREAK;
#undef OP2_CALL_METHOD
#undef OP2_PROBE_STRICT_EQ

                CASE2(OP2_using_new)
                    :
                {
                    JSValue cap = js_using_new_capability(ctx);
                    if (JS_IsException(cap))
                        goto exception;
                    *sp++ = cap;
                }
                BREAK;

                CASE2(OP2_using_add)
                    :
                {
                    int hint = get_u16(pc);
                    pc += 4;
                    sf->cur_pc = pc;
                    if (js_using_add(ctx, sp[-2], sp[-1], hint))
                        goto exception;
                    JS_FreeValue(ctx, sp[-2]);
                    sp[-2] = sp[-1];
                    sp--;
                }
                BREAK;

                CASE2(OP2_using_dispose)
                    :
                {
                    int disp_flags = get_u16(pc);
                    JSValue ret;
                    pc += 4;
                    sf->cur_pc = pc;
                    ret = js_using_dispose_value(ctx, sp[-2],
                        (disp_flags & 2) ? sp[-1]
                                         : JS_UNINITIALIZED,
                        disp_flags & 1);
                    JS_FreeValue(ctx, sp[-2]);
                    if (!(disp_flags & 2))
                        JS_FreeValue(ctx, sp[-1]);
                    sp[-2] = ret;
                    sp--;
                    if (JS_IsException(ret))
                        goto exception;
                }
                BREAK;
#if !DIRECT_DISPATCH
                default:
                    JS_ThrowInternalError(ctx, "invalid ext opcode: pc=%u opcode=0x%02x",
                        (int)(pc - b->byte_code_buf - 1), (int)pc[-1]);
                    goto exception;
#endif
            }
#undef OP2_READ_LOC_LOC
            BREAK;

#define OP_CMP_EQ(opcode, inv)                                                           \
    CASE(opcode)                                                                         \
        :                                                                                \
    {                                                                                    \
        JSValue ce_op1, ce_op2;                                                          \
        int ce_res;                                                                      \
        uint32_t ce_tag1, ce_tag2;                                                       \
        ce_op1 = sp[-2];                                                                 \
        ce_op2 = sp[-1];                                                                 \
        ce_tag1 = JS_VALUE_GET_TAG(ce_op1);                                              \
        ce_tag2 = JS_VALUE_GET_TAG(ce_op2);                                              \
        if (likely(ce_tag1 == JS_TAG_INT)) {                                             \
            if (ce_tag2 == JS_TAG_INT) {                                                 \
                ce_res = JS_VALUE_GET_INT(ce_op1) == JS_VALUE_GET_INT(ce_op2);           \
            } else if (JS_TAG_IS_FLOAT64(ce_tag2)) {                                     \
                ce_res = (JS_VALUE_GET_INT(ce_op1) == JS_VALUE_GET_FLOAT64(ce_op2));     \
            } else {                                                                     \
                goto slow_eq##inv;                                                       \
            }                                                                            \
        } else if (JS_TAG_IS_FLOAT64(ce_tag1)) {                                         \
            if (ce_tag2 == JS_TAG_INT) {                                                 \
                ce_res = JS_VALUE_GET_FLOAT64(ce_op1) == JS_VALUE_GET_INT(ce_op2);       \
            } else if (JS_TAG_IS_FLOAT64(ce_tag2)) {                                     \
                ce_res = (JS_VALUE_GET_FLOAT64(ce_op1) == JS_VALUE_GET_FLOAT64(ce_op2)); \
            } else {                                                                     \
                goto slow_eq##inv;                                                       \
            }                                                                            \
        } else if (ce_tag1 == JS_TAG_OBJECT) {                                           \
            if (ce_tag2 == JS_TAG_NULL || ce_tag2 == JS_TAG_UNDEFINED) {                 \
                JSObject* ce_p = JS_VALUE_GET_OBJ(ce_op1);                               \
                ce_res = ce_p->is_HTMLDDA;                                               \
                JS_FreeValue(ctx, ce_op1);                                               \
            } else if (ce_tag2 == JS_TAG_OBJECT) {                                       \
                ce_res = JS_VALUE_GET_OBJ(ce_op1) == JS_VALUE_GET_OBJ(ce_op2);           \
                JS_FreeValue(ctx, ce_op1);                                               \
                JS_FreeValue(ctx, ce_op2);                                               \
            } else {                                                                     \
                goto slow_eq##inv;                                                       \
            }                                                                            \
        } else if (ce_tag1 == JS_TAG_NULL || ce_tag1 == JS_TAG_UNDEFINED) {              \
            if (ce_tag2 == JS_TAG_NULL || ce_tag2 == JS_TAG_UNDEFINED) {                 \
                ce_res = TRUE;                                                           \
            } else if (ce_tag2 == JS_TAG_OBJECT) {                                       \
                JSObject* ce_p = JS_VALUE_GET_OBJ(ce_op2);                               \
                ce_res = ce_p->is_HTMLDDA;                                               \
                JS_FreeValue(ctx, ce_op2);                                               \
            } else {                                                                     \
                goto slow_eq##inv;                                                       \
            }                                                                            \
        } else if (ce_tag1 == JS_TAG_STRING && ce_tag2 == JS_TAG_STRING) {               \
            ce_res = js_string_eq(ctx, JS_VALUE_GET_STRING(ce_op1),                      \
                JS_VALUE_GET_STRING(ce_op2));                                            \
            JS_FreeValue(ctx, ce_op1);                                                   \
            JS_FreeValue(ctx, ce_op2);                                                   \
        } else {                                                                         \
            slow_eq##inv : sf->cur_pc = pc;                                              \
            if (js_eq_slow(ctx, sp, inv))                                                \
                goto exception;                                                          \
            sp--;                                                                        \
            goto slow_eq_done##inv;                                                      \
        }                                                                                \
        sp[-2] = JS_NewBool(ctx, ce_res ^ inv);                                          \
        sp--;                                                                            \
        slow_eq_done##inv :;                                                             \
    }                                                                                    \
    BREAK

            OP_CMP_EQ(OP_eq, 0);
            OP_CMP_EQ(OP_neq, 1);

#define OP_CMP_STRICT_EQ(opcode, inv)                                           \
    CASE(opcode)                                                                \
        :                                                                       \
    {                                                                           \
        JSValue op1, op2;                                                       \
        int res;                                                                \
        uint32_t tag1, tag2;                                                    \
        op1 = sp[-2];                                                           \
        op2 = sp[-1];                                                           \
        tag1 = JS_VALUE_GET_TAG(op1);                                           \
        tag2 = JS_VALUE_GET_TAG(op2);                                           \
        if (likely(tag1 == JS_TAG_INT)) {                                       \
            if (tag2 == JS_TAG_INT) {                                           \
                res = JS_VALUE_GET_INT(op1) == JS_VALUE_GET_INT(op2);           \
            } else if (JS_TAG_IS_FLOAT64(tag2)) {                               \
                res = (JS_VALUE_GET_INT(op1) == JS_VALUE_GET_FLOAT64(op2));     \
            } else {                                                            \
                JS_FreeValue(ctx, op2);                                         \
                res = FALSE;                                                    \
            }                                                                   \
        } else if (JS_TAG_IS_FLOAT64(tag1)) {                                   \
            if (tag2 == JS_TAG_INT) {                                           \
                res = JS_VALUE_GET_FLOAT64(op1) == JS_VALUE_GET_INT(op2);       \
            } else if (JS_TAG_IS_FLOAT64(tag2)) {                               \
                res = (JS_VALUE_GET_FLOAT64(op1) == JS_VALUE_GET_FLOAT64(op2)); \
            } else {                                                            \
                JS_FreeValue(ctx, op2);                                         \
                res = FALSE;                                                    \
            }                                                                   \
        } else if (tag1 == JS_TAG_OBJECT) {                                     \
            if (tag2 == JS_TAG_OBJECT) {                                        \
                res = JS_VALUE_GET_OBJ(op1) == JS_VALUE_GET_OBJ(op2);           \
            } else {                                                            \
                res = FALSE;                                                    \
            }                                                                   \
            JS_FreeValue(ctx, op1);                                             \
            JS_FreeValue(ctx, op2);                                             \
        } else if (tag1 == JS_TAG_NULL || tag1 == JS_TAG_UNDEFINED) {           \
            res = (tag1 == tag2);                                               \
            JS_FreeValue(ctx, op2);                                             \
        } else if (tag1 == JS_TAG_STRING && tag2 == JS_TAG_STRING) {            \
            res = js_string_eq(ctx, JS_VALUE_GET_STRING(op1),                   \
                JS_VALUE_GET_STRING(op2));                                      \
            JS_FreeValue(ctx, op1);                                             \
            JS_FreeValue(ctx, op2);                                             \
        } else {                                                                \
            res = js_strict_eq2(ctx, op1, op2, JS_EQ_STRICT);                   \
            JS_FreeValue(ctx, op1);                                             \
            JS_FreeValue(ctx, op2);                                             \
        }                                                                       \
        sp[-2] = JS_NewBool(ctx, res ^ inv);                                    \
        sp--;                                                                   \
    }                                                                           \
    BREAK

            OP_CMP_STRICT_EQ(OP_strict_eq, 0);
            OP_CMP_STRICT_EQ(OP_strict_neq, 1);

            CASE(OP_in)
                : sf->cur_pc = pc;
            if (js_operator_in(ctx, sp))
                goto exception;
            sp--;
            BREAK;
            CASE(OP_private_in)
                : sf->cur_pc = pc;
            if (js_operator_private_in(ctx, sp))
                goto exception;
            sp--;
            BREAK;
            CASE(OP_instanceof)
                : sf->cur_pc = pc;
            if (js_operator_instanceof(ctx, sp))
                goto exception;
            sp--;
            BREAK;
            CASE(OP_typeof)
                :
            {
                JSValue op1;
                JSAtom atom;

                op1 = sp[-1];
                atom = js_operator_typeof(ctx, op1);
                JS_FreeValue(ctx, op1);
                sp[-1] = js_typeof_string(ctx, atom);
            }
            BREAK;
            CASE(OP_delete)
                : sf->cur_pc = pc;
            if (js_operator_delete(ctx, sp))
                goto exception;
            sp--;
            BREAK;
            CASE(OP_delete_var)
                :
            {
                JSAtom atom;
                int ret;

                atom = get_u32(pc);
                pc += 4;
                sf->cur_pc = pc;

                ret = JS_DeleteGlobalVar(ctx, atom);
                if (unlikely(ret < 0))
                    goto exception;
                *sp++ = JS_NewBool(ctx, ret);
            }
            BREAK;

            CASE(OP_to_object)
                : if (JS_VALUE_GET_TAG(sp[-1]) != JS_TAG_OBJECT)
            {
                sf->cur_pc = pc;
                ret_val = JS_ToObject(ctx, sp[-1]);
                if (JS_IsException(ret_val))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = ret_val;
            }
            BREAK;

            CASE(OP_to_propkey)
                : switch (JS_VALUE_GET_TAG(sp[-1]))
            {
            case JS_TAG_INT:
            case JS_TAG_STRING:
            case JS_TAG_SYMBOL:
                break;
            default:
                sf->cur_pc = pc;
                ret_val = JS_ToPropertyKey(ctx, sp[-1]);
                if (JS_IsException(ret_val))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = ret_val;
                break;
            }
            BREAK;

#if 0
        CASE(OP_to_string):
            if (JS_VALUE_GET_TAG(sp[-1]) != JS_TAG_STRING) {
                ret_val = JS_ToString(ctx, sp[-1]);
                if (JS_IsException(ret_val))
                    goto exception;
                JS_FreeValue(ctx, sp[-1]);
                sp[-1] = ret_val;
            }
            BREAK;
#endif
            CASE(OP_with_get_var)
                : CASE(OP_with_put_var)
                : CASE(OP_with_delete_var)
                : CASE(OP_with_make_ref)
                : CASE(OP_with_get_ref)
                :
            {
                JSAtom atom;
                int32_t diff;
                JSValue obj, val;
                int ret, is_with;
                atom = get_u32(pc);
                diff = get_u32(pc + 4);
                is_with = pc[8];
                pc += 9;
                sf->cur_pc = pc;

                obj = sp[-1];
                ret = JS_HasProperty(ctx, obj, atom);
                if (unlikely(ret < 0))
                    goto exception;
                if (ret) {
                    if (is_with) {
                        ret = js_has_unscopable(ctx, obj, atom);
                        if (unlikely(ret < 0))
                            goto exception;
                        if (ret)
                            goto no_with;
                    }
                    switch (opcode) {
                    case OP_with_get_var:
                        ret = JS_HasProperty(ctx, obj, atom);
                        if (unlikely(ret <= 0)) {
                            if (ret < 0)
                                goto exception;
                            if (is_strict_mode(ctx)) {
                                JS_ThrowReferenceErrorNotDefined(ctx, atom);
                                goto exception;
                            }
                            val = JS_UNDEFINED;
                        } else {
                            val = JS_GetProperty(ctx, obj, atom);
                            if (unlikely(JS_IsException(val)))
                                goto exception;
                        }
                        set_value(ctx, &sp[-1], val);
                        break;
                    case OP_with_put_var:
                        ret = JS_HasProperty(ctx, obj, atom);
                        if (unlikely(ret <= 0)) {
                            if (ret < 0)
                                goto exception;
                            if (is_strict_mode(ctx)) {
                                JS_ThrowReferenceErrorNotDefined(ctx, atom);
                                goto exception;
                            }
                        }
                        ret = JS_SetPropertyInternal(ctx, obj, atom, sp[-2], obj,
                            JS_PROP_THROW_STRICT);
                        JS_FreeValue(ctx, sp[-1]);
                        sp -= 2;
                        if (unlikely(ret < 0))
                            goto exception;
                        break;
                    case OP_with_delete_var:
                        ret = JS_DeleteProperty(ctx, obj, atom, 0);
                        if (unlikely(ret < 0))
                            goto exception;
                        JS_FreeValue(ctx, sp[-1]);
                        sp[-1] = JS_NewBool(ctx, ret);
                        break;
                    case OP_with_make_ref:
                        *sp++ = JS_AtomToValue(ctx, atom);
                        break;
                    case OP_with_get_ref:
                        ret = JS_HasProperty(ctx, obj, atom);
                        if (unlikely(ret < 0))
                            goto exception;
                        if (!ret) {
                            val = JS_UNDEFINED;
                        } else {
                            val = JS_GetProperty(ctx, obj, atom);
                            if (unlikely(JS_IsException(val)))
                                goto exception;
                        }
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
            BREAK;

            CASE(OP_await)
                : ret_val = JS_NewInt32(ctx, FUNC_RET_AWAIT);
            goto done_generator;
            CASE(OP_yield)
                : ret_val = JS_NewInt32(ctx, FUNC_RET_YIELD);
            goto done_generator;
            CASE(OP_yield_star)
                : CASE(OP_async_yield_star)
                : ret_val = JS_NewInt32(ctx, FUNC_RET_YIELD_STAR);
            goto done_generator;
            CASE(OP_return_async)
                : ret_val = JS_UNDEFINED;
            goto done_generator;
            CASE(OP_initial_yield)
                : ret_val = JS_NewInt32(ctx, FUNC_RET_INITIAL_YIELD);
            goto done_generator;

            CASE(OP_nop)
                : BREAK;
            CASE(OP_is_undefined_or_null)
                : if (JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_UNDEFINED || JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_NULL)
            {
                goto set_true;
            }
            else
            {
                goto free_and_set_false;
            }
#if SHORT_OPCODES
            CASE(OP_is_undefined)
                : if (JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_UNDEFINED)
            {
                goto set_true;
            }
            else
            {
                goto free_and_set_false;
            }
            CASE(OP_is_null)
                : if (JS_VALUE_GET_TAG(sp[-1]) == JS_TAG_NULL)
            {
                goto set_true;
            }
            else
            {
                goto free_and_set_false;
            }
            CASE(OP_typeof_is_undefined)
                : if (js_operator_typeof(ctx, sp[-1]) == JS_ATOM_undefined)
            {
                goto free_and_set_true;
            }
            else
            {
                goto free_and_set_false;
            }
            CASE(OP_typeof_is_function)
                : if (js_operator_typeof(ctx, sp[-1]) == JS_ATOM_function)
            {
                goto free_and_set_true;
            }
            else
            {
                goto free_and_set_false;
            }
        free_and_set_true:
            JS_FreeValue(ctx, sp[-1]);
#endif
        set_true:
            sp[-1] = JS_TRUE;
            BREAK;
        free_and_set_false:
            JS_FreeValue(ctx, sp[-1]);
            sp[-1] = JS_FALSE;
            BREAK;
            CASE(OP_invalid)
                : DEFAULT : JS_ThrowInternalError(ctx, "invalid opcode: pc=%u opcode=0x%02x",
                                (int)(pc - b->byte_code_buf - 1), opcode);
            goto exception;
        }
    }
exception:
#if CONFIG_LITERAL_BOILERPLATE
    BB_REC_CLEAR();
#endif
    if (is_backtrace_needed(ctx, rt->current_exception)) {
        sf->cur_pc = pc;
        build_backtrace(ctx, rt->current_exception, NULL, 0, 0, 0);
    }
    if (!rt->current_exception_is_uncatchable) {
        while (sp > stack_buf) {
            JSValue val = *--sp;
            JS_FreeValue(ctx, val);
            if (JS_VALUE_GET_TAG(val) == JS_TAG_CATCH_OFFSET) {
                int pos = JS_VALUE_GET_INT(val);
                if (pos == 0) {
                    JS_FreeValue(ctx, sp[-1]);
                    sp--;
                    JS_IteratorClose(ctx, sp[-1], TRUE);
                } else if (pos < 0) {
                    if (sp > stack_buf) {
                        JS_FreeValue(ctx, sp[-1]);
                        sp--;
                    }
                    if (sp > stack_buf) {
                        JS_FreeValue(ctx, sp[-1]);
                        sp--;
                    }
                } else if (pos < b->byte_code_len) {
                    *sp++ = rt->current_exception;
                    rt->current_exception = JS_UNINITIALIZED;
                    pc = b->byte_code_buf + pos;
                    goto restart;
                }
            }
        }
    }
    ret_val = JS_EXCEPTION;
    if (b->func_kind != JS_FUNC_NORMAL) {
    done_generator:
        sf->cur_pc = pc;
        sf->cur_sp = sp;
    } else {
    done:
        if (unlikely(b->var_ref_count != 0)) {
            close_var_refs(rt, b, sf);
        }
        for (pval = local_buf; pval < sp; pval++) {
            JS_FreeValue(ctx, *pval);
        }
    }
#if CONFIG_LITERAL_BOILERPLATE
    BB_REC_CLEAR();
#endif
    if (unlikely(tc_argv_live)) {
        for (i = 0; i < argc; i++)
            JS_FreeValue(ctx, argv[i]);
        js_free(ctx, argv);
        tc_argv_live = FALSE;
    }
    rt->current_stack_frame = sf->prev_frame;
    if (unlikely(tc_pending)) {
        tc_pending = FALSE;
        JS_FreeValue(ctx, tc_owner_func);
        JS_FreeValue(ctx, tc_owner_this);
        tc_owner_func = tc_func;
        tc_owner_this = tc_this;
        if (unlikely(js_poll_interrupts(ctx))) {
            for (i = 0; i < tc_argc; i++)
                JS_FreeValue(ctx, tc_argv[i]);
            js_free(ctx, tc_argv);
            JS_FreeValue(ctx, tc_owner_func);
            JS_FreeValue(ctx, tc_owner_this);
            return JS_EXCEPTION;
        }
        func_obj = tc_func;
        this_obj = tc_this;
        argc = tc_argc;
        argv = tc_argv;
        arg_allocated_size = tc_arg_alloc;
        new_target = JS_UNDEFINED;
        p = JS_VALUE_GET_OBJ(func_obj);
        b = p->u.func.function_bytecode;
        ctx = b->realm;
        tc_entry = TRUE;
        goto restart_call;
    }
    JS_FreeValue(ctx, tc_owner_func);
    JS_FreeValue(ctx, tc_owner_this);
    return ret_val;
}

#ifdef OPCODE_ASM_LABEL
#pragma GCC diagnostic pop
#endif

JSValue JS_Call(JSContext* ctx, JSValueConst func_obj, JSValueConst this_obj,
    int argc, JSValueConst* argv)
{
    return JS_CallInternal(ctx, func_obj, this_obj, JS_UNDEFINED,
        argc, (JSValue*)argv, JS_CALL_FLAG_COPY_ARGV);
}

JSValue JS_CallFree(JSContext* ctx, JSValue func_obj, JSValueConst this_obj,
    int argc, JSValueConst* argv)
{
    JSValue res = JS_CallInternal(ctx, func_obj, this_obj, JS_UNDEFINED,
        argc, (JSValue*)argv, JS_CALL_FLAG_COPY_ARGV);
    JS_FreeValue(ctx, func_obj);
    return res;
}

JSContext* JS_GetFunctionRealm(JSContext* ctx, JSValueConst func_obj)
{
    JSObject* p;
    JSContext* realm;

    if (JS_VALUE_GET_TAG(func_obj) != JS_TAG_OBJECT)
        return ctx;
    p = JS_VALUE_GET_OBJ(func_obj);
    switch (p->class_id) {
    case JS_CLASS_C_FUNCTION:
        realm = p->u.cfunc.realm;
        break;
    case JS_CLASS_BYTECODE_FUNCTION:
    case JS_CLASS_GENERATOR_FUNCTION:
    case JS_CLASS_ASYNC_FUNCTION:
    case JS_CLASS_ASYNC_GENERATOR_FUNCTION: {
        JSFunctionBytecode* b;
        b = p->u.func.function_bytecode;
        realm = b->realm;
    } break;
    case JS_CLASS_PROXY: {
        JSProxyData* s = p->u.opaque;
        if (!s)
            return ctx;
        if (s->is_revoked) {
            JS_ThrowTypeErrorRevokedProxy(ctx);
            return NULL;
        } else {
            realm = JS_GetFunctionRealm(ctx, s->target);
        }
    } break;
    case JS_CLASS_BOUND_FUNCTION: {
        JSBoundFunction* bf = p->u.bound_function;
        realm = JS_GetFunctionRealm(ctx, bf->func_obj);
    } break;
    default:
        realm = ctx;
        break;
    }
    return realm;
}

static JSValue js_resolve_ctor_proto(JSContext* ctx, JSValueConst ctor,
    int class_id)
{
    JSValue proto;
    JSContext* realm;

    if (JS_IsUndefined(ctor)) {
        proto = JS_DupValue(ctx, ctx->class_proto[class_id]);
    } else {
        proto = JS_GetProperty(ctx, ctor, JS_ATOM_prototype);
        if (JS_IsException(proto))
            return proto;
        if (!JS_IsObject(proto)) {
            JS_FreeValue(ctx, proto);
            realm = JS_GetFunctionRealm(ctx, ctor);
            if (!realm)
                return JS_EXCEPTION;
            proto = JS_DupValue(ctx, realm->class_proto[class_id]);
        }
    }
    return proto;
}

JSValue js_create_from_ctor(JSContext* ctx, JSValueConst ctor,
    int class_id)
{
    JSValue proto, obj;

    proto = js_resolve_ctor_proto(ctx, ctor, class_id);
    if (JS_IsException(proto))
        return proto;
    obj = JS_NewObjectProtoClass(ctx, proto, class_id);
    JS_FreeValue(ctx, proto);
    return obj;
}

static JSValue JS_CallConstructorInternal(JSContext* ctx,
    JSValueConst func_obj,
    JSValueConst new_target,
    int argc, JSValue* argv, int flags)
{
    JSObject* p;
    JSFunctionBytecode* b;

    if (js_poll_interrupts(ctx))
        return JS_EXCEPTION;
    flags |= JS_CALL_FLAG_CONSTRUCTOR;
    if (unlikely(JS_VALUE_GET_TAG(func_obj) != JS_TAG_OBJECT))
        goto not_a_function;
    p = JS_VALUE_GET_OBJ(func_obj);
    if (unlikely(!p->is_constructor))
        return JS_ThrowTypeErrorNotAConstructor(ctx, func_obj);
    if (unlikely(p->class_id != JS_CLASS_BYTECODE_FUNCTION)) {
        JSClassCall* call_func;
        call_func = ctx->rt->class_array[p->class_id].call;
        if (!call_func) {
        not_a_function:
            return JS_ThrowTypeError(ctx, "not a function");
        }
        return call_func(ctx, func_obj, new_target, argc,
            (JSValueConst*)argv, flags);
    }

    b = p->u.func.function_bytecode;
    if (b->is_derived_class_constructor) {
        return JS_CallInternal(ctx, func_obj, JS_UNDEFINED, new_target, argc, argv, flags);
    } else {
        JSValue obj, ret;
#if CONFIG_PRESIZE_CTOR
        if (b->ctor_presize) {
            JSValue proto = js_resolve_ctor_proto(ctx, new_target, JS_CLASS_OBJECT);
            if (JS_IsException(proto))
                return JS_EXCEPTION;
            obj = js_ctor_presize_new_this(ctx, b->ctor_presize, proto);
        } else
#endif
            obj = js_create_from_ctor(ctx, new_target, JS_CLASS_OBJECT);
        if (JS_IsException(obj))
            return JS_EXCEPTION;
        ret = JS_CallInternal(ctx, func_obj, obj, new_target, argc, argv, flags);
        if (JS_VALUE_GET_TAG(ret) == JS_TAG_OBJECT || JS_IsException(ret)) {
            JS_FreeValue(ctx, obj);
            return ret;
        } else {
            JS_FreeValue(ctx, ret);
            return obj;
        }
    }
}

JSValue JS_CallConstructor2(JSContext* ctx, JSValueConst func_obj,
    JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    return JS_CallConstructorInternal(ctx, func_obj, new_target,
        argc, (JSValue*)argv,
        JS_CALL_FLAG_COPY_ARGV);
}

JSValue JS_CallConstructor(JSContext* ctx, JSValueConst func_obj,
    int argc, JSValueConst* argv)
{
    return JS_CallConstructorInternal(ctx, func_obj, func_obj,
        argc, (JSValue*)argv,
        JS_CALL_FLAG_COPY_ARGV);
}

JSValue JS_Invoke(JSContext* ctx, JSValueConst this_val, JSAtom atom,
    int argc, JSValueConst* argv)
{
    JSValue func_obj;
    func_obj = JS_GetProperty(ctx, this_val, atom);
    if (JS_IsException(func_obj))
        return func_obj;
    return JS_CallFree(ctx, func_obj, this_val, argc, argv);
}

static JSValue JS_InvokeFree(JSContext* ctx, JSValue this_val, JSAtom atom,
    int argc, JSValueConst* argv)
{
    JSValue res = JS_Invoke(ctx, this_val, atom, argc, argv);
    JS_FreeValue(ctx, this_val);
    return res;
}

static JSAsyncFunctionState* async_func_init(JSContext* ctx,
    JSValueConst func_obj, JSValueConst this_obj,
    int argc, JSValueConst* argv)
{
    JSAsyncFunctionState* s;
    JSObject* p;
    JSFunctionBytecode* b;
    JSStackFrame* sf;
    int i, arg_buf_len, n;

    p = JS_VALUE_GET_OBJ(func_obj);
    b = p->u.func.function_bytecode;
    arg_buf_len = max_int(b->arg_count, argc);
    s = js_malloc(ctx, sizeof(*s) + sizeof(JSValue) * (arg_buf_len + b->var_count + b->stack_size) + sizeof(JSVarRef*) * b->var_ref_count);
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    js_rc(s)->ref_count = 1;
    add_gc_object(ctx->rt, &s->header, JS_GC_OBJ_TYPE_ASYNC_FUNCTION);

    sf = &s->frame;
    sf->js_mode = b->js_mode | JS_MODE_ASYNC;
    sf->cur_pc = b->byte_code_buf;
    sf->arg_buf = (JSValue*)(s + 1);
    sf->cur_func = JS_DupValue(ctx, func_obj);
    s->this_val = JS_DupValue(ctx, this_obj);
    s->argc = argc;
    sf->arg_count = arg_buf_len;
    sf->var_buf = sf->arg_buf + arg_buf_len;
    sf->cur_sp = sf->var_buf + b->var_count;
    sf->var_refs = (JSVarRef**)(sf->cur_sp + b->stack_size);
    for (i = 0; i < b->var_ref_count; i++)
        sf->var_refs[i] = NULL;
    for (i = 0; i < argc; i++)
        sf->arg_buf[i] = JS_DupValue(ctx, argv[i]);
    n = arg_buf_len + b->var_count;
    for (i = argc; i < n; i++)
        sf->arg_buf[i] = JS_UNDEFINED;
    s->resolving_funcs[0] = JS_UNDEFINED;
    s->resolving_funcs[1] = JS_UNDEFINED;
    s->is_completed = FALSE;
    return s;
}

static void async_func_free_frame(JSRuntime* rt, JSAsyncFunctionState* s)
{
    JSStackFrame* sf = &s->frame;
    JSValue* sp;

    if (sf->cur_sp != NULL) {
        for (sp = sf->arg_buf; sp < sf->cur_sp; sp++) {
            JS_FreeValueRT(rt, *sp);
        }
    }
    JS_FreeValueRT(rt, sf->cur_func);
    JS_FreeValueRT(rt, s->this_val);
}

static JSValue async_func_resume(JSContext* ctx, JSAsyncFunctionState* s)
{
    JSRuntime* rt = ctx->rt;
    JSStackFrame* sf = &s->frame;
    JSValue func_obj, ret;

    assert(!s->is_completed);
    if (js_check_stack_overflow(ctx->rt, 0)) {
        ret = JS_ThrowStackOverflow(ctx);
    } else {
        func_obj = JS_MKPTR(JS_TAG_INT, s);
        ret = JS_CallInternal(ctx, func_obj, s->this_val, JS_UNDEFINED,
            s->argc, sf->arg_buf, JS_CALL_FLAG_GENERATOR);
    }
    if (JS_IsException(ret) || JS_IsUndefined(ret)) {
        JSObject* p;
        JSFunctionBytecode* b;

        p = JS_VALUE_GET_OBJ(sf->cur_func);
        b = p->u.func.function_bytecode;

        if (JS_IsUndefined(ret)) {
            if (unlikely(sf->cur_sp == NULL)) {
                ret = JS_ThrowInternalError(ctx,
                    "async function exited without async return");
            } else {
                ret = sf->cur_sp[-1];
                sf->cur_sp[-1] = JS_UNDEFINED;
            }
        }
        s->is_completed = TRUE;

        close_var_refs(rt, b, sf);

        async_func_free_frame(rt, s);
    }
    return ret;
}

static void __async_func_free(JSRuntime* rt, JSAsyncFunctionState* s)
{
    if (!s->is_completed) {
        async_func_free_frame(rt, s);
    }

    JS_FreeValueRT(rt, s->resolving_funcs[0]);
    JS_FreeValueRT(rt, s->resolving_funcs[1]);

    remove_gc_object(&s->header);
    if (rt->gc_phase == JS_GC_PHASE_REMOVE_CYCLES && js_rc(s)->ref_count != 0) {
        list_add_tail(&s->header.link, &rt->gc_zero_ref_count_list);
    } else {
        js_free_rt(rt, s);
    }
}

static void async_func_free(JSRuntime* rt, JSAsyncFunctionState* s)
{
    if (--js_rc(s)->ref_count == 0) {
        if (rt->gc_phase != JS_GC_PHASE_REMOVE_CYCLES) {
            list_del(&s->header.link);
            list_add(&s->header.link, &rt->gc_zero_ref_count_list);
            if (rt->gc_phase == JS_GC_PHASE_NONE) {
                free_zero_refcount(rt);
            }
        }
    }
}

typedef enum JSGeneratorStateEnum {
    JS_GENERATOR_STATE_SUSPENDED_START,
    JS_GENERATOR_STATE_SUSPENDED_YIELD,
    JS_GENERATOR_STATE_SUSPENDED_YIELD_STAR,
    JS_GENERATOR_STATE_EXECUTING,
    JS_GENERATOR_STATE_COMPLETED,
} JSGeneratorStateEnum;

typedef struct JSGeneratorData {
    JSGeneratorStateEnum state;
    JSAsyncFunctionState* func_state;
} JSGeneratorData;

static void free_generator_stack_rt(JSRuntime* rt, JSGeneratorData* s)
{
    if (s->state == JS_GENERATOR_STATE_COMPLETED)
        return;
    if (s->func_state) {
        async_func_free(rt, s->func_state);
        s->func_state = NULL;
    }
    s->state = JS_GENERATOR_STATE_COMPLETED;
}

static void js_generator_finalizer(JSRuntime* rt, JSValue obj)
{
    JSGeneratorData* s = JS_GetOpaque(obj, JS_CLASS_GENERATOR);

    if (s) {
        free_generator_stack_rt(rt, s);
        js_free_rt(rt, s);
    }
}

static void free_generator_stack(JSContext* ctx, JSGeneratorData* s)
{
    free_generator_stack_rt(ctx->rt, s);
}

static void js_generator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSGeneratorData* s = p->u.generator_data;

    if (!s || !s->func_state)
        return;
    mark_func(rt, &s->func_state->header);
}

#define GEN_MAGIC_THROW 2

static JSValue js_generator_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    BOOL* pdone, int magic)
{
    JSGeneratorData* s = JS_GetOpaque(this_val, JS_CLASS_GENERATOR);
    JSStackFrame* sf;
    JSValue ret, func_ret;

    *pdone = TRUE;
    if (!s)
        return JS_ThrowTypeError(ctx, "not a generator");
    switch (s->state) {
    default:
    case JS_GENERATOR_STATE_SUSPENDED_START:
        sf = &s->func_state->frame;
        if (magic == GEN_MAGIC_NEXT) {
            goto exec_no_arg;
        } else {
            free_generator_stack(ctx, s);
            goto done;
        }
        break;
    case JS_GENERATOR_STATE_SUSPENDED_YIELD_STAR:
    case JS_GENERATOR_STATE_SUSPENDED_YIELD:
        sf = &s->func_state->frame;
        ret = JS_DupValue(ctx, argv[0]);
        if (magic == GEN_MAGIC_THROW && s->state == JS_GENERATOR_STATE_SUSPENDED_YIELD) {
            JS_Throw(ctx, ret);
            s->func_state->throw_flag = TRUE;
        } else {
            sf->cur_sp[-1] = ret;
            sf->cur_sp[0] = JS_NewInt32(ctx, magic);
            sf->cur_sp++;
        exec_no_arg:
            s->func_state->throw_flag = FALSE;
        }
        s->state = JS_GENERATOR_STATE_EXECUTING;
        func_ret = async_func_resume(ctx, s->func_state);
        s->state = JS_GENERATOR_STATE_SUSPENDED_YIELD;
        if (s->func_state->is_completed) {
            free_generator_stack(ctx, s);
            return func_ret;
        } else {
            assert(JS_VALUE_GET_TAG(func_ret) == JS_TAG_INT);
            ret = sf->cur_sp[-1];
            sf->cur_sp[-1] = JS_UNDEFINED;
            if (JS_VALUE_GET_INT(func_ret) == FUNC_RET_YIELD_STAR) {
                s->state = JS_GENERATOR_STATE_SUSPENDED_YIELD_STAR;
                *pdone = 2;
            } else {
                *pdone = FALSE;
            }
        }
        break;
    case JS_GENERATOR_STATE_COMPLETED:
    done:
        switch (magic) {
        default:
        case GEN_MAGIC_NEXT:
            ret = JS_UNDEFINED;
            break;
        case GEN_MAGIC_RETURN:
            ret = JS_DupValue(ctx, argv[0]);
            break;
        case GEN_MAGIC_THROW:
            ret = JS_Throw(ctx, JS_DupValue(ctx, argv[0]));
            break;
        }
        break;
    case JS_GENERATOR_STATE_EXECUTING:
        ret = JS_ThrowTypeError(ctx, "cannot invoke a running generator");
        break;
    }
    return ret;
}

static JSValue js_generator_function_call(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv,
    int flags)
{
    JSValue obj, func_ret;
    JSGeneratorData* s;

    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        return JS_EXCEPTION;
    s->state = JS_GENERATOR_STATE_SUSPENDED_START;
    s->func_state = async_func_init(ctx, func_obj, this_obj, argc, argv);
    if (!s->func_state) {
        s->state = JS_GENERATOR_STATE_COMPLETED;
        goto fail;
    }

    func_ret = async_func_resume(ctx, s->func_state);
    if (JS_IsException(func_ret))
        goto fail;
    JS_FreeValue(ctx, func_ret);

    obj = js_create_from_ctor(ctx, func_obj, JS_CLASS_GENERATOR);
    if (JS_IsException(obj))
        goto fail;
    JS_SetOpaque(obj, s);
    return obj;
fail:
    free_generator_stack_rt(ctx->rt, s);
    js_free(ctx, s);
    return JS_EXCEPTION;
}

static void js_async_function_resolve_finalizer(JSRuntime* rt, JSValue val)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSAsyncFunctionState* s = p->u.async_function_data;
    if (s) {
        async_func_free(rt, s);
    }
}

static void js_async_function_resolve_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    JSObject* p = JS_VALUE_GET_OBJ(val);
    JSAsyncFunctionState* s = p->u.async_function_data;
    if (s) {
        mark_func(rt, &s->header);
    }
}

static int js_async_function_resolve_create(JSContext* ctx,
    JSAsyncFunctionState* s,
    JSValue* resolving_funcs)
{
    int i;
    JSObject* p;

    for (i = 0; i < 2; i++) {
        resolving_funcs[i] = JS_NewObjectProtoClass(ctx, ctx->function_proto,
            JS_CLASS_ASYNC_FUNCTION_RESOLVE + i);
        if (JS_IsException(resolving_funcs[i])) {
            if (i == 1)
                JS_FreeValue(ctx, resolving_funcs[0]);
            return -1;
        }
        p = JS_VALUE_GET_OBJ(resolving_funcs[i]);
        js_rc(s)->ref_count++;
        p->u.async_function_data = s;
    }
    return 0;
}

static void js_async_function_resume(JSContext* ctx, JSAsyncFunctionState* s)
{
    JSValue func_ret, ret2;

    func_ret = async_func_resume(ctx, s);
    if (s->is_completed) {
        if (JS_IsException(func_ret)) {
            JSValue error;
        fail:
            error = JS_GetException(ctx);
            ret2 = JS_Call(ctx, s->resolving_funcs[1], JS_UNDEFINED,
                1, (JSValueConst*)&error);
            JS_FreeValue(ctx, error);
            JS_FreeValue(ctx, ret2);
        } else {
            ret2 = JS_Call(ctx, s->resolving_funcs[0], JS_UNDEFINED,
                1, (JSValueConst*)&func_ret);
            JS_FreeValue(ctx, func_ret);
            JS_FreeValue(ctx, ret2);
        }
    } else {
        JSValue value, promise, resolving_funcs[2], resolving_funcs1[2];
        int i, res;

        value = s->frame.cur_sp[-1];
        s->frame.cur_sp[-1] = JS_UNDEFINED;

        JS_FreeValue(ctx, func_ret);
        promise = js_promise_resolve(ctx, ctx->promise_ctor,
            1, (JSValueConst*)&value, 0);
        JS_FreeValue(ctx, value);
        if (JS_IsException(promise))
            goto fail;
        if (js_async_function_resolve_create(ctx, s, resolving_funcs)) {
            JS_FreeValue(ctx, promise);
            goto fail;
        }

        for (i = 0; i < 2; i++)
            resolving_funcs1[i] = JS_UNDEFINED;
        res = perform_promise_then(ctx, promise,
            (JSValueConst*)resolving_funcs,
            (JSValueConst*)resolving_funcs1);
        JS_FreeValue(ctx, promise);
        for (i = 0; i < 2; i++)
            JS_FreeValue(ctx, resolving_funcs[i]);
        if (res)
            goto fail;
    }
}

static JSValue js_async_function_resolve_call(JSContext* ctx,
    JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv,
    int flags)
{
    JSObject* p = JS_VALUE_GET_OBJ(func_obj);
    JSAsyncFunctionState* s = p->u.async_function_data;
    BOOL is_reject = p->class_id - JS_CLASS_ASYNC_FUNCTION_RESOLVE;
    JSValueConst arg;

    if (argc > 0)
        arg = argv[0];
    else
        arg = JS_UNDEFINED;
    s->throw_flag = is_reject;
    if (is_reject) {
        JS_Throw(ctx, JS_DupValue(ctx, arg));
    } else {
        s->frame.cur_sp[-1] = JS_DupValue(ctx, arg);
    }
    js_async_function_resume(ctx, s);
    return JS_UNDEFINED;
}

JSValue js_async_function_call(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv, int flags)
{
    JSValue promise;
    JSAsyncFunctionState* s;

    s = async_func_init(ctx, func_obj, this_obj, argc, argv);
    if (!s)
        return JS_EXCEPTION;

    promise = JS_NewPromiseCapability(ctx, s->resolving_funcs);
    if (JS_IsException(promise)) {
        async_func_free(ctx->rt, s);
        return JS_EXCEPTION;
    }

    js_async_function_resume(ctx, s);

    async_func_free(ctx->rt, s);

    return promise;
}

typedef enum JSAsyncGeneratorStateEnum {
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_START,
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD,
    JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD_STAR,
    JS_ASYNC_GENERATOR_STATE_EXECUTING,
    JS_ASYNC_GENERATOR_STATE_AWAITING_RETURN,
    JS_ASYNC_GENERATOR_STATE_COMPLETED,
} JSAsyncGeneratorStateEnum;

typedef struct JSAsyncGeneratorRequest {
    struct list_head link;
    int completion_type;
    JSValue result;
    JSValue promise;
    JSValue resolving_funcs[2];
} JSAsyncGeneratorRequest;

typedef struct JSAsyncGeneratorData {
    JSObject* generator;
    JSAsyncGeneratorStateEnum state;
    JSAsyncFunctionState* func_state;
    struct list_head queue;
} JSAsyncGeneratorData;

static void js_async_generator_free(JSRuntime* rt,
    JSAsyncGeneratorData* s)
{
    struct list_head *el, *el1;
    JSAsyncGeneratorRequest* req;

    list_for_each_safe(el, el1, &s->queue)
    {
        req = list_entry(el, JSAsyncGeneratorRequest, link);
        JS_FreeValueRT(rt, req->result);
        JS_FreeValueRT(rt, req->promise);
        JS_FreeValueRT(rt, req->resolving_funcs[0]);
        JS_FreeValueRT(rt, req->resolving_funcs[1]);
        js_free_rt(rt, req);
    }
    if (s->func_state)
        async_func_free(rt, s->func_state);
    js_free_rt(rt, s);
}

static void js_async_generator_finalizer(JSRuntime* rt, JSValue obj)
{
    JSAsyncGeneratorData* s = JS_GetOpaque(obj, JS_CLASS_ASYNC_GENERATOR);

    if (s) {
        js_async_generator_free(rt, s);
    }
}

static void js_async_generator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    JSAsyncGeneratorData* s = JS_GetOpaque(val, JS_CLASS_ASYNC_GENERATOR);
    struct list_head* el;
    JSAsyncGeneratorRequest* req;
    if (s) {
        list_for_each(el, &s->queue)
        {
            req = list_entry(el, JSAsyncGeneratorRequest, link);
            JS_MarkValue(rt, req->result, mark_func);
            JS_MarkValue(rt, req->promise, mark_func);
            JS_MarkValue(rt, req->resolving_funcs[0], mark_func);
            JS_MarkValue(rt, req->resolving_funcs[1], mark_func);
        }
        if (s->func_state) {
            mark_func(rt, &s->func_state->header);
        }
    }
}

static JSValue js_async_generator_resolve_function(JSContext* ctx,
    JSValueConst this_obj,
    int argc, JSValueConst* argv,
    int magic, JSValue* func_data);

static int js_async_generator_resolve_function_create(JSContext* ctx,
    JSValueConst generator,
    JSValue* resolving_funcs,
    BOOL is_resume_next)
{
    int i;
    JSValue func;

    for (i = 0; i < 2; i++) {
        func = JS_NewCFunctionData(ctx, js_async_generator_resolve_function, 1,
            i + is_resume_next * 2, 1, &generator);
        if (JS_IsException(func)) {
            if (i == 1)
                JS_FreeValue(ctx, resolving_funcs[0]);
            return -1;
        }
        resolving_funcs[i] = func;
    }
    return 0;
}

static int js_async_generator_await(JSContext* ctx,
    JSAsyncGeneratorData* s,
    JSValueConst value)
{
    JSValue promise, resolving_funcs[2], resolving_funcs1[2];
    int i, res;

    promise = js_promise_resolve(ctx, ctx->promise_ctor,
        1, &value, 0);
    if (JS_IsException(promise))
        goto fail;

    if (js_async_generator_resolve_function_create(ctx, JS_MKPTR(JS_TAG_OBJECT, s->generator),
            resolving_funcs, FALSE)) {
        JS_FreeValue(ctx, promise);
        goto fail;
    }

    for (i = 0; i < 2; i++)
        resolving_funcs1[i] = JS_UNDEFINED;
    res = perform_promise_then(ctx, promise,
        (JSValueConst*)resolving_funcs,
        (JSValueConst*)resolving_funcs1);
    JS_FreeValue(ctx, promise);
    for (i = 0; i < 2; i++)
        JS_FreeValue(ctx, resolving_funcs[i]);
    if (res)
        goto fail;
    return 0;
fail:
    return -1;
}

static void js_async_generator_resolve_or_reject(JSContext* ctx,
    JSAsyncGeneratorData* s,
    JSValueConst result,
    int is_reject)
{
    JSAsyncGeneratorRequest* next;
    JSValue ret;

    next = list_entry(s->queue.next, JSAsyncGeneratorRequest, link);
    list_del(&next->link);
    ret = JS_Call(ctx, next->resolving_funcs[is_reject], JS_UNDEFINED, 1,
        &result);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, next->result);
    JS_FreeValue(ctx, next->promise);
    JS_FreeValue(ctx, next->resolving_funcs[0]);
    JS_FreeValue(ctx, next->resolving_funcs[1]);
    js_free(ctx, next);
}

static void js_async_generator_resolve(JSContext* ctx,
    JSAsyncGeneratorData* s,
    JSValueConst value,
    BOOL done)
{
    JSValue result;
    result = js_create_iterator_result(ctx, JS_DupValue(ctx, value), done);
    js_async_generator_resolve_or_reject(ctx, s, result, 0);
    JS_FreeValue(ctx, result);
}

static void js_async_generator_reject(JSContext* ctx,
    JSAsyncGeneratorData* s,
    JSValueConst exception)
{
    js_async_generator_resolve_or_reject(ctx, s, exception, 1);
}

static void js_async_generator_complete(JSContext* ctx,
    JSAsyncGeneratorData* s)
{
    if (s->state != JS_ASYNC_GENERATOR_STATE_COMPLETED) {
        s->state = JS_ASYNC_GENERATOR_STATE_COMPLETED;
        async_func_free(ctx->rt, s->func_state);
        s->func_state = NULL;
    }
}

static int js_async_generator_completed_return(JSContext* ctx,
    JSAsyncGeneratorData* s,
    JSValueConst value)
{
    JSValue promise, resolving_funcs[2], resolving_funcs1[2];
    int res;

    promise = js_promise_resolve(ctx, ctx->promise_ctor, 1, &value,
        0);
    if (JS_IsException(promise)) {
        JSValue err = JS_GetException(ctx);
        promise = js_promise_resolve(ctx, ctx->promise_ctor, 1, (JSValueConst*)&err,
            1);
        JS_FreeValue(ctx, err);
        if (JS_IsException(promise))
            return -1;
    }
    if (js_async_generator_resolve_function_create(ctx,
            JS_MKPTR(JS_TAG_OBJECT, s->generator),
            resolving_funcs1,
            TRUE)) {
        JS_FreeValue(ctx, promise);
        return -1;
    }
    resolving_funcs[0] = JS_UNDEFINED;
    resolving_funcs[1] = JS_UNDEFINED;
    res = perform_promise_then(ctx, promise,
        (JSValueConst*)resolving_funcs1,
        (JSValueConst*)resolving_funcs);
    JS_FreeValue(ctx, resolving_funcs1[0]);
    JS_FreeValue(ctx, resolving_funcs1[1]);
    JS_FreeValue(ctx, promise);
    return res;
}

static void js_async_generator_resume_next(JSContext* ctx,
    JSAsyncGeneratorData* s)
{
    JSAsyncGeneratorRequest* next;
    JSValue func_ret, value;

    for (;;) {
        if (list_empty(&s->queue))
            break;
        next = list_entry(s->queue.next, JSAsyncGeneratorRequest, link);
        switch (s->state) {
        case JS_ASYNC_GENERATOR_STATE_EXECUTING:
            goto resume_exec;
        case JS_ASYNC_GENERATOR_STATE_AWAITING_RETURN:
            goto done;
        case JS_ASYNC_GENERATOR_STATE_SUSPENDED_START:
            if (next->completion_type == GEN_MAGIC_NEXT) {
                goto exec_no_arg;
            } else {
                js_async_generator_complete(ctx, s);
            }
            break;
        case JS_ASYNC_GENERATOR_STATE_COMPLETED:
            if (next->completion_type == GEN_MAGIC_NEXT) {
                js_async_generator_resolve(ctx, s, JS_UNDEFINED, TRUE);
            } else if (next->completion_type == GEN_MAGIC_RETURN) {
                s->state = JS_ASYNC_GENERATOR_STATE_AWAITING_RETURN;
                js_async_generator_completed_return(ctx, s, next->result);
                goto done;
            } else {
                js_async_generator_reject(ctx, s, next->result);
            }
            break;
        case JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD:
        case JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD_STAR:
            value = JS_DupValue(ctx, next->result);
            if (next->completion_type == GEN_MAGIC_THROW && s->state == JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD) {
                JS_Throw(ctx, value);
                s->func_state->throw_flag = TRUE;
            } else {
                s->func_state->frame.cur_sp[-1] = value;
                s->func_state->frame.cur_sp[0] = JS_NewInt32(ctx, next->completion_type);
                s->func_state->frame.cur_sp++;
            exec_no_arg:
                s->func_state->throw_flag = FALSE;
            }
            s->state = JS_ASYNC_GENERATOR_STATE_EXECUTING;
        resume_exec:
            func_ret = async_func_resume(ctx, s->func_state);
            if (s->func_state->is_completed) {
                if (JS_IsException(func_ret)) {
                    value = JS_GetException(ctx);
                    js_async_generator_complete(ctx, s);
                    js_async_generator_reject(ctx, s, value);
                    JS_FreeValue(ctx, value);
                } else {
                    js_async_generator_complete(ctx, s);
                    js_async_generator_resolve(ctx, s, func_ret, TRUE);
                    JS_FreeValue(ctx, func_ret);
                }
            } else {
                int func_ret_code, ret;
                assert(JS_VALUE_GET_TAG(func_ret) == JS_TAG_INT);
                func_ret_code = JS_VALUE_GET_INT(func_ret);
                value = s->func_state->frame.cur_sp[-1];
                s->func_state->frame.cur_sp[-1] = JS_UNDEFINED;
                switch (func_ret_code) {
                case FUNC_RET_YIELD:
                case FUNC_RET_YIELD_STAR:
                    if (func_ret_code == FUNC_RET_YIELD_STAR)
                        s->state = JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD_STAR;
                    else
                        s->state = JS_ASYNC_GENERATOR_STATE_SUSPENDED_YIELD;
                    js_async_generator_resolve(ctx, s, value, FALSE);
                    JS_FreeValue(ctx, value);
                    break;
                case FUNC_RET_AWAIT:
                    ret = js_async_generator_await(ctx, s, value);
                    JS_FreeValue(ctx, value);
                    if (ret < 0) {
                        s->func_state->throw_flag = TRUE;
                        goto resume_exec;
                    }
                    goto done;
                default:
                    abort();
                }
            }
            break;
        default:
            abort();
        }
    }
done:;
}

static JSValue js_async_generator_resolve_function(JSContext* ctx,
    JSValueConst this_obj,
    int argc, JSValueConst* argv,
    int magic, JSValue* func_data)
{
    BOOL is_reject = magic & 1;
    JSAsyncGeneratorData* s = JS_GetOpaque(func_data[0], JS_CLASS_ASYNC_GENERATOR);
    JSValueConst arg = argv[0];

    if (magic >= 2) {
        assert(s->state == JS_ASYNC_GENERATOR_STATE_AWAITING_RETURN || s->state == JS_ASYNC_GENERATOR_STATE_COMPLETED);
        s->state = JS_ASYNC_GENERATOR_STATE_COMPLETED;
        if (is_reject) {
            js_async_generator_reject(ctx, s, arg);
        } else {
            js_async_generator_resolve(ctx, s, arg, TRUE);
        }
        js_async_generator_resume_next(ctx, s);
    } else if (s->state == JS_ASYNC_GENERATOR_STATE_EXECUTING) {
        s->func_state->throw_flag = is_reject;
        if (is_reject) {
            JS_Throw(ctx, JS_DupValue(ctx, arg));
        } else {
            s->func_state->frame.cur_sp[-1] = JS_DupValue(ctx, arg);
        }
        js_async_generator_resume_next(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue js_async_generator_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic)
{
    JSAsyncGeneratorData* s = JS_GetOpaque(this_val, JS_CLASS_ASYNC_GENERATOR);
    JSValue promise, resolving_funcs[2];
    JSAsyncGeneratorRequest* req;

    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;
    if (!s) {
        JSValue err, res2;
        JS_ThrowTypeError(ctx, "not an AsyncGenerator object");
        err = JS_GetException(ctx);
        res2 = JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED,
            1, (JSValueConst*)&err);
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, res2);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        return promise;
    }
    req = js_mallocz(ctx, sizeof(*req));
    if (!req)
        goto fail;
    req->completion_type = magic;
    req->result = JS_DupValue(ctx, argv[0]);
    req->promise = JS_DupValue(ctx, promise);
    req->resolving_funcs[0] = resolving_funcs[0];
    req->resolving_funcs[1] = resolving_funcs[1];
    list_add_tail(&req->link, &s->queue);
    if (s->state != JS_ASYNC_GENERATOR_STATE_EXECUTING) {
        js_async_generator_resume_next(ctx, s);
    }
    return promise;
fail:
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    JS_FreeValue(ctx, promise);
    return JS_EXCEPTION;
}

static JSValue js_async_generator_function_call(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv,
    int flags)
{
    JSValue obj, func_ret;
    JSAsyncGeneratorData* s;

    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        return JS_EXCEPTION;
    s->state = JS_ASYNC_GENERATOR_STATE_SUSPENDED_START;
    init_list_head(&s->queue);
    s->func_state = async_func_init(ctx, func_obj, this_obj, argc, argv);
    if (!s->func_state)
        goto fail;
    func_ret = async_func_resume(ctx, s->func_state);
    if (JS_IsException(func_ret))
        goto fail;
    JS_FreeValue(ctx, func_ret);

    obj = js_create_from_ctor(ctx, func_obj, JS_CLASS_ASYNC_GENERATOR);
    if (JS_IsException(obj))
        goto fail;
    s->generator = JS_VALUE_GET_OBJ(obj);
    JS_SetOpaque(obj, s);
    return obj;
fail:
    js_async_generator_free(ctx->rt, s);
    return JS_EXCEPTION;
}
