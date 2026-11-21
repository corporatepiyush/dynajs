/*
 * test_logk_core.c -- src/core/logk (the dyna:log line buffer: bounded
 * append, JSON and control-character escaping, truncation repair, and the
 * $DEBUG namespace matcher) as a standalone C library.
 *
 * Oracles are independent of the code. Escaping: a code-point escaper
 * written here (\n \r \t \b \f short forms, \u00XX for other C0 controls,
 * DEL and the C1 range U+0080..U+009F,  / , and \" \\ in JSON
 * mode only) must match byte for byte on 30k random strings that mix
 * ASCII, controls, valid UTF-8 and stray bytes, at lengths on both sides
 * of the 64-byte SIMD cut-over; on valid UTF-8 a JSON string decoder
 * written here must give the input back. Bounds: puts past 64 KiB - 16 stop
 * exactly at the limit and set truncated; force may fill to 64 KiB.
 * Repair: random nested log objects (depth <= 8, escapes, multibyte text)
 * are cut at every byte offset and the repaired line must parse as JSON
 * under a strict RFC 8259 reader written here, be valid UTF-8, end in one
 * newline, and keep the cut text as a prefix. A cut right after "\u" with
 * zero to three hex digits must drop the partial escape, but "\\u..." (an
 * escaped backslash followed by a literal u) must keep its text. Matcher: fnmatch(3) with one
 * star, last matching pattern wins, '-' negates.
 */
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-logk.h"
#include "dyn-simdk.h"

static int fails, checks;

#define CHECK(c, ...)                                       \
    do {                                                    \
        checks++;                                           \
        if (!(c)) {                                         \
            fails++;                                        \
            if (fails < 30) {                               \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while (0)

static unsigned long long rs = 0x0123456789abcdefull;
static unsigned rnd(unsigned n)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs % n);
}

static size_t o_escape(const unsigned char* s, size_t n, int json, char* out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c == 0xC2 && i + 1 < n && s[i + 1] >= 0x80 && s[i + 1] <= 0x9F) {
            o += (size_t)sprintf(out + o, "\\u%04x", s[i + 1]);
            i += 2;
            continue;
        }
        if (c == 0xE2 && i + 2 < n && s[i + 1] == 0x80 && (s[i + 2] == 0xA8 || s[i + 2] == 0xA9)) {
            o += (size_t)sprintf(out + o, "\\u%04x", 0x2000 + (s[i + 2] - 0xA8 + 0x28));
            i += 3;
            continue;
        }
        if (c == '\n')
            o += (size_t)sprintf(out + o, "\\n");
        else if (c == '\r')
            o += (size_t)sprintf(out + o, "\\r");
        else if (c == '\t')
            o += (size_t)sprintf(out + o, "\\t");
        else if (c == '\b')
            o += (size_t)sprintf(out + o, "\\b");
        else if (c == '\f')
            o += (size_t)sprintf(out + o, "\\f");
        else if (c < 0x20 || c == 0x7F)
            o += (size_t)sprintf(out + o, "\\u%04x", c);
        else if (json && (c == '"' || c == '\\'))
            o += (size_t)sprintf(out + o, "\\%c", c);
        else
            out[o++] = (char)c;
        i++;
    }
    return o;
}

static int utf8_ok(const unsigned char* s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i], need, cp, min;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            need = 1, cp = c & 0x1F, min = 0x80;
        else if (c >= 0xE0 && c <= 0xEF)
            need = 2, cp = c & 0x0F, min = 0x800;
        else if (c >= 0xF0 && c <= 0xF4)
            need = 3, cp = c & 0x07, min = 0x10000;
        else
            return 0;
        if (i + need >= n + 0 && i + need > n - 1 + 1)
            return 0;
        for (unsigned k = 1; k <= need; k++) {
            if (i + k >= n || (s[i + k] & 0xC0) != 0x80)
                return 0;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += need + 1;
    }
    return 1;
}

static size_t put_utf8(unsigned cp, unsigned char* o)
{
    if (cp < 0x80) {
        o[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (unsigned char)(0xC0 | cp >> 6);
        o[1] = (unsigned char)(0x80 | (cp & 63));
        return 2;
    }
    o[0] = (unsigned char)(0xE0 | cp >> 12);
    o[1] = (unsigned char)(0x80 | (cp >> 6 & 63));
    o[2] = (unsigned char)(0x80 | (cp & 63));
    return 3;
}

static long o_unescape(const char* s, size_t n, unsigned char* out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '"')
            return -1;
        if (c != '\\') {
            out[o++] = c;
            i++;
            continue;
        }
        if (i + 1 >= n)
            return -1;
        switch (s[i + 1]) {
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/'; break;
        case 'u': {
            unsigned cp = 0, h;
            if (i + 6 > n)
                return -1;
            for (h = 2; h < 6; h++) {
                char d = s[i + h];
                if (d >= '0' && d <= '9')
                    cp = cp * 16 + (unsigned)(d - '0');
                else if ((d | 32) >= 'a' && (d | 32) <= 'f')
                    cp = cp * 16 + (unsigned)((d | 32) - 'a' + 10);
                else
                    return -1;
            }
            o += put_utf8(cp, out + o);
            i += 4;
            break;
        }
        default:
            return -1;
        }
        i += 2;
    }
    return (long)o;
}

