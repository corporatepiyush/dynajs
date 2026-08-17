/*
 * The Multiset record: sorted, front-coded keys with varint counts.
 *
 * The old record spent 12 bytes of overhead per entry -- a u32 key length and
 * a u64 count -- and emitted entries in the container's INSERTION order, so
 * two multisets holding exactly the same thing produced different bytes. That
 * is not a size problem, it is a correctness one: a record that is not a
 * function of the value cannot be compared, deduplicated or cached.
 *
 * So the record is now sorted, which makes it canonical AND makes the keys
 * front-codable. Each entry carries (shared, suffix_len, suffix, count).
 *
 * The oracle is always the (key -> count) mapping over every key, never the
 * record length or the distinct count -- a codec that swapped two counts has
 * the same size and the same number of entries.
 */
import { Multiset } from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

function build(pairs) {
    const m = new Multiset();
    for (const [k, c] of pairs) m.add(k, c);
    return m;
}

function roundTrip(pairs, label, expectUnder) {
    const src = build(pairs), rec = src.serialize(), back = Multiset.deserialize(rec);
    eq(back.size, src.size, label + ": total size");
    let bad = null;
    for (const [k] of pairs)
        if (back.count(k) !== src.count(k)) { bad = k; break; }
    check(bad === null, label + ": count differs for " + JSON.stringify(bad) +
          (bad !== null ? " (" + src.count(bad) + " vs " + back.count(bad) + ")" : ""));
    /* FIXED POINT */
    check(sameBytes(rec, back.serialize()), label + ": re-encode is byte-identical");
    if (expectUnder !== undefined)
        check(rec.length < expectUnder, label + ": " + rec.length + " bytes, want < " + expectUnder);
    return rec;
}

/* ------------------------------------------------- 1. CANONICAL order
   The record must not depend on insertion order. This is the property the
   sort exists for and the only test that can see it. */
{
    const pairs = [];
    for (let i = 0; i < 500; i++) pairs.push(["user:" + i, (i % 7) + 1]);

    const a = build(pairs).serialize();
    const b = build(pairs.slice().reverse()).serialize();
    const sh = pairs.slice();
    for (let i = sh.length - 1; i > 0; i--) {
        const j = (i * 2654435761) % (i + 1);
        const t = sh[i]; sh[i] = sh[j]; sh[j] = t;
    }
    const c = build(sh).serialize();

    check(sameBytes(a, b), "reverse insertion order gives identical bytes");
    check(sameBytes(a, c), "shuffled insertion order gives identical bytes");

    /* Reaching a count by many adds or by one must also give one record. */
    const one = new Multiset(), many = new Multiset();
    for (const [k] of pairs) one.add(k, 5);
    for (const [k] of pairs) for (let i = 0; i < 5; i++) many.add(k);
    check(sameBytes(one.serialize(), many.serialize()),
          "add(k,5) and five add(k) produce the same record");
}

/* ------------------------------------------- 2. front coding really fires */
{
    const pairs = [];
    let raw = 0;
    for (let i = 0; i < 3000; i++) {
        const k = "session/2026/07/user-" + i;
        pairs.push([k, 1 + (i % 3)]);
        raw += k.length + 12;                /* what the old record cost */
    }
    const rec = roundTrip(pairs, "shared-prefix keys", raw / 2);
    print("  front coding: " + rec.length + " bytes vs " + raw +
          " for the old form (" + (raw / rec.length).toFixed(2) + "x)");

    /* ADVERSARIAL: keys sharing nothing and counts too large to be one byte.
       This is where the format can only lose, so it must lose by little. */
    const nosh = [];
    let rawn = 0;
    for (let i = 0; i < 1500; i++) {
        const k = String.fromCharCode(65 + (i % 26)) + i + "-" + (i * 7919);
        nosh.push([k, 1000000]);
        rawn += k.length + 12;
    }
    const r2 = roundTrip(nosh, "no shared prefix, large counts");
    check(r2.length < rawn,
          "even the adversarial case beats the old form (" + r2.length +
          " vs " + rawn + ")");
}

/* ---------------------------------------- 3. counts at varint boundaries
   The count is a varint, so its length steps at 127/128, 16383/16384 and so
   on. Each side must be exact, and a count is a u64 -- the top of that range
   has to survive too. */
{
    for (const c of [1, 2, 126, 127, 128, 129, 16383, 16384, 2097151, 2097152,
                     268435455, 268435456, 4294967295, 4294967296,
                     9007199254740991]) {
        const m = new Multiset();
        m.add("solo", c);
        const back = Multiset.deserialize(m.serialize());
        eq(back.count("solo"), m.count("solo"), "count " + c + " survives");
    }
}

