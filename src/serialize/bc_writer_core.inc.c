typedef struct {
    JSObject* obj;
    uint32_t hash_next;
} JSObjectListEntry;

typedef struct {
    JSObjectListEntry* object_tab;
    int object_count;
    int object_size;
    uint32_t* hash_table;
    uint32_t hash_size;
} JSObjectList;

static void js_object_list_init(JSObjectList* s)
{
    memset(s, 0, sizeof(*s));
}

static uint32_t js_object_list_get_hash(JSObject* p, uint32_t hash_size)
{
    return ((uintptr_t)p * 3163) & (hash_size - 1);
}

static int js_object_list_resize_hash(JSContext* ctx, JSObjectList* s,
    uint32_t new_hash_size)
{
    JSObjectListEntry* e;
    uint32_t i, h, *new_hash_table;

    new_hash_table = js_malloc(ctx, sizeof(new_hash_table[0]) * new_hash_size);
    if (!new_hash_table)
        return -1;
    js_free(ctx, s->hash_table);
    s->hash_table = new_hash_table;
    s->hash_size = new_hash_size;

    for (i = 0; i < s->hash_size; i++) {
        s->hash_table[i] = -1;
    }
    for (i = 0; i < s->object_count; i++) {
        e = &s->object_tab[i];
        h = js_object_list_get_hash(e->obj, s->hash_size);
        e->hash_next = s->hash_table[h];
        s->hash_table[h] = i;
    }
    return 0;
}

static int js_object_list_add(JSContext* ctx, JSObjectList* s, JSObject* obj)
{
    JSObjectListEntry* e;
    uint32_t h, new_hash_size;

    if (js_resize_array(ctx, (void*)&s->object_tab,
            sizeof(s->object_tab[0]),
            &s->object_size, s->object_count + 1))
        return -1;
    if (unlikely((s->object_count + 1) >= s->hash_size)) {
        new_hash_size = max_uint32(s->hash_size, 4);
        while (new_hash_size <= s->object_count)
            new_hash_size *= 2;
        if (js_object_list_resize_hash(ctx, s, new_hash_size))
            return -1;
    }
    e = &s->object_tab[s->object_count++];
    h = js_object_list_get_hash(obj, s->hash_size);
    e->obj = obj;
    e->hash_next = s->hash_table[h];
    s->hash_table[h] = s->object_count - 1;
    return 0;
}

static int js_object_list_find(JSContext* ctx, JSObjectList* s, JSObject* obj)
{
    JSObjectListEntry* e;
    uint32_t h, p;

    if (s->object_count == 0)
        return -1;
    h = js_object_list_get_hash(obj, s->hash_size);
    p = s->hash_table[h];
    while (p != -1) {
        e = &s->object_tab[p];
        if (e->obj == obj)
            return p;
        p = e->hash_next;
    }
    return -1;
}

static void js_object_list_end(JSContext* ctx, JSObjectList* s)
{
    js_free(ctx, s->object_tab);
    js_free(ctx, s->hash_table);
}

typedef enum BCTagEnum {
    BC_TAG_NULL = 1,
    BC_TAG_UNDEFINED,
    BC_TAG_BOOL_FALSE,
    BC_TAG_BOOL_TRUE,
    BC_TAG_INT32,
    BC_TAG_FLOAT64,
    BC_TAG_STRING,
    BC_TAG_OBJECT,
    BC_TAG_ARRAY,
    BC_TAG_BIG_INT,
    BC_TAG_TEMPLATE_OBJECT,
    BC_TAG_FUNCTION_BYTECODE,
    BC_TAG_MODULE,
    BC_TAG_TYPED_ARRAY,
    BC_TAG_ARRAY_BUFFER,
    BC_TAG_SHARED_ARRAY_BUFFER,
    BC_TAG_DATE,
    BC_TAG_OBJECT_VALUE,
    BC_TAG_OBJECT_REFERENCE,
} BCTagEnum;

#define BC_VERSION 13

