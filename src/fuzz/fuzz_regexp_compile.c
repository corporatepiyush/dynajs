#include "dynajs.h"
#include "dyna-libc.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int nbinterrupts;

static int interrupt_handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    nbinterrupts++;
    return (nbinterrupts > 100);
}

static void drop(JSContext* ctx, JSValue v)
{
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    JSRuntime* rt;
    JSContext* ctx;
    const uint8_t *pattern, *flags;
    size_t pattern_len, flags_len, i;
    size_t valid_idx = 0, esc_idx = 0, slash_idx = 0;
    int seen_u = 0, seen_v = 0;
    char valid_flags[16] = "";
    char escaped_pattern[4096];
    char slash_escaped[2048];
    char script[8192];
    char literal_script[4096];

    if (size < 2 || size > 65536)
        return 0;

    rt = JS_NewRuntime();
    if (!rt)
        return 0;
    ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 0;
    }
    JS_SetMemoryLimit(rt, 0x4000000);
    JS_SetMaxStackSize(rt, 0x40000);
    nbinterrupts = 0;
    JS_SetInterruptHandler(rt, interrupt_handler, NULL);

    pattern_len = size / 2;
    flags_len = size - pattern_len;
    pattern = data;
    flags = data + pattern_len;

    for (i = 0; i < flags_len && valid_idx < sizeof(valid_flags) - 1; i++) {
        int c = flags[i];
        if (c == 0 || !strchr("dgimsuvy", c))
            continue;
        if (memchr(valid_flags, c, valid_idx))
            continue;
        if (c == 'u') {
            if (seen_v)
                continue;
            seen_u = 1;
        }
        if (c == 'v') {
            if (seen_u)
                continue;
            seen_v = 1;
        }
        valid_flags[valid_idx++] = (char)c;
    }
    valid_flags[valid_idx] = '\0';

    for (i = 0; i < pattern_len && esc_idx < sizeof(escaped_pattern) - 2; i++) {
        int c = pattern[i];
        if (c == '\\' || c == '"' || c == '\n' || c == '\r' || c == '\t')
            escaped_pattern[esc_idx++] = '\\';
        escaped_pattern[esc_idx++] = (char)c;
    }
    escaped_pattern[esc_idx] = '\0';

    snprintf(script, sizeof(script), "new RegExp(\"%s\", \"%s\")",
        escaped_pattern, valid_flags);

    {
        JSValue re = JS_Eval(ctx, script, strlen(script), "<regexp>", 0);
        if (!JS_IsException(re)) {
            static const char* const subjects[] = {
                "'test string'",
                "''",
                "'aaaaaaaaaa'",
                "'1234567890'",
                "'!@#$%^&*()'",
            };
            size_t k;
            for (k = 0; k < sizeof(subjects) / sizeof(subjects[0]); k++) {
                char match_script[sizeof(script) + 256];
                snprintf(match_script, sizeof(match_script),
                    "var re = %s; re.test(%s); re.exec(%s); %s.match(re);",
                    script, subjects[k], subjects[k], subjects[k]);
                drop(ctx, JS_Eval(ctx, match_script, strlen(match_script), "<regexp-match>", 0));
            }
        }
        drop(ctx, re);
    }

    for (i = 0; i < pattern_len && slash_idx < sizeof(slash_escaped) - 2; i++) {
        int c = pattern[i];
        if (c == '/')
            slash_escaped[slash_idx++] = '\\';
        slash_escaped[slash_idx++] = (char)c;
    }
    slash_escaped[slash_idx] = '\0';

    snprintf(literal_script, sizeof(literal_script), "/%s/%s.test('test')",
        slash_escaped, valid_flags);
    drop(ctx, JS_Eval(ctx, literal_script, strlen(literal_script), "<regexp-literal>", 0));

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
