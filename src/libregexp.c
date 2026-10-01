#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>

#include "cutils.h"
#include "libregexp.h"
#include "libunicode.h"
#include "dyna-simd-kernels.h"

#ifndef CONFIG_RE_PREFILTER
#define CONFIG_RE_PREFILTER 1
#endif

#if defined(TEST)
#define DUMP_REOP
#endif
typedef enum {
#define DEF(id, size) REOP_##id,
#include "libregexp-opcode.h"
#undef DEF
    REOP_COUNT,
} REOPCodeEnum;

#define CAPTURE_COUNT_MAX 255
#define REGISTER_COUNT_MAX 255
#define INTERRUPT_COUNTER_INIT 10000

#ifndef LRE_DEFAULT_EXEC_STEPS
#define LRE_DEFAULT_EXEC_STEPS 100000000ull
#endif
#define LRE_EXEC_STEPS_PER_CHAR 1000

#ifndef LRE_BACKTRACK_MAX_STACK_SIZE
#define LRE_BACKTRACK_MAX_STACK_SIZE (256u << 20)
#endif

#define CP_LS 0x2028
#define CP_PS 0x2029

#define TMP_BUF_SIZE 128

typedef struct {
    DynBuf byte_code;
    const uint8_t* buf_ptr;
    const uint8_t* buf_end;
    const uint8_t* buf_start;
    int re_flags;
    BOOL is_unicode;
    BOOL unicode_sets;
    BOOL ignore_case;
    BOOL multi_line;
    BOOL dotall;
    uint8_t group_name_scope;
    int capture_count;
    int total_capture_count;
    int has_named_captures;
    void* opaque;
    DynBuf group_names;
    union {
        char error_msg[TMP_BUF_SIZE];
        char tmp_buf[TMP_BUF_SIZE];
    } u;
} REParseState;

typedef struct {
#ifdef DUMP_REOP
    const char* name;
#endif
    uint8_t size;
} REOpCode;

static const REOpCode reopcode_info[REOP_COUNT] = {
#ifdef DUMP_REOP
#define DEF(id, size) { #id, size },
#else
#define DEF(id, size) { size },
#endif
#include "libregexp-opcode.h"
#undef DEF
};

#define RE_HEADER_FLAGS 0
#define RE_HEADER_CAPTURE_COUNT 2
#define RE_HEADER_REGISTER_COUNT 3
#define RE_HEADER_BYTECODE_LEN 4

#define RE_HEADER_LEN 8

static inline int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int dbuf_insert(DynBuf* s, int pos, int len)
{
    if (dbuf_claim(s, len))
        return -1;
    memmove(s->buf + pos + len, s->buf + pos, s->size - pos);
    s->size += len;
    return 0;
}

typedef struct REString {
    struct REString* next;
    uint32_t hash;
    uint32_t len;
    uint32_t buf[];
} REString;

typedef struct {
    CharRange cr;
    uint32_t n_strings;
    uint32_t hash_size;
    int hash_bits;
    REString** hash_table;
} REStringList;

static uint32_t re_string_hash(int len, const uint32_t* buf)
{
    int i;
    uint32_t h;
    h = 1;
    for (i = 0; i < len; i++)
        h = h * 263 + buf[i];
    return h * 0x61C88647;
}

static void re_string_list_init(REParseState* s1, REStringList* s)
{
    cr_init(&s->cr, s1->opaque, lre_realloc);
    s->n_strings = 0;
    s->hash_size = 0;
    s->hash_bits = 0;
    s->hash_table = NULL;
}

static void re_string_list_free(REStringList* s)
{
    REString *p, *p_next;
    int i;
    for (i = 0; i < s->hash_size; i++) {
        for (p = s->hash_table[i]; p != NULL; p = p_next) {
            p_next = p->next;
            lre_realloc(s->cr.mem_opaque, p, 0);
        }
    }
    lre_realloc(s->cr.mem_opaque, s->hash_table, 0);

    cr_free(&s->cr);
}

static void lre_print_char(int c, BOOL is_range)
{
    if (c == '\'' || c == '\\' || (is_range && (c == '-' || c == ']'))) {
        printf("\\%c", c);
    } else if (c >= ' ' && c <= 126) {
        printf("%c", c);
    } else {
        printf("\\u{%04x}", c);
    }
}

static __maybe_unused void re_string_list_dump(const char* str, const REStringList* s)
{
    REString* p;
    const CharRange* cr;
    int i, j, k;

    printf("%s:\n", str);
    printf("  ranges: [");
    cr = &s->cr;
    for (i = 0; i < cr->len; i += 2) {
        lre_print_char(cr->points[i], TRUE);
        if (cr->points[i] != cr->points[i + 1] - 1) {
            printf("-");
            lre_print_char(cr->points[i + 1] - 1, TRUE);
        }
    }
    printf("]\n");

    j = 0;
    for (i = 0; i < s->hash_size; i++) {
        for (p = s->hash_table[i]; p != NULL; p = p->next) {
            printf("  %d/%d: '", j, s->n_strings);
            for (k = 0; k < p->len; k++) {
                lre_print_char(p->buf[k], FALSE);
            }
            printf("'\n");
            j++;
        }
    }
}

static int re_string_find2(REStringList* s, int len, const uint32_t* buf,
    uint32_t h0, BOOL add_flag)
{
    uint32_t h = 0;
    REString* p;
    if (s->n_strings != 0) {
        h = h0 >> (32 - s->hash_bits);
        for (p = s->hash_table[h]; p != NULL; p = p->next) {
            if (p->hash == h0 && p->len == len && !memcmp(p->buf, buf, len * sizeof(buf[0]))) {
                return 1;
            }
        }
    }
    if (!add_flag)
        return 0;
    if (unlikely((s->n_strings + 1) > s->hash_size)) {
        REString **new_hash_table, *p_next;
        int new_hash_bits, i;
        uint32_t new_hash_size;
        new_hash_bits = max_int(s->hash_bits + 1, 4);
        new_hash_size = 1 << new_hash_bits;
        new_hash_table = lre_realloc(s->cr.mem_opaque, NULL,
            sizeof(new_hash_table[0]) * new_hash_size);
        if (!new_hash_table)
            return -1;
        memset(new_hash_table, 0, sizeof(new_hash_table[0]) * new_hash_size);
        for (i = 0; i < s->hash_size; i++) {
            for (p = s->hash_table[i]; p != NULL; p = p_next) {
                p_next = p->next;
                h = p->hash >> (32 - new_hash_bits);
                p->next = new_hash_table[h];
                new_hash_table[h] = p;
            }
        }
        lre_realloc(s->cr.mem_opaque, s->hash_table, 0);
        s->hash_bits = new_hash_bits;
        s->hash_size = new_hash_size;
        s->hash_table = new_hash_table;
        h = h0 >> (32 - s->hash_bits);
    }

    p = lre_realloc(s->cr.mem_opaque, NULL, sizeof(REString) + len * sizeof(buf[0]));
    if (!p)
        return -1;
    p->next = s->hash_table[h];
    s->hash_table[h] = p;
    s->n_strings++;
    p->hash = h0;
    p->len = len;
    memcpy(p->buf, buf, sizeof(buf[0]) * len);
    return 1;
}

static int re_string_find(REStringList* s, int len, const uint32_t* buf,
    BOOL add_flag)
{
    uint32_t h0;
    h0 = re_string_hash(len, buf);
    return re_string_find2(s, len, buf, h0, add_flag);
}

static int re_string_add(REStringList* s, int len, const uint32_t* buf)
{
    if (len == 1) {
        return cr_union_interval(&s->cr, buf[0], buf[0]);
    }
    if (re_string_find(s, len, buf, TRUE) < 0)
        return -1;
    return 0;
}

static int re_string_list_op(REStringList* a, REStringList* b, int op)
{
    int i, ret;
    REString *p, **pp;

    if (cr_op1(&a->cr, b->cr.points, b->cr.len, op))
        return -1;

    switch (op) {
    case CR_OP_UNION:
        if (b->n_strings != 0) {
            for (i = 0; i < b->hash_size; i++) {
                for (p = b->hash_table[i]; p != NULL; p = p->next) {
                    if (re_string_find2(a, p->len, p->buf, p->hash, TRUE) < 0)
                        return -1;
                }
            }
        }
        break;
    case CR_OP_INTER:
    case CR_OP_SUB:
        for (i = 0; i < a->hash_size; i++) {
            pp = &a->hash_table[i];
            for (;;) {
                p = *pp;
                if (p == NULL)
                    break;
                ret = re_string_find2(b, p->len, p->buf, p->hash, FALSE);
                if (op == CR_OP_SUB)
                    ret = !ret;
                if (!ret) {
                    *pp = p->next;
                    a->n_strings--;
                    lre_realloc(a->cr.mem_opaque, p, 0);
                } else {
                    pp = &p->next;
                }
            }
        }
        break;
    default:
        abort();
    }
    return 0;
}

static int re_string_list_canonicalize(REParseState* s1,
    REStringList* s, BOOL is_unicode)
{
    if (cr_regexp_canonicalize(&s->cr, is_unicode))
        return -1;
    if (s->n_strings != 0) {
        REStringList a_s, *a = &a_s;
        int i, j;
        REString* p;

        re_string_list_init(s1, a);

        a->n_strings = s->n_strings;
        a->hash_size = s->hash_size;
        a->hash_bits = s->hash_bits;
        a->hash_table = s->hash_table;

        s->n_strings = 0;
        s->hash_size = 0;
        s->hash_bits = 0;
        s->hash_table = NULL;

        for (i = 0; i < a->hash_size; i++) {
            for (p = a->hash_table[i]; p != NULL; p = p->next) {
                for (j = 0; j < p->len; j++) {
                    p->buf[j] = lre_canonicalize(p->buf[j], is_unicode);
                }
                if (re_string_add(s, p->len, p->buf)) {
                    re_string_list_free(a);
                    return -1;
                }
            }
        }
        re_string_list_free(a);
    }
    return 0;
}

static const uint16_t char_range_d[] = {
    1,
    0x0030,
    0x0039 + 1,
};

static const uint16_t char_range_s[] = {
    10,
    0x0009,
    0x000D + 1,
    0x0020,
    0x0020 + 1,
    0x00A0,
    0x00A0 + 1,
    0x1680,
    0x1680 + 1,
    0x2000,
    0x200A + 1,
    0x2028,
    0x2029 + 1,
    0x202F,
    0x202F + 1,
    0x205F,
    0x205F + 1,
    0x3000,
    0x3000 + 1,
    0xFEFF,
    0xFEFF + 1,
};

static const uint16_t char_range_w[] = {
    4,
    0x0030,
    0x0039 + 1,
    0x0041,
    0x005A + 1,
    0x005F,
    0x005F + 1,
    0x0061,
    0x007A + 1,
};

#define CLASS_RANGE_BASE 0x40000000

typedef enum {
    CHAR_RANGE_d,
    CHAR_RANGE_D,
    CHAR_RANGE_s,
    CHAR_RANGE_S,
    CHAR_RANGE_w,
    CHAR_RANGE_W,
} CharRangeEnum;

static const uint16_t* const char_range_table[] = {
    char_range_d,
    char_range_s,
    char_range_w,
};

static int cr_init_char_range(REParseState* s, REStringList* cr, uint32_t c)
{
    BOOL invert;
    const uint16_t* c_pt;
    int len, i;

    invert = c & 1;
    c_pt = char_range_table[c >> 1];
    len = *c_pt++;
    re_string_list_init(s, cr);
    for (i = 0; i < len * 2; i++) {
        if (cr_add_point(&cr->cr, c_pt[i]))
            goto fail;
    }
    if (invert) {
        if (cr_invert(&cr->cr))
            goto fail;
    }
    return 0;
fail:
    re_string_list_free(cr);
    return -1;
}

#ifdef DUMP_REOP
static __maybe_unused void lre_dump_bytecode(const uint8_t* buf,
    int buf_len)
{
    int pos, len, opcode, bc_len, re_flags, i;
    uint32_t val, val2;

    assert(buf_len >= RE_HEADER_LEN);

    re_flags = lre_get_flags(buf);
    bc_len = get_u32(buf + RE_HEADER_BYTECODE_LEN);
    assert(bc_len + RE_HEADER_LEN <= buf_len);
    printf("flags: 0x%x capture_count=%d reg_count=%d\n",
        re_flags, buf[RE_HEADER_CAPTURE_COUNT], buf[RE_HEADER_REGISTER_COUNT]);
    if (re_flags & LRE_FLAG_NAMED_GROUPS) {
        const char* p;
        p = (char*)buf + RE_HEADER_LEN + bc_len;
        printf("named groups: ");
        for (i = 1; i < buf[RE_HEADER_CAPTURE_COUNT]; i++) {
            if (i != 1)
                printf(",");
            printf("<%s>", p);
            p += strlen(p) + LRE_GROUP_NAME_TRAILER_LEN;
        }
        printf("\n");
        assert(p == (char*)(buf + buf_len));
    }
    printf("bytecode_len=%d\n", bc_len);

    buf += RE_HEADER_LEN;
    pos = 0;
    while (pos < bc_len) {
        printf("%5u: ", pos);
        opcode = buf[pos];
        len = reopcode_info[opcode].size;
        if (opcode >= REOP_COUNT) {
            printf(" invalid opcode=0x%02x\n", opcode);
            break;
        }
        if ((pos + len) > bc_len) {
            printf(" buffer overflow (opcode=0x%02x)\n", opcode);
            break;
        }
        printf("%s", reopcode_info[opcode].name);
        switch (opcode) {
        case REOP_char:
        case REOP_char_i:
            val = get_u16(buf + pos + 1);
            if (val >= ' ' && val <= 126)
                printf(" '%c'", val);
            else
                printf(" 0x%04x", val);
            break;
        case REOP_char32:
        case REOP_char32_i:
            val = get_u32(buf + pos + 1);
            if (val >= ' ' && val <= 126)
                printf(" '%c'", val);
            else
                printf(" 0x%08x", val);
            break;
        case REOP_goto:
        case REOP_split_goto_first:
        case REOP_split_next_first:
        case REOP_lookahead:
        case REOP_negative_lookahead:
            val = get_u32(buf + pos + 1);
            val += (pos + 5);
            printf(" %u", val);
            break;
        case REOP_loop:
            val2 = buf[pos + 1];
            val = get_u32(buf + pos + 2);
            val += (pos + 6);
            printf(" r%u, %u", val2, val);
            break;
        case REOP_loop_split_goto_first:
        case REOP_loop_split_next_first:
        case REOP_loop_check_adv_split_goto_first:
        case REOP_loop_check_adv_split_next_first: {
            uint32_t limit;
            val2 = buf[pos + 1];
            limit = get_u32(buf + pos + 2);
            val = get_u32(buf + pos + 6);
            val += (pos + 10);
            printf(" r%u, %u, %u", val2, limit, val);
        } break;
        case REOP_save_start:
        case REOP_save_end:
            printf(" %u", buf[pos + 1]);
            break;
        case REOP_back_reference:
        case REOP_back_reference_i:
        case REOP_backward_back_reference:
        case REOP_backward_back_reference_i: {
            int n, i;
            n = buf[pos + 1];
            len += n;
            for (i = 0; i < n; i++) {
                if (i != 0)
                    printf(",");
                printf(" %u", buf[pos + 2 + i]);
            }
        } break;
        case REOP_save_reset:
            printf(" %u %u", buf[pos + 1], buf[pos + 2]);
            break;
        case REOP_set_i32:
            val = buf[pos + 1];
            val2 = get_u32(buf + pos + 2);
            printf(" r%u, %d", val, val2);
            break;
        case REOP_set_char_pos:
        case REOP_check_advance:
            val = buf[pos + 1];
            printf(" r%u", val);
            break;
        case REOP_range:
        case REOP_range_i: {
            int n, i;
            n = get_u16(buf + pos + 1);
            len += n * 4;
            for (i = 0; i < n * 2; i++) {
                val = get_u16(buf + pos + 3 + i * 2);
                printf(" 0x%04x", val);
            }
        } break;
        case REOP_range32:
        case REOP_range32_i: {
            int n, i;
            n = get_u16(buf + pos + 1);
            len += n * 8;
            for (i = 0; i < n * 2; i++) {
                val = get_u32(buf + pos + 3 + i * 4);
                printf(" 0x%08x", val);
            }
        } break;
        default:
            break;
        }
        printf("\n");
        pos += len;
    }
}
#endif

