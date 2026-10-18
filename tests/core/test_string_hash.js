"use strict";

var fails = 0;
function check(cond, what) {
    if (!cond) { fails++; print("FAIL: " + what); }
}

function ropeSplit(s, at) {
    return s.slice(0, at) + s.slice(at);
}

var alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
function text(len, seed) {
    var s = "";
    for (var i = 0; i < len; i++) s += alphabet[(i * 7 + seed * 13) % alphabet.length];
    return s;
}

for (var len = 0; len <= 40; len++) {
    var flat = text(len, len);
    var m = new Map();
    var st = new Set();
    var obj = {};
    m.set(flat, len);
    st.add(flat);
    obj[flat] = len;
    for (var at = 0; at <= len; at++) {
        var r = ropeSplit(flat, at);
        check(r === flat, "rope === flat len=" + len + " at=" + at);
        check(m.get(r) === len, "Map.get(rope) len=" + len + " at=" + at);
        check(m.has(r), "Map.has(rope) len=" + len + " at=" + at);
        check(st.has(r), "Set.has(rope) len=" + len + " at=" + at);
        check(obj[r] === len, "obj[rope] len=" + len + " at=" + at);
    }
}

for (var extra = 1; extra <= 4; extra++) {
    var baseLen = 8192 + extra;
    var base = "";
    while (base.length < baseLen) base += alphabet;
    base = base.slice(0, baseLen);
    for (var tl = 1; tl <= 9; tl++) {
        var tail = text(tl, tl);
        var flatRef = (base + tail).slice(0);
        var rope = base + tail;
        check(rope === flatRef, "rope === flat base=" + baseLen + " tail=" + tl);

        var mr = new Map();
        mr.set(flatRef, "flat");
        check(mr.get(rope) === "flat",
              "Map.get(rope) base=" + baseLen + " tail=" + tl);

        var mr2 = new Map();
        mr2.set(base + tail, "rope");
        check(mr2.get(flatRef) === "rope",
              "Map.get(flat) after rope insert base=" + baseLen + " tail=" + tl);

        var sr = new Set();
        sr.add(base + tail);
        check(sr.has(flatRef), "Set rope/flat base=" + baseLen + " tail=" + tl);

        var or = {};
        or[base + tail] = 1;
        check(or[flatRef] === 1, "obj rope/flat base=" + baseLen + " tail=" + tl);
    }
}

for (var len3 = 0; len3 <= 24; len3++) {
    var w = "";
    for (var i3 = 0; i3 < len3; i3++) w += String.fromCharCode(0x100 + ((i3 * 37) % 0x2000));
    var m3 = new Map(), s3 = new Set(), o3 = {};
    m3.set(w, len3); s3.add(w); o3[w] = len3;
    for (var at3 = 0; at3 <= len3; at3++) {
        var r3 = ropeSplit(w, at3);
        check(m3.get(r3) === len3, "wide Map.get len=" + len3 + " at=" + at3);
        check(s3.has(r3), "wide Set.has len=" + len3 + " at=" + at3);
        check(o3[r3] === len3, "wide obj[] len=" + len3 + " at=" + at3);
    }
}

for (var len4 = 1; len4 <= 20; len4++) {
    var narrow = text(len4, 5);
    var wide = String.fromCharCode(0x2603) + narrow;
    var m4 = new Map();
    m4.set(wide, len4);
    check(m4.get(String.fromCharCode(0x2603) + narrow) === len4,
          "mixed narrow+wide len=" + len4);
}

var big = new Map(), N = 20000;
for (var i5 = 0; i5 < N; i5++) big.set(text(1 + (i5 % 60), i5) + i5, i5);
check(big.size === N, "Map keeps " + N + " distinct keys, got " + big.size);
var found = 0;
for (var i6 = 0; i6 < N; i6++) if (big.get(text(1 + (i6 % 60), i6) + i6) === i6) found++;
check(found === N, "all " + N + " keys read back, got " + found);

if (fails === 0) print("test_string_hash: all tests passed");
else { print("test_string_hash: " + fails + " FAILURES"); throw Error("string hash mismatch"); }
