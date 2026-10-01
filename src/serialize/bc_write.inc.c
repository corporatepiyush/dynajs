static int JS_WriteObjectAtoms(BCWriterState* s)
{
    JSRuntime* rt = s->ctx->rt;
    DynBuf dbuf1;
    int i, atoms_size;

    dbuf1 = s->dbuf;
    js_dbuf_init(s->ctx, &s->dbuf);
    bc_put_u8(s, BC_VERSION);

    bc_put_leb128(s, s->idx_to_atom_count);
    for (i = 0; i < s->idx_to_atom_count; i++) {
        JSAtomStruct* p = rt->atom_array[s->idx_to_atom[i]];
        JS_WriteString(s, p);
    }
    atoms_size = s->dbuf.size;
    if (dbuf_claim(&dbuf1, atoms_size))
        goto fail;
    memmove(dbuf1.buf + atoms_size, dbuf1.buf, dbuf1.size);
    memcpy(dbuf1.buf, s->dbuf.buf, atoms_size);
    dbuf1.size += atoms_size;
    dbuf_free(&s->dbuf);
    s->dbuf = dbuf1;
    return 0;
fail:
    dbuf_free(&dbuf1);
    return -1;
}

uint8_t* JS_WriteObject2(JSContext* ctx, size_t* psize, JSValueConst obj,
    int flags, uint8_t*** psab_tab, size_t* psab_tab_len)
{
    BCWriterState ss, *s = &ss;

    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->allow_bytecode = ((flags & JS_WRITE_OBJ_BYTECODE) != 0);
    s->allow_sab = ((flags & JS_WRITE_OBJ_SAB) != 0);
    s->allow_reference = ((flags & JS_WRITE_OBJ_REFERENCE) != 0);
    if (s->allow_bytecode)
        s->first_atom = JS_ATOM_END;
    else
        s->first_atom = 1;
    js_dbuf_init(ctx, &s->dbuf);
    js_object_list_init(&s->object_list);

    if (JS_WriteObjectRec(s, obj))
        goto fail;
    if (JS_WriteObjectAtoms(s))
        goto fail;
    js_object_list_end(ctx, &s->object_list);
    js_free(ctx, s->atom_to_idx);
    js_free(ctx, s->idx_to_atom);
    *psize = s->dbuf.size;
    if (psab_tab)
        *psab_tab = s->sab_tab;
    if (psab_tab_len)
        *psab_tab_len = s->sab_tab_len;
    return s->dbuf.buf;
fail:
    js_object_list_end(ctx, &s->object_list);
    js_free(ctx, s->atom_to_idx);
    js_free(ctx, s->idx_to_atom);
    js_free(ctx, s->sab_tab);
    dbuf_free(&s->dbuf);
    *psize = 0;
    if (psab_tab)
        *psab_tab = NULL;
    if (psab_tab_len)
        *psab_tab_len = 0;
    return NULL;
}

uint8_t* JS_WriteObject(JSContext* ctx, size_t* psize, JSValueConst obj,
    int flags)
{
    return JS_WriteObject2(ctx, psize, obj, flags, NULL, NULL);
}

typedef struct BCReaderState {
    JSContext* ctx;
    const uint8_t *buf_start, *ptr, *buf_end;
    uint32_t first_atom;
    uint32_t idx_to_atom_count;
    JSAtom* idx_to_atom;
    int error_state;
    BOOL allow_sab : 8;
    BOOL allow_bytecode : 8;
    BOOL is_rom_data : 8;
    BOOL allow_reference : 8;
    JSObject** objects;
    int objects_count;
    int objects_size;

#ifdef DUMP_READ_OBJECT
    const uint8_t* ptr_last;
    int level;
#endif
} BCReaderState;

#ifdef DUMP_READ_OBJECT
static void __attribute__((format(printf, 2, 3))) bc_read_trace(BCReaderState* s, const char* fmt, ...)
{
    va_list ap;
    int i, n, n0;

    if (!s->ptr_last)
        s->ptr_last = s->buf_start;

    n = n0 = 0;
    if (s->ptr > s->ptr_last || s->ptr == s->buf_start) {
        n0 = printf("%04x: ", (int)(s->ptr_last - s->buf_start));
        n += n0;
    }
    for (i = 0; s->ptr_last < s->ptr; i++) {
        if ((i & 7) == 0 && i > 0) {
            printf("\n%*s", n0, "");
            n = n0;
        }
        n += printf(" %02x", *s->ptr_last++);
    }
    if (*fmt == '}')
        s->level--;
    if (n < 32 + s->level * 2) {
        printf("%*s", 32 + s->level * 2 - n, "");
    }
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    if (strchr(fmt, '{'))
        s->level++;
}
#else
#define bc_read_trace(...)
#endif

static int bc_read_error_end(BCReaderState* s)
{
    if (!s->error_state) {
        JS_ThrowSyntaxError(s->ctx, "read after the end of the buffer");
    }
    return s->error_state = -1;
}

static int bc_get_u8(BCReaderState* s, uint8_t* pval)
{
    if (unlikely(s->buf_end - s->ptr < 1)) {
        *pval = 0;
        return bc_read_error_end(s);
    }
    *pval = *s->ptr++;
    return 0;
}

static int bc_get_u16(BCReaderState* s, uint16_t* pval)
{
    uint16_t v;
    if (unlikely(s->buf_end - s->ptr < 2)) {
        *pval = 0;
        return bc_read_error_end(s);
    }
    v = get_u16(s->ptr);
    if (is_be())
        v = bswap16(v);
    *pval = v;
    s->ptr += 2;
    return 0;
}

static __maybe_unused int bc_get_u32(BCReaderState* s, uint32_t* pval)
{
    uint32_t v;
    if (unlikely(s->buf_end - s->ptr < 4)) {
        *pval = 0;
        return bc_read_error_end(s);
    }
    v = get_u32(s->ptr);
    if (is_be())
        v = bswap32(v);
    *pval = v;
    s->ptr += 4;
    return 0;
}

static int bc_get_u64(BCReaderState* s, uint64_t* pval)
{
    uint64_t v;
    if (unlikely(s->buf_end - s->ptr < 8)) {
        *pval = 0;
        return bc_read_error_end(s);
    }
    v = get_u64(s->ptr);
    if (is_be())
        v = bswap64(v);
    *pval = v;
    s->ptr += 8;
    return 0;
}

static int bc_get_leb128(BCReaderState* s, uint32_t* pval)
{
    int ret;
    ret = get_leb128(pval, s->ptr, s->buf_end);
    if (unlikely(ret < 0))
        return bc_read_error_end(s);
    s->ptr += ret;
    return 0;
}

static int bc_get_sleb128(BCReaderState* s, int32_t* pval)
{
    int ret;
    ret = get_sleb128(pval, s->ptr, s->buf_end);
    if (unlikely(ret < 0))
        return bc_read_error_end(s);
    s->ptr += ret;
    return 0;
}

static int bc_get_leb128_int(BCReaderState* s, int* pval)
{
    return bc_get_leb128(s, (uint32_t*)pval);
}

static int bc_get_leb128_u16(BCReaderState* s, uint16_t* pval)
{
    uint32_t val;
    if (bc_get_leb128(s, &val)) {
        *pval = 0;
        return -1;
    }
    *pval = val;
    return 0;
}

static int bc_get_buf(BCReaderState* s, uint8_t* buf, uint32_t buf_len)
{
    if (buf_len != 0) {
        if (unlikely(!buf || s->buf_end - s->ptr < buf_len))
            return bc_read_error_end(s);
        memcpy(buf, s->ptr, buf_len);
        s->ptr += buf_len;
    }
    return 0;
}

static int bc_idx_to_atom(BCReaderState* s, JSAtom* patom, uint32_t idx)
{
    JSAtom atom;

    if (__JS_AtomIsTaggedInt(idx)) {
        atom = idx;
    } else if (idx < s->first_atom) {
        atom = JS_DupAtom(s->ctx, idx);
    } else {
        idx -= s->first_atom;
        if (idx >= s->idx_to_atom_count) {
            JS_ThrowSyntaxError(s->ctx, "invalid atom index (pos=%u)",
                (unsigned int)(s->ptr - s->buf_start));
            *patom = JS_ATOM_NULL;
            return s->error_state = -1;
        }
        atom = JS_DupAtom(s->ctx, s->idx_to_atom[idx]);
    }
    *patom = atom;
    return 0;
}

