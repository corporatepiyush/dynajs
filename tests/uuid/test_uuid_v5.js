// covers: dyna:uuid.v5
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_v5");
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

__kitS.section("v5 result version==5", () => {
    const V5 = [
        [NAMESPACE_DNS, "www.example.com", "2ed6657d-e927-568b-95e1-2665a8aea6a2"],
        [NAMESPACE_DNS, "example.org",     "aad03681-8b63-5304-89e0-8ca8f49461b5"],
        [NAMESPACE_DNS, "hello",           "9342d47a-1bab-5709-9869-c840b2eac501"],
        [NAMESPACE_URL, "www.example.com", "b63cdfa4-3df9-568e-97ae-006c5b8fd652"],
        [NAMESPACE_URL, "example.org",     "54a35416-963c-5dd6-a1e2-5ab7bb5bafc7"],
        [NAMESPACE_URL, "hello",           "074171de-bc84-5ea4-b636-1135477620e1"],
    ];
    const V3 = [
        [NAMESPACE_DNS, "www.example.com", "5df41881-3aed-3515-88a7-2f4a814cf09e"],
        [NAMESPACE_DNS, "example.org",     "04738bdf-b25a-3829-a801-b21a1d25095b"],
        [NAMESPACE_DNS, "hello",           "0bacede4-4014-3f9d-b720-173f68a1c933"],
        [NAMESPACE_URL, "www.example.com", "a777199a-c522-31c4-8f4b-335feec7215b"],
        [NAMESPACE_URL, "example.org",     "39682ca1-9168-3da2-a1bb-f4dbcde99bf9"],
        [NAMESPACE_URL, "hello",           "cf3741de-a2dd-36f7-a791-8736e42c4c2f"],
    ];
    for (const [ns, name, want] of V5) {
        assertEq(v5(ns, name), want, "v5(" + ns + "," + name + ")");
        assertEq(version(want), 5, "v5 result version==5");
        assertEq(variant(want), "RFC4122", "v5 result variant");
    }
    for (const [ns, name, want] of V3) {
        assertEq(v3(ns, name), want, "v3(" + ns + "," + name + ")");
        assertEq(version(want), 3, "v3 result version==3");
        assertEq(variant(want), "RFC4122", "v3 result variant");
    }

    assertEq(v5(NAMESPACE_DNS, "hello"), v5(NAMESPACE_DNS, "hello"), "v5 deterministic");
    const nsBytes = bytes(NAMESPACE_DNS);
    assertEq(v5(nsBytes, "www.example.com"), "2ed6657d-e927-568b-95e1-2665a8aea6a2",
        "v5 accepts a 16-byte namespace view");
    const nameBytes = new Uint8Array([0x77, 0x77, 0x77, 0x2e, 0x65, 0x78, 0x61, 0x6d,
                                      0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d]);
    assertEq(v5(NAMESPACE_DNS, nameBytes), "2ed6657d-e927-568b-95e1-2665a8aea6a2",
        "v5 accepts a byte-view name");
    assertEq(v3(nsBytes, nameBytes), "5df41881-3aed-3515-88a7-2f4a814cf09e",
        "v3 accepts byte-view namespace + name");

    assert(CANON.test(v5(NAMESPACE_DNS, "")), "v5 empty name is canonical");
    assert(CANON.test(v3(NAMESPACE_DNS, "")), "v3 empty name is canonical");

    assertThrows(() => v5("not-a-uuid", "x"), "v5 bad namespace throws", SyntaxError);
    assertThrows(() => v5(NAMESPACE_DNS, {}), "v5 object name throws", TypeError);
    assertThrows(() => v5(new Uint8Array(15), "x"), "v5 15-byte namespace throws", TypeError);
});
__kitS.end();

print("test_uuid: all tests passed (" + n + " assertions)");
