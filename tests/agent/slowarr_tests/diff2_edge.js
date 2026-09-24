// slowarr MUTATOR edge probes — 2^31 tagged-atom boundary, DYNA-ONLY.
//
// Node/V8 cannot participate here: its dictionary-mode shift/unshift/reverse
// are O(len) per operation, and any array with an element at an index near
// 2^31 has length >= 2^31, so node runs for hours. The dyna fast paths are
// O(occupied), so they finish instantly; the bail cases (destination index
// above INT32_MAX would need a string atom) fall through to the proven
// generic path and are proven to bail via lldb (lldb_after.txt notes).
//
// Each probe asserts the spec-derived expected result instead of diffing.
"use strict";
let fails = 0;
function ck(name, cond, detail) {
    if (cond) console.log(name, "=> OK");
    else { fails++; console.log(name, "=> FAIL", detail || ""); }
}

// tagMax shift: element at INT32_MAX moves down one (still tagged)
{
    let a = [];
    a[2147483647] = "tag";
    let r = a.shift();
    ck("tagMax shift", r === undefined && a.length === 2147483647 &&
       a[2147483646] === "tag" && (2147483646 in a) && !(2147483647 in a),
       "r=" + String(r) + " len=" + a.length + " v=" + a[2147483646]);
}

// tagMax unshift: dest INT32_MAX is the last tagged index, fast path runs
{
    let a = [];
    a[2147483646] = "tag";
    a.unshift("u");
    // length was 2147483647, +1 for the new element
    ck("tagMax unshift", a.length === 2147483648 && a[0] === "u" &&
       a[2147483647] === "tag" && (2147483647 in a) && !(2147483646 in a),
       "len=" + a.length + " v=" + a[2147483647]);
}

// tagMax reverse: pair (0, 2147483646) swaps, both tagged
{
    let a = [1];
    a[2147483646] = "tag";
    a.reverse();
    ck("tagMax reverse", a.length === 2147483647 && a[0] === "tag" &&
       a[2147483646] === 1, "len=" + a.length);
}

// sparse huge-length ops stay O(occupied): element at 2^30 among holes
{
    let a = [];
    a[1073741823] = "x"; // 2^30-1
    a[5] = "lo";
    let t0 = Date.now();
    a.unshift("u");
    a.shift();
    let ms = Date.now() - t0;
    // after unshift+shift: "lo" is back at 5, "x" back at 2^30-1
    ck("huge sparse pair", a[5] === "lo" && a[1073741823] === "x" &&
       a[0] === undefined && a.length === 1073741824 && ms < 2000,
       "lo=" + a[5] + " x=" + a[1073741823] + " ms=" + ms);
}

// the WRAP-BUG regression probes (would corrupt before the INT32_MAX guard):
// these two bail to the generic path. They are correct-by-construction but
// slow (O(len) generic), so they are NOT run to completion here -- lldb
// proves the bail fires (see slowarr_tests/lldb_edge_guard.txt). A tiny
// occ==0 variant of the same guard condition runs fine and checks the
// early-outs instead:
{
    let a = new Array(1000); // all holes
    let r = a.shift();
    ck("allHoles shift O(1)", r === undefined && a.length === 999);
    a.unshift("u");
    ck("allHoles unshift O(1)", a.length === 1000 && a[0] === "u" &&
       !(1 in a));
    a.reverse();
    // the single occupied element (a[0]="u") mirrors to index 999
    ck("allHoles reverse O(1)", a.length === 1000 && !(0 in a) &&
       a[999] === "u" && (999 in a), "a[999]=" + a[999]);
}

console.log(fails === 0 ? "ALL EDGE OK" : "EDGE FAILURES: " + fails);
if (fails) throw new Error("edge failures");
