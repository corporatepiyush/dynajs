// covers: dyna:serialize.BSONEncode
// Array element keys are the decimal index strings "0", "1", ... (bsonspec.org: "the keys must be the
// ascending numeric values starting with 0"). BSONDecode and every docdb path read arrays by position, so
// a wrong key is invisible to a round trip; this walks the encoded bytes and reads each key back. The keys
// are written by dyn_bson_u32_key (one digit without division, else reversed digits) instead of
// snprintf("%u"), which cost about 9% of an insertMany of documents with arrays (P5); dense and sparse
// (non-fast) arrays take different encoder loops, so both are walked at the 1/2/3/4-digit boundaries.
import { suite } from "../lib/kit.js";
import { BSONEncode } from "dyna:serialize";

const t = suite("serialize.BSONEncode array keys");
const td = new TextDecoder();

function arrayKeys(bytes) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let p = 4;
    const type = bytes[p++];
    while (bytes[p] !== 0) p++;
    p++;
    if (type !== 4) throw new Error("field a is not an array: type " + type);
    const end = p + dv.getInt32(p, true) - 1;
    p += 4;
    const keys = [];
    while (p < end) {
        const ty = bytes[p++];
        const k0 = p;
        while (bytes[p] !== 0) p++;
        keys.push(td.decode(bytes.subarray(k0, p)));
        p++;
        if (ty === 0x10) p += 4;
        else if (ty === 0x0a) p += 0;
        else throw new Error("unexpected element type " + ty);
    }
    return keys;
}
const want = (n) => Array.from({ length: n }, (_, i) => String(i));

t.test("a dense array's keys are 0..n-1 across the digit boundaries", (a) => {
    for (const n of [0, 1, 9, 10, 11, 99, 100, 101, 1000, 1001, 10001]) {
        const keys = arrayKeys(BSONEncode({ a: Array.from({ length: n }, (_, i) => i) }));
        a.eq(keys.join(), want(n).join(), "n=" + n);
    }
});

t.test("a sparse array (not a fast array) keys every index, holes included as null", (a) => {
    for (const n of [12, 105, 1003]) {
        const arr = [];
        arr[n - 1] = 7;
        arr[0] = 1;
        const keys = arrayKeys(BSONEncode({ a: arr }));
        a.eq(keys.join(), want(n).join(), "n=" + n);
    }
});

await t.run();