typedef struct BCWriterState {
    JSContext* ctx;
    DynBuf dbuf;
    BOOL allow_bytecode : 8;
    BOOL allow_sab : 8;
    BOOL allow_reference : 8;
    uint32_t first_atom;
    uint32_t* atom_to_idx;
    int atom_to_idx_size;
    JSAtom* idx_to_atom;
    int idx_to_atom_count;
    int idx_to_atom_size;
    uint8_t** sab_tab;
    int sab_tab_len;
    int sab_tab_size;
    JSObjectList object_list;
} BCWriterState;

#ifdef DUMP_READ_OBJECT
static const char* const bc_tag_str[] = {
    "invalid",
    "null",
    "undefined",
    "false",
    "true",
    "int32",
    "float64",
    "string",
    "object",
    "array",
    "bigint",
    "template",
    "function",
    "module",
    "TypedArray",
    "ArrayBuffer",
    "SharedArrayBuffer",
    "Date",
    "ObjectValue",
    "ObjectReference",
};
#endif

inline BOOL is_be(void)
{
    union {
        uint16_t a;
        uint8_t b;
    } u = { 0x100 };
    return u.b;
}

static void bc_put_u8(BCWriterState* s, uint8_t v)
{
    dbuf_putc(&s->dbuf, v);
}

static void bc_put_u16(BCWriterState* s, uint16_t v)
{
    if (is_be())
        v = bswap16(v);
    dbuf_put_u16(&s->dbuf, v);
}

static __maybe_unused void bc_put_u32(BCWriterState* s, uint32_t v)
{
    if (is_be())
        v = bswap32(v);
    dbuf_put_u32(&s->dbuf, v);
}

static void bc_put_u64(BCWriterState* s, uint64_t v)
{
    if (is_be())
        v = bswap64(v);
    dbuf_put(&s->dbuf, (uint8_t*)&v, sizeof(v));
}

static void bc_put_leb128(BCWriterState* s, uint32_t v)
{
    dbuf_put_leb128(&s->dbuf, v);
}

static void bc_put_sleb128(BCWriterState* s, int32_t v)
{
    dbuf_put_sleb128(&s->dbuf, v);
}

static void bc_set_flags(uint32_t* pflags, int* pidx, uint32_t val, int n)
{
    *pflags = *pflags | (val << *pidx);
    *pidx += n;
}

static int bc_atom_to_idx(BCWriterState* s, uint32_t* pres, JSAtom atom)
{
    uint32_t v;

    if (atom < s->first_atom || __JS_AtomIsTaggedInt(atom)) {
        *pres = atom;
        return 0;
    }
    atom -= s->first_atom;
    if (atom < s->atom_to_idx_size && s->atom_to_idx[atom] != 0) {
        *pres = s->atom_to_idx[atom];
        return 0;
    }
    if (atom >= s->atom_to_idx_size) {
        int old_size, i;
        old_size = s->atom_to_idx_size;
        if (js_resize_array(s->ctx, (void**)&s->atom_to_idx,
                sizeof(s->atom_to_idx[0]), &s->atom_to_idx_size,
                atom + 1))
            return -1;
        for (i = old_size; i < s->atom_to_idx_size; i++)
            s->atom_to_idx[i] = 0;
    }
    if (js_resize_array(s->ctx, (void**)&s->idx_to_atom,
            sizeof(s->idx_to_atom[0]),
            &s->idx_to_atom_size, s->idx_to_atom_count + 1))
        goto fail;

    v = s->idx_to_atom_count++;
    s->idx_to_atom[v] = atom + s->first_atom;
    v += s->first_atom;
    s->atom_to_idx[atom] = v;
    *pres = v;
    return 0;
fail:
    *pres = 0;
    return -1;
}

static int bc_put_atom(BCWriterState* s, JSAtom atom)
{
    uint32_t v;

    if (__JS_AtomIsTaggedInt(atom)) {
        v = (__JS_AtomToUInt32(atom) << 1) | 1;
    } else {
        if (bc_atom_to_idx(s, &v, atom))
            return -1;
        v <<= 1;
    }
    bc_put_leb128(s, v);
    return 0;
}

