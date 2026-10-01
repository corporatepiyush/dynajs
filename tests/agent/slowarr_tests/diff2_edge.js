"use strict";
let fails = 0;
function ck(name, cond, detail) {
    if (cond) console.log(name, "=> OK");
    else { fails++; console.log(name, "=> FAIL", detail || ""); }
}

{
    let a = [];
    a[2147483647] = "tag";
    let r = a.shift();
    ck("tagMax shift", r === undefined && a.length === 2147483647 &&
       a[2147483646] === "tag" && (2147483646 in a) && !(2147483647 in a),
       "r=" + String(r) + " len=" + a.length + " v=" + a[2147483646]);
}

{
    let a = [];
    a[2147483646] = "tag";
    a.unshift("u");
    ck("tagMax unshift", a.length === 2147483648 && a[0] === "u" &&
       a[2147483647] === "tag" && (2147483647 in a) && !(2147483646 in a),
       "len=" + a.length + " v=" + a[2147483647]);
}

{
    let a = [1];
    a[2147483646] = "tag";
    a.reverse();
    ck("tagMax reverse", a.length === 2147483647 && a[0] === "tag" &&
       a[2147483646] === 1, "len=" + a.length);
}

{
    let a = [];
    a[1073741823] = "x";
    a[5] = "lo";
    let t0 = Date.now();
    a.unshift("u");
    a.shift();
    let ms = Date.now() - t0;
    ck("huge sparse pair", a[5] === "lo" && a[1073741823] === "x" &&
       a[0] === undefined && a.length === 1073741824 && ms < 2000,
       "lo=" + a[5] + " x=" + a[1073741823] + " ms=" + ms);
}

{
    let a = new Array(1000);
    let r = a.shift();
    ck("allHoles shift O(1)", r === undefined && a.length === 999);
    a.unshift("u");
    ck("allHoles unshift O(1)", a.length === 1000 && a[0] === "u" &&
       !(1 in a));
    a.reverse();
    ck("allHoles reverse O(1)", a.length === 1000 && !(0 in a) &&
       a[999] === "u" && (999 in a), "a[999]=" + a[999]);
}

console.log(fails === 0 ? "ALL EDGE OK" : "EDGE FAILURES: " + fails);
if (fails) throw new Error("edge failures");
