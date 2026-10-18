import { CSR } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("FAIL " + w + (d ? "  [" + d + "]" : "")); } };
const err = (fn) => { try { fn(); return null; } catch (e) { return e; } };
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

const values = [1, 2, 3, 4, 5];
const cols = [0, 2, 1, 0, 3];
const ptr = [0, 2, 3, 5];
const ref = new CSR(values, cols, ptr, 4);
const want = [0, 1, 2].map((i) => ref.row(i));

for (const [name, C, P] of [
    ["Int32Array", Int32Array, Int32Array],
    ["Uint32Array", Uint32Array, Uint32Array],
    ["mixed", Int32Array, Array],
    ["Float64Array values", Uint32Array, Int32Array],
]) {
    const v = name === "Float64Array values" ? new Float64Array(values) : values;
    const m = new CSR(v, C.from(cols), P === Array ? ptr : P.from(ptr), 4);
    ok(m.rows === 3 && m.nnz === 5 && same([0, 1, 2].map((i) => m.row(i)), want),
        `${name} columns/rowPointers build the same matrix as plain arrays`, JSON.stringify([0, 1, 2].map((i) => m.row(i))));
    m.close();
}

{
    const buf = new ArrayBuffer(4 * 8);
    const view = new Int32Array(buf, 8, 5);
    view.set(cols);
    const m = new CSR(values, view, Int32Array.from(ptr), 4);
    ok(same([0, 1, 2].map((i) => m.row(i)), want), "an Int32Array view at a byte offset is read from its offset");
    m.close();
}

let e = err(() => new CSR([1], Int32Array.of(-1), Int32Array.of(0, 1), 4));
ok(e instanceof RangeError && /columns\[0\]/.test(e.message), "a negative Int32Array column index is refused", String(e));
e = err(() => new CSR([1], Uint32Array.of(4), Uint32Array.of(0, 1), 4));
ok(e instanceof RangeError && /columns\[0\]/.test(e.message), "a Uint32Array column index past cols is refused", String(e));
e = err(() => new CSR([1, 2], Int32Array.of(0, 1), Int32Array.of(0, 2, 1), 4));
ok(e instanceof RangeError && /rowPointers/.test(e.message), "decreasing Int32Array rowPointers are refused", String(e));
e = err(() => new CSR([1, 2], Int32Array.of(0), Int32Array.of(0, 2), 4));
ok(e instanceof TypeError && /columns has 1 entries but values has 2/.test(e.message), "an Int32Array column count mismatch is refused", String(e));
e = err(() => new CSR([1], Int16Array.of(0), [0, 1], 4));
ok(e instanceof TypeError, "a 16-bit index array is still refused", String(e));
e = err(() => new CSR([1], new Int32Array(new SharedArrayBuffer(4)), [0, 1], 4));
ok(e instanceof TypeError && /SharedArrayBuffer/.test(e.message), "an index array over a SharedArrayBuffer is refused", String(e));

ref.close();
print(`test_ml_csr_int_index: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(`${fail} failure(s)`);