static void bc_byte_swap(uint8_t* bc_buf, int bc_len)
{
    int pos, len, op, fmt;

    pos = 0;
    while (pos < bc_len) {
        op = bc_buf[pos];
        if (op == OP_ext) {
            int op2;
            if (pos + 2 > bc_len)
                break;
            op2 = bc_buf[pos + 1];
            if (op2 >= OP2_COUNT)
                break;
            if (opcode_info2[op2].fmt == OP_FMT_loc2 && pos + opcode_info2[op2].size <= bc_len) {
                put_u16(bc_buf + pos + 2, bswap16(get_u16(bc_buf + pos + 2)));
                put_u16(bc_buf + pos + 4, bswap16(get_u16(bc_buf + pos + 4)));
            } else if (opcode_info2[op2].fmt == OP_FMT_label_const8 && pos + opcode_info2[op2].size <= bc_len) {
                put_u32(bc_buf + pos + 2, bswap32(get_u32(bc_buf + pos + 2)));
            } else if (opcode_info2[op2].fmt == OP_FMT_label_var_ref_atom && pos + opcode_info2[op2].size <= bc_len) {
                put_u32(bc_buf + pos + 2, bswap32(get_u32(bc_buf + pos + 2)));
                put_u16(bc_buf + pos + 6, bswap16(get_u16(bc_buf + pos + 6)));
                put_u32(bc_buf + pos + 8, bswap32(get_u32(bc_buf + pos + 8)));
            } else if ((opcode_info2[op2].fmt == OP_FMT_atom || opcode_info2[op2].fmt == OP_FMT_atom_loc || opcode_info2[op2].fmt == OP_FMT_atom_i8 || opcode_info2[op2].fmt == OP_FMT_atom_const) && pos + opcode_info2[op2].size <= bc_len) {
                put_u32(bc_buf + pos + 2, bswap32(get_u32(bc_buf + pos + 2)));
                if (opcode_info2[op2].fmt == OP_FMT_atom_loc) {
                    put_u16(bc_buf + pos + 7,
                        bswap16(get_u16(bc_buf + pos + 7)));
                } else if (opcode_info2[op2].fmt == OP_FMT_atom_const) {
                    put_u32(bc_buf + pos + 6,
                        bswap32(get_u32(bc_buf + pos + 6)));
                }
            }
            pos += opcode_info2[op2].size;
            continue;
        }
        len = short_opcode_info(op).size;
        fmt = short_opcode_info(op).fmt;
        switch (fmt) {
        case OP_FMT_u16:
        case OP_FMT_i16:
        case OP_FMT_label16:
        case OP_FMT_npop:
        case OP_FMT_loc:
        case OP_FMT_arg:
        case OP_FMT_var_ref:
            put_u16(bc_buf + pos + 1,
                bswap16(get_u16(bc_buf + pos + 1)));
            break;
        case OP_FMT_i32:
        case OP_FMT_u32:
        case OP_FMT_const:
        case OP_FMT_label:
        case OP_FMT_atom:
        case OP_FMT_atom_u8:
            put_u32(bc_buf + pos + 1,
                bswap32(get_u32(bc_buf + pos + 1)));
            break;
        case OP_FMT_atom_u16:
        case OP_FMT_label_u16:
            put_u32(bc_buf + pos + 1,
                bswap32(get_u32(bc_buf + pos + 1)));
            put_u16(bc_buf + pos + 1 + 4,
                bswap16(get_u16(bc_buf + pos + 1 + 4)));
            break;
        case OP_FMT_atom_label_u8:
        case OP_FMT_atom_label_u16:
            put_u32(bc_buf + pos + 1,
                bswap32(get_u32(bc_buf + pos + 1)));
            put_u32(bc_buf + pos + 1 + 4,
                bswap32(get_u32(bc_buf + pos + 1 + 4)));
            if (fmt == OP_FMT_atom_label_u16) {
                put_u16(bc_buf + pos + 1 + 4 + 4,
                    bswap16(get_u16(bc_buf + pos + 1 + 4 + 4)));
            }
            break;
        case OP_FMT_npop_u16:
            put_u16(bc_buf + pos + 1,
                bswap16(get_u16(bc_buf + pos + 1)));
            put_u16(bc_buf + pos + 1 + 2,
                bswap16(get_u16(bc_buf + pos + 1 + 2)));
            break;
        default:
            break;
        }
        pos += len;
    }
}

