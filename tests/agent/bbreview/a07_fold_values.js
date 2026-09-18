// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["1/0 = Infinity number"];
__EXP[1] = ["-1/0 = -Infinity -Infinity"];
__EXP[2] = ["0/0 = NaN number"];
__EXP[3] = ["0.1+0.2 = 0.30000000000000004 true"];
__EXP[4] = ["2**31 = 2147483648 true -2147483648"];
__EXP[5] = ["2**32 = 4294967296 true"];
__EXP[11] = ["'12'+'34' = 1234 1234 true"];
__EXP[13] = [["~int32 edge: 2147483648", "~int32 edge: 2147483648"]];
__EXP[14] = ["minint: -2147483648 -2147483648"];
__EXP[16] = ["surrogate fold: true 2 1f600"];
__EXP[17] = ["surrogate concat: a\ud83d\ude00bc 5"];
__EXP[19] = ["emoji idx: d83d de00"];
__EXP[20] = ["strnum fold: 12 12 1234 46"];
__EXP[21] = ["neg zero fold: -0 true true"];
__EXP[22] = [["~big int fold: 9007199254740992", "~big int fold: 9007199254740992"]];
// A: fold boundary values — must match node bit-for-bit
__L(0, "1/0 =", 1 / 0, typeof (1 / 0));
__L(1, "-1/0 =", -(1 / 0), (-1) / 0);
__L(2, "0/0 =", 0 / 0, typeof (0 / 0));
__L(3, "0.1+0.2 =", 0.1 + 0.2, (0.1 + 0.2) === 0.30000000000000004);
__L(4, "2**31 =", 2 ** 31, 2 ** 31 > 0, 2 ** 31 | 0);
__L(5, "2**32 =", 2 ** 32, 2 ** 32 === 4294967296);
__A("a07_fold_values.js:2**-1 =", function () { assert_eq(2 ** -1, 0.5, "2**-1 ="); });
__A("a07_fold_values.js:(-2)**2 =", function () { assert_eq((-2) ** 2, 4, "(-2)**2 ="); });
__A("a07_fold_values.js:2**0 =", function () { assert_eq(2 ** 0, 1, "2**0 ="); });
__A("a07_fold_values.js:(-2)**3 =", function () { assert_eq((-2) ** 3, -8, "(-2)**3 ="); });
__A("a07_fold_values.js:(-2)**0 =", function () { assert_eq((-2) ** 0, 1, "(-2)**0 ="); });
__L(11, "'12'+'34' =", "12" + "34", +"1234", "12" + "34" === "1234");
__A("a07_fold_values.js:1e21*2 =", function () { assert_eq(1e21 * 2, 2e+21, "1e21*2 ="); });
__L(13, "int32 edge:", 2147483647 + 1, (2147483647 + 1) | 0, 2147483648 * 2);
__L(14, "minint:", -2147483648, (-2147483648) | 0);
__A("a07_fold_values.js:0x7fffffff+1:", function () { assert_eq((0x7fffffff + 1) | 0, -2147483648, "0x7fffffff+1:"); });
__L(16, "surrogate fold:", "\u{1F600}" === "\uD83D\uDE00", "\u{1F600}".length, "\u{1F600}".codePointAt(0).toString(16));
__L(17, "surrogate concat:", "a\u{1F600}b" + "c", ("a\u{1F600}b" + "c").length);
__A("a07_fold_values.js:wide fold len:", function () { assert_eq(("\u{1F600}" + "!").length, 3, "wide fold len:"); });
__L(19, "emoji idx:", "\u{1F600}".charCodeAt(0).toString(16), "\u{1F600}".charCodeAt(1).toString(16));
__L(20, "strnum fold:", "1" + 2, 1 + "2", "" + 12 + 34, 12 + 34 + "");
__L(21, "neg zero fold:", -0, 1 / -0 === -Infinity, 0 === -0);
__L(22, "big int fold:", 2 ** 53, 2 ** 53 + 1 === 2 ** 53, 9007199254740993);

summary("bbreview");
