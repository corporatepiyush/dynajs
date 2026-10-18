// flags: --std
// Split from tests/test_audit_w3_native_9.js: the core sections.
import { Path, makeTempDir, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";

const t = suite("test_core_audit_w3_native_9");
const best = (f, reps = 5) => { let b = Infinity; for (let r = 0; r < reps; r++) { const t = performance.now(); f(); b = Math.min(b, performance.now() - t); } return b; };
const T = makeTempDir("w3n9_");
// ---- E2c-04: `local += x` with one int and one double --------------------
// ORACLE: ECMA-262 Number::add is IEEE-754 double addition after ToNumber;
// the expected value is computed through a different opcode path (a plain
// `a + b` on arguments), and Object.is distinguishes -0 and NaN.
// CONTROL: int += int and double += double are unchanged paths.
t.test("E2c-04 local += x with mixed int and double", ({ ok }) => {
    const add = (a, b) => a + b;
    const vals = [0, -0, 1, -1, 7, 2147483647, -2147483648, 0.5, -0.5, 1.5, 1e300, -1e300, 5e-324, NaN, Infinity, -Infinity, 2 ** 53, 123456789.987];
    let bad = 0, n = 0;
    const run = () => {
        for (const a of vals) for (const b of vals) {
            let s = a; s += b; n++;
            if (!Object.is(s, add(a, b))) bad++;
            let t = b; t += 1; n++;
            if (!Object.is(t, add(b, 1))) bad++;
            let u = 1; u += b; n++;
            if (!Object.is(u, add(1, b))) bad++;
        }
    };
    run(); run();
    ok(bad === 0, "E2c-04: `local += x` equals `a + b` for " + n + " int/double pairs (" + bad + " differ)");
    let s = 0.5; for (let i = 0; i < 1000; i++) s += i;
    ok(s === 499500.5, "control: a double accumulator of ints sums exactly (" + s + ")");
    let k = 2147483640; for (let i = 0; i < 20; i++) k += 1;
    ok(k === 2147483660, "control: int += int still widens past int32 (" + k + ")");
});
// ---- E3-14: new TypedArray(array) -----------------------------------------
// ORACLE: ECMA-262 23.2.5.1.4: values = IterableToList(source) using the
// source's @@iterator, THEN each is converted and stored. So (1) the result
// equals an element-wise copy, (2) a replaced Array.prototype[@@iterator] or a
// replaced %ArrayIteratorPrototype%.next must be honoured, (3) the list is a
// snapshot: a valueOf that grows the source cannot add elements.
// SCALING: building from an array is the same work as Float64Array.from; it
// was 6.6x slower because it went through the iterator protocol per element.
// CONTROL: construction from another typed array (unchanged path).
t.test("E3-14 new TypedArray(array)", ({ ok }) => {
    const N = 400000, src = [];
    for (let i = 0; i < N; i++) src.push(i * 0.5);
    const a = new Float64Array(src);
    let same = a.length === N;
    for (let i = 0; i < N && same; i += 997) same = a[i] === src[i];
    ok(same, "new Float64Array(array) copies the elements");
    ok(Array.from(new Int8Array([1.9, -1.9, 300, "7", null, undefined])).join() === "1,-1,44,7,0,0", "conversion per element is unchanged");
    ok(Array.from(new Uint8Array(new Float64Array([1, 2, 3]))).join() === "1,2,3", "control: construction from a typed array");

    const it = Array.prototype[Symbol.iterator];
    Array.prototype[Symbol.iterator] = function* () { yield 7; yield 8; };
    let viaIter;
    try { viaIter = Array.from(new Uint8Array([1, 2, 3, 4])); } finally { Array.prototype[Symbol.iterator] = it; }
    ok(viaIter.join() === "7,8", "a replaced Array.prototype[@@iterator] is honoured (" + viaIter.join() + ")");

    const grow = [1, { valueOf() { grow.push(99); return 2; } }, 3];
    const g = new Float64Array(grow);
    ok(g.length === 3 && Array.from(g).join() === "1,2,3", "the source is snapshotted before conversion runs user code (" + Array.from(g).join() + ")");

    const tNew = best(() => new Float64Array(src)), tFrom = best(() => Float64Array.from(src));
    ok(tNew < tFrom * 3, "[red] E3-14: new Float64Array(array) costs about what Float64Array.from does (" + tNew.toFixed(2) + " ms vs " + tFrom.toFixed(2) + " ms)");
});
t.after(() => removeAll(new Path(String(T))));
await t.run();