static int JS_WriteFunctionBytecode(BCWriterState* s,
    const uint8_t* bc_buf1, int bc_len)
{
    int pos, len, op;
    JSAtom atom;
    uint8_t* bc_buf;
    uint32_t val;

    bc_buf = js_malloc(s->ctx, bc_len);
    if (!bc_buf)
        return -1;
    memcpy(bc_buf, bc_buf1, bc_len);

    pos = 0;
    while (pos < bc_len) {
        op = bc_buf[pos];
        if (op == OP_ext) {
            if (opcode_info2[bc_buf[pos + 1]].fmt == OP_FMT_label_var_ref_atom) {
                atom = get_u32(bc_buf + pos + 8);
                if (bc_atom_to_idx(s, &val, atom))
                    goto fail;
                put_u32(bc_buf + pos + 8, val);
            } else if (opcode_info2[bc_buf[pos + 1]].fmt == OP_FMT_atom || opcode_info2[bc_buf[pos + 1]].fmt == OP_FMT_atom_loc || opcode_info2[bc_buf[pos + 1]].fmt == OP_FMT_atom_i8 || opcode_info2[bc_buf[pos + 1]].fmt == OP_FMT_atom_const) {
                atom = get_u32(bc_buf + pos + 2);
                if (bc_atom_to_idx(s, &val, atom))
                    goto fail;
                put_u32(bc_buf + pos + 2, val);
            }
            pos += opcode_info2[bc_buf[pos + 1]].size;
            continue;
        }
        len = short_opcode_info(op).size;
        switch (short_opcode_info(op).fmt) {
        case OP_FMT_atom:
        case OP_FMT_atom_u8:
        case OP_FMT_atom_u16:
        case OP_FMT_atom_label_u8:
        case OP_FMT_atom_label_u16:
            atom = get_u32(bc_buf + pos + 1);
            if (bc_atom_to_idx(s, &val, atom))
                goto fail;
            put_u32(bc_buf + pos + 1, val);
            break;
        default:
            break;
        }
        pos += len;
    }

    if (is_be())
        bc_byte_swap(bc_buf, bc_len);

    dbuf_put(&s->dbuf, bc_buf, bc_len);

    js_free(s->ctx, bc_buf);
    return 0;
fail:
    js_free(s->ctx, bc_buf);
    return -1;
}

static void JS_WriteString(BCWriterState* s, JSString* p)
{
    int i;
    bc_put_leb128(s, ((uint32_t)p->len << 1) | p->is_wide_char);
    if (p->is_wide_char) {
        const uint16_t* d = js_str_data16(p);
        for (i = 0; i < p->len; i++)
            bc_put_u16(s, d[i]);
    } else {
        dbuf_put(&s->dbuf, js_str_data8(p), p->len);
    }
}

static int JS_WriteBigInt(BCWriterState* s, JSValueConst obj)
{
    JSBigIntBuf buf;
    JSBigInt* p;
    uint32_t len, i;
    js_limb_t v, b;
    int shift;

    bc_put_u8(s, BC_TAG_BIG_INT);

    if (JS_VALUE_GET_TAG(obj) == JS_TAG_SHORT_BIG_INT)
        p = js_bigint_set_short(&buf, obj);
    else
        p = JS_VALUE_GET_PTR(obj);
    if (p->len == 1 && p->tab[0] == 0) {
        len = 0;
    } else {
        len = p->len * (JS_LIMB_BITS / 8);
        v = p->tab[p->len - 1];
        shift = JS_LIMB_BITS - 8;
        while (shift > 0) {
            b = (v >> shift) & 0xff;
            if (b != 0x00 && b != 0xff)
                break;
            if ((b & 1) != ((v >> (shift - 1)) & 1))
                break;
            shift -= 8;
            len--;
        }
    }
    bc_put_leb128(s, len);
    if (len > 0) {
        for (i = 0; i < (len / (JS_LIMB_BITS / 8)); i++) {
#if JS_LIMB_BITS == 32
            bc_put_u32(s, p->tab[i]);
#else
            bc_put_u64(s, p->tab[i]);
#endif
        }
        for (i = 0; i < len % (JS_LIMB_BITS / 8); i++) {
            bc_put_u8(s, (p->tab[p->len - 1] >> (i * 8)) & 0xff);
        }
    }
    return 0;
}

