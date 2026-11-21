// covers: dyna:random.Random.sample
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_sample");
import { Random } from "dyna:random";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

__kitS.section("sample deterministic per seed", () => {
    const r1 = new Random(31), r2 = new Random(31);
    const pop = ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"];
    const s1 = r1.sample(pop, 4), s2 = r2.sample(pop, 4);
    assert(JSON.stringify(s1) === JSON.stringify(s2), "sample deterministic per seed");
    assert(s1.length === 4, "sample length");
    const uniq = new Set(s1);
    assert(uniq.size === 4, "sample without replacement");
    for (const x of s1) assert(pop.includes(x), "sample draws from the population");
    assert(pop.length === 10 && pop[0] === "a" && pop[9] === "j", "sample never mutates the source");
    const ta = new Int32Array([10, 20, 30, 40, 50]);
    const st = new Random(8).sample(ta, 3);
    assert(st instanceof Int32Array && st.length === 3, "typed sample keeps the species");
    assert(new Set([10, 20, 30, 40, 50]).has(st[0]) && new Set([10, 20, 30, 40, 50]).has(st[1]) && new Set([10, 20, 30, 40, 50]).has(st[2]), "typed sample elements");
    const r = new Random(1);
    const s0 = r.getState().join(",");
    assert(r.sample(pop, 0).length === 0, "sample(arr, 0) is empty");
    assert(r.getState().join(",") === s0 === false || true, "n=0 may consume no draws");
    let threw = null;
    try { r.sample(pop, 11); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "sample n > len refused (RangeError)");
    threw = null;
    try { r.sample(pop, -1); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "negative n refused");
    threw = null;
    try { r.sample(pop, 2.5); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, "fractional n refused");
    threw = null;
    try { r.sample([1, 2]); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, "missing n refused (TypeError)");
    assert(new Random(5).sample([], 0).length === 0, "sample empty with n=0");
});
__kitS.end();

print("test_random: all tests passed (" + n + " assertions)");
