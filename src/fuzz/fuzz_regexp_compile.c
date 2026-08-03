// Copyright 2025 Google LLC
// Fuzz target for DynaJS RegExp compilation and execution.
//
// The input is split into a pattern and a flag set, escaped into a script the
// harness builds itself, and evaluated. Nothing from `data` reaches JS_Eval
// directly, so unlike fuzz_eval/fuzz_json this target needs NO NUL-terminated
// copy: it indexes the raw libFuzzer buffer, which is exactly sized, so a read
// one past the end is a heap overflow ASan reports.
//
// KNOWN LIMIT: the script is assembled with "%s", so a pattern containing 0x00
// is truncated there. Embedded NULs are covered by fuzz_eval/fuzz_json, which
// pass an explicit length. The escaper also renders \n as backslash-newline,
// which JS reads as a line continuation -- the byte is deleted, not
// represented. Switching to \xNN escapes changes what this target covers.

#include "dynajs.h"
#include "dyna-libc.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Bounds the work one input may demand. Mechanism from fuzz_common.c:21-25,38,
// inlined so this target links against libdynajs.fuzz.a alone. Without it
// lre_check_timeout() returns 0 (regexp.inc.c:459) and backtracking is unbounded.
static int nbinterrupts;

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    (void)rt; (void)opaque;
    nbinterrupts++;
    return (nbinterrupts > 100);
}

// fuzz_bytecode.c:36-42. A bare JS_GetException() drops an owned value.
static void drop(JSContext *ctx, JSValue v)
{
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    JSRuntime *rt;
    JSContext *ctx;
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
    // Cap pathological allocations / recursion (64 MiB, 256 KiB stack):
    // fuzz_bytecode.c:53-55 verbatim.
    JS_SetMemoryLimit(rt, 0x4000000);
    JS_SetMaxStackSize(rt, 0x40000);
    // One reset per input, so the cap bounds the input's TOTAL work across all
    // the evals below rather than each one separately.
    nbinterrupts = 0;
    JS_SetInterruptHandler(rt, interrupt_handler, NULL);

    // Raw, exactly-sized libFuzzer buffer. size >= 2 makes both halves non-empty.
    pattern_len = size / 2;
    flags_len = size - pattern_len;
    pattern = data;
    flags = data + pattern_len;

    // The engine accepts dgimsuvy (regexp.inc.c:33-56), rejects a repeat, and
    // rejects u with v -- each of those throws before any pattern is compiled,
    // so filter here instead of wasting the input.
    for (i = 0; i < flags_len && valid_idx < sizeof(valid_flags) - 1; i++) {
        int c = flags[i];
        // strchr(s, 0) returns the TERMINATOR, so a 0x00 byte would otherwise
        // pass as a valid flag.
        if (c == 0 || !strchr("dgimsuvy", c))
            continue;
        // Explicit length: valid_flags is not a NUL-terminated string yet, and
        // strchr over the unwritten tail is the read this target shipped with.
        if (memchr(valid_flags, c, valid_idx))
            continue;
        if (c == 'u') { if (seen_v) continue; seen_u = 1; }
        if (c == 'v') { if (seen_u) continue; seen_v = 1; }
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
            static const char *const subjects[] = {
                "'test string'", "''", "'aaaaaaaaaa'",
                "'1234567890'", "'!@#$%^&*()'",
            };
            size_t k;
            for (k = 0; k < sizeof(subjects) / sizeof(subjects[0]); k++) {
                // Must hold all of `script` plus the wrapper: at 4096 every
                // pattern over ~4000 bytes became a syntax error and this
                // whole path was dead.
                char match_script[sizeof(script) + 256];
                snprintf(match_script, sizeof(match_script),
                         "var re = %s; re.test(%s); re.exec(%s); %s.match(re);",
                         script, subjects[k], subjects[k], subjects[k]);
                drop(ctx, JS_Eval(ctx, match_script, strlen(match_script),
                                  "<regexp-match>", 0));
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
    drop(ctx, JS_Eval(ctx, literal_script, strlen(literal_script),
                      "<regexp-literal>", 0));

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