static int JS_WriteObjectRec(BCWriterState* s, JSValueConst obj);

static int JS_WriteFunctionTag(BCWriterState* s, JSValueConst obj)
{
    JSFunctionBytecode* b = JS_VALUE_GET_PTR(obj);
    uint32_t flags;
    int idx, i;

    bc_put_u8(s, BC_TAG_FUNCTION_BYTECODE);
    flags = idx = 0;
    bc_set_flags(&flags, &idx, b->has_prototype, 1);
    bc_set_flags(&flags, &idx, b->has_simple_parameter_list, 1);
    bc_set_flags(&flags, &idx, b->is_derived_class_constructor, 1);
    bc_set_flags(&flags, &idx, b->need_home_object, 1);
    bc_set_flags(&flags, &idx, b->func_kind, 2);
    bc_set_flags(&flags, &idx, b->new_target_allowed, 1);
    bc_set_flags(&flags, &idx, b->super_call_allowed, 1);
    bc_set_flags(&flags, &idx, b->super_allowed, 1);
    bc_set_flags(&flags, &idx, b->arguments_allowed, 1);
    bc_set_flags(&flags, &idx, b->has_debug, 1);
    bc_set_flags(&flags, &idx, b->is_direct_or_indirect_eval, 1);
    assert(idx <= 16);
    bc_put_u16(s, flags);
    bc_put_u8(s, b->js_mode);
    bc_put_atom(s, b->func_name);

    bc_put_leb128(s, b->arg_count);
    bc_put_leb128(s, b->var_count);
    bc_put_leb128(s, b->defined_arg_count);
    bc_put_leb128(s, b->stack_size);
    bc_put_leb128(s, b->var_ref_count);
    bc_put_leb128(s, b->closure_var_count);
    bc_put_leb128(s, b->cpool_count);
    bc_put_leb128(s, b->byte_code_len);
    if (b->vardefs) {
        bc_put_leb128(s, b->arg_count + b->var_count);
        for (i = 0; i < b->arg_count + b->var_count; i++) {
            JSBytecodeVarDef* vd = &b->vardefs[i];
            bc_put_atom(s, vd->var_name);
            bc_put_leb128(s, vd->scope_next + 1);
            bc_put_leb128(s, vd->var_ref_idx);
            flags = idx = 0;
            bc_set_flags(&flags, &idx, vd->var_kind, 4);
            bc_set_flags(&flags, &idx, vd->is_const, 1);
            bc_set_flags(&flags, &idx, vd->is_lexical, 1);
            bc_set_flags(&flags, &idx, vd->is_captured, 1);
            bc_set_flags(&flags, &idx, vd->has_scope, 1);
            assert(idx <= 8);
            bc_put_u8(s, flags);
        }
    } else {
        bc_put_leb128(s, 0);
    }

    for (i = 0; i < b->closure_var_count; i++) {
        JSClosureVar* cv = &b->closure_var[i];
        bc_put_atom(s, cv->var_name);
        bc_put_leb128(s, cv->var_idx);
        flags = idx = 0;
        bc_set_flags(&flags, &idx, cv->closure_type, 3);
        bc_set_flags(&flags, &idx, cv->is_const, 1);
        bc_set_flags(&flags, &idx, cv->is_lexical, 1);
        bc_set_flags(&flags, &idx, cv->var_kind, 4);
        assert(idx <= 16);
        bc_put_u16(s, flags);
    }

    if (JS_WriteFunctionBytecode(s, b->byte_code_buf, b->byte_code_len))
        goto fail;

    if (b->has_debug) {
        bc_put_atom(s, b->debug.filename);
        bc_put_leb128(s, b->debug.pc2line_len);
        dbuf_put(&s->dbuf, b->debug.pc2line_buf, b->debug.pc2line_len);
        if (b->debug.source) {
            bc_put_leb128(s, b->debug.source_len);
            dbuf_put(&s->dbuf, (uint8_t*)b->debug.source, b->debug.source_len);
        } else {
            bc_put_leb128(s, 0);
        }
    }

    for (i = 0; i < b->cpool_count; i++) {
        if (JS_WriteObjectRec(s, b->cpool[i]))
            goto fail;
    }
    return 0;
fail:
    return -1;
}