static void re_emit_op(REParseState* s, int op)
{
    dbuf_putc(&s->byte_code, op);
}

static int re_emit_op_u32(REParseState* s, int op, uint32_t val)
{
    int pos;
    dbuf_putc(&s->byte_code, op);
    pos = s->byte_code.size;
    dbuf_put_u32(&s->byte_code, val);
    return pos;
}

static int re_emit_goto(REParseState* s, int op, uint32_t val)
{
    int pos;
    dbuf_putc(&s->byte_code, op);
    pos = s->byte_code.size;
    dbuf_put_u32(&s->byte_code, val - (pos + 4));
    return pos;
}

static int re_emit_goto_u8(REParseState* s, int op, uint32_t arg, uint32_t val)
{
    int pos;
    dbuf_putc(&s->byte_code, op);
    dbuf_putc(&s->byte_code, arg);
    pos = s->byte_code.size;
    dbuf_put_u32(&s->byte_code, val - (pos + 4));
    return pos;
}

static int re_emit_goto_u8_u32(REParseState* s, int op, uint32_t arg0, uint32_t arg1, uint32_t val)
{
    int pos;
    dbuf_putc(&s->byte_code, op);
    dbuf_putc(&s->byte_code, arg0);
    dbuf_put_u32(&s->byte_code, arg1);
    pos = s->byte_code.size;
    dbuf_put_u32(&s->byte_code, val - (pos + 4));
    return pos;
}

static void re_emit_op_u8(REParseState* s, int op, uint32_t val)
{
    dbuf_putc(&s->byte_code, op);
    dbuf_putc(&s->byte_code, val);
}

static void re_emit_op_u16(REParseState* s, int op, uint32_t val)
{
    dbuf_putc(&s->byte_code, op);
    dbuf_put_u16(&s->byte_code, val);
}

static int __attribute__((format(printf, 2, 3))) re_parse_error(REParseState* s, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->u.error_msg, sizeof(s->u.error_msg), fmt, ap);
    va_end(ap);
    return -1;
}

static int re_parse_out_of_memory(REParseState* s)
{
    return re_parse_error(s, "out of memory");
}

static int parse_digits(const uint8_t** pp, BOOL allow_overflow)
{
    const uint8_t* p;
    uint64_t v;
    int c;

    p = *pp;
    v = 0;
    for (;;) {
        c = *p;
        if (c < '0' || c > '9')
            break;
        v = v * 10 + c - '0';
        if (v >= INT32_MAX) {
            if (allow_overflow)
                v = INT32_MAX;
            else
                return -1;
        }
        p++;
    }
    *pp = p;
    return v;
}

static int re_parse_expect(REParseState* s, const uint8_t** pp, int c)
{
    const uint8_t* p;
    p = *pp;
    if (*p != c)
        return re_parse_error(s, "expecting '%c'", c);
    p++;
    *pp = p;
    return 0;
}

int lre_parse_escape(const uint8_t** pp, int allow_utf16)
{
    const uint8_t* p;
    uint32_t c;

    p = *pp;
    c = *p++;
    switch (c) {
    case 'b':
        c = '\b';
        break;
    case 'f':
        c = '\f';
        break;
    case 'n':
        c = '\n';
        break;
    case 'r':
        c = '\r';
        break;
    case 't':
        c = '\t';
        break;
    case 'v':
        c = '\v';
        break;
    case 'x': {
        int h0, h1;

        h0 = from_hex(*p++);
        if (h0 < 0)
            return -1;
        h1 = from_hex(*p++);
        if (h1 < 0)
            return -1;
        c = (h0 << 4) | h1;
    } break;
    case 'u': {
        int h, i;
        uint32_t c1;

        if (*p == '{' && allow_utf16) {
            p++;
            c = 0;
            for (;;) {
                h = from_hex(*p++);
                if (h < 0)
                    return -1;
                c = (c << 4) | h;
                if (c > 0x10FFFF)
                    return -1;
                if (*p == '}')
                    break;
            }
            p++;
        } else {
            c = 0;
            for (i = 0; i < 4; i++) {
                h = from_hex(*p++);
                if (h < 0) {
                    return -1;
                }
                c = (c << 4) | h;
            }
            if (is_hi_surrogate(c) && allow_utf16 == 2 && p[0] == '\\' && p[1] == 'u') {
                c1 = 0;
                for (i = 0; i < 4; i++) {
                    h = from_hex(p[2 + i]);
                    if (h < 0)
                        break;
                    c1 = (c1 << 4) | h;
                }
                if (i == 4 && is_lo_surrogate(c1)) {
                    p += 6;
                    c = from_surrogate(c, c1);
                }
            }
        }
    } break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        c -= '0';
        if (allow_utf16 == 2) {
            if (c != 0 || is_digit(*p))
                return -1;
        } else {
            uint32_t v;
            v = *p - '0';
            if (v > 7)
                break;
            c = (c << 3) | v;
            p++;
            if (c >= 32)
                break;
            v = *p - '0';
            if (v > 7)
                break;
            c = (c << 3) | v;
            p++;
        }
        break;
    default:
        return -2;
    }
    *pp = p;
    return c;
}

#ifdef CONFIG_ALL_UNICODE
static BOOL is_unicode_char(int c)
{
    return ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c == '_'));
}

static void seq_prop_cb(void* opaque, const uint32_t* seq, int seq_len)
{
    REStringList* sl = opaque;
    re_string_add(sl, seq_len, seq);
}

static int parse_unicode_property(REParseState* s, REStringList* cr,
    const uint8_t** pp, BOOL is_inv,
    BOOL allow_sequence_prop)
{
    const uint8_t* p;
    char name[64], value[64];
    char* q;
    BOOL script_ext;
    int ret;

    p = *pp;
    if (*p != '{')
        return re_parse_error(s, "expecting '{' after \\p");
    p++;
    q = name;
    while (is_unicode_char(*p)) {
        if ((q - name) >= sizeof(name) - 1)
            goto unknown_property_name;
        *q++ = *p++;
    }
    *q = '\0';
    q = value;
    if (*p == '=') {
        p++;
        while (is_unicode_char(*p)) {
            if ((q - value) >= sizeof(value) - 1)
                return re_parse_error(s, "unknown unicode property value");
            *q++ = *p++;
        }
    }
    *q = '\0';
    if (*p != '}')
        return re_parse_error(s, "expecting '}'");
    p++;
    if (!strcmp(name, "Script") || !strcmp(name, "sc")) {
        script_ext = FALSE;
        goto do_script;
    } else if (!strcmp(name, "Script_Extensions") || !strcmp(name, "scx")) {
        script_ext = TRUE;
    do_script:
        re_string_list_init(s, cr);
        ret = unicode_script(&cr->cr, value, script_ext);
        if (ret) {
            re_string_list_free(cr);
            if (ret == -2)
                return re_parse_error(s, "unknown unicode script");
            else
                goto out_of_memory;
        }
    } else if (!strcmp(name, "General_Category") || !strcmp(name, "gc")) {
        re_string_list_init(s, cr);
        ret = unicode_general_category(&cr->cr, value);
        if (ret) {
            re_string_list_free(cr);
            if (ret == -2)
                return re_parse_error(s, "unknown unicode general category");
            else
                goto out_of_memory;
        }
    } else if (value[0] == '\0') {
        re_string_list_init(s, cr);
        ret = unicode_general_category(&cr->cr, name);
        if (ret == -1) {
            re_string_list_free(cr);
            goto out_of_memory;
        }
        if (ret < 0) {
            ret = unicode_prop(&cr->cr, name);
            if (ret == -1) {
                re_string_list_free(cr);
                goto out_of_memory;
            }
        }
        if (ret < 0 && !is_inv && allow_sequence_prop) {
            CharRange cr_tmp;
            cr_init(&cr_tmp, s->opaque, lre_realloc);
            ret = unicode_sequence_prop(name, seq_prop_cb, cr, &cr_tmp);
            cr_free(&cr_tmp);
            if (ret == -1) {
                re_string_list_free(cr);
                goto out_of_memory;
            }
        }
        if (ret < 0)
            goto unknown_property_name;
    } else {
    unknown_property_name:
        return re_parse_error(s, "unknown unicode property name");
    }

    if (s->ignore_case && s->unicode_sets) {
        if (re_string_list_canonicalize(s, cr, s->is_unicode)) {
            re_string_list_free(cr);
            goto out_of_memory;
        }
    }
    if (is_inv) {
        if (cr_invert(&cr->cr)) {
            re_string_list_free(cr);
            goto out_of_memory;
        }
    }
    if (s->ignore_case && !s->unicode_sets) {
        if (re_string_list_canonicalize(s, cr, s->is_unicode)) {
            re_string_list_free(cr);
            goto out_of_memory;
        }
    }
    *pp = p;
    return 0;
out_of_memory:
    return re_parse_out_of_memory(s);
}
#endif

static int get_class_atom(REParseState* s, REStringList* cr,
    const uint8_t** pp, BOOL inclass);

static int parse_class_string_disjunction(REParseState* s, REStringList* cr,
    const uint8_t** pp)
{
    const uint8_t* p;
    DynBuf str;
    int c;

    p = *pp;
    if (*p != '{')
        return re_parse_error(s, "expecting '{' after \\q");

    dbuf_init2(&str, s->opaque, lre_realloc);
    re_string_list_init(s, cr);

    p++;
    for (;;) {
        str.size = 0;
        while (*p != '}' && *p != '|') {
            c = get_class_atom(s, NULL, &p, FALSE);
            if (c < 0)
                goto fail;
            if (dbuf_put_u32(&str, c)) {
                re_parse_out_of_memory(s);
                goto fail;
            }
        }
        if (re_string_add(cr, str.size / 4, (uint32_t*)str.buf)) {
            re_parse_out_of_memory(s);
            goto fail;
        }
        if (*p == '}')
            break;
        p++;
    }
    if (s->ignore_case) {
        if (re_string_list_canonicalize(s, cr, TRUE))
            goto fail;
    }
    p++;
    dbuf_free(&str);
    *pp = p;
    return 0;
fail:
    dbuf_free(&str);
    re_string_list_free(cr);
    return -1;
}

static int get_class_atom(REParseState* s, REStringList* cr,
    const uint8_t** pp, BOOL inclass)
{
    const uint8_t* p;
    uint32_t c;
    int ret;

    p = *pp;

    c = *p;
    switch (c) {
    case '\\':
        p++;
        if (p >= s->buf_end)
            goto unexpected_end;
        c = *p++;
        switch (c) {
        case 'd':
            c = CHAR_RANGE_d;
            goto class_range;
        case 'D':
            c = CHAR_RANGE_D;
            goto class_range;
        case 's':
            c = CHAR_RANGE_s;
            goto class_range;
        case 'S':
            c = CHAR_RANGE_S;
            goto class_range;
        case 'w':
            c = CHAR_RANGE_w;
            goto class_range;
        case 'W':
            c = CHAR_RANGE_W;
        class_range:
            if (!cr)
                goto default_escape;
            if (cr_init_char_range(s, cr, c))
                return -1;
            c += CLASS_RANGE_BASE;
            break;
        case 'c':
            c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (((c >= '0' && c <= '9') || c == '_') && inclass && !s->is_unicode)) {
                c &= 0x1f;
                p++;
            } else if (s->is_unicode) {
                goto invalid_escape;
            } else {
                p--;
                c = '\\';
            }
            break;
        case '-':
            if (!inclass && s->is_unicode)
                goto invalid_escape;
            break;
        case '^':
        case '$':
        case '\\':
        case '.':
        case '*':
        case '+':
        case '?':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
        case '/':
            break;
#ifdef CONFIG_ALL_UNICODE
        case 'p':
        case 'P':
            if (s->is_unicode && cr) {
                if (parse_unicode_property(s, cr, &p, (c == 'P'), s->unicode_sets))
                    return -1;
                c = CLASS_RANGE_BASE;
                break;
            }
            goto default_escape;
#endif
        case 'q':
            if (s->unicode_sets && cr && inclass) {
                if (parse_class_string_disjunction(s, cr, &p))
                    return -1;
                c = CLASS_RANGE_BASE;
                break;
            }
            goto default_escape;
        default:
        default_escape:
            p--;
            ret = lre_parse_escape(&p, s->is_unicode * 2);
            if (ret >= 0) {
                c = ret;
            } else {
                if (s->is_unicode) {
                invalid_escape:
                    return re_parse_error(s, "invalid escape sequence in regular expression");
                } else {
                    goto normal_char;
                }
            }
            break;
        }
        break;
    case '\0':
        if (p >= s->buf_end) {
        unexpected_end:
            return re_parse_error(s, "unexpected end");
        }
        goto normal_char;

    case '&':
    case '!':
    case '#':
    case '$':
    case '%':
    case '*':
    case '+':
    case ',':
    case '.':
    case ':':
    case ';':
    case '<':
    case '=':
    case '>':
    case '?':
    case '@':
    case '^':
    case '`':
    case '~':
        if (s->unicode_sets && p[1] == c) {
            return re_parse_error(s, "invalid class set operation in regular expression");
        }
        goto normal_char;

    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '/':
    case '-':
    case '|':
        if (s->unicode_sets) {
            return re_parse_error(s, "invalid character in class in regular expression");
        }
        goto normal_char;

    default:
    normal_char:
        if (c >= 128) {
            c = unicode_from_utf8(p, UTF8_CHAR_LEN_MAX, &p);
            if ((unsigned)c > 0xffff && !s->is_unicode) {
                return re_parse_error(s, "malformed unicode char");
            }
        } else {
            p++;
        }
        break;
    }
    *pp = p;
    return c;
}

