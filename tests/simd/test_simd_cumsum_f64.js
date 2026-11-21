// covers: dyna:simd.cumsum
// cumsum(Float64Array): left-to-right float64 running sum, in place. The oracle is the same loop in JS, which is
// exactly the definition, so every element must match to the bit -- including where float32 would have drifted.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_cumsum_f64");
import { cumsum } from "dyna:simd";

let failures = 0,
    passed = 0;
function ok(cond, msg) {
    if (cond) passed++;
    else {
        failures++;
        if (failures < 20) print("FAIL " + msg);
    }
}
let seed = 12345;
const rnd = () => ((seed = (Math.imul(seed ^ (seed >>> 15), 0x2c1b3c6d) + 0x6d2b79f5) >>> 0) / 2 ** 32);

for (const n of [0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 1000, 360000]) {
    const src = Float64Array.from({ length: n }, () => (rnd() - 0.3) * 10 ** Math.floor(rnd() * 8));
    const want = new Float64Array(n);
    let acc = 0;
    for (let i = 0; i < n; i++) want[i] = acc += src[i];
    const got = src.slice();
    ok(cumsum(got) === got, `n=${n}: returns its argument`);
    ok(got.every((v, i) => Object.is(v, want[i])), `n=${n}: every element is the ordered running sum`);
}

__kitS.section("float64 matches the JS running sum at 360000", () => {
    // a long float32 scan drifts; the float64 one equals the exact sum of small integers
    const n = 360000;
    const ones = new Float64Array(n).fill(0.1);
    const f = Float32Array.from(ones);
    cumsum(ones);
    cumsum(f);
    let acc = 0;
    for (let i = 0; i < n; i++) acc += 0.1;
    ok(ones[n - 1] === acc, "float64 matches the JS running sum at 360000");
    ok(Math.abs(f[n - 1] - 36000) / 36000 > 1e-4, `float32 drifts measurably (${f[n - 1]}) -- the reason this form exists`);
});

__kitS.section("a window past the end is refused", () => {
    const a = Float64Array.from([1, 2, 3, 4, 5, 6]);
    cumsum(a, 2, 3);
    ok([...a].join() === "1,2,3,7,12,6", `window (offset 2, length 3) scans only its slice (${[...a]})`);
    let threw = "";
    try {
        cumsum(a, 4, 3);
    } catch (e) {
        threw = String(e);
    }
    ok(/window out of range/.test(threw), "a window past the end is refused");
    threw = "";
    try {
        cumsum(new BigInt64Array(4));
    } catch (e) {
        threw = String(e);
    }
    ok(/Float64Array/.test(threw), `BigInt64Array (also 8 bytes per element) is refused (${threw})`);
    const frozen = new Float64Array(new ArrayBuffer(16, { maxByteLength: 16 })).fill(1);
    ok(cumsum(frozen)[1] === 2, "resizable buffer");
});
__kitS.end();
print(`simd cumsum f64: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
