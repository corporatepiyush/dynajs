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
    JSValue v = JS_Eval(ctx, src, strlen(src), "<parsers-fuzz>",
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
        "import { domainToASCII, domainToUnicode, punycodeEncode, punycodeDecode }"
        "  from 'dyna:url';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => domainToASCII(FUZZ)); t(() => domainToUnicode(FUZZ));\n"
        "t(() => punycodeEncode(FUZZ)); t(() => punycodeDecode(FUZZ));\n");

    run(ctx,
        "import { TOML } from 'dyna:config';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const v = t(() => TOML.parse(FUZZ));\n"
        "if (v !== null) t(() => TOML.stringify(v));\n");

    run(ctx,
        "import { Proto, ASN1 } from 'dyna:serialize';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const a = t(() => Proto.decode(FUZZB, { fields: [] }));\n"
        "if (a !== null) t(() => Proto.encode(a, { fields: [] }));\n"
        "const b = t(() => Proto.decode(FUZZB, { fields: [\n"
        "  { number: 1, name: 'a', type: 'int32' },\n"
        "  { number: 2, name: 'b', type: 'string' },\n"
        "  { number: 3, name: 'c', type: 'message', message: { fields: [\n"
        "    { number: 1, name: 'x', type: 'bytes' } ] } } ] }));\n"
        "if (b !== null) t(() => Proto.encode(b, { fields: [] }));\n"
        "const d = t(() => ASN1.decode(FUZZB));\n"
        "if (d !== null) t(() => ASN1.encode(d));\n");

    run(ctx,
        "import { Schema } from 'dyna:schema';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const v = t(() => JSON.parse(FUZZ));\n"
        "if (v && typeof v === 'object') {\n"
        "  const c = t(() => Schema.compile(v));\n"
        "  if (c) { t(() => c.validate(v)); t(() => c.validate(null));\n"
        "           t(() => c.validate(0)); t(() => c.validate('')); }\n"
        "  t(() => Schema.validate(v, v));\n"
        "}\n");

    run(ctx,
        "import { MultipartParse } from 'dyna:net';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => MultipartParse('multipart/form-data; boundary=AaB03x', FUZZB));\n");

    run(ctx,
        "import { X509 } from 'dyna:crypto';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "if (typeof X509 !== 'undefined') {\n"
        "  t(() => X509.parse(FUZZ));\n"
        "  t(() => X509.parse(FUZZB));\n"
        "}\n");

    run(ctx,
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => new Headers({ 'X-Fuzz': FUZZ }));\n"
        "t(() => new Response(FUZZ));\n"
        "t(() => new Response(FUZZB));\n"
        "t(() => new Request('http://localhost/' + encodeURIComponent(FUZZ.slice(0, 100))));\n");

    run(ctx,
        "import { Bcrypt, Scrypt, Argon2id } from 'dyna:crypto';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => Bcrypt.hash(FUZZ, 4));\n"
        "t(() => Bcrypt.hash(FUZZ, 32));\n"
        "t(() => Scrypt(FUZZ, FUZZB, { N: 2, r: 1, p: 1, keyLen: 32 }));\n"
        "t(() => Scrypt(FUZZ, FUZZB, { N: 3, r: 1, p: 1, keyLen: 32 }));\n"
        "t(() => Argon2id.hash(FUZZ, FUZZB, { iterations: 1, memory: 64,"
        "  parallelism: 1, hashLen: 32 }));\n"
        "t(() => Argon2id.hash(FUZZ, FUZZB, { iterations: 1, memory: 1 << 30,"
        "  parallelism: 1, hashLen: 32 }));\n");

    run(ctx,
        "import { Robots, Extractor, Fetcher } from 'dyna:scrape';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "const r = new Robots(FUZZ, { agent: FUZZ });\n"
        "t(() => r.allows(FUZZ)); t(() => r.crawlDelay());\n"
        "t(() => r.sitemaps());\n"
        "let first = 0;\n"
        "const cl = { request(m, u) {\n"
        "  if (u.endsWith('/robots.txt')) return { status: 404, headers: {}, body: '' };\n"
        "  if (!first++) return { status: 302, headers: { Location: FUZZ }, body: '' };\n"
        "  return { status: 200, headers: {\n"
        "    'X-Robots-Tag': FUZZ.slice(0, 128),\n"
        "    Link: FUZZ.slice(0, 128) }, body: '<i>x</i>' };\n"
        "} };\n"
        "const fz = new Fetcher({ agent: 'fz/1', client: cl, minDelayMs: 0,\n"
        "  robotsTtlMs: 0 });\n"
        "t(() => fz.get('http://a.test/p'));\n"
        "t(() => fz.get('http://a.test/p2'));\n"
        "t(() => fz.get('http://a.test/p3'));\n");
    run(ctx,
        "import { Extractor } from 'dyna:scrape';\n"
        "import { Selector, HTMLParse } from 'dyna:html';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => new Extractor({ x: { sel: new Selector('*'),\n"
        "  attr: FUZZ.slice(0, 64), as: FUZZ.slice(0, 12) } })\n"
        "  .run(HTMLParse('<i>' + FUZZ + '</i>'), {}));\n");
#endif

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
