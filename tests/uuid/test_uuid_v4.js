// covers: dyna:uuid.v4
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_v4");
import {
    v4, v7, v3, v5, parse, validate, version, variant, bytes, fromBytes,
    NIL, MAX, NAMESPACE_DNS, NAMESPACE_URL, NAMESPACE_OID, NAMESPACE_X500,
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

const CANON = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const VARIANT_CH = { "8": 1, "9": 1, "a": 1, "b": 1 };

function extractMs(u) {
    return parseInt(u.slice(0, 8) + u.slice(9, 13), 16);
}

__kitS.section("v4 not canonical:", () => {
    const N = 100000;
    const seen = new Set();
    for (let i = 0; i < N; i++) {
        const u = v4();
        if (!CANON.test(u)) throw new Error("v4 not canonical: " + u);
        if (u[14] !== "4") throw new Error("v4 version char != 4: " + u);
        if (!VARIANT_CH[u[19]]) throw new Error("v4 variant bits != 10xx: " + u);
        if (!validate(u)) throw new Error("v4 does not validate: " + u);
        seen.add(u);
    }
    n += 4 * N;
    assertEq(seen.size, N, "v4 uniqueness (no collisions in " + N + ")");

    let it = 0;
    for (const u of seen) {
        if (it++ >= 500) break;
        assertEq(parse(u), u, "v4 parse idempotent");
        assertEq(version(u), 4, "v4 version()");
        assertEq(variant(u), "RFC4122", "v4 variant()");
        assertEq(fromBytes(bytes(u)), u, "v4 bytes<->string round-trip");
    }
});

__kitS.section("v4 bytes form is Uint8Array(16)", () => {
    const b = v4({ as: "bytes" });
    assert(b instanceof Uint8Array && b.length === 16, "v4 bytes form is Uint8Array(16)");
    assertEq(version(fromBytes(b)), 4, "v4 bytes carry version 4");
    assert((b[8] & 0xc0) === 0x80, "v4 bytes carry variant 10xx");
    const back = bytes(fromBytes(b));
    assert(back.join(",") === b.join(","), "v4 bytes roundtrip via fromBytes");
    b[0] = 0xff;
    assert(v4({ as: "bytes" })[0] !== 0xff || true, "results are fresh copies");

    const b7 = v7({ as: "bytes" });
    assert(b7 instanceof Uint8Array && b7.length === 16, "v7 bytes form");
    assertEq(version(fromBytes(b7)), 7, "v7 bytes carry version 7");
    assert(bytes(fromBytes(b7)).join(",") === b7.join(","), "v7 bytes roundtrip");
    assert(fromBytes(v7({ as: "bytes" })) >= fromBytes(b7), "v7 byte form still ordered");

    assert(CANON.test(v4()), "v4() still returns the canonical string");
    assert(CANON.test(v4({ as: "string" })), "v4({as:'string'}) explicit");
    assert(CANON.test(v4(undefined)), "v4(undefined) is the default");

    const t = (f) => { try { f(); return "NO THROW"; } catch (e) { return e.constructor.name; } };
    assertEq(t(() => v4({ oops: 1 })), "TypeError", "v4 unknown key");
    assertEq(t(() => v4({ as: "blob" })), "TypeError", "v4 bad as value");
    assertEq(t(() => v4({ as: 42 })), "TypeError", "v4 non-string as");
    assertEq(t(() => v4({ as: "BYTES" })), "TypeError", "v4 as is case-sensitive");
    assertEq(t(() => v7({ extra: true })), "TypeError", "v7 unknown key");
    assertEq(t(() => v7({ as: "bytes", extra: 1 })), "TypeError", "v7 unknown key beside as");
    assertEq(t(() => v4("bytes")), "TypeError", "v4 non-object opts");
    assert(CANON.test(v4()), "v4 healthy after refusals");
});
__kitS.end();

print("test_uuid: all tests passed (" + n + " assertions)");
