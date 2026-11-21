// covers: dyna:hash.BLAKE3
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_blake3_keyed");
import { BLAKE2b, BLAKE2s, BLAKE3, BLAKE2bHex, BLAKE2sHex, BLAKE3Hex } from "dyna:hash";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + a + ", want " + b + ")");
const hex = (u) => {
    let s = "";
    for (const b of u) s += b.toString(16).padStart(2, "0");
    return s;
};
const throws = (fn, kind, rx, m) => {
    try { fn(); fail++; print("  FAIL: " + m + " (did not throw)"); }
    catch (e) {
        if (!(e instanceof kind)) { fail++; print("  FAIL: " + m + " wrong kind " + (e && e.constructor.name)); return; }
        if (rx && !rx.test(e.message)) { fail++; print("  FAIL: " + m + " message [" + e.message + "]"); return; }
        pass++;
    }
};

const k64 = Uint8Array.from({ length: 64 }, (_, i) => i);
const k32 = Uint8Array.from({ length: 32 }, (_, i) => i);
const k33 = Uint8Array.from({ length: 33 }, (_, i) => i);
const LONG = Uint8Array.from({ length: 256 * 40 }, (_, i) => i % 256);

__kitS.section("derive-key output is exactly 32 bytes", () => {
ok(BLAKE3(new Uint8Array(0), { context: "x" }).length === 32,
   "derive-key output is exactly 32 bytes");
});

__kitS.section("b3 31-byte key refuses (BLAKE3 has no variable-length key)", () => {
throws(() => BLAKE3("abc", { key: new Uint8Array(31) }), RangeError, /exactly 32/,
       "b3 31-byte key refuses (BLAKE3 has no variable-length key)");
});

__kitS.section("b3 33-byte key refuses", () => {
throws(() => BLAKE3("abc", { key: k33 }), RangeError, /exactly 32/,
       "b3 33-byte key refuses");
});

__kitS.section("b3 empty key refuses (0 is NOT unkeyed for BLAKE3)", () => {
throws(() => BLAKE3("abc", { key: new Uint8Array(0) }), RangeError, /exactly 32/,
       "b3 empty key refuses (0 is NOT unkeyed for BLAKE3)");
});

__kitS.section("empty context refuses (domain separation that separates nothing)", () => {
throws(() => BLAKE3("abc", { context: "" }), RangeError, /non-empty/,
       "empty context refuses (domain separation that separates nothing)");
});

__kitS.section("context + deriveKey together refuse", () => {
throws(() => BLAKE3("abc", { context: "a", deriveKey: "b" }), TypeError, /same option/,
       "context + deriveKey together refuse");
});

__kitS.section("key + context refuse (MAC or derive, not both)", () => {
throws(() => BLAKE3("abc", { key: k32, context: "a" }), TypeError, /different modes/,
       "key + context refuse (MAC or derive, not both)");
});

__kitS.section("derive-key + length refuses", () => {
throws(() => BLAKE3("abc", { context: "a", length: 32 }), TypeError, /exactly 32 bytes/,
       "derive-key + length refuses");
});

__kitS.section("CC-6: unknown b3 opts key refuses", () => {
throws(() => BLAKE3("abc", { key: k32, derive: true }), TypeError, /unknown option "derive"/,
       "CC-6: unknown b3 opts key refuses");
});

__kitS.section("a number key is refused as a type error, never stringified into key bytes", () => {
throws(() => BLAKE3("abc", { key: 12345 }), TypeError, /a key or secret must be a string, an ArrayBuffer or a byte view/,
       "a number key is refused as a type error, never stringified into key bytes");
});

__kitS.section("BLAKE3 positional length form still works", () => {
eq(hex(BLAKE3("abc")), "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85",
   "BLAKE3 positional length form still works");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_hash_keyed: " + fail + " failures");
