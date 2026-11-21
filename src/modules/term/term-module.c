#include "term-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TERM)

#define DYN_TERM_TABLE_MAX_COLS 4096u
#define DYN_LINE_MAX (1 << 20)

JSClassID dyn_cmd_class_id;

static JSClassDef dyn_cmd_class = {
    "Command",
    .finalizer = dyn_cmd_finalizer,
    .gc_mark = dyn_cmd_gc_mark,
};

char* dyn_arg_esc(const char* s)
{
    size_t n = strlen(s), i, j = 0;
    char* r = (char*)malloc(4 * n + 1);
    if (!r)
        return NULL;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7f) {
            static const char hex[] = "0123456789abcdef";
            r[j++] = '\\';
            r[j++] = 'x';
            r[j++] = hex[ch >> 4];
            r[j++] = hex[ch & 15];
        } else {
            r[j++] = (char)ch;
        }
    }
    r[j] = 0;
    return r;
}

static const JSCFunctionListEntry dyn_cmd_proto[] = {
    JS_CFUNC_DEF("describe", 1, dyn_cmd_describe),
    JS_CFUNC_DEF("option", 1, dyn_cmd_option),
    JS_CFUNC_DEF("argument", 1, dyn_cmd_argument),
    JS_CFUNC_DEF("command", 1, dyn_cmd_command),
    JS_CFUNC_DEF("allowUnknown", 1, dyn_cmd_allow_unknown),
    JS_CFUNC_DEF("action", 1, dyn_cmd_action),
    JS_CFUNC_DEF("version", 0, dyn_cmd_version),
    JS_CFUNC_DEF("parse", 1, dyn_cmd_parse),
    JS_CFUNC_DEF("help", 0, dyn_cmd_help),
    JS_CGETSET_DEF("name", dyn_cmd_get_name, NULL),
};

const dyn_style_t DYN_STYLES[] = {
    { "reset", "\033[0m", "\033[0m" },
    { "bold", "\033[1m", "\033[22m" },
    { "dim", "\033[2m", "\033[22m" },
    { "italic", "\033[3m", "\033[23m" },
    { "underline", "\033[4m", "\033[24m" },
    { "blink", "\033[5m", "\033[25m" },
    { "inverse", "\033[7m", "\033[27m" },
    { "hidden", "\033[8m", "\033[28m" },
    { "strikethrough", "\033[9m", "\033[29m" },
    { "doubleunderline", "\033[21m", "\033[24m" },
    { "black", "\033[30m", "\033[39m" },
    { "red", "\033[31m", "\033[39m" },
    { "green", "\033[32m", "\033[39m" },
    { "yellow", "\033[33m", "\033[39m" },
    { "blue", "\033[34m", "\033[39m" },
    { "magenta", "\033[35m", "\033[39m" },
    { "cyan", "\033[36m", "\033[39m" },
    { "white", "\033[37m", "\033[39m" },
    { "bgBlack", "\033[40m", "\033[49m" },
    { "bgRed", "\033[41m", "\033[49m" },
    { "bgGreen", "\033[42m", "\033[49m" },
    { "bgYellow", "\033[43m", "\033[49m" },
    { "bgBlue", "\033[44m", "\033[49m" },
    { "bgMagenta", "\033[45m", "\033[49m" },
    { "bgCyan", "\033[46m", "\033[49m" },
    { "bgWhite", "\033[47m", "\033[49m" },
    { "framed", "\033[51m", "\033[54m" },
    { "overlined", "\033[53m", "\033[55m" },
    { "gray", "\033[90m", "\033[39m" },
    { "grey", "\033[90m", "\033[39m" },
    { "redBright", "\033[91m", "\033[39m" },
    { "greenBright", "\033[92m", "\033[39m" },
    { "yellowBright", "\033[93m", "\033[39m" },
    { "blueBright", "\033[94m", "\033[39m" },
    { "magentaBright", "\033[95m", "\033[39m" },
    { "cyanBright", "\033[96m", "\033[39m" },
    { "whiteBright", "\033[97m", "\033[39m" },
    { "bgGray", "\033[100m", "\033[49m" },
    { "bgGrey", "\033[100m", "\033[49m" },
    { "bgRedBright", "\033[101m", "\033[49m" },
    { "bgGreenBright", "\033[102m", "\033[49m" },
    { "bgYellowBright", "\033[103m", "\033[49m" },
    { "bgBlueBright", "\033[104m", "\033[49m" },
    { "bgMagentaBright", "\033[105m", "\033[49m" },
    { "bgCyanBright", "\033[106m", "\033[49m" },
    { "bgWhiteBright", "\033[107m", "\033[49m" },
};

