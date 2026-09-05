/* Does deleting properties one at a time thrash compact_properties()?
 *
 * delete_property() compacts when deleted_prop_count >= 8 and >= prop_count/2
 * (src/object/property_get.inc.c). The claim under test is that once the
 * threshold is crossed, a delete loop re-compacts on roughly every other
 * deletion, making the loop quadratic.
 *
 * Measure the curve instead of arguing about it: compaction rewrites the whole
 * shape, so if it fires a constant fraction of the time the loop is O(n^2) and
 * doubling n quadruples the time. If instead each compaction resets prop_count
 * to the surviving count -- halving the next trigger point -- the total work is
 * geometric and the loop is O(n).
 */
"use strict";

var sink = 0;

function make(n) {
    var o = {}, i;
    for (i = 0; i < n; i++) o["k" + i] = i;
    return o;
}

function delete_all(n) {
    var o = make(n), i;
    for (i = 0; i < n; i++) delete o["k" + i];
    var c = 0;
    for (var k in o) c++;
    return c;
}

function delete_half(n) {
    var o = make(n), i;
    for (i = 0; i < n; i += 2) delete o["k" + i];
    var c = 0;
    for (var k in o) c++;
    return c;
}

function build_only(n) {          /* control: the build cost alone */
    var o = make(n);
    return o["k0"];
}

function ms(fn, n) {
    var best = Infinity, k;
    for (k = 0; k < 5; k++) {
        var t0 = Date.now();
        sink += fn(n);
        var dt = Date.now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

print("n\tdelete_all\tdelete_half\tCTL_build");
/* large enough that the whole-millisecond clock is not the resolution limit:
   at 32000 the rows were 5 ms and the ratios were dominated by rounding */
var sizes = [50000, 100000, 200000, 400000];
var prev = null;
for (var i = 0; i < sizes.length; i++) {
    var n = sizes[i];
    var a = ms(delete_all, n), h = ms(delete_half, n), b = ms(build_only, n);
    print(n + "\t" + a + "\t\t" + h + "\t\t" + b);
    if (prev)
        print("  x2 ratio:\tdelete_all=" +
              (prev.a ? (a / prev.a).toFixed(1) : "n/a") +
              "\tdelete_half=" + (prev.h ? (h / prev.h).toFixed(1) : "n/a") +
              "\tCTL_build=" + (prev.b ? (b / prev.b).toFixed(1) : "n/a") +
              "\t(linear~2.0, quadratic~4.0)");
    prev = { a: a, h: h, b: b };
}

/* oracle: deletion must leave exactly the untouched keys readable, and the
   surviving values must be their own. A broken compaction shows up here. */
function oracle() {
    var o = make(500), i, hh = 0;
    for (i = 0; i < 500; i += 2) delete o["k" + i];
    for (i = 0; i < 500; i++) {
        var v = o["k" + i];
        hh = (hh * 31 + (v === undefined ? -1 : v)) | 0;
    }
    var n = 0;
    for (var k in o) n++;
    return hh + "/" + n;
}
print("oracle\t" + oracle());
if (sink === -1) print("unreachable");
