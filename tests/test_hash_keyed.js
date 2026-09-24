// flags: --std
/* test_hash_keyed.js --: keyed BLAKE2b/BLAKE2s (RFC 7693) and keyed +
 * derive-key BLAKE3.
 *
 * Vectors are the reference implementations': python hashlib.blake2b/2s
 * (which IS RFC 7693 keyed mode; the empty-input b2b vector is the one
 * printed in the RFC's appendix) and the official blake3 package for the
 * keyed and derive-key modes. The derive-key sequence proved the subtle
 * one: the context is hashed under DERIVE_KEY_CONTEXT, and that 32-byte
 * ROOT hash keys a DERIVE_KEY_MATERIAL pass -- WITHOUT the KEYED_HASH flag
 * (the reference's init_keyed_internal passes the material flag alone).
 * Wrong flag positions or a spurious KEYED bit give clean-looking, wrong
 * digests -- that failure mode is exactly why the vectors are pinned here.
 *
 * Contract:
 *   BLAKE2b(data, {key})  -- key 0..64 bytes; 0 (empty) IS legal = unkeyed
 *                            (RFC 7693: it is the parameter block's
 *                            key_length field, not a flag)
 *   BLAKE2s(data, {key})  -- key 0..32 bytes, same RFC semantics
 *   BLAKE3(data, {key})   -- key exactly 32 bytes (BLAKE3 keyed mode)
 *   BLAKE3(data, {context}) or {deriveKey} -- derive-key mode; the output
 *                            is exactly 32 bytes and `length` is refused
 * The second argument is number (length, unchanged) or object (opts), never
 * both spellings at once; unknown opts keys are rejections.
 */
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

/* ---- 1. RFC 7693 keyed BLAKE2b-512 (python hashlib oracle) ---- */
eq(hex(BLAKE2b(new Uint8Array(0), { key: k64 })),
   "10ebb67700b1868efb4417987acf4690ae9d972fb7a590c2f02871799aaa4786b5e996e8f0f4eb981fc214b005f42d2ff4233499391653df7aefcbc13fc51568",
   "b2b keyed, empty input (the RFC 7693 appendix vector)");
eq(hex(BLAKE2b("abc", { key: k64 })),
   "06bbc3dedf13a31139498655251b7588ccd3bb5aaa071b2d44d8e0a04095579ed590fbfdcf941f4370ce5ce623624e7a76d33e7a8109dcda9b57d72f8f8efa51",
   "b2b keyed, \"abc\"");
eq(hex(BLAKE2b(LONG, { key: k64 })),
   "265869d904d8a560e5d54d3dcc82fb72b2b2a362780af9cdbe9e26e9dbed39668924cf9a3b355e208e2c66dc58569698a1d7445969e68d9f6ee94a7d3c58f77e",
   "b2b keyed, 10240 bytes (multi-block, past the last-block rule)");
eq(BLAKE2bHex("abc", { key: k64 }),
   "06bbc3dedf13a31139498655251b7588ccd3bb5aaa071b2d44d8e0a04095579ed590fbfdcf941f4370ce5ce623624e7a76d33e7a8109dcda9b57d72f8f8efa51",
   "the Hex form agrees");
eq(hex(BLAKE2b("abc", { key: k64 })), BLAKE2bHex("abc", { key: k64 }),
   "the byte form and the Hex form agree (one function)");
eq(hex(BLAKE2b("abc", { key: k64, length: 32 })),
   "dff38c978666dff5631db35ca15535520d134f5c8060ea569c6a178ad393719f",
   "b2b keyed with length 32 (truncated keyed digest)");

/* ---- 2. RFC 7693 keyed BLAKE2s-256 ---- */
eq(hex(BLAKE2s(new Uint8Array(0), { key: k32 })),
   "48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49",
   "b2s keyed, empty input");
eq(hex(BLAKE2s("abc", { key: k32 })),
   "a281f725754969a702f6fe36fc591b7def866e4b70173ece402fc01c064d6b65",
   "b2s keyed, \"abc\"");
eq(hex(BLAKE2s(LONG, { key: k32 })),
   "578322375b4c82665bd2b2f80cce9af8d61db023fea2a4273775a56c28fcb9de",
   "b2s keyed, 10240 bytes");

/* ---- 3. keyed BLAKE3 (official oracle) ---- */
eq(BLAKE3Hex(new Uint8Array(0), { key: k32 }),
   "73492b19995d71cdb1e9d74decc09809eb732f1b00bc95c27cb15f9dd4d6478f",
   "b3 keyed, empty input (the official keyed vector)");
eq(BLAKE3Hex("abc", { key: k32 }),
   "6da54495d8152f2bcba87bd7282df70901cdb66b4448ed5f4c7bd2852b8b5532",
   "b3 keyed, \"abc\"");
eq(BLAKE3Hex(LONG, { key: k32 }),
   "9327ceb0285597a2e03ffb4414c79f276438748ebe867cfc82c9133eaabbc3d0",
   "b3 keyed, 10240 bytes (multi-chunk tree, keyed parents)");

/* ---- 4. BLAKE3 derive-key ---- */
eq(BLAKE3Hex(new Uint8Array(0), { context: "dyna:hash test context" }),
   "e0e9fff8df2b911323a17a053c1d3bd63ef7d3addc6592ccf5f5458d336c977a",
   "b3 derive-key, empty key material");
