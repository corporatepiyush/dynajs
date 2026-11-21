#include "logk.h"
#include "../dyn-simdk.h"

#define DYN_LOG_ESC(c) (DYN_LOG_ESC_BM[(c) >> 3] & (1u << ((c) & 7)))
#define DYN_LOG_SIMD_MIN 64

enum { DYN_ESC_CTRL,
    DYN_ESC_JSON };

void dyn_line_init(dyn_line_t* b)
{
    b->p = b->inln;
    b->n = 0;
    b->cap = DYN_LINE_INLINE;
    b->truncated = 0;
}

void dyn_line_free(dyn_line_t* b)
{
    if (b->p != b->inln) {
        free(b->p);
        b->p = b->inln;
        b->cap = DYN_LINE_INLINE;
    }
    b->n = 0;
    b->truncated = 0;
}

static size_t dyn_grow_cap(size_t cur, size_t need, size_t seed)
{
    size_t nc = cur ? cur : seed;
    while (nc < need) {
        if (nc < (1u << 16))
            nc *= 2;
        else if (nc < (1u << 20))
            nc += nc / 2;
        else
            nc += nc / 4;
    }
    return nc;
}

void dyn_line_put_slow(dyn_line_t* b, const char* s, size_t n)
{
    size_t limit = DYN_LOG_MAX_LINE - DYN_LOG_TAIL_RESERVE;
    if (b->truncated)
        return;
    if (b->n + n > limit) {
        n = (b->n < limit) ? limit - b->n : 0;
        b->truncated = 1;
    }
    if (b->n + n > b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + n, 512);
        char* np;
        if (b->p == b->inln) {
            np = (char*)malloc(nc);
            if (np && b->n)
                memcpy(np, b->p, b->n);
        } else {
            np = (char*)realloc(b->p, nc);
        }
        if (!np) {
            b->truncated = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

void dyn_line_force(dyn_line_t* b, const char* s, size_t n)
{
    if (b->n >= DYN_LOG_MAX_LINE)
        return;
    if (n > DYN_LOG_MAX_LINE - b->n)
        n = DYN_LOG_MAX_LINE - b->n;
    if (b->n + n > b->cap) {
        size_t nc = dyn_grow_cap(b->cap, b->n + n, 512);
        char* np;
        if (b->p == b->inln) {
            np = (char*)malloc(nc);
            if (np && b->n)
                memcpy(np, b->p, b->n);
        } else {
            np = (char*)realloc(b->p, nc);
        }
        if (!np)
            return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

const uint8_t DYN_LOG_ESC_BM[32] = {
    0xff,
    0xff,
    0xff,
    0xff,
    0x04,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x10,
    0x00,
    0x00,
    0x00,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x04,
    0x00,
    0x00,
    0x00,
    0x04,
    0x00,
    0x00,
    0x00,
};

static void dyn_line_escape(dyn_line_t* b, const char* s, size_t n, int mode)
{
    static const char HEX[] = "0123456789abcdef";
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (!DYN_LOG_ESC(c)) {
            size_t run = i;
            if (n - i >= DYN_LOG_SIMD_MIN) {
                size_t k = simd.find_bitmap((const uint8_t*)s + i, n - i,
                    DYN_LOG_ESC_BM);
                i = (k == (size_t)-1) ? n : i + k;
            } else {
                while (i < n && !DYN_LOG_ESC((unsigned char)s[i]))
                    i++;
            }
            dyn_line_put(b, s + run, i - run);
            if (i >= n)
                break;
            c = (unsigned char)s[i];
        }
        if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] >= 0x80
            && (unsigned char)s[i + 1] <= 0x9F) {
            char e[6] = { '\\', 'u', '0', '0', 0, 0 };
            e[4] = HEX[((unsigned char)s[i + 1] >> 4) & 0xF];
            e[5] = HEX[(unsigned char)s[i + 1] & 0xF];
            dyn_line_put(b, e, 6);
            i += 2;
            continue;
        }
        if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 && ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) {
            dyn_line_put(b, ((unsigned char)s[i + 2] == 0xA8) ? "\\u2028" : "\\u2029", 6);
            i += 3;
            continue;
        }
        switch (c) {
        case '\n':
            dyn_line_puts(b, "\\n");
            break;
        case '\r':
            dyn_line_puts(b, "\\r");
            break;
        case '\t':
            dyn_line_puts(b, "\\t");
            break;
        case '\b':
            dyn_line_puts(b, "\\b");
            break;
        case '\f':
            dyn_line_puts(b, "\\f");
            break;
        case '"':
            dyn_line_puts(b, mode == DYN_ESC_JSON ? "\\\"" : "\"");
            break;
        case '\\':
            dyn_line_puts(b, mode == DYN_ESC_JSON ? "\\\\" : "\\");
            break;
        case 0x7F:
            dyn_line_put(b, "\\u007f", 6);
            break;
        default:
            if (c < 0x20) {
                char e[6] = { '\\', 'u', '0', '0', 0, 0 };
                e[4] = HEX[(c >> 4) & 0xF];
                e[5] = HEX[c & 0xF];
                dyn_line_put(b, e, 6);
            } else {
                dyn_line_put(b, (const char*)&c, 1);
            }
        }
        i++;
    }
}