static int re_emit_range(REParseState* s, const CharRange* cr)
{
    int len, i;
    uint32_t high;

    len = (unsigned)cr->len / 2;
    if (len >= 65535)
        return re_parse_error(s, "too many ranges");
    if (len == 0) {
        re_emit_op_u32(s, REOP_char32, -1);
    } else {
        high = cr->points[cr->len - 1];
        if (high == UINT32_MAX)
            high = cr->points[cr->len - 2];
        if (high <= 0xffff) {
            re_emit_op_u16(s, s->ignore_case ? REOP_range_i : REOP_range, len);
            for (i = 0; i < cr->len; i += 2) {
                dbuf_put_u16(&s->byte_code, cr->points[i]);
                high = cr->points[i + 1] - 1;
                if (high == UINT32_MAX - 1)
                    high = 0xffff;
                dbuf_put_u16(&s->byte_code, high);
            }
        } else {
            re_emit_op_u16(s, s->ignore_case ? REOP_range32_i : REOP_range32, len);
            for (i = 0; i < cr->len; i += 2) {
                dbuf_put_u32(&s->byte_code, cr->points[i]);
                dbuf_put_u32(&s->byte_code, cr->points[i + 1] - 1);
            }
        }
    }
    return 0;
}

static int re_string_cmp_len(const void* a, const void* b, void* arg)
{
    REString* p1 = *(REString* const*)a;
    REString* p2 = *(REString* const*)b;
    return (p1->len < p2->len) - (p1->len > p2->len);
}

static void re_emit_char(REParseState* s, int c)
{
    if (c <= 0xffff)
        re_emit_op_u16(s, s->ignore_case ? REOP_char_i : REOP_char, c);
    else
        re_emit_op_u32(s, s->ignore_case ? REOP_char32_i : REOP_char32, c);
}

static int re_emit_string_list(REParseState* s, const REStringList* sl)
{
    REString **tab, *p;
    int i, j, split_pos, last_match_pos, n;
    BOOL has_empty_string, is_last;

    if (sl->n_strings == 0) {
        if (re_emit_range(s, &sl->cr))
            return -1;
    } else {
        tab = lre_realloc(s->opaque, NULL, sizeof(tab[0]) * sl->n_strings);
        if (!tab) {
            re_parse_out_of_memory(s);
            return -1;
        }
        has_empty_string = FALSE;
        n = 0;
        for (i = 0; i < sl->hash_size; i++) {
            for (p = sl->hash_table[i]; p != NULL; p = p->next) {
                if (p->len == 0) {
                    has_empty_string = TRUE;
                } else {
                    tab[n++] = p;
                }
            }
        }
        assert(n <= sl->n_strings);

        rqsort(tab, n, sizeof(tab[0]), re_string_cmp_len, NULL);

        last_match_pos = -1;
        for (i = 0; i < n; i++) {
            p = tab[i];
            is_last = !has_empty_string && sl->cr.len == 0 && i == (n - 1);
            if (!is_last)
                split_pos = re_emit_op_u32(s, REOP_split_next_first, 0);
            else
                split_pos = 0;
            for (j = 0; j < p->len; j++) {
                re_emit_char(s, p->buf[j]);
            }
            if (!is_last) {
                last_match_pos = re_emit_op_u32(s, REOP_goto, last_match_pos);
                put_u32(s->byte_code.buf + split_pos, s->byte_code.size - (split_pos + 4));
            }
        }

        if (sl->cr.len != 0) {
            is_last = !has_empty_string;
            if (!is_last)
                split_pos = re_emit_op_u32(s, REOP_split_next_first, 0);
            else
                split_pos = 0;
            if (re_emit_range(s, &sl->cr)) {
                lre_realloc(s->opaque, tab, 0);
                return -1;
            }
            if (!is_last)
                put_u32(s->byte_code.buf + split_pos, s->byte_code.size - (split_pos + 4));
        }

        while (last_match_pos != -1) {
            int next_pos = get_u32(s->byte_code.buf + last_match_pos);
            put_u32(s->byte_code.buf + last_match_pos, s->byte_code.size - (last_match_pos + 4));
            last_match_pos = next_pos;
        }

        lre_realloc(s->opaque, tab, 0);
    }
    return 0;
}

static int re_parse_nested_class(REParseState* s, REStringList* cr, const uint8_t** pp);

static int re_parse_class_set_operand(REParseState* s, REStringList* cr, const uint8_t** pp)
{
    int c1;
    const uint8_t* p = *pp;

    if (*p == '[') {
        if (re_parse_nested_class(s, cr, pp))
            return -1;
    } else {
        c1 = get_class_atom(s, cr, pp, TRUE);
        if (c1 < 0)
            return -1;
        if (c1 < CLASS_RANGE_BASE) {
            re_string_list_init(s, cr);
            if (s->ignore_case)
                c1 = lre_canonicalize(c1, s->is_unicode);
            if (cr_union_interval(&cr->cr, c1, c1)) {
                re_string_list_free(cr);
                return -1;
            }
        }
    }
    return 0;
}

static int re_parse_nested_class(REParseState* s, REStringList* cr, const uint8_t** pp)
{
    const uint8_t* p;
    uint32_t c1, c2;
    int ret;
    REStringList cr1_s, *cr1 = &cr1_s;
    BOOL invert, is_first;

    if (lre_check_stack_overflow(s->opaque, 0))
        return re_parse_error(s, "stack overflow");

    re_string_list_init(s, cr);
    p = *pp;
    p++;

    invert = FALSE;
    if (*p == '^') {
        p++;
        invert = TRUE;
    }

    is_first = TRUE;
    for (;;) {
        if (*p == ']')
            break;
        if (*p == '[' && s->unicode_sets) {
            if (re_parse_nested_class(s, cr1, &p))
                goto fail;
            goto class_union;
        } else {
            c1 = get_class_atom(s, cr1, &p, TRUE);
            if ((int)c1 < 0)
                goto fail;
            if (*p == '-' && p[1] != ']') {
                const uint8_t* p0 = p + 1;
                if (p[1] == '-' && s->unicode_sets && is_first)
                    goto class_atom;
                if (c1 >= CLASS_RANGE_BASE) {
                    if (s->is_unicode) {
                        re_string_list_free(cr1);
                        goto invalid_class_range;
                    }
                    goto class_atom;
                }
                c2 = get_class_atom(s, cr1, &p0, TRUE);
                if ((int)c2 < 0)
                    goto fail;
                if (c2 >= CLASS_RANGE_BASE) {
                    re_string_list_free(cr1);
                    if (s->is_unicode) {
                        goto invalid_class_range;
                    }
                    goto class_atom;
                }
                p = p0;
                if (c2 < c1) {
                invalid_class_range:
                    re_parse_error(s, "invalid class range");
                    goto fail;
                }
                if (s->ignore_case) {
                    CharRange cr2_s, *cr2 = &cr2_s;
                    cr_init(cr2, s->opaque, lre_realloc);
                    if (cr_add_interval(cr2, c1, c2 + 1) || cr_regexp_canonicalize(cr2, s->is_unicode) || cr_op1(&cr->cr, cr2->points, cr2->len, CR_OP_UNION)) {
                        cr_free(cr2);
                        goto memory_error;
                    }
                    cr_free(cr2);
                } else {
                    if (cr_union_interval(&cr->cr, c1, c2))
                        goto memory_error;
                }
                is_first = FALSE;
            } else {
            class_atom:
                if (c1 >= CLASS_RANGE_BASE) {
                class_union:
                    ret = re_string_list_op(cr, cr1, CR_OP_UNION);
                    re_string_list_free(cr1);
                    if (ret)
                        goto memory_error;
                } else {
                    if (s->ignore_case)
                        c1 = lre_canonicalize(c1, s->is_unicode);
                    if (cr_union_interval(&cr->cr, c1, c1))
                        goto memory_error;
                }
            }
        }
        if (s->unicode_sets && is_first) {
            if (*p == '&' && p[1] == '&' && p[2] != '&') {
                for (;;) {
                    if (*p == ']') {
                        break;
                    } else if (*p == '&' && p[1] == '&' && p[2] != '&') {
                        p += 2;
                    } else {
                        goto invalid_operation;
                    }
                    if (re_parse_class_set_operand(s, cr1, &p))
                        goto fail;
                    ret = re_string_list_op(cr, cr1, CR_OP_INTER);
                    re_string_list_free(cr1);
                    if (ret)
                        goto memory_error;
                }
            } else if (*p == '-' && p[1] == '-') {
                for (;;) {
                    if (*p == ']') {
                        break;
                    } else if (*p == '-' && p[1] == '-') {
                        p += 2;
                    } else {
                    invalid_operation:
                        re_parse_error(s, "invalid operation in regular expression");
                        goto fail;
                    }
                    if (re_parse_class_set_operand(s, cr1, &p))
                        goto fail;
                    ret = re_string_list_op(cr, cr1, CR_OP_SUB);
                    re_string_list_free(cr1);
                    if (ret)
                        goto memory_error;
                }
            }
        }
        is_first = FALSE;
    }

    p++;
    *pp = p;
    if (invert) {
        if (cr->n_strings != 0) {
            re_parse_error(s, "negated character class with strings in regular expression debugger eval code");
            goto fail;
        }
        if (cr_invert(&cr->cr))
            goto memory_error;
    }
    return 0;
memory_error:
    re_parse_out_of_memory(s);
fail:
    re_string_list_free(cr);
    return -1;
}

static int re_parse_char_class(REParseState* s, const uint8_t** pp)
{
    REStringList cr_s, *cr = &cr_s;

    if (re_parse_nested_class(s, cr, pp))
        return -1;
    if (re_emit_string_list(s, cr))
        goto fail;
    re_string_list_free(cr);
    return 0;
fail:
    re_string_list_free(cr);
    return -1;
}

static BOOL re_need_check_adv_and_capture_init(BOOL* pneed_capture_init,
    const uint8_t* bc_buf, int bc_buf_len)
{
    int pos, opcode, len;
    uint32_t val;
    BOOL need_check_adv, need_capture_init;

    need_check_adv = TRUE;
    need_capture_init = FALSE;
    pos = 0;
    while (pos < bc_buf_len) {
        opcode = bc_buf[pos];
        len = reopcode_info[opcode].size;
        switch (opcode) {
        case REOP_range:
        case REOP_range_i:
            val = get_u16(bc_buf + pos + 1);
            len += val * 4;
            need_check_adv = FALSE;
            break;
        case REOP_range32:
        case REOP_range32_i:
            val = get_u16(bc_buf + pos + 1);
            len += val * 8;
            need_check_adv = FALSE;
            break;
        case REOP_char:
        case REOP_char_i:
        case REOP_char32:
        case REOP_char32_i:
        case REOP_dot:
        case REOP_any:
        case REOP_space:
        case REOP_not_space:
            need_check_adv = FALSE;
            break;
        case REOP_line_start:
        case REOP_line_start_m:
        case REOP_line_end:
        case REOP_line_end_m:
        case REOP_set_i32:
        case REOP_set_char_pos:
        case REOP_word_boundary:
        case REOP_word_boundary_i:
        case REOP_not_word_boundary:
        case REOP_not_word_boundary_i:
        case REOP_prev:
            break;
        case REOP_save_start:
        case REOP_save_end:
        case REOP_save_reset:
            break;
        case REOP_back_reference:
        case REOP_back_reference_i:
        case REOP_backward_back_reference:
        case REOP_backward_back_reference_i:
            val = bc_buf[pos + 1];
            len += val;
            need_capture_init = TRUE;
            break;
        default:
            need_capture_init = TRUE;
            goto done;
        }
        pos += len;
    }
done:
    *pneed_capture_init = need_capture_init;
    return need_check_adv;
}

static int re_parse_group_name(char* buf, int buf_size, const uint8_t** pp)
{
    const uint8_t *p, *p1;
    uint32_t c, d;
    char* q;

    p = *pp;
    q = buf;
    for (;;) {
        c = *p;
        if (c == '\\') {
            p++;
            if (*p != 'u')
                return -1;
            c = lre_parse_escape(&p, 2);
        } else if (c == '>') {
            break;
        } else if (c >= 128) {
            c = unicode_from_utf8(p, UTF8_CHAR_LEN_MAX, &p);
            if (is_hi_surrogate(c)) {
                d = unicode_from_utf8(p, UTF8_CHAR_LEN_MAX, &p1);
                if (is_lo_surrogate(d)) {
                    c = from_surrogate(c, d);
                    p = p1;
                }
            }
        } else {
            p++;
        }
        if (c > 0x10FFFF)
            return -1;
        if (q == buf) {
            if (!lre_js_is_ident_first(c))
                return -1;
        } else {
            if (!lre_js_is_ident_next(c))
                return -1;
        }
        if ((q - buf + UTF8_CHAR_LEN_MAX + 1) > buf_size)
            return -1;
        if (c < 128) {
            *q++ = c;
        } else {
            q += unicode_to_utf8((uint8_t*)q, c);
        }
    }
    if (q == buf)
        return -1;
    *q = '\0';
    p++;
    *pp = p;
    return 0;
}

static int re_parse_captures(REParseState* s, int* phas_named_captures,
    const char* capture_name, BOOL emit_group_index)
{
    const uint8_t* p;
    int capture_index, n;
    char name[TMP_BUF_SIZE];

    capture_index = 1;
    n = 0;
    *phas_named_captures = 0;
    for (p = s->buf_start; p < s->buf_end; p++) {
        switch (*p) {
        case '(':
            if (p[1] == '?') {
                if (p[2] == '<' && p[3] != '=' && p[3] != '!') {
                    *phas_named_captures = 1;
                    if (capture_name) {
                        p += 3;
                        if (re_parse_group_name(name, sizeof(name), &p) == 0) {
                            if (!strcmp(name, capture_name)) {
                                if (emit_group_index)
                                    dbuf_putc(&s->byte_code, capture_index);
                                n++;
                            }
                        }
                    }
                    capture_index++;
                    if (capture_index >= CAPTURE_COUNT_MAX)
                        goto done;
                }
            } else {
                capture_index++;
                if (capture_index >= CAPTURE_COUNT_MAX)
                    goto done;
            }
            break;
        case '\\':
            p++;
            break;
        case '[':
            for (p += 1 + (*p == ']'); p < s->buf_end && *p != ']'; p++) {
                if (*p == '\\')
                    p++;
            }
            break;
        }
    }
done:
    if (capture_name) {
        return n;
    } else {
        return capture_index;
    }
}

static int re_count_captures(REParseState* s)
{
    if (s->total_capture_count < 0) {
        s->total_capture_count = re_parse_captures(s, &s->has_named_captures,
            NULL, FALSE);
    }
    return s->total_capture_count;
}

static BOOL re_has_named_captures(REParseState* s)
{
    if (s->has_named_captures < 0)
        re_count_captures(s);
    return s->has_named_captures;
}

static int find_group_name(REParseState* s, const char* name, BOOL emit_group_index)
{
    const char *p, *buf_end;
    size_t len, name_len;
    int capture_index, n;

    p = (char*)s->group_names.buf;
    if (!p)
        return 0;
    buf_end = (char*)s->group_names.buf + s->group_names.size;
    name_len = strlen(name);
    capture_index = 1;
    n = 0;
    while (p < buf_end) {
        len = strlen(p);
        if (len == name_len && memcmp(name, p, name_len) == 0) {
            if (emit_group_index)
                dbuf_putc(&s->byte_code, capture_index);
            n++;
        }
        p += len + LRE_GROUP_NAME_TRAILER_LEN;
        capture_index++;
    }
    return n;
}

static BOOL is_duplicate_group_name(REParseState* s, const char* name, int scope)
{
    const char *p, *buf_end;
    size_t len, name_len;
    int scope1;

    p = (char*)s->group_names.buf;
    if (!p)
        return 0;
    buf_end = (char*)s->group_names.buf + s->group_names.size;
    name_len = strlen(name);
    while (p < buf_end) {
        len = strlen(p);
        if (len == name_len && memcmp(name, p, name_len) == 0) {
            scope1 = (uint8_t)p[len + 1];
            if (scope == scope1)
                return TRUE;
        }
        p += len + LRE_GROUP_NAME_TRAILER_LEN;
    }
    return FALSE;
}

