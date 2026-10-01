#include "dynajs.h"
#include "dyna-nat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void drop(JSContext* ctx, JSValue v)
{
    if (JS_IsException(v))
        JS_FreeValue(ctx, JS_GetException(ctx));
    else
        JS_FreeValue(ctx, v);
}

static void run(JSContext* ctx, const char* src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<stdlib-fuzz>",
        JS_EVAL_TYPE_MODULE);
    drop(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    JSRuntime* rt;
    JSContext* ctx;
    JSValue global, s;
    char* exact;

    if (size > 65536)
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

    exact = (char*)malloc(size ? size : 1);
    if (!exact) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    if (size)
        memcpy(exact, data, size);
    s = JS_NewStringLen(ctx, exact, size);
    free(exact);
    if (JS_IsException(s)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 0;
    }
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "FUZZ", s);
    {
        static const uint8_t stub = 0;
        JSValue ab = JS_NewArrayBufferCopy(ctx, size ? data : &stub, size);
        if (!JS_IsException(ab)) {
            JSValueConst ta[3];
            JSValue u8;
            ta[0] = ab;
            ta[1] = JS_UNDEFINED;
            ta[2] = JS_UNDEFINED;
            u8 = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT8);
            JS_FreeValue(ctx, ab);
            if (JS_IsException(u8))
                JS_FreeValue(ctx, JS_GetException(ctx));
            else
                JS_SetPropertyStr(ctx, global, "FUZZB", u8);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    JS_FreeValue(ctx, global);

#ifdef CONFIG_NATIVE_MODULES
    run(ctx,
        "import { DiffLines, DiffWords, DiffChars, Levenshtein, DiceCoefficient }"
        "  from 'dyna:matcher';\n"
        "const s = FUZZ, half = s.slice(0, s.length >> 1);\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => DiffLines(s, half)); t(() => DiffLines(half, s));\n"
        "t(() => DiffWords(s, half)); t(() => DiffChars(half, s));\n"
        "t(() => Levenshtein(s, half)); t(() => Levenshtein(half, s, {max: 3}));\n"
        "t(() => DiceCoefficient(s, half));\n");

    run(ctx,
        "import { JSON5Parse, JSON5Stringify, StableStringify } from 'dyna:encoding';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const v = t(() => JSON5Parse(FUZZ));\n"
        "if (v !== null) { t(() => JSON5Stringify(v)); t(() => StableStringify(v)); }\n");

    run(ctx,
        "import { INI, Env, FrontMatter } from 'dyna:config';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => INI.parse(FUZZ)); t(() => Env.parse(FUZZ));\n"
        "t(() => FrontMatter.split(FUZZ));\n");

    run(ctx,
        "import { decode, encode, encodings } from 'dyna:bytes';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "const b = new Uint8Array(FUZZ.length);\n"
        "for (let i = 0; i < b.length; i++) b[i] = FUZZ.charCodeAt(i) & 0xff;\n"
        "for (const label of encodings()) { t(() => decode(b, label));"
        "  t(() => encode(FUZZ, label)); }\n");

    run(ctx,
        "import { ULIDTime, NanoIDAlphabet } from 'dyna:uuid';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => ULIDTime(FUZZ));\n"
        "if (FUZZ.length >= 2 && FUZZ.length <= 256) t(() => NanoIDAlphabet(FUZZ, 32));\n");

    run(ctx,
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => FUZZ.stripAnsi()); t(() => FUZZ.displayWidth());\n"
        "t(() => FUZZ.graphemes()); t(() => FUZZ.wrapAnsi(8, { hard: true }));\n");

    run(ctx,
        "import { XMLParse, XMLStringify, XMLToObject, SAXParser } from 'dyna:xml';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const v = t(() => XMLParse(FUZZ));\n"
        "if (v) { t(() => XMLStringify(v)); t(() => XMLToObject(v)); }\n"
        "t(() => { const p = new SAXParser({});\n"
        "  for (let i = 0; i < FUZZ.length; i += 7) p.write(FUZZ.slice(i, i + 7));\n"
        "  p.end(); });\n"
        "t(() => { const p = new SAXParser({ onOpen(){}, onText(){}, onCData(){},\n"
        "    onComment(){}, onPI(){}, onClose(){} });\n"
        "  for (let i = 0; i < FUZZ.length; i++) p.write(FUZZ[i]);\n"
        "  p.end(); });\n");

    run(ctx,
        "import { Parse, ParseAll, Stringify } from 'dyna:yaml';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const v = t(() => Parse(FUZZ));\n"
        "if (v !== null) t(() => Parse(Stringify(v)));\n"
        "t(() => ParseAll(FUZZ));\n");

    run(ctx,
        "import * as ml from 'dyna:ml';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const names = Object.getOwnPropertyNames(ml)\n"
        "  .filter(n => typeof ml[n] === 'function' && ml[n].deserialize);\n"
        "for (const n of names) {\n"
        "  const M = ml[n];\n"
        "  const mo = t(() => M.deserialize(FUZZB));\n"
        "  if (mo) { t(() => mo.serialize()); }\n"
        "}\n"
        "if (FUZZB.length > 4) {\n"
        "  const half = FUZZB.subarray(0, FUZZB.length >> 1);\n"
        "  for (const n of names) t(() => ml[n].deserialize(half));\n"
        "}\n");

    run(ctx,
        "import { Robots } from 'dyna:scrape';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const r = t(() => new Robots(FUZZ));\n"
        "if (r) {\n"
        "  t(() => r.allows('/'));\n"
        "  t(() => r.allows('/a/b/c/d/e'));\n"
        "  t(() => r.allows(FUZZ));\n"
        "  t(() => r.crawlDelay());\n"
        "  t(() => r.sitemaps());\n"
        "  t(() => r.ruleCount);\n"
        "}\n"
        "t(() => new Robots(FUZZ, { agent: 'dynabot' }));\n"
        "t(() => new Robots(FUZZ, { agent: FUZZ }));\n");

    run(ctx,
        "import { HTMLParse, HTMLStringify, Selector, Sanitizer, MarkdownToHTML }\n"
        "  from 'dyna:html';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const d = t(() => HTMLParse(FUZZ));\n"
        "if (d) t(() => HTMLStringify(d));\n"
        "const san = new Sanitizer({ allow: { p: [], b: [], a: ['href'] },\n"
        "  protocols: { 'a.href': ['https'] } });\n"
        "t(() => san.clean(FUZZ));\n"
        "t(() => MarkdownToHTML(FUZZ));\n"
        "t(() => MarkdownToHTML(FUZZ, { allowRawHTML: true }));\n"
        "t(() => new Selector(FUZZ));\n");

    run(ctx,
        "import { MsgPackDecode, CBORDecode, MsgPackEncode, CBOREncode, ValueHash }\n"
        "  from 'dyna:serialize';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const a = t(() => MsgPackDecode(FUZZB));\n"
        "if (a !== null) { t(() => MsgPackEncode(a)); t(() => ValueHash(a)); }\n"
        "const b = t(() => CBORDecode(FUZZB));\n"
        "if (b !== null) { t(() => CBOREncode(b)); t(() => ValueHash(b)); }\n");

    run(ctx,
        "import { TarList, TarExtract, ZipList, ZipRead } from 'dyna:compress';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "t(() => TarList(FUZZB)); t(() => TarExtract(FUZZB));\n"
        "t(() => TarList(FUZZB, { allowUnsafeNames: true }));\n"
        "const z = t(() => ZipList(FUZZB));\n"
        "if (z) for (const e of z) t(() => ZipRead(FUZZB, e.name));\n");

    run(ctx,
        "import { sniffType } from 'dyna:file';\n"
        "import { QREncode, QRToString } from 'dyna:encoding';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "t(() => sniffType(FUZZB));\n"
        "t(() => QREncode(FUZZ));\n"
        "t(() => QREncode(FUZZ, { ecc: 'H' }));\n"
        "t(() => QRToString(FUZZ));\n");

    run(ctx,
        "import { PlainDate, Duration, parseDate } from 'dyna:time';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const d = t(() => parseDate(FUZZ));\n"
        "if (d) { t(() => d.add(new Duration({ months: 1 }))); t(() => d.toString()); }\n");

    run(ctx,
        "import { JSONPath } from 'dyna:encoding';\n"
        "import { Decimal } from 'dyna:decimal';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const q = t(() => new JSONPath(FUZZ));\n"
        "if (q) { t(() => q.all({ a: [1, { b: 2 }] })); t(() => q.paths([1, 2])); }\n"
        "t(() => new Decimal(FUZZ));\n");
#endif

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
