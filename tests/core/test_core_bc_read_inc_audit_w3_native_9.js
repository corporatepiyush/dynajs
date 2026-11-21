// covers: c:src/serialize/bc_read.inc.c
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
t.after(() => removeAll(new Path(String(T))));
await t.run();
