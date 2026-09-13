/* Fuzz target for the pypi-adoption-plan parsers (docs/report/
 * pypi-adoption-plan-2026-08.md, section 6 rule 7: every parser added is a
 * fuzz target day one). Covers the landed P1 surfaces that had NO target:
 *   dyna:url       IDNA + punycode (3.3)
 *   dyna:config    TOML (3.13)
 *   dyna:serialize protobuf + ASN.1 (3.7, 3.12)
 *   dyna:schema    JSON Schema (3.10)
 *   dyna:net       multipart/form-data (3.14)
 *   dyna:crypto    bcrypt/scrypt/argon2id parameter validation (3.5a)
 *
 * TWO INPUTS, on purpose (fuzz_stdlib.c's rule): FUZZ is the bytes as a JS
 * STRING (the only way a caller reaches a text parser); FUZZB is the SAME
 * bytes as a Uint8Array, because a length-prefixed binary format must be
 * driven by the real bytes -- a string round trip replaces every invalid
 * UTF-8 sequence and can never produce the declared-length attack the
 * decoders are bounded against. Both buffers are allocated at EXACTLY the
 * input size, so a one-past-the-end read is a heap overflow, not a read
 * into spare capacity that reports clean.
 *
 * Run with -max_len=8192. Build with
 * LIB_FUZZING_ENGINE="-fsanitize=fuzzer,address" to catch silent overreads.
 */

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
    JSValue v = JS_Eval(ctx, src, strlen(src), "<parsers-fuzz>",
                        JS_EVAL_TYPE_MODULE);
    drop(ctx, v);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    JSRuntime *rt;
    JSContext *ctx;
    JSValue global, s;
    char *exact;

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

    exact = (char *)malloc(size ? size : 1);
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
    JS_SetPropertyStr(ctx, global, "FUZZ", s);   /* consumes s */
    {
        static const uint8_t stub = 0;
        JSValue ab = JS_NewArrayBufferCopy(ctx, size ? data : &stub, size);
        if (!JS_IsException(ab)) {
            JSValueConst ta[3];
            JSValue u8;
            ta[0] = ab; ta[1] = JS_UNDEFINED; ta[2] = JS_UNDEFINED;
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
    /* IDNA: every entry point over the same bytes. domainToASCII/
       domainToUnicode exercise the mapping/NFC/label-split pipeline;
       the punycode pair exercises the raw codec both directions. */
    run(ctx,
        "import { domainToASCII, domainToUnicode, punycodeEncode, punycodeDecode }"
        "  from 'dyna:url';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => domainToASCII(FUZZ)); t(() => domainToUnicode(FUZZ));\n"
        "t(() => punycodeEncode(FUZZ)); t(() => punycodeDecode(FUZZ));\n");

    /* TOML: the grammar validator, the value builder, and stringify over
       whatever parsed. */
    run(ctx,
        "import { TOML } from 'dyna:config';\n"
        "const t = f => { try { return f(); } catch (e) { return null; } };\n"
        "const v = t(() => TOML.parse(FUZZ));\n"
        "if (v !== null) t(() => TOML.stringify(v));\n");

    /* Protobuf + ASN.1: length-prefixed BINARY formats, driven over FUZZB
       (a string round trip can never produce a declared-length attack).
       Empty fields exercises the unknown-field preservation path (its
       amplification cap); the permissive schema exercises the value paths. */
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

    /* JSON Schema: compile and validate only when the input IS a schema --
       the compile path (ref-DAG, regex compilation) and the validate path
       (recursion caps) are distinct, drive both. */
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

    /* Multipart: the BODY over FUZZB with a fixed boundary; the whole
       parser state machine (boundary search, header caps, part caps). */
    run(ctx,
        "import { MultipartParse } from 'dyna:net';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => MultipartParse('multipart/form-data; boundary=AaB03x', FUZZB));\n");

    /* X.509 certificates: PEM string and DER bytes */
    run(ctx,
        "import { X509 } from 'dyna:crypto';\n"
        "const t = f => { try { f(); } catch (e) {} };\n"
        "if (typeof X509 !== 'undefined') {\n"
        "  t(() => X509.parse(FUZZ));\n"
        "  t(() => X509.parse(FUZZB));\n"
        "}\n");

    /* Fetch primitives: Headers, Request, Response, FormData */
    run(ctx,
        "const t = f => { try { f(); } catch (e) {} };\n"
        "t(() => new Headers({ 'X-Fuzz': FUZZ }));\n"
        "t(() => new Response(FUZZ));\n"
        "t(() => new Response(FUZZB));\n"
        "t(() => new Request('http://localhost/' + encodeURIComponent(FUZZ.slice(0, 100))));\n");

    /* KDFs: parameter validation is the DoS boundary, so drive the FULL
       ranges with bounded inputs -- an out-of-range N/cost/memory must
       refuse, and an in-range one must run. */
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

    /* dyna:scrape (2026-08 modernization): the robots GROUP parser is
       attacker-influenced input driven twice over -- once as the file body,
       once as the AGENT that group selection matches; Fetcher drives
       fe_resolve with a hostile 30x Location and Extractor's option parser
       with hostile attr/as strings (long keys must trip the %.39s refusal,
       not a buffer). */
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