static int JS_WriteModule(BCWriterState* s, JSValueConst obj)
{
    JSModuleDef* m = JS_VALUE_GET_PTR(obj);
    int i;

    bc_put_u8(s, BC_TAG_MODULE);
    bc_put_atom(s, m->module_name);

    bc_put_leb128(s, m->req_module_entries_count);
    for (i = 0; i < m->req_module_entries_count; i++) {
        JSReqModuleEntry* rme = &m->req_module_entries[i];
        bc_put_atom(s, rme->module_name);
        if (JS_WriteObjectRec(s, rme->attributes))
            goto fail;
    }

    bc_put_leb128(s, m->export_entries_count);
    for (i = 0; i < m->export_entries_count; i++) {
        JSExportEntry* me = &m->export_entries[i];
        bc_put_u8(s, me->export_type);
        if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
            bc_put_leb128(s, me->u.local.var_idx);
        } else {
            bc_put_leb128(s, me->u.req_module_idx);
            bc_put_atom(s, me->local_name);
        }
        bc_put_atom(s, me->export_name);
    }

    bc_put_leb128(s, m->star_export_entries_count);
    for (i = 0; i < m->star_export_entries_count; i++) {
        JSStarExportEntry* se = &m->star_export_entries[i];
        bc_put_leb128(s, se->req_module_idx);
    }

    bc_put_leb128(s, m->import_entries_count);
    for (i = 0; i < m->import_entries_count; i++) {
        JSImportEntry* mi = &m->import_entries[i];
        bc_put_leb128(s, mi->var_idx);
        bc_put_u8(s, mi->is_star);
        bc_put_atom(s, mi->import_name);
        bc_put_leb128(s, mi->req_module_idx);
    }

    bc_put_u8(s, m->has_tla);

    if (JS_WriteObjectRec(s, m->func_obj))
        goto fail;
    return 0;
fail:
    return -1;
}

static int JS_WriteArray(BCWriterState* s, JSValueConst obj)
{
    JSContext* ctx = s->ctx;
    JSObject* p = JS_VALUE_GET_OBJ(obj);
    uint32_t i, len;
    int ret;
    BOOL is_template;
    JSShapeProperty* prs;
    JSProperty* pr;

    if (s->allow_bytecode && !p->extensible) {
        bc_put_u8(s, BC_TAG_TEMPLATE_OBJECT);
        is_template = TRUE;
    } else {
        bc_put_u8(s, BC_TAG_ARRAY);
        is_template = FALSE;
    }
    if (js_get_length32(ctx, &len, obj))
        goto fail;
    bc_put_leb128(s, len);
    if (p->fast_array) {
        for (i = 0; i < p->u.array.count; i++) {
            ret = JS_WriteObjectRec(s, p->u.array.u.values[i]);
            if (ret)
                goto fail;
        }
        for (i = p->u.array.count; i < len; i++) {
            ret = JS_WriteObjectRec(s, JS_UNDEFINED);
            if (ret)
                goto fail;
        }
    } else {
        for (i = 0; i < len; i++) {
            JSAtom atom;
            atom = JS_NewAtomUInt32(ctx, i);
            if (atom == JS_ATOM_NULL)
                goto fail;
            prs = find_own_property(&pr, p, atom);
            JS_FreeAtom(ctx, atom);
            if (prs && (prs->flags & JS_PROP_ENUMERABLE)) {
                if (prs->flags & JS_PROP_TMASK) {
                    JS_ThrowTypeError(ctx, "only value properties are supported");
                    goto fail;
                }
                ret = JS_WriteObjectRec(s, pr->u.value);
                if (ret)
                    goto fail;
            } else {
                ret = JS_WriteObjectRec(s, JS_UNDEFINED);
                if (ret)
                    goto fail;
            }
        }
    }
    if (is_template) {
        prs = find_own_property(&pr, p, JS_ATOM_raw);
        if (prs) {
            if (prs->flags & JS_PROP_TMASK) {
                JS_ThrowTypeError(ctx, "only value properties are supported");
                goto fail;
            }
            ret = JS_WriteObjectRec(s, pr->u.value);
            if (ret)
                goto fail;
        } else {
            ret = JS_WriteObjectRec(s, JS_UNDEFINED);
            if (ret)
                goto fail;
        }
    }
    return 0;
fail:
    return -1;
}

