/* The string hash must agree between a flat string and a rope with the same
 * contents, at every split offset.
 *
 * hash_string_rope() chains h across rope segments, so folding four characters
 * per step is only legal because the fold is algebraically identical to four
 * serial steps -- a split at an offset that is not a multiple of four leaves
 * the tail loop to finish the group. If it were merely "a good hash" instead of
 * the same recurrence, a rope and its flattened form would land in different
 * buckets and lookups would miss.
 *
 * A miss here is silent: the entry is simply not found. So this asserts
 * round-trips rather than printing a hash, and covers Map, Set and ordinary
 * property keys, which reach the hash through three different callers.
 */
"use strict";

var fails = 0;
function check(cond, what) {
    if (!cond) { fails++; print("FAIL: " + what); }
}

/* Build the same text two ways: as one literal, and as a rope split at every
   offset. `a + b` produces a rope; the engine flattens it lazily. */
function ropeSplit(s, at) {
    return s.slice(0, at) + s.slice(at);
}

var alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
function text(len, seed) {
    var s = "";
    for (var i = 0; i < len; i++) s += alphabet[(i * 7 + seed * 13) % alphabet.length];
    return s;
}

/* 1. rope vs flat at every split offset, over lengths that straddle the
      4-character group boundary in both the string and the split */
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

/* 2. REAL ropes. A concatenation only becomes a rope node when the left
      operand exceeds JS_STRING_ROPE_SHORT2_LEN (8192) -- below that the engine
      flattens eagerly, so short strings never reach hash_string_rope() and a
      test built from them proves nothing about chaining. That is exactly how
      this test first passed against a deliberately broken fold constant.

      The split offset must not be a multiple of 4: at a multiple the group
      boundary coincides with the segment boundary and even a wrong fold
      agrees. Lengths 8193..8196 cover every residue. */
for (var extra = 1; extra <= 4; extra++) {
    var baseLen = 8192 + extra;
    var base = "";
    while (base.length < baseLen) base += alphabet;
    base = base.slice(0, baseLen);          /* slice returns a flat string */
    for (var tl = 1; tl <= 9; tl++) {
        var tail = text(tl, tl);
        /* forcing a linearization gives the flat reference... */
        var flatRef = (base + tail).slice(0);
        /* ...while this stays an unlinearized rope, hashed by chaining */
        var rope = base + tail;
        check(rope === flatRef, "rope === flat base=" + baseLen + " tail=" + tl);

        var mr = new Map();
        mr.set(flatRef, "flat");
        check(mr.get(rope) === "flat",
              "Map.get(rope) base=" + baseLen + " tail=" + tl);

        var mr2 = new Map();
        mr2.set(base + tail, "rope");       /* insert as a rope... */
        check(mr2.get(flatRef) === "rope",  /* ...look up flat */
              "Map.get(flat) after rope insert base=" + baseLen + " tail=" + tl);

        var sr = new Set();
        sr.add(base + tail);
        check(sr.has(flatRef), "Set rope/flat base=" + baseLen + " tail=" + tl);

        var or = {};
        or[base + tail] = 1;
        check(or[flatRef] === 1, "obj rope/flat base=" + baseLen + " tail=" + tl);
    }
}

/* 3. wide (16-bit) strings take the other hash loop */
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

/* 4. mixed narrow+wide concatenation, which promotes the result to wide */
for (var len4 = 1; len4 <= 20; len4++) {
    var narrow = text(len4, 5);
    var wide = String.fromCharCode(0x2603) + narrow;
    var m4 = new Map();
    m4.set(wide, len4);
    check(m4.get(String.fromCharCode(0x2603) + narrow) === len4,
          "mixed narrow+wide len=" + len4);
}

/* 5. bulk round-trip: every key must be found, and the count must match, which
      catches a hash that collides two distinct strings into one record */
var big = new Map(), N = 20000;
for (var i5 = 0; i5 < N; i5++) big.set(text(1 + (i5 % 60), i5) + i5, i5);
check(big.size === N, "Map keeps " + N + " distinct keys, got " + big.size);
var found = 0;
for (var i6 = 0; i6 < N; i6++) if (big.get(text(1 + (i6 % 60), i6) + i6) === i6) found++;
check(found === N, "all " + N + " keys read back, got " + found);

if (fails === 0) print("test_string_hash: all tests passed");
else { print("test_string_hash: " + fails + " FAILURES"); throw Error("string hash mismatch"); }
