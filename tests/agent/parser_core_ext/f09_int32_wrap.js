// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[4] = [["~i5", "~i5"]];
__EXP[6] = ["i7 2 -2147483648 true"];
__EXP[7] = ["i8 true Infinity -Infinity"];
__EXP[9] = ["i10 2 -2 1.5 NaN"];
__EXP[10] = ["i11 75 2 1"];
__EXP[11] = ["i12 true true false -Infinity"];
__EXP[12] = ["i13 -1 3 5 -3"];
__EXP[13] = ["i14 -1 4294967295 0"];
__EXP[14] = [["~i15 4294967294", "~i15 4294967294"]];
__EXP[15] = ["i16 1e+21 1e-7 1.2345678901234568e+29"];
__EXP[16] = ["i17 25 1 4 0.5"];
__EXP[17] = ["i18 -Infinity Infinity Infinity"];
// F9: the int32 folder must respect float64 semantics where the mathematical
// result leaves int32 range, and wrap ONLY where the operator itself wraps.
__A("f09_int32_wrap.js:i1", function () { assert_eq(2147483647 + 1, 2147483648, "i1"); });
__A("f09_int32_wrap.js:i2", function () { assert_eq(-2147483648 - 1, -2147483649, "i2"); });
__A("f09_int32_wrap.js:i3", function () { assert_eq((2147483647 + 1) | 0, -2147483648, "i3"); });
__A("f09_int32_wrap.js:i4", function () { assert_eq(2147483648 === 2147483647 + 1, true, "i4"); });
__L(4, "i5", (2 ** 31) | 0, 2 ** 53);
__A("f09_int32_wrap.js:i6", function () { assert_eq((-1) >>> 0, 4294967295, "i6"); });
__L(6, "i7", 1 << 33, 1 << 31, (1 << 31) < 0);
__L(7, "i8", 0 / 0 !== 0 / 0, 1 / 0, -1 / 0);
__A("f09_int32_wrap.js:i9", function () { assert_eq(Number.MAX_SAFE_INTEGER + 1 === Number.MAX_SAFE_INTEGER, false, "i9"); });
__L(9, "i10", 5 % 3, -5 % 3, 5.5 % 2, Infinity % 2);
__L(10, "i11", "37" * 2 + 1, true + true, null + 1);
__L(11, "i12", -0 === 0, Object.is(-0, -0), Object.is(-0, 0), 1 / -0);
__L(12, "i13", ~0, ~~3.7, -(-5), +"-3");
__L(13, "i14", 4294967295 | 0, 4294967295 >>> 0, 4294967296 | 0);
__L(14, "i15", 2147483647 * 2, (2147483647 * 2) | 0, 2147483648 * 2);
__L(15, "i16", 1e21, 1e-7, 123456789012345678901234567890);
__L(16, "i17", (0 - 5) ** 2, 2 ** 0, (-2) ** 2, 2 ** -1);
__L(17, "i18", Math.max(), Math.min(), -Math.max());

summary("parser_core_ext");
