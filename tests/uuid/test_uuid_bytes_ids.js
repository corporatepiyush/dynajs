// covers: dyna:uuid.bytes
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_bytes_ids");
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

__kitS.section("byte-form v4 has version 4", () => {
    const b = v4({ as: "bytes" });
    const s = fromBytes(b);
    assertEq(version(s), 4, "byte-form v4 has version 4");
    assert(bytes(s).join(",") === b.join(","), "bytes(fromBytes(b)) == b");
    assertEq(compare(s, fromBytes(bytes(s))), 0, "compare across the byte roundtrip is 0");

    const seq = [];
    for (let i = 0; i < 50; i++)
        seq.push(fromBytes(v7({ as: "bytes" })));
    let ordered = true;
    for (let i = 1; i < seq.length; i++)
        if (compare(seq[i - 1], seq[i]) > 0) ordered = false;
    assert(ordered, "50 byte-form v7 ids compare-ordered");
});
__kitS.end();

print("test_uuid_ids: all tests passed (" + n + " assertions)");
