/* How much of BigInt arithmetic is the allocator?
 *
 * The claim under test: every js_bigint_add/mul allocates a fresh JSBigInt, so
 * a chain like a+b+c+d allocates three temporaries per iteration and malloc
 * dominates.
 *
 * That is only true above one limb. js_binary_arith_slow and js_add_slow both
 * open with a JS_TAG_SHORT_BIG_INT path that does the arithmetic in registers,
 * and js_bigint_set_short() borrows a stack JSBigIntBuf, so a value that fits
 * a limb never reaches js_bigint_new(). The interesting number is therefore
 * the STEP across the limb boundary -- the first width at which an allocation
 * appears -- not the absolute cost at any one width.
 *
 * Operand width is held constant inside each loop. An accumulator that grows
 * (sum += a*a) walks up through widths as it runs and measures a mixture, not
 * the width in the label; x ^= 1n keeps the width fixed while stopping the
 * operands from being loop-invariant.
 */
"use strict";

var sink = 0;

function ms(fn, reps) {
    fn(1000);
    var best = Infinity;
    for (var k = 0; k < 5; k++) {
        var t0 = Date.now();
        sink += Number(fn(reps) & 1n);
        var dt = Date.now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

/* a + b at fixed width */
function mk_add(bits) {
    var a = (1n << BigInt(bits - 1)) | 1n;
    var b = (1n << BigInt(bits - 1)) | 3n;
    return function (n) {
        var acc = 0n, x = a, i;
        for (i = 0; i < n; i++) { x = x ^ 1n; acc = x + b; }
        return acc;
    };
}

/* a * b at fixed width (result is 2x wide, as a product must be) */
function mk_mul(bits) {
    var a = (1n << BigInt(bits - 1)) | 1n;
    var b = (1n << BigInt(bits - 1)) | 3n;
    return function (n) {
        var acc = 0n, x = a, i;
        for (i = 0; i < n; i++) { x = x ^ 1n; acc = x * b; }
        return acc;
    };
}

var REPS = 500000;
print("A BigInt is short (no allocation) while it fits one signed limb: 63 bits.");
print("");
print("bits\tadd ns/op\tmul ns/op\tnote");
var widths = [8, 16, 32, 48, 62, 63, 64, 65, 96, 128, 192, 256, 512, 1024];
var prev_add = 0, prev_mul = 0, step_add = 0, step_mul = 0;
for (var w = 0; w < widths.length; w++) {
    var bits = widths[w];
    var ta = ms(mk_add(bits), REPS) * 1e6 / REPS;
    var tm = ms(mk_mul(bits), REPS) * 1e6 / REPS;
    var note = "";
    if (bits === 64) {
        note = "<-- first heap width";
        step_add = ta - prev_add;
        step_mul = tm - prev_mul;
    }
    print(bits + "\t" + ta.toFixed(1) + "\t\t" + tm.toFixed(1) + "\t\t" + note);
    if (bits === 63) { prev_add = ta; prev_mul = tm; }
}

print("");
print("STEP across the limb boundary (63 -> 64 bits), i.e. the cost of the");
print("allocation appearing:  add " + step_add.toFixed(1) +
      " ns   mul " + step_mul.toFixed(1) + " ns");

/* Control: the same loop on Numbers, which allocate nothing and take no
   BigInt path at all. Anchors what the loop itself costs. */
function num_loop(n) {
    var acc = 0, x = 1024, i;
    for (i = 0; i < n; i++) { x = x ^ 1; acc = x + 3; }
    return BigInt(acc | 0);
}
print("CTL Number add\t" + (ms(num_loop, REPS) * 1e6 / REPS).toFixed(1) + " ns/op");
if (sink === -1) print("unreachable");
