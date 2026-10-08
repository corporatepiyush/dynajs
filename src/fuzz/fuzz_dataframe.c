#include "dynajs.h"
#include "dyna-nat.h"
#include "src/fuzz/fuzz_common.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void run(JSContext* ctx, const char* src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<dataframe-fuzz>",
        JS_EVAL_TYPE_MODULE);
    fuzz_drain_or_abort(ctx, v);
    JS_FreeValue(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    JSRuntime* rt;
    JSContext* ctx;
    JSValue global;

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

    {
        JSValue ab = JS_NewArrayBufferCopy(ctx, data, size);
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 0;
        }
        global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "FUZZAB", ab);
        JS_FreeValue(ctx, global);
    }

#ifdef CONFIG_NATIVE_MODULES
    run(ctx,
        "import { DataFrame } from 'dyna:dataframe';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const n8 = FUZZAB.byteLength;\n"
        "const nf = Math.floor(n8 / 8);\n"
        "if (nf >= 2) {\n"
        "  const rows = nf;\n"
        "  const f = new Float64Array(FUZZAB, 0, rows);\n"
        "  const g = new Float64Array(rows);\n"
        "  for (let i = 0; i < rows; i++) g[i] = f[(i + 1) % rows];\n"
        "  const k = new Int32Array(rows), u = new Uint32Array(rows), z = new Int32Array(rows);\n"
        "  const b = new Uint8Array(FUZZAB);\n"
        "  for (let i = 0; i < rows; i++) { k[i] = b[i % n8]; u[i] = b[(i * 3) % n8]; }\n"
        "  const w = new Float64Array(rows);\n"
        "  for (let i = 0; i < rows; i++) w[i] = b[(i * 5) % n8];\n"
        "  const df = t(() => new DataFrame({ f, g, k, u, w, z }));\n"
        "  if (df) {\n"
        "    const m = new Uint8Array(rows);\n"
        "    for (let i = 0; i < rows; i++) m[i] = b[(i * 7) % n8] & 1;\n"
        "    const masks = [undefined, m, new Uint8Array(rows).fill(1), new Uint8Array(rows)];\n"
        "    const S = [b[0], b[1] | 0, b[2] / 255, 1 + (b[3] & 63), rows, rows - 1, 0, 1];\n"
        "    const names = ['f', 'g', 'k', 'u', 'w', 'z'];\n"
        "    for (const ww of [S[0], S[3], 256, 257, rows - 1]) {\n"
        "      t(() => df.GROUP_ARRAY_MOVING_SUM('z', 'f', ww));\n"
        "      t(() => df.GROUP_ARRAY_MOVING_AVG('z', 'f', ww));\n"
        "      t(() => df.ROLLING_SUM('f', ww)); t(() => df.ROLLING_VAR('f', ww));\n"
        "    }\n"
        "    const proto = Object.getPrototypeOf(df);\n"
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
        "    t(() => df.SUM('s'));\n"
        "    t(() => df.SORT('s'));\n"
        "    t(() => df.CORR_MATRIX(['s', 'v']));\n"
        "  }\n"
        "}\n");
#endif

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
