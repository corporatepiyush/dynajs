// flags: --std
/* test_hasher_lifecycle.js -- hash.Hasher carries the full DynResource
 * surface (audit X-10).
 *
 * Its twin abstraction in the same file, crypto.Hmac, always implemented
 * close()/dispose()/closed/[Symbol.dispose]; Hasher was a plain GC class,
 * so two siblings with the same streaming shape disagreed about who owns
 * native memory and when it goes back. Since X-10 both carry the family
 * contract, and this file pins it:
 *
 *   - the full surface exists (close, dispose, closed, [Symbol.dispose]);
 *   - close() is idempotent (double close is safe, closed stays true);
 *   - every method and getter throws TypeError after close;
 *   - [Symbol.dispose] and dispose() are close();
 *   - the GC finalizer is the backstop: a DROPPED, never-closed Hasher
 *     releases its native block and its DynResource-box ledger entry;
 *   - close() releases the ledger entry deterministically (the campaign's
 *     native-memory harness, as in test_native_memory.js);
 *   - hashing itself is untouched (FIPS 180-4 vector; subclassing works).
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_hasher_lifecycle.js
 * Prints "test_hasher_lifecycle: all N tests passed"; throws on failure. */

import { gc } from "std";
import { Hasher, SHA256Hex } from "dyna:hash";
import { memoryUsage } from "dyna:sys";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throwsTypeError(fn, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        assert(e instanceof TypeError, msg + " (TypeError, got "
               + (e.constructor && e.constructor.name) + ": " + e.message + ")");
        return;
    }
    throw new Error("assertion failed: " + msg + " did not throw");
}
function nativeSize() { return memoryUsage().nativeSize; }
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");

try {
    /* ---- 1. the surface exists ---------------------------------------- */
    const proto = Object.getPrototypeOf(new Hasher("sha256"));
    for (const m of ["close", "dispose", "closed"]) {
        assert(m in proto, "Hasher.prototype." + m + " exists");
    }
    const syms = Object.getOwnPropertySymbols(proto);
    assert(syms.some((s) => String(s).indexOf("dispose") >= 0),
           "Hasher.prototype has [Symbol.dispose]");

    /* ---- 2. hashing is untouched by the lifecycle machinery ----------- */
    const h0 = new Hasher("sha256");
    h0.update("abc");
    assert(hex(h0.digest()) ===
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "FIPS 180-4 vector via streaming digest");
    assert(SHA256Hex("abc") ===
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "one-shot and streaming agree");

    /* ---- 3. close(): idempotent, reports closed ----------------------- */
    const h1 = new Hasher("sha256");
    h1.update("x");
    assert(h1.closed === false, "fresh hasher reads closed === false");
    h1.close();
    assert(h1.closed === true, "closed === true after close()");
    h1.close();                                  /* double close is safe */
    h1.dispose();                                /* ...and so is dispose */
    assert(h1.closed === true, "closed stays true after double close");

    /* ---- 4. use after close throws TypeError, family-wide message ----- */
    throwsTypeError(() => h1.update("y"), "update after close throws");
    throwsTypeError(() => h1.digest(), "digest after close throws");
    throwsTypeError(() => h1.digestHex(), "digestHex after close throws");
    throwsTypeError(() => h1.reset(), "reset after close throws");
    throwsTypeError(() => h1.algorithm, "algorithm getter after close throws");
    throwsTypeError(() => h1.digestSize, "digestSize getter after close throws");

    /* ---- 5. dispose() and [Symbol.dispose] are close() ---------------- */
    const h2 = new Hasher("sha256");
    h2.dispose();
    assert(h2.closed === true, "dispose() closes");
    const h3 = new Hasher("sha256");
    h3[Symbol.dispose]();
    assert(h3.closed === true, "[Symbol.dispose] closes");

    /* ---- 6. a closing toString cannot resurrect or corrupt (the family
     *        coercion rule; the hook is ToString, not valueOf) ----------- */
    const h4 = new Hasher("sha256");
    const evil = { toString() { h4.close(); return "abc"; } };
    throwsTypeError(() => h4.update(evil),
                    "toString that closes `this` mid-coercion throws, not UAF");
    assert(h4.closed === true, "the close from toString took effect");

    /* ---- 7. subclassing keeps working (the new_target proto path) ----- */
    class MyHasher extends Hasher { }
    const sh = new MyHasher("sha512");
    assert(sh instanceof MyHasher && sh instanceof Hasher,
           "class X extends Hasher produces X instances");
    sh.update("abc");
    assert(sh.digestHex() ===
           "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
           + "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
           "subclass instance hashes correctly");
    sh.close();
    assert(sh.closed === true, "subclass instance closes");
    throwsTypeError(() => sh.update("x"), "subclass use after close throws");

    /* ---- 8. close() releases the ledger entry deterministically ------- */
    const R = 20000;
    const before = nativeSize();
    (function () {
        const rs = [];
        for (let i = 0; i < R; i++) rs.push(new Hasher("sha256"));
        for (let i = 0; i < R; i++) rs[i].close();
    })();
    gc(); gc();
    const afterClose = nativeSize();
    assert(afterClose <= before + (1 << 14),
           "close()+GC returns the resource-box ledger (before " + before
           + ", now " + afterClose + ")");

    /* ---- 9. the GC finalizer is the backstop: DROP without closing ---- */
    (function () {
        const junk = [];
        for (let i = 0; i < R; i++) junk.push(new Hasher("sha256"));
        junk[0] = null;                       /* nothing retained past scope */
        return junk.length;
    })();
    (function scrub() {                       /* scrub stale stack refs */
        const junk = [];
        for (let i = 0; i < 200; i++) junk.push(new Hasher("sha256"));
        return junk.length;
    })();
    gc(); gc();                               /* the engine's two passes */
    const afterGc = nativeSize();
    assert(afterGc <= before + (1 << 14),
           "dropped, never-closed hashers release natively via the finalizer"
           + " (before " + before + ", now " + afterGc + ")");
} finally {
    gc();
}

print("test_hasher_lifecycle: all " + n + " tests passed");
