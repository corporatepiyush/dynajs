// covers: c:src/vm/interpreter.inc.c
// Every slot of a bytecode function's frame (copied arguments, locals,
// operand stack, captured-variable table) must be written before anything
// reads it. The frame is allocated without the hardening zero-fill; in
// ASan/UBSan builds it is filled with a poison JSValue (tag -4, unmapped
// pointer) so a read-before-write faults in the san stage. This suite drives
// every prologue path: fewer args than params, copied argv (calls from
// natives), arguments objects, hoisted var reads, TDZ, captures before
// assignment, tail-call frame reuse, exceptions unwinding a partly used
// operand stack, and frames large enough to cross pages.

import("../lib/kit.js").then(async ({ sections: __kitSections }) => {
const __kitS = __kitSections("test_frame_init", { chain: true });
let bigFn;
let failures = 0;
function eq(actual, expected, what) {
    if (actual !== expected && !(actual !== actual && expected !== expected)) {
        failures++;
        console.log("FAIL", what, "got", String(actual), "expected", String(expected));
    }
}

function fewer(a, b, c, d) {
    return [a, b, c, d].map((x) => (x === undefined ? "u" : String(x))).join("");
}

function argsObj(a, b, c) {
    return arguments.length + ":" + Array.prototype.join.call(arguments, "|");
}

function mapped(a, b) {
    arguments[0] = 10;
    b = 20;
    return a + ":" + arguments[1];
}

function hoisted() {
    const before = typeof v1 + typeof v2 + typeof v3;
    var v1 = 1, v2 = "s", v3 = {};
    return before + typeof v1 + typeof v2 + typeof v3;
}

function tdz() {
    try {
        return x;
    } catch (e) {
        return e instanceof ReferenceError ? "tdz" : "other";
    } finally {
    }
    let x = 1;
}

function captureBefore(n) {
    const read = () => (typeof late) + ":" + n;
    const first = read();
    var late = n * 2;
    return first + "/" + read();
}

function captureArgMissing(a, b) {
    const f = () => b;
    const before = f();
    b = a + 1;
    return String(before) + ":" + f();
}

function loopCaptures(n) {
    const fs = [];
    for (let i = 0; i < n; i++) {
        let k;
        fs.push(() => k);
        k = i * i;
    }
    return fs.map((f) => f()).join();
}

function tcTarget(a, b, c, d, e, f) {
    let local1, local2;
    local1 = a;
    return [local1, b, c, d, e, f, local2].map((x) => (x === undefined ? "u" : x)).join("");
}
function tcBig(a) {
    let p1 = 1, p2 = 2, p3 = 3, p4 = 4, p5 = 5, p6 = 6, p7 = 7, p8 = 8;
    p1 = p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8;
    return tcTarget(a, p1);
}
function tcLoop(n, acc) {
    if (n === 0)
        return acc;
    return tcLoop(n - 1, acc + n);
}

function deepThrow(n) {
    if (n === 0)
        throw new Error("bottom");
    return 1 + [deepThrow(n - 1)].length;
}
let caught = "";
__kitS.section("no args", () => {
eq(fewer(), "uuuu", "no args");
eq(fewer(1), "1uuu", "one arg");
eq(fewer(1, 2, 3), "123u", "three args");
eq(fewer(1, 2, 3, 4, 5), "1234", "extra args");

eq([1, 2, 3].map(function (x, i, arr, extra) { return extra === undefined ? x : -1; }).join(), "1,2,3", "copied argv from native");
eq(Reflect.apply(fewer, null, [7]), "7uuu", "Reflect.apply fewer");
eq(fewer.call(null, 8, 9), "89uu", "call fewer");
eq(argsObj(), "0:", "arguments empty");
eq(argsObj(1), "1:1", "arguments one");
eq(argsObj(1, 2, 3, 4), "4:1|2|3|4", "arguments extra");
eq(mapped(1), "10:undefined", "mapped arguments with missing param");
eq(mapped(1, 2), "10:20", "mapped arguments");
eq(hoisted(), "undefinedundefinedundefinednumberstringobject", "hoisted var reads");
eq(tdz(), "tdz", "let TDZ");
eq(captureBefore(3), "undefined:3/number:3", "capture before assignment");
eq(captureArgMissing(4), "undefined:5", "capture missing arg");
eq(loopCaptures(5), "0,1,4,9,16", "per-iteration bindings");
eq(tcBig("a"), "a36uuuuu", "tail call into fewer-arg frame");
eq(tcLoop(10000, 0), 50005000, "tail-call loop");
});

function midStackThrow(o) {
    return [1, 2, 3, o.missing.field, 5];
}
let msg = "";
__kitS.section("exception unwinding partly-filled operand stacks", () => {
try {
    deepThrow(50);
} catch (e) {
    caught = e.message;
}
eq(caught, "bottom", "exception unwinding partly-filled operand stacks");
});

function catchInLoop(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        try {
            if (i % 3 === 0)
                throw i;
            s += i;
        } catch (e) {
            s -= e;
        }
    }
    return s;
}
__kitS.section("throw with live operand stack", () => {
try {
    midStackThrow({});
} catch (e) {
    msg = e instanceof TypeError ? "type" : "other";
}
eq(msg, "type", "throw with live operand stack");
});

let bigSrc = "(function(seed){";
__kitS.section("catch offsets on the operand stack", () => {
eq(catchInLoop(10), 27 - 18, "catch offsets on the operand stack");
});
let expectBig = 0;

function* gen(a, b) {
    let x;
    yield a;
    yield x;
    x = b;
    yield x;
}

async function asyncFrame(a, b) {
    let x;
    await null;
    return String(a) + String(b) + String(x);
}
let asyncResult = "";
__kitS.section("frame larger than a page", () => {
for (let i = 0; i < 600; i++)
    bigSrc += "var v" + i + (i % 2 ? "" : " = seed + " + i) + ";";
bigSrc += "var s = 0;";
for (let i = 0; i < 600; i++)
    bigSrc += "s += (v" + i + " === undefined ? 1 : v" + i + ");";
bigSrc += "return s;})";
bigFn = (0, eval)(bigSrc);
for (let i = 0; i < 600; i++)
    expectBig += i % 2 ? 1 : 1 + i;
eq(bigFn(1), expectBig, "frame larger than a page");
eq(bigFn(1), expectBig, "frame larger than a page, reused stack");
eq([...gen(1)].map(String).join(), "1,undefined,undefined", "generator frame");
});

function recurse(n) {
    let a, b, c;
    if (n === 0)
        return 0;
    a = n;
    return a + recurse(n - 1) + (b === undefined && c === undefined ? 0 : 1000);
}

let overflow = "";
function unbounded(n) {
    let pad1, pad2, pad3;
    return unbounded(n + 1) + 1;
}
__kitS.section("recursive frames with unassigned locals", () => {
asyncFrame(1).then((v) => { asyncResult = v; });
eq(recurse(150), 11325, "recursive frames with unassigned locals");
});
__kitS.section("stack overflow still a RangeError", () => {
try {
    unbounded(0);
} catch (e) {
    overflow = e instanceof RangeError ? "range" : String(e);
}
eq(overflow, "range", "stack overflow still a RangeError");
});
__kitS.section("frames usable after overflow", () => {
eq(recurse(100), 5050, "frames usable after overflow");
});

__kitS.section("async frame result", () => {
Promise.resolve().then(() => Promise.resolve()).then(() => {
    eq(asyncResult, "1undefinedundefined", "async frame");
    if (failures) {
        console.log("test_frame_init: " + failures + " failure(s)");
        throw new Error("test_frame_init failed");
    }
    console.log("test_frame_init: ok");
});
});
__kitS.end();

});