static JSValue dyn_styles_list(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue arr = JS_NewArray(ctx);
    size_t i;
    (void)argc;
    (void)argv;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < countof(DYN_STYLES); i++)
        JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
            JS_NewString(ctx, DYN_STYLES[i].name), JS_PROP_C_W_E);
    return arr;
}

static int dyn_fd_of(JSContext* ctx, int argc, JSValueConst* argv,
    int32_t* pfd, const char* who)
{
    int32_t fd = 1;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (!JS_IsNumber(argv[0])) {
            JS_ThrowTypeError(ctx, "%s(fd): fd must be a number", who);
            return -1;
        }
        if (JS_ToInt32(ctx, &fd, argv[0]))
            return -1;
    }
    *pfd = fd;
    return 0;
}

static JSValue dyn_is_tty(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int32_t fd;
    if (dyn_fd_of(ctx, argc, argv, &fd, "IsTTY"))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isatty(fd) == 1);
}

static JSValue dyn_columns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* e = getenv("COLUMNS");
    struct winsize ws;
    int32_t fd = 1;
    (void)this_val;
    memset(&ws, 0, sizeof ws);
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_ToInt32(ctx, &fd, argv[0]))
            return JS_EXCEPTION;
    }
    if (fd >= 0 && ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return JS_NewInt32(ctx, ws.ws_col);
    if (e && *e) {
        char* end;
        long v = strtol(e, &end, 10);
        if (*end == '\0' && v > 0 && v < 100000)
            return JS_NewInt32(ctx, (int32_t)v);
    }
    return JS_NewInt32(ctx, 80);
}

static JSValue dyn_color_depth(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* e;
    int32_t fd;
    if (dyn_fd_of(ctx, argc, argv, &fd, "ColorDepth"))
        return JS_EXCEPTION;
    if ((e = getenv("FORCE_COLOR")) != NULL && *e) {
        if (!strcmp(e, "0") || !strcmp(e, "false"))
            return JS_NewInt32(ctx, 0);
        if (!strcmp(e, "2"))
            return JS_NewInt32(ctx, 8);
        if (!strcmp(e, "3"))
            return JS_NewInt32(ctx, 24);
        return JS_NewInt32(ctx, 4);
    }
    if ((e = getenv("NO_COLOR")) != NULL && *e)
        return JS_NewInt32(ctx, 0);
    if (isatty(fd) != 1)
        return JS_NewInt32(ctx, 0);
    if ((e = getenv("COLORTERM")) != NULL
        && (strstr(e, "truecolor") || strstr(e, "24bit")))
        return JS_NewInt32(ctx, 24);
    if ((e = getenv("TERM")) != NULL && strstr(e, "256color"))
        return JS_NewInt32(ctx, 8);
    if (e && !strcmp(e, "dumb"))
        return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, 4);
}

unsigned char dyn_inq[DYN_INQ_SIZE];

unsigned int dyn_inq_head, dyn_inq_n;

static int dyn_inq_pop(void)
{
    unsigned char c;
    if (!dyn_inq_n)
        return -1;
    c = dyn_inq[dyn_inq_head];
    dyn_inq_head = (dyn_inq_head + 1) & (DYN_INQ_SIZE - 1);
    dyn_inq_n--;
    return c;
}

int dyn_byte_at(int fd, unsigned char* c, int timeout_ms)
{
    ssize_t r;
    int q = dyn_inq_pop();
    if (q >= 0) {
        *c = (unsigned char)q;
        return 1;
    }
    if (timeout_ms >= 0) {
        fd_set rfds;
        struct timeval tv;
        int s;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        s = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (s == 0)
            return -1;
        if (s < 0)
            return (errno == EINTR) ? dyn_byte_at(fd, c, timeout_ms) : 0;
    }
    r = dyn_read_eintr(fd, c, 1);
    return r == 1 ? 1 : 0;
}

static JSValue dyn_keypress_fn(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_key_t k;
    dyn_termios_t saved;
    int raw;
    (void)this_val;
    (void)argc;
    (void)argv;
    raw = dyn_raw_on(&saved);
    dyn_key_read(0, &k);
    dyn_raw_off(&saved, raw);
    if (k.eof)
        return JS_NULL;
    return dyn_key_obj(ctx, &k);
}