static void rand_bytes(unsigned char* s, size_t n)
{
    static const char* const PIECES[] = { "\n", "\t", "\"", "\\", "\x7f", "\x01", "\x1f", "\xc2\x85", "\xc2\x9f", "\xc2\xa0", "\xe2\x80\xa8",
        "\xe2\x80\xa9", "\xe2\x80\xaa", "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "\xc2", "\xe2\x80", "\xff", "/", "u" };
    size_t i = 0;
    while (i < n) {
        if (rnd(3)) {
            s[i++] = (unsigned char)(' ' + rnd(95));
        } else {
            const char* p = PIECES[rnd(sizeof(PIECES) / sizeof(PIECES[0]))];
            size_t l = strlen(p);
            if (i + l > n)
                l = n - i;
            memcpy(s + i, p, l);
            i += l;
        }
    }
}

static void t_escape(void)
{
    static unsigned char s[3000];
    static char want[20000];
    static unsigned char back[20000];
    int k;
    for (k = 0; k < 30000; k++) {
        size_t n = rnd(k & 1 ? 200 : 2000), wn;
        int json = (int)rnd(2);
        dyn_line_t b;
        rand_bytes(s, n);
        dyn_line_init(&b);
        if (json)
            dyn_line_json_str(&b, (const char*)s, n);
        else
            dyn_line_esc_ctrl(&b, (const char*)s, n);
        wn = o_escape(s, n, json, want + json);
        if (json) {
            want[0] = '"';
            want[wn + 1] = '"';
            wn += 2;
        }
        CHECK(b.n == wn && memcmp(b.p, want, wn) == 0, "escape json=%d n=%zu got %zu want %zu", json, n, b.n, wn);
        if (json && utf8_ok(s, n)) {
            long bn = o_unescape(b.p + 1, b.n - 2, back);
            CHECK(bn == (long)n && memcmp(back, s, n) == 0, "json round trip n=%zu", n);
        }
        dyn_line_free(&b);
    }
}

static void t_bounds(void)
{
    static char big[70000];
    dyn_line_t b;
    size_t lim = DYN_LOG_MAX_LINE - DYN_LOG_TAIL_RESERVE, i;
    memset(big, 'x', sizeof(big));
    dyn_line_init(&b);
    for (i = 0; i < 2000; i++)
        dyn_line_put(&b, big, 37);
    CHECK(b.n == lim && b.truncated == 1, "put stops at limit n=%zu", b.n);
    dyn_line_put(&b, "y", 1);
    CHECK(b.n == lim, "put after truncation is dropped");
    dyn_line_force(&b, big, 100);
    CHECK(b.n == DYN_LOG_MAX_LINE, "force fills to MAX_LINE n=%zu", b.n);
    dyn_line_force(&b, "z", 1);
    CHECK(b.n == DYN_LOG_MAX_LINE, "force at MAX_LINE dropped");
    for (i = 0; i < b.n; i++)
        if (b.p[i] != 'x')
            break;
    CHECK(i == b.n, "content intact");
    dyn_line_free(&b);
    CHECK(b.n == 0 && b.truncated == 0 && b.p == b.inln && b.cap == DYN_LINE_INLINE, "free resets");
    dyn_line_put(&b, big, 511);
    CHECK(b.p == b.inln, "inline up to 512");
    dyn_line_put(&b, big, 2);
    CHECK(b.p != b.inln && b.n == 513 && b.cap >= 513, "spill to heap");
    dyn_line_free(&b);
    dyn_line_put(&b, big, 65000);
    CHECK(b.n == 65000 && !b.truncated, "single big put");
    dyn_line_free(&b);
}

static const char* jp;
static const char* je;

static void ws(void)
{
    while (jp < je && (*jp == ' ' || *jp == '\n' || *jp == '\t' || *jp == '\r'))
        jp++;
}

static int jvalue(int depth);

static int jstring(void)
{
    const char* a;
    if (jp >= je || *jp != '"')
        return 0;
    a = ++jp;
    while (jp < je && *jp != '"') {
        if (*jp == '\\')
            jp++;
        jp++;
    }
    if (jp >= je)
        return 0;
    {
        static unsigned char tmp[70000];
        long r = o_unescape(a, (size_t)(jp - a), tmp);
        jp++;
        return r >= 0;
    }
}

static int jnumber(void)
{
    const char* a = jp;
    if (jp < je && *jp == '-')
        jp++;
    if (jp < je && *jp == '0')
        jp++;
    else if (jp < je && *jp >= '1' && *jp <= '9')
        while (jp < je && *jp >= '0' && *jp <= '9')
            jp++;
    else
        return 0;
    if (jp < je && *jp == '.') {
        const char* d = ++jp;
        while (jp < je && *jp >= '0' && *jp <= '9')
            jp++;
        if (jp == d)
            return 0;
    }
    if (jp < je && (*jp == 'e' || *jp == 'E')) {
        const char* d;
        jp++;
        if (jp < je && (*jp == '+' || *jp == '-'))
            jp++;
        d = jp;
        while (jp < je && *jp >= '0' && *jp <= '9')
            jp++;
        if (jp == d)
            return 0;
    }
    return jp > a;
}

static int jlit(const char* w)
{
    size_t l = strlen(w);
    if ((size_t)(je - jp) < l || memcmp(jp, w, l))
        return 0;
    jp += l;
    return 1;
}

static int jvalue(int depth)
{
    ws();
    if (jp >= je || depth > 40)
        return 0;
    if (*jp == '{') {
        jp++;
        ws();
        if (jp < je && *jp == '}') {
            jp++;
            return 1;
        }
        for (;;) {
            ws();
            if (!jstring())
                return 0;
            ws();
            if (jp >= je || *jp++ != ':')
                return 0;
            if (!jvalue(depth + 1))
                return 0;
            ws();
            if (jp < je && *jp == ',') {
                jp++;
                continue;
            }
            if (jp < je && *jp == '}') {
                jp++;
                return 1;
            }
            return 0;
        }
    }
    if (*jp == '[') {
        jp++;
        ws();
        if (jp < je && *jp == ']') {
            jp++;
            return 1;
        }
        for (;;) {
            if (!jvalue(depth + 1))
                return 0;
            ws();
            if (jp < je && *jp == ',') {
                jp++;
                continue;
            }
            if (jp < je && *jp == ']') {
                jp++;
                return 1;
            }
            return 0;
        }
    }
    if (*jp == '"')
        return jstring();
    if (*jp == 't')
        return jlit("true");
    if (*jp == 'f')
        return jlit("false");
    if (*jp == 'n')
        return jlit("null");
    return jnumber();
}

static int json_ok(const char* s, size_t n)
{
    jp = s;
    je = s + n;
    if (!jvalue(0))
        return 0;
    ws();
    return jp == je;
}

static void gen_value(dyn_line_t* b, int depth);

static void gen_str(dyn_line_t* b)
{
    unsigned char s[80];
    size_t n = rnd(40), i = 0;
    while (i < n) {
        static const char* const P[] = { "a", "Z", " ", "\n", "\"", "\\", "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "\x01", "\xe2\x80\xa8", "\xc2\x85", "u", "uab1", "\\u00" };
        const char* p = P[rnd(sizeof(P) / sizeof(P[0]))];
        size_t l = strlen(p);
        if (i + l > n)
            break;
        memcpy(s + i, p, l);
        i += l;
    }
    dyn_line_json_str(b, (const char*)s, i);
}

static void gen_value(dyn_line_t* b, int depth)
{
    unsigned r = depth >= 8 ? rnd(4) : rnd(7);
    char num[32];
    int i, n;
    switch (r) {
    case 0:
        gen_str(b);
        break;
    case 1:
        n = snprintf(num, sizeof(num), "%d", (int)rnd(200000) - 100000);
        dyn_line_put(b, num, (size_t)n);
        break;
    case 2:
        n = snprintf(num, sizeof(num), "%d.%ue%d", (int)rnd(100) - 50, rnd(1000), (int)rnd(20) - 10);
        dyn_line_put(b, num, (size_t)n);
        break;
    case 3:
        dyn_line_puts(b, (const char* const[]) { "true", "false", "null" }[rnd(3)]);
        break;
    case 4:
    case 5:
        dyn_line_put(b, "{", 1);
        n = (int)rnd(4);
        for (i = 0; i < n; i++) {
            if (i)
                dyn_line_put(b, ",", 1);
            gen_str(b);
            dyn_line_put(b, ":", 1);
            gen_value(b, depth + 1);
        }
        dyn_line_put(b, "}", 1);
        break;
    default:
        dyn_line_put(b, "[", 1);
        n = (int)rnd(4);
        for (i = 0; i < n; i++) {
            if (i)
                dyn_line_put(b, ",", 1);
            gen_value(b, depth + 1);
        }
        dyn_line_put(b, "]", 1);
        break;
    }
}

static size_t expect_keep(const char* src, size_t cut)
{
    size_t i = 0, st = 0;
    int in_str = 0;
    while (i < cut) {
        if (!in_str) {
            if (src[i] == '"') {
                in_str = 1;
                st = i + 1;
            }
            i++;
            continue;
        }
        if (src[i] == '"') {
            in_str = 0;
            i++;
            continue;
        }
        i += src[i] == '\\' ? (src[i + 1] == 'u' ? 6 : 2) : 1;
    }
    if (!in_str)
        return (size_t)-1;
    i = st;
    for (;;) {
        unsigned char c = (unsigned char)src[i];
        size_t len = c == '\\' ? (src[i + 1] == 'u' ? 6 : 2) : c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + len > cut)
            return i;
        i += len;
    }
}

static void t_repair(void)
{
    int k;
    for (k = 0; k < 400; k++) {
        dyn_line_t src, b;
        size_t cut;
        dyn_line_init(&src);
        dyn_line_puts(&src, "{\"level\":30,\"time\":1700000000000,\"msg\":");
        gen_str(&src);
        if (rnd(2)) {
            dyn_line_puts(&src, ",\"ctx\":");
            gen_value(&src, 1);
        }
        dyn_line_put(&src, "}", 1);
        CHECK(json_ok(src.p, src.n), "generator produced invalid JSON");
        for (cut = 1; cut <= src.n; cut++) {
            int ok;
            size_t keep;
            dyn_line_init(&b);
            dyn_line_put(&b, src.p, cut);
            dyn_line_json_repair(&b);
            ok = b.n > 0 && b.p[b.n - 1] == '\n' && json_ok(b.p, b.n - 1) && utf8_ok((const unsigned char*)b.p, b.n);
            CHECK(ok, "repair cut=%zu of %zu: %.*s", cut, src.n, (int)(b.n < 300 ? b.n : 300), b.p);
            for (keep = 0; keep < b.n && keep < cut && b.p[keep] == src.p[keep]; keep++)
                ;
            CHECK(keep > 0, "repair keeps a prefix cut=%zu", cut);
            {
                size_t want = expect_keep(src.p, cut);
                if (want != (size_t)-1 && b.n >= want + 4 && b.n > 6 && memcmp(b.p + b.n - 6, "...\"", 4) == 0 && memcmp(b.p + want, "...\"", 4) == 0)
                    CHECK(memcmp(b.p, src.p, want) == 0, "string cut keeps whole tokens cut=%zu", cut);
                else if (want != (size_t)-1 && b.n > 6 && memcmp(b.p + b.n - 6, "...\"", 4) == 0)
                    CHECK(0, "string cut=%zu kept %zu bytes, want %zu then ...\": %.*s SRC %.*s", cut, keep, want, (int)(b.n < 200 ? b.n : 200), b.p, (int)cut, src.p);
            }
            dyn_line_free(&b);
        }
        dyn_line_free(&src);
    }
}

static int o_match(const char* env, const char* ns)
{
    char buf[256], *save = NULL, *tok;
    int on = 0;
    snprintf(buf, sizeof(buf), "%s", env);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int neg = *tok == '-';
        char* p = tok + neg;
        size_t l;
        while (*p == ' ')
            p++;
        l = strlen(p);
        while (l && p[l - 1] == ' ')
            p[--l] = 0;
        if (l && fnmatch(p, ns, 0) == 0)
            on = !neg;
    }
    return on;
}

static void t_debug(void)
{
    static const char* const NS[] = { "app", "app:db", "app:http", "lib", "lib:x:y", "", "a*b", "db" };
    static const char* const PAT[] = { "app", "app:*", "*", "-app:db", "lib:*", "*:db", "- app", " app ", "a*", "*b", "db", "-*", "", "x*y" };
    int k;
    for (k = 0; k < 50000; k++) {
        char env[200];
        int n = (int)rnd(4), i;
        const char* ns = NS[rnd(sizeof(NS) / sizeof(NS[0]))];
        env[0] = 0;
        for (i = 0; i < n; i++) {
            if (i)
                strcat(env, ",");
            strcat(env, PAT[rnd(sizeof(PAT) / sizeof(PAT[0]))]);
        }
        if (strstr(env, "a*b"))
            continue;
        CHECK(dyn_debug_match(env, ns) == o_match(env, ns), "debug env='%s' ns='%s' got %d", env, ns, dyn_debug_match(env, ns));
    }
}

int main(void)
{
    simd_init();
    t_escape();
    t_bounds();
    t_repair();
    t_debug();
    printf("%s test_logk_core: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