static int bc_get_atom(BCReaderState* s, JSAtom* patom)
{
    uint32_t v;
    if (bc_get_leb128(s, &v))
        return -1;
    if (v & 1) {
        *patom = __JS_AtomFromUInt32(v >> 1);
        return 0;
    } else {
        return bc_idx_to_atom(s, patom, v >> 1);
    }
}

static JSString* JS_ReadString(BCReaderState* s)
{
    uint32_t len;
    size_t size;
    BOOL is_wide_char;
    JSString* p;

    if (bc_get_leb128(s, &len))
        return NULL;
    is_wide_char = len & 1;
    len >>= 1;
    if (len > JS_STRING_LEN_MAX) {
        JS_ThrowInternalError(s->ctx, "string too long");
        return NULL;
    }
    p = js_alloc_string(s->ctx, len, is_wide_char);
    if (!p) {
        s->error_state = -1;
        return NULL;
    }
    size = (size_t)len << is_wide_char;
    if ((s->buf_end - s->ptr) < size) {
        bc_read_error_end(s);
        js_free_string(s->ctx->rt, p);
        return NULL;
    }
    memcpy(p->u.str8, s->ptr, size);
    s->ptr += size;
    if (is_wide_char) {
        if (is_be()) {
            uint32_t i;
            for (i = 0; i < len; i++)
                p->u.str16[i] = bswap16(p->u.str16[i]);
        }
    } else {
        p->u.str8[size] = '\0';
    }
#ifdef DUMP_READ_OBJECT
    JS_DumpString(s->ctx->rt, p);
    printf("\n");
#endif
    return p;
}

static uint32_t bc_get_flags(uint32_t flags, int* pidx, int n)
{
    uint32_t val;
    val = (flags >> *pidx) & ((1U << n) - 1);
    *pidx += n;
    return val;
}

static int JS_ReadFunctionBytecode(BCReaderState* s, JSFunctionBytecode* b,
    int byte_code_offset, uint32_t bc_len)
{
    uint8_t* bc_buf;
    int pos, len, op;
    JSAtom atom;
    uint32_t idx;

    if (s->is_rom_data) {
        if (unlikely(s->buf_end - s->ptr < bc_len))
            return bc_read_error_end(s);
        bc_buf = DYN_UNCONST(s->ptr);
        s->ptr += bc_len;
    } else {
        bc_buf = (void*)((uint8_t*)b + byte_code_offset);
        if (bc_get_buf(s, bc_buf, bc_len))
            return -1;
    }
    b->byte_code_buf = bc_buf;

    if (is_be())
        bc_byte_swap(bc_buf, bc_len);

    pos = 0;
    while (pos < bc_len) {
        op = bc_buf[pos];
        if (op >= OP_COUNT) {
            b->byte_code_len = pos;
            return bc_read_error_end(s);
        }
        if (op == OP_ext) {
            int op2;
            if ((uint32_t)2 > bc_len - (uint32_t)pos) {
                b->byte_code_len = pos;
                return bc_read_error_end(s);
            }
            op2 = bc_buf[pos + 1];
            if (op2 >= OP2_COUNT) {
                b->byte_code_len = pos;
                return bc_read_error_end(s);
            }
            len = opcode_info2[op2].size;
            if ((uint32_t)len > bc_len - (uint32_t)pos) {
                b->byte_code_len = pos;
                return bc_read_error_end(s);
            }
            if (opcode_info2[op2].fmt == OP_FMT_loc2 && (op2 == OP2_using_add || op2 == OP2_using_dispose)) {
                if (get_u16(bc_buf + pos + 2) > 3 || get_u16(bc_buf + pos + 4) != 0)
                    goto invalid_operand;
            } else if (opcode_info2[op2].fmt == OP_FMT_loc2 && (get_u16(bc_buf + pos + 2) >= b->var_count || get_u16(bc_buf + pos + 4) >= b->var_count))
                goto invalid_operand;
            if (opcode_info2[op2].fmt == OP_FMT_label_const8 && bc_buf[pos + 6] >= (uint32_t)b->cpool_count)
                goto invalid_operand;
            if (opcode_info2[op2].fmt == OP_FMT_label_var_ref_atom) {
                if (get_u16(bc_buf + pos + 6) >= (uint32_t)b->closure_var_count)
                    goto invalid_operand;
                idx = get_u32(bc_buf + pos + 8);
                if (!s->is_rom_data) {
                    if (bc_idx_to_atom(s, &atom, idx)) {
                        b->byte_code_len = pos;
                        return -1;
                    }
                    put_u32(bc_buf + pos + 8, atom);
                } else {
                    if (bc_idx_to_atom(s, &atom, idx)) {
                        b->byte_code_len = pos;
                        return -1;
                    }
                }
            } else if (opcode_info2[op2].fmt == OP_FMT_atom || opcode_info2[op2].fmt == OP_FMT_atom_loc || opcode_info2[op2].fmt == OP_FMT_atom_i8 || opcode_info2[op2].fmt == OP_FMT_atom_const) {
                if (!s->is_rom_data) {
                    if (bc_idx_to_atom(s, &atom, get_u32(bc_buf + pos + 2))) {
                        b->byte_code_len = pos;
                        return -1;
                    }
                    put_u32(bc_buf + pos + 2, atom);
                } else {
                    if (bc_idx_to_atom(s, &atom, get_u32(bc_buf + pos + 2))) {
                        b->byte_code_len = pos;
                        return -1;
                    }
                }
                switch (opcode_info2[op2].fmt) {
                case OP_FMT_atom_loc:
                    if (bc_buf[pos + 6] > 3)
                        goto invalid_operand;
                    if (bc_buf[pos + 6] == 2) {
                        if (get_u16(bc_buf + pos + 7) >= (uint32_t)b->arg_count)
                            goto invalid_operand;
                    } else if (bc_buf[pos + 6] <= 1) {
                        if (get_u16(bc_buf + pos + 7) >= (uint32_t)b->var_count)
                            goto invalid_operand;
                    }
                    break;
                case OP_FMT_atom_i8:
                    if (bc_buf[pos + 6] > 1)
                        goto invalid_operand;
                    if (bc_buf[pos + 6] == 1 && bc_buf[pos + 7] >= (uint32_t)b->cpool_count)
                        goto invalid_operand;
                    break;
                case OP_FMT_atom_const:
                    if (get_u32(bc_buf + pos + 6) >= (uint32_t)b->cpool_count)
                        goto invalid_operand;
                    break;
                default:
                    break;
                }
            }
            pos += len;
            continue;
        }
        len = short_opcode_info(op).size;
        if ((uint32_t)len > bc_len - (uint32_t)pos) {
            b->byte_code_len = pos;
            return bc_read_error_end(s);
        }
        switch (short_opcode_info(op).fmt) {
        case OP_FMT_atom:
        case OP_FMT_atom_u8:
        case OP_FMT_atom_u16:
        case OP_FMT_atom_label_u8:
        case OP_FMT_atom_label_u16:
            idx = get_u32(bc_buf + pos + 1);
            if (s->is_rom_data) {
                if (bc_idx_to_atom(s, &atom, idx)) {
                    b->byte_code_len = pos;
                    return -1;
                }
            } else {
                if (bc_idx_to_atom(s, &atom, idx)) {
                    b->byte_code_len = pos;
                    return -1;
                }
                put_u32(bc_buf + pos + 1, atom);
#ifdef DUMP_READ_OBJECT
                bc_read_trace(s, "at %d, fixup atom: ", pos + 1);
                print_atom(s->ctx, atom);
                printf("\n");
#endif
            }
            break;
        case OP_FMT_const:
            if (get_u32(bc_buf + pos + 1) >= (uint32_t)b->cpool_count)
                goto invalid_operand;
            break;
        case OP_FMT_const8:
            if (bc_buf[pos + 1] >= (uint32_t)b->cpool_count)
                goto invalid_operand;
            break;
        case OP_FMT_loc:
            if (get_u16(bc_buf + pos + 1) >= b->var_count)
                goto invalid_operand;
            break;
        case OP_FMT_loc8:
            if (bc_buf[pos + 1] >= b->var_count)
                goto invalid_operand;
            break;
        case OP_FMT_arg:
            if (get_u16(bc_buf + pos + 1) >= b->arg_count)
                goto invalid_operand;
            break;
        case OP_FMT_var_ref:
            if (get_u16(bc_buf + pos + 1) >= b->closure_var_count)
                goto invalid_operand;
            break;
        default:
            break;
        }
        pos += len;
    }
    return 0;

invalid_operand:
    b->byte_code_len = pos;
    JS_ThrowSyntaxError(s->ctx, "invalid bytecode operand (pos=%u)",
        (unsigned int)pos);
    return s->error_state = -1;
}