static int dyn_read_line(int fd, dyn_sb_t* b)
{
    unsigned char c;
    int over = 0;
    for (;;) {
        if (dyn_byte_at(fd, &c, -1) != 1)
            break;
        if (c == '\n') {
            if (over)
                return -1;
            if (b->n && b->p[b->n - 1] == '\r')
                b->n--;
            if (b->n > DYN_LINE_MAX) {
                b->n = 0;
                return -1;
            }
            return 1;
        }
        if (over)
            continue;
        if (b->n >= DYN_LINE_MAX + 1) {
            over = 1;
            b->n = 0;
            continue;
        }
        dyn_sb_putc(b, (char)c);
        if (b->oom)
            return -1;
    }
    if (over || b->n > DYN_LINE_MAX) {
        b->n = 0;
        return -1;
    }
    return b->n ? 1 : 0;
}

static JSValue dyn_prompt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *msg, *dflt = NULL;
    size_t mlen = 0, dlen = 0;
    dyn_sb_t line;
    JSValue r;
    int lr;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "prompt(message, opts): message must be a string");
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char* const pkeys[] = { "default" };
        JSValue v;
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "prompt(message, opts): opts must be an object");
        if (dyn_opts_strict(ctx, argv[1], pkeys, 1))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "default");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "prompt({default}): default must be a string");
            }
            dflt = JS_ToCStringLen(ctx, &dlen, v);
            JS_FreeValue(ctx, v);
            if (!dflt)
                return JS_EXCEPTION;
        }
    }
    msg = JS_ToCStringLen(ctx, &mlen, argv[0]);
    if (!msg) {
        if (dflt)
            JS_FreeCString(ctx, dflt);
        return JS_EXCEPTION;
    }
    dyn_sb_init(&line);
    dyn_stdio_sync();
    if (mlen && write(1, msg, mlen) < 0) {
    }
    lr = dyn_read_line(0, &line);
    if (lr < 0) {
        JS_ThrowRangeError(ctx, "prompt: the answer exceeds %d bytes without a newline",
            DYN_LINE_MAX);
        r = JS_EXCEPTION;
    } else if (line.n == 0 && dflt) {
        r = JS_NewStringLen(ctx, dflt, dlen);
    } else {
        r = JS_NewStringLen(ctx, line.p ? line.p : "", line.n);
    }
    dyn_sb_free(&line);
    JS_FreeCString(ctx, msg);
    if (dflt)
        JS_FreeCString(ctx, dflt);
    return r;
}

static JSValue dyn_confirm(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* msg;
    size_t mlen = 0;
    dyn_sb_t line;
    int yes = 0;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "confirm(message): message must be a string");
    msg = JS_ToCStringLen(ctx, &mlen, argv[0]);
    if (!msg)
        return JS_EXCEPTION;
    dyn_sb_init(&line);
    dyn_stdio_sync();
    if (mlen && write(1, msg, mlen) < 0) {
    }
    if (dyn_read_line(0, &line) < 0) {
        JS_FreeCString(ctx, msg);
        dyn_sb_free(&line);
        return JS_ThrowRangeError(ctx, "confirm: the answer exceeds %d bytes without a newline",
            DYN_LINE_MAX);
    }
    if (line.n == 1 && (line.p[0] == 'y' || line.p[0] == 'Y'))
        yes = 1;
    else if (line.n == 3 && strncasecmp(line.p, "yes", 3) == 0)
        yes = 1;
    dyn_sb_free(&line);
    JS_FreeCString(ctx, msg);
    return JS_NewBool(ctx, yes);
}

static void dyn_sel_draw(char* const* o, const size_t* ol, int n,
    int cur, int redraw, int show)
{
    dyn_sb_t b;
    int i;
    dyn_sb_init(&b);
    if (redraw) {
        char up[24];
        snprintf(up, sizeof up, "\033[%dA", n);
        dyn_sb_puts(&b, up);
    }
    for (i = 0; i < n; i++) {
        dyn_sb_puts(&b, (show && i == cur) ? "> " : "  ");
        dyn_sb_put(&b, o[i], ol[i]);
        dyn_sb_puts(&b, "\033[K\n");
    }
    dyn_stdio_sync();
    if (b.p && b.n && write(1, b.p, b.n) < 0) {
    }
    dyn_sb_free(&b);
}

