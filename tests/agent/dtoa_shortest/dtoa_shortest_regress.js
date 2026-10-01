__EXP = {};
__EXP[1] = ["REGRESS-OK"];
var out = (typeof print === "function") ? print : console.log;
var fails = 0;
function eq(actual, expected, label) {
    if (actual !== expected) {
        fails++;
        __L(0, "FAIL " + label + ": got " + actual + " want " + expected);
    }
}

eq(String(1e300), "1e+300", "1e300");
eq(String(1e-300), "1e-300", "1e-300");
eq(String(1.7976931348623157e308), "1.7976931348623157e+308", "MAX_VALUE");
eq(String(5e-324), "5e-324", "MIN_VALUE");
eq(String(2.2250738585072014e-308), "2.2250738585072014e-308", "min normal");

eq(String(0.1), "0.1", "0.1");
eq(String(1 / 3), "0.3333333333333333", "1/3");
eq(String(0.5), "0.5", "0.5");
eq(String(1e-7), "1e-7", "1e-7");
eq(String(Number.EPSILON), "2.220446049250313e-16", "EPSILON");
eq(String(123456789012345680), "123456789012345680", "int>2^53");

eq(String(1e20), "100000000000000000000", "1e20 fixed");
eq(String(1e21), "1e+21", "1e21 exp");
eq(String(1e22), "1e+22", "1e22 exp");
eq(String(-0), "0", "minus zero");
eq(String(0), "0", "zero");
eq((1e21).toFixed(2), "1e+21", "toFixed>=1e21");

eq((0.1).toFixed(2), "0.10", "toFixed");
eq((1 / 3).toString(36), "0.c", "radix36");
eq((255).toString(16), "ff", "radix16");
eq((1 / 3).toPrecision(1), "0.3", "toPrecision");
eq((1 / 3).toExponential(3), "3.333e-1", "toExponential");

eq(String(6.090821257125e287), "6.090821257125e+287", "binade-boundary pin");
eq(String(Math.pow(2, -1017)), "7.120236347223045e-307", "binade pin 2^-1017 (straddle-above candidate)");
eq(String(Math.pow(2, -645)), "6.84940421565126e-195", "binade pin 2^-645");
eq(String(Math.pow(2, 976)), "6.386688990511104e+293", "binade pin 2^976");
eq((Math.pow(2, 956)).toExponential(), "6.090821257125e+287", "binade pin toExponential");
eq(String(0.1), "0.1", "control 0.1");
eq(String(1e300), "1e+300", "control 1e300");

if (fails === 0) {
    __L(1, "REGRESS-OK");
} else {
    __L(2, "REGRESS-FAIL " + fails);
}

summary("dtoa_shortest");