static int JS_ValidateFunctionStackSize(BCReaderState* s, JSFunctionBytecode* b);

static int JS_ValidateFunctionBytecode(BCReaderState* s,
    JSFunctionBytecode* b, int bc_len)
{
    const uint8_t* bc = b->byte_code_buf;
    uint8_t* start_map;
    int pos, len, op, last_op;
    int ret = -1;

    if (bc_len <= 0) {
        JS_ThrowInternalError(s->ctx, "invalid function bytecode (empty)");
        s->error_state = -1;
        return -1;
    }
    start_map = js_mallocz(s->ctx, ((size_t)bc_len + 7) / 8);
    if (!start_map)
        return -1;
    pos = 0;
    last_op = -1;
    while (pos < bc_len) {
        op = bc[pos];
        if (op >= OP_COUNT)
            goto fail;
        start_map[pos >> 3] |= (uint8_t)(1 << (pos & 7));
        if (op == OP_ext) {
            if (bc_len - pos < 2 || bc[pos + 1] >= OP2_COUNT)
                goto fail;
            len = opcode_info2[bc[pos + 1]].size;
        } else {
            len = short_opcode_info(op).size;
        }
        if (len <= 0 || (uint32_t)len > (uint32_t)(bc_len - pos))
            goto fail;
        last_op = op;
        pos += len;
    }
    if (b->func_kind != JS_FUNC_NORMAL && last_op != OP_return_async) {
        JS_ThrowInternalError(s->ctx,
            "invalid function bytecode (no return terminator)");
        goto fail;
    }
#define CHECK_LABEL_TARGET(operand_off, val)                                        \
    do {                                                                            \
        int64_t t = (int64_t)(operand_off) + (val);                                 \
        if (t < 0 || t >= bc_len || !(start_map[t >> 3] & (uint8_t)(1 << (t & 7)))) \
            goto fail;                                                              \
    } while (0)
    pos = 0;
    while (pos < bc_len) {
        op = bc[pos];
        if (op == OP_ext) {
            if (bc_len - pos < 2 || bc[pos + 1] >= OP2_COUNT)
                goto fail;
            if (opcode_info2[bc[pos + 1]].fmt == OP_FMT_label_const8)
                CHECK_LABEL_TARGET(pos + 2, (int32_t)get_u32(bc + pos + 2));
            else if (opcode_info2[bc[pos + 1]].fmt == OP_FMT_label_var_ref_atom)
                CHECK_LABEL_TARGET(pos + 2, (int32_t)get_u32(bc + pos + 2));
            pos += opcode_info2[bc[pos + 1]].size;
            continue;
        }
        len = short_opcode_info(op).size;
        switch (short_opcode_info(op).fmt) {
        case OP_FMT_label:
            CHECK_LABEL_TARGET(pos + 1, (int32_t)get_u32(bc + pos + 1));
            break;
        case OP_FMT_label8:
            CHECK_LABEL_TARGET(pos + 1, (int8_t)bc[pos + 1]);
            break;
        case OP_FMT_label16:
            CHECK_LABEL_TARGET(pos + 1, (int16_t)get_u16(bc + pos + 1));
            break;
        case OP_FMT_label_u16:
        case OP_FMT_atom_label_u8:
        case OP_FMT_atom_label_u16:
            CHECK_LABEL_TARGET(pos + 5, (int32_t)get_u32(bc + pos + 5));
            break;
        case OP_FMT_const:
            if (op == OP_switch) {
                uint32_t cp_idx = get_u32(bc + pos + 1);
                uint64_t tbytes;
                JSString* tbl;
                uint32_t count, k;
                if (cp_idx >= (uint32_t)b->cpool_count)
                    goto fail;
                if (JS_VALUE_GET_TAG(b->cpool[cp_idx]) != JS_TAG_STRING)
                    goto fail;
                tbl = JS_VALUE_GET_STRING(b->cpool[cp_idx]);
                tbytes = ((uint64_t)tbl->len) << tbl->is_wide_char;
                if (tbytes < 8)
                    goto fail;
                count = switch_tbl_get(tbl->u.str8 + 4);
                if (tbytes < 8 + (uint64_t)count * 4)
                    goto fail;
                for (k = 0; k < count; k++) {
                    int32_t off = (int32_t)switch_tbl_get(
                        tbl->u.str8 + 8 + k * 4);
                    if (off != 0)
                        CHECK_LABEL_TARGET(pos, off);
                }
            } else if (op == OP_fclosure) {
                uint32_t cp_idx = get_u32(bc + pos + 1);
                if (cp_idx >= (uint32_t)b->cpool_count || JS_VALUE_GET_TAG(b->cpool[cp_idx]) != JS_TAG_FUNCTION_BYTECODE) {
                    JS_ThrowInternalError(s->ctx,
                        "invalid function bytecode (cpool closure)");
                    goto fail;
                }
            }
            break;
        case OP_FMT_const8:
            if (op == OP_fclosure8) {
                uint32_t cp_idx = bc[pos + 1];
                if (cp_idx >= (uint32_t)b->cpool_count || JS_VALUE_GET_TAG(b->cpool[cp_idx]) != JS_TAG_FUNCTION_BYTECODE) {
                    JS_ThrowInternalError(s->ctx,
                        "invalid function bytecode (cpool closure)");
                    goto fail;
                }
            }
            break;
        default:
            break;
        }
        pos += len;
    }
#undef CHECK_LABEL_TARGET
    if (JS_ValidateFunctionStackSize(s, b))
        goto fail;
    ret = 0;
fail:
    js_free(s->ctx, start_map);
    if (ret < 0) {
        if (!JS_HasException(s->ctx)) {
            JS_ThrowSyntaxError(s->ctx, "invalid branch target (pos=%u)",
                (unsigned int)pos);
        }
        s->error_state = -1;
    }
    return ret;
}

typedef struct BCStackSizeState {
    const uint8_t* bc;
    int bc_len;
    int stack_size_max;
    int stack_len_max;
    uint16_t* stack_level_tab;
    int32_t* catch_pos_tab;
    int* pc_stack;
    int pc_stack_len;
    int pc_stack_size;
} BCStackSizeState;

static int bc_stack_check(JSContext* ctx, BCStackSizeState* s,
    int pos, int op, int stack_len, int catch_pos)
{
    if ((unsigned)pos >= (unsigned)s->bc_len) {
        JS_ThrowInternalError(ctx,
            "invalid function bytecode (branch target 0x%02x out of range)",
            op);
        return -1;
    }
    if (stack_len > s->stack_len_max) {
        s->stack_len_max = stack_len;
        if (s->stack_len_max > s->stack_size_max) {
            JS_ThrowInternalError(ctx,
                "invalid function bytecode (stack_size)");
            return -1;
        }
    }
    if (s->stack_level_tab[pos] != 0xffff) {
        if (s->stack_level_tab[pos] != stack_len) {
            JS_ThrowInternalError(ctx,
                "invalid function bytecode (inconsistent stack size)");
            return -1;
        } else if (s->catch_pos_tab[pos] != catch_pos) {
            JS_ThrowInternalError(ctx,
                "invalid function bytecode (inconsistent catch position)");
            return -1;
        } else {
            return 0;
        }
    }
    s->stack_level_tab[pos] = stack_len;
    s->catch_pos_tab[pos] = catch_pos;
    if (s->pc_stack_len == s->pc_stack_size) {
        int new_size = s->pc_stack_size ? s->pc_stack_size * 2 : 64;
        int* pc_stack = js_realloc(ctx, s->pc_stack,
            sizeof(*pc_stack) * new_size);
        if (!pc_stack)
            return -1;
        s->pc_stack = pc_stack;
        s->pc_stack_size = new_size;
    }
    s->pc_stack[s->pc_stack_len++] = pos;
    return 0;
}

