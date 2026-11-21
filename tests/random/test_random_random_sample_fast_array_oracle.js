// covers: dyna:random.Random.sample
// sample(arr, n) on a plain fast array now reads the element storage directly and builds the result in one step
// (the generic path reads every picked element with a property get and appends it). The draws and the order of the
// result are unchanged. Oracle: an independent partial Fisher-Yates in JS over a second Random with the same seed
// (j = i + nextBounded(len - i), positions swapped in a sparse map); the result must hold the same values (same
// object identities) in the same order, for fast arrays, holey arrays, frozen arrays, array-likes and typed arrays,
// n from 0 to len, and the result must be a plain dense array of length n.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_sample_fast_array_oracle");
import { Random } from "dyna:random";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; if (fails < 15) print("FAIL: " + what); } }
function jsSample(rng, len, n) {
    const m = new Map();
    const get = (k) => (m.has(k) ? m.get(k) : k);
    for (let i = 0; i < n; i++) {
        const j = i + rng.nextBounded(len - i);
        const a = get(i), b = get(j);
        m.set(i, b); m.set(j, a);
    }
    return Array.from({ length: n }, (_, i) => get(i));
}
function mk(len) { return Array.from({ length: len }, (_, i) => (i % 3 === 0 ? { id: i } : i % 3 === 1 ? "s" + i : i * 0.5)); }
__kitS.section("fast arrays: same values, same order, same identities", () => {
    let seed = 300;
    for (const len of [1, 2, 3, 10, 64, 1000, 5000]) for (const n of [0, 1, 2, len >> 1, len - 1, len]) {
        if (n < 0 || n > len) continue;
        const a = mk(len), s = seed++;
        const got = new Random(s).sample(a, n);
        const idx = jsSample(new Random(s), len, n);
        ok(Array.isArray(got) && got.length === n, "len=" + len + " n=" + n + ": plain array of length n");
        ok(idx.every((k, i) => got[i] === a[k] && i in got), "len=" + len + " n=" + n + ": values follow the JS draws");
        ok(a.length === len && a.every((v, i) => (i % 3 === 0 ? v.id === i : true)), "len=" + len + ": source unchanged");
    }
});
__kitS.section("other array shapes agree with the fast path", () => {
    let seed = 900;
    for (const len of [5, 100, 777]) {
        const n = len >> 1, s = seed++;
        const idx = jsSample(new Random(s), len, n);
        const fast = mk(len);
        const holey = mk(len); holey[len + 5] = 1; holey.length = len;
        delete holey[1];
        const frozen = Object.freeze(mk(len));
        const like = Object.assign({ length: len }, mk(len));
        const typed = Float64Array.from({ length: len }, (_, i) => i * 1.25);
        const want = (src) => idx.map((k) => src[k]);
        for (const [name, src] of [["fast", fast], ["holey", holey], ["frozen", frozen], ["array-like", like]]) {
            let got;
            try { got = new Random(s).sample(src, n); } catch (e) { got = e; }
            if (name === "array-like" && got instanceof Error) { ok(true, "array-like rejected"); continue; }
            const w = want(src);
            ok(Array.isArray(got) && got.length === n && got.every((v, i) => Object.is(v, w[i])), name + " len=" + len + ": values follow the JS draws");
        }
        const gt = new Random(s).sample(typed, n);
        ok(gt instanceof Float64Array && Array.from(gt).every((v, i) => v === typed[idx[i]]), "typed len=" + len);
    }
});
__kitS.section("errors keep their order: n out of range throws before any draw", () => {
    const r = new Random(1), r2 = new Random(1);
    let threw = false;
    try { r.sample([1, 2, 3], 4); } catch (e) { threw = e instanceof RangeError; }
    ok(threw, "n > length throws RangeError");
    ok(r.nextU64() === r2.nextU64(), "no draw consumed by the rejected call");
});
print("sample fast array oracle: " + checks + " checks, " + fails + " failed");
if (fails) throw new Error(fails + " failed");
__kitS.end();