static JSValue dyn_select(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* msg;
    size_t mlen = 0;
    char** o = NULL;
    size_t* ol = NULL;
    int64_t len64 = 0;
    int n = 0, i, cur = 0, ok;
    JSValue arr, r = JS_NULL;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "select(message, options): message must be a string");
    arr = argv[1];
    if (!JS_IsArray(ctx, arr))
        return JS_ThrowTypeError(ctx, "select(message, options): options must be an array of strings");
    {
        JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        ok = JS_ToInt64(ctx, &len64, lv) == 0;
        JS_FreeValue(ctx, lv);
        if (!ok)
            return JS_EXCEPTION;
    }
    if (len64 < 1)
        return JS_ThrowTypeError(ctx, "select(message, options): options must be non-empty");
    if (len64 > 65536)
        return JS_ThrowRangeError(ctx, "select(message, options): more than 65536 options");
    n = (int)len64;
    msg = JS_ToCStringLen(ctx, &mlen, argv[0]);
    if (!msg)
        return JS_EXCEPTION;
    o = (char**)calloc((size_t)n, sizeof *o);
    ol = (size_t*)calloc((size_t)n, sizeof *ol);
    if (!o || !ol) {
        free(o);
        free(ol);
        JS_FreeCString(ctx, msg);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        const char* s;
        if (JS_IsException(v)) {
            int bad = i;
            while (--i >= 0)
                free(o[i]);
            free(o);
            free(ol);
            JS_FreeCString(ctx, msg);
            (void)bad;
            return JS_EXCEPTION;
        }
        if (!JS_IsString(v)) {
            int bad = i;
            JS_FreeValue(ctx, v);
            while (--i >= 0)
                free(o[i]);
            free(o);
            free(ol);
            JS_FreeCString(ctx, msg);
            return JS_ThrowTypeError(ctx,
                "select(message, options): option %d must be a string", bad);
        }
        s = JS_ToCStringLen(ctx, &ol[i], v);
        JS_FreeValue(ctx, v);
        if (!s) {
            while (--i >= 0)
                free(o[i]);
            free(o);
            free(ol);
            JS_FreeCString(ctx, msg);
            return JS_EXCEPTION;
        }
        o[i] = dyn_dup_span(s, ol[i]);
        JS_FreeCString(ctx, s);
        if (!o[i]) {
            while (--i >= 0)
                free(o[i]);
            free(o);
            free(ol);
            JS_FreeCString(ctx, msg);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    {
        dyn_termios_t saved;
        int raw = dyn_raw_on(&saved);
        dyn_stdio_sync();
        if (mlen && write(1, msg, mlen) < 0) {
        }
        if (write(1, "\n", 1) < 0) {
        }
        dyn_sel_draw(o, ol, n, cur, 0, 1);
        for (;;) {
            dyn_key_t k;
            dyn_key_read(0, &k);
            if (k.eof) {
                dyn_sel_draw(o, ol, n, cur, 1, 0);
                r = JS_NULL;
                break;
            }
            if (k.name && !k.ctrl && !k.meta && !strcmp(k.name, "up")) {
                cur = (cur + n - 1) % n;
                dyn_sel_draw(o, ol, n, cur, 1, 1);
                continue;
            }
            if (k.name && !k.ctrl && !k.meta && !strcmp(k.name, "down")) {
                cur = (cur + 1) % n;
                dyn_sel_draw(o, ol, n, cur, 1, 1);
                continue;
            }
            if (k.name && !k.ctrl && !k.meta && !strcmp(k.name, "enter")) {
                dyn_sel_draw(o, ol, n, cur, 1, 1);
                r = JS_NewStringLen(ctx, o[cur], ol[cur]);
                break;
            }
            if ((k.name && !strcmp(k.name, "escape") && !k.ctrl && !k.meta) || (k.ctrl && k.name && !strcmp(k.name, "c"))) {
                dyn_sel_draw(o, ol, n, cur, 1, 0);
                r = JS_NULL;
                break;
            }
        }
        dyn_raw_off(&saved, raw);
    }
    for (i = 0; i < n; i++)
        free(o[i]);
    free(o);
    free(ol);
    JS_FreeCString(ctx, msg);
    return r;
}

JSClassID dyn_pbar_class_id;

static JSClassDef dyn_pbar_class = {
    "ProgressBar",
    .finalizer = dyn_pbar_finalizer,
};

static const JSCFunctionListEntry dyn_pbar_proto[] = {
    JS_CFUNC_DEF("update", 1, dyn_pbar_update),
};

JSClassID dyn_spin_class_id;

static JSClassDef dyn_spin_class = {
    "Spinner",
    .finalizer = dyn_spin_finalizer,
};

static const JSCFunctionListEntry dyn_spin_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_spin_start),
    JS_CFUNC_DEF("tick", 0, dyn_spin_tick),
    JS_CFUNC_DEF("stop", 0, dyn_spin_stop),
};