static int JS_ValidateFunctionStackSize(BCReaderState* s, JSFunctionBytecode* b)
{
    JSContext* ctx = s->ctx;
    BCStackSizeState s_s, *ss = &s_s;
    const uint8_t* bc = b->byte_code_buf;
    int bc_len = b->byte_code_len;
    int diff, n_pop, pos_next, stack_len, pos, op, catch_pos, catch_level;
    const JSOpCode* oi;
    int ret = -1;
    size_t lvl_len;

    lvl_len = (sizeof(ss->stack_level_tab[0]) * (size_t)bc_len + 3) & ~(size_t)3;
    ss->stack_level_tab = js_malloc(ctx, lvl_len + sizeof(ss->catch_pos_tab[0]) * (size_t)bc_len);
    if (!ss->stack_level_tab)
        return -1;
    memset(ss->stack_level_tab, 0xff,
        sizeof(ss->stack_level_tab[0]) * (size_t)bc_len);
    ss->catch_pos_tab = (int32_t*)((uint8_t*)ss->stack_level_tab + lvl_len);

    ss->bc = bc;
    ss->bc_len = bc_len;
    ss->stack_size_max = b->stack_size;
    ss->stack_len_max = 0;
    ss->pc_stack = NULL;
    ss->pc_stack_len = 0;
    ss->pc_stack_size = 0;

    if (bc_stack_check(ctx, ss, 0, OP_invalid, 0, -1))
        goto fail;

    while (ss->pc_stack_len > 0) {
        pos = ss->pc_stack[--ss->pc_stack_len];
        stack_len = ss->stack_level_tab[pos];
        catch_pos = ss->catch_pos_tab[pos];
        op = bc[pos];
        if (op == OP_ext) {
            oi = &opcode_info2[bc[pos + 1]];
        } else {
            oi = &short_opcode_info(op);
        }
        pos_next = pos + oi->size;
        n_pop = oi->n_pop;
        if (oi->fmt == OP_FMT_npop || oi->fmt == OP_FMT_npop_u16) {
            n_pop += get_u16(bc + pos + 1);
        } else {
#if SHORT_OPCODES
            if (oi->fmt == OP_FMT_npopx) {
                n_pop += op - OP_call0;
            }
#endif
        }

        if (stack_len < n_pop) {
            JS_ThrowInternalError(ctx,
                "invalid function bytecode (stack underflow)");
            goto fail;
        }
        stack_len += oi->n_push - n_pop;
        if (stack_len > ss->stack_len_max) {
            ss->stack_len_max = stack_len;
            if (ss->stack_len_max > ss->stack_size_max) {
                JS_ThrowInternalError(ctx,
                    "invalid function bytecode (stack_size)");
                goto fail;
            }
        }
        switch (op) {
        case OP_ext:
            if (bc[pos + 1] == OP2_streq_const_if_false || bc[pos + 1] == OP2_streq_varprop_if_false) {
                diff = get_u32(bc + pos + 2);
                if (bc_stack_check(ctx, ss, pos + 2 + diff, op, stack_len,
                        catch_pos))
                    goto fail;
            }
            break;
        case OP_tail_call:
        case OP_tail_call_method:
        case OP_return:
        case OP_return_undef:
        case OP_return_async:
        case OP_throw:
        case OP_throw_error:
        case OP_ret:
            goto done_insn;
        case OP_goto:
            diff = get_u32(bc + pos + 1);
            pos_next = pos + 1 + diff;
            break;
#if SHORT_OPCODES
        case OP_goto16:
            diff = (int16_t)get_u16(bc + pos + 1);
            pos_next = pos + 1 + diff;
            break;
        case OP_goto8:
            diff = (int8_t)bc[pos + 1];
            pos_next = pos + 1 + diff;
            break;
        case OP_if_true8:
        case OP_if_false8:
            diff = (int8_t)bc[pos + 1];
            if (bc_stack_check(ctx, ss, pos + 1 + diff, op, stack_len,
                    catch_pos))
                goto fail;
            break;
#endif
        case OP_if_true:
        case OP_if_false:
        case OP_lt_if_false:
        case OP_lte_if_false:
        case OP_gt_if_false:
        case OP_gte_if_false:
        case OP_strict_eq_if_false:
        case OP_strict_neq_if_false:
        case OP_strict_eq_if_true:
        case OP_strict_neq_if_true:
            diff = get_u32(bc + pos + 1);
            if (bc_stack_check(ctx, ss, pos + 1 + diff, op, stack_len,
                    catch_pos))
                goto fail;
            break;
        case OP_switch: {
            const uint8_t* t = JS_VALUE_GET_STRING(b->cpool[get_u32(bc + pos + 1)])
                                   ->u.str8;
            uint32_t count = switch_tbl_get(t + 4), k;
            for (k = 0; k < count; k++) {
                int32_t off = (int32_t)switch_tbl_get(t + 8 + k * 4);
                if (off != 0 && bc_stack_check(ctx, ss, pos + off, op, stack_len, catch_pos))
                    goto fail;
            }
        } break;
        case OP_gosub:
            diff = get_u32(bc + pos + 1);
            if (bc_stack_check(ctx, ss, pos + 1 + diff, op, stack_len + 1,
                    catch_pos))
                goto fail;
            break;
        case OP_with_get_var:
        case OP_with_delete_var:
            diff = get_u32(bc + pos + 5);
            if (bc_stack_check(ctx, ss, pos + 5 + diff, op, stack_len + 1,
                    catch_pos))
                goto fail;
            break;
        case OP_with_make_ref:
        case OP_with_get_ref:
            diff = get_u32(bc + pos + 5);
            if (bc_stack_check(ctx, ss, pos + 5 + diff, op, stack_len + 2,
                    catch_pos))
                goto fail;
            break;
        case OP_with_put_var:
            diff = get_u32(bc + pos + 5);
            if (bc_stack_check(ctx, ss, pos + 5 + diff, op, stack_len - 1,
                    catch_pos))
                goto fail;
            break;
        case OP_catch:
            diff = get_u32(bc + pos + 1);
            if (bc_stack_check(ctx, ss, pos + 1 + diff, op, stack_len,
                    catch_pos))
                goto fail;
            catch_pos = pos;
            break;
        case OP_for_of_start:
        case OP_for_await_of_start:
            catch_pos = pos;
            break;
        case OP_drop:
            catch_level = stack_len;
            goto check_catch;
        case OP_nip:
            catch_level = stack_len - 1;
            goto check_catch;
        case OP_nip1:
            catch_level = stack_len - 1;
            goto check_catch;
        case OP_iterator_close:
            catch_level = stack_len + 2;
        check_catch:
            if (catch_pos >= 0) {
                int level;
                level = ss->stack_level_tab[catch_pos];
                if (bc[catch_pos] != OP_catch)
                    level++;
                if (catch_level == level) {
                    catch_pos = ss->catch_pos_tab[catch_pos];
                }
            }
            break;
        case OP_nip_catch:
            if (catch_pos < 0) {
                JS_ThrowInternalError(ctx,
                    "invalid function bytecode (nip_catch: no catch op)");
                goto fail;
            }
            stack_len = ss->stack_level_tab[catch_pos];
            if (bc[catch_pos] != OP_catch)
                stack_len++;
            stack_len++;
            catch_pos = ss->catch_pos_tab[catch_pos];
            break;
        default:
            break;
        }
        if (bc_stack_check(ctx, ss, pos_next, op, stack_len, catch_pos))
            goto fail;
    done_insn:;
    }
    ret = 0;
fail:
    js_free(ctx, ss->pc_stack);
    js_free(ctx, ss->stack_level_tab);
    return ret;
}