/* ------------------------------------------- 4. key shapes and boundaries */
{
    roundTrip([], "empty multiset", 40);
    roundTrip([["", 3]], "the empty key", 40);
    roundTrip([["", 1], ["a", 2]], "empty key plus one");
    roundTrip([["a", 1], ["ab", 2], ["abc", 3]], "keys that are prefixes");
    /* the longest key first, to prove the sort is what orders them */
    {
        const x = build([["a", 1], ["ab", 2], ["abc", 3]]).serialize();
        const y = build([["abc", 3], ["ab", 2], ["a", 1]]).serialize();
        check(sameBytes(x, y), "prefix chains are order-independent");
    }

    /* long keys: the decoder's buffer has to grow */
    for (const len of [1, 63, 64, 65, 1000, 5000]) {
        let k = "";
        while (k.length < len) k += "abcdefghij";
        k = k.slice(0, len);
        roundTrip([[k, 7], [k.slice(0, len - 1) + "Z", 9]], "key of length " + len);
    }

    /* non-ASCII keys are stored as UTF-8 bytes and must round-trip */
    {
        const pairs = [];
        for (let c = 0; c < 300; c++) pairs.push([String.fromCharCode(c) + "|" + c, c + 1]);
        roundTrip(pairs, "code points 0..299 as UTF-8");
    }

    /* many entries, to drive the sort through its recursion */
    {
        const pairs = [];
        for (let i = 0; i < 5000; i++)
            pairs.push([((i * 48271) % 99991).toString(36) + ":x", (i % 11) + 1]);
        roundTrip(pairs, "5000 scrambled keys");
    }
}

/* ------------------------------------- 5. removing an entry, then encoding
   A key whose count drops to zero must not appear in the record; one that is
   merely reduced must appear with its new count. */
{
    const m = new Multiset();
    for (let i = 0; i < 50; i++) m.add("k" + i, 10);
    for (let i = 0; i < 25; i++) m.remove("k" + i, 10);      /* to zero */
    m.remove("k30", 4);                                       /* to 6 */
    const back = Multiset.deserialize(m.serialize());
    eq(back.count("k0"), 0, "a removed key is absent from the record");
    eq(back.count("k30"), 6, "a reduced key keeps its new count");
    eq(back.size, m.size, "total size after removals");
    let bad = null;
    for (let i = 0; i < 50; i++)
        if (back.count("k" + i) !== m.count("k" + i)) { bad = i; break; }
    check(bad === null, "every count survives removals, differs at " + bad);
}

/* --------------------------------------------- 6. ADVERSARIAL RECORDS */
{
    const pairs = [];
    for (let i = 0; i < 300; i++) pairs.push(["p/" + (i % 9) + "/" + i.toString(36), (i % 5) + 1]);
    const good = build(pairs).serialize();
    const TRAILER = 4;
    const repair = (b) => {
        const crc = CRC32C(b.subarray(0, b.length - TRAILER)) >>> 0;
        for (let k = 0; k < 4; k++) b[b.length - TRAILER + k] = (crc >>> (k * 8)) & 0xff;
        return b;
    };

    let attempts = 0, refused = 0, decoded = 0;
    for (let i = 20; i < good.length - TRAILER; i += Math.max(1, (good.length / 250) | 0)) {
        for (const mask of [0xff, 0x01, 0x80]) {
            const b = good.slice(); b[i] ^= mask; repair(b);
            attempts++;
            try {
                const o = Multiset.deserialize(b);
                decoded++;
                o.count("p/0/0"); o.count(""); o.size;
                check(typeof o.size === "number", "a corrupted multiset reports a size");
            } catch (e) {
                refused++;
                check(e instanceof Error, "a refusal is an Error");
            }
        }
    }
    check(attempts > 100, "the sweep ran, " + attempts + " mutations");
    print("  mutation sweep: " + attempts + " mutations, " + refused +
          " refused, " + decoded + " decoded and exercised");

    const HEADER = 20;                     /* then u32 sentinel, u32 count */
    const u32 = (b, o) => (b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0;
    eq(u32(good, HEADER), 0xFFFFFFFF, "a new-form record starts with the sentinel");

    /* A FORGED ENTRY COUNT: an entry can be as short as three bytes, so only
       an explicit bound stops a huge count from being believed. */
    for (const forged of [0x7fffffff, 0xffffffff, 0x00ffffff]) {
        const b = good.slice();
        for (let k = 0; k < 4; k++) b[HEADER + 4 + k] = (forged >>> (k * 8)) & 0xff;
        repair(b);
        let threw = false;
        try { Multiset.deserialize(b); } catch (e) { threw = true; }
        check(threw, "a forged entry count of " + forged + " must be refused");
    }

    /* A FORGED `shared` on the FIRST entry: nothing has been decoded yet, so
       any non-zero shared is out of range. The first varint sits right after
       the sentinel and the count, and 0x7E is a complete one-byte varint --
       a byte with the high bit set would be a continuation and would desync
       the stream, throwing for an unrelated reason. */
    {
        const b = good.slice();
        eq(b[HEADER + 8], 0x00, "the first entry shares nothing, so its varint is 0");
        b[HEADER + 8] = 0x7E;
        repair(b);
        let threw = false;
        try { Multiset.deserialize(b); } catch (e) { threw = true; }
        check(threw, "a shared prefix longer than what was decoded is refused");
    }
}

/* ------------------------------------------- 7. truncation at every length */
{
    const pairs = [];
    for (let i = 0; i < 400; i++) pairs.push(["t/" + i.toString(36), (i % 3) + 1]);
    const good = build(pairs).serialize();
    let survived = 0;
    for (let len = 0; len < good.length; len += Math.max(1, (good.length / 150) | 0)) {
        try { Multiset.deserialize(good.subarray(0, len)); survived++; }
        catch (e) { /* expected */ }
    }
    eq(survived, 0, "no truncation of the record may decode");
}

if (fails === 0) print("test_structures_multiset_codec: all " + n + " checks passed");
else print("test_structures_multiset_codec: " + fails + " FAILED of " + n);
