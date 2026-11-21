// covers: dyna:random.Random.nextBounded
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_next_bounded");
import { Random } from "dyna:random";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

__kitS.section("nextU64 is a BigInt", () => {
    const r = new Random(7);
    {
        assert(typeof r.nextU64() === "bigint", "nextU64 is a BigInt");
        const u53 = r.nextU53();
        assert(typeof u53 === "number" && Number.isInteger(u53), "nextU53 integer");
        assert(u53 >= 0 && u53 < 2 ** 53, "nextU53 in [0,2^53)");
        for (let i = 0; i < 1000; i++) {
            const f = r.nextFloat();
            assert(f >= 0 && f < 1, "nextFloat in [0,1)");
        }
        for (let i = 0; i < 1000; i++) {
            const d = r.nextBounded(6);
            assert(typeof d === "number" && d >= 0 && d < 6, "d6 in [0,6)");
        }
        assert(typeof r.nextBounded(10n) === "bigint", "BigInt bound => BigInt");
    }
});

__kitS.section("nextBounded(0) throws RangeError", () => {
    const r = new Random(1);
    {
        let threw = false;
        try { r.nextBounded(0); } catch { threw = true; }
        assert(threw, "nextBounded(0) throws RangeError");
        threw = false;
        try { r.nextBounded(-5); } catch { threw = true; }
        assert(threw, "nextBounded(-5) throws");
    }
});

__kitS.section("coin flips ~balanced", () => {
    const r = new Random(123);
    {
        let ones = 0;
        for (let i = 0; i < 10000; i++) ones += r.nextBounded(2);
        assert(ones > 4500 && ones < 5500, "coin flips ~balanced (" + ones + "/10000)");
    }
});

__kitS.section("the argument was coerced exactly once", () => {
    const r = new Random(1);
    let calls = 0;
    const v = r.nextBounded({ valueOf() { calls++; return 6; } });
    assert(calls === 1, "the argument was coerced exactly once");
    assert(v >= 0 && v < 6, "and the draw is in range: " + v);
});

__kitS.section("nextBounded(6.5) throws RangeError (no silent truncation)", () => {
    const r = new Random(1);
    {
        let threw = false;
        try { r.nextBounded(6.5); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "nextBounded(6.5) throws RangeError (no silent truncation)");
        threw = false;
        try { r.nextBounded(0n); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "nextBounded(0n) throws RangeError");
    }
    {
        const B = 2n ** 64n - 1n;
        for (let i = 0; i < 100; i++) {
            const v = r.nextBounded(B);
            assert(typeof v === "bigint" && v >= 0n && v < B,
                   "nextBounded(2**64-1) in range @" + i);
        }
    }
    {
        const r1 = new Random(9n);
        for (let i = 0; i < 50; i++)
            assert(r1.nextBounded(1n) === 0n, "nextBounded(1n) is always 0 @" + i);
    }
});
__kitS.end();

print("test_random: all tests passed (" + n + " assertions)");