static int JS_WriteObjectTag(BCWriterState* s, JSValueConst obj)
{
    JSObject* p = JS_VALUE_GET_OBJ(obj);
    uint32_t i, prop_count;
    JSShape* sh;
    JSShapeProperty* pr;
    int pass;
    JSAtom atom;

    bc_put_u8(s, BC_TAG_OBJECT);
    prop_count = 0;
    sh = p->shape;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1)
            bc_put_leb128(s, prop_count);
        for (i = 0, pr = get_shape_prop(sh); i < sh->prop_count; i++, pr++) {
            atom = pr->atom;
            if (atom != JS_ATOM_NULL && JS_AtomIsString(s->ctx, atom) && (pr->flags & JS_PROP_ENUMERABLE)) {
                if (pr->flags & JS_PROP_TMASK) {
                    JS_ThrowTypeError(s->ctx, "only value properties are supported");
                    goto fail;
                }
                if (pass == 0) {
                    prop_count++;
                } else {
                    bc_put_atom(s, atom);
                    if (JS_WriteObjectRec(s, p->prop[i].u.value))
                        goto fail;
                }
            }
        }
    }
    return 0;
fail:
    return -1;
}

static int JS_WriteTypedArray(BCWriterState* s, JSValueConst obj)
{
    JSObject* p = JS_VALUE_GET_OBJ(obj);
    JSTypedArray* ta = p->u.typed_array;

    bc_put_u8(s, BC_TAG_TYPED_ARRAY);
    bc_put_u8(s, p->class_id - JS_CLASS_UINT8C_ARRAY);
    bc_put_leb128(s, p->u.array.count);
    bc_put_leb128(s, ta->offset);
    if (JS_WriteObjectRec(s, JS_MKPTR(JS_TAG_OBJECT, ta->buffer)))
        return -1;
    return 0;
}

static int JS_WriteArrayBuffer(BCWriterState* s, JSValueConst obj)
{
    JSObject* p = JS_VALUE_GET_OBJ(obj);
    JSArrayBuffer* abuf = p->u.array_buffer;
    if (abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(s->ctx);
        return -1;
    }
    bc_put_u8(s, BC_TAG_ARRAY_BUFFER);
    bc_put_leb128(s, abuf->byte_length);
    bc_put_leb128(s, abuf->max_byte_length);
    dbuf_put(&s->dbuf, abuf->data, abuf->byte_length);
    return 0;
}

static int JS_WriteSharedArrayBuffer(BCWriterState* s, JSValueConst obj)
{
    JSObject* p = JS_VALUE_GET_OBJ(obj);
    JSArrayBuffer* abuf = p->u.array_buffer;
    assert(!abuf->detached);
    bc_put_u8(s, BC_TAG_SHARED_ARRAY_BUFFER);
    bc_put_leb128(s, abuf->byte_length);
    bc_put_leb128(s, abuf->max_byte_length);
    bc_put_u64(s, (uintptr_t)abuf->data);
    if (js_resize_array(s->ctx, (void**)&s->sab_tab, sizeof(s->sab_tab[0]),
            &s->sab_tab_size, s->sab_tab_len + 1))
        return -1;
    s->sab_tab[s->sab_tab_len++] = abuf->data;
    return 0;
}

