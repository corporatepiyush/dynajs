// Split from tests/test_cov_sys_time_random_uuid_misc.js: the random sections.
import { Random } from "dyna:random";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
print("=== random ===");
{
    const rA = new Random(42);
    const rB = new Random(42);
    const seqA = [rA.nextU53(), rA.nextU53(), rA.nextU53()];
    const seqB = [rB.nextU53(), rB.nextU53(), rB.nextU53()];
    eq(JSON.stringify(seqA), JSON.stringify(seqB), "same seed same sequence");
    const rC = new Random(43);
    assert(rC.nextU53() !== seqA[0] || rC.nextU53() !== seqA[1], "different seed different stream");

    const rD = new Random(7);
    for (let i = 0; i < 100; i++) {
        const v = rD.nextU53();
        assert(v >= 0 && v <= Number.MAX_SAFE_INTEGER, "nextU53 within safe range");
    }
    const rE = new Random(9);
    for (let i = 0; i < 50; i++) {
        const f = rE.nextFloat();
        assert(f >= 0 && f < 1, "nextFloat in [0,1): " + f);
    }
    const rF = new Random(11);
    for (let i = 0; i < 50; i++) {
        const b = rF.nextBounded(10);
        assert(b >= 0 && b < 10 && Number.isInteger(b), "nextBounded(10) int in [0,10)");
    }
    try { new Random(-42).nextU53(); n++; } catch (e) { n++; }
    try { new Random(2 ** 53).nextU53(); n++; } catch (e) { n++; }
    const rG = new Random(13);
    let allZero = true;
    for (let i = 0; i < 20; i++) if (rG.nextBounded(1) !== 0) allZero = false;
    assert(allZero, "nextBounded(1) always 0");

    const buf = new Uint8Array(32);
    const rH = new Random(21);
    rH.fill(buf);
    let nonZero = 0;
    for (const b of buf) if (b !== 0) nonZero++;
    assert(nonZero > 20, "fill populated most bytes");
    const buf2 = new Uint8Array(32);
    new Random(21).fill(buf2);
    eq(JSON.stringify([...buf]), JSON.stringify([...buf2]), "fill deterministic per seed");

    const rI = new Random({ seed: 5 });
    assert(typeof rI.nextU53() === "number", "options-object ctor works");

    try { new Random(-42).nextU53(); n++; } catch (e) { n++; }
    try { new Random(2 ** 53).nextU53(); n++; } catch (e) { n++; }
}
if (fails) {
    print("test_random_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_random_cov_sys_time_random_uuid_misc failed");
}
print("test_random_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