static int re_parse_disjunction(REParseState* s, BOOL is_backward_dir);

static int re_parse_modifiers(REParseState* s, const uint8_t** pp)
{
    const uint8_t* p = *pp;
    int mask = 0;
    int val;

    for (;;) {
        if (*p == 'i') {
            val = LRE_FLAG_IGNORECASE;
        } else if (*p == 'm') {
            val = LRE_FLAG_MULTILINE;
        } else if (*p == 's') {
            val = LRE_FLAG_DOTALL;
        } else {
            break;
        }
        if (mask & val)
            return re_parse_error(s, "duplicate modifier: '%c'", *p);
        mask |= val;
        p++;
    }
    *pp = p;
    return mask;
}

static BOOL update_modifier(BOOL val, int add_mask, int remove_mask,
    int mask)
{
    if (add_mask & mask)
        val = TRUE;
    if (remove_mask & mask)
        val = FALSE;
    return val;
}

static int re_parse_term(REParseState* s, BOOL is_backward_dir)
{
    const uint8_t* p;
    int c, last_atom_start, quant_min, quant_max, last_capture_count;
    BOOL greedy, is_neg, is_backward_lookahead;
    REStringList cr_s, *cr = &cr_s;

    last_atom_start = -1;
    last_capture_count = 0;
    p = s->buf_ptr;
    c = *p;
    switch (c) {
    case '^':
        p++;
        re_emit_op(s, s->multi_line ? REOP_line_start_m : REOP_line_start);
        break;
    case '$':
        p++;
        re_emit_op(s, s->multi_line ? REOP_line_end_m : REOP_line_end);
        break;
    case '.':
        p++;
        last_atom_start = s->byte_code.size;
        last_capture_count = s->capture_count;
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        re_emit_op(s, s->dotall ? REOP_any : REOP_dot);
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        break;
    case '{':
        if (s->is_unicode) {
            return re_parse_error(s, "syntax error");
        } else if (!is_digit(p[1])) {
            goto parse_class_atom;
        } else {
            const uint8_t* p1 = p + 1;
            parse_digits(&p1, TRUE);
            if (*p1 == ',') {
                p1++;
                if (is_digit(*p1)) {
                    parse_digits(&p1, TRUE);
                }
            }
            if (*p1 != '}') {
                goto parse_class_atom;
            }
        }
        [[fallthrough]];
    case '*':
    case '+':
    case '?':
        return re_parse_error(s, "nothing to repeat");
    case '(':
        if (p[1] == '?') {
            if (p[2] == ':') {
                p += 3;
                last_atom_start = s->byte_code.size;
                last_capture_count = s->capture_count;
                s->buf_ptr = p;
                if (re_parse_disjunction(s, is_backward_dir))
                    return -1;
                p = s->buf_ptr;
                if (re_parse_expect(s, &p, ')'))
                    return -1;
            } else if (p[2] == 'i' || p[2] == 'm' || p[2] == 's' || p[2] == '-') {
                BOOL saved_ignore_case, saved_multi_line, saved_dotall;
                int add_mask, remove_mask;
                p += 2;
                remove_mask = 0;
                add_mask = re_parse_modifiers(s, &p);
                if (add_mask < 0)
                    return -1;
                if (*p == '-') {
                    p++;
                    remove_mask = re_parse_modifiers(s, &p);
                    if (remove_mask < 0)
                        return -1;
                }
                if ((add_mask == 0 && remove_mask == 0) || (add_mask & remove_mask) != 0) {
                    return re_parse_error(s, "invalid modifiers");
                }
                if (re_parse_expect(s, &p, ':'))
                    return -1;
                saved_ignore_case = s->ignore_case;
                saved_multi_line = s->multi_line;
                saved_dotall = s->dotall;
                s->ignore_case = update_modifier(s->ignore_case, add_mask, remove_mask, LRE_FLAG_IGNORECASE);
                s->multi_line = update_modifier(s->multi_line, add_mask, remove_mask, LRE_FLAG_MULTILINE);
                s->dotall = update_modifier(s->dotall, add_mask, remove_mask, LRE_FLAG_DOTALL);

                last_atom_start = s->byte_code.size;
                last_capture_count = s->capture_count;
                s->buf_ptr = p;
                if (re_parse_disjunction(s, is_backward_dir))
                    return -1;
                p = s->buf_ptr;
                if (re_parse_expect(s, &p, ')'))
                    return -1;
                s->ignore_case = saved_ignore_case;
                s->multi_line = saved_multi_line;
                s->dotall = saved_dotall;
            } else if ((p[2] == '=' || p[2] == '!')) {
                is_neg = (p[2] == '!');
                is_backward_lookahead = FALSE;
                p += 3;
                goto lookahead;
            } else if (p[2] == '<' && (p[3] == '=' || p[3] == '!')) {
                int pos;
                is_neg = (p[3] == '!');
                is_backward_lookahead = TRUE;
                p += 4;
            lookahead:
                if (!s->is_unicode && !is_backward_lookahead) {
                    last_atom_start = s->byte_code.size;
                    last_capture_count = s->capture_count;
                }
                pos = re_emit_op_u32(s, REOP_lookahead + is_neg, 0);
                s->buf_ptr = p;
                if (re_parse_disjunction(s, is_backward_lookahead))
                    return -1;
                p = s->buf_ptr;
                if (re_parse_expect(s, &p, ')'))
                    return -1;
                re_emit_op(s, REOP_lookahead_match + is_neg);
                if (dbuf_error(&s->byte_code))
                    return -1;
                put_u32(s->byte_code.buf + pos, s->byte_code.size - (pos + 4));
            } else if (p[2] == '<') {
                p += 3;
                if (re_parse_group_name(s->u.tmp_buf, sizeof(s->u.tmp_buf),
                        &p)) {
                    return re_parse_error(s, "invalid group name");
                }
                if (is_duplicate_group_name(s, s->u.tmp_buf, s->group_name_scope)) {
                    return re_parse_error(s, "duplicate group name");
                }
                dbuf_put(&s->group_names, (uint8_t*)s->u.tmp_buf,
                    strlen(s->u.tmp_buf) + 1);
                dbuf_putc(&s->group_names, s->group_name_scope);
                s->has_named_captures = 1;
                goto parse_capture;
            } else {
                return re_parse_error(s, "invalid group");
            }
        } else {
            int capture_index;
            p++;
            dbuf_putc(&s->group_names, 0);
            dbuf_putc(&s->group_names, 0);
        parse_capture:
            if (s->capture_count >= CAPTURE_COUNT_MAX)
                return re_parse_error(s, "too many captures");
            last_atom_start = s->byte_code.size;
            last_capture_count = s->capture_count;
            capture_index = s->capture_count++;
            re_emit_op_u8(s, REOP_save_start + is_backward_dir,
                capture_index);

            s->buf_ptr = p;
            if (re_parse_disjunction(s, is_backward_dir))
                return -1;
            p = s->buf_ptr;

            re_emit_op_u8(s, REOP_save_start + 1 - is_backward_dir,
                capture_index);

            if (re_parse_expect(s, &p, ')'))
                return -1;
        }
        break;
    case '\\':
        switch (p[1]) {
        case 'b':
        case 'B':
            if (p[1] != 'b') {
                re_emit_op(s, s->ignore_case && s->is_unicode ? REOP_not_word_boundary_i : REOP_not_word_boundary);
            } else {
                re_emit_op(s, s->ignore_case && s->is_unicode ? REOP_word_boundary_i : REOP_word_boundary);
            }
            p += 2;
            break;
        case 'k': {
            const uint8_t* p1;
            int dummy_res, n;
            BOOL is_forward;

            p1 = p;
            if (p1[2] != '<') {
                if (s->is_unicode || re_has_named_captures(s))
                    return re_parse_error(s, "expecting group name");
                else
                    goto parse_class_atom;
            }
            p1 += 3;
            if (re_parse_group_name(s->u.tmp_buf, sizeof(s->u.tmp_buf),
                    &p1)) {
                if (s->is_unicode || re_has_named_captures(s))
                    return re_parse_error(s, "invalid group name");
                else
                    goto parse_class_atom;
            }
            is_forward = FALSE;
            n = find_group_name(s, s->u.tmp_buf, FALSE);
            if (n == 0) {
                n = re_parse_captures(s, &dummy_res, s->u.tmp_buf, FALSE);
                if (n == 0) {
                    if (s->is_unicode || re_has_named_captures(s))
                        return re_parse_error(s, "group name not defined");
                    else
                        goto parse_class_atom;
                }
                is_forward = TRUE;
            }
            last_atom_start = s->byte_code.size;
            last_capture_count = s->capture_count;

            re_emit_op_u8(s, REOP_back_reference + 2 * is_backward_dir + s->ignore_case, n);
            if (is_forward) {
                re_parse_captures(s, &dummy_res, s->u.tmp_buf, TRUE);
            } else {
                find_group_name(s, s->u.tmp_buf, TRUE);
            }
            p = p1;
        } break;
        case '0':
            p += 2;
            c = 0;
            if (s->is_unicode) {
                if (is_digit(*p)) {
                    return re_parse_error(s, "invalid decimal escape in regular expression");
                }
            } else {
                if (*p >= '0' && *p <= '7') {
                    c = *p++ - '0';
                    if (*p >= '0' && *p <= '7') {
                        c = (c << 3) + *p++ - '0';
                    }
                }
            }
            goto normal_char;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': {
            const uint8_t* q = ++p;

            c = parse_digits(&p, FALSE);
            if (c < 0 || (c >= s->capture_count && c >= re_count_captures(s))) {
                if (!s->is_unicode) {
                    p = q;
                    if (*p <= '7') {
                        c = 0;
                        if (*p <= '3')
                            c = *p++ - '0';
                        if (*p >= '0' && *p <= '7') {
                            c = (c << 3) + *p++ - '0';
                            if (*p >= '0' && *p <= '7') {
                                c = (c << 3) + *p++ - '0';
                            }
                        }
                    } else {
                        c = *p++;
                    }
                    goto normal_char;
                }
                return re_parse_error(s, "back reference out of range in regular expression");
            }
            last_atom_start = s->byte_code.size;
            last_capture_count = s->capture_count;

            re_emit_op_u8(s, REOP_back_reference + 2 * is_backward_dir + s->ignore_case, 1);
            dbuf_putc(&s->byte_code, c);
        } break;
        default:
            goto parse_class_atom;
        }
        break;
    case '[':
        last_atom_start = s->byte_code.size;
        last_capture_count = s->capture_count;
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        if (re_parse_char_class(s, &p))
            return -1;
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        break;
    case ']':
    case '}':
        if (s->is_unicode)
            return re_parse_error(s, "syntax error");
        goto parse_class_atom;
    default:
    parse_class_atom:
        c = get_class_atom(s, cr, &p, FALSE);
        if ((int)c < 0)
            return -1;
    normal_char:
        last_atom_start = s->byte_code.size;
        last_capture_count = s->capture_count;
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        if (c >= CLASS_RANGE_BASE) {
            int ret = 0;
            if (c == (CLASS_RANGE_BASE + CHAR_RANGE_s)) {
                re_emit_op(s, REOP_space);
            } else if (c == (CLASS_RANGE_BASE + CHAR_RANGE_S)) {
                re_emit_op(s, REOP_not_space);
            } else {
                ret = re_emit_string_list(s, cr);
            }
            re_string_list_free(cr);
            if (ret)
                return -1;
        } else {
            if (s->ignore_case)
                c = lre_canonicalize(c, s->is_unicode);
            re_emit_char(s, c);
        }
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        break;
    }

    if (last_atom_start >= 0) {
        c = *p;
        switch (c) {
        case '*':
            p++;
            quant_min = 0;
            quant_max = INT32_MAX;
            goto quantifier;
        case '+':
            p++;
            quant_min = 1;
            quant_max = INT32_MAX;
            goto quantifier;
        case '?':
            p++;
            quant_min = 0;
            quant_max = 1;
            goto quantifier;
        case '{': {
            const uint8_t* p1 = p;
            if (!is_digit(p[1])) {
                if (s->is_unicode)
                    goto invalid_quant_count;
                break;
            }
            p++;
            quant_min = parse_digits(&p, TRUE);
            quant_max = quant_min;
            if (*p == ',') {
                p++;
                if (is_digit(*p)) {
                    quant_max = parse_digits(&p, TRUE);
                    if (quant_max < quant_min) {
                    invalid_quant_count:
                        return re_parse_error(s, "invalid repetition count");
                    }
                } else {
                    quant_max = INT32_MAX;
                }
            }
            if (*p != '}' && !s->is_unicode) {
                p = p1;
                break;
            }
            if (re_parse_expect(s, &p, '}'))
                return -1;
        }
        quantifier:
            greedy = TRUE;
            if (*p == '?') {
                p++;
                greedy = FALSE;
            }
            if (last_atom_start < 0) {
                return re_parse_error(s, "nothing to repeat");
            }
            {
                BOOL need_capture_init, add_zero_advance_check;
                int len, pos;

                add_zero_advance_check = re_need_check_adv_and_capture_init(&need_capture_init,
                    s->byte_code.buf + last_atom_start,
                    s->byte_code.size - last_atom_start);

                if (need_capture_init && last_capture_count != s->capture_count) {
                    if (dbuf_insert(&s->byte_code, last_atom_start, 3))
                        goto out_of_memory;
                    int wpos = last_atom_start;
                    s->byte_code.buf[wpos++] = REOP_save_reset;
                    s->byte_code.buf[wpos++] = last_capture_count;
                    s->byte_code.buf[wpos++] = s->capture_count - 1;
                }

                len = s->byte_code.size - last_atom_start;
                if (quant_min == 0) {
                    if (!need_capture_init && last_capture_count != s->capture_count) {
                        if (dbuf_insert(&s->byte_code, last_atom_start, 3))
                            goto out_of_memory;
                        s->byte_code.buf[last_atom_start++] = REOP_save_reset;
                        s->byte_code.buf[last_atom_start++] = last_capture_count;
                        s->byte_code.buf[last_atom_start++] = s->capture_count - 1;
                    }
                    if (quant_max == 0) {
                        s->byte_code.size = last_atom_start;
                    } else if (quant_max == 1 || quant_max == INT32_MAX) {
                        BOOL has_goto = (quant_max == INT32_MAX);
                        if (dbuf_insert(&s->byte_code, last_atom_start, 5 + add_zero_advance_check * 2))
                            goto out_of_memory;
                        s->byte_code.buf[last_atom_start] = REOP_split_goto_first + greedy;
                        put_u32(s->byte_code.buf + last_atom_start + 1,
                            len + 5 * has_goto + add_zero_advance_check * 2 * 2);
                        if (add_zero_advance_check) {
                            s->byte_code.buf[last_atom_start + 1 + 4] = REOP_set_char_pos;
                            s->byte_code.buf[last_atom_start + 1 + 4 + 1] = 0;
                            re_emit_op_u8(s, REOP_check_advance, 0);
                        }
                        if (has_goto)
                            re_emit_goto(s, REOP_goto, last_atom_start);
                    } else {
                        if (dbuf_insert(&s->byte_code, last_atom_start, 11 + add_zero_advance_check * 2))
                            goto out_of_memory;
                        pos = last_atom_start;
                        s->byte_code.buf[pos++] = REOP_split_goto_first + greedy;
                        put_u32(s->byte_code.buf + pos, 6 + add_zero_advance_check * 2 + len + 10);
                        pos += 4;

                        s->byte_code.buf[pos++] = REOP_set_i32;
                        s->byte_code.buf[pos++] = 0;
                        put_u32(s->byte_code.buf + pos, quant_max);
                        pos += 4;
                        last_atom_start = pos;
                        if (add_zero_advance_check) {
                            s->byte_code.buf[pos++] = REOP_set_char_pos;
                            s->byte_code.buf[pos++] = 0;
                        }
                        re_emit_goto_u8_u32(s, (add_zero_advance_check ? REOP_loop_check_adv_split_next_first : REOP_loop_split_next_first) - greedy, 0, quant_max, last_atom_start);
                    }
                } else if (quant_min == 1 && quant_max == INT32_MAX && !add_zero_advance_check) {
                    re_emit_goto(s, REOP_split_next_first - greedy,
                        last_atom_start);
                } else {
                    if (quant_min == quant_max)
                        add_zero_advance_check = FALSE;
                    if (dbuf_insert(&s->byte_code, last_atom_start, 6 + add_zero_advance_check * 2))
                        goto out_of_memory;
                    pos = last_atom_start;
                    s->byte_code.buf[pos++] = REOP_set_i32;
                    s->byte_code.buf[pos++] = 0;
                    put_u32(s->byte_code.buf + pos, quant_max);
                    pos += 4;
                    last_atom_start = pos;
                    if (add_zero_advance_check) {
                        s->byte_code.buf[pos++] = REOP_set_char_pos;
                        s->byte_code.buf[pos++] = 0;
                    }
                    if (quant_min == quant_max) {
                        re_emit_goto_u8(s, REOP_loop, 0, last_atom_start);
                    } else {
                        re_emit_goto_u8_u32(s, (add_zero_advance_check ? REOP_loop_check_adv_split_next_first : REOP_loop_split_next_first) - greedy, 0, quant_max - quant_min, last_atom_start);
                    }
                }
                last_atom_start = -1;
            }
            break;
        default:
            break;
        }
    }
    s->buf_ptr = p;
    return 0;