void dyn_line_json_str(dyn_line_t* b, const char* s, size_t n)
{
    dyn_line_put(b, "\"", 1);
    dyn_line_escape(b, s, n, DYN_ESC_JSON);
    dyn_line_put(b, "\"", 1);
}

void dyn_line_esc_ctrl(dyn_line_t* b, const char* s, size_t n)
{
    dyn_line_escape(b, s, n, DYN_ESC_CTRL);
}

static int dyn_json_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int dyn_json_punct(char c)
{
    return c == '"' || c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':';
}

void dyn_line_json_repair(dyn_line_t* b)
{
    size_t safe = 1;
    char cstack[32];
    int sp = 0, depth = 0;
    int in_str = 0, in_key = 0, esc = 0, expect_key = 0;
    size_t nn = b->n;
    int k;

    for (size_t i = 0; i < b->n; i++) {
        char c = b->p[i];
        if (in_str) {
            if (esc) {
                esc = 0;
                continue;
            }
            if (c == '\\') {
                esc = 1;
                continue;
            }
            if (c == '"') {
                int was_key = in_key;
                in_str = in_key = 0;
                if (!was_key)
                    safe = i + 1;
            }
            continue;
        }
        switch (c) {
        case '"':
            in_str = 1;
            in_key = expect_key;
            break;
        case ':':
            expect_key = 0;
            break;
        case ',':
            if (i && !dyn_json_punct(b->p[i - 1]))
                safe = i;
            expect_key = sp > 0 && cstack[sp - 1] == '{';
            break;
        case '{':
            depth++;
            if (sp < (int)(sizeof(cstack) / sizeof(cstack[0]))) {
                cstack[sp] = '{';
                sp++;
            }
            expect_key = 1;
            safe = i + 1;
            break;
        case '[':
            depth++;
            if (sp < (int)(sizeof(cstack) / sizeof(cstack[0]))) {
                cstack[sp] = '[';
                sp++;
            }
            expect_key = 0;
            safe = i + 1;
            break;
        case '}':
        case ']':
            if (depth > 0)
                depth--;
            if (sp > 0)
                sp--;
            expect_key = sp > 0 && cstack[sp - 1] == '{';
            safe = i + 1;
            break;
        default:
            break;
        }
    }

    if (in_str && !in_key) {
        size_t hex = 0;
        while (hex < 4 && nn > hex && dyn_json_hex(b->p[nn - 1 - hex]))
            hex++;
        if (hex < 4 && nn >= hex + 2 && b->p[nn - 1 - hex] == 'u') {
            size_t bs = 0;
            while (nn - 1 - hex > bs && b->p[nn - 2 - hex - bs] == '\\')
                bs++;
            if (bs & 1)
                nn -= hex + 2;
        }
        {
            size_t bs = 0;
            while (nn > bs && b->p[nn - 1 - bs] == '\\')
                bs++;
            if (bs & 1)
                nn--;
        }
        {
            size_t cont = 0, need;
            unsigned char lead;
            while (cont < 3 && nn > safe + cont && ((unsigned char)b->p[nn - 1 - cont] & 0xC0) == 0x80)
                cont++;
            lead = nn > safe + cont ? (unsigned char)b->p[nn - 1 - cont] : 0;
            need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
            if (lead >= 0xC0 && cont < need)
                nn -= cont + 1;
            else if (lead < 0xC0 && cont > 0)
                nn -= cont;
        }
        b->n = nn;
        dyn_line_force(b, "...", 3);
        dyn_line_force(b, "\"", 1);
    } else {
        b->n = safe;
    }
    while (b->n + (size_t)depth + 1 > DYN_LOG_MAX_LINE && b->n > 0)
        b->n--;
    for (k = 0; k < depth; k++) {
        int idx = sp - 1 - k;
        dyn_line_force(b,
            (idx >= 0 && cstack[idx] == '[') ? "]" : "}", 1);
    }
    dyn_line_force(b, "\n", 1);
}

