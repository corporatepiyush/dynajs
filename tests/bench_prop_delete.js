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

function build_only(n) {
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
