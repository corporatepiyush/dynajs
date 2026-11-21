// covers: dyna:serialize.ValueHash
// Owner decision D8: ValueHash inherits the CBOREncode split. By default an integral Number in [-2^63, 2^64) hashes
// as an int, so the hash equals the pre-P15 one and ValueHash(2**60) === ValueHash(2n ** 60n); with { bigint: true }
// it hashes as a float64 and the two differ. A lone surrogate hashes as U+FFFD in both modes (pre-P15 hashed the
// WTF-8 bytes 19d59c649e17db1b; kept as U+FFFD for well-formed CBOR text).
import { suite } from "../lib/kit.js";
import { ValueHash } from "dyna:serialize";

const t = suite("test_serialize_value_hash_d8_bigint_option");
const B = { bigint: true };

t.test("default matches the pre-P15 hashes", (a) => {
    a.eq(ValueHash(2 ** 60), "6883980eca8f2885", "2^60");
    a.eq(ValueHash(2 ** 53), "a1acbdca5273bd61", "2^53");
    a.eq(ValueHash(-(2 ** 53) - 2), "a01a2309d30d7488", "-(2^53+2)");
    a.eq(ValueHash(2 ** 60), ValueHash(2n ** 60n), "Number and equal BigInt collide by default");
    a.eq(ValueHash(1e300), "c9c513342b2cf0e0", "a non-int is unchanged");
});

t.test("{ bigint: true } separates Number and BigInt", (a) => {
    a.eq(ValueHash(2 ** 60, B), "a843af9d274ad6ba", "2^60 as float64");
    a.ne(ValueHash(2 ** 60, B), ValueHash(2n ** 60n, B));
    a.eq(ValueHash(2n ** 60n, B), ValueHash(2n ** 60n), "a BigInt hashes the same in both modes");
    a.eq(ValueHash(5, B), ValueHash(5), "a safe int hashes the same in both modes");
});

t.test("lone surrogates", (a) => {
    a.eq(ValueHash("\ud800"), ValueHash("�"));
    a.eq(ValueHash("\ud800", B), ValueHash("�"));
});

t.test("bad options are a TypeError", (a) => {
    a.throws(() => ValueHash(1, { bigint: 1 }), TypeError);
    a.throws(() => ValueHash(1, 7), TypeError);
    a.throws(() => ValueHash(1, { seed: 1 }), TypeError);
});

await t.run();
