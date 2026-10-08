// A bytes-level cookbook: length-prefixed message framing, content hashing
// pipelines, and UTF-8 hygiene — the everyday grammar of dyna:bytes.
// Runs with no flags: everything comes from dyna:bytes and engine globals.
import { Bytes, Text, bytesOf, fromUtf8, toUtf8, isValidUtf8 } from "dyna:bytes";
import { HexEncode, Base64Encode, Base64Decode } from "dyna:encoding";

let failures = 0;
function check(cond, what) {
    if (cond) { print("  ok  " + what); return; }
    failures++;
    print("  FAIL " + what);
}

// ------------------------------------------------------------------
// 1. Frame protocol: [u32 BE length][payload]. The fixed-width writers
//    return the offset after the value, so building a header is a chain.
// ------------------------------------------------------------------
function frame(payload) {
    const head = Bytes.alloc(4);   // zero-filled buffer; the constructor takes data, not a length
    head.writeUint32BE(0, payload.length);
    return Bytes.concat([head.array, payload]);   // a Bytes handle
}

function unframe(buf) {
    const len = buf.readUint32BE(0);
    if (4 + len > buf.length) throw new RangeError("truncated frame: want " + len + ", have " + (buf.length - 4));
    return buf.slice(4, 4 + len);
}

{
    const msg = fromUtf8("dynajs speaks bytes");
    const wire = frame(msg);
    check(wire.length === 4 + msg.length, "frame is 4-byte header plus payload");
    check(wire.readUint8(0) === 0 && wire.readUint8(3) === 19, "length 19 encoded big-endian across the header");
    check(toUtf8(unframe(wire)) === "dynajs speaks bytes", "unframe recovers the payload");

    let threw = null;
    try { unframe(wire.slice(0, 10)); } catch (e) { threw = e; }
    check(threw instanceof RangeError, "truncated frame throws RangeError");
}

// ------------------------------------------------------------------
// 2. Slices are views, not copies: one owner, many zero-cost windows.
// ------------------------------------------------------------------
{
    const owner = new Bytes("the quick brown fox");
    const word = owner.slice(4, 9);            // "quick"
    check(word.toString() === "quick", "slice selects a window");
    word.array[0] = 0x51;                       // 'q' -> 'Q' through the view
    check(owner.toString() === "the Quick brown fox", "writing through a view mutates the owner");
    check(owner.indexOf(fromUtf8("Quick")) === 4, "indexOf finds a view needle at its first byte");
    check(owner.slice(100).length === 0 && owner.slice(-100).toString().length === 19,
          "slice clamps both ends instead of throwing");
}

// ------------------------------------------------------------------
// 3. Text hygiene: validate, count, and repair before you trust bytes.
// ------------------------------------------------------------------
{
    const good = new Text("日本語 ✓");
    const bad = new Uint8Array([0x6f, 0x6b, 0xC3, 0x28]);   // "ok" + a truncated 2-byte sequence
    check(good.isValidUtf8 === true, "well-formed text passes the cached getter");
    check(isValidUtf8(bad) === false, "0xC3 0x28 is malformed and the checker says so");
    check(good.countUtf8() === 5 && good.countUtf16() === 5,
          "code-point counts agree across encodings (no lone surrogates)");
    check(toUtf8(bad).includes("\uFFFD"), "lossy decode substitutes U+FFFD instead of throwing");
    check(good.toUtf8().length === 13, "3x CJK (3B each) + space (1B) + check mark U+2713 (3B) = 13 bytes");
}

// ------------------------------------------------------------------
// 4. Wire formats: hex for logs, base64 for transport — round-trip proof.
// ------------------------------------------------------------------
{
    const raw = fromUtf8("round trip!");
    const b64 = Base64Encode(raw);
    const restored = Base64Decode(b64);
    check(restored.every((v, i) => v === raw[i]), "base64 round trip restores every byte");
    check(HexEncode(raw) === "726f756e64207472697021", "hex of 'round trip!' is the documented ASCII expansion");
    const padded = new Text("any carnal pleasure").toUtf8();  // 19 bytes: 19 mod 4 = 3 -> one '=' of padding
    check(padded.length === 19, "the sample payload is exactly 19 bytes");
    check(Base64Decode(Base64Encode(padded)).every((v, i) => v === padded[i]),
          "19-byte input survives base64 padding rules");
}

// ------------------------------------------------------------------
// 5. byte-level diff: same bytes, different windows, one compare.
// ------------------------------------------------------------------
{
    const v1 = fromUtf8("config: alpha=1");
    const v2 = fromUtf8("config: alpha=2");
    const cmp = new Bytes(v1).compare(v2);
    check(cmp === -1, "49 < 50, lexicographic compare says -1");
    const firstDiff = (function () {
        for (let i = 0; i < v1.length; i++) if (v1[i] !== v2[i]) return i;
        return -1;
    })();
    check(firstDiff === 14, "the versions diverge exactly at the digit");
    check(bytesOf(v1.buffer).length === v1.length, "bytesOf aliases a plain ArrayBuffer one-to-one");
}

print(failures === 0
    ? "dynajs_bytes_cookbook: all checks passed"
    : "dynajs_bytes_cookbook: " + failures + " check(s) FAILED");
if (failures) throw new Error(failures + " check(s) failed");
