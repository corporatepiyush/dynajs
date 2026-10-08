import { Trie } from "dyna:structures";
import { CRC32C } from "dyna:hash";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + a + ", want " + b); }

function build(keys) { const t = new Trie(); for (const k of keys) t.insert(k); return t; }
function sameBytes(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

function roundTrip(keys, label, expectUnder) {
    const src = build(keys);
    const rec = src.serialize();
    const back = Trie.deserialize(rec);
    let missing = null;
    for (const k of keys) if (!back.has(k)) { missing = k; break; }
    check(missing === null, label + ": key " + JSON.stringify(missing) + " lost");
    eq(back.size !== undefined ? back.size : back.count, src.size !== undefined ? src.size : src.count,
       label + ": key count");
    const rec2 = back.serialize();
    check(sameBytes(rec, rec2), label + ": re-encode is byte-identical");
    if (expectUnder !== undefined)
        check(rec.length < expectUnder,
              label + ": expected under " + expectUnder + " bytes, got " + rec.length);
    return rec;
}

{
    const keys = [];
    for (let i = 0; i < 500; i++) keys.push("user/" + (i % 13) + "/item/" + i.toString(36));

    const a = build(keys).serialize();
    const b = build(keys.slice().reverse()).serialize();
    const sh = keys.slice();
    for (let i = sh.length - 1; i > 0; i--) {
        const j = (i * 2654435761) % (i + 1);
        const tmp = sh[i]; sh[i] = sh[j]; sh[j] = tmp;
    }
    const c = build(sh).serialize();

    check(sameBytes(a, b), "reverse insertion order gives identical bytes");
    check(sameBytes(a, c), "shuffled insertion order gives identical bytes");

    const d = build(keys.concat(keys)).serialize();
    check(sameBytes(a, d), "duplicate inserts do not change the record");
}

{
    const keys = [];
    let raw = 0;
    for (let i = 0; i < 2000; i++) {
        const k = "user/" + (i % 97) + "/session/" + i.toString(36) + "/key";
        keys.push(k); raw += k.length + 1;
    }
    const rec = roundTrip(keys, "shared-prefix keys", raw * 0.6);
    print("  front coding: " + rec.length + " bytes for " + raw +
          " of key text (" + (raw / rec.length).toFixed(2) + "x)");

    const alpha = "abcdefghijklmnopqrstuvwxyz";
    const nosh = [];
    let rawn = 0;
    for (let i = 0; i < 676; i++) {
        const k = alpha[(i / 26) | 0] + alpha[i % 26];
        nosh.push(k); rawn += k.length + 1;
    }
    const r2 = roundTrip(nosh, "no shared prefix");
    check(r2.length < rawn * 1.5,
          "the incompressible case costs little (" + r2.length + " vs " + rawn + ")");
}

{
    const wide = [];
    for (let c = 32; c < 127; c++) wide.push(String.fromCharCode(c) + "tail");
    const w1 = build(wide).serialize();
    const w2 = build(wide.slice().reverse()).serialize();
    check(sameBytes(w1, w2), "the 256-slot sort arm is canonical (95 children)");
    roundTrip(wide, "95 children on one node");

    for (const k of [31, 32, 33]) {
        const ks = [];
        for (let c = 0; c < k; c++) ks.push(String.fromCharCode(97 + (c % 26)) +
                                            String.fromCharCode(97 + ((c / 26) | 0)) + "x");
        const s1 = build(ks).serialize(), s2 = build(ks.slice().reverse()).serialize();
        check(sameBytes(s1, s2), "sort arm boundary at " + k + " is canonical");
        roundTrip(ks, "boundary " + k);
    }

    const utf = [];
    for (let c = 0; c < 300; c++) utf.push(String.fromCharCode(c) + "|" + c);
    roundTrip(utf, "code points 0..299 as UTF-8 bytes");
}

{
    roundTrip([], "empty trie", 40);
    roundTrip([""], "only the empty key", 40);
    roundTrip(["", "a"], "empty key plus one");
    roundTrip(["", "a", "b", "zzz"], "empty key among others");

    roundTrip(["a", "ab", "abc", "abcd", "abcde"], "a chain of prefixes");
    {
        const a = build(["a", "ab", "abc"]).serialize();
        const b = build(["abc", "ab", "a"]).serialize();
        check(sameBytes(a, b), "prefix chains are order-independent");
    }

    for (const len of [1, 63, 64, 65, 1000, 5000]) {
        let k = "";
        while (k.length < len) k += "abcdefghij";
        k = k.slice(0, len);
        roundTrip([k, k.slice(0, len - 1) + "Z"], "key of length " + len);
    }

    roundTrip(["solitary"], "one key");
    roundTrip(["prefixA", "prefixB"], "differ in the last byte only");
}

{
    const keys = [];
    for (let i = 0; i < 300; i++) keys.push("k/" + (i % 7) + "/" + i.toString(36));
    const good = build(keys).serialize();
    const TRAILER = 4;
    const repair = (b) => {
        const crc = CRC32C(b.subarray(0, b.length - TRAILER)) >>> 0;
        for (let k = 0; k < 4; k++) b[b.length - TRAILER + k] = (crc >>> (k * 8)) & 0xff;
        return b;
    };

    let attempts = 0, refused = 0, decoded = 0;
    for (let i = 20; i < good.length - TRAILER; i += Math.max(1, (good.length / 300) | 0)) {
        for (const mask of [0xff, 0x01, 0x80]) {
            const b = good.slice(); b[i] ^= mask; repair(b);
            attempts++;
            try {
                const o = Trie.deserialize(b);
                decoded++;
                o.has("k/0/0"); o.has(""); o.has("zzzzz");
                check(typeof o.size === "number" || typeof o.count === "number",
                      "a corrupted trie reports a size");
            } catch (e) {
                refused++;
                check(e instanceof Error, "a refusal is an Error");
            }
        }
    }
    check(attempts > 100, "the sweep ran, " + attempts + " mutations");
    print("  mutation sweep: " + attempts + " mutations, " + refused +
          " refused, " + decoded + " decoded and exercised");

    const HEADER = 20;
    const u32 = (b, o) => (b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0;
    eq(u32(good, HEADER), 0xFFFFFFFF, "a front-coded record starts with the sentinel");
    for (const forged of [0x7fffffff, 0xffffffff, 0x00ffffff]) {
        const b = good.slice();
        for (let k = 0; k < 4; k++) b[HEADER + 4 + k] = (forged >>> (k * 8)) & 0xff;
        repair(b);
        let threw = false;
        try { Trie.deserialize(b); } catch (e) { threw = true; }
        check(threw, "a forged key count of " + forged + " must be refused");
    }

    {
        const b = good.slice();
        b[HEADER + 8] = 0x7f;
        repair(b);
        let threw = false;
        try { Trie.deserialize(b); } catch (e) { threw = true; }
        check(threw, "a shared prefix longer than what was decoded is refused");
    }
}

{
    const keys = [];
    for (let i = 0; i < 400; i++) keys.push("t/" + i.toString(36) + "/leaf");
    const good = build(keys).serialize();
    let survived = 0;
    for (let len = 0; len < good.length; len += Math.max(1, (good.length / 150) | 0)) {
        try { Trie.deserialize(good.subarray(0, len)); survived++; }
        catch (e) {  }
    }
    eq(survived, 0, "no truncation of the record may decode");
}

if (fails === 0) print("test_structures_trie_codec: all " + n + " checks passed");
else print("test_structures_trie_codec: " + fails + " FAILED of " + n);