out_of_memory:
    return re_parse_out_of_memory(s);
}

static int re_parse_alternative(REParseState* s, BOOL is_backward_dir)
{
    const uint8_t* p;
    int ret = 0;
    size_t start, term_start, end;
    size_t* terms = NULL;
    size_t n_terms = 0, terms_cap = 0;

    start = s->byte_code.size;
    for (;;) {
        p = s->buf_ptr;
        if (p >= s->buf_end)
            break;
        if (*p == '|' || *p == ')')
            break;
        term_start = s->byte_code.size;
        ret = re_parse_term(s, is_backward_dir);
        if (ret)
            goto done;
        if (is_backward_dir) {
            if (n_terms >= terms_cap) {
                size_t ncap = terms_cap ? terms_cap * 2 : 16;
                size_t* nt = lre_realloc(s->opaque, terms, ncap * sizeof(*nt));
                if (!nt) {
                    ret = -1;
                    goto done;
                }
                terms = nt;
                terms_cap = ncap;
            }
            terms[n_terms++] = term_start;
        }
    }

    if (is_backward_dir && n_terms > 1) {
        uint8_t* tmp;
        size_t i, w, len;

        end = s->byte_code.size;
        len = end - start;
        tmp = lre_realloc(s->opaque, NULL, len);
        if (!tmp) {
            ret = -1;
            goto done;
        }
        memcpy(tmp, s->byte_code.buf + start, len);
        w = start;
        i = n_terms;
        while (i-- > 0) {
            size_t ts = terms[i];
            size_t te = (i + 1 < n_terms) ? terms[i + 1] : end;
            memcpy(s->byte_code.buf + w, tmp + (ts - start), te - ts);
            w += te - ts;
        }
        lre_realloc(s->opaque, tmp, 0);
    }
done:
    lre_realloc(s->opaque, terms, 0);
    return ret;
}

static int re_parse_disjunction(REParseState* s, BOOL is_backward_dir)
{
    int start, len, pos;

    if (lre_check_stack_overflow(s->opaque, 0))
        return re_parse_error(s, "stack overflow");

    start = s->byte_code.size;
    if (re_parse_alternative(s, is_backward_dir))
        return -1;
    while (*s->buf_ptr == '|') {
        s->buf_ptr++;

        len = s->byte_code.size - start;

        if (dbuf_insert(&s->byte_code, start, 5)) {
            return re_parse_out_of_memory(s);
        }
        s->byte_code.buf[start] = REOP_split_next_first;
        put_u32(s->byte_code.buf + start + 1, len + 5);

        pos = re_emit_op_u32(s, REOP_goto, 0);

        s->group_name_scope++;

        if (re_parse_alternative(s, is_backward_dir))
            return -1;

        len = s->byte_code.size - (pos + 4);
        put_u32(s->byte_code.buf + pos, len);
    }
    return 0;
}

static int compute_register_count(uint8_t* bc_buf, int bc_buf_len)
{
    int stack_size, stack_size_max, pos, opcode, len;
    uint32_t val;

    stack_size = 0;
    stack_size_max = 0;
    bc_buf += RE_HEADER_LEN;
    bc_buf_len -= RE_HEADER_LEN;
    pos = 0;
    while (pos < bc_buf_len) {
        opcode = bc_buf[pos];
        len = reopcode_info[opcode].size;
        assert(opcode < REOP_COUNT);
        assert((pos + len) <= bc_buf_len);
        switch (opcode) {
        case REOP_set_i32:
        case REOP_set_char_pos:
            bc_buf[pos + 1] = stack_size;
            stack_size++;
            if (stack_size > stack_size_max) {
                if (stack_size > REGISTER_COUNT_MAX)
                    return -1;
                stack_size_max = stack_size;
            }
            break;
        case REOP_check_advance:
        case REOP_loop:
        case REOP_loop_split_goto_first:
        case REOP_loop_split_next_first:
            assert(stack_size > 0);
            stack_size--;
            bc_buf[pos + 1] = stack_size;
            break;
        case REOP_loop_check_adv_split_goto_first:
        case REOP_loop_check_adv_split_next_first:
            assert(stack_size >= 2);
            stack_size -= 2;
            bc_buf[pos + 1] = stack_size;
            break;
        case REOP_range:
        case REOP_range_i:
            val = get_u16(bc_buf + pos + 1);
            len += val * 4;
            break;
        case REOP_range32:
        case REOP_range32_i:
            val = get_u16(bc_buf + pos + 1);
            len += val * 8;
            break;
        case REOP_back_reference:
        case REOP_back_reference_i:
        case REOP_backward_back_reference:
        case REOP_backward_back_reference_i:
            val = bc_buf[pos + 1];
            len += val;
            break;
        }
        pos += len;
    }
    return stack_size_max;
}

static void* lre_bytecode_realloc(void* opaque, void* ptr, size_t size)
{
    if (size > (INT32_MAX / 2)) {
        return NULL;
    } else {
        return lre_realloc(opaque, ptr, size);
    }
}

uint8_t* lre_compile(int* plen, char* error_msg, int error_msg_size,
    const char* buf, size_t buf_len, int re_flags,
    void* opaque)
{
    REParseState s_s, *s = &s_s;
    int register_count;
    BOOL is_sticky;

    memset(s, 0, sizeof(*s));
    s->opaque = opaque;
    s->buf_ptr = (const uint8_t*)buf;
    s->buf_end = s->buf_ptr + buf_len;
    s->buf_start = s->buf_ptr;
    s->re_flags = re_flags;
    s->is_unicode = ((re_flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)) != 0);
    is_sticky = ((re_flags & LRE_FLAG_STICKY) != 0);
    s->ignore_case = ((re_flags & LRE_FLAG_IGNORECASE) != 0);
    s->multi_line = ((re_flags & LRE_FLAG_MULTILINE) != 0);
    s->dotall = ((re_flags & LRE_FLAG_DOTALL) != 0);
    s->unicode_sets = ((re_flags & LRE_FLAG_UNICODE_SETS) != 0);
    s->capture_count = 1;
    s->total_capture_count = -1;
    s->has_named_captures = -1;

    dbuf_init2(&s->byte_code, opaque, lre_bytecode_realloc);
    dbuf_init2(&s->group_names, opaque, lre_realloc);

    dbuf_put_u16(&s->byte_code, re_flags);
    dbuf_putc(&s->byte_code, 0);
    dbuf_putc(&s->byte_code, 0);
    dbuf_put_u32(&s->byte_code, 0);

    if (!is_sticky) {
        re_emit_op_u32(s, REOP_split_goto_first, 1 + 5);
        re_emit_op(s, REOP_any);
        re_emit_op_u32(s, REOP_goto, -(5 + 1 + 5));
    }
    re_emit_op_u8(s, REOP_save_start, 0);

    if (re_parse_disjunction(s, FALSE)) {
    error:
        dbuf_free(&s->byte_code);
        dbuf_free(&s->group_names);
        pstrcpy(error_msg, error_msg_size, s->u.error_msg);
        *plen = 0;
        return NULL;
    }

    re_emit_op_u8(s, REOP_save_end, 0);

    re_emit_op(s, REOP_match);

    if (*s->buf_ptr != '\0') {
        re_parse_error(s, "extraneous characters at the end");
        goto error;
    }

    if (dbuf_error(&s->byte_code)) {
        re_parse_out_of_memory(s);
        goto error;
    }

    register_count = compute_register_count(s->byte_code.buf, s->byte_code.size);
    if (register_count < 0) {
        re_parse_error(s, "too many imbricated quantifiers");
        goto error;
    }

    s->byte_code.buf[RE_HEADER_CAPTURE_COUNT] = s->capture_count;
    s->byte_code.buf[RE_HEADER_REGISTER_COUNT] = register_count;
    put_u32(s->byte_code.buf + RE_HEADER_BYTECODE_LEN,
        s->byte_code.size - RE_HEADER_LEN);

    if (s->group_names.size > (s->capture_count - 1) * LRE_GROUP_NAME_TRAILER_LEN) {
        dbuf_put(&s->byte_code, s->group_names.buf, s->group_names.size);
        put_u16(s->byte_code.buf + RE_HEADER_FLAGS,
            lre_get_flags(s->byte_code.buf) | LRE_FLAG_NAMED_GROUPS);
    }
    dbuf_free(&s->group_names);

#ifdef DUMP_REOP
    lre_dump_bytecode(s->byte_code.buf, s->byte_code.size);
#endif

    error_msg[0] = '\0';
    *plen = s->byte_code.size;
    return s->byte_code.buf;
}

static BOOL is_line_terminator(uint32_t c)
{
    return (c == '\n' || c == '\r' || c == CP_LS || c == CP_PS);
}

#define GET_CHAR(c, cptr, cbuf_end, cbuf_type)                \
    do {                                                      \
        if (cbuf_type == 0) {                                 \
            c = *cptr++;                                      \
        } else {                                              \
            const uint16_t* _p = (const uint16_t*)cptr;       \
            const uint16_t* _end = (const uint16_t*)cbuf_end; \
            c = *_p++;                                        \
            if (is_hi_surrogate(c) && cbuf_type == 2) {       \
                if (_p < _end && is_lo_surrogate(*_p)) {      \
                    c = from_surrogate(c, *_p++);             \
                }                                             \
            }                                                 \
            cptr = (const void*)_p;                           \
        }                                                     \
    } while (0)

#define PEEK_CHAR(c, cptr, cbuf_end, cbuf_type)               \
    do {                                                      \
        if (cbuf_type == 0) {                                 \
            c = cptr[0];                                      \
        } else {                                              \
            const uint16_t* _p = (const uint16_t*)cptr;       \
            const uint16_t* _end = (const uint16_t*)cbuf_end; \
            c = *_p++;                                        \
            if (is_hi_surrogate(c) && cbuf_type == 2) {       \
                if (_p < _end && is_lo_surrogate(*_p)) {      \
                    c = from_surrogate(c, *_p);               \
                }                                             \
            }                                                 \
        }                                                     \
    } while (0)

#define PEEK_PREV_CHAR(c, cptr, cbuf_start, cbuf_type)            \
    do {                                                          \
        if (cbuf_type == 0) {                                     \
            c = cptr[-1];                                         \
        } else {                                                  \
            const uint16_t* _p = (const uint16_t*)cptr - 1;       \
            const uint16_t* _start = (const uint16_t*)cbuf_start; \
            c = *_p;                                              \
            if (is_lo_surrogate(c) && cbuf_type == 2) {           \
                if (_p > _start && is_hi_surrogate(_p[-1])) {     \
                    c = from_surrogate(*--_p, c);                 \
                }                                                 \
            }                                                     \
        }                                                         \
    } while (0)

#define GET_PREV_CHAR(c, cptr, cbuf_start, cbuf_type)             \
    do {                                                          \
        if (cbuf_type == 0) {                                     \
            cptr--;                                               \
            c = cptr[0];                                          \
        } else {                                                  \
            const uint16_t* _p = (const uint16_t*)cptr - 1;       \
            const uint16_t* _start = (const uint16_t*)cbuf_start; \
            c = *_p;                                              \
            if (is_lo_surrogate(c) && cbuf_type == 2) {           \
                if (_p > _start && is_hi_surrogate(_p[-1])) {     \
                    c = from_surrogate(*--_p, c);                 \
                }                                                 \
            }                                                     \
            cptr = (const void*)_p;                               \
        }                                                         \
    } while (0)

#define PREV_CHAR(cptr, cbuf_start, cbuf_type)                    \
    do {                                                          \
        if (cbuf_type == 0) {                                     \
            cptr--;                                               \
        } else {                                                  \
            const uint16_t* _p = (const uint16_t*)cptr - 1;       \
            const uint16_t* _start = (const uint16_t*)cbuf_start; \
            if (is_lo_surrogate(*_p) && cbuf_type == 2) {         \
                if (_p > _start && is_hi_surrogate(_p[-1])) {     \
                    --_p;                                         \
                }                                                 \
            }                                                     \
            cptr = (const void*)_p;                               \
        }                                                         \
    } while (0)

typedef enum {
    RE_EXEC_STATE_SPLIT,
    RE_EXEC_STATE_LOOKAHEAD,
    RE_EXEC_STATE_NEGATIVE_LOOKAHEAD,
} REExecStateEnum;

#if INTPTR_MAX >= INT64_MAX
#define BP_TYPE_BITS 3
#else
#define BP_TYPE_BITS 2
#endif

typedef union {
    uint8_t* ptr;
    intptr_t val;
    struct {
        uintptr_t val : sizeof(uintptr_t) * 8 - BP_TYPE_BITS;
        uintptr_t type : BP_TYPE_BITS;
    } bp;
} StackElem;

typedef struct {
    const uint8_t* cbuf;
    const uint8_t* cbuf_end;
    int cbuf_type;
    int capture_count;
    BOOL is_unicode;
    int interrupt_counter;
    void* opaque;
    uint64_t exec_steps;
    uint64_t step_limit;

    StackElem* stack_buf;
    size_t stack_size;
    StackElem static_stack_buf[32];
} REExecContext;

