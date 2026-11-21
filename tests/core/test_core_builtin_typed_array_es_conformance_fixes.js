// covers: c:src/builtins/builtin-typed-array.c
// Spec behaviours fixed while taking test262 from 139 to 4 failures and enabling joint iteration, await-dictionary,
// legacy RegExp statics, immutable ArrayBuffers, string module export names and private-add-to-nonextensible.
// test262 is the bulk oracle; this file pins the cases it does not cover and the ones whose regression would be
// silent (a crash, a lost write, a value that looks plausible).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_typed_array_es_conformance_fixes");
let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throwsT(f, ctor, m) {
    let e = null;
    try { f(); } catch (x) { e = x; }
    check(e instanceof ctor, m + " -- got " + (e ? e.constructor.name + ": " + e.message : "no exception"));
}

// EvalDeclarationInstantiation creates a var only when the binding does not exist: a second eval("var y") keeps y.

__kitS.section("transferToImmutable detaches the source", () => {
    const src = new Uint8Array([1, 2, 3, 4]).buffer;
    const im = src.transferToImmutable();
    check(src.detached && im.immutable && im.byteLength === 4, "transferToImmutable detaches the source");
    const u = new Uint8Array(im);
    throwsT(() => { "use strict"; u[0] = 9; }, TypeError, "a strict element write throws");
    check(Reflect.set(u, 0, 9) === false && u[0] === 1, "Reflect.set reports false and the byte is unchanged");
    check(Reflect.defineProperty(u, 0, { value: 1 }) === true && Reflect.defineProperty(u, 0, { value: 2 }) === false, "defineProperty only accepts the same value");
    const d = Object.getOwnPropertyDescriptor(u, 0);
    check(d.writable === false && d.configurable === false && d.enumerable === true, "elements describe themselves as frozen");
    throwsT(() => u.fill(0), TypeError, "fill is refused");
    throwsT(() => u.set([7]), TypeError, "set is refused");
    throwsT(() => new DataView(im).setUint8(0, 1), TypeError, "DataView setters are refused");
    check(new DataView(im).getUint8(3) === 4, "reads still work");
    throwsT(() => Atomics.store(new Int32Array(new ArrayBuffer(4).transferToImmutable()), 0, 1), TypeError, "Atomics.store is refused");
    throwsT(() => im.transfer(), TypeError, "an immutable buffer cannot be transferred");
    throwsT(() => im.resize(0), TypeError, "or resized");
    const s = im.sliceToImmutable(1, 3);
    check(s.immutable && String([...new Uint8Array(s)]) === "2,3" && !im.detached, "sliceToImmutable copies without detaching");
    check(Reflect.set(u.subarray(1), 0, 9) === false, "views made from an immutable view stay immutable");
    const plain = new Uint8Array(4);
    plain[0] = 5;
    check(plain[0] === 5 && new ArrayBuffer(1).immutable === false, "ordinary buffers are unaffected");
});
__kitS.end();

print("test_core_builtin_typed_array_es_conformance_fixes: " + n + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failures");
