// timeout: 120
// flags: --std
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.

let n = 0, failed = 0;
function ok(c, m) { n++; if (!c) { failed++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throwsAny(fn, m) {
    let e = null;
    try { fn(); } catch (err) { e = err; }
    ok(e !== null, m);
}

// Array-ext numeric boundaries
eq([].removeRange(0, 0), [], "empty removeRange");
eq([1].removeRange(0, 2 ** 53), [], "removeRange count 2^53");
eq([1, 2, 3].removeRange(2 ** 53, 5), [1, 2, 3], "removeRange start 2^53 clamps");
eq([1, 2, 3].removeRange(-(2 ** 53), 1), [2, 3], "removeRange start -2^53 clamps to 0");
eq([1, 2, 3].removeRange(0x7fffffffffffffff, 0x7fffffffffffffff), [1, 2, 3], "removeRange INT64_MAX");
eq([1, 2, 3].removeRange(-0x8000000000000000, 3), [], "removeRange INT64_MIN start");
throwsAny(function () { [].aperture(-1); }, "aperture negative throws");
eq([1].aperture(0), [[], []], "aperture 0 returns len+1 empty windows (implementation-defined; DOC-REQ)");
ok(Array.isArray([1, 2].aperture(1)), "aperture normal returns array of windows");
eq([1, 2, 3].aperture(2), [[1, 2], [2, 3]], "aperture normal windows");
eq([].insertAll(0, [1, 2]), [1, 2], "insertAll empty receiver");
eq([1, 2].insertAll(1, []), [1, 2], "insertAll empty source");
throwsAny(function () { Array.prototype.insert.call({ length: 2 ** 53 }, 0, 1); }, "insert on 2^53 length throws");
throwsAny(function () { Array.prototype.prepend.call({ length: 2 ** 32 }, 1); }, "prepend on 2^32 length throws");

// Object shape transitions: fast array -> dictionary -> sparse length writes
(function () {
    let a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    for (let i = 10; i < 40; i++) a[i] = i;
    a[1000] = "x";
    a.length = 20;
    eq(a.length, 20, "sparse shrink keeps length");
    eq(a[1000], undefined, "sparse shrink removes trailing index");
    eq(a[19], 19, "sparse shrink keeps in-range values");
    let b = [];
    b[100] = 1;
    b[101] = 2;
    b.length = 0;
    eq(b.length, 0, "length 0 clears sparse array");
    eq(b[100], undefined, "length 0 removes index");
    let c = [1, 2, 3];
    Object.defineProperty(c, "10", { value: 9, configurable: false, writable: true, enumerable: true });
    c.length = 5;
    eq(c.length, 11, "non-configurable index clamps length");
})();

// Regexp compile malformed / boundary inputs
throwsAny(function () { new RegExp("("); }, "unbalanced group");
throwsAny(function () { new RegExp("["); }, "unbalanced class");
throwsAny(function () { new RegExp("a{2,1}"); }, "reversed quantifier");
throwsAny(function () { new RegExp("\\"); }, "trailing backslash");
ok(new RegExp("(?:a|b|c|d|e|f|g|h){2,4}").test("hgfedcba"), "nested alternatives with quantifier");
eq(new RegExp("^(a|b|c|d|e|f|g|h|i|j)+$").exec("jihgfedcba")[0], "jihgfedcba", "long alternation group");
eq(new RegExp("((((((((((a))))))))))").exec("a")[10], "a", "deep capture nesting");
eq("a".repeat(1000).replace(new RegExp("a{500}"), "b")[0], "b", "quantified repetition 500");
eq(/(?:)/.exec("")[0], "", "empty pattern");
eq(/\1(a)/.exec("aa")[0], "a", "forward backreference matches empty before participation");
eq(/(?<x>a)(?<y>b)\k<y>\k<x>/.exec("abba")[0], "abba", "two named groups cross backrefs");
throwsAny(function () { new RegExp("\\k<x>(?<y>a)"); }, "forward undefined name in unicode");
eq(new RegExp("a|".repeat(5000) + "a").exec("a")[0], "a", "alternation chain priority");
eq(new RegExp("(?:ab)+").exec("ababab")[0], "ababab", "repeated group");
eq(new RegExp("[\\s\\S]*").exec("x\ny")[0], "x\ny", "space class union");

// Parser contextual keywords / ASI boundaries
eq(new Function("return class C { get\nfoo() { return 1; } };")() .prototype.foo, 1, "getter after newline");
eq(new Function("let C = class C { set\nfoo(v) { this._v = v; } m() { return this._v; } }; let c = new C(); c.foo = 5; return c.m();")(), 5, "setter after newline stores value");
eq(new Function("return class C { static get x() { return 1; } };")() .x, 1, "static getter");
ok(new Function("return class C { async foo() { return 2; } };")() .prototype.foo() instanceof Promise, "async method returns a promise");
eq(new Function("return ({ get: 1 })")().get, 1, "object literal get key");
eq(new Function("return ({ set: 2 })")().set, 2, "object literal set key");
eq(new Function("return ({ async: 3 })")().async, 3, "object literal async key");
ok(new Function("let a = 1\nlet b = 2\nreturn a + b")() === 3, "ASI two lets");
eq(new Function("return (1,\n2)")() , 2, "parenthesized newline");
throwsAny(function () { new Function("return (1\n2)")(); }, "no ASI inside parens");

// Number formatting boundaries
ok((0).format(0) === "0", "format zero");
ok((1234.5678).format(2, "_", ".").indexOf("_") > 0, "format custom separators");
ok((-1234.5678).format(2).indexOf("-") === 0, "format negative sign");
ok((1e-300).abbr(5).length > 0, "abbr tiny value");
ok((0).abbr(3).length > 0, "abbr zero");
ok(Number.isFinite((1e308).round(-308)), "round -308 finite");
ok(Number.isFinite((1e100).ceil(20)), "ceil high precision finite");
eq((1).pad(3, true), "+001", "pad with sign");
eq((-1).pad(3, true), "-001", "pad negative with sign");
eq((255).pad(0, false, 16), "ff", "pad hex lowercase");
eq((86400000).duration(), "1 day", "duration day");
eq((0).ordinalize(), "0th", "ordinalize zero");
eq((11).ordinalize(), "11th", "ordinalize 11th");
eq((21).ordinalize(), "21st", "ordinalize 21st");
eq((112).ordinalize(), "112th", "ordinalize 112th");

// JSON.rawJSON / Promise.try argument shapes
eq(JSON.stringify([JSON.rawJSON("0"), JSON.rawJSON("-1.5"), JSON.rawJSON('"s"')]), '[0,-1.5,"s"]', "rawJSON primitives in array");
throwsAny(function () { JSON.rawJSON("{}"); }, "rawJSON empty object text");
throwsAny(function () { JSON.rawJSON("1 2"); }, "rawJSON trailing garbage");
throwsAny(function () { JSON.rawJSON("01"); }, "rawJSON leading zero");
let trySeen = "pending";
Promise.try(function () { return 1; }).then(function (v) { trySeen = v; });
Promise.try(function () { throw new RangeError("x"); }).catch(function (e) { ok(e instanceof RangeError, "Promise.try throw routes to reject"); });

// String ext boundaries
eq("".trimChars("x"), "", "trimChars empty");
eq("xxx".trimChars("x"), "", "trimChars all");
eq("\u00e9x\u00e9".trimChars("\u00e9"), "x", "trimChars wide");
eq("abc".containsAny("xyz"), false, "containsAny miss");
eq("abc".containsAny("cb"), true, "containsAny hit");

// Iterator helpers boundaries
eq([].reduce(function (a, b) { return a + b; }, 0), 0, "empty reduce with seed");
throwsAny(function () { [].reduce(function (a, b) { return a + b; }); }, "empty reduce without seed throws");
eq(Object.groupBy([], function (v) { return v; }), {}, "groupBy empty");
eq(Object.groupBy([1, 2, 3], function (v) { return v > 1 ? "big" : "small"; }).big, [2, 3], "groupBy groups");
eq(Object.groupBy([1, 2, 3], function (v) { return v; })[2], [2], "groupBy numeric keys");

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
