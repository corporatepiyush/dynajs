// converted transcript generator: record stream pinned by
// count + FNV-1a digest + first-40 records (node oracle; dynajs divergences are explicit)
__PIN_EXP_N = 40;
__PIN_EXP_LINES = ["-1.1434293291498499e-7", "2.3616792882790083e-31", "-6.487727165555293e+252", "7.014975529234866e+118", "2.721944079472344e+80", "6.172906046866731e-240", "2.493020904803264e-67", "4.0520442908802754e+88", "1.368021607977509e-22", "-6.037478505930678e+77", "-9.40217978605167e+111", "1.1596687782213098e+288", "5.094777575518353e+277", "7.210480200768755e-219", "-1.83865832583438e-105", "5.492933315876686e+241", "-7.445780194560952e-141", "3.2023679488159002e+237", "1.870665730283803e-235", "-2.3934822267126757e-110", "7.021752008674665e-34", "4.817502640846961e+117", "-2.5954209446752366e-90", "3.566105527643953e+60", "5.86701642479937e-126", "3.019285370596544e+304", "685728332839630800000", "-9.777670160228982e+82", "2.3278409702390007e+195", "2.9274629359967505e-227", "-2.713048416635321e-69", "3.049617070641933e-43", "-5.73810651589526e+243", "-7.749828234391796e-277", "3.0178442239568133e-121", "-1.4190802965094217e+283", "-8.948657232364516e-48", "2.0892401657151198e-75", "-3.0891585285856264e-240", "-4.355154499743509e-47"];
__PIN_DYN_LINES = ["-1.1434293291498499e-7", "2.3616792882790083e-31", "-6.487727165555293e+252", "7.014975529234866e+118", "2.721944079472344e+80", "6.172906046866731e-240", "2.493020904803264e-67", "4.0520442908802754e+88", "1.368021607977509e-22", "-6.037478505930678e+77", "-9.40217978605167e+111", "1.1596687782213098e+288", "5.094777575518353e+277", "7.210480200768755e-219", "-1.83865832583438e-105", "5.492933315876686e+241", "-7.445780194560952e-141", "3.2023679488159002e+237", "1.870665730283803e-235", "-2.3934822267126757e-110", "7.021752008674665e-34", "4.817502640846961e+117", "-2.5954209446752366e-90", "3.566105527643953e+60", "5.86701642479937e-126", "3.019285370596544e+304", "685728332839630800000", "-9.777670160228982e+82", "2.3278409702390007e+195", "2.9274629359967505e-227", "-2.713048416635321e-69", "3.049617070641933e-43", "-5.73810651589526e+243", "-7.749828234391796e-277", "3.0178442239568133e-121", "-1.4190802965094217e+283", "-8.948657232364516e-48", "2.0892401657151198e-75", "-3.0891585285856264e-240", "-4.355154499743509e-47"];
// dtoa differential battery 2: 500k more random bit patterns (different
// seed than dtoa_string_diff.js) + toString(radix 2..36) sweep +
// toFixed/toPrecision/toExponential tables.
// Runs unmodified on dynajs AND node; a driver diffs two engines' outputs
// byte-for-byte. The radix sweep and the *Fixed/*Precision/*Exponential
// tables guard the paths the fast path must NOT touch (radix != 10,
// EXP_DISABLED, FRAC and FIXED formats).
var out = (typeof print === "function") ? print : console.log;

var u32 = new Uint32Array(2);
var f64 = new Float64Array(u32.buffer);
function setbits(hi, lo) { u32[0] = lo; u32[1] = hi; }
var s = 987654321 >>> 0;
function rnd() { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s; }
var N = 500000;
for (var i = 0; i < N; i++) {
    setbits(rnd(), rnd());
    var d = f64[0];
    if (d !== d || d === Infinity || d === -Infinity) continue;
    __PIN(String(d));
}
// radix sweep on assorted doubles (non-10 radixes exercise the non-fast path + EXP_DISABLED)
var vals = [0.5, 1/3, 1234.5678, 1e-7, 1e300, 1e-300, 255, 1e21, 0.1, Math.PI, 2/3, 7, 1e15, 1e16];
for (var j = 0; j < vals.length; j++) {
    for (var r = 2; r <= 36; r++) {
        __PIN(vals[j].toString(r));
    }
}
// toFixed table
for (var j2 = 0; j2 < vals.length; j2++) {
    for (var fd = 0; fd <= 30; fd += 3) {
        __PIN(vals[j2].toFixed(fd));
    }
}
// toPrecision table
for (var j3 = 0; j3 < vals.length; j3++) {
    for (var pr = 1; pr <= 21; pr += 2) {
        __PIN(vals[j3].toPrecision(pr));
    }
}
// toExponential
for (var j4 = 0; j4 < vals.length; j4++) {
    for (var fe = 0; fe <= 20; fe += 4) {
        __PIN(vals[j4].toExponential(fe));
    }
    __PIN(vals[j4].toExponential());
}

__A("transcript-record-count", function () { assert_eq(__PIN_count, 500642); });
__A("transcript-digest-fnv1a", function () { assert_diverge((__PIN_h1 >>> 0).toString(16), "da348ef", "3bcc8061", "fnv1a"); });
summary("dtoa_shortest");
