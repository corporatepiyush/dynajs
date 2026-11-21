// covers: dyna:uuid.ULID
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_ulid_idgen");
import { NanoID, NanoIDAlphabet, ULID, ULIDTime } from "dyna:uuid";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const DEFAULT_ALPHA = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLFGQZbfghjklqvwyzrict";

const CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

eq(ULID().length, 26, "ULID is 26 characters");

__kitS.section("every ULID character is Crockford base32", () => {
    let bad = 0;
    for (let i = 0; i < 500; i++)
        for (const ch of ULID()) if (CROCKFORD.indexOf(ch) < 0) bad++;
    eq(bad, 0, "every ULID character is Crockford base32");
    for (const ch of "ILOU")
        assert(CROCKFORD.indexOf(ch) < 0, "Crockford excludes " + ch);
});

eq(ULID(1469918176385).slice(0, 10), "01ARYZ6S41",
   "ULID timestamp matches the spec's published example");

__kitS.section("ULIDs sort lexicographically by their timestamp", () => {
    let ordered = true;
    let prev = ULID(0);
    for (let ms = 1; ms < 3000; ms += 7) {
        const cur = ULID(ms);
        if (!(cur > prev)) { ordered = false; break; }
        prev = cur;
    }
    assert(ordered, "ULIDs sort lexicographically by their timestamp");
});

__kitS.section("5000 ULIDs in one millisecond are all distinct", () => {
    const seen = new Set();
    for (let i = 0; i < 5000; i++) seen.add(ULID(1700000000000));
    eq(seen.size, 5000, "5000 ULIDs in one millisecond are all distinct");
});

__kitS.section("ULID refuses a timestamp past 48 bits", () => {
    let threw = false;
    try { ULID(281474976710656); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "ULID refuses a timestamp past 48 bits");
});

__kitS.section("ULID(0n) round-trips", () => {
    eq(ULIDTime(ULID(0n)), 0, "ULID(0n) round-trips");
    eq(ULIDTime(ULID(1234567890123n)), 1234567890123, "ULID(1234567890123n) round-trips");
    eq(ULIDTime(ULID(281474976710655n)), 281474976710655, "ULID(2**48-1 as bigint) round-trips");
    let threw = false;
    try { ULID(281474976710656n); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "ULID(2**48 as bigint) throws RangeError");
    threw = false;
    try { ULID(-1n); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "ULID(-1n) throws RangeError");
    threw = false;
    try { ULID(2n ** 64n + 5n); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "ULID(2**64n+5n) throws RangeError (no mod-2^64 wrap)");
    threw = false;
    try { ULID(6.5); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "ULID(6.5) throws RangeError (no silent truncation)");
    eq(ULIDTime(ULID(1234.0)), 1234, "ULID(1234.0) is an integer value");
});
__kitS.end();

if (fails) {
    print("test_ids: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_ids failed");
}
print("test_ids: " + n + " assertions, 0 failures");
