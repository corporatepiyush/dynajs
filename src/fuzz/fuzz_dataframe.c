// Fuzz target for dyna:dataframe (src/dyna-dataframe.c).
//
// WHY THIS EXISTS: codegraph's untrusted-unfuzzed query (Q27) listed
// dfc_moving_blocks, dfc_set_grow and friends as doing caller-controlled size
// arithmetic with NO fuzz target reaching them. The module is ~160 methods of
// index arithmetic over borrowed TypedArray backing stores -- window bounds,
// group codes used as array indices, quantile positions, prefix-sum offsets --
// and every one of those is a wrong-answer-or-overread away from a fuzzer.
//
// IT ENUMERATES THE PROTOTYPE, IT DOES NOT LIST METHODS. A target that names
// its surface by hand goes stale the moment a method lands, and its own comment
// is what stops anyone re-checking. Reading the prototype from the BINARY means
// a new method is covered the day it is registered.
//
// THE BYTES BECOME COLUMNS, NOT TEXT. A Float64Array viewed over the fuzzer's
// buffer holds whatever bit patterns it chose -- signalling NaNs, denormals,
// +/-0, 1e308 -- which is exactly the input the reduction kernels' NaN and
// overflow paths are written for and which a generator of "plausible" numbers
// never produces. The integer column doubles as the GROUP key, so the fuzzer
// steers group codes directly at the code that indexes tables with them.
//
// EXACTLY `size` BYTES: JS_NewArrayBufferCopy allocates the input's length with
// no slack, and every view is taken over that buffer, so a read one past a
// column's end is a reported heap overflow rather than a read into spare
// capacity that reports clean.
//
// RUN IT WITH src/fuzz/corpus_dataframe, NOT COLD. The interesting thresholds
// need thousands of rows (DFO_RADIX_MIN 2048; a 256-wide window needs a group
// longer than 256, so >2 KiB of input), and libFuzzer grows its length limit on
// a TIME schedule -- a cold 3000-run sweep never gets there. Measured: an
// abort() planted in dfc_moving_blocks was NOT hit in 3000 cold runs and was
// hit immediately from the seeds. A cold run reports clean while covering none
// of what this target exists for.
//
// The default LIB_FUZZING_ENGINE is -fsanitize=fuzzer with NO address
// sanitizer, which catches crashes and hangs but not a silent overread. Build
// with LIB_FUZZING_ENGINE="-fsanitize=fuzzer,address" to catch those.

#include "dynajs.h"
#include "dyna-nat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void drop(JSContext *ctx, JSValue v) {
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, v);
}

static void run(JSContext *ctx, const char *src) {
    JSValue v = JS_Eval(ctx, src, strlen(src), "<dataframe-fuzz>",
                        JS_EVAL_TYPE_MODULE);
    drop(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    JSRuntime *rt;
    JSContext *ctx;
    JSValue global;

    // 64 KiB is well past every threshold in the module (DFO_RADIX_MIN 2048,
    // DFG_ROLL_SLIDE_MIN 256, DFM_UNROLL_MIN 64) and keeps a run inside budget.
    if (size < 16 || size > 65536)
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
#ifdef CONFIG_NATIVE_MODULES
    if (js_nat_init_all(ctx) < 0) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
#endif

    {   // The raw bytes, allocated at EXACTLY `size`.
        JSValue ab = JS_NewArrayBufferCopy(ctx, data, size);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 0;
        }
        global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "FUZZAB", ab);   // consumes ab
        JS_FreeValue(ctx, global);
    }

