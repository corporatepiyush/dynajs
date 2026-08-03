/* test_crypto_reuse.js -- the COMPILED-CAPABILITY contract for dyna:crypto's
 * Hasher (STDLIB_OOP_PLAN.md §1.3/§1.5).
 *
 * A capability is built once from its configuration and reused across unbounded
 * inputs, so two properties have to hold that a one-shot API never has to:
 *
 *   1. REUSE EQUIVALENCE -- one Hasher driven through reset() must produce, for
 *      every message, exactly what a freshly constructed Hasher produces. If it
 *      does not, "hoist the hasher out of the loop" is silently wrong.
 *   2. REENTRANCY SAFETY -- argument coercion runs arbitrary user JS, and that
 *      JS can call back into THE SAME hasher. The existing coerce-before-resolve
 *      rule protects against a valueOf that CLOSES the object; it says nothing
 *      about one that USES it. The state must stay consistent and the result
 *      must be the documented byte order, never corruption.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_crypto_reuse.js
 * Prints "test_crypto_reuse: all N tests passed" on success; throws on failure. */

import { SHA256Hex, MD5Hex, SHA1, SHA1Hex, Hasher } from "dyna:hash";
import { Base64Encode } from "dyna:encoding";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}

const ALGOS = ["md5", "sha1", "sha224", "sha256", "sha384", "sha512"];

/* ---- 1. reuse equivalence: reset() == a fresh Hasher ------------------- */
{
    /* Messages chosen to straddle every block boundary that matters: empty, a
     * short string, exactly one 64-byte block, one 128-byte block, and a length
     * that forces the padding to spill into an extra block for both widths. */
    const MSGS = ["", "a", "abc", "x".repeat(55), "x".repeat(56), "x".repeat(64),
                  "y".repeat(111), "y".repeat(112), "y".repeat(128),
                  "z".repeat(1000)];

    for (const algo of ALGOS) {
        const reused = new Hasher(algo);
        for (const m of MSGS) {
            const fresh = new Hasher(algo);
            const want = fresh.update(m).digestHex();
            reused.reset();
            const got = reused.update(m).digestHex();
            eq(got, want, `reuse ${algo} len=${m.length}`);
        }
        /* Interleaving reset() with a *partial* message must not leak the
         * abandoned prefix into the next digest -- the real reuse hazard. */
        reused.update("CONTAMINANT");
        reused.reset();
        eq(reused.update("abc").digestHex(),
           new Hasher(algo).update("abc").digestHex(),
           `reset() discards a partial message (${algo})`);
    }
}

/* ---- 2. digest() does not consume the streaming state ------------------ */
{
    const h = new Hasher("sha256");
    h.update("ab");
    const d1 = h.digestHex();
    const d2 = h.digestHex();
    eq(d2, d1, "digestHex() is idempotent (state survives finalization)");
    eq(d1, SHA256Hex("ab"), "partial digest matches the one-shot");
    h.update("c");
    eq(h.digestHex(), SHA256Hex("abc"),
       "the hasher keeps absorbing after a digest");
}

