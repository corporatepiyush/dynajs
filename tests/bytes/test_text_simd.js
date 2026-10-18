import { Bytes, isValidUtf8 }
    from "dyna:bytes";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

let seed = 0x12345678 >>> 0;
function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }
const AB = "abcd";
function randAscii(len) {
    let s = "";
    for (let i = 0; i < len; i++) s += AB[rnd() % AB.length];
    return s;
}

let cases = 0;
for (let len = 0; len <= 80; len++) {
    for (let rep = 0; rep < 20; rep++) {
        const s = randAscii(len);
        const ch = AB[rnd() % AB.length];
        let ref = 0;
        for (let i = 0; i < s.length; i++) if (s[i] === ch) ref++;
        assert(new Bytes(s).count(new Bytes(ch)) === ref, "count mismatch len=" + len);
        cases++;
    }
}

for (let len = 0; len <= 80; len++) {
    for (let sl = 1; sl <= 8; sl++) {
        for (let rep = 0; rep < 8; rep++) {
            const s = randAscii(len);
            let set = "";
            for (let k = 0; k < sl; k++) set += "cdefghij"[rnd() % 8];
            let ref = -1;
            for (let i = 0; i < s.length; i++)
                if (set.indexOf(s[i]) !== -1) { ref = i; break; }
            assert(new Bytes(s).indexOfAny(set) === ref,
                   "indexOfAny mismatch len=" + len + " set=" + set);
            cases++;
        }
    }
}
print("test_text_simd: " + cases + " count/find_first_of cases OK");

function utf8Bytes(cp) {
    if (cp < 0x80) return [cp];
    if (cp < 0x800) return [0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)];
    if (cp < 0x10000)
        return [0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)];
    return [0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
            0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)];
}
function buf(arr) { return new Uint8Array(arr).buffer; }

for (let rep = 0; rep < 3000; rep++) {
    const bytes = [];
    const ncp = rnd() % 40;
    for (let i = 0; i < ncp; i++) {
        let cp;
        do { cp = rnd() % 0x110000; } while (cp >= 0xD800 && cp <= 0xDFFF);
        bytes.push(...utf8Bytes(cp));
    }
    assert(isValidUtf8(buf(bytes)) === true, "valid utf8 rejected @rep" + rep);
}
const invalid = [
    [0x80],
    [0xC0, 0x80],
    [0xE0, 0x80, 0x80],
    [0xF0, 0x80, 0x80, 0x80],
    [0xED, 0xA0, 0x80],
    [0xC2],
    [0xE2, 0x82],
    [0xF4, 0x90, 0x80, 0x80],
    [0x41, 0xFF, 0x42],
    [0xC2, 0x41],
];
for (const seq of invalid)
    assert(isValidUtf8(buf(seq)) === false,
           "invalid utf8 accepted: " + seq);
for (let off = 0; off < 40; off++) {
    const b = [];
    for (let i = 0; i < 40; i++) b.push(i === off ? 0xFF : 0x61);
    assert(isValidUtf8(buf(b)) === false, "bad byte @" + off + " accepted");
}
print("test_text_simd: utf8 validation OK");

print("test_text_simd: all tests passed");