#ifdef CONFIG_NATIVE_MODULES
    // Columns are VIEWS over the fuzzer's own buffer, so the float column holds
    // whatever bit patterns it chose. The key column is Int32 because the
    // group-by and bitwise families refuse a float key -- with one they never
    // run, and the target would fuzz the argument check instead of the kernel.
    run(ctx,
        "import { DataFrame } from 'dyna:dataframe';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const n8 = FUZZAB.byteLength;\n"
        "const nf = Math.floor(n8 / 8);\n"          /* f64 elements available */
        "if (nf >= 2) {\n"
        "  const rows = nf;\n"
        "  const f = new Float64Array(FUZZAB, 0, rows);\n"
        /* a second float column from the same bytes, rotated, so pairwise
           statistics see two DIFFERENT attacker-chosen columns */
        "  const g = new Float64Array(rows);\n"
        "  for (let i = 0; i < rows; i++) g[i] = f[(i + 1) % rows];\n"
        /* integer columns: the low bytes drive group codes, which the module
           uses directly as table indices */
        /* z is CONSTANT: one group holding every row. Without it the block
           path (w >= 256 AND a group longer than w) needs the fuzzer to find a
           uniform key byte, and that path stays unreached for a long time. */
        "  const k = new Int32Array(rows), u = new Uint32Array(rows), z = new Int32Array(rows);\n"
        "  const b = new Uint8Array(FUZZAB);\n"
        "  for (let i = 0; i < rows; i++) { k[i] = b[i % n8]; u[i] = b[(i * 3) % n8]; }\n"
        "  const w = new Float64Array(rows);\n"
        "  for (let i = 0; i < rows; i++) w[i] = b[(i * 5) % n8];\n"
        "  const df = t(() => new DataFrame({ f, g, k, u, w, z }));\n"
        "  if (df) {\n"
        /* the mask is attacker-chosen too: all-set, all-clear and arbitrary are
           three different paths through every masked kernel */
        "    const m = new Uint8Array(rows);\n"
        "    for (let i = 0; i < rows; i++) m[i] = b[(i * 7) % n8] & 1;\n"
        "    const masks = [undefined, m, new Uint8Array(rows).fill(1), new Uint8Array(rows)];\n"
        /* scalars steered by the input, so the fuzzer can drive window bounds,
           quantile positions and k straight at the index arithmetic */
        "    const S = [b[0], b[1] | 0, b[2] / 255, 1 + (b[3] & 63), rows, rows - 1, 0, 1];\n"
        "    const names = ['f', 'g', 'k', 'u', 'w', 'z'];\n"
        /* drive the one-group case at the windowed kernels directly, so the
           block path does not depend on the enumeration picking 'z' */
        "    for (const ww of [S[0], S[3], 256, 257, rows - 1]) {\n"
        "      t(() => df.GROUP_ARRAY_MOVING_SUM('z', 'f', ww));\n"
        "      t(() => df.GROUP_ARRAY_MOVING_AVG('z', 'f', ww));\n"
        "      t(() => df.ROLLING_SUM('f', ww)); t(() => df.ROLLING_VAR('f', ww));\n"
        "    }\n"
        "    const proto = Object.getPrototypeOf(df);\n"
        /* ENUMERATED FROM THE BINARY: a method added tomorrow is covered today */
        "    for (const meth of Object.getOwnPropertyNames(proto)) {\n"
        "      if (meth === 'constructor') continue;\n"
        "      const d = Object.getOwnPropertyDescriptor(proto, meth);\n"
        "      if (!d || d.get || typeof d.value !== 'function') continue;\n"
        "      const c1 = names[b[4] % names.length], c2 = names[b[5] % names.length];\n"
        "      const s1 = S[b[6] % S.length], s2 = S[b[7] % S.length];\n"
        "      for (const mk of masks) {\n"
        "        t(() => proto[meth].call(df, c1, mk));\n"
        "        t(() => proto[meth].call(df, c1, c2, mk));\n"
        "        t(() => proto[meth].call(df, c1, s1, mk));\n"
        "        t(() => proto[meth].call(df, c1, c2, s1, mk));\n"
        "        t(() => proto[meth].call(df, c1, s1, s2, mk));\n"
        "        t(() => proto[meth].call(df, [c1, c2], mk));\n"
        "        t(() => proto[meth].call(df, mk));\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "}\n");

    // A STRING column is a different representation entirely: dictionary-
    // encoded and COPIED at construction, where the numeric columns alias. It
    // reaches the dictionary builder and the string-column refusals, neither of
    // which the loop above can touch.
    run(ctx,
        "import { DataFrame } from 'dyna:dataframe';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const b = new Uint8Array(FUZZAB), rows = Math.min(b.length, 512);\n"
        "if (rows >= 2) {\n"
        "  const s = [], v = new Float64Array(rows);\n"
        "  for (let i = 0; i < rows; i++) {\n"
        "    s.push(String.fromCharCode(b[i], b[(i + 1) % rows]));\n"
        "    v[i] = b[(i * 11) % rows];\n"
        "  }\n"
        "  const df = t(() => new DataFrame({ s, v }));\n"
        "  if (df) {\n"
        "    t(() => df.GROUP_BY_SUM('s', 'v'));\n"
        "    t(() => df.GROUP_ARRAY('s', 'v'));\n"
        "    t(() => df.GROUP_ARRAY_MOVING_SUM('s', 'v', 1 + (b[0] & 15)));\n"
        "    t(() => df.GROUP_UNIQ_ARRAY('s', 'v'));\n"
        "    t(() => df.GROUP_CONCAT('s'));\n"
        "    t(() => df.VALUE_COUNTS('s'));\n"
        "    t(() => df.SUM('s'));\n"       /* must REFUSE a string column */
        "    t(() => df.SORT('s'));\n"
        "    t(() => df.CORR_MATRIX(['s', 'v']));\n"
        "  }\n"
        "}\n");
#endif

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
