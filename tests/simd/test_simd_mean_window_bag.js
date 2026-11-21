// covers: dyna:simd.mean
// C3b: the d.ts types the window bag {offset?, length?, limit?} as all-optional, so an empty bag {} (or one holding only
// undefined/null) is the same as no window (whole array). It used to throw "window requires length (or limit)". An offset
// without a length stays a TypeError (the window needs an end); unknown keys and length+limit together stay TypeErrors.
import { suite } from "../lib/kit.js";
import * as simd from "dyna:simd";

const t = suite("test_simd_mean_window_bag");
const X = Float32Array.of(1, 2, 3, 4, 5, 6, 7, 8);

t.test("empty bag is the whole array", (a) => {
    a.eq(simd.mean(X, {}), simd.mean(X), "mean(x, {}) == mean(x)");
    a.eq(simd.mean(X, {}), 4.5, "mean(x, {}) == 4.5");
    a.eq(simd.mean(X, { offset: undefined, length: null }), 4.5, "undefined/null fields are absent");
    a.eq(simd.mean(X, { limit: undefined }), 4.5, "undefined limit is absent");
    a.eq(simd.sum(X, {}), simd.sum(X), "the shared window helper: sum(x, {}) == sum(x)");
    a.eq(simd.max(X, {}), 8, "max(x, {}) is the whole-array max");
});

t.test("windows still apply", (a) => {
    a.eq(simd.mean(X, { offset: 2, length: 2 }), 3.5, "{offset:2,length:2}");
    a.eq(simd.mean(X, { length: 4 }), 2.5, "{length:4} starts at 0");
    a.eq(simd.mean(X, { limit: 2 }), 1.5, "limit aliases length");
    a.eq(simd.mean(X, 6, 2), 7.5, "positional (offset, length)");
});

t.test("malformed bags still throw", (a) => {
    a.throws(() => simd.mean(X, { offset: 1 }), { type: TypeError, message: "window requires length (or limit)" }, "offset without length");
    a.throws(() => simd.mean(X, { length: 1, limit: 1 }), { type: TypeError, message: "never both" }, "length and limit");
    a.throws(() => simd.mean(X, { __bogus: 1 }), { type: TypeError, message: 'unknown option "__bogus"' }, "unknown key");
    a.throws(() => simd.mean(X, { offset: 4, length: 8 }), RangeError, "window past the end");
    a.throws(() => simd.mean(Float32Array.of(), {}), { type: RangeError, message: "empty" }, "empty array with empty bag");
});

await t.run();
