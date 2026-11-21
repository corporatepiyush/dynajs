// covers: dyna:hash.BLAKE2bHex
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_blake2b_hex_keyed");
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

__kitS.section("the Hex form agrees", () => {
eq(BLAKE2bHex("abc", { key: k64 }),
   "06bbc3dedf13a31139498655251b7588ccd3bb5aaa071b2d44d8e0a04095579ed590fbfdcf941f4370ce5ce623624e7a76d33e7a8109dcda9b57d72f8f8efa51",
   "the Hex form agrees");
});

__kitS.section("b2b keyed differs from unkeyed", () => {
ok(BLAKE2bHex("abc") !== BLAKE2bHex("abc", { key: k64 }),
   "b2b keyed differs from unkeyed");
});

__kitS.section("the unkeyed digest did not move (RFC 7693's other appendix vector)", () => {
eq(BLAKE2bHex("abc"),
   "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923",
   "the unkeyed digest did not move (RFC 7693's other appendix vector)");
});

__kitS.section("b2b empty key == unkeyed", () => {
eq(BLAKE2bHex("abc", { key: new Uint8Array(0) }), BLAKE2bHex("abc"),
   "b2b empty key == unkeyed");
});

__kitS.section("b2b 33-byte key is LEGAL (RFC: any length 0..64)", () => {
eq(BLAKE2bHex("abc", { key: k33 }),
   "07e54f8dbb1e74920fc565bef19b89e7c8610436a06db3614d3ae58d42139be05d9b99b4e066226a47b5d03531e45a82a5bfadabacb4defdbd9701996adbfebd",
   "b2b 33-byte key is LEGAL (RFC: any length 0..64)");
});

__kitS.section("b2b 33-byte key with a 33-byte digest (odd out_len) too", () => {
eq(BLAKE2bHex(new Uint8Array(0), { key: k33, length: 33 }),
   "d5500be30a4db7f536eda6c14f947b02b7be04621d92c967014ebbd1c826be2c54",
   "b2b 33-byte key with a 33-byte digest (odd out_len) too");
});

__kitS.section("BLAKE2b(data, 64) == default", () => {
eq(BLAKE2bHex("abc"), BLAKE2bHex("abc", 64), "BLAKE2b(data, 64) == default");
});

__kitS.section("explicit default length unchanged", () => {
eq(BLAKE2bHex("abc"), BLAKE2bHex("abc", 64), "explicit default length unchanged");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_hash_keyed: " + fail + " failures");
