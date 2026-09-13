/* Does the per-shape property hash degrade, and on what input?
 *
 * Integer-index atoms are `index | (1<<31)` (src/value/atoms.inc.c:26), so a
 * bucket taken as `atom & prop_hash_mask` is just the low bits of the index.
 * Keys whose indices share a stride >= the table size therefore all land in
 * one bucket: the chain walks in find_own_property() and add_shape_property()
 * go O(k) and building the object goes O(k^2).
 *
 * This sweeps the stride rather than assuming one, because a mixing function
 * can happen to scatter one stride while leaving another degenerate -- testing
 * the single stride you designed against proves nothing. Every power-of-two
 * stride that keeps k*stride under JS_ATOM_MAX_INT is exercised, and the row
 * reports the worst. Controls: the same key count with consecutive indices and
 * with string keys, both of which spread under any of these functions.
 */
"use strict";

var sink = 0;
var ATOM_MAX_INT = 2147483647;

function ms(fn) {
    var best = Infinity, k;
    for (k = 0; k < 3; k++) {
        var t0 = Date.now();
        sink += fn();
        var dt = Date.now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

function build_stride(k, stride) {
    var o = {}, i;
    for (i = 0; i < k; i++) o[i * stride] = i;
    return o["0"] === 0 ? 1 : 2;
}

function get_stride(o, k, stride) {
    var i, s = 0;
    for (i = 0; i < k; i++) s += o[i * stride];
    return s;
}

function build_dense(k) {
    var o = {}, i;
    for (i = 0; i < k; i++) o[i] = i;
    return o["0"] === 0 ? 1 : 2;
}

function build_string(k, keys) {
    var o = {}, i;
    for (i = 0; i < k; i++) o[keys[i]] = i;
    return o[keys[0]] === 0 ? 1 : 2;
}

var K = 32000;
var keys = [];
for (var i = 0; i < K; i++) keys.push("key_" + i);

print("CONTROLS at k=" + K + ":  dense=" + ms(function () { return build_dense(K); }) +
      "ms  string=" + ms(function () { return build_string(K, keys); }) + "ms");
print("");
print("stride\tbuild(ms)\tget(ms)");

var worst_build = 0, worst_get = 0, worst_stride = 0;
for (var sh = 6; sh <= 16; sh++) {
    var stride = 1 << sh;
    if (K * stride > ATOM_MAX_INT) continue;
    var b = ms(function () { return build_stride(K, stride); });
    var o = {};
    for (var i = 0; i < K; i++) o[i * stride] = i;
    var g = ms(function () { return get_stride(o, K, stride); });
    print(stride + "\t" + b + "\t\t" + g);
    if (b > worst_build) { worst_build = b; worst_stride = stride; }
    if (g > worst_get) worst_get = g;
}
print("");
print("WORST build=" + worst_build + "ms get=" + worst_get +
      "ms at stride=" + worst_stride);

/* Which build is this? There is no runtime accessor for CONFIG_PROP_HASH_MIX,
   so infer it behaviourally: with mixing on, the worst stride costs about what
   the dense control costs; with mixing off it is orders of magnitude worse.
   A behavioural probe is the only kind that cannot pass against the wrong
   binary -- checking for a symbol would not, since both builds export the
   same ones. */
var dense_ms = ms(function () { return build_dense(K); });
var ratio = worst_build / Math.max(1, dense_ms);
print("CONFIG_PROP_HASH_MIX: " + (ratio > 10 ? "OFF" : "ON") +
      "  (worst stride is " + ratio.toFixed(0) + "x the dense control)");

/* Quadratic check on the worst stride found: doubling k must roughly double
   the time, not quadruple it. */
if (worst_stride) {
    print("");
    print("k\tbuild(ms) at stride " + worst_stride);
    var prev = 0;
    for (var k = 4000; k <= K && k * worst_stride <= ATOM_MAX_INT; k *= 2) {
        var kk = k;
        var t = ms(function () { return build_stride(kk, worst_stride); });
        print(kk + "\t" + t + (prev ? "\t\tx2 ratio=" + (prev ? (t / prev).toFixed(1) : "n/a") +
              " (linear~2.0, quadratic~4.0)" : ""));
        prev = t;
    }
}

/* correctness oracle: every key reads back its own value, deletions included */
function oracle() {
    var o = {}, h = 0, i;
    for (i = 0; i < 3000; i++) o[i * 65536] = i * 7 + 1;
    for (i = 0; i < 3000; i++) h = (h * 31 + o[i * 65536]) | 0;
    for (i = 0; i < 3000; i += 3) delete o[i * 65536];
    for (i = 0; i < 3000; i++)
        h = (h * 31 + (o[i * 65536] === undefined ? -1 : o[i * 65536])) | 0;
    var n = 0;
    for (var kk2 in o) n++;
    return h + "/" + n;
}
print("");
print("oracle\t" + oracle());
if (sink === -12345) print("unreachable");
