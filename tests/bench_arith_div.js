/* Isolated harness for the float64 division/modulo paths.
 *
 * OP_add/OP_sub/OP_mul decode a float operand inline; OP_div/OP_mod did not,
 * so every double division left the dispatch loop for js_binary_arith_slow.
 * The int rows are the CONTROL: they never touch the changed code and must not
 * move. Results go to a volatile-ish sink (a global read after timing) so the
 * timed region holds nothing but the operation.
 */
"use strict";

var sink = 0;

function bench(name, fn, n) {
    /* warm up so the shape/branch state is the same for every row */
    fn(1000);
    var best = Infinity;
    for (var k = 0; k < 5; k++) {
        var t0 = Date.now();
        var r = fn(n);
        var dt = Date.now() - t0;
        sink += r;
        if (dt < best) best = dt;
    }
    /* subtract the empty-loop cost measured at the same trip count */
    var e0 = Date.now();
    sink += empty(n);
    var edt = Date.now() - e0;
    for (var k = 0; k < 4; k++) {
        var e1 = Date.now();
        sink += empty(n);
        var e2 = Date.now() - e1;
        if (e2 < edt) edt = e2;
    }
    var net = best - edt;
    if (net < 0) net = 0;
    print(name + "\t" + n + "\t" + best + "ms\tempty=" + edt +
          "ms\tnet=" + net + "ms\t" + (net * 1e6 / n).toFixed(2) + "ns/op");
}

function empty(n) {
    var i, a = 0.0;
    for (i = 0; i < n; i++) a += 1.0;
    return a;
}

/* --- the rows under test --- */

function div_ff(n) {           /* double / double  -- was the slow path */
    var i, a = 1e6 + 0.5, s = 0.0;
    for (i = 0; i < n; i++) s += a / 3.25;
    return s;
}

function div_fi(n) {           /* double / int -- was the slow path */
    var i, a = 1e6 + 0.5, s = 0.0;
    for (i = 0; i < n; i++) s += a / 7;
    return s;
}

function div_if(n) {           /* int / double -- was the slow path */
    var i, b = 3.25, s = 0.0;
    for (i = 0; i < n; i++) s += 1000003 / b;
    return s;
}

function mod_ff(n) {           /* double % double -- was the slow path */
    var i, a = 1e6 + 0.5, s = 0.0;
    for (i = 0; i < n; i++) s += a % 3.25;
    return s;
}

function mod_fi(n) {           /* double % int -- was the slow path */
    var i, a = 1e6 + 0.5, s = 0.0;
    for (i = 0; i < n; i++) s += a % 7;
    return s;
}

/* --- controls: must not move --- */

function div_ii(n) {           /* int / int -- already inline */
    var i, s = 0.0;
    for (i = 0; i < n; i++) s += 1000003 / 7;
    return s;
}

function mul_ff(n) {           /* double * double -- already inline */
    var i, a = 1.0000001, s = 0.0;
    for (i = 0; i < n; i++) s += a * 3.25;
    return s;
}

function add_ff(n) {           /* double + double -- already inline */
    var i, a = 1.0000001, s = 0.0;
    for (i = 0; i < n; i++) s += a + 3.25;
    return s;
}

var N = 20000000;
print("name\treps\tbest\tempty\tnet\tper-op");
bench("div_ff", div_ff, N);
bench("div_fi", div_fi, N);
bench("div_if", div_if, N);
bench("mod_ff", mod_ff, N);
bench("mod_fi", mod_fi, N);
bench("CTL_div_ii", div_ii, N);
bench("CTL_mul_ff", mul_ff, N);
bench("CTL_add_ff", add_ff, N);

/* value oracle: run in a separate pass so the timing loops cannot perturb it.
   A wrong-value change (float vs int result, -0, NaN, fmod sign) moves this. */
function oracle() {
    var h = 0, vals = [], i;
    var probes = [
        [7.5, 2.5], [7.5, 2], [7, 2.5], [-7.5, 2.5], [7.5, -2.5],
        [-7.5, -2.5], [0.0, 3.0], [-0.0, 3.0], [3.0, 0.0], [-3.0, 0.0],
        [0.0, 0.0], [1.0, Infinity], [Infinity, 1.0], [NaN, 1.0], [1.0, NaN],
        [1e308, 1e-308], [5e-324, 2.0], [1, 3], [-1, 3], [1, -3],
        [2147483647, -1], [-2147483648, -1], [-2147483648, 1], [0, -1],
    ];
    for (i = 0; i < probes.length; i++) {
        var a = probes[i][0], b = probes[i][1];
        var q = a / b, m = a % b;
        /* Object.is distinguishes -0 from 0 and NaN from NaN */
        vals.push(String(q) + (Object.is(q, -0) ? "(-0)" : "") + "|" +
                  String(m) + (Object.is(m, -0) ? "(-0)" : ""));
    }
    for (i = 0; i < vals.length; i++) {
        for (var j = 0; j < vals[i].length; j++)
            h = (h * 31 + vals[i].charCodeAt(j)) | 0;   /* order-dependent */
    }
    return h;
}
print("oracle\t" + oracle());
if (sink === 12345.6789) print("unreachable");