static int JS_WriteObjectRec(BCWriterState* s, JSValueConst obj)
{
    uint32_t tag;

    if (js_check_stack_overflow(s->ctx->rt, 0)) {
        JS_ThrowStackOverflow(s->ctx);
        return -1;
    }

    tag = JS_VALUE_GET_NORM_TAG(obj);
    switch (tag) {
    case JS_TAG_NULL:
        bc_put_u8(s, BC_TAG_NULL);
        break;
    case JS_TAG_UNDEFINED:
        bc_put_u8(s, BC_TAG_UNDEFINED);
        break;
    case JS_TAG_BOOL:
        bc_put_u8(s, BC_TAG_BOOL_FALSE + JS_VALUE_GET_INT(obj));
        break;
    case JS_TAG_INT:
        bc_put_u8(s, BC_TAG_INT32);
        bc_put_sleb128(s, JS_VALUE_GET_INT(obj));
        break;
    case JS_TAG_FLOAT64: {
        JSFloat64Union u;
        bc_put_u8(s, BC_TAG_FLOAT64);
        u.d = JS_VALUE_GET_FLOAT64(obj);
        bc_put_u64(s, u.u64);
    } break;
    case JS_TAG_STRING: {
        JSString* p = JS_VALUE_GET_STRING(obj);
        bc_put_u8(s, BC_TAG_STRING);
        JS_WriteString(s, p);
    } break;
    case JS_TAG_STRING_ROPE: {
        JSValue str;
        int ret;
        str = JS_ToString(s->ctx, obj);
        if (JS_IsException(str))
            goto fail;
        ret = JS_WriteObjectRec(s, str);
        JS_FreeValue(s->ctx, str);
        if (ret)
            goto fail;
    } break;
    case JS_TAG_FUNCTION_BYTECODE:
        if (!s->allow_bytecode)
            goto invalid_tag;
        if (JS_WriteFunctionTag(s, obj))
            goto fail;
        break;
    case JS_TAG_MODULE:
        if (!s->allow_bytecode)
            goto invalid_tag;
        if (JS_WriteModule(s, obj))
            goto fail;
        break;
    case JS_TAG_OBJECT: {
        JSObject* p = JS_VALUE_GET_OBJ(obj);
        int ret, idx;

        if (s->allow_reference) {
            idx = js_object_list_find(s->ctx, &s->object_list, p);
            if (idx >= 0) {
                bc_put_u8(s, BC_TAG_OBJECT_REFERENCE);
                bc_put_leb128(s, idx);
                break;
            } else {
                if (js_object_list_add(s->ctx, &s->object_list, p))
                    goto fail;
            }
        } else {
            if (p->tmp_mark) {
                JS_ThrowTypeError(s->ctx, "circular reference");
                goto fail;
            }
            p->tmp_mark = 1;
        }
        switch (p->class_id) {
        case JS_CLASS_ARRAY:
            ret = JS_WriteArray(s, obj);
            break;
        case JS_CLASS_OBJECT:
            ret = JS_WriteObjectTag(s, obj);
            break;
        case JS_CLASS_ARRAY_BUFFER:
            ret = JS_WriteArrayBuffer(s, obj);
            break;
        case JS_CLASS_SHARED_ARRAY_BUFFER:
            if (!s->allow_sab)
                goto invalid_tag;
            ret = JS_WriteSharedArrayBuffer(s, obj);
            break;
        case JS_CLASS_DATE:
            bc_put_u8(s, BC_TAG_DATE);
            ret = JS_WriteObjectRec(s, p->u.object_data);
            break;
        case JS_CLASS_NUMBER:
        case JS_CLASS_STRING:
        case JS_CLASS_BOOLEAN:
        case JS_CLASS_BIG_INT:
            bc_put_u8(s, BC_TAG_OBJECT_VALUE);
            ret = JS_WriteObjectRec(s, p->u.object_data);
            break;
        default:
            if (p->class_id >= JS_CLASS_UINT8C_ARRAY && p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
                ret = JS_WriteTypedArray(s, obj);
            } else {
                JS_ThrowTypeError(s->ctx, "unsupported object class");
                ret = -1;
            }
            break;
        }
        p->tmp_mark = 0;
        if (ret)
            goto fail;
    } break;
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        if (JS_WriteBigInt(s, obj))
            goto fail;
        break;
    default:
    invalid_tag:
        JS_ThrowInternalError(s->ctx, "unsupported tag (%d)", tag);
        goto fail;
    }
    return 0;

fail:
    return -1;
}