static JSValue JS_ReadBigInt(BCReaderState* s)
{
    JSValue obj = JS_UNDEFINED;
    uint32_t len, i, n;
    JSBigInt* p;
    js_limb_t v;
    uint8_t v8;

    if (bc_get_leb128(s, &len))
        goto fail;
    bc_read_trace(s, "len=%" PRId64 "\n", (int64_t)len);
    if (len == 0) {
        bc_read_trace(s, "}\n");
        return __JS_NewShortBigInt(s->ctx, 0);
    }
    p = js_bigint_new(s->ctx, (len - 1) / (JS_LIMB_BITS / 8) + 1);
    if (!p)
        goto fail;
    obj = JS_MKPTR(JS_TAG_BIG_INT, p);
    for (i = 0; i < len / (JS_LIMB_BITS / 8); i++) {
#if JS_LIMB_BITS == 32
        if (bc_get_u32(s, &v))
            goto fail;
#else
        if (bc_get_u64(s, &v))
            goto fail;
#endif
        p->tab[i] = v;
    }
    n = len % (JS_LIMB_BITS / 8);
    if (n != 0) {
        int shift;
        v = 0;
        for (i = 0; i < n; i++) {
            if (bc_get_u8(s, &v8))
                goto fail;
            v |= (js_limb_t)v8 << (i * 8);
        }
        shift = JS_LIMB_BITS - n * 8;
        if (shift != 0) {
            v = (js_slimb_t)(v << shift) >> shift;
        }
        p->tab[p->len - 1] = v;
    }
    bc_read_trace(s, "}\n");
    return JS_CompactBigInt(s->ctx, p);
fail:
    JS_FreeValue(s->ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadObjectRec(BCReaderState* s);

static int BC_add_object_ref1(BCReaderState* s, JSObject* p)
{
    if (s->allow_reference) {
        if (js_resize_array(s->ctx, (void*)&s->objects,
                sizeof(s->objects[0]),
                &s->objects_size, s->objects_count + 1))
            return -1;
        if (p)
            JS_DupValue(s->ctx, JS_MKPTR(JS_TAG_OBJECT, p));
        s->objects[s->objects_count++] = p;
    }
    return 0;
}

static int BC_add_object_ref(BCReaderState* s, JSValueConst obj)
{
    return BC_add_object_ref1(s, JS_VALUE_GET_OBJ(obj));
}

static JSValue JS_ReadFunctionTag(BCReaderState* s)
{
    JSContext* ctx = s->ctx;
    JSFunctionBytecode bc, *b;
    JSValue obj = JS_UNDEFINED;
    uint16_t v16;
    uint8_t v8;
    int idx, i, local_count;
    int cpool_offset, byte_code_offset;
    int closure_var_offset, vardefs_offset;
    uint64_t function_size;

    memset(&bc, 0, sizeof(bc));

    if (bc_get_u16(s, &v16))
        goto fail;
    idx = 0;
    bc.has_prototype = bc_get_flags(v16, &idx, 1);
    bc.has_simple_parameter_list = bc_get_flags(v16, &idx, 1);
    bc.is_derived_class_constructor = bc_get_flags(v16, &idx, 1);
    bc.need_home_object = bc_get_flags(v16, &idx, 1);
    bc.func_kind = bc_get_flags(v16, &idx, 2);
    bc.new_target_allowed = bc_get_flags(v16, &idx, 1);
    bc.super_call_allowed = bc_get_flags(v16, &idx, 1);
    bc.super_allowed = bc_get_flags(v16, &idx, 1);
    bc.arguments_allowed = bc_get_flags(v16, &idx, 1);
    bc.has_debug = bc_get_flags(v16, &idx, 1);
    bc.is_direct_or_indirect_eval = bc_get_flags(v16, &idx, 1);
    bc.read_only_bytecode = s->is_rom_data;
    if (bc_get_u8(s, &v8))
        goto fail;
    bc.js_mode = v8;
    if (bc_get_atom(s, &bc.func_name))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.arg_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.var_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.defined_arg_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.stack_size))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.var_ref_count))
        goto fail;
    if (bc_get_leb128_int(s, &bc.closure_var_count))
        goto fail;
    if (bc_get_leb128_int(s, &bc.cpool_count))
        goto fail;
    if (bc_get_leb128_int(s, &bc.byte_code_len))
        goto fail;
    if (bc_get_leb128_int(s, &local_count))
        goto fail;
    if (bc.closure_var_count < 0 || bc.cpool_count < 0 || bc.byte_code_len < 0) {
        JS_ThrowInternalError(ctx, "invalid function bytecode (negative count)");
        goto fail;
    }
    {
        uint64_t avail = (uint64_t)(s->buf_end - s->ptr);
        if ((uint64_t)local_count > avail || (uint64_t)bc.closure_var_count > avail || (uint64_t)bc.cpool_count > avail || (uint64_t)bc.byte_code_len > avail) {
            JS_ThrowInternalError(ctx,
                "invalid function bytecode (count exceeds buffer)");
            goto fail;
        }
    }
    if (local_count != (int)bc.arg_count + (int)bc.var_count) {
        JS_ThrowInternalError(ctx, "invalid function bytecode (local_count)");
        goto fail;
    }
    if ((int)bc.defined_arg_count > (int)bc.arg_count) {
        JS_ThrowInternalError(ctx,
            "invalid function bytecode (defined_arg_count)");
        goto fail;
    }

    if (bc.has_debug) {
        function_size = sizeof(*b);
    } else {
        function_size = offsetof(JSFunctionBytecode, debug);
    }
    cpool_offset = function_size;
    function_size += (uint64_t)bc.cpool_count * sizeof(*bc.cpool);
    vardefs_offset = function_size;
    function_size += (uint64_t)local_count * sizeof(*bc.vardefs);
    closure_var_offset = function_size;
    function_size += (uint64_t)bc.closure_var_count * sizeof(*bc.closure_var);
    byte_code_offset = function_size;
    if (!bc.read_only_bytecode) {
        function_size += bc.byte_code_len;
    }

    if (function_size > INT32_MAX) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }

    b = js_mallocz(ctx, function_size);
    if (!b)
        goto fail;

    memcpy(b, &bc, offsetof(JSFunctionBytecode, debug));
    if (local_count != 0) {
        b->vardefs = (void*)((uint8_t*)b + vardefs_offset);
    }
    if (b->closure_var_count != 0) {
        b->closure_var = (void*)((uint8_t*)b + closure_var_offset);
    }
    if (b->cpool_count != 0) {
        b->cpool = (void*)((uint8_t*)b + cpool_offset);
    }

    js_rc(b)->ref_count = 1;
    add_gc_object(ctx->rt, &b->header, JS_GC_OBJ_TYPE_FUNCTION_BYTECODE);

    obj = JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b);

#ifdef DUMP_READ_OBJECT
    bc_read_trace(s, "name: ");
    print_atom(s->ctx, b->func_name);
    printf("\n");
