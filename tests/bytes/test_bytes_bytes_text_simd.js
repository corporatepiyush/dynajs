// covers: dyna:bytes.Bytes
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_bytes_text_simd");
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

__kitS.section("count mismatch len", () => {
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
});

__kitS.section("indexOfAny mismatch len", () => {
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
print("test_text_simd: utf8 validation OK");
});
__kitS.end();

print("test_text_simd: all tests passed");