/* ---- 3. reentrancy: user JS that USES the same hasher mid-coercion -----
 *
 * THE HOOK IS toString(), NOT valueOf(). dyna:crypto coerces a non-buffer
 * argument with ToString, and ToString on an ordinary object calls toString()
 * (inherited from Object.prototype) and never reaches valueOf(). So the
 * `{valueOf(){ ... }}` attack that CLAUDE.md §8 prescribes SILENTLY DOES NOT
 * FIRE against this module -- the argument just becomes "[object Object]" and
 * the test passes while exercising nothing. The first assertion below pins that
 * trap in place so a future reader cannot re-introduce a vacuous test. */
{
    const h = new Hasher("sha256");
    h.update("A");
    let fired = false;
    h.update({ valueOf() { fired = true; h.update("B"); return "C"; } });
    assert(!fired, "valueOf is NOT the coercion hook here (ToString wins)");
    eq(h.digestHex(), SHA256Hex("A[object Object]"),
       "a valueOf-only object coerces via Object.prototype.toString");
}
{
    /* The real attack. update() coerces its argument to C locals BEFORE
     * resolving the native handle, and the native absorb itself runs no JS, so
     * the inner update() completes ENTIRELY first and the outer call's own bytes
     * land after it: "A", then "B", then "C". The assertion is that the state is
     * a consistent hash of that well-defined sequence -- not garbage, not a
     * crash, not a lost update. */
    const h = new Hasher("sha256");
    h.update("A");
    h.update({ toString() { h.update("B"); return "C"; } });
    eq(h.digestHex(), SHA256Hex("ABC"),
       "reentrant update() during coercion: inner bytes absorbed first");
}
{
    /* A toString that resets: the reset takes effect, then the outer call's
     * coerced bytes are absorbed into the fresh state. */
    const h = new Hasher("md5");
    h.update("DISCARDED");
    h.update({ toString() { h.reset(); return "abc"; } });
    eq(h.digestHex(), MD5Hex("abc"),
       "reentrant reset() during coercion: prefix discarded, no corruption");
}
{
    /* A toString that digests: the inner digest sees only what was absorbed so
     * far, and the outer update still lands afterwards. */
    const h = new Hasher("sha256");
    h.update("A");
    let inner = null;
    h.update({ toString() { inner = h.digestHex(); return "B"; } });
    eq(inner, SHA256Hex("A"), "reentrant digest() sees the prefix");
    eq(h.digestHex(), SHA256Hex("AB"), "outer update lands after the inner digest");
}
{
    /* Deep reentrancy: nested toString calls must not overflow or interleave
     * partial block writes. Each level contributes one byte; the innermost
     * completes first, so the absorbed order is the reverse of the nesting. */
    const h = new Hasher("sha256");
    let depth = 0;
    const nest = {
        toString() {
            const me = String.fromCharCode(65 + depth);
            if (depth < 8) { depth++; h.update(nest); }
            return me;
        },
    };
    h.update(nest);
    /* depth 0 emits "A" but recurses first, so the bytes land innermost-out:
     * "I" (depth 8, no recursion) then H, G, ... then A. */
    eq(h.digestHex(), SHA256Hex("IHGFEDCBA"),
       "8-deep reentrancy absorbs innermost-first with no lost or duplicated byte");
}

/* ---- 4. an unknown algorithm is rejected at construction --------------- */
{
    n++;
    let threw = false;
    try { new Hasher("sha3-256"); } catch (e) { threw = e instanceof TypeError; }
    if (!threw) throw new Error("assertion failed: unknown algorithm must throw TypeError");
}

/* ---- 5. RFC 6455 accept key: dyna:net's handshake rides on this library
 *
 * dyna:net's dws_accept() computes base64(SHA1(clientKey + GUID)) and used to
 * carry its own compact SHA-1; it now calls core/dyn-hash. Nothing in
 * tests/test_http.js exercises the handshake, so this pins the exact computation
 * the WebSocket upgrade depends on against the published RFC 6455 §1.3 vector.
 * (An end-to-end upgrade test is still missing -- a pre-existing gap.) */
{
    const key = "dGhlIHNhbXBsZSBub25jZQ==";
    const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    eq(SHA1Hex(key + GUID), "b37a4f2cc0624f1690f64606cf385945b2bec4ea",
       "RFC 6455 accept: SHA1 of key||GUID");
    eq(Base64Encode(SHA1(key + GUID)), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
       "RFC 6455 accept: the base64 the client compares against");
}

/* ---- 6. every algorithm's reported metadata matches its output --------- */
{
    for (const algo of ALGOS) {
        const h = new Hasher(algo);
        eq(h.algorithm, algo, `algorithm getter (${algo})`);
        eq(h.digest().length, h.digestSize, `digestSize matches digest() (${algo})`);
    }
}

console.log("test_crypto_reuse: all " + n + " tests passed");