static int dyn_utf8_width(const char* s, size_t n)
{
    size_t i;
    int w = 0;
    for (i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xc0) != 0x80)
            w++;
    return w;
}

static void dyn_sb_pad(dyn_sb_t* b, int n)
{
    static const char SP[] = "                                ";
    while (n > 0) {
        int k = n < 32 ? n : 32;
        dyn_sb_put(b, SP, (size_t)k);
        n -= k;
    }
}

static void dyn_csv_cell(dyn_sb_t* b, const char* s, size_t n)
{
    size_t i;
    int quote = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        quote |= (c == '"') | (c == ',') | (c == '\r') | (c == '\n');
    }
    if (!quote) {
        dyn_sb_put(b, s, n);
        return;
    }
    dyn_sb_putc(b, '"');
    for (i = 0; i < n; i++) {
        if (s[i] == '"')
            dyn_sb_putc(b, '"');
        dyn_sb_putc(b, s[i]);
    }
    dyn_sb_putc(b, '"');
}

static JSValue dyn_table(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    enum { DYN_ALIGN_LEFT,
        DYN_ALIGN_RIGHT,
        DYN_ALIGN_CENTER };
    JSValueConst rows, opts;
    JSValue head = JS_UNDEFINED;
    int64_t nrow64 = 0;
    uint32_t nrow = 0, ncols = 0, i, j;
    size_t* coff = NULL;
    dyn_sb_t store;
    const char* cbase = "";
    size_t* w = NULL;
    size_t ncells;
    size_t* clen = NULL;
    int* cwid = NULL;
    char** hs = NULL;
    size_t* hl = NULL;
    int* hw = NULL;
    int* align = NULL;
    int fmt_grid = 1, fmt_tsv = 0, fmt_csv = 0;
    int fc_owned = 0;
    dyn_sb_t b;
    JSValue r = JS_EXCEPTION;
    const char* fc = "grid";

    dyn_sb_init(&b);
    dyn_sb_init(&store);
    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Table(rows, opts): rows must be an array");
    rows = argv[0];
    opts = (argc > 1 && !JS_IsNull(argv[1])) ? argv[1] : JS_UNDEFINED;
    if (!JS_IsUndefined(opts)) {
        static const char* const tkeys[] = { "head", "align", "format" };
        if (!JS_IsObject(opts))
            return JS_ThrowTypeError(ctx, "Table(rows, opts): opts must be an object");
        if (dyn_opts_strict(ctx, opts, tkeys, 3))
            return JS_EXCEPTION;
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, rows, "length");
        int64_t dummy = 0;
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &dummy, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (dummy < 0)
            dummy = 0;
        if (dummy > 65536 * 4096)
            return JS_ThrowRangeError(ctx, "Table(rows): too many rows");
        nrow64 = dummy;
    }
    if (!JS_IsUndefined(opts)) {
        head = JS_GetPropertyStr(ctx, opts, "head");
        if (JS_IsException(head))
            return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(head) && !JS_IsNull(head)) {
        if (!JS_IsArray(ctx, head)) {
            JS_FreeValue(ctx, head);
            return JS_ThrowTypeError(ctx, "Table({head}): head must be an array of strings");
        }
        {
            JSValue lv = JS_GetPropertyStr(ctx, head, "length");
            int64_t hlen = 0;
            if (JS_IsException(lv) || JS_ToInt64(ctx, &hlen, lv)) {
                JS_FreeValue(ctx, lv);
                JS_FreeValue(ctx, head);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, lv);
            ncols = (uint32_t)(hlen < 0 ? 0 : hlen);
        }
    } else {
        JS_FreeValue(ctx, head);
        head = JS_UNDEFINED;
    }
    if (ncols == 0 && nrow64 > 0) {
        JSValue row0 = JS_GetPropertyUint32(ctx, rows, 0);
        JSValue lv;
        int64_t rl = 0;
        if (JS_IsException(row0))
            return JS_EXCEPTION;
        if (!JS_IsArray(ctx, row0)) {
            JS_FreeValue(ctx, row0);
            return JS_ThrowTypeError(ctx, "Table(rows): every row must be an array");
        }
        lv = JS_GetPropertyStr(ctx, row0, "length");
        JS_FreeValue(ctx, row0);
        if (JS_IsException(lv) || JS_ToInt64(ctx, &rl, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        ncols = (uint32_t)(rl < 0 ? 0 : rl);
    }
    nrow = (uint32_t)nrow64;
    if (ncols == 0) {
        JS_FreeValue(ctx, head);
        return JS_NewString(ctx, "");
    }
    if (ncols > DYN_TERM_TABLE_MAX_COLS) {
        JS_FreeValue(ctx, head);
        return JS_ThrowRangeError(ctx, "Table(rows): more than %u columns",
            (unsigned)DYN_TERM_TABLE_MAX_COLS);
    }
    if ((uint64_t)nrow * ncols > (uint64_t)1 << 28) {
        JS_FreeValue(ctx, head);
        return JS_ThrowRangeError(ctx, "Table(rows): too many cells");
    }
    ncells = (size_t)nrow * ncols + 1;
    coff = (size_t*)calloc(ncells, sizeof *coff);
    clen = (size_t*)calloc(ncells, sizeof *clen);
    cwid = (int*)calloc(ncells, sizeof *cwid);
    hs = (char**)calloc(ncols, sizeof *hs);
    hl = (size_t*)calloc(ncols, sizeof *hl);
    hw = (int*)calloc(ncols, sizeof *hw);
    align = (int*)calloc(ncols, sizeof *align);
    w = (size_t*)calloc(ncols, sizeof *w);
    if (!coff || !clen || !cwid || !hs || !hl || !hw || !align || !w)
        goto oom;
    if (!JS_IsUndefined(head)) {
        for (j = 0; j < ncols; j++) {
            JSValue hv = JS_GetPropertyUint32(ctx, head, j);
            const char* s;
            if (JS_IsException(hv))
                goto fail;
            if (!JS_IsString(hv)) {
                JS_FreeValue(ctx, hv);
                JS_ThrowTypeError(ctx, "Table({head}): head[%d] must be a string", (int)j);
                goto fail;
            }
            s = JS_ToCStringLen(ctx, &hl[j], hv);
            JS_FreeValue(ctx, hv);
            if (!s)
                goto fail;
            hs[j] = dyn_dup_span(s, hl[j]);
            JS_FreeCString(ctx, s);
            if (!hs[j])
                goto oom;
            hw[j] = dyn_utf8_width(hs[j], hl[j]);
            w[j] = (size_t)hw[j];
        }
    }
    {
        if (!JS_IsUndefined(opts)) {
            JSValue v = JS_GetPropertyStr(ctx, opts, "format");
            if (JS_IsException(v))
                goto fail;
            if (!JS_IsUndefined(v)) {
                if (!JS_IsString(v)) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowTypeError(ctx, "Table({format}): format must be a string");
                    goto fail;
                }
                fc = JS_ToCString(ctx, v);
                JS_FreeValue(ctx, v);
                if (!fc)
                    goto fail;
                fc_owned = 1;
                fmt_grid = fmt_tsv = fmt_csv = 0;
                if (!strcmp(fc, "grid"))
                    fmt_grid = 1;
                else if (!strcmp(fc, "tsv"))
                    fmt_tsv = 1;
                else if (!strcmp(fc, "csv"))
                    fmt_csv = 1;
                else {
                    char* e = dyn_arg_esc(fc);
                    JS_ThrowRangeError(ctx,
                        "Table({format}): unknown format \"%s\" (valid: grid, tsv, csv)",
                        e ? e : fc);
                    free(e);
                    goto fail;
                }
            }
            v = JS_GetPropertyStr(ctx, opts, "align");
            if (JS_IsException(v))
                goto fail;
            if (!JS_IsUndefined(v)) {
                int64_t al = 0, ai;
                JSValue lv;
                if (!JS_IsArray(ctx, v)) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowTypeError(ctx, "Table({align}): align must be an array");
                    goto fail;
                }
                lv = JS_GetPropertyStr(ctx, v, "length");
                if (JS_IsException(lv) || JS_ToInt64(ctx, &al, lv)) {
                    JS_FreeValue(ctx, lv);
                    JS_FreeValue(ctx, v);
                    goto fail;
                }
                JS_FreeValue(ctx, lv);
                if (al > (int64_t)ncols) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowRangeError(ctx,
                        "Table({align}): %d entries for %d columns", (int)al, (int)ncols);
                    goto fail;
                }
                for (ai = 0; ai < al; ai++) {
                    JSValue ev = JS_GetPropertyUint32(ctx, v, (uint32_t)ai);
                    const char* a = NULL;
                    if (!JS_IsException(ev))
                        a = JS_ToCString(ctx, ev);
                    JS_FreeValue(ctx, ev);
                    if (!a) {
                        JS_FreeValue(ctx, v);
                        goto fail;
                    }
                    if (!strcmp(a, "left"))
                        align[ai] = DYN_ALIGN_LEFT;
                    else if (!strcmp(a, "right"))
                        align[ai] = DYN_ALIGN_RIGHT;
                    else if (!strcmp(a, "center"))
                        align[ai] = DYN_ALIGN_CENTER;
                    else {
                        char* e = dyn_arg_esc(a);
                        JS_ThrowRangeError(ctx,
                            "Table({align}): unknown alignment \"%s\" (valid: left, right, center)",
                            e ? e : a);
                        free(e);
                        JS_FreeCString(ctx, a);
                        JS_FreeValue(ctx, v);
                        goto fail;
                    }
                    JS_FreeCString(ctx, a);
                }
                JS_FreeValue(ctx, v);
            }
        }
    }
    for (i = 0; i < nrow; i++) {
        JSValue row = JS_GetPropertyUint32(ctx, rows, i);
        int64_t rl = 0;
        JSValue lv;
        if (JS_IsException(row))
            goto fail;
        if (!JS_IsArray(ctx, row)) {
            JS_FreeValue(ctx, row);
            JS_ThrowTypeError(ctx, "Table(rows): row %d must be an array", (int)i);
            goto fail;
        }
        lv = JS_GetPropertyStr(ctx, row, "length");
        if (JS_IsException(lv) || JS_ToInt64(ctx, &rl, lv)) {
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, row);
            goto fail;
        }
        JS_FreeValue(ctx, lv);
        if (rl != (int64_t)ncols) {
            JS_FreeValue(ctx, row);
            JS_ThrowTypeError(ctx,
                "Table(rows): row %d has %d cells, expected %d",
                (int)i, (int)rl, (int)ncols);
            goto fail;
        }
        for (j = 0; j < ncols; j++) {
            JSValue v = JS_GetPropertyUint32(ctx, row, j);
            JSValue sv;
            const char* s;
            size_t idx = (size_t)i * ncols + j;
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            if (JS_IsNumber(v)) {
                sv = JS_ToString(ctx, v);
                JS_FreeValue(ctx, v);
            } else if (JS_IsString(v)) {
                sv = v;
            } else {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, row);
                JS_ThrowTypeError(ctx,
                    "Table(rows): cell (%d, %d) must be a string or number",
                    (int)i, (int)j);
                goto fail;
            }
            if (JS_IsException(sv)) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            s = JS_ToCStringLen(ctx, &clen[idx], sv);
            JS_FreeValue(ctx, sv);
            if (!s) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            coff[idx] = store.n;
            dyn_sb_put(&store, s, clen[idx]);
            cwid[idx] = dyn_utf8_width(s, clen[idx]);
            JS_FreeCString(ctx, s);
            if (store.oom) {
                JS_FreeValue(ctx, row);
                goto oom;
            }
            if ((size_t)cwid[idx] > w[j])
                w[j] = (size_t)cwid[idx];
        }
        JS_FreeValue(ctx, row);
    }
    cbase = store.p ? store.p : "";
    {
        if (fmt_grid) {
            if (!JS_IsUndefined(head)) {
                for (j = 0; j < ncols; j++) {
                    int pad = (int)w[j] - hw[j];
                    int pl = 0, pr = pad;
                    if (align[j] == DYN_ALIGN_CENTER) {
                        pl = pad / 2;
                        pr = pad - pl;
                    } else if (align[j] == DYN_ALIGN_LEFT) {
                        pl = 0;
                        pr = pad;
                    } else {
                        pl = pad;
                        pr = 0;
                    }
                    dyn_sb_pad(&b, pl);
                    dyn_sb_put(&b, hs[j], hl[j]);
                    if (j + 1 < ncols) {
                        dyn_sb_pad(&b, pr);
                        dyn_sb_puts(&b, "  ");
                    }
                }
                dyn_sb_putc(&b, '\n');
                for (j = 0; j < ncols; j++) {
                    size_t d;
                    for (d = 0; d < w[j]; d++)
                        dyn_sb_putc(&b, '-');
                    if (j + 1 < ncols)
                        dyn_sb_puts(&b, "  ");
                }
                dyn_sb_putc(&b, '\n');
            }
            for (i = 0; i < nrow; i++) {
                for (j = 0; j < ncols; j++) {
                    size_t idx = (size_t)i * ncols + j;
                    int pad = (int)w[j] - cwid[idx];
                    int pl = 0, pr = pad;
                    if (align[j] == DYN_ALIGN_CENTER) {
                        pl = pad / 2;
                        pr = pad - pl;
                    } else if (align[j] == DYN_ALIGN_RIGHT) {
                        pl = pad;
                        pr = 0;
                    }
                    dyn_sb_pad(&b, pl);
                    dyn_sb_put(&b, cbase + coff[idx], clen[idx]);
                    if (j + 1 < ncols) {
                        dyn_sb_pad(&b, pr);
                        dyn_sb_puts(&b, "  ");
                    }
                }
                dyn_sb_putc(&b, '\n');
            }
        } else if (fmt_tsv) {
            if (!JS_IsUndefined(head)) {
                for (j = 0; j < ncols; j++) {
                    if (j)
                        dyn_sb_putc(&b, '\t');
                    dyn_sb_put(&b, hs[j], hl[j]);
                }
                dyn_sb_putc(&b, '\n');
            }
            for (i = 0; i < nrow; i++) {
                for (j = 0; j < ncols; j++) {
                    size_t idx = (size_t)i * ncols + j;
                    if (j)
                        dyn_sb_putc(&b, '\t');
                    dyn_sb_put(&b, cbase + coff[idx], clen[idx]);
                }
                dyn_sb_putc(&b, '\n');
            }
        } else {
            if (!JS_IsUndefined(head)) {
                for (j = 0; j < ncols; j++) {
                    if (j)
                        dyn_sb_putc(&b, ',');
                    dyn_csv_cell(&b, hs[j], hl[j]);
                }
                dyn_sb_putc(&b, '\n');
            }
            for (i = 0; i < nrow; i++) {
                for (j = 0; j < ncols; j++) {
                    size_t idx = (size_t)i * ncols + j;
                    if (j)
                        dyn_sb_putc(&b, ',');
                    dyn_csv_cell(&b, cbase + coff[idx], clen[idx]);
                }
                dyn_sb_putc(&b, '\n');
            }
        }
    }
    if (b.oom)
        goto oom_b;
    r = JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    goto cleanup;

