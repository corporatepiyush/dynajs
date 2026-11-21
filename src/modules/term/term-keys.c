#include "term-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TERM)

#define DYN_KEY_ESC_MS 50

static void dyn_inq_unread(unsigned char c)
{
    if (dyn_inq_n >= DYN_INQ_SIZE)
        return;
    dyn_inq_head = (dyn_inq_head + DYN_INQ_SIZE - 1) & (DYN_INQ_SIZE - 1);
    dyn_inq[dyn_inq_head] = c;
    dyn_inq_n++;
}

static void dyn_key_csi(dyn_key_t* k, char* params, unsigned char f)
{
    int p0 = 0, mod = 0;
    const char* semi;
    if (params[0])
        p0 = (int)strtol(params, NULL, 10);
    semi = strchr(params, ';');
    if (semi && semi[1])
        mod = (int)strtol(semi + 1, NULL, 10) - 1;
    if (mod > 0) {
        k->shift |= (mod & 1) != 0;
        k->meta |= (mod & 2) != 0;
        k->ctrl |= (mod & 4) != 0;
    }
    switch (f) {
    case 'A':
        k->name = "up";
        break;
    case 'B':
        k->name = "down";
        break;
    case 'C':
        k->name = "right";
        break;
    case 'D':
        k->name = "left";
        break;
    case 'H':
        k->name = "home";
        break;
    case 'F':
        k->name = "end";
        break;
    case 'Z':
        k->name = "tab";
        k->shift = 1;
        break;
    case '~':
        switch (p0) {
        case 1:
            k->name = "home";
            break;
        case 2:
            k->name = "insert";
            break;
        case 3:
            k->name = "delete";
            break;
        case 4:
            k->name = "end";
            break;
        case 5:
            k->name = "pageup";
            break;
        case 6:
            k->name = "pagedown";
            break;
        }
        break;
    }
}

static void dyn_key_base(dyn_key_t* k, int fd, unsigned char b)
{
    if (b == '\r' || b == '\n') {
        k->name = "enter";
        return;
    }
    if (b == '\t') {
        k->name = "tab";
        return;
    }
    if (b == 0x7f || b == 0x08) {
        k->name = "backspace";
        return;
    }
    if (b == 0x1b) {
        k->name = "escape";
        return;
    }
    if (b < 0x20) {
        static const char map[] = "@abcdefghijklmnopqrstuvwxyz[\\]^_";
        k->ctrl = 1;
        k->chbuf[0] = map[b];
        k->chbuf[1] = 0;
        k->name = k->chbuf;
        return;
    }
    if (b < 0x80) {
        k->chbuf[0] = (char)b;
        k->chbuf[1] = 0;
        k->name = k->chbuf;
        return;
    }
    {
        int need = (b >= 0xf0) ? 3 : (b >= 0xe0) ? 2
            : (b >= 0xc0)                        ? 1
                                                 : 0;
        int i;
        if (need == 0)
            return;
        k->chbuf[0] = (char)b;
        for (i = 1; i <= need; i++) {
            unsigned char c;
            if (dyn_byte_at(fd, &c, DYN_KEY_ESC_MS) != 1)
                return;
            if ((c & 0xc0) != 0x80) {
                dyn_inq_unread(c);
                return;
            }
            if (k->seqlen >= (int)sizeof k->seq - 1) {
                dyn_inq_unread(c);
                return;
            }
            k->seq[k->seqlen++] = (char)c;
            k->chbuf[i] = (char)c;
        }
        k->chbuf[need + 1] = 0;
        k->name = k->chbuf;
    }
}

int dyn_key_read(int fd, dyn_key_t* k)
{
    unsigned char b = 0;

    memset(k, 0, sizeof *k);
    if (dyn_byte_at(fd, &b, -1) != 1) {
        k->eof = 1;
        return 0;
    }
    k->seq[k->seqlen++] = (char)b;
    for (;;) {
        if (b != 0x1b) {
            dyn_key_base(k, fd, b);
            return 1;
        }
        {
            unsigned char b1;
            if (dyn_byte_at(fd, &b1, DYN_KEY_ESC_MS) != 1) {
                k->name = "escape";
                return 1;
            }
            if (k->seqlen >= (int)sizeof k->seq - 1) {
                dyn_inq_unread(b1);
                k->name = "escape";
                return 1;
            }
            k->seq[k->seqlen++] = (char)b1;
            if (b1 == 0x1b) {
                k->meta = 1;
                b = b1;
                continue;
            }
            if (b1 == '[' || b1 == 'O') {
                char params[12];
                int np = 0;
                unsigned char f;
                for (;;) {
                    if (dyn_byte_at(fd, &f, DYN_KEY_ESC_MS) != 1)
                        return 1;
                    if (k->seqlen >= (int)sizeof k->seq - 1) {
                        dyn_inq_unread(f);
                        return 1;
                    }
                    k->seq[k->seqlen++] = (char)f;
                    if (f >= 0x30 && f <= 0x3f && np < (int)sizeof params - 1) {
                        params[np++] = (char)f;
                        continue;
                    }
                    break;
                }
                params[np] = 0;
                if (b1 == 'O') {
                    switch (f) {
                    case 'A':
                        k->name = "up";
                        break;
                    case 'B':
                        k->name = "down";
                        break;
                    case 'C':
                        k->name = "right";
                        break;
                    case 'D':
                        k->name = "left";
                        break;
                    case 'H':
                        k->name = "home";
                        break;
                    case 'F':
                        k->name = "end";
                        break;
                    }
                    return 1;
                }
                dyn_key_csi(k, params, f);
                return 1;
            }
            k->meta = 1;
            dyn_key_base(k, fd, b1);
            return 1;
        }
    }
}

JSValue dyn_key_obj(JSContext* ctx, const dyn_key_t* k)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, "name",
        k->name ? JS_NewString(ctx, k->name) : JS_NULL, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "sequence", JS_NewStringLen(ctx, k->seq, k->seqlen), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "ctrl", JS_NewBool(ctx, k->ctrl), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "meta", JS_NewBool(ctx, k->meta), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "shift", JS_NewBool(ctx, k->shift), JS_PROP_C_W_E);
    return o;
}

#endif
