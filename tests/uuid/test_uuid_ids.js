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

{
    assertEq(compare(NIL, NIL), 0, "compare(NIL, NIL) == 0");
    assertEq(compare(NIL, MAX), -1, "NIL sorts before MAX");
    assertEq(compare(MAX, NIL), 1, "MAX sorts after NIL");
    assertEq(compare("00000000-0000-0000-0000-000000000001",
                     "00000000-0000-0000-0000-000000000002"), -1, "last-byte ordering");

    const u = v7();
    assertEq(compare(u, parse(u)), 0, "canonical vs parsed");
    assertEq(compare(u, "urn:uuid:" + u.toUpperCase()), 0, "urn + uppercase form");
    assertEq(compare(u, "{" + u + "}"), 0, "braced form");
    assertEq(compare(u, u.replace(/-/g, "")), 0, "raw hex form");

    let prev = null, ordered = true;
    for (let i = 0; i < 200; i++) {
        const id = v7();
        if (prev !== null && compare(prev, id) > 0) ordered = false;
        prev = id;
    }
    assert(ordered, "200 fresh v7 ids are non-decreasing under compare");

    const a = "0e000000-0000-7000-8000-000000000000";
    const b = "10000000-0000-7000-8000-000000000000";
    assertEq(compare(a, b), a < b ? -1 : 1, "byte order agrees with string order");

    assertThrows(() => compare("nope", NIL), "malformed a", TypeError);
    assertThrows(() => compare(NIL, "123"), "malformed b", TypeError);
    assertThrows(() => compare(42, NIL), "non-string a", TypeError);
    assertThrows(() => compare(NIL, null), "non-string b", TypeError);
    assertThrows(() => compare(NIL), "missing b", TypeError);
    assertThrows(() => compare("1234", NIL), "too-short raw hex", TypeError);
}

{
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
}

{
    const T = 4000000000000;
    let prev = null, strictlyAscending = true;
    for (let i = 0; i < 1000; i++) {
        const u = ULID(T, { monotonic: true });
        if (prev !== null && !(u > prev)) strictlyAscending = false;
        prev = u;
    }
    assert(strictlyAscending, "1000 same-ms monotonic ULIDs strictly ascending");
    assertEq(ULIDTime(prev), T, "timestamp roundtrips through ULIDTime");

    assertEq(prev.length, 26, "monotonic ULID is 26 chars");
    assert(!/[ILOU]/.test(prev), "no I L O U symbols");

    let p = null, ok = true;
    for (let i = 0; i < 500; i++) {
        const u = ULID(undefined, { monotonic: true });
        if (p !== null && !(u > p)) ok = false;
        p = u;
    }
    assert(ok, "500 clock-driven monotonic ULIDs strictly ascending");

    {
        const hi = ULID(T + 5000, { monotonic: true });
        const lo = ULID(T, { monotonic: true });
        assert(lo > hi, "explicit ts below the floor clamps UP (id still ascends)");
        assertEq(ULIDTime(lo), T + 5000, "clamped id carries the held millisecond");
    }

    {
        let allAsc = true, p2 = null;
        for (let i = 0; i < 50; i++) {
            const u = ULID(T + 20000);
            if (p2 !== null && !(u > p2)) allAsc = false;
            p2 = u;
        }
        assert(!allAsc, "default ULIDs are NOT monotonic within a ms");
        assertEq(ULIDTime(p2), T + 20000, "default ULIDTime roundtrip");
    }

    {
        const a = ULID(0, { monotonic: true });
        const b = ULID(2 ** 48 - 1, { monotonic: true });
        const c = ULID(0, { monotonic: true });
        assert(a < b && b < c, "ULID 0 -> 2^48-1 -> 0 still ascends (clamps up)");
    }
    {
        const ta = new Uint8Array(16);
        if (typeof ta.buffer.transfer === "function") {
            ta.buffer.transfer();
            assertThrows(() => v8({ fill: ta }), "detached fill", TypeError);
        }
    }
    {
        assertThrows(() => ULID(5, { get monotonic() { throw new Error("boom"); } }),
                     "ULID getter throws propagate");
        let sawTypeError = false;
        try { v8({ get oops() { throw new Error("boom"); } }); }
        catch (e) { sawTypeError = e instanceof TypeError; }
        assert(sawTypeError, "unknown-key check precedes the getter");
    }

    assertThrows(() => ULID(T, { mono: 1 }), "ULID unknown option key", TypeError);
    assertThrows(() => ULID(T, "monotonic"), "ULID non-object opts", TypeError);
    assertThrows(() => ULID(2 ** 48, { monotonic: true }), "atMillis above 48 bits still refused", RangeError);
    assertEq(ULID(0, { monotonic: true }).length, 26, "ULID(0, monotonic) valid");
}

{
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
}

print("test_uuid_ids: all tests passed (" + n + " assertions)");
