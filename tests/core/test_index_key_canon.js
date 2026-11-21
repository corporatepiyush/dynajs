// covers: c:src/object/property_get.inc.c
// obj[k] with a number key: an integral double key in int32 range (1.0, 4/2,
// -0) is turned into the same int key the interpreter would have had, before
// JS_GetPropertyValue / JS_SetPropertyValue / JS_ValueToAtom pick a path.
// Before that, such keys went number -> string (dtoa) -> atom hash on every
// access; seismic-detection spent 17% of its time there (loop counters that
// start from a division). The canonical form must give exactly what
// ToPropertyKey gives: 1.0 -> "1", -0 -> "0", 0.5 -> "0.5", 1e10 -> "10000000000".
// The range guard (|d| <= INT32_MAX) keeps the double -> int32 cast defined
// in C; it is not observable from JS because an out-of-range double never
// equals its saturated cast.
//
// Second part: an int key that misses the array/typed-array fast cases (plain
// object, out-of-bounds array index, Proxy, getter on the prototype) goes
// straight to JS_GetPropertyInternal / JS_SetPropertyInternal with the int atom
// instead of the generic ValueToAtom path. The receiver passed must still be
// the original object (getter/setter `this`), and exotic objects must still
// see the canonical string key.

import("../lib/kit.js").then(async ({ sections: __kitSections }) => {
const __kitS = __kitSections("test_index_key_canon", { chain: true });
let failures = 0;
function eq(actual, expected, what) {
    if (actual !== expected) {
        failures++;
        console.log("FAIL", what, "got", JSON.stringify(actual), "expected", JSON.stringify(expected));
    }
}

const two = 4 / 2;
const one = 3 / 3;
const half = 1 / 2;
const negz = -0;

{
    const a = [10, 20, 30];
__kitS.section("array read with integral double", () => {
    eq(a[one], 20, "array read with integral double");
    eq(a[two], 30, "array read with 4/2");
    eq(a[negz], 10, "array read with -0");
    eq(a[half], undefined, "array read with 0.5 is not index 0");
    eq(a[1e10], undefined, "array read with 1e10");
    eq(a[NaN], undefined, "array read with NaN");
});
__kitS.section("0.5 is a named property, not an element", () => {
    a[half] = "h";
    eq(a.length, 3, "0.5 is a named property, not an element");
    eq(a[0], 10, "0.5 write leaves index 0 alone");
    eq(a["0.5"], "h", "0.5 write lands on key '0.5'");
});
__kitS.section("append with integral double key", () => {
    a[3 / 1] = 40;
    eq(a.length, 4, "append with integral double key");
    eq(one in a, true, "in with integral double");
    eq(half in a, true, "in with 0.5 after the 0.5 write");
    eq(2.5 in a, false, "in with 2.5");
});
}

{
let ta;
__kitS.section("typed array read with integral double", () => {
    ta = new Float64Array([1.5, 2.5]);
    eq(ta[one], 2.5, "typed array read with integral double");
    eq(ta[negz], 1.5, "typed array read with -0");
    eq(ta[half], undefined, "typed array read with 0.5");
});
__kitS.section("typed array 0.5 write is ignored", () => {
    ta[half] = 9;
    eq(ta[0], 1.5, "typed array 0.5 write is ignored");
});
__kitS.section("typed array write with integral double", () => {
    ta[one] = 7;
    eq(ta[1], 7, "typed array write with integral double");
});
}

{
    const o = { 0: "z", 1: "o", "0.5": "half" };
__kitS.section("plain object read with integral double", () => {
    eq(o[one], "o", "plain object read with integral double");
    eq(o[negz], "z", "plain object read with -0");
    eq(o[half], "half", "plain object read with 0.5");
});
__kitS.section("plain object keys after double-key write", () => {
    o[two] = "t";
    eq(Object.keys(o).join(), "0,1,2,0.5", "plain object keys after double-key write");
    eq(o[2], "t", "plain object int read after double write");
});
__kitS.section("delete with integral double", () => {
    delete o[one];
    eq(1 in o, false, "delete with integral double");
});
}

{
let p;
    const seen = [];
__kitS.section("proxy get trap key for 1.0", () => {
    p = new Proxy({}, {
        get(t, k) { seen.push("get:" + k); return k; },
        set(t, k, v) { seen.push("set:" + k); return true; },
    });
    eq(p[one], "1", "proxy get trap key for 1.0");
});
__kitS.section("proxy get trap key for -0", () => {
    eq(p[negz], "0", "proxy get trap key for -0");
    eq(p[5], "5", "proxy get trap key for int 5");
    eq(p[half], "0.5", "proxy get trap key for 0.5");
});
__kitS.section("proxy trap keys", () => {
    p[two] = 0;
    p[6] = 0;
    eq(seen.join(), "get:1,get:0,get:5,get:0.5,set:2,set:6", "proxy trap keys");
});
}

{
    const recv = [];
    const arr = [1, 2];
    const obj = { a: 1 };
__kitS.section("out-of-bounds array read reaches prototype getter", () => {
    Object.defineProperty(Object.prototype, 77, {
        get() { recv.push(this); return "g"; },
        set(v) { recv.push(this); },
        configurable: true,
    });
    eq(arr[77], "g", "out-of-bounds array read reaches prototype getter");
});
__kitS.section("plain object int read reaches prototype getter", () => {
    eq(obj[77], "g", "plain object int read reaches prototype getter");
    eq(arr[77 / 1], "g", "double key reaches prototype getter");
});
__kitS.section("getter and setter calls", () => {
    arr[77] = 1;
    obj[77] = 1;
    eq(recv.length, 5, "getter and setter calls");
    eq(recv[0] === arr && recv[1] === obj && recv[2] === arr && recv[3] === arr && recv[4] === obj, true, "receiver is the original object");
    eq(Object.prototype.hasOwnProperty.call(arr, 77), false, "setter on prototype intercepts the write");
    delete Object.prototype[77];
});
}

{
let a, frozen, fo;
    let s = 0;
__kitS.section("loop counter that starts from a division reads every element", () => {
    a = new Array(64).fill(1);
    for (let i = 64 / 64 - 1; i < 64; i++)
        s += a[i];
    eq(s, 64, "loop counter that starts from a division reads every element");
});
    let threw = false;
__kitS.section("strict write to frozen array with double key throws", () => {
    frozen = Object.freeze([1, 2, 3]);
    try {
        (function () { "use strict"; frozen[one] = 9; })();
    } catch (e) {
        threw = e instanceof TypeError;
    }
    eq(threw, true, "strict write to frozen array with double key throws");
});
__kitS.section("frozen array unchanged", () => {
    eq(frozen[1], 2, "frozen array unchanged");
});
__kitS.section("strict int-key write to frozen plain object throws", () => {
    fo = Object.freeze({ 1: "x" });
    threw = false;
    try {
        (function () { "use strict"; fo[1] = 9; })();
    } catch (e) {
        threw = e instanceof TypeError;
    }
    eq(threw, true, "strict int-key write to frozen plain object throws");
});
__kitS.section("strict double-key add to frozen plain object throws", () => {
    threw = false;
    try {
        (function () { "use strict"; fo[two] = 9; })();
    } catch (e) {
        threw = e instanceof TypeError;
    }
    eq(threw, true, "strict double-key add to frozen plain object throws");
});
__kitS.section("sloppy write to frozen plain object is ignored", () => {
    fo[1] = 9;
    eq(fo[1], "x", "sloppy write to frozen plain object is ignored");
});
}

// Third part: get_array_el tests class_id == JS_CLASS_ARRAY before the class
// switch (one compare instead of a jump-table branch on every plain-array
// read). The plain-array case must keep the bounds check (an index == count
// still reaches the prototype chain) and must stay limited to plain arrays:
// mapped arguments keep var_refs, not values, and typed arrays/arguments keep
// their own switch cases.
{
let i16;
    const hits = [];
    const big = [];
    const arr = [1, 2, 3];
    let got;
__kitS.section("index == length reaches the Array.prototype getter", () => {
    for (let i = 0; i < 100; i++)
        big.push(i);
    Object.defineProperty(Array.prototype, 3, {
        get() { hits.push(this.length); return "proto3"; },
        configurable: true,
    });
    for (let i = 0; i < 4; i++)
        got = arr[i];
    eq(got, "proto3", "index == length reaches the Array.prototype getter");
});
__kitS.section("getter ran once with the array as receiver", () => {
    eq(hits.join(), "3", "getter ran once with the array as receiver");
});
__kitS.section("index past a shrunk length reaches the prototype, not stale storage", () => {
    big.length = 3;
    eq(big[3], "proto3", "index past a shrunk length reaches the prototype, not stale storage");
});

    function mapped(a, b) {
        a = "A";
        arguments[1] = "B";
        let out = "";
        for (let i = 0; i < arguments.length; i++)
            out += arguments[i];
        return out + a + b;
    }
    function unmapped(a) {
        "use strict";
        a = 9;
        return arguments[0];
    }
__kitS.section("shrunk array index past length after the getter is gone", () => {
    delete Array.prototype[3];
    eq(big[3], undefined, "shrunk array index past length after the getter is gone");
    eq(mapped(1, 2, 3), "AB3AB", "mapped arguments read through var_refs");
    eq(unmapped(5), 5, "unmapped arguments read");
});
__kitS.section("typed array read keeps its element type", () => {
    i16 = new Int16Array([-2, 7]);
    eq(i16[0] + i16[1], 5, "typed array read keeps its element type");
});
}
__kitS.end();
if (!__kitS.listing) {

if (failures)
    throw new Error(failures + " failure(s)");
console.log("ok test_index_key_canon");
}

});
