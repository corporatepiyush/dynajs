import { NanoID, NanoIDAlphabet, ULID, ULIDTime } from "dyna:uuid";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const DEFAULT_ALPHA = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLFGQZbfghjklqvwyzrict";

eq(NanoID().length, 21, "NanoID default length is 21");
for (const size of [1, 2, 8, 21, 32, 64, 255, 256, 1000]) {
    eq(NanoID(size).length, size, "NanoID(" + size + ") length");
}

{
    let bad = 0;
    for (let i = 0; i < 500; i++)
        for (const ch of NanoID(64))
            if (DEFAULT_ALPHA.indexOf(ch) < 0) bad++;
    eq(bad, 0, "every NanoID character is in the default alphabet");
}

{
    const seen = new Set();
    for (let i = 0; i < 20000; i++) seen.add(NanoID());
    eq(seen.size, 20000, "20000 NanoIDs are all distinct");
}

{
    const counts = new Map();
    const N = 2000, LEN = 64;
    for (let i = 0; i < N; i++)
        for (const ch of NanoID(LEN)) counts.set(ch, (counts.get(ch) || 0) + 1);
    const total = N * LEN, expect = total / DEFAULT_ALPHA.length;
    let chi = 0;
    for (const a of DEFAULT_ALPHA) {
        const o = counts.get(a) || 0;
        chi += ((o - expect) * (o - expect)) / expect;
    }
    assert(chi < 112, "NanoID default alphabet is flat (chi2 " + chi.toFixed(1) + " < 112)");
    eq(counts.size, DEFAULT_ALPHA.length, "every alphabet symbol was produced");
}

{
    const A62 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    eq(A62.length, 62, "the biased-alphabet probe really has 62 symbols");
    const counts = new Map();
    const N = 3000, LEN = 64;
    for (let i = 0; i < N; i++)
        for (const ch of NanoIDAlphabet(A62, LEN)) counts.set(ch, (counts.get(ch) || 0) + 1);
    const total = N * LEN, expect = total / 62;
    let chi = 0;
    for (const a of A62) {
        const o = counts.get(a) || 0;
        chi += ((o - expect) * (o - expect)) / expect;
    }
    assert(chi < 110, "NanoIDAlphabet(62 symbols) is unbiased (chi2 " + chi.toFixed(1) + " < 110)");

    const skew = new Map();
    for (const a of A62) skew.set(a, expect);
    skew.set("0", expect * 1.3);
    skew.set("z", expect * 0.7);
    let chiBad = 0;
    for (const a of A62) {
        const o = skew.get(a);
        chiBad += ((o - expect) * (o - expect)) / expect;
    }
    assert(chiBad > 110, "fault injection: a skewed sample DOES exceed the threshold ("
           + chiBad.toFixed(1) + ")");
}

eq(NanoIDAlphabet("ab", 32).length, 32, "NanoIDAlphabet length");
{
    let ok = true;
    for (const ch of NanoIDAlphabet("ab", 200)) if (ch !== "a" && ch !== "b") ok = false;
    assert(ok, "NanoIDAlphabet('ab') emits only a and b");
}
{
    const s = NanoIDAlphabet("ab", 400);
    assert(s.indexOf("a") >= 0 && s.indexOf("b") >= 0,
           "a two-symbol alphabet produces both symbols");
}

for (const bad of [0, -1, 100000]) {
    let threw = false;
    try { NanoID(bad); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "NanoID(" + bad + ") throws RangeError");
}
{
    for (const bad of [2 ** 32 + 5, 2 ** 64 + 5, 6.5, NaN]) {
        let threw = false;
        try { NanoID(bad); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "NanoID(" + bad + ") throws RangeError (no int32 wrap)");
    }
    {
        let threw = false;
        try { NanoIDAlphabet("ab", 2 ** 32 + 5); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "NanoIDAlphabet size wrap throws RangeError");
    }
    {
        let threw = false;
        try { ULID(2 ** 64 + 5); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "ULID(2**64 + 5) throws RangeError (no int64 wrap)");
    }
}
for (const bad of ["", "x"]) {
    let threw = false;
    try { NanoIDAlphabet(bad, 5); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "NanoIDAlphabet with " + bad.length + " symbols throws RangeError");
}
{
    let threw = false;
    try { NanoIDAlphabet("aé", 5); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "NanoIDAlphabet refuses a non-ASCII alphabet");
}

const CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

eq(ULID().length, 26, "ULID is 26 characters");
{
    let bad = 0;
    for (let i = 0; i < 500; i++)
        for (const ch of ULID()) if (CROCKFORD.indexOf(ch) < 0) bad++;
    eq(bad, 0, "every ULID character is Crockford base32");
    for (const ch of "ILOU")
        assert(CROCKFORD.indexOf(ch) < 0, "Crockford excludes " + ch);
}

for (const ms of [0, 1, 1000, 1469918176385, 281474976710655]) {
    eq(ULIDTime(ULID(ms)), ms, "ULIDTime round-trips " + ms);
}

eq(ULID(1469918176385).slice(0, 10), "01ARYZ6S41",
   "ULID timestamp matches the spec's published example");

{
    let ordered = true;
    let prev = ULID(0);
    for (let ms = 1; ms < 3000; ms += 7) {
        const cur = ULID(ms);
        if (!(cur > prev)) { ordered = false; break; }
        prev = cur;
    }
    assert(ordered, "ULIDs sort lexicographically by their timestamp");
}

{
    const seen = new Set();
    for (let i = 0; i < 5000; i++) seen.add(ULID(1700000000000));
    eq(seen.size, 5000, "5000 ULIDs in one millisecond are all distinct");
}

eq(ULIDTime(ULID(12345).toLowerCase()), 12345, "ULIDTime accepts lowercase");
for (const bad of ["", "short", "0".repeat(27), "0".repeat(25) + "U"]) {
    let threw = false;
    try { ULIDTime(bad); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "ULIDTime refuses " + JSON.stringify(bad.slice(0, 30)));
}
{
    let threw = false;
    try { ULID(281474976710656); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "ULID refuses a timestamp past 48 bits");
}

{
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
}

if (fails) {
    print("test_ids: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_ids failed");
}
print("test_ids: " + n + " assertions, 0 failures");
