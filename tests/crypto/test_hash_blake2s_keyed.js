// covers: dyna:hash.BLAKE2s
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_blake2s_keyed");
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

__kitS.section("b2s keyed, empty input", () => {
eq(hex(BLAKE2s(new Uint8Array(0), { key: k32 })),
   "48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49",
   "b2s keyed, empty input");
});

__kitS.section("b2s keyed, \"abc\"", () => {
eq(hex(BLAKE2s("abc", { key: k32 })),
   "a281f725754969a702f6fe36fc591b7def866e4b70173ece402fc01c064d6b65",
   "b2s keyed, \"abc\"");
});

__kitS.section("b2s keyed, 10240 bytes", () => {
eq(hex(BLAKE2s(LONG, { key: k32 })),
   "578322375b4c82665bd2b2f80cce9af8d61db023fea2a4273775a56c28fcb9de",
   "b2s keyed, 10240 bytes");
});

__kitS.section("b2s 33-byte key refuses", () => {
throws(() => BLAKE2s("abc", { key: k33 }), RangeError, /0 to 32/,
       "b2s 33-byte key refuses");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_hash_keyed: " + fail + " failures");
