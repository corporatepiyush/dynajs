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
 */
"use strict";

var sink = 0;

function ms(fn, reps) {
    fn(1000);
    var best = Infinity;
    for (var k = 0; k < 5; k++) {
        var t0 = Date.now();
        sink += fn(reps);
        var dt = Date.now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

function pad(base, len) {
    var s = base;
    while (s.length < len) s += "abcdefghijklmnopqrstuvwxyz0123456789";
    return s.slice(0, len);
}

/* two distinct string objects with identical contents */
function twin(len, tag) {
    var a = pad("key" + tag + "_", len);
    /* force a fresh allocation with the same contents */
    var b = (a + "#").slice(0, len);
    return [a, b];
}

var REPS = 2000000;
print("len\teq_ns/op\tneq_ns/op");
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
    print(len + "\t" + (teq * 1e6 / REPS).toFixed(2) + "\t\t" +
          (tneq * 1e6 / REPS).toFixed(2));
}

/* Map/Set with string keys, the other caller */
print("");
var keys = [], i;
for (i = 0; i < 1000; i++) keys.push("k" + i);
var longkeys = [];
for (i = 0; i < 1000; i++) longkeys.push(pad("longkey" + i + "_", 48));

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
print("map short keys (len 2-5)\t" + (ms(map_round(keys), 300) * 1e6 / (300 * 2000)).toFixed(2) + " ns/op");
print("map long keys  (len 48)  \t" + (ms(map_round(longkeys), 300) * 1e6 / (300 * 2000)).toFixed(2) + " ns/op");
if (sink === -1) print("unreachable");
