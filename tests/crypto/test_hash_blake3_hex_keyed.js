// covers: dyna:hash.BLAKE3Hex
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_blake3_hex_keyed");
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

__kitS.section("b3 keyed, empty input (the official keyed vector)", () => {
eq(BLAKE3Hex(new Uint8Array(0), { key: k32 }),
   "73492b19995d71cdb1e9d74decc09809eb732f1b00bc95c27cb15f9dd4d6478f",
   "b3 keyed, empty input (the official keyed vector)");
});

__kitS.section("b3 keyed, \"abc\"", () => {
eq(BLAKE3Hex("abc", { key: k32 }),
   "6da54495d8152f2bcba87bd7282df70901cdb66b4448ed5f4c7bd2852b8b5532",
   "b3 keyed, \"abc\"");
});

__kitS.section("b3 keyed, 10240 bytes (multi-chunk tree, keyed parents)", () => {
eq(BLAKE3Hex(LONG, { key: k32 }),
   "9327ceb0285597a2e03ffb4414c79f276438748ebe867cfc82c9133eaabbc3d0",
   "b3 keyed, 10240 bytes (multi-chunk tree, keyed parents)");
});

__kitS.section("dyna:hash test context", () => {
eq(BLAKE3Hex(new Uint8Array(0), { context: "dyna:hash test context" }),
   "e0e9fff8df2b911323a17a053c1d3bd63ef7d3addc6592ccf5f5458d336c977a",
   "b3 derive-key, empty key material");
});

__kitS.section("dyna:hash test context (L43)", () => {
eq(BLAKE3Hex(new Uint8Array(0), { deriveKey: "dyna:hash test context" }),
   "e0e9fff8df2b911323a17a053c1d3bd63ef7d3addc6592ccf5f5458d336c977a",
   "{deriveKey} is the same option spelled differently");
});

__kitS.section("dyna:hash test context (L47)", () => {
eq(BLAKE3Hex(LONG, { context: "dyna:hash test context" }),
   "4fd782eb7945a0c5e6ad50a5c9b443b3b520c9dff8f4d42cf2b4226fadf0fc8d",
   "b3 derive-key, 10240 bytes of key material");
});

__kitS.section("b3 derive-key, one-char context", () => {
eq(BLAKE3Hex(new Uint8Array(0), { context: "x" }),
   "6e14701f13ec737635fe3b13f8f006cf4d5337fedf50dba35184d0c6be6741ba",
   "b3 derive-key, one-char context");
});

__kitS.section("b3 keyed differs from unkeyed", () => {
ok(BLAKE3Hex("abc") !== BLAKE3Hex("abc", { key: k32 }),
   "b3 keyed differs from unkeyed");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_hash_keyed: " + fail + " failures");