static int lre_poll_timeout(REExecContext* s)
{
    if (unlikely(--s->interrupt_counter <= 0)) {
        s->interrupt_counter = INTERRUPT_COUNTER_INIT;
        if (lre_check_timeout(s->opaque))
            return LRE_RET_TIMEOUT;
        s->exec_steps += INTERRUPT_COUNTER_INIT;
        if (s->step_limit != 0 && s->exec_steps > s->step_limit)
            return LRE_RET_TIMEOUT;
    }
    return 0;
}

static no_inline int stack_realloc(REExecContext* s, size_t n)
{
    StackElem* new_stack;
    size_t new_size;
#ifndef RE_STACK_GROW_SMALL
#define RE_STACK_GROW_SMALL 512
#endif
#ifndef RE_STACK_GROW_LARGE
#define RE_STACK_GROW_LARGE (1u << 20)
#endif
    if (s->stack_size < RE_STACK_GROW_SMALL)
        new_size = s->stack_size * 8;
    else if (s->stack_size < RE_STACK_GROW_LARGE)
        new_size = s->stack_size * 2;
    else
        new_size = s->stack_size + (s->stack_size >> 2);
    if (new_size < n)
        new_size = n;
    {
        size_t max_size = LRE_BACKTRACK_MAX_STACK_SIZE / sizeof(StackElem);
        if (new_size > max_size)
            new_size = max_size;
        if (new_size < n)
            return -1;
    }
    if (s->stack_buf == s->static_stack_buf) {
        new_stack = lre_realloc(s->opaque, NULL, new_size * sizeof(StackElem));
        if (!new_stack)
            return -1;
        memcpy(new_stack, s->stack_buf, s->stack_size * sizeof(StackElem));
    } else {
        new_stack = lre_realloc(s->opaque, s->stack_buf, new_size * sizeof(StackElem));
        if (!new_stack)
            return -1;
    }
    s->stack_size = new_size;
    s->stack_buf = new_stack;
    return 0;
}