oom:
    JS_ThrowOutOfMemory(ctx);
    goto fail;
oom_b:
    JS_ThrowOutOfMemory(ctx);
    goto fail;
fail:
    r = JS_EXCEPTION;
cleanup:
    dyn_sb_free(&b);
    dyn_sb_free(&store);
    free(w);
    if (hs) {
        uint32_t t;
        for (t = 0; t < ncols; t++)
            free(hs[t]);
    }
    free(coff);
    free(clen);
    free(cwid);
    free(hs);
    free(hl);
    free(hw);
    free(align);
    JS_FreeValue(ctx, head);
    if (fc_owned)
        JS_FreeCString(ctx, fc);
    return r;
}

static const JSCFunctionListEntry dyn_term_funcs[] = {
    JS_CFUNC_DEF("StyleText", 2, dyn_style_text),
    JS_CFUNC_DEF("Styles", 0, dyn_styles_list),
    JS_CFUNC_DEF("IsTTY", 0, dyn_is_tty),
    JS_CFUNC_DEF("Columns", 0, dyn_columns),
    JS_CFUNC_DEF("ColorDepth", 0, dyn_color_depth),
    JS_CFUNC_DEF("prompt", 2, dyn_prompt),
    JS_CFUNC_DEF("confirm", 1, dyn_confirm),
    JS_CFUNC_DEF("select", 2, dyn_select),
    JS_CFUNC_DEF("keypress", 0, dyn_keypress_fn),
    JS_CFUNC_DEF("Table", 2, dyn_table),
};

static int dyn_term_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_cmd_class_id, &dyn_cmd_class,
            dyn_cmd_proto, countof(dyn_cmd_proto),
            dyn_cmd_ctor, "Command")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_pbar_class_id, &dyn_pbar_class,
            dyn_pbar_proto, countof(dyn_pbar_proto),
            dyn_pbar_ctor, "ProgressBar")
        < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_spin_class_id, &dyn_spin_class,
            dyn_spin_proto, countof(dyn_spin_proto),
            dyn_spin_ctor, "Spinner")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_term_funcs, countof(dyn_term_funcs));
}

int js_nat_init_term(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:cli", dyn_term_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Command");
    JS_AddModuleExport(ctx, m, "ProgressBar");
    JS_AddModuleExport(ctx, m, "Spinner");
    return JS_AddModuleExportList(ctx, m, dyn_term_funcs, countof(dyn_term_funcs));
}

#endif
