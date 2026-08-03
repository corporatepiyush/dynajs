/* test_ids.js -- NanoID and ULID in dyna:uuid (design 26).
 *
 * The property that matters for an ID generator is UNBIASED output, not just
 * "it returned 21 characters": a modulo instead of rejection sampling still
 * produces well-formed ids and quietly favours the first (256 % n) symbols.
 * A chi-square-shaped check over a large sample is what catches that.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_ids.js
 */
import { NanoID, NanoIDAlphabet, ULID, ULIDTime } from "dyna:uuid";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

/* ------------------------------------------------------------------ NanoID */

const DEFAULT_ALPHA = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLFGQZbfghjklqvwyzrict";

eq(NanoID().length, 21, "NanoID default length is 21");
for (const size of [1, 2, 8, 21, 32, 64, 255, 256, 1000]) {
    eq(NanoID(size).length, size, "NanoID(" + size + ") length");
}

/* Every character must come from the alphabet -- a mask bug leaks other bytes. */
{
    let bad = 0;
    for (let i = 0; i < 500; i++)
        for (const ch of NanoID(64))
            if (DEFAULT_ALPHA.indexOf(ch) < 0) bad++;
    eq(bad, 0, "every NanoID character is in the default alphabet");
}

/* Uniqueness over a large sample: 126 bits should never collide here. */
{
    const seen = new Set();
    for (let i = 0; i < 20000; i++) seen.add(NanoID());
    eq(seen.size, 20000, "20000 NanoIDs are all distinct");
}

/* THE BIAS CHECK. Rejection sampling gives a flat distribution; modulo does
 * not. With 64 symbols over 128000 draws the expected count per symbol is 2000;
 * a modulo bias on a 64-symbol alphabet is invisible (256 % 64 == 0), so the
 * real test is a NON-power-of-two alphabet, below. */
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
    /* 63 df: the 99.9% critical value is ~112. A flat generator sits far below. */
    assert(chi < 112, "NanoID default alphabet is flat (chi2 " + chi.toFixed(1) + " < 112)");
    eq(counts.size, DEFAULT_ALPHA.length, "every alphabet symbol was produced");
}

/* A non-power-of-two alphabet is where modulo bias actually shows. 62 symbols:
 * 256 % 62 == 8, so a modulo implementation over-produces the first 8. */
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
    /* 61 df: 99.9% critical value ~110. A modulo implementation lands far above. */
    assert(chi < 110, "NanoIDAlphabet(62 symbols) is unbiased (chi2 " + chi.toFixed(1) + " < 110)");

    /* PROVE THE BIAS CHECK CAN FAIL: score a deliberately skewed sample the same
     * way and require it to blow past the threshold. */
    const skew = new Map();
    for (const a of A62) skew.set(a, expect);
    skew.set("0", expect * 1.3);          /* the shape a modulo bias produces */
    skew.set("z", expect * 0.7);
    let chiBad = 0;
    for (const a of A62) {
        const o = skew.get(a);
        chiBad += ((o - expect) * (o - expect)) / expect;
    }
    assert(chiBad > 110, "fault injection: a skewed sample DOES exceed the threshold ("
           + chiBad.toFixed(1) + ")");
}

/* Custom alphabets */
eq(NanoIDAlphabet("ab", 32).length, 32, "NanoIDAlphabet length");
{
    let ok = true;
    for (const ch of NanoIDAlphabet("ab", 200)) if (ch !== "a" && ch !== "b") ok = false;
    assert(ok, "NanoIDAlphabet('ab') emits only a and b");
}
/* A two-symbol alphabet must produce BOTH -- a broken mask can lock to one. */
{
    const s = NanoIDAlphabet("ab", 400);
    assert(s.indexOf("a") >= 0 && s.indexOf("b") >= 0,
           "a two-symbol alphabet produces both symbols");
}

/* Refusals */
for (const bad of [0, -1, 100000]) {
    let threw = false;
    try { NanoID(bad); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "NanoID(" + bad + ") throws RangeError");
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

/* -------------------------------------------------------------------- ULID */

const CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

eq(ULID().length, 26, "ULID is 26 characters");
{
    let bad = 0;
    for (let i = 0; i < 500; i++)
        for (const ch of ULID()) if (CROCKFORD.indexOf(ch) < 0) bad++;
    eq(bad, 0, "every ULID character is Crockford base32");
    /* I, L, O and U are excluded precisely so a transcription is unambiguous. */
    for (const ch of "ILOU")
        assert(CROCKFORD.indexOf(ch) < 0, "Crockford excludes " + ch);
}

/* The timestamp round-trips through the first 10 characters. */
for (const ms of [0, 1, 1000, 1469918176385, 281474976710655]) {
    eq(ULIDTime(ULID(ms)), ms, "ULIDTime round-trips " + ms);
}

/* The spec's own vector: 1469918176385 encodes as 01ARYZ6S41. */
eq(ULID(1469918176385).slice(0, 10), "01ARYZ6S41",
   "ULID timestamp matches the spec's published example");

/* LEXICOGRAPHIC ORDERING BY TIME is the whole reason ULID exists over UUIDv4,
 * and it only holds because Crockford base32 is sorted by value. */
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

/* Same millisecond: the random tail still differs. */
{
    const seen = new Set();
    for (let i = 0; i < 5000; i++) seen.add(ULID(1700000000000));
    eq(seen.size, 5000, "5000 ULIDs in one millisecond are all distinct");
}

/* ULIDTime is case-insensitive (Crockford's rule) and refuses junk. */
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

if (fails) {
    print("test_ids: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_ids failed");
}
print("test_ids: " + n + " assertions, 0 failures");
