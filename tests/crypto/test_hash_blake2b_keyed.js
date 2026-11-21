// covers: dyna:hash.BLAKE2b
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_blake2b_keyed");
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

__kitS.section("b2b keyed, empty input (the RFC 7693 appendix vector)", () => {
eq(hex(BLAKE2b(new Uint8Array(0), { key: k64 })),
   "10ebb67700b1868efb4417987acf4690ae9d972fb7a590c2f02871799aaa4786b5e996e8f0f4eb981fc214b005f42d2ff4233499391653df7aefcbc13fc51568",
   "b2b keyed, empty input (the RFC 7693 appendix vector)");
});

__kitS.section("b2b keyed, \"abc\"", () => {
eq(hex(BLAKE2b("abc", { key: k64 })),
   "06bbc3dedf13a31139498655251b7588ccd3bb5aaa071b2d44d8e0a04095579ed590fbfdcf941f4370ce5ce623624e7a76d33e7a8109dcda9b57d72f8f8efa51",
   "b2b keyed, \"abc\"");
});

__kitS.section("b2b keyed, 10240 bytes (multi-block, past the last-block rule)", () => {
eq(hex(BLAKE2b(LONG, { key: k64 })),
   "265869d904d8a560e5d54d3dcc82fb72b2b2a362780af9cdbe9e26e9dbed39668924cf9a3b355e208e2c66dc58569698a1d7445969e68d9f6ee94a7d3c58f77e",
   "b2b keyed, 10240 bytes (multi-block, past the last-block rule)");
});

__kitS.section("the byte form and the Hex form agree (one function)", () => {
eq(hex(BLAKE2b("abc", { key: k64 })), BLAKE2bHex("abc", { key: k64 }),
   "the byte form and the Hex form agree (one function)");
});

__kitS.section("b2b keyed with length 32 (truncated keyed digest)", () => {
eq(hex(BLAKE2b("abc", { key: k64, length: 32 })),
   "dff38c978666dff5631db35ca15535520d134f5c8060ea569c6a178ad393719f",
   "b2b keyed with length 32 (truncated keyed digest)");
});

__kitS.section("b2b 65-byte key refuses", () => {
throws(() => BLAKE2b("abc", { key: new Uint8Array(65) }), RangeError, /0 to 64/,
       "b2b 65-byte key refuses");
});

__kitS.section("length 0 still refuses", () => {
throws(() => BLAKE2b("abc", { key: k64, length: 0 }), RangeError, /length is 1/,
       "length 0 still refuses");
});

__kitS.section("CC-6: a misspelled key name refuses by name", () => {
throws(() => BLAKE2b("abc", { kry: k64 }), TypeError, /unknown option "kry"/,
       "CC-6: a misspelled key name refuses by name");
});
__kitS.end();

throws(() => BLAKE2b("abc", 32, { key: k64 }), TypeError, /at most two arguments/,
       "no third argument: the number and object forms do not mix");

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_hash_keyed: " + fail + " failures");
