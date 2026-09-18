/* Where is the crossover between an inline compare and a call to memcmp?
 *
 * js_string_eq backs both `===` on strings and the key comparison inside Map
 * and Set. A profile of a string-keyed Map reaches memcmp through a dyld stub,
 * which for a 2-4 character key costs more than the comparison it performs.
 *
 * This sweeps key length so the threshold can be read off rather than guessed,
 * and keeps long keys in the same run as the control that must not move: an
 * inline path that helps at length 4 and hurts at length 64 is not a win, it
 * is a shifted cost.
 *
 * Equal-but-distinct strings are used throughout. Comparing a string to
 * itself hits the p1 == p2 pointer check and measures nothing.
 *
 * WIDE ROWS. Every string here used to be ASCII, so every row ran the
 * narrow/narrow memcmp arm and NOT ONE reached memcmp16 or memcmp16_8 -- the
 * file could not measure the wide compare at all, and a flat A/B over it read
 * as "no regression" when it was really "not covered".
 *
 * Which row reaches which function is NOT uniform, and assuming it is puts the
 * rows back in the same position of measuring nothing:
 *   memcmp16    <- `<` on wide/wide, via js_string_compare. The `lt` column.
 *   memcmp16_8  <- `===` and `<` on mixed width. The whole `mixed` section.
 *   neither     <- `===` on equal-length wide/wide: js_string_eq takes a raw
 *                  libc memcmp over the units. That arm is worth measuring in
 *                  its own right, but it is not the chunked compare.
 * Lengths straddle 8 because that is the chunk width: 7 stays scalar, 8 does not.
 *
 * CLOCK. Date.now() has a 1 ms tick; at REPS=2e6 that is 0.5 ns/op of printed
 * resolution on a table looking for a 1-2 ns crossover, i.e. half the signal.
 * performance.now() is get_time_ns()/1e6. Reps are scaled until the timed
 * region clears MIN_MS, and the empty loop is subtracted -- bench_prop_delete
 * records the same lesson: "the ratios were dominated by rounding".
 */
"use strict";

var sink = 0;
var MIN_MS = 150;

/* Milliseconds for ONE call of fn(reps), scaled so the region clears the
 * clock's noise floor by ~1000x, then taking the best of five. */
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

/* The driving loop itself costs several ns per iteration; without this the
 * short rows report the loop and call it a comparison. */
function empty(n) { var s = 0; for (var i = 0; i < n; i++) s += 1; return s; }

var REPS = 2000000;
var EMPTY_NS = 0;

function nsop(t) { var v = t * 1e6 / REPS - EMPTY_NS; return (v < 0 ? 0 : v).toFixed(2); }

function pad(base, len) {
    var s = base;
    while (s.length < len) s += "abcdefghijklmnopqrstuvwxyz0123456789";
    return s.slice(0, len);
}

/* A string containing a unit above U+00FF is stored wide, so these are the
 * only ones that route to memcmp16. */
function wpad(base, len) {
    var s = base;
    while (s.length < len) s += "一二三四五六七八";
    return s.slice(0, len);
}

/* two distinct string objects with identical contents */
function twin(len, tag) {
    var a = pad("key" + tag + "_", len);
    /* force a fresh allocation with the same contents */
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

/* ---- CONTROL: narrow/narrow. Untouched branch; must not move in any A/B. ---- */
print("");
print("#S narrow  len  eq_ns/op  neq_ns/op");
var lens = [1, 2, 4, 6, 8, 12, 16, 24, 32, 64, 128];
for (var li = 0; li < lens.length; li++) {
    var len = lens[li];
    var t = twin(len, li);
    var a = t[0], b = t[1];
    /* differs only in the last character: the compare must run to the end */
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

/* ---- wide/wide. Lengths straddle the 8-element chunk width. ----
 * WHICH FUNCTION EACH COLUMN MEASURES, because they are not the same one:
 *   eq/neqLast/neqFirst use `===`, which for equal-length wide/wide takes
 *     js_string_eq's raw libc memcmp arm -- it does NOT reach memcmp16.
 *   lt uses `<`, which goes js_string_compare -> js_string_memcmp -> memcmp16,
 *     and is therefore the ONLY column here that measures the chunked compare.
 * Getting this backwards is how a "wide" bench ends up not covering the wide
 * code: the rows exist, the function is never called. */
print("");
print("#S wide    len  eq_ns/op  neqLast_ns/op  neqFirst_ns/op  lt_ns/op");
var wlens = [1, 2, 4, 7, 8, 9, 12, 15, 16, 17, 24, 32, 64, 128];
for (var wi = 0; wi < wlens.length; wi++) {
    var L = wlens[wi];
    var wt = wtwin(L, wi);
    var wa = wt[0], wb = wt[1];
    /* last unit differs: the chunk loop must run the whole way */
    var wc = wa.slice(0, L - 1) + "鿿";
    /* first unit differs: the early exit, so 1 and 128 should be equal here */
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
    /* `<` is the only shape here that reaches memcmp16; wc differs in the LAST
       unit, so the chunk loop runs the whole way before the tail decides. */
    var wlt = ms(function (n) {
        var s = 0;
        for (var i = 0; i < n; i++) if (wa < wc) s++;
        return s;
    }, REPS);
    print("#S wide    " + L + "\t" + nsop(we) + "\t" + nsop(wn) + "\t" +
          nsop(wf) + "\t" + nsop(wlt));
}

/* ---- mixed width: memcmp16_8, the arm nothing else here reaches ----
 * One side holds a unit above U+00FF so it is wide; the other is pure ASCII of
 * the SAME length. They agree for L-1 units and differ at the last, so the
 * chunk loop runs to the end before the tail resolves it. Equality can only be
 * false here (a wide unit cannot equal a byte), which is fine: the cost being
 * measured is the scan, not the verdict. */
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

/* Map/Set with string keys, the other caller */
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

/* sort() drives js_string_compare rather than js_string_eq: a different
 * caller of the same two functions, and the only one that needs the SIGN. */
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
