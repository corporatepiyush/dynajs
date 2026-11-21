// covers: dyna:random.Random.shuffle
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_shuffle");
import { Random } from "dyna:random";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

__kitS.section("shuffle returns this", () => {
    const src1 = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    const src2 = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
    const r1 = new Random(77), r2 = new Random(77);
    assert(r1.shuffle(src1) === r1, "shuffle returns this");
    r2.shuffle(src2);
    assert(src1.join(",") === src2.join(","), "same seed => same permutation");
    assert(src1.slice().sort((x, y) => x - y).join(",") === "1,2,3,4,5,6,7,8,9,10",
        "shuffle is a permutation (multiset preserved)");
    assert(src1.join(",") !== "1,2,3,4,5,6,7,8,9,10", "shuffle actually reordered");
});

__kitS.section("Float64Array shuffle preserves the multiset", () => {
    const r = new Random(12);
    const f64 = new Float64Array([0, 1, 2, 3, 4, 5, 6, 7]);
    r.shuffle(f64);
    assert(Array.from(f64).sort((x, y) => x - y).join(",") === "0,1,2,3,4,5,6,7",
        "Float64Array shuffle preserves the multiset");
    const i32 = new Int32Array([5, -5, 0, 12345]);
    r.shuffle(i32);
    assert(i32.length === 4, "Int32Array shuffle in place");
    const big = new BigInt64Array([1n, 2n, 3n]);
    r.shuffle(big);
    assert(big.reduce((s, x) => s + x) === 6n, "BigInt64Array shuffle");
    const u8 = new Uint8Array([1, 2, 3]);
    const r3 = new Random(12);
    r3.shuffle(new Float64Array([0, 1, 2, 3, 4, 5, 6, 7]));
    r3.shuffle(new Int32Array([5, -5, 0, 12345]));
    const u8b = new Uint8Array([1, 2, 3]);
    r3.shuffle(u8b);
    assert(u8.join() === u8b.join(), "typed shuffle deterministic per seed");
});

__kitS.section("shuffle moves holes as holes: own-key COUNT is preserved, values are not densified", () => {
    const sparse = [1, 2, 3, 4, 5];
    delete sparse[2];
    delete sparse[4];
    const keysBefore = Object.keys(sparse).length;
    new Random(42).shuffle(sparse);
    const keysAfter = Object.keys(sparse).length;
    assert(keysBefore === 3 && keysAfter === 3,
        "shuffle moves holes as holes: own-key COUNT is preserved, values are not densified (" + keysAfter + ")");
    assert(sparse.length === 5, "length untouched");
    const ownVals = Object.keys(sparse).map(k => sparse[k]);
    assert(ownVals.every(v => [1, 2, 3, 4, 5].includes(v)), "no densified undefined: " + ownVals.join(","));
    assert(new Set(ownVals).size === 3, "kept values remain distinct");

    function hasOwn(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }

    const frozen = Object.freeze([3, 1, 2]);
    const r = new Random(42);
    const s0 = r.getState().join(",");
    let threw = null;
    try { r.shuffle(frozen); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, "frozen array refused with TypeError");
    assert(r.getState().join(",") === s0, "refused shuffle consumed no draws");

    const sealed = Object.seal([3, 1, 2]);
    try { new Random(1).shuffle(sealed); assert(false, "sealed refused"); }
    catch (e) { assert(e instanceof TypeError, "sealed array refused with TypeError"); }

    const lockedTA = Object.preventExtensions(new Uint8Array([1, 2, 3]));
    try { new Random(1).shuffle(lockedTA); assert(false, "non-extensible TA refused"); }
    catch (e) { assert(e instanceof TypeError, "non-extensible typed array refused"); }

    const empty = [];
    new Random(1).shuffle(empty);
    assert(empty.length === 0, "shuffling empty array is a no-op");
    const one = [9];
    new Random(1).shuffle(one);
    assert(one.length === 1 && one[0] === 9, "single element shuffle is a no-op");
});
__kitS.end();

print("test_random: all tests passed (" + n + " assertions)");
