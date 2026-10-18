"use strict";

var sink = 0;
var MIN_MS = 150;

function ms(fn, reps) {
    fn(1000);
    var mult = 1, dt, m, k;
    for (;;) {
        var w0 = performance.now();
        for (m = 0; m < mult; m++) sink += fn(reps);
        dt = performance.now() - w0;
        if (dt >= MIN_MS || mult >= 4096) break;
        mult = Math.max(mult * 2, Math.ceil(mult * MIN_MS / Math.max(dt, 0.001)));
    }
    var best = Infinity;
    for (k = 0; k < 5; k++) {
        var t0 = performance.now();
        for (m = 0; m < mult; m++) sink += fn(reps);
        var d = performance.now() - t0;
        if (d < best) best = d;
    }
    return best / mult;
}

function empty(n) { var s = 0; for (var i = 0; i < n; i++) s += 1; return s; }

var REPS = 2000000;
var EMPTY_NS = 0;

function nsop(t) { var v = t * 1e6 / REPS - EMPTY_NS; return (v < 0 ? 0 : v).toFixed(2); }

function pad(base, len) {
    var s = base;
    while (s.length < len) s += "abcdefghijklmnopqrstuvwxyz0123456789";
    return s.slice(0, len);
}

function wpad(base, len) {
    var s = base;
    while (s.length < len) s += "一二三四五六七八";
    return s.slice(0, len);
}

function twin(len, tag) {
    var a = pad("key" + tag + "_", len);
    var b = (a + "#").slice(0, len);
    return [a, b];
}
function wtwin(len, tag) {
    var a = wpad("中" + tag + "_", len);
    var b = (a + "。").slice(0, len);
    return [a, b];
}

EMPTY_NS = ms(empty, REPS) * 1e6 / REPS;
print("empty-loop calibration: " + EMPTY_NS.toFixed(2) + " ns/iter (subtracted below)");

print("");
print("#S narrow  len  eq_ns/op  neq_ns/op");
var lens = [1, 2, 4, 6, 8, 12, 16, 24, 32, 64, 128];
for (var li = 0; li < lens.length; li++) {
    var len = lens[li];
    var t = twin(len, li);
    var a = t[0], b = t[1];
    var c = a.slice(0, len - 1) + (a.charAt(len - 1) === "z" ? "y" : "z");

    var teq = ms(function (n) {
        var s = 0;
        for (var i = 0; i < n; i++) if (a === b) s++;
        return s;
    }, REPS);
    var tneq = ms(function (n) {
        var s = 0;
        for (var i = 0; i < n; i++) if (a === c) s++;
        return s;
    }, REPS);
    print("#S narrow  " + len + "\t" + nsop(teq) + "\t" + nsop(tneq));
}

print("");
print("#S wide    len  eq_ns/op  neqLast_ns/op  neqFirst_ns/op  lt_ns/op");
var wlens = [1, 2, 4, 7, 8, 9, 12, 15, 16, 17, 24, 32, 64, 128];
for (var wi = 0; wi < wlens.length; wi++) {
    var L = wlens[wi];
    var wt = wtwin(L, wi);
    var wa = wt[0], wb = wt[1];
    var wc = wa.slice(0, L - 1) + "鿿";
    var wd = "鿿" + wa.slice(1);

    var we = ms(function (n) {
        var s = 0;
        for (var i = 0; i < n; i++) if (wa === wb) s++;
        return s;
    }, REPS);
    var wn = ms(function (n) {
        var s = 0;
        for (var i = 0; i < n; i++) if (wa === wc) s++;
        return s;
    }, REPS);
    var wf = ms(function (n) {
        var s = 0;
        for (var i = 0; i < n; i++) if (wa === wd) s++;
        return s;
    }, REPS);
    var wlt = ms(function (n) {
        var s = 0;
        for (var i = 0; i < n; i++) if (wa < wc) s++;
        return s;
    }, REPS);
    print("#S wide    " + L + "\t" + nsop(we) + "\t" + nsop(wn) + "\t" +
          nsop(wf) + "\t" + nsop(wlt));
}

print("");
print("#S mixed   len  neq_ns/op  lt_ns/op");
var mlens = [8, 9, 16, 17, 32, 64, 128];
for (var mi = 0; mi < mlens.length; mi++) {
    var ML = mlens[mi];
    var narrow = pad("m" + mi + "_", ML - 1) + "b";
    var wide = pad("m" + mi + "_", ML - 1) + "一";

    var mn = ms(function (n) {
        var s = 0;
        for (var i = 0; i < n; i++) if (wide === narrow) s++;
        return s;
    }, REPS);
    var mlt = ms(function (n) {
        var s = 0;
        for (var i = 0; i < n; i++) if (wide < narrow) s++;
        return s;
    }, REPS);
    print("#S mixed   " + ML + "\t" + nsop(mn) + "\t" + nsop(mlt));
}

print("");
var keys = [], i;
for (i = 0; i < 1000; i++) keys.push("k" + i);
var longkeys = [];
for (i = 0; i < 1000; i++) longkeys.push(pad("longkey" + i + "_", 48));
var wkeys = [];
for (i = 0; i < 1000; i++) wkeys.push(wpad("鍵" + i + "_", 12));
var wlongkeys = [];
for (i = 0; i < 1000; i++) wlongkeys.push(wpad("鍵長" + i + "_", 48));

function map_round(ks) {
    return function (n) {
        var s = 0;
        for (var j = 0; j < n; j++) {
            var m = new Map();
            for (var i = 0; i < 1000; i++) m.set(ks[i], i);
            for (i = 0; i < 1000; i++) if (m.has(ks[i])) s++;
        }
        return s;
    };
}
function mapns(ks) { return (ms(map_round(ks), 300) * 1e6 / (300 * 2000)).toFixed(2); }
print("#S map narrow-short  (len 2-5)\t" + mapns(keys) + " ns/op");
print("#S map narrow-long   (len 48)\t" + mapns(longkeys) + " ns/op");
print("#S map wide-short    (len 12)\t" + mapns(wkeys) + " ns/op");
print("#S map wide-long     (len 48)\t" + mapns(wlongkeys) + " ns/op");

var wsortsrc = wkeys.slice();
var nsortsrc = longkeys.slice();
function sort_round(src) {
    return function (n) {
        var s = 0;
        for (var j = 0; j < n; j++) { var a = src.slice(); a.sort(); s += a.length; }
        return s;
    };
}
print("#S sort narrow 1000  (len 48)\t" + ms(sort_round(nsortsrc), 50).toFixed(3) + " ms/50");
print("#S sort wide   1000  (len 12)\t" + ms(sort_round(wsortsrc), 50).toFixed(3) + " ms/50");

if (sink === -1) print("unreachable");