eq(BLAKE3Hex(new Uint8Array(0), { deriveKey: "dyna:hash test context" }),
   "e0e9fff8df2b911323a17a053c1d3bd63ef7d3addc6592ccf5f5458d336c977a",
   "{deriveKey} is the same option spelled differently");
eq(BLAKE3Hex(LONG, { context: "dyna:hash test context" }),
   "4fd782eb7945a0c5e6ad50a5c9b443b3b520c9dff8f4d42cf2b4226fadf0fc8d",
   "b3 derive-key, 10240 bytes of key material");
eq(BLAKE3Hex(new Uint8Array(0), { context: "x" }),
   "6e14701f13ec737635fe3b13f8f006cf4d5337fedf50dba35184d0c6be6741ba",
   "b3 derive-key, one-char context");
ok(BLAKE3(new Uint8Array(0), { context: "x" }).length === 32,
   "derive-key output is exactly 32 bytes");

/* ---- 5. keyed output is unkeyed-incomparable (a MAC must differ) ---- */
ok(BLAKE2bHex("abc") !== BLAKE2bHex("abc", { key: k64 }),
   "b2b keyed differs from unkeyed");
ok(BLAKE3Hex("abc") !== BLAKE3Hex("abc", { key: k32 }),
   "b3 keyed differs from unkeyed");
eq(BLAKE2bHex("abc"),
   "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923",
   "the unkeyed digest did not move (RFC 7693's other appendix vector)");

/* ---- 6. key length 0 IS legal and equals unkeyed (RFC 7693 semantics) ---- */
eq(BLAKE2bHex("abc", { key: new Uint8Array(0) }), BLAKE2bHex("abc"),
   "b2b empty key == unkeyed");
eq(BLAKE2sHex("abc", { key: new Uint8Array(0) }), BLAKE2sHex("abc"),
   "b2s empty key == unkeyed");

/* ---- 7. the validation matrix (loop 2: wrong key sizes) ---- */
eq(BLAKE2bHex("abc", { key: k33 }),
   "07e54f8dbb1e74920fc565bef19b89e7c8610436a06db3614d3ae58d42139be05d9b99b4e066226a47b5d03531e45a82a5bfadabacb4defdbd9701996adbfebd",
   "b2b 33-byte key is LEGAL (RFC: any length 0..64)");
eq(BLAKE2bHex(new Uint8Array(0), { key: k33, length: 33 }),
   "d5500be30a4db7f536eda6c14f947b02b7be04621d92c967014ebbd1c826be2c54",
   "b2b 33-byte key with a 33-byte digest (odd out_len) too");
throws(() => BLAKE2b("abc", { key: new Uint8Array(65) }), RangeError, /0 to 64/,
       "b2b 65-byte key refuses");
throws(() => BLAKE2s("abc", { key: k33 }), RangeError, /0 to 32/,
       "b2s 33-byte key refuses");
throws(() => BLAKE3("abc", { key: new Uint8Array(31) }), RangeError, /exactly 32/,
       "b3 31-byte key refuses (BLAKE3 has no variable-length key)");
throws(() => BLAKE3("abc", { key: k33 }), RangeError, /exactly 32/,
       "b3 33-byte key refuses");
throws(() => BLAKE3("abc", { key: new Uint8Array(0) }), RangeError, /exactly 32/,
       "b3 empty key refuses (0 is NOT unkeyed for BLAKE3)");
throws(() => BLAKE3("abc", { context: "" }), RangeError, /non-empty/,
       "empty context refuses (domain separation that separates nothing)");
throws(() => BLAKE3("abc", { context: "a", deriveKey: "b" }), TypeError, /same option/,
       "context + deriveKey together refuse");
throws(() => BLAKE3("abc", { key: k32, context: "a" }), TypeError, /different modes/,
       "key + context refuse (MAC or derive, not both)");
throws(() => BLAKE3("abc", { context: "a", length: 32 }), TypeError, /exactly 32 bytes/,
       "derive-key + length refuses");
throws(() => BLAKE2b("abc", { key: k64, length: 0 }), RangeError, /length is 1/,
       "length 0 still refuses");
throws(() => BLAKE2b("abc", { kry: k64 }), TypeError, /unknown option "kry"/,
       "CC-6: a misspelled key name refuses by name");
throws(() => BLAKE3("abc", { key: k32, derive: true }), TypeError, /unknown option "derive"/,
       "CC-6: unknown b3 opts key refuses");
throws(() => BLAKE2b("abc", 32, { key: k64 }), TypeError, /at most two arguments/,
       "no third argument: the number and object forms do not mix");
throws(() => BLAKE3("abc", { key: 12345 }), RangeError, /exactly 32/,
       "a number key coerces to its decimal bytes and fails the size check");

/* ---- 8. the old positional forms are untouched ---- */
eq(BLAKE2bHex("abc"), BLAKE2bHex("abc", 64), "BLAKE2b(data, 64) == default");
eq(BLAKE2bHex("abc"), BLAKE2bHex("abc", 64), "explicit default length unchanged");
eq(hex(BLAKE3("abc")), "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85",
   "BLAKE3 positional length form still works");

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_hash_keyed: " + fail + " failures");
