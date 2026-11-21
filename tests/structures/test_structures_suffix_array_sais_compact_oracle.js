// covers: dyna:structures.SuffixArray
// SA-IS gathers the reduced string (names of LMS substrings) and the LMS positions with branchless
// compactions (store, then advance by the predicate) into a buffer of n1 + 1 slots, so the trailing store
// stays in bounds. Texts with many recursion levels (periodic, few-letter) and every small length must sort
// exactly like the naive suffix sort here.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_suffix_array_sais_compact_oracle");
import { SuffixArray } from "dyna:structures";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; print("FAIL: " + what); } }
let seed = 0xA11CE;
const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
function naive(b) {
    const idx = Array.from({ length: b.length }, (_, i) => i);
    idx.sort((x, y) => {
        while (x < b.length && y < b.length) { if (b[x] !== b[y]) return b[x] - b[y]; x++; y++; }
        return x < b.length ? 1 : y < b.length ? -1 : 0;
    });
    return idx;
}
function check(b, what) {
    const got = new SuffixArray(b).array(), want = naive(b);
    let same = got.length === want.length;
    for (let i = 0; same && i < want.length; i++) same = got[i] === want[i];
    ok(same, what + " (n=" + b.length + ")");
}

__kitS.section("random texts over 1..4 letters at every length up to 200", () => {
    for (const k of [1, 2, 3, 4])
        for (let n = 1; n <= 200; n++) {
            const b = new Uint8Array(n);
            for (let i = 0; i < n; i++) b[i] = 97 + ((rnd() * k) | 0);
            check(b, "k=" + k);
        }
});

__kitS.section("periodic and Fibonacci-word texts recurse several levels", () => {
    let f0 = "a", f1 = "ab";
    while (f1.length < 3000) { const t = f1 + f0; f0 = f1; f1 = t; }
    for (const s of [f1, "abc".repeat(700), "aab".repeat(500) + "b", "mississippi".repeat(150), "ba".repeat(999)])
        check(new TextEncoder().encode(s), s.slice(0, 12));
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_structures_suffix_array_sais_compact_oracle: " + checks + " checks passed");
__kitS.end();
