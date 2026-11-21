// covers: dyna:serialize.ObjectId
// ObjectId: the 12-byte layout (big-endian seconds, 5 per-process random bytes, big-endian counter),
// uniqueness and ordering of generated ids, parsing and refusals, equality and the time accessors.
import { suite } from "../lib/kit.js";
import { ObjectId, BSONEncode, BSONDecode } from "dyna:serialize";

const t = suite("serialize.ObjectId");

t.test("generated ids: layout, uniqueness, monotonic counter within a process", (a) => {
    const before = Math.floor(Date.now() / 1000);
    const ids = Array.from({ length: 5000 }, () => new ObjectId());
    const after = Math.floor(Date.now() / 1000);
    const hexes = ids.map((i) => i.toHexString());
    a.eq(new Set(hexes).size, 5000, "unique");
    a.ok(hexes.every((h) => /^[0-9a-f]{24}$/.test(h)));
    const secs = ids[0].getTimestamp().getTime() / 1000;
    a.ok(secs >= before && secs <= after, "seconds prefix is now");
    a.ok(hexes.every((h) => h.slice(8, 18) === hexes[0].slice(8, 18)), "the 5 random bytes are fixed per process");
    let rising = 0;
    for (let i = 1; i < hexes.length; i++) if (parseInt(hexes[i].slice(18), 16) === (parseInt(hexes[i - 1].slice(18), 16) + 1) % 0x1000000) rising++;
    a.eq(rising, hexes.length - 1, "the counter increments by one");
});

t.test("parsing and copying", (a) => {
    const h = "507f1f77bcf86cd799439011";
    const o = new ObjectId(h);
    a.eq(o.toHexString(), h);
    a.eq(String(o), h);
    a.eq(JSON.stringify({ o }), `{"o":"${h}"}`);
    a.eq(new ObjectId(h.toUpperCase()).toHexString(), h);
    a.eq(new ObjectId(o.id).toHexString(), h);
    a.eq(new ObjectId(o).toHexString(), h);
    a.ok(o.equals(h) && o.equals(new ObjectId(h)) && !o.equals(new ObjectId()) && !o.equals(null) && !o.equals(5));
    a.eq(o.getTimestamp().getTime(), 0x507f1f77 * 1000);
    o.id[0] = 0;
    a.eq(o.toHexString(), h, "id is a copy");
});

t.test("refusals and isValid", (a) => {
    for (const bad of ["xyz", "507f1f77bcf86cd79943901", "507f1f77bcf86cd79943901g", 5, {}, new Uint8Array(11)])
        a.throws(() => new ObjectId(bad), TypeError);
    a.ok(ObjectId.isValid("507f1f77bcf86cd799439011") && !ObjectId.isValid("nope") && !ObjectId.isValid() && ObjectId.isValid(new Uint8Array(12)));
    a.throws(() => ObjectId.createFromTime(-1), RangeError);
    a.eq(ObjectId.createFromTime(0x10).toHexString(), "000000100000000000000000");
});

t.test("round trip through BSON keeps the class", (a) => {
    const o = new ObjectId();
    const back = BSONDecode(BSONEncode({ o })).o;
    a.ok(back instanceof ObjectId && back.equals(o));
});

await t.run();
