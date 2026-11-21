// covers: c:src/modules/matcher/matcher-approx.c
// JaroWinkler keeps its scratch on the stack: ASCII operands of up to 64 bytes are widened into
// local arrays instead of a malloc'd code-point copy, and the kernel's match flags / matched-letter
// buffer live in 256-entry stack arrays when both sides fit (heap above that, as before). The two
// mallocs and two frees per call were a third of the call, and libmalloc's free read the clock.
// The oracle is a plain JS Jaro-Winkler over code points. The sweep crosses every switch-over (63/64/
// 65 and 255/256/257 code points, on either side), mixes ASCII with non-ASCII operands (which still
// take the heap copy), and pins the textbook values.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_matcher_jaro_winkler_scratch");
import { JaroWinkler } from "dyna:matcher";

let pass = 0, fail = 0;
const near = (a, b, m) => { if (Math.abs(a - b) < 1e-12) pass++; else { fail++; print("FAIL " + m + ": got " + a + ", want " + b); } };
function jw(s1, s2) {
    const a = [...s1].map((c) => c.codePointAt(0)), b = [...s2].map((c) => c.codePointAt(0));
    if (s1 === s2) return 1;
    if (!a.length || !b.length) return a.length === b.length ? 1 : 0;
    let win = Math.floor(Math.max(a.length, b.length) / 2); if (win > 0) win--;
    const bf = new Uint8Array(b.length), am = [];
    for (let i = 0; i < a.length; i++) {
        const lo = Math.max(0, i - win), hi = Math.min(b.length, i + win + 1);
        for (let j = lo; j < hi; j++) if (!bf[j] && b[j] === a[i]) { bf[j] = 1; am.push(a[i]); break; }
    }
    const m = am.length; if (!m) return 0;
    let t = 0, k = 0;
    for (let j = 0; j < b.length; j++) if (bf[j]) { if (b[j] !== am[k]) t++; k++; }
    t = Math.floor(t / 2);
    let j = (m / a.length + m / b.length + (m - t) / m) / 3;
    if (j > 0.7) { let l = 0; while (l < 4 && l < a.length && l < b.length && a[l] === b[l]) l++; j += l * 0.1 * (1 - j); }
    return j;
}
const chk = (a, b, m) => near(JaroWinkler(a, b), jw(a, b), m + " " + a.length + "/" + b.length);

__kitS.section("textbook values", () => {
    near(JaroWinkler("MARTHA", "MARHTA"), 0.9611111111111111, "MARTHA");
    near(JaroWinkler("DWAYNE", "DUANE"), 0.84, "DWAYNE");
    near(JaroWinkler("DIXON", "DICKSONX"), 0.8133333333333332, "DIXON");
    near(JaroWinkler("", ""), 1, "both empty");
    near(JaroWinkler("a", ""), 0, "one empty");
    near(JaroWinkler("abc", "xyz"), 0, "no common letter");
});

__kitS.section("lengths across the stack limits", () => {
    let x = 7;
    const rnd = (k) => { x = (x * 1103515245 + 12345) >>> 0; return (x >>> 8) % k; };
    const word = (n, alpha) => { let s = ""; for (let i = 0; i < n; i++) s += alpha[rnd(alpha.length)]; return s; };
    const lens = [1, 2, 3, 31, 63, 64, 65, 100, 127, 128, 129, 255, 256, 257, 300, 513];
    for (const la of lens)
        for (const lb of lens) {
            const a = word(la, "abcdef");
            const b = rnd(2) ? a.slice(0, lb).padEnd(lb, "x") : word(lb, "abcdef");
            chk(a, b, "ascii");
        }
    for (const la of [5, 63, 64, 65, 255, 256, 257])
        for (const lb of [5, 64, 65, 256, 257]) {
            chk(word(la, "abé€"), word(lb, "abé"), "non-ascii both");
            chk(word(la, "abc"), word(lb, "abéc"), "ascii vs non-ascii");
            chk(word(la, "a\u{1F600}b"), word(lb, "abc"), "astral vs ascii");
        }
});

__kitS.section("repeated calls reuse nothing stale", () => {
    const big = "q".repeat(400), small = ["abcd", "abdc", "dcba", "xbcd"];
    for (let i = 0; i < 300; i++) {
        chk(big, big.slice(0, 399) + "z", "heap call");
        const a = small[i & 3], b = small[(i >> 2) & 3];
        chk(a, b, "stack call after heap");
    }
});

__kitS.end();
print(`test_matcher_jaro_winkler_scratch: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