static intptr_t lre_exec_backtrack(REExecContext* s, uint8_t** capture,
    const uint8_t* pc, const uint8_t* cptr)
{
    int opcode;
    int cbuf_type;
    uint32_t val, c, idx;
    const uint8_t* cbuf_end;
    StackElem *sp, *bp, *stack_end;
#ifdef DUMP_EXEC
    const uint8_t* pc_start = pc;
#endif
    cbuf_type = s->cbuf_type;
    cbuf_end = s->cbuf_end;

    sp = s->stack_buf;
    bp = s->stack_buf;
    stack_end = s->stack_buf + s->stack_size;

#define CHECK_STACK_SPACE(n)                           \
    if (unlikely((stack_end - sp) < (n))) {            \
        size_t saved_sp = sp - s->stack_buf;           \
        size_t saved_bp = bp - s->stack_buf;           \
        if (stack_realloc(s, sp - s->stack_buf + (n))) \
            return LRE_RET_MEMORY_ERROR;               \
        stack_end = s->stack_buf + s->stack_size;      \
        sp = s->stack_buf + saved_sp;                  \
        bp = s->stack_buf + saved_bp;                  \
    }

#define SAVE_CAPTURE(idx, value)  \
    {                             \
        CHECK_STACK_SPACE(2);     \
        sp[0].val = idx;          \
        sp[1].ptr = capture[idx]; \
        sp += 2;                  \
        capture[idx] = (value);   \
    }

#define SAVE_CAPTURE_CHECK(idx, value)         \
    {                                          \
        if (sp == bp || sp[-2].val != (idx)) { \
            CHECK_STACK_SPACE(2);              \
            sp[0].val = (idx);                 \
            sp[1].ptr = capture[idx];          \
            sp += 2;                           \
        }                                      \
        capture[idx] = (value);                \
    }

#ifdef DUMP_EXEC
    printf("%5s %5s %5s %5s %s\n", "PC", "CP", "BP", "SP", "OPCODE");
#endif
    for (;;) {
        opcode = *pc++;
#ifdef DUMP_EXEC
        printf("%5ld %5ld %5ld %5ld %s\n",
            pc - 1 - pc_start,
            cbuf_type == 0 ? cptr - s->cbuf : (cptr - s->cbuf) / 2,
            bp - s->stack_buf,
            sp - s->stack_buf,
            reopcode_info[opcode].name);
#endif
        switch (opcode) {
        case REOP_match:
            return 1;
        no_match:
            for (;;) {
                REExecStateEnum type;
                if (bp == s->stack_buf)
                    return 0;
                while (sp > bp) {
                    capture[sp[-2].val] = sp[-1].ptr;
                    sp -= 2;
                }

                pc = sp[-3].ptr;
                cptr = sp[-2].ptr;
                type = sp[-1].bp.type;
                bp = s->stack_buf + sp[-1].bp.val;
                sp -= 3;
                if (type != RE_EXEC_STATE_LOOKAHEAD)
                    break;
            }
            if (lre_poll_timeout(s))
                return LRE_RET_TIMEOUT;
            break;
        case REOP_lookahead_match: {
            StackElem *sp1, *sp_top, *next_sp;
            REExecStateEnum type;

            sp_top = sp;
            for (;;) {
                sp1 = sp;
                sp = bp;
                pc = sp[-3].ptr;
                cptr = sp[-2].ptr;
                type = sp[-1].bp.type;
                bp = s->stack_buf + sp[-1].bp.val;
                sp[-1].ptr = (void*)sp1;
                sp -= 3;
                if (type == RE_EXEC_STATE_LOOKAHEAD)
                    break;
            }
            if (sp != s->stack_buf) {
                sp1 = sp;
                while (sp1 < sp_top) {
                    next_sp = (void*)sp1[2].ptr;
                    sp1 += 3;
                    while (sp1 < next_sp)
                        *sp++ = *sp1++;
                }
            }
        } break;
        case REOP_negative_lookahead_match:
            for (;;) {
                REExecStateEnum type;
                type = bp[-1].bp.type;
                while (sp > bp) {
                    capture[sp[-2].val] = sp[-1].ptr;
                    sp -= 2;
                }
                pc = sp[-3].ptr;
                cptr = sp[-2].ptr;
                type = sp[-1].bp.type;
                bp = s->stack_buf + sp[-1].bp.val;
                sp -= 3;
                if (type == RE_EXEC_STATE_NEGATIVE_LOOKAHEAD)
                    break;
            }
            goto no_match;
        case REOP_char32:
        case REOP_char32_i:
            val = get_u32(pc);
            pc += 4;
            goto test_char;
        case REOP_char:
        case REOP_char_i:
            val = get_u16(pc);
            pc += 2;
        test_char:
            if (cptr >= cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (opcode == REOP_char_i || opcode == REOP_char32_i) {
                c = lre_canonicalize(c, s->is_unicode);
            }
            if (val != c)
                goto no_match;
            break;
        case REOP_split_goto_first:
        case REOP_split_next_first: {
            const uint8_t* pc1;

            val = get_u32(pc);
            pc += 4;
            if (opcode == REOP_split_next_first) {
                pc1 = pc + (int)val;
            } else {
                pc1 = pc;
                pc = pc + (int)val;
            }
            CHECK_STACK_SPACE(3);
            sp[0].ptr = DYN_UNCONST(pc1);
            sp[1].ptr = DYN_UNCONST(cptr);
            sp[2].bp.val = bp - s->stack_buf;
            sp[2].bp.type = RE_EXEC_STATE_SPLIT;
            sp += 3;
            bp = sp;
        } break;
        case REOP_lookahead:
        case REOP_negative_lookahead:
            val = get_u32(pc);
            pc += 4;
            CHECK_STACK_SPACE(3);
            sp[0].ptr = DYN_UNCONST(pc + (int)val);
            sp[1].ptr = DYN_UNCONST(cptr);
            sp[2].bp.val = bp - s->stack_buf;
            sp[2].bp.type = RE_EXEC_STATE_LOOKAHEAD + opcode - REOP_lookahead;
            sp += 3;
            bp = sp;
            break;
        case REOP_goto:
            val = get_u32(pc);
            pc += 4 + (int)val;
            if (lre_poll_timeout(s))
                return LRE_RET_TIMEOUT;
            break;
        case REOP_line_start:
        case REOP_line_start_m:
            if (cptr == s->cbuf)
                break;
            if (opcode == REOP_line_start)
                goto no_match;
            PEEK_PREV_CHAR(c, cptr, s->cbuf, cbuf_type);
            if (!is_line_terminator(c))
                goto no_match;
            break;
        case REOP_line_end:
        case REOP_line_end_m:
            if (cptr == cbuf_end)
                break;
            if (opcode == REOP_line_end)
                goto no_match;
            PEEK_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (!is_line_terminator(c))
                goto no_match;
            break;
        case REOP_dot:
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (is_line_terminator(c))
                goto no_match;
            break;
        case REOP_any:
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            break;
        case REOP_space:
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (!lre_is_space(c))
                goto no_match;
            break;
        case REOP_not_space:
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (lre_is_space(c))
                goto no_match;
            break;
        case REOP_save_start:
        case REOP_save_end:
            val = *pc++;
            assert(val < s->capture_count);
            idx = 2 * val + opcode - REOP_save_start;
            SAVE_CAPTURE(idx, DYN_UNCONST(cptr));
            break;
        case REOP_save_reset: {
            uint32_t val2;
            val = pc[0];
            val2 = pc[1];
            pc += 2;
            assert(val2 < s->capture_count);
            CHECK_STACK_SPACE(2 * (val2 - val + 1));
            while (val <= val2) {
                idx = 2 * val;
                SAVE_CAPTURE(idx, NULL);
                idx = 2 * val + 1;
                SAVE_CAPTURE(idx, NULL);
                val++;
            }
        } break;
        case REOP_set_i32:
            idx = 2 * s->capture_count + pc[0];
            val = get_u32(pc + 1);
            pc += 5;
            SAVE_CAPTURE_CHECK(idx, (void*)(uintptr_t)val);
            break;
        case REOP_loop: {
            uint32_t val2;
            idx = 2 * s->capture_count + pc[0];
            val = get_u32(pc + 1);
            pc += 5;

            val2 = (uintptr_t)capture[idx] - 1;
            SAVE_CAPTURE_CHECK(idx, (void*)(uintptr_t)val2);
            if (val2 != 0) {
                pc += (int)val;
                if (lre_poll_timeout(s))
                    return LRE_RET_TIMEOUT;
            }
        } break;
        case REOP_loop_split_goto_first:
        case REOP_loop_split_next_first:
        case REOP_loop_check_adv_split_goto_first:
        case REOP_loop_check_adv_split_next_first: {
            const uint8_t* pc1;
            uint32_t val2, limit;
            idx = 2 * s->capture_count + pc[0];
            limit = get_u32(pc + 1);
            val = get_u32(pc + 5);
            pc += 9;

            val2 = (uintptr_t)capture[idx] - 1;
            SAVE_CAPTURE_CHECK(idx, (void*)(uintptr_t)val2);

            if (val2 > limit) {
                pc += (int)val;
                if (lre_poll_timeout(s))
                    return LRE_RET_TIMEOUT;
            } else {
                if ((opcode == REOP_loop_check_adv_split_goto_first || opcode == REOP_loop_check_adv_split_next_first) && capture[idx + 1] == cptr && val2 != limit) {
                    goto no_match;
                }

                if (val2 != 0) {
                    if (opcode == REOP_loop_split_next_first || opcode == REOP_loop_check_adv_split_next_first) {
                        pc1 = pc + (int)val;
                    } else {
                        pc1 = pc;
                        pc = pc + (int)val;
                    }
                    CHECK_STACK_SPACE(3);
                    sp[0].ptr = DYN_UNCONST(pc1);
                    sp[1].ptr = DYN_UNCONST(cptr);
                    sp[2].bp.val = bp - s->stack_buf;
                    sp[2].bp.type = RE_EXEC_STATE_SPLIT;
                    sp += 3;
                    bp = sp;
                }
            }
        } break;
        case REOP_set_char_pos:
            idx = 2 * s->capture_count + pc[0];
            pc++;
            SAVE_CAPTURE_CHECK(idx, DYN_UNCONST(cptr));
            break;
        case REOP_check_advance:
            idx = 2 * s->capture_count + pc[0];
            pc++;
            if (capture[idx] == cptr)
                goto no_match;
            break;
        case REOP_word_boundary:
        case REOP_word_boundary_i:
        case REOP_not_word_boundary:
        case REOP_not_word_boundary_i: {
            BOOL v1, v2;
            int ignore_case = (opcode == REOP_word_boundary_i || opcode == REOP_not_word_boundary_i);
            BOOL is_boundary = (opcode == REOP_word_boundary || opcode == REOP_word_boundary_i);
            if (cptr == s->cbuf) {
                v1 = FALSE;
            } else {
                PEEK_PREV_CHAR(c, cptr, s->cbuf, cbuf_type);
                if (c < 256) {
                    v1 = (lre_is_word_byte(c) != 0);
                } else {
                    v1 = ignore_case && (c == 0x017f || c == 0x212a);
                }
            }
            if (cptr >= cbuf_end) {
                v2 = FALSE;
            } else {
                PEEK_CHAR(c, cptr, cbuf_end, cbuf_type);
                if (c < 256) {
                    v2 = (lre_is_word_byte(c) != 0);
                } else {
                    v2 = ignore_case && (c == 0x017f || c == 0x212a);
                }
            }
            if (v1 ^ v2 ^ is_boundary)
                goto no_match;
        } break;
        case REOP_back_reference:
#ifndef RE_BACKREF_MEMCMP_MIN
#define RE_BACKREF_MEMCMP_MIN 16
#endif
#if !defined(CONFIG_REGEXP_BACKREF_MEMCMP) || CONFIG_REGEXP_BACKREF_MEMCMP
            if (cbuf_type != 2) {
                const uint8_t *b_start, *b_end, *b_pc = pc;
                int b_i, b_n = *pc++;
                pc += b_n;
                for (b_i = 0; b_i < b_n; b_i++) {
                    uint32_t b_val = b_pc[1 + b_i];
                    if (b_val >= s->capture_count)
                        goto no_match;
                    b_start = capture[2 * b_val];
                    b_end = capture[2 * b_val + 1];
                    if (b_start && b_end) {
                        size_t b_len = b_end - b_start;
                        if (b_len > (size_t)(cbuf_end - cptr))
                            goto no_match;
                        if (b_len < RE_BACKREF_MEMCMP_MIN) {
                            size_t b_k;
                            for (b_k = 0; b_k < b_len; b_k++)
                                if (cptr[b_k] != b_start[b_k])
                                    goto no_match;
                        } else if (memcmp(cptr, b_start, b_len) != 0) {
                            goto no_match;
                        }
                        cptr += b_len;
                        break;
                    }
                }
                break;
            }
            goto backref_general;
#endif
        case REOP_back_reference_i:
        case REOP_backward_back_reference:
        case REOP_backward_back_reference_i:
        backref_general:
            {
                const uint8_t *cptr1, *cptr1_end, *cptr1_start;
                const uint8_t* pc1;
                uint32_t c1, c2;
                int i, n;

                n = *pc++;
                pc1 = pc;
                pc += n;

                for (i = 0; i < n; i++) {
                    val = pc1[i];
                    if (val >= s->capture_count)
                        goto no_match;
                    cptr1_start = capture[2 * val];
                    cptr1_end = capture[2 * val + 1];
                    if (cptr1_start && cptr1_end) {
                        if (opcode == REOP_back_reference || opcode == REOP_back_reference_i) {
                            cptr1 = cptr1_start;
                            while (cptr1 < cptr1_end) {
                                if (cptr >= cbuf_end)
                                    goto no_match;
                                GET_CHAR(c1, cptr1, cptr1_end, cbuf_type);
                                GET_CHAR(c2, cptr, cbuf_end, cbuf_type);
                                if (opcode == REOP_back_reference_i) {
                                    c1 = lre_canonicalize(c1, s->is_unicode);
                                    c2 = lre_canonicalize(c2, s->is_unicode);
                                }
                                if (c1 != c2)
                                    goto no_match;
                            }
                        } else {
                            cptr1 = cptr1_end;
                            while (cptr1 > cptr1_start) {
                                if (cptr == s->cbuf)
                                    goto no_match;
                                GET_PREV_CHAR(c1, cptr1, cptr1_start, cbuf_type);
                                GET_PREV_CHAR(c2, cptr, s->cbuf, cbuf_type);
                                if (opcode == REOP_backward_back_reference_i) {
                                    c1 = lre_canonicalize(c1, s->is_unicode);
                                    c2 = lre_canonicalize(c2, s->is_unicode);
                                }
                                if (c1 != c2)
                                    goto no_match;
                            }
                        }
                        break;
                    }
                }
            }
            break;
        case REOP_range:
        case REOP_range_i: {
            int n;
            uint32_t low, high, idx_min, idx_max, ridx;

            n = get_u16(pc);
            pc += 2;
            if (cptr >= cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (opcode == REOP_range_i) {
                c = lre_canonicalize(c, s->is_unicode);
            }
            idx_min = 0;
            low = get_u16(pc + 0 * 4);
            if (c < low)
                goto no_match;
            idx_max = n - 1;
            high = get_u16(pc + idx_max * 4 + 2);
            if (unlikely(c >= 0xffff) && high == 0xffff)
                goto range_match;
            if (c > high)
                goto no_match;
            if (n == 1)
                goto range_match;
            while (idx_min <= idx_max) {
                ridx = (idx_min + idx_max) / 2;
                low = get_u16(pc + ridx * 4);
                high = get_u16(pc + ridx * 4 + 2);
                if (c < low)
                    idx_max = ridx - 1;
                else if (c > high)
                    idx_min = ridx + 1;
                else
                    goto range_match;
            }
            goto no_match;
        range_match:
            pc += 4 * n;
        } break;
        case REOP_range32:
        case REOP_range32_i: {
            int n;
            uint32_t low, high, idx_min, idx_max, ridx;

            n = get_u16(pc);
            pc += 2;
            if (cptr >= cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (opcode == REOP_range32_i) {
                c = lre_canonicalize(c, s->is_unicode);
            }
            idx_min = 0;
            low = get_u32(pc + 0 * 8);
            if (c < low)
                goto no_match;
            idx_max = n - 1;
            high = get_u32(pc + idx_max * 8 + 4);
            if (c > high)
                goto no_match;
            if (n == 1)
                goto range32_match;
            while (idx_min <= idx_max) {
                ridx = (idx_min + idx_max) / 2;
                low = get_u32(pc + ridx * 8);
                high = get_u32(pc + ridx * 8 + 4);
                if (c < low)
                    idx_max = ridx - 1;
                else if (c > high)
                    idx_min = ridx + 1;
                else
                    goto range32_match;
            }
            goto no_match;
        range32_match:
            pc += 8 * n;
        } break;
        case REOP_prev:
            if (cptr == s->cbuf)
                goto no_match;
            PREV_CHAR(cptr, s->cbuf, cbuf_type);
            break;
        default:
#ifdef DUMP_EXEC
            printf("unknown opcode pc=%ld\n", pc - 1 - pc_start);
#endif
            abort();
        }
    }
}

#if CONFIG_RE_PREFILTER

#define RE_PF_PROLOGUE_LEN 13
#define RE_PF_SAVE_START 11
#define RE_PF_MAX_LIT 16
#define RE_PF_MIN_LEN 32
#ifndef RE_PF_SIMD_MIN
#define RE_PF_SIMD_MIN 64
#endif

enum { RE_PF_NONE = 0,
    RE_PF_CHAR,
    RE_PF_LIT,
    RE_PF_SET,
    RE_PF_LIT16,
    RE_PF_BITMAP16,
    RE_PF_BITMAP,
    RE_PF_BOL,
    RE_PF_LINESTART };

typedef struct {
    int kind;
    uint32_t ch;
    uint8_t lit[RE_PF_MAX_LIT];
    uint16_t lit16[RE_PF_MAX_LIT];
    size_t lit_len;
    uint8_t set[8];
    size_t set_len;
    uint8_t bitmap[32];
} REPrefilter;

static BOOL re_pf_is_unanchored(const uint8_t* bc, size_t bc_len)
{
    return bc_len > RE_PF_PROLOGUE_LEN && bc[0] == REOP_split_goto_first && get_u32(bc + 1) == 1 + 5 && bc[5] == REOP_any && bc[6] == REOP_goto && (int32_t)get_u32(bc + 7) == -(5 + 1 + 5) && bc[11] == REOP_save_start && bc[12] == 0;
}

static BOOL re_pf_range_to_set(REPrefilter* pf, const uint8_t* p, int n)
{
    size_t k = 0;
    int i;
    uint32_t v;

    for (i = 0; i < n; i++) {
        uint32_t low = get_u16(p + i * 4);
        uint32_t high = get_u16(p + i * 4 + 2);
        if (low > 0xff)
            continue;
        if (high > 0xff)
            high = 0xff;
        for (v = low; v <= high; v++) {
            if (k >= countof(pf->set))
                return FALSE;
            pf->set[k++] = (uint8_t)v;
        }
    }
    if (k == 0)
        return FALSE;
    pf->set_len = k;
    return TRUE;
}

static BOOL re_pf_range_to_bitmap(REPrefilter* pf, const uint8_t* p, int n)
{
    int i;
    uint32_t v, count = 0;

    for (i = 0; i < n; i++) {
        uint32_t low = get_u16(p + i * 4);
        uint32_t high = get_u16(p + i * 4 + 2);
        if (low > 0xff)
            continue;
        if (high > 0xff)
            high = 0xff;
        count += high - low + 1;
        if (count > 192)
            return FALSE;
    }
    if (count == 0)
        return FALSE;

    memset(pf->bitmap, 0, sizeof(pf->bitmap));
    for (i = 0; i < n; i++) {
        uint32_t low = get_u16(p + i * 4);
        uint32_t high = get_u16(p + i * 4 + 2);
        uint32_t lb, hb;
        if (low > 0xff)
            continue;
        if (high > 0xff)
            high = 0xff;
        lb = low >> 3;
        hb = high >> 3;
        if (lb == hb) {
            pf->bitmap[lb] |= (uint8_t)((0xffu << (low & 7)) & (0xffu >> (7 - (high & 7))));
            continue;
        }
        pf->bitmap[lb] |= (uint8_t)(0xffu << (low & 7));
        if (hb > lb + 1)
            memset(pf->bitmap + lb + 1, 0xff, hb - lb - 1);
        pf->bitmap[hb] |= (uint8_t)(0xffu >> (7 - (high & 7)));
    }
    (void)v;
    return TRUE;
}

static BOOL re_pf_first_bytes(REPrefilter* pf, const uint8_t* bc, size_t bc_len,
    size_t pos, int depth)
{
    int steps = 0;

    if (depth > 8)
        return FALSE;
    for (;;) {
        int op;
        if (pos >= bc_len || ++steps > 64)
            return FALSE;
        op = bc[pos];
        if (op >= REOP_COUNT)
            return FALSE;
        switch (op) {
        case REOP_save_start:
        case REOP_save_end:
            pos += 2;
            continue;
        case REOP_save_reset:
            pos += 3;
            continue;
        case REOP_goto:
            if (pos + 5 > bc_len)
                return FALSE;
            pos = (size_t)((int64_t)pos + 5 + (int32_t)get_u32(bc + pos + 1));
            continue;
        case REOP_char: {
            uint32_t c;
            if (pos + 3 > bc_len)
                return FALSE;
            c = get_u16(bc + pos + 1);
            if (c <= 0xff)
                pf->bitmap[c >> 3] |= (uint8_t)(1u << (c & 7));
            return TRUE;
        }
        case REOP_char32:
            return pos + 5 <= bc_len;
        case REOP_range: {
            int n, i;
            if (pos + 3 > bc_len)
                return FALSE;
            n = get_u16(bc + pos + 1);
            if (n < 1 || pos + 3 + (size_t)n * 4 > bc_len)
                return FALSE;
            for (i = 0; i < n; i++) {
                uint32_t lo = get_u16(bc + pos + 3 + i * 4);
                uint32_t hi = get_u16(bc + pos + 3 + i * 4 + 2);
                uint32_t v;
                if (lo > 0xff)
                    continue;
                if (hi > 0xff)
                    hi = 0xff;
                for (v = lo; v <= hi; v++)
                    pf->bitmap[v >> 3] |= (uint8_t)(1u << (v & 7));
            }
            return TRUE;
        }
        case REOP_split_next_first:
        case REOP_split_goto_first: {
            size_t a, b;
            if (pos + 5 > bc_len)
                return FALSE;
            a = pos + 5;
            b = (size_t)((int64_t)pos + 5 + (int32_t)get_u32(bc + pos + 1));
            return re_pf_first_bytes(pf, bc, bc_len, a, depth + 1) && re_pf_first_bytes(pf, bc, bc_len, b, depth + 1);
        }
        default:
            return FALSE;
        }
    }
}

#ifndef RE_PF_CASE_MIN
#define RE_PF_CASE_MIN 256
#endif

static no_inline BOOL re_pf_fold_char_to_bitmap(REPrefilter* pf, uint32_t val,
    BOOL is_unicode, uint32_t* pcount)
{
    uint32_t b, count = 0;
    memset(pf->bitmap, 0, sizeof(pf->bitmap));
    for (b = 0; b < 256; b++) {
        if (lre_canonicalize(b, is_unicode) == val) {
            pf->bitmap[b >> 3] |= (uint8_t)(1u << (b & 7));
            count++;
        }
    }
    *pcount = count;
    return TRUE;
}

static BOOL re_pf_ranges_contain(const uint8_t* p, int n, uint32_t c)
{
    int i;
    for (i = 0; i < n; i++) {
        uint32_t low = get_u16(p + i * 4);
        uint32_t high = get_u16(p + i * 4 + 2);
        if (c >= low && c <= high)
            return TRUE;
    }
    return FALSE;
}

static no_inline BOOL re_pf_fold_range_to_bitmap(REPrefilter* pf, const uint8_t* p, int n,
    BOOL is_unicode, uint32_t* pcount)
{
    uint32_t b, count = 0;
    memset(pf->bitmap, 0, sizeof(pf->bitmap));
    for (b = 0; b < 256; b++) {
        if (re_pf_ranges_contain(p, n, lre_canonicalize(b, is_unicode))) {
            pf->bitmap[b >> 3] |= (uint8_t)(1u << (b & 7));
            count++;
        }
    }
    *pcount = count;
    return TRUE;
}

static no_inline void re_pf_bitmap_to_best_kind(REPrefilter* pf, uint32_t count)
{
    if (count == 0) {
        pf->kind = RE_PF_CHAR;
        pf->ch = 0x100;
    } else if (count <= countof(pf->set)) {
        uint32_t b;
        size_t k = 0;
        for (b = 0; b < 256; b++)
            if (pf->bitmap[b >> 3] & (1u << (b & 7)))
                pf->set[k++] = (uint8_t)b;
        pf->set_len = k;
        pf->kind = RE_PF_SET;
    } else if (count <= 192) {
        pf->kind = RE_PF_BITMAP;
    }
}

#define RE_PF_FOLD_KEY_MAX 64
#define RE_PF_FOLD_CACHE 8

typedef struct {
    uint32_t hash;
    uint16_t key_len;
    uint8_t is_unicode;
    int kind;
    uint32_t ch;
    size_t set_len;
    uint8_t key[RE_PF_FOLD_KEY_MAX];
    uint8_t set[8];
    uint8_t bitmap[32];
} REPfFoldEntry;

static _Thread_local REPfFoldEntry re_pf_fold_cache[RE_PF_FOLD_CACHE];

static uint32_t re_pf_fold_hash(const uint8_t* key, size_t len, BOOL is_unicode)
{
    uint32_t h = 2166136261u ^ (uint32_t)(is_unicode != 0);
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= key[i];
        h *= 16777619u;
    }
    h ^= h >> 15;
    h *= 0x2c1b3c6dU;
    h ^= h >> 12;
    h *= 0x297a2d39U;
    h ^= h >> 15;
    return h;
}

static BOOL re_pf_fold_cache_get(REPrefilter* pf, const uint8_t* key, size_t len,
    BOOL is_unicode, uint32_t hash)
{
    const REPfFoldEntry* e = &re_pf_fold_cache[hash & (RE_PF_FOLD_CACHE - 1)];

    if (e->key_len != (uint16_t)len || e->hash != hash || e->is_unicode != (uint8_t)(is_unicode != 0) || memcmp(e->key, key, len) != 0)
        return FALSE;
    pf->kind = e->kind;
    pf->ch = e->ch;
    pf->set_len = e->set_len;
    memcpy(pf->set, e->set, sizeof(pf->set));
    memcpy(pf->bitmap, e->bitmap, sizeof(pf->bitmap));
    return TRUE;
}

static no_inline void re_pf_fold_cache_put(const REPrefilter* pf, const uint8_t* key,
    size_t len, BOOL is_unicode, uint32_t hash)
{
    REPfFoldEntry* e = &re_pf_fold_cache[hash & (RE_PF_FOLD_CACHE - 1)];

    e->hash = hash;
    e->key_len = (uint16_t)len;
    e->is_unicode = (uint8_t)(is_unicode != 0);
    e->kind = pf->kind;
    e->ch = pf->ch;
    e->set_len = pf->set_len;
    memcpy(e->key, key, len);
    memcpy(e->set, pf->set, sizeof(e->set));
    memcpy(e->bitmap, pf->bitmap, sizeof(e->bitmap));
}

static no_inline void re_pf_build(REPrefilter* pf, const uint8_t* bc, size_t bc_len,
    int cbuf_type, BOOL is_unicode, size_t subject_len)
{
    const uint8_t* p;
    int op;

    pf->kind = RE_PF_NONE;
    if (!re_pf_is_unanchored(bc, bc_len))
        return;

    p = bc + RE_PF_PROLOGUE_LEN;
    {
        int guard = 8;
        while (guard-- > 0 && (size_t)(p - bc) < bc_len) {
            if (*p == REOP_save_start || *p == REOP_save_end)
                p += 2;
            else if (*p == REOP_save_reset)
                p += 3;
            else if (*p == REOP_set_i32)
                p += 6;
            else if (*p == REOP_set_char_pos || *p == REOP_check_advance)
                p += 2;
            else if (*p == REOP_word_boundary || *p == REOP_word_boundary_i || *p == REOP_not_word_boundary || *p == REOP_not_word_boundary_i)
                p += 1;
            else
                break;
        }
        if ((size_t)(p - bc) >= bc_len)
            return;
    }
    op = *p;

    if (op == REOP_line_start) {
        pf->kind = RE_PF_BOL;
        return;
    }
    if (op == REOP_line_start_m) {
        pf->kind = RE_PF_LINESTART;
        return;
    }

    if ((op == REOP_char_i || op == REOP_range_i) && cbuf_type == 0) {
        uint32_t count, hash = 0;
        size_t klen;
        int n = 0;

        if (op == REOP_char_i) {
            klen = 3;
        } else {
            n = get_u16(p + 1);
            if (n < 1 || (size_t)(p - bc) + 3 + (size_t)n * 4 > bc_len)
                return;
            klen = 3 + (size_t)n * 4;
        }
        pf->ch = 0;
        pf->set_len = 0;

        if (subject_len < RE_PF_CASE_MIN)
            return;

        if (klen <= RE_PF_FOLD_KEY_MAX) {
            hash = re_pf_fold_hash(p, klen, is_unicode);
            if (re_pf_fold_cache_get(pf, p, klen, is_unicode, hash))
                return;
        }

        if (op == REOP_char_i)
            re_pf_fold_char_to_bitmap(pf, get_u16(p + 1), is_unicode, &count);
        else
            re_pf_fold_range_to_bitmap(pf, p + 3, n, is_unicode, &count);
        re_pf_bitmap_to_best_kind(pf, count);

        if (klen <= RE_PF_FOLD_KEY_MAX)
            re_pf_fold_cache_put(pf, p, klen, is_unicode, hash);
        return;
    }

    if (op == REOP_space && cbuf_type == 0) {
        uint32_t b;
        size_t k = 0;
        for (b = 0; b < 256 && k <= countof(pf->set); b++)
            if (lre_is_space(b)) {
                if (k < countof(pf->set))
                    pf->set[k] = (uint8_t)b;
                k++;
            }
        if (k > 0 && k <= countof(pf->set)) {
            pf->set_len = k;
            pf->kind = RE_PF_SET;
        }
        return;
    }

    if (op == REOP_char) {
        uint32_t c = get_u16(p + 1);

        if (cbuf_type == 0) {
            if (c > 0xff) {
                pf->kind = RE_PF_CHAR;
                pf->ch = c;
                return;
            }
            pf->lit[0] = (uint8_t)c;
            pf->lit_len = 1;
            p += 3;
            while (pf->lit_len < RE_PF_MAX_LIT && (size_t)(p - bc) + 3 <= bc_len && *p == REOP_char) {
                uint32_t c2 = get_u16(p + 1);
                if (c2 > 0xff)
                    break;
                pf->lit[pf->lit_len++] = (uint8_t)c2;
                p += 3;
            }
            pf->kind = pf->lit_len > 1 ? RE_PF_LIT : RE_PF_CHAR;
            pf->ch = pf->lit[0];
        } else {
            if (is_hi_surrogate(c) || is_lo_surrogate(c))
                return;
            pf->lit16[0] = (uint16_t)c;
            pf->lit_len = 1;
            p += 3;
            while (pf->lit_len < RE_PF_MAX_LIT && (size_t)(p - bc) + 3 <= bc_len && *p == REOP_char) {
                uint32_t c2 = get_u16(p + 1);
                if (is_hi_surrogate(c2) || is_lo_surrogate(c2))
                    break;
                pf->lit16[pf->lit_len++] = (uint16_t)c2;
                p += 3;
            }
            pf->kind = pf->lit_len > 1 ? RE_PF_LIT16 : RE_PF_CHAR;
            pf->ch = pf->lit16[0];
        }
        return;
    }

    if (op == REOP_char32 && cbuf_type == 0) {
        pf->kind = RE_PF_CHAR;
        pf->ch = get_u32(p + 1);
        return;
    }

    if ((op == REOP_split_next_first || op == REOP_split_goto_first) && cbuf_type == 0) {
        uint32_t count = 0, i, b;
        memset(pf->bitmap, 0, sizeof(pf->bitmap));
        if (re_pf_first_bytes(pf, bc, bc_len, (size_t)(p - bc), 0)) {
            for (i = 0; i < sizeof(pf->bitmap); i++)
                for (b = 0; b < 8; b++)
                    count += (pf->bitmap[i] >> b) & 1;
            if (count > 0 && count <= 192)
                pf->kind = RE_PF_BITMAP;
        }
        return;
    }

    if (op == REOP_range) {
        int n = get_u16(p + 1);
        if (n >= 1 && (size_t)(p - bc) + 3 + (size_t)n * 4 <= bc_len) {
            if (cbuf_type == 0) {
                if (re_pf_range_to_set(pf, p + 3, n))
                    pf->kind = RE_PF_SET;
                else if (re_pf_range_to_bitmap(pf, p + 3, n))
                    pf->kind = RE_PF_BITMAP;
            } else {
                if (re_pf_range_to_bitmap(pf, p + 3, n))
                    pf->kind = RE_PF_BITMAP16;
            }
        }
        return;
    }
}

static no_inline const uint8_t* re_pf_scan(const REPrefilter* pf, const uint8_t* cbuf,
    const uint8_t* cptr,
    const uint8_t* cbuf_end, int cbuf_type)
{
    size_t r;

    if (pf->kind == RE_PF_BOL)
        return cptr == cbuf ? cptr : NULL;
    if (pf->kind == RE_PF_LINESTART) {
        if (cptr == cbuf)
            return cptr;
        if (cbuf_type == 0) {
            static const uint8_t terms[2] = { '\n', '\r' };
            size_t n = (size_t)(cbuf_end - (cptr - 1));
            r = simd.find_first_of(cptr - 1, n, terms, 2);
            if (r == SIZE_MAX)
                return NULL;
            return (cptr - 1) + r + 1 <= cbuf_end ? (cptr - 1) + r + 1 : NULL;
        } else {
            static const uint16_t terms16[4] = { '\n', '\r', 0x2028, 0x2029 };
            const uint16_t* q = (const uint16_t*)(cptr)-1;
            size_t n16 = (size_t)((const uint16_t*)cbuf_end - q);
            if (simd.find_first_of_u16) {
                r = simd.find_first_of_u16(q, n16, terms16, 4);
                if (r == SIZE_MAX)
                    return NULL;
                return (const uint8_t*)(q + r + 1);
            }
            while (q < (const uint16_t*)cbuf_end) {
                uint16_t u = *q;
                if (u == '\n' || u == '\r' || u == 0x2028 || u == 0x2029)
                    return (const uint8_t*)(q + 1);
                q++;
            }
            return NULL;
        }
    }

    if (cbuf_type == 0) {
        size_t n = (size_t)(cbuf_end - cptr);

        switch (pf->kind) {
        case RE_PF_CHAR:
            if (pf->ch > 0xff)
                return NULL;
            r = simd.find_u8(cptr, (uint8_t)pf->ch, n);
            break;
        case RE_PF_LIT:
            r = simd.strfind(cptr, n, pf->lit, pf->lit_len);
            break;
        case RE_PF_SET:
            r = simd.find_first_of(cptr, n, pf->set, pf->set_len);
            break;
        case RE_PF_BITMAP: {
            size_t probe = n < RE_PF_SIMD_MIN ? n : RE_PF_SIMD_MIN;
            const uint8_t *q = cptr, *e = cptr + probe;
            while (q < e && !(pf->bitmap[*q >> 3] & (1u << (*q & 7))))
                q++;
            if (q != e)
                return q;
            if (probe == n)
                return NULL;
            r = simd.find_bitmap(cptr + probe, n - probe, pf->bitmap);
            return r == SIZE_MAX ? NULL : cptr + probe + r;
        }
        default:
            return cptr;
        }
        return r == SIZE_MAX ? NULL : cptr + r;
    } else {
        const uint16_t* p = (const uint16_t*)cptr;
        size_t n = (size_t)((const uint16_t*)cbuf_end - p);

        if (pf->kind == RE_PF_LIT16) {
            size_t rem = n;
            for (;;) {
                if (rem < pf->lit_len)
                    return NULL;
                r = simd.find_u16(p, pf->lit16[0], rem - pf->lit_len + 1);
                if (r == SIZE_MAX)
                    return NULL;
                p += r;
                rem -= r;
                if (!memcmp(p, pf->lit16, pf->lit_len * sizeof(uint16_t)))
                    return (const uint8_t*)p;
                p++;
                rem--;
            }
        }
        if (pf->kind == RE_PF_BITMAP16) {
            size_t i;
            for (i = 0; i < n; i++) {
                uint16_t u = p[i];
                if (u <= 0xff && (pf->bitmap[u >> 3] & (1u << (u & 7))))
                    return (const uint8_t*)(p + i);
            }
            return NULL;
        }
        if (pf->kind != RE_PF_CHAR)
            return cptr;
        r = simd.find_u16(p, (uint16_t)pf->ch, n);
        return r == SIZE_MAX ? NULL : (const uint8_t*)(p + r);
    }
}
#endif

static uint64_t lre_default_exec_steps = LRE_DEFAULT_EXEC_STEPS;

static uint64_t lre_exec_step_limit(int clen)
{
    if (lre_default_exec_steps == 0)
        return 0;
    return lre_default_exec_steps + (uint64_t)clen * LRE_EXEC_STEPS_PER_CHAR;
}

void lre_set_exec_step_limit(uint64_t steps)
{
    lre_default_exec_steps = steps;
}

int lre_exec(uint8_t** capture,
    const uint8_t* bc_buf, const uint8_t* cbuf, int cindex, int clen,
    int cbuf_type, void* opaque)
{
    REExecContext s_s, *s = &s_s;
    int re_flags, i, ret;
    const uint8_t* cptr;

    re_flags = lre_get_flags(bc_buf);
    s->is_unicode = (re_flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)) != 0;
    s->capture_count = bc_buf[RE_HEADER_CAPTURE_COUNT];
    s->cbuf = cbuf;
    s->cbuf_end = cbuf + (clen << cbuf_type);
    s->cbuf_type = cbuf_type;
    if (s->cbuf_type == 1 && s->is_unicode)
        s->cbuf_type = 2;
    s->interrupt_counter = INTERRUPT_COUNTER_INIT;
    s->opaque = opaque;
    s->exec_steps = 0;
    s->step_limit = lre_exec_step_limit(clen);

    s->stack_buf = s->static_stack_buf;
    s->stack_size = countof(s->static_stack_buf);

    for (i = 0; i < s->capture_count * 2; i++)
        capture[i] = NULL;

    cptr = cbuf + (cindex << cbuf_type);
    if (0 < cindex && cindex < clen && s->cbuf_type == 2) {
        const uint16_t* p = (const uint16_t*)cptr;
        if (is_lo_surrogate(*p) && is_hi_surrogate(p[-1])) {
            cptr = (const uint8_t*)(p - 1);
        }
    }

    ret = LRE_RET_MEMORY_ERROR;
#if CONFIG_RE_PREFILTER
    {
        const uint8_t* bc = bc_buf + RE_HEADER_LEN;
        size_t bc_len = get_u32(bc_buf + RE_HEADER_BYTECODE_LEN);
        REPrefilter pf;

        if ((size_t)(s->cbuf_end - cptr) >= (RE_PF_MIN_LEN << s->cbuf_type)) {
            simd_init();
            re_pf_build(&pf, bc, bc_len, s->cbuf_type, s->is_unicode,
                (size_t)(s->cbuf_end - cptr));
        } else {
            pf.kind = RE_PF_NONE;
        }

        if (pf.kind != RE_PF_NONE) {
            BOOL first_attempt = TRUE;
            for (;;) {
                cptr = re_pf_scan(&pf, s->cbuf, cptr, s->cbuf_end, s->cbuf_type);
                if (!cptr) {
                    ret = 0;
                    break;
                }
                if (!first_attempt) {
                    for (i = 0; i < s->capture_count * 2; i++)
                        capture[i] = NULL;
                }
                first_attempt = FALSE;

                ret = lre_exec_backtrack(s, capture,
                    bc + RE_PF_SAVE_START, cptr);
                if (ret != 0)
                    break;

                if (cptr >= s->cbuf_end) {
                    ret = 0;
                    break;
                }
                {
                    uint32_t c_dummy;
                    GET_CHAR(c_dummy, cptr, s->cbuf_end, s->cbuf_type);
                    (void)c_dummy;
                }
                if (lre_poll_timeout(s)) {
                    ret = LRE_RET_TIMEOUT;
                    break;
                }
            }
            goto done;
        }
    }
#endif
    ret = lre_exec_backtrack(s, capture, bc_buf + RE_HEADER_LEN, cptr);

#if CONFIG_RE_PREFILTER
done:
#endif
    if (s->stack_buf != s->static_stack_buf)
        lre_realloc(s->opaque, s->stack_buf, 0);
    return ret;
}

int lre_get_alloc_count(const uint8_t* bc_buf)
{
    return bc_buf[RE_HEADER_CAPTURE_COUNT] * 2 + bc_buf[RE_HEADER_REGISTER_COUNT];
}

int lre_get_capture_count(const uint8_t* bc_buf)
{
    return bc_buf[RE_HEADER_CAPTURE_COUNT];
}

int lre_get_flags(const uint8_t* bc_buf)
{
    return get_u16(bc_buf + RE_HEADER_FLAGS);
}

const char* lre_get_groupnames(const uint8_t* bc_buf)
{
    uint32_t re_bytecode_len;
    if ((lre_get_flags(bc_buf) & LRE_FLAG_NAMED_GROUPS) == 0)
        return NULL;
    re_bytecode_len = get_u32(bc_buf + RE_HEADER_BYTECODE_LEN);
    return (const char*)(bc_buf + RE_HEADER_LEN + re_bytecode_len);
}

#ifdef TEST

BOOL lre_check_stack_overflow(void* opaque, size_t alloca_size)
{
    return FALSE;
}

void* lre_realloc(void* opaque, void* ptr, size_t size)
{
    return realloc(ptr, size);
}

int main(int argc, char** argv)
{
    int len, flags, ret, i;
    uint8_t* bc;
    char error_msg[64];
    uint8_t* capture;
    const char* input;
    int input_len, capture_count;

    if (argc < 4) {
        printf("usage: %s regexp flags input\n", argv[0]);
        return 1;
    }
    flags = atoi(argv[2]);
    bc = lre_compile(&len, error_msg, sizeof(error_msg), argv[1],
        strlen(argv[1]), flags, NULL);
    if (!bc) {
        fprintf(stderr, "error: %s\n", error_msg);
        exit(1);
    }

    input = argv[3];
    input_len = strlen(input);

    capture = malloc(sizeof(capture[0]) * lre_get_alloc_count(bc));
    ret = lre_exec(capture, bc, (uint8_t*)input, 0, input_len, 0, NULL);
    printf("ret=%d\n", ret);
    if (ret == 1) {
        capture_count = lre_get_capture_count(bc);
        for (i = 0; i < 2 * capture_count; i++) {
            uint8_t* ptr;
            ptr = capture[i];
            printf("%d: ", i);
            if (!ptr)
                printf("<nil>");
            else
                printf("%u", (int)(ptr - (uint8_t*)input));
            printf("\n");
        }
    }
    free(capture);
    return 0;
}
#endif