#endif
    bc_read_trace(s, "args=%d vars=%d defargs=%d closures=%d cpool=%d\n",
        b->arg_count, b->var_count, b->defined_arg_count,
        b->closure_var_count, b->cpool_count);
    bc_read_trace(s, "stack=%d bclen=%d locals=%d\n",
        b->stack_size, b->byte_code_len, local_count);

    if (local_count != 0) {
        bc_read_trace(s, "vars {\n");
        for (i = 0; i < local_count; i++) {
            JSBytecodeVarDef* vd = &b->vardefs[i];
            if (bc_get_atom(s, &vd->var_name))
                goto fail;
            if (bc_get_leb128_int(s, &vd->scope_next))
                goto fail;
            vd->scope_next--;
            if (bc_get_leb128_u16(s, &vd->var_ref_idx))
                goto fail;
            if (bc_get_u8(s, &v8))
                goto fail;
            idx = 0;
            vd->var_kind = bc_get_flags(v8, &idx, 4);
            vd->is_const = bc_get_flags(v8, &idx, 1);
            vd->is_lexical = bc_get_flags(v8, &idx, 1);
            vd->is_captured = bc_get_flags(v8, &idx, 1);
            vd->has_scope = bc_get_flags(v8, &idx, 1);
            if (vd->is_captured && vd->var_ref_idx >= bc.var_ref_count) {
                JS_ThrowInternalError(ctx,
                    "invalid function bytecode (var_ref_idx)");
                goto fail;
            }
#ifdef DUMP_READ_OBJECT
            bc_read_trace(s, "name: ");
            print_atom(s->ctx, vd->var_name);
            printf("\n");
#endif
        }
        bc_read_trace(s, "}\n");
    }
    if (b->closure_var_count != 0) {
        bc_read_trace(s, "closure vars {\n");
        for (i = 0; i < b->closure_var_count; i++) {
            JSClosureVar* cv = &b->closure_var[i];
            int var_idx;
            if (bc_get_atom(s, &cv->var_name))
                goto fail;
            if (bc_get_leb128_int(s, &var_idx))
                goto fail;
            cv->var_idx = var_idx;
            if (bc_get_u16(s, &v16))
                goto fail;
            idx = 0;
            cv->closure_type = bc_get_flags(v16, &idx, 3);
            cv->is_const = bc_get_flags(v16, &idx, 1);
            cv->is_lexical = bc_get_flags(v16, &idx, 1);
            cv->var_kind = bc_get_flags(v16, &idx, 4);
#ifdef DUMP_READ_OBJECT
            bc_read_trace(s, "name: ");
            print_atom(s->ctx, cv->var_name);
            printf("\n");
#endif
        }
        bc_read_trace(s, "}\n");
    }
    {
        bc_read_trace(s, "bytecode {\n");
        if (JS_ReadFunctionBytecode(s, b, byte_code_offset, b->byte_code_len))
            goto fail;
        bc_read_trace(s, "}\n");
    }
    if (b->has_debug) {
        bc_read_trace(s, "debug {\n");
        if (bc_get_atom(s, &b->debug.filename))
            goto fail;
#ifdef DUMP_READ_OBJECT
        bc_read_trace(s, "filename: ");
        print_atom(s->ctx, b->debug.filename);
        printf("\n");
#endif
        if (bc_get_leb128_int(s, &b->debug.pc2line_len))
            goto fail;
        if (b->debug.pc2line_len < 0 || (uint64_t)b->debug.pc2line_len > (uint64_t)(s->buf_end - s->ptr)) {
            JS_ThrowInternalError(ctx, "invalid debug section length");
            goto fail;
        }
        if (b->debug.pc2line_len) {
            b->debug.pc2line_buf = js_mallocz(ctx, b->debug.pc2line_len);
            if (!b->debug.pc2line_buf)
                goto fail;
            if (bc_get_buf(s, b->debug.pc2line_buf, b->debug.pc2line_len))
                goto fail;
        }
        if (bc_get_leb128_int(s, &b->debug.source_len))
            goto fail;
        if (b->debug.source_len < 0 || (uint64_t)b->debug.source_len > (uint64_t)(s->buf_end - s->ptr)) {
            JS_ThrowInternalError(ctx, "invalid debug section length");
            goto fail;
        }
        if (b->debug.source_len) {
            bc_read_trace(s, "source: %d bytes\n", b->source_len);
            b->debug.source = js_mallocz(ctx, b->debug.source_len);
            if (!b->debug.source)
                goto fail;
            if (bc_get_buf(s, (uint8_t*)b->debug.source, b->debug.source_len))
                goto fail;
        }
        bc_read_trace(s, "}\n");
    }
    if (b->cpool_count != 0) {
        bc_read_trace(s, "cpool {\n");
        for (i = 0; i < b->cpool_count; i++) {
            JSValue val;
            val = JS_ReadObjectRec(s);
            if (JS_IsException(val))
                goto fail;
            b->cpool[i] = val;
        }
        bc_read_trace(s, "}\n");
    }
    if (JS_ValidateFunctionBytecode(s, b, b->byte_code_len))
        goto fail;
    b->realm = JS_DupContext(ctx);
    return obj;
fail:
    if (JS_IsUndefined(obj))
        JS_FreeAtom(ctx, bc.func_name);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadModule(BCReaderState* s)
{
    JSContext* ctx = s->ctx;
    JSValue obj = JS_UNDEFINED;
    JSModuleDef* m = NULL;
    JSAtom module_name;
    int i;
    uint8_t v8;

    if (bc_get_atom(s, &module_name))
        goto fail;
#ifdef DUMP_READ_OBJECT
    bc_read_trace(s, "name: ");
    print_atom(s->ctx, module_name);
    printf("\n");
#endif
    m = js_new_module_def(ctx, module_name);
    if (!m)
        goto fail;
    obj = JS_NewModuleValue(ctx, m);
    if (bc_get_leb128_int(s, &m->req_module_entries_count))
        goto fail;
    if (m->req_module_entries_count < 0 || (uint64_t)m->req_module_entries_count > (uint64_t)(s->buf_end - s->ptr))
        goto fail;
    if (m->req_module_entries_count != 0) {
        m->req_module_entries_size = m->req_module_entries_count;
        m->req_module_entries = js_mallocz(ctx, sizeof(m->req_module_entries[0]) * m->req_module_entries_size);
        if (!m->req_module_entries)
            goto fail;
        for (i = 0; i < m->req_module_entries_count; i++) {
            JSReqModuleEntry* rme = &m->req_module_entries[i];
            JSValue val;
            if (bc_get_atom(s, &rme->module_name))
                goto fail;
            val = JS_ReadObjectRec(s);
            if (JS_IsException(val))
                goto fail;
            rme->attributes = val;
        }
    }

    if (bc_get_leb128_int(s, &m->export_entries_count))
        goto fail;
    if (m->export_entries_count < 0 || (uint64_t)m->export_entries_count > (uint64_t)(s->buf_end - s->ptr))
        goto fail;
    if (m->export_entries_count != 0) {
        m->export_entries_size = m->export_entries_count;
        m->export_entries = js_mallocz(ctx, sizeof(m->export_entries[0]) * m->export_entries_size);
        if (!m->export_entries)
            goto fail;
        for (i = 0; i < m->export_entries_count; i++) {
            JSExportEntry* me = &m->export_entries[i];
            if (bc_get_u8(s, &v8))
                goto fail;
            me->export_type = v8;
            if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
                if (bc_get_leb128_int(s, &me->u.local.var_idx))
                    goto fail;
            } else {
                if (bc_get_leb128_int(s, &me->u.req_module_idx))
                    goto fail;
                if ((uint32_t)me->u.req_module_idx >= (uint32_t)m->req_module_entries_count) {
                    JS_ThrowInternalError(ctx,
                        "invalid module bytecode (export req_module_idx)");
                    goto fail;
                }
                if (bc_get_atom(s, &me->local_name))
                    goto fail;
            }
            if (bc_get_atom(s, &me->export_name))
                goto fail;
        }
    }

    if (bc_get_leb128_int(s, &m->star_export_entries_count))
        goto fail;
    if (m->star_export_entries_count < 0 || (uint64_t)m->star_export_entries_count > (uint64_t)(s->buf_end - s->ptr))
        goto fail;
    if (m->star_export_entries_count != 0) {
        m->star_export_entries_size = m->star_export_entries_count;
        m->star_export_entries = js_mallocz(ctx, sizeof(m->star_export_entries[0]) * m->star_export_entries_size);
        if (!m->star_export_entries)
            goto fail;
        for (i = 0; i < m->star_export_entries_count; i++) {
            JSStarExportEntry* se = &m->star_export_entries[i];
            if (bc_get_leb128_int(s, &se->req_module_idx))
                goto fail;
            if ((uint32_t)se->req_module_idx >= (uint32_t)m->req_module_entries_count) {
                JS_ThrowInternalError(ctx,
                    "invalid module bytecode (star req_module_idx)");
                goto fail;
            }
        }
    }

    if (bc_get_leb128_int(s, &m->import_entries_count))
        goto fail;
    if (m->import_entries_count < 0 || (uint64_t)m->import_entries_count > (uint64_t)(s->buf_end - s->ptr))
        goto fail;
    if (m->import_entries_count != 0) {
        m->import_entries_size = m->import_entries_count;
        m->import_entries = js_mallocz(ctx, sizeof(m->import_entries[0]) * m->import_entries_size);
        if (!m->import_entries)
            goto fail;
        for (i = 0; i < m->import_entries_count; i++) {
            JSImportEntry* mi = &m->import_entries[i];
            uint8_t b8;
            if (bc_get_leb128_int(s, &mi->var_idx))
                goto fail;
            if (bc_get_u8(s, &b8))
                goto fail;
            mi->is_star = (b8 != 0);
            if (bc_get_atom(s, &mi->import_name))
                goto fail;
            if (bc_get_leb128_int(s, &mi->req_module_idx))
                goto fail;
            if ((uint32_t)mi->req_module_idx >= (uint32_t)m->req_module_entries_count) {
                JS_ThrowInternalError(ctx,
                    "invalid module bytecode (import req_module_idx)");
                goto fail;
            }
        }
    }

    if (bc_get_u8(s, &v8))
        goto fail;
    m->has_tla = (v8 != 0);

    m->func_obj = JS_ReadObjectRec(s);
    if (JS_IsException(m->func_obj))
        goto fail;
    if (JS_VALUE_GET_TAG(m->func_obj) != JS_TAG_FUNCTION_BYTECODE) {
        JS_ThrowInternalError(ctx, "invalid module bytecode (function)");
        goto fail;
    }
    {
        JSFunctionBytecode* fb = JS_VALUE_GET_PTR(m->func_obj);
        if (fb->func_kind != JS_FUNC_ASYNC) {
            JS_ThrowInternalError(ctx,
                "invalid module bytecode (function kind)");
            goto fail;
        }
    }
    if (JS_VALUE_GET_TAG(m->func_obj) == JS_TAG_FUNCTION_BYTECODE) {
        JSFunctionBytecode* fb = JS_VALUE_GET_PTR(m->func_obj);
        for (i = 0; i < m->export_entries_count; i++) {
            JSExportEntry* me = &m->export_entries[i];
            if (me->export_type == JS_EXPORT_TYPE_LOCAL && (uint32_t)me->u.local.var_idx >= (uint32_t)fb->closure_var_count) {
                JS_ThrowInternalError(ctx,
                    "invalid module bytecode (export var_idx)");
                goto fail;
            }
        }
        for (i = 0; i < m->import_entries_count; i++) {
            JSImportEntry* mi = &m->import_entries[i];
            if (!mi->is_star && (uint32_t)mi->var_idx >= (uint32_t)fb->closure_var_count) {
                JS_ThrowInternalError(ctx,
                    "invalid module bytecode (import var_idx)");
                goto fail;
            }
        }
    }
    return obj;
