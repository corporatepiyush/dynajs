// covers: dyna:random.Random.normal
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_random_random_normal");
import { Random } from "dyna:random";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}

__kitS.section("detached-by-valueOf fill throws (no UAF)", () => {
    const r = new Random(1);
    const ta = new Uint8Array(16);
    if (typeof ta.buffer.transfer === "function") {
        let armed = false;
        const off = { valueOf() { if (armed) ta.buffer.transfer(); return 2; } };
        armed = true;
        let threw = null;
        try { r.fill(ta, off, 4); } catch (e) { threw = e; }
        assert(threw !== null, "detached-by-valueOf fill throws (no UAF)");
    }
    const frozen = Object.freeze([1, 2, 3, 4]);
    assert(new Random(6).sample(frozen, 2).length === 2, "sample on frozen works");
    assert([1, 2, 3, 4].includes(new Random(7).choice(frozen)), "choice on frozen works");
    assert(Number.isFinite(new Random(8).normal(1e308, 1)), "normal(1e308, 1) finite");
    assert(typeof new Random(9).normal(0, 1e300) === "number", "normal(0, 1e300) returns");
    const big = new Random(10).poisson(2147483647);
    assert(Number.isInteger(big) && Math.abs(big - 2147483647) < 500000,
        "poisson(2^31-1) lands in band");
    const j = new Random(15);
    j.jump();
    const cp = j.getState();
    const w = [j.nextU64(), j.nextU53()];
    j.setState(cp);
    assert(j.nextU64() === w[0] && j.nextU53() === w[1], "post-jump state replay");
});

__kitS.section("mixed sequence replay: identical final state", () => {
    const r1 = new Random(2024), r2 = new Random(2024);
    const t1 = [1, 2, 3, 4, 5], t2 = [1, 2, 3, 4, 5];
    r1.normal(3, 2); r1.exponential(1.5); r1.poisson(40);
    r1.shuffle(t1); r1.choice(t1); r1.sample(t1, 2); r1.bytes(7);
    r1.fill(new Uint8Array(5), 1, 3);
    r2.normal(3, 2); r2.exponential(1.5); r2.poisson(40);
    r2.shuffle(t2); r2.choice(t2); r2.sample(t2, 2); r2.bytes(7);
    r2.fill(new Uint8Array(5), 1, 3);
    assert(r1.getState().join(",") === r2.getState().join(","),
        "mixed sequence replay: identical final state");
});
__kitS.end();

print("test_random: all tests passed (" + n + " assertions)");
