// covers: dyna:uuid.v8
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_v8_ids");
import {
    v4, v7, v8, compare, parse, version, variant, bytes, fromBytes,
    ULID, ULIDTime, NIL, MAX,
} from "dyna:uuid";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + " (got " + got + ", want " + want + ")");
}
function assertThrows(fn, msg, ErrType) {
    n++;
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
    if (ErrType && !(err instanceof ErrType))
        throw new Error("assertion failed (wrong error type, got " + err + "): " + msg);
}

__kitS.section("v8 reports version 8", () => {
    const fill = new Uint8Array(16);
    for (let i = 0; i < 16; i++) fill[i] = 0xab;
    const u = v8({ fill });

    assertEq(version(u), 8, "v8 reports version 8");
    assertEq(variant(u), "RFC4122", "v8 reports the RFC 4122 variant");
    const got = bytes(u);
    assertEq(got[6], 0x8b, "byte 6: version nibble set, caller low nibble kept");
    assertEq(got[8], 0xab, "byte 8: top bits were already 10 in 0xab (kept)");
    assertEq(got[7], 0xab, "byte 7 untouched");
    for (let i = 0; i < 16; i++)
        if (i !== 6)
            assertEq(fill[i], 0xab, "input byte " + i + " never mutated");
    assertEq(fill[6], 0xab, "input byte 6 never mutated");

    const f2 = new Uint8Array(16);
    f2[6] = 0x3c; f2[8] = 0x3f;
    const u2 = v8({ fill: f2 });
    const b2 = bytes(u2);
    assertEq(b2[6], 0x8c, "byte 6 = 1000_1100: version set, low nibble 0xc kept");
    assertEq(b2[8], 0xbf, "byte 8 = 10_111111: variant set, low six 0x3f kept");
    assertEq(variant(u2), "RFC4122", "variant bits make it RFC4122");

    const i8 = new Int8Array(16); i8.fill(-1);
    const u3 = v8({ fill: i8 });
    assertEq(version(u3), 8, "Int8Array fill accepted");
    const ab = new ArrayBuffer(16);
    const u4 = v8({ fill: ab });
    assertEq(version(u4), 8, "bare ArrayBuffer fill accepted");

    assertThrows(() => v8({ fill: new Uint8Array(15) }), "15-byte fill", RangeError);
    assertThrows(() => v8({ fill: new Uint8Array(17) }), "17-byte fill", RangeError);
    assertThrows(() => v8({ fill: "strings are not views" }), "string fill", TypeError);
    assertThrows(() => v8({ fill: new Uint32Array(4) }), "4-byte-element view", TypeError);
    assertThrows(() => v8({}), "missing fill", TypeError);
    assertThrows(() => v8(), "missing opts", TypeError);
    assertThrows(() => v8({ fill: new Uint8Array(16), extra: 1 }), "unknown key", TypeError);
    assertThrows(() => v8("nope"), "non-object opts", TypeError);
});
__kitS.end();

print("test_uuid_ids: all tests passed (" + n + " assertions)");