fail:
    if (m)
        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_MODULE, m));
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadObjectTag(BCReaderState* s)
{
    JSContext* ctx = s->ctx;
    JSValue obj;
    uint32_t prop_count, i;
    JSAtom atom;
    JSValue val;
    int ret;

    obj = JS_NewObject(ctx);
    if (BC_add_object_ref(s, obj))
        goto fail;
    if (bc_get_leb128(s, &prop_count))
        goto fail;
    for (i = 0; i < prop_count; i++) {
        if (bc_get_atom(s, &atom))
            goto fail;
        if (unlikely(atom == JS_ATOM_NULL)) {
            JS_ThrowSyntaxError(ctx, "invalid property atom");
            goto fail;
        }
#ifdef DUMP_READ_OBJECT
        bc_read_trace(s, "propname: ");
        print_atom(s->ctx, atom);
        printf("\n");
#endif
        val = JS_ReadObjectRec(s);
        if (JS_IsException(val)) {
            JS_FreeAtom(ctx, atom);
            goto fail;
        }
        ret = JS_DefinePropertyValue(ctx, obj, atom, val, JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
        if (ret < 0)
            goto fail;
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadArray(BCReaderState* s, int tag)
{
    JSContext* ctx = s->ctx;
    JSValue obj;
    uint32_t len, i;
    JSValue val;
    int ret, prop_flags;
    BOOL is_template;

    obj = JS_NewArray(ctx);
    if (BC_add_object_ref(s, obj))
        goto fail;
    is_template = (tag == BC_TAG_TEMPLATE_OBJECT);
    if (bc_get_leb128(s, &len))
        goto fail;
    for (i = 0; i < len; i++) {
        val = JS_ReadObjectRec(s);
        if (JS_IsException(val))
            goto fail;
        if (is_template)
            prop_flags = JS_PROP_ENUMERABLE;
        else
            prop_flags = JS_PROP_C_W_E;
        ret = JS_DefinePropertyValueUint32(ctx, obj, i, val,
            prop_flags);
        if (ret < 0)
            goto fail;
    }
    if (is_template) {
        val = JS_ReadObjectRec(s);
        if (JS_IsException(val))
            goto fail;
        if (!JS_IsUndefined(val)) {
            ret = JS_DefinePropertyValue(ctx, obj, JS_ATOM_raw, val, 0);
            if (ret < 0)
                goto fail;
        }
        JS_PreventExtensions(ctx, obj);
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadTypedArray(BCReaderState* s)
{
    JSContext* ctx = s->ctx;
    JSValue obj = JS_UNDEFINED, array_buffer = JS_UNDEFINED;
    uint8_t array_tag;
    JSValueConst args[3];
    uint32_t offset, len, idx;

    if (bc_get_u8(s, &array_tag))
        return JS_EXCEPTION;
    if (array_tag >= JS_TYPED_ARRAY_COUNT)
        return JS_ThrowTypeError(ctx, "invalid typed array");
    if (bc_get_leb128(s, &len))
        return JS_EXCEPTION;
    if (bc_get_leb128(s, &offset))
        return JS_EXCEPTION;
    idx = s->objects_count;
    if (BC_add_object_ref1(s, NULL))
        goto fail;
    array_buffer = JS_ReadObjectRec(s);
    if (JS_IsException(array_buffer))
        return JS_EXCEPTION;
    if (!js_get_array_buffer(ctx, array_buffer)) {
        JS_FreeValue(ctx, array_buffer);
        return JS_EXCEPTION;
    }
    args[0] = array_buffer;
    args[1] = JS_NewInt64(ctx, offset);
    args[2] = JS_NewInt64(ctx, len);
    obj = js_typed_array_constructor(ctx, JS_UNDEFINED,
        3, args,
        JS_CLASS_UINT8C_ARRAY + array_tag);
    if (JS_IsException(obj))
        goto fail;
    if (s->allow_reference) {
        s->objects[idx] = JS_VALUE_GET_OBJ(JS_DupValue(ctx, obj));
    }
    JS_FreeValue(ctx, array_buffer);
    return obj;
fail:
    JS_FreeValue(ctx, array_buffer);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadArrayBuffer(BCReaderState* s)
{
    JSContext* ctx = s->ctx;
    uint32_t byte_length, max_byte_length;
    uint64_t max_byte_length_u64, *pmax_byte_length = NULL;
    JSValue obj;

    if (bc_get_leb128(s, &byte_length))
        return JS_EXCEPTION;
    if (bc_get_leb128(s, &max_byte_length))
        return JS_EXCEPTION;
    if (max_byte_length < byte_length)
        return JS_ThrowTypeError(ctx, "invalid array buffer");
    if (max_byte_length != UINT32_MAX) {
        max_byte_length_u64 = max_byte_length;
        pmax_byte_length = &max_byte_length_u64;
    }
    if (unlikely(s->buf_end - s->ptr < byte_length)) {
        bc_read_error_end(s);
        return JS_EXCEPTION;
    }
    obj = js_array_buffer_constructor3(ctx, JS_UNDEFINED,
        byte_length, pmax_byte_length,
        JS_CLASS_ARRAY_BUFFER,
        DYN_UNCONST(s->ptr),
        js_array_buffer_free, NULL,
        TRUE);
    if (JS_IsException(obj))
        goto fail;
    if (BC_add_object_ref(s, obj))
        goto fail;
    s->ptr += byte_length;
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadSharedArrayBuffer(BCReaderState* s)
{
    JSContext* ctx = s->ctx;
    uint32_t byte_length, max_byte_length;
    uint64_t max_byte_length_u64, *pmax_byte_length = NULL;
    uint8_t* data_ptr;
    JSValue obj;
    uint64_t u64;

    if (bc_get_leb128(s, &byte_length))
        return JS_EXCEPTION;
    if (bc_get_leb128(s, &max_byte_length))
        return JS_EXCEPTION;
    if (max_byte_length < byte_length)
        return JS_ThrowTypeError(ctx, "invalid array buffer");
    if (max_byte_length != UINT32_MAX) {
        max_byte_length_u64 = max_byte_length;
        pmax_byte_length = &max_byte_length_u64;
    }
    if (bc_get_u64(s, &u64))
        return JS_EXCEPTION;
    data_ptr = (uint8_t*)(uintptr_t)u64;
    if (!s->ctx->rt->sab_funcs.sab_valid_ptr || !s->ctx->rt->sab_funcs.sab_valid_ptr(s->ctx->rt->sab_funcs.sab_opaque, data_ptr)) {
        return JS_ThrowTypeError(s->ctx, "invalid shared array buffer pointer");
    }
    obj = js_array_buffer_constructor3(ctx, JS_UNDEFINED,
        byte_length, pmax_byte_length,
        JS_CLASS_SHARED_ARRAY_BUFFER,
        data_ptr,
        NULL, NULL, FALSE);
    if (JS_IsException(obj))
        goto fail;
    if (BC_add_object_ref(s, obj))
        goto fail;
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadDate(BCReaderState* s)
{
    JSContext* ctx = s->ctx;
    JSValue val, obj = JS_UNDEFINED;

    val = JS_ReadObjectRec(s);
    if (JS_IsException(val))
        goto fail;
    if (!JS_IsNumber(val)) {
        JS_ThrowTypeError(ctx, "Number tag expected for date");
        goto fail;
    }
    obj = JS_NewObjectProtoClass(ctx, ctx->class_proto[JS_CLASS_DATE],
        JS_CLASS_DATE);
    if (JS_IsException(obj))
        goto fail;
    if (BC_add_object_ref(s, obj))
        goto fail;
    JS_SetObjectData(ctx, obj, val);
    return obj;
fail:
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadObjectValue(BCReaderState* s)
{
    JSContext* ctx = s->ctx;
    JSValue val, obj = JS_UNDEFINED;

    val = JS_ReadObjectRec(s);
    if (JS_IsException(val))
        goto fail;
    obj = JS_ToObject(ctx, val);
    if (JS_IsException(obj))
        goto fail;
    if (BC_add_object_ref(s, obj))
        goto fail;
    JS_FreeValue(ctx, val);
    return obj;
fail:
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadObjectRec(BCReaderState* s)
{
    JSContext* ctx = s->ctx;
    uint8_t tag;
    JSValue obj = JS_UNDEFINED;

    if (js_check_stack_overflow(ctx->rt, 0))
        return JS_ThrowStackOverflow(ctx);

    if (bc_get_u8(s, &tag))
        return JS_EXCEPTION;

    bc_read_trace(s, "%s {\n", bc_tag_str[tag]);

    switch (tag) {
    case BC_TAG_NULL:
        obj = JS_NULL;
        break;
    case BC_TAG_UNDEFINED:
        obj = JS_UNDEFINED;
        break;
    case BC_TAG_BOOL_FALSE:
    case BC_TAG_BOOL_TRUE:
        obj = JS_NewBool(ctx, tag - BC_TAG_BOOL_FALSE);
        break;
    case BC_TAG_INT32: {
        int32_t val;
        if (bc_get_sleb128(s, &val))
            return JS_EXCEPTION;
        bc_read_trace(s, "%d\n", val);
        obj = JS_NewInt32(ctx, val);
    } break;
    case BC_TAG_FLOAT64: {
        JSFloat64Union u;
        if (bc_get_u64(s, &u.u64))
            return JS_EXCEPTION;
        bc_read_trace(s, "%g\n", u.d);
        obj = __JS_NewFloat64(ctx, u.d);
    } break;
    case BC_TAG_STRING: {
        JSString* p;
        p = JS_ReadString(s);
        if (!p)
            return JS_EXCEPTION;
        obj = JS_MKPTR(JS_TAG_STRING, p);
    } break;
    case BC_TAG_FUNCTION_BYTECODE:
        if (!s->allow_bytecode)
            goto invalid_tag;
        obj = JS_ReadFunctionTag(s);
        break;
    case BC_TAG_MODULE:
        if (!s->allow_bytecode)
            goto invalid_tag;
        obj = JS_ReadModule(s);
        break;
    case BC_TAG_OBJECT:
        obj = JS_ReadObjectTag(s);
        break;
    case BC_TAG_ARRAY:
    case BC_TAG_TEMPLATE_OBJECT:
        obj = JS_ReadArray(s, tag);
        break;
    case BC_TAG_TYPED_ARRAY:
        obj = JS_ReadTypedArray(s);
        break;
    case BC_TAG_ARRAY_BUFFER:
        obj = JS_ReadArrayBuffer(s);
        break;
    case BC_TAG_SHARED_ARRAY_BUFFER:
        if (!s->allow_sab || !ctx->rt->sab_funcs.sab_dup)
            goto invalid_tag;
        obj = JS_ReadSharedArrayBuffer(s);
        break;
    case BC_TAG_DATE:
        obj = JS_ReadDate(s);
        break;
    case BC_TAG_OBJECT_VALUE:
        obj = JS_ReadObjectValue(s);
        break;
    case BC_TAG_BIG_INT:
        obj = JS_ReadBigInt(s);
        break;
    case BC_TAG_OBJECT_REFERENCE: {
        uint32_t val;
        if (!s->allow_reference)
            return JS_ThrowSyntaxError(ctx, "object references are not allowed");
        if (bc_get_leb128(s, &val))
            return JS_EXCEPTION;
        bc_read_trace(s, "%u\n", val);
        if (val >= s->objects_count || !s->objects[val]) {
            return JS_ThrowSyntaxError(ctx, "invalid object reference (%u >= %u)",
                val, s->objects_count);
        }
        obj = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, s->objects[val]));
    } break;
    default:
    invalid_tag:
        return JS_ThrowSyntaxError(ctx, "invalid tag (tag=%d pos=%u)",
            tag, (unsigned int)(s->ptr - s->buf_start));
    }
    bc_read_trace(s, "}\n");
    return obj;
}

static int JS_ReadObjectAtoms(BCReaderState* s)
{
    uint8_t v8;
    JSString* p;
    int i;
    JSAtom atom;

    if (bc_get_u8(s, &v8))
        return -1;
    if (v8 != BC_VERSION) {
        JS_ThrowSyntaxError(s->ctx, "invalid version (%d expected=%d)",
            v8, BC_VERSION);
        return -1;
    }
    if (bc_get_leb128(s, &s->idx_to_atom_count))
        return -1;
    if ((uint64_t)s->idx_to_atom_count > (uint64_t)(s->buf_end - s->ptr)) {
        JS_ThrowSyntaxError(s->ctx, "invalid atom table size (%u)",
            s->idx_to_atom_count);
        return s->error_state = -1;
    }

    bc_read_trace(s, "%d atom indexes {\n", s->idx_to_atom_count);

    if (s->idx_to_atom_count != 0) {
        s->idx_to_atom = js_mallocz(s->ctx, s->idx_to_atom_count * sizeof(s->idx_to_atom[0]));
        if (!s->idx_to_atom)
            return s->error_state = -1;
    }
    for (i = 0; i < s->idx_to_atom_count; i++) {
        p = JS_ReadString(s);
        if (!p)
            return -1;
        atom = JS_NewAtomStr(s->ctx, p);
        if (atom == JS_ATOM_NULL)
            return s->error_state = -1;
        s->idx_to_atom[i] = atom;
        if (s->is_rom_data && (atom != (i + s->first_atom)))
            s->is_rom_data = FALSE;
    }
    bc_read_trace(s, "}\n");
    return 0;
}

static void bc_reader_free(BCReaderState* s)
{
    int i;
    if (s->idx_to_atom) {
        for (i = 0; i < s->idx_to_atom_count; i++) {
            JS_FreeAtom(s->ctx, s->idx_to_atom[i]);
        }
        js_free(s->ctx, s->idx_to_atom);
    }
    if (s->objects) {
        for (i = 0; i < s->objects_count; i++) {
            if (s->objects[i])
                JS_FreeValue(s->ctx, JS_MKPTR(JS_TAG_OBJECT, s->objects[i]));
        }
        js_free(s->ctx, s->objects);
    }
}
