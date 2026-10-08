// Parametric black-box contract test, generated from dynajs.d.ts lines 7676-7979. Engine sources not consulted.
// Covers: Number + NumberConstructor.range, ObjectConstructor ramda-family, Object legacy accessors,
// Date extensions, RegExp.escape + RegExp.unicodeSets.
// Every expectation below is derived from the dynajs.d.ts contract text only. DOC-TENSION rows are marked inline.

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }

// ---- local extras ----
// Canonical JSON (object keys sorted) so object rows do not depend on key order.
function canon(v) {
  if (Array.isArray(v)) return "[" + v.map(canon).join(",") + "]";
  if (v !== null && typeof v === "object") {
    const ks = Object.keys(v).sort();
    return "{" + ks.map((k) => JSON.stringify(k) + ":" + canon(v[k])).join(",") + "}";
  }
  return JSON.stringify(v);
}
function assertDeepEqSorted(a, b, msg) { n++; if (canon(a) !== canon(b)) throw new Error("assertion failed (deep sorted): " + msg + " — got " + canon(a) + " expected " + canon(b)); }
function approxEq(got, expected, tol, label) { n++; if (!(typeof got === "number" && Math.abs(got - expected) <= tol)) throw new Error("assertion failed (approx): " + label + " — got |" + got + "| expected ~|" + expected + "| tol " + tol); }
// Row-kind dispatcher: "@number" => typeof check; function => predicate(got); Array => order-pinned deep;
// object => key-order-insensitive deep; anything else => Object.is equality. Failure messages name the row label.
function check(got, expected, label) {
  if (expected === "@number") { n++; if (typeof got !== "number") throw new Error("assertion failed (typeof number): " + label); return; }
  if (typeof expected === "function") { n++; if (!expected(got)) throw new Error("assertion failed (predicate): " + label + " — got |" + got + "|"); return; }
  if (Array.isArray(expected)) { assertDeepEq(got, expected, label); return; }
  if (expected !== null && typeof expected === "object") { assertDeepEqSorted(got, expected, label); return; }
  assertEq(got, expected, label);
}
// Fixed UTC-constructed date factory: all Date rows use UTC construction; zone-ambiguous rows are
// asserted zone-agnostically (see the boundary section).
const D = (y, mo, da, h, mi, s, ms) => new Date(Date.UTC(y, mo, da, h || 0, mi || 0, s || 0, ms || 0));

/* ================================================================== *
 * 1. Number methods (dynajs.d.ts 7676-7736)
 * ================================================================== */

// Table N1: exact method rows [label, receiver, method, args, expected].
// NOTE: doc pins NO error classes for any Number method, so refusal rows cannot be contract-derived here.
const N1 = [
  ["abs(-5)", -5, "abs", [], 5],
  ["abs(2.5)", 2.5, "abs", [], 2.5],
  ["abs(0)", 0, "abs", [], 0],
  ["sqrt(16)", 16, "sqrt", [], 4],
  ["sqrt(0.25)", 0.25, "sqrt", [], 0.5],
  ["exp(0)", 0, "exp", [], 1],
  ["sin(0)", 0, "sin", [], 0],
  ["cos(0)", 0, "cos", [], 1],
  ["tan(0)", 0, "tan", [], 0],
  ["asin(0)", 0, "asin", [], 0],
  ["acos(1)", 1, "acos", [], 0],
  ["atan(0)", 0, "atan", [], 0],
  ["negate(5)", 5, "negate", [], -5],
  ["negate(-2.5)", -2.5, "negate", [], 2.5],
  ["inc(1)", 1, "inc", [], 2],
  ["inc(-1)", -1, "inc", [], 0],
  ["dec(1)", 1, "dec", [], 0],
  ["dec(0)", 0, "dec", [], -1],
  ["add(2,3)", 2, "add", [3], 5],
  ["add(-2,-3)", -2, "add", [-3], -5],
  ["add decimals", 1.5, "add", [2.25], 3.75],
  ["subtract(5,3)", 5, "subtract", [3], 2],
  ["subtract(3,5)", 3, "subtract", [5], -2],
  ["multiply(6,7)", 6, "multiply", [7], 42],
  ["multiply by zero", 9, "multiply", [0], 0],
  ["multiply decimals", 2.5, "multiply", [4], 10],
  ["divide(10,4)", 10, "divide", [4], 2.5],
  ["divide(10,2)", 10, "divide", [2], 5],
  ["modulo(7,3)", 7, "modulo", [3], 1],
  ["modulo sign of DIVIDEND (-7,3) — contrast mathMod", -7, "modulo", [3], -1],
  ["modulo sign of DIVIDEND (7,-3)", 7, "modulo", [-3], 1],
  ["pow(2,10)", 2, "pow", [10], 1024],
  ["pow(9,0.5)", 9, "pow", [0.5], 3],
  ["pow(5,0)", 5, "pow", [0], 1],
  ["pow(2,-2)", 2, "pow", [-2], 0.25],
  ["gt true", 5, "gt", [3], true],
  ["gt false", 3, "gt", [5], false],
  ["gt equal false", 3, "gt", [3], false],
  ["gte equal true", 3, "gte", [3], true],
  ["gte false", 2, "gte", [3], false],
  ["lt true", 2, "lt", [3], true],
  ["lt false", 3, "lt", [2], false],
  ["lte equal true", 4, "lte", [4], true],
  ["lte false", 5, "lte", [4], false],
  ["isInteger(5)", 5, "isInteger", [], true],
  ["isInteger(5.5)", 5.5, "isInteger", [], false],
  ["isInteger integral double", 5.0, "isInteger", [], true],
  ["isOdd(3)", 3, "isOdd", [], true],
  ["isOdd(4)", 4, "isOdd", [], false],
  ["isOdd(0)", 0, "isOdd", [], false],
  ["isOdd(-3)", -3, "isOdd", [], true],
  ["isEven(0)", 0, "isEven", [], true],
  ["isEven(-4)", -4, "isEven", [], true],
  ["isEven(7)", 7, "isEven", [], false],
  // dynajs.d.ts: "isOdd/isEven are both false on non-integers" — no partition on 3.5;
  // complementarity holds on integers
  ["isOdd/isEven both false on a non-integer (doc)", 3.5, "isOdd", [], false],
  ["isEven also false on a non-integer (doc)", 3.5, "isEven", [], false],
  ["isOdd/isEven are complements on integers (definitional)", 3, "isOdd", [], (odd) => odd === !(3).isEven()],
  ["isMultipleOf(10,5)", 10, "isMultipleOf", [5], true],
  ["isMultipleOf(10,3)", 10, "isMultipleOf", [3], false],
  ["isMultipleOf(-6,3)", -6, "isMultipleOf", [3], true],
  ["isMultipleOf(6,-3)", 6, "isMultipleOf", [-3], true],
  ["isMultipleOf(0,3)", 0, "isMultipleOf", [3], true],
  ["isMultipleOf(7,1)", 7, "isMultipleOf", [1], true],
  ["mathMod(-7,3) sign of DIVISOR (doc)", -7, "mathMod", [3], 2],
  ["mathMod(7,3)", 7, "mathMod", [3], 1],
  // dynajs.d.ts: mathMod is NaN unless both are integers and n >= 1 — negative
  // divisors are refused, they never take the divisor's sign
  ["mathMod(7,-3) -> NaN (doc: n >= 1)", 7, "mathMod", [-3], NaN],
  ["mathMod(-7,-3) -> NaN (doc: n >= 1)", -7, "mathMod", [-3], NaN],
  ["mathMod exact zero", 6, "mathMod", [3], 0],
  ["clamp middle", 5, "clamp", [1, 10], 5],
  ["clamp below lo", 0, "clamp", [1, 10], 1],
  ["clamp above hi", 20, "clamp", [1, 10], 10],
  ["clamp at lo boundary", 1, "clamp", [1, 10], 1],
  ["clamp at hi boundary", 10, "clamp", [1, 10], 10],
  ["log(1)", 1, "log", [], 0],
  ["log(10) in base 10 (x/x ratio is exact)", 10, "log", [10], 1],
  ["log(1) in base 10", 1, "log", [10], 0],
  ["round(2.4)", 2.4, "round", [], 2],
  ["round(2.5) HALF AWAY FROM ZERO (doc)", 2.5, "round", [], 3],
  ["round(-2.5) HALF AWAY FROM ZERO (doc; not Math.round)", -2.5, "round", [], -3],
  ["round(-2.4)", -2.4, "round", [], -2],
  ["round(0.5)", 0.5, "round", [], 1],
  ["round(-0.5)", -0.5, "round", [], -1],
  ["round integer identity", 42, "round", [], 42],
  ["round(precision 0) parity with default", 2.5, "round", [0], 3],
  ["round(1.234, 2)", 1.234, "round", [2], 1.23],
  ["round(125, -1) NEGATIVE precision: tens", 125, "round", [-1], 130],
  ["round(-125, -1) tens away from zero", -125, "round", [-1], -130],
  ["round(1250, -2) NEGATIVE precision: hundreds", 1250, "round", [-2], 1300],
  ["round(1234.5678, -2)", 1234.5678, "round", [-2], 1200],
  ["ceil(1.2)", 1.2, "ceil", [], 2],
  ["ceil(-1.2)", -1.2, "ceil", [], -1],
  ["ceil integer", 3, "ceil", [], 3],
  ["ceil(1.21, 1)", 1.21, "ceil", [1], 1.3],
  ["ceil(17, -1) tens", 17, "ceil", [-1], 20],
  ["ceil(25, -1) tens", 25, "ceil", [-1], 30],
  ["floor(1.7)", 1.7, "floor", [], 1],
  ["floor(-1.2)", -1.2, "floor", [], -2],
  ["floor integer", 3, "floor", [], 3],
  ["floor(1.29, 1)", 1.29, "floor", [1], 1.2],
  ["floor(16, -1) tens", 16, "floor", [-1], 10],
  ["floor(25, -1) tens", 25, "floor", [-1], 20],
  ["chr(65)", 65, "chr", [], "A"],
  ["chr(97)", 97, "chr", [], "a"],
  ["chr(32)", 32, "chr", [], " "],
  ["chr astral CODE POINT (not code unit)", 128512, "chr", [], "\u{1F600}"],
  ["pad(5,3) zero-pads to place digits", 5, "pad", [3], "005"],
  ["pad(42,4)", 42, "pad", [4], "0042"],
  ["pad no padding needed", 123, "pad", [2], "123"],
  ["pad base 16", 255, "pad", [4, false, 16], "00ff"],
  ["pad base 2", 5, "pad", [4, false, 2], "0101"],
  ["pad base 36", 36, "pad", [2, false, 36], "10"],
  ["hex(15) no padding without place", 15, "hex", [], "f"],
  ["hex(254,4) zero-padded", 254, "hex", [4], "00fe"],
  ["hex(255,2) lowercase (doc)", 255, "hex", [2], "ff"],
  ["format grouping (place 0)", 1234567, "format", [0, ",", "."], "1,234,567"],
  ["format custom separators (place 0)", 987654321, "format", [0, " ", ","], "987 654 321"],
  ["format below grouping threshold", 123, "format", [0, ",", "."], "123"],
  ["format with decimals (doc does not pin place>0 shape; structural)", 1234.5, "format", [1, ".", ","], (s) => typeof s === "string" && s.includes(",") && s.includes("234")],
  ["abbr doc example 1.2k", 1200, "abbr", [1], "1.2k"],
  // DOC-TENSION resolved: d.ts example said "3.4M" but dynajs.d.ts pins the suffix
  // set "k/m/b/t" (lowercase) and the d.ts example itself mixes cases —
  // lowercase is the consistent reading (d.ts corrected)
  ["abbr doc example 3.4m (dynajs.d.ts: lowercase k/m/b/t)", 3400000, "abbr", [1], "3.4m"],
  ["ordinalize 1st", 1, "ordinalize", [], "1st"],
  ["ordinalize 2nd", 2, "ordinalize", [], "2nd"],
  ["ordinalize 3rd", 3, "ordinalize", [], "3rd"],
  ["ordinalize 4th", 4, "ordinalize", [], "4th"],
  ["ordinalize 11th (-teen exception)", 11, "ordinalize", [], "11th"],
  ["ordinalize 12th (-teen exception)", 12, "ordinalize", [], "12th"],
  ["ordinalize 13th (-teen exception)", 13, "ordinalize", [], "13th"],
  ["ordinalize 21st", 21, "ordinalize", [], "21st"],
  ["ordinalize 22nd", 22, "ordinalize", [], "22nd"],
  ["ordinalize 23rd", 23, "ordinalize", [], "23rd"],
  ["ordinalize 101st", 101, "ordinalize", [], "101st"],
  ["ordinalize 111th", 111, "ordinalize", [], "111th"],
  ["ordinalize 0th", 0, "ordinalize", [], "0th"],
  ["times default identity -> indices", 3, "times", [], [0, 1, 2]],
  ["times empty", 0, "times", [], []],
  ["times fn doubling", 4, "times", [function (i) { return i * 2; }], [0, 2, 4, 6]],
  ["times constant fn", 3, "times", [function () { return "x"; }], ["x", "x", "x"]],
  ["upto inclusive end", 1, "upto", [3], [1, 2, 3]],
  ["upto single value", 1, "upto", [1], [1]],
  ["upto start>end empty", 5, "upto", [1], []],
  ["upto step", 1, "upto", [5, 2], [1, 3, 5]],
  ["upto step excludes end", 1, "upto", [4, 2], [1, 3]],
  ["upto step lands exactly on end (inclusive)", 1, "upto", [4, 3], [1, 4]],
  ["upto fn maps values", 1, "upto", [3, 1, function (v) { return v * 10; }], [10, 20, 30]],
  ["upto fn receives index", 1, "upto", [3, 1, function (v, i) { return i; }], [0, 1, 2]],
  ["downto inclusive end", 3, "downto", [1], [3, 2, 1]],
  ["downto single value", 3, "downto", [3], [3]],
  ["downto start<end empty", 1, "downto", [3], []],
  ["downto step", 5, "downto", [1, 2], [5, 3, 1]],
  ["downto step excludes end", 5, "downto", [2, 2], [5, 3]],
];
for (const row of N1) {
  const [label, recv, m, args, expected] = row;
  check(recv[m](...args), expected, label);
}

// Table N2: approx rows [label, receiver, method, args, expected, "approx", tolerance].
// Irrational results cannot be pinned exactly from the doc; the tolerance encodes "the documented value".
const N2 = [
  ["sqrt(2) approx", 2, "sqrt", [], 1.4142135623730951, 1e-15],
  ["exp(1) approx", 1, "exp", [], 2.718281828459045, 1e-15],
  ["sin(pi/2) approx", Math.PI / 2, "sin", [], 1, 1e-15],
  ["tan(pi/4) approx", Math.PI / 4, "tan", [], 1, 1e-15],
  ["log(e) default base approx", Math.E, "log", [], 1, 1e-15],
  ["log(8) base 2 approx", 8, "log", [2], 3, 1e-12],
  ["log(100) base 10 approx", 100, "log", [10], 2, 1e-12],
];
for (const [label, recv, m, args, expected, tol] of N2) approxEq(recv[m](...args), expected, tol, label);

// Table N3: Math-chaining parity rows [label, receiver, method, args, mathEquivalent(recv, args)].
const N3 = [
  ["abs ≡ Math.abs", 2.5, "abs", [], (x) => Math.abs(x)],
  ["sqrt ≡ Math.sqrt", 2, "sqrt", [], (x) => Math.sqrt(x)],
  ["exp ≡ Math.exp", 1.5, "exp", [], (x) => Math.exp(x)],
  ["sin ≡ Math.sin", 0.5, "sin", [], (x) => Math.sin(x)],
  ["cos ≡ Math.cos", 0.5, "cos", [], (x) => Math.cos(x)],
  ["tan ≡ Math.tan", 0.5, "tan", [], (x) => Math.tan(x)],
  ["asin ≡ Math.asin", 0.5, "asin", [], (x) => Math.asin(x)],
  ["acos ≡ Math.acos", 0.5, "acos", [], (x) => Math.acos(x)],
  ["atan ≡ Math.atan", 0.5, "atan", [], (x) => Math.atan(x)],
  ["pow ≡ Math.pow", 2, "pow", [10], (x, a) => Math.pow(x, a[0])],
  ["log() ≡ Math.log", 2, "log", [], (x) => Math.log(x)],
  ["modulo ≡ %", -7, "modulo", [3], (x, a) => x % a[0]],
  ["divide ≡ /", 10, "divide", [4], (x, a) => x / a[0]],
  ["multiply ≡ *", 6, "multiply", [7], (x, a) => x * a[0]],
  ["subtract ≡ -", 5, "subtract", [3], (x, a) => x - a[0]],
  ["add ≡ +", 2, "add", [3], (x, a) => x + a[0]],
  ["negate ≡ -x", 5, "negate", [], (x) => -x],
  ["inc ≡ x+1", 1, "inc", [], (x) => x + 1],
  ["dec ≡ x-1", 1, "dec", [], (x) => x - 1],
  ["round ≡ Math.round for non-half values", 2.4, "round", [], (x) => Math.round(x)],
  ["ceil ≡ Math.ceil", 1.2, "ceil", [], (x) => Math.ceil(x)],
  ["floor ≡ Math.floor", 1.7, "floor", [], (x) => Math.floor(x)],
  ["clamp ≡ Math.min/Math.max", 5, "clamp", [1, 10], (x, a) => Math.min(Math.max(x, a[0]), a[1])],
];
for (const [label, recv, m, args, mathFn] of N3) assertEq(recv[m](...args), mathFn(recv, args), label);

// Table N4: Number.range — the doc stresses EXCLUSIVE end, returns a real array.
const N4 = [
  ["range(1,5) EXCLUSIVE of end (doc)", [1, 5], [1, 2, 3, 4]],
  ["range(0,3)", [0, 3], [0, 1, 2]],
  ["range(1,1) empty because end is exclusive", [1, 1], []],
  ["range with step", [0, 10, 3], [0, 3, 6, 9]],
  ["range step excludes end", [1, 5, 2], [1, 3]],
  ["range step landing on end boundary", [0, 10, 5], [0, 5]],
  // Inferred: `end` optional => single-arg form counts up from 0, exclusive.
  ["range single-arg form (inferred from optional end)", [3], [0, 1, 2]],
];
for (const [label, args, expected] of N4) check(Number.range(...args), expected, label);

// Table N5: structural rows for outputs the doc names but does not pin exactly.
const N5 = [
  ["pad sign forces + (layout unpinned; structural)", 5, "pad", [3, true], (s) => typeof s === "string" && s.startsWith("+") && s.endsWith("5")],
  ["pad negative keeps - (layout unpinned; structural)", -5, "pad", [3], (s) => typeof s === "string" && s.startsWith("-") && s.endsWith("05")],
  ["metric SI string", 1500, "metric", [], (s) => typeof s === "string" && s.length > 0],
  ["metric small magnitude", 0.001, "metric", [], (s) => typeof s === "string" && s.length > 0],
  ["bytes zero", 0, "bytes", [], (s) => typeof s === "string" && s.length > 0],
  ["bytes 1MiB", 1048576, "bytes", [], (s) => typeof s === "string" && s.length > 0],
  ["duration structural", 3661, "duration", [], (s) => typeof s === "string" && s.length > 0],
  ["duration zero", 0, "duration", [], (s) => typeof s === "string" && s.length > 0],
  ["abbr below threshold structural", 999, "abbr", [], (s) => typeof s === "string" && s.length > 0],
  ["abbr very large structural", 1500000000, "abbr", [], (s) => typeof s === "string" && s.length > 0],
];
for (const [label, recv, m, args, pred] of N5) check(recv[m](...args), pred, label);

/* ================================================================== *
 * 2. ObjectConstructor ramda-family (dynajs.d.ts 7745-7861)
 * ================================================================== */

// Table O1: isX family + isNil/isNotNil [label, value, fn, expected].
const O1 = [
  ["isObject plain object", {}, "isObject", true],
  ["isObject null false", null, "isObject", false],
  ["isObject number false", 42, "isObject", false],
  ["isObject string false", "s", "isObject", false],
  ["isArray array", [], "isArray", true],
  ["isArray object false", {}, "isArray", false],
  ["isBoolean true", true, "isBoolean", true],
  ["isBoolean number false", 0, "isBoolean", false],
  ["isNumber 42", 42, "isNumber", true],
  ["isNumber NaN", NaN, "isNumber", true],
  ["isNumber string false", "42", "isNumber", false],
  ["isString", "s", "isString", true],
  ["isString number false", 1, "isString", false],
  ["isFunction arrow", () => 1, "isFunction", true],
  ["isFunction object false", {}, "isFunction", false],
  ["isDate date", new Date(), "isDate", true],
  ["isDate string false", "2024-01-01", "isDate", false],
  ["isRegExp regex", /r/, "isRegExp", true],
  ["isRegExp string false", "/r/", "isRegExp", false],
  ["isError error", new Error("e"), "isError", true],
  ["isError object false", {}, "isError", false],
  ["isSet set", new Set(), "isSet", true],
  ["isSet array false", [], "isSet", false],
  ["isMap map", new Map(), "isMap", true],
  ["isMap set false", new Set(), "isMap", false],
  ["isArguments true ONLY for arguments objects (doc)", (function () { return arguments; })(), "isArguments", true],
  ["isArguments array false", [], "isArguments", false],
  ["isArguments object false", {}, "isArguments", false],
  ["isNil null", null, "isNil", true],
  ["isNil undefined", undefined, "isNil", true],
  ["isNil zero false", 0, "isNil", false],
  ["isNil empty string false", "", "isNil", false],
  ["isNotNil zero", 0, "isNotNil", true],
  ["isNotNil null false", null, "isNotNil", false],
  ["isNotNil undefined false", undefined, "isNotNil", false],
];
for (const [label, value, fn, expected] of O1) check(Object[fn](value), expected, label);

// Table O1b: type() short names [label, value, expected].
const O1b = [
  ["type String", "s", "String"],
  ["type Number", 42, "Number"],
  ["type Boolean", true, "Boolean"],
  ["type Object", {}, "Object"],
  ["type Array", [], "Array"],
  ["type Function", () => 1, "Function"],
  ["type Null", null, "Null"],
  ["type Undefined", undefined, "Undefined"],
  ["type Date", new Date(), "Date"],
  ["type RegExp", /r/, "RegExp"],
  ["type Error", new Error("e"), "Error"],
];
for (const [label, value, expected] of O1b) check(Object.type(value), expected, label);

// Table O2: heterogeneous contract rows [label, thunk, expected].
const O2 = [
  ["defaultTo keeps value", () => Object.defaultTo("d", "v"), "v"],
  ["defaultTo null -> default", () => Object.defaultTo("d", null), "d"],
  ["defaultTo undefined -> default", () => Object.defaultTo("d", undefined), "d"],
  ["defaultTo zero is NOT nil", () => Object.defaultTo("d", 0), 0],
  ["defaultTo empty string is NOT nil", () => Object.defaultTo("d", ""), ""],
  ["size counts own enumerable", () => Object.size({ a: 1, b: 2 }), 2],
  ["size empty", () => Object.size({}), 0],
  ["isEmpty true", () => Object.isEmpty({}), true],
  ["isEmpty false", () => Object.isEmpty({ a: 1 }), false],
  ["objOf builds single-key object", () => Object.objOf("k", 1), { k: 1 }],
  ["toPairs key-value pairs", () => Object.toPairs({ a: 1, b: 2 }), [["a", 1], ["b", 2]]],
  ["fromPairs rebuilds object", () => Object.fromPairs([["a", 1], ["b", 2]]), { a: 1, b: 2 }],
  // ramda-style pred-first argument order (dynajs.d.ts + d.ts corrected; the d.ts
  // had the outlier (obj, pred))
  ["pickBy keeps matching values", () => Object.pickBy((v) => typeof v === "number" && v % 2 === 1, { a: 1, b: 2, c: 3 }), { a: 1, c: 3 }],
  ["tap passes value and returns it", () => { let seen; const o = { a: 1 }; const r = Object.tap((v) => { seen = v; }, o); return seen === o && r === o; }, true],
  // DOC-TENSION resolved: dynajs.d.ts pins clone as a DEEP clone ("a cyclic
  // structure overflows the C stack"); d.ts's old "shallow" wording corrected
  ["clone deep: nested refs are COPIES, not shared", () => { const o = { a: 1, n: { b: 2 } }; const c = Object.clone(o); return c !== o && c.n !== o.n && c.n.b === 2; }, true],
  ["clone deep: nested mutation isolated", () => { const o = { a: 1, n: { b: 2 } }; const c = Object.clone(o); c.n.b = 99; return o.n.b === 2; }, true],
  ["clone shallow: top-level mutation isolated", () => { const o = { a: 1 }; const c = Object.clone(o); c.a = 99; return o.a === 1; }, true],
  ["equals deep nested", () => Object.equals({ a: [1, { b: 2 }] }, { a: [1, { b: 2 }] }), true],
  ["equals arrays ordered", () => Object.equals([1, 2], [1, 2]), true],
  ["equals order matters", () => Object.equals([1, 2], [2, 1]), false],
  ["equals value mismatch", () => Object.equals({ a: 1 }, { a: 2 }), false],
  ["equals cross-type false", () => Object.equals("1", 1), false],
  ["equals same reference", () => { const x = { a: [1] }; return Object.equals(x, x); }, true],
  ["identical numbers", () => Object.identical(1, 1), true],
  ["identical NaN true (Object.is identity)", () => Object.identical(NaN, NaN), true],
  ["identical 0 vs -0 false (Object.is identity)", () => Object.identical(0, -0), false],
  ["identical -0 self true", () => Object.identical(-0, -0), true],
  ["identical distinct objects false", () => Object.identical({}, {}), false],
  ["identical strings", () => Object.identical("a", "a"), true],
  ["equals is deep where identical is not (contrast row)", () => Object.equals({ a: 1 }, { a: 1 }) && !Object.identical({ a: 1 }, { a: 1 }), true],
  ["invert swaps keys/values", () => Object.invert({ a: "1", b: "2" }), { 1: "a", 2: "b" }],
  ["invertObj swaps keys/values (LEGACY alias)", () => Object.invertObj({ a: "1", b: "2" }), { 1: "a", 2: "b" }],
  ["invert collision collapses to one key (structural)", () => { const r = Object.invert({ a: "x", b: "x" }); return Object.size(r) === 1 && "x" in r; }, true],
  ["groupBy by derived key", () => Object.groupBy(["a", "ab", "b", "bc"], (s) => s[0]), { a: ["a", "ab"], b: ["b", "bc"] }],
  ["groupBy numeric parity keys", () => Object.groupBy([1, 2, 3, 4], (x) => (x % 2 === 0 ? "even" : "odd")), { odd: [1, 3], even: [2, 4] }],
  ["groupBy empty input", () => Object.groupBy([], (x) => String(x)), {}],
  ["pickAll keeps absent keys as undefined (doc)", () => { const r = Object.pickAll(["a", "z"], { a: 1 }); return "z" in r && r.z === undefined && r.a === 1; }, true],
  ["pick drops absent keys (contrast with pickAll)", () => { const r = Object.pick(["a", "z"], { a: 1 }); return !("z" in r) && r.a === 1; }, true],
];
for (const [label, thunk, expected] of O2) check(thunk(), expected, label);

// Table O3: prop/path/get/set/transform family [label, fn, args, expected] (signatures per doc).
function fnAdd(a, b) { return a + b; }
function fnKey(k, a) { return k === "a" ? a : b2(k, a); }
function b2(k, a) { return a; }
const O3 = [
  // argument orders per dynajs.d.ts (ramda-style; the d.ts block, since corrected, was the outlier)
  ["prop reads key", "prop", ["a", { a: 1 }], 1],
  ["prop missing -> undefined", "prop", ["z", { a: 1 }], undefined],
  ["propOr default when missing", "propOr", ["d", "z", { a: 1 }], "d"],
  // propOr defaults only on undefined/missing (ramda semantics; null is a value)
  ["propOr null is a real value, not missing", "propOr", ["d", "a", { a: null }], null],
  ["propOr undefined gets default", "propOr", ["d", "a", { a: undefined }], "d"],
  ["propOr keeps found value", "propOr", ["d", "a", { a: 1 }], 1],
  ["props at several keys (missing -> undefined)", "props", [["a", "c", "z"], { a: 1, b: 2, c: 3 }], [1, 3, undefined]],
  ["path nested", "path", [["a", "b"], { a: { b: 2 } }], 2],
  ["path missing leaf -> undefined", "path", [["a", "z"], { a: { b: 2 } }], undefined],
  ["path through scalar -> undefined", "path", [["a", "b"], { a: 1 }], undefined],
  ["pathOr default when path missing", "pathOr", ["d", ["x", "y"], { a: 1 }], "d"],
  ["pathOr keeps found value", "pathOr", ["d", ["a"], { a: 1 }], 1],
  ["paths at several paths (missing -> undefined)", "paths", [[["a", "b"], ["z"], ["a"]], { a: { b: 1 } }], [1, undefined, { b: 1 }]],
  ["assocPath top-level set", "assocPath", [["a"], 9, { a: 1, b: 2 }], { a: 9, b: 2 }],
  ["assocPath nested overwrite", "assocPath", [["a", "b"], 9, { a: { b: 1 }, c: 1 }], { a: { b: 9 }, c: 1 }],
  ["assocPath CREATES intermediate objects (doc)", "assocPath", [["x", "y"], 5, { a: 1 }], { a: 1, x: { y: 5 } }],
  ["dissocPath removes nested key", "dissocPath", [["a", "b"], { a: { b: 1, c: 2 } }], { a: { c: 2 } }],
  ["hasPath true", "hasPath", [["a", "b"], { a: { b: 1 } }], true],
  ["hasPath false when leaf missing", "hasPath", [["a", "z"], { a: { b: 1 } }], false],
  ["hasPath false through scalar", "hasPath", [["a", "b"], { a: 1 }], false],
  ["get dotted path", "get", [{ a: { b: 2 } }, "a.b"], 2],
  ["get array path (same as dotted)", "get", [{ a: { b: 2 } }, ["a", "b"]], 2],
  ["get dotted missing -> undefined (doc: or undefined)", "get", [{ a: { b: 2 } }, "a.z"], undefined],
  ["get deeper than tree -> undefined", "get", [{ a: { b: 2 } }, "a.b.c"], undefined],
  ["set dotted path", "set", [{ a: { b: 1 }, c: 1 }, "a.b", 9], { a: { b: 9 }, c: 1 }],
  ["set array path", "set", [{ a: { b: 1 } }, ["a", "b"], 9], { a: { b: 9 } }],
  ["set creates missing path", "set", [{}, ["x", "y"], 1], { x: { y: 1 } }],
  ["modify one key", "modify", ["a", function (v) { return v * 10; }, { a: 1, b: 2 }], { a: 10, b: 2 }],
  ["modify missing key untouched", "modify", ["z", function (v) { return v * 10; }, { a: 1 }], { a: 1 }],
  ["modifyPath nested", "modifyPath", [["a", "b"], function (v) { return v + 1; }, { a: { b: 1 } }], { a: { b: 2 } }],
  ["evolve applies per-key fns", "evolve", [{ a: function (v) { return v * 2; } }, { a: 2, b: 3 }], { a: 4, b: 3 }],
  ["evolve missing keys untouched", "evolve", [{ n: function (v) { return v + 1; }, s: function () { return 0; } }, { n: 1 }], { n: 2 }], // n exists -> transformed; s absent -> no key
  ["mapObjIndexed gets value and key", "mapObjIndexed", [function (v, k) { return k + v; }, { a: 1 }], { a: "a1" }],
  ["mapKeys transforms keys", "mapKeys", [function (k) { return k.toUpperCase(); }, { a: 1 }], { A: 1 }],
  ["mergeWith custom combine on shared keys", "mergeWith", [fnAdd, { a: 1, b: 10 }, { a: 2, c: 5 }], { a: 3, b: 10, c: 5 }],
  ["mergeWithKey is key-aware", "mergeWithKey", [function (k, a, b) { return k === "a" ? a : b; }, { a: 1, b: 10 }, { a: 2 }], { a: 1, b: 10 }],
  ["renameKeys per {old: new}", "renameKeys", [{ old: "new" }, { old: 1, keep: 2 }], { new: 1, keep: 2 }],
  // defaults(o, d): o's values win, d fills the missing keys (dynajs.d.ts example)
  ["defaults fills MISSING keys only", "defaults", [{ a: 1, b: 2 }, { b: 9 }], { b: 2, a: 1 }],
  ["defaults conflict keeps o's value", "defaults", [{ a: 1, b: 9 }, { b: 2 }], { b: 9, a: 1 }],
  ["project onto given keys", "project", [["a"], [{ a: 1, b: 2 }, { a: 3 }]], [{ a: 1 }, { a: 3 }]],
];
for (const [label, fn, args, expected] of O3) check(Object[fn](...args), expected, label);

// Table O4: merge family [label, fn, a, b, expected]. merge ≡ mergeRight (doc);
// mergeLeft is the FIRST-wins variant, NOT an alias (doc).
const O4 = [
  ["mergeRight later source wins", "mergeRight", { a: 1, b: 2 }, { b: 3, c: 4 }, { a: 1, b: 3, c: 4 }],
  ["merge ≡ mergeRight (doc: canonical pair, alias)", "merge", { a: 1, b: 2 }, { b: 3, c: 4 }, { a: 1, b: 3, c: 4 }],
  ["mergeLeft FIRST wins (doc: not an alias)", "mergeLeft", { a: 1, b: 2 }, { b: 3, c: 4 }, { a: 1, b: 2, c: 4 }],
  ["mergeRight scalar replaces object (shallow)", "mergeRight", { a: { x: 1 } }, { a: 5 }, { a: 5 }],
  ["mergeRight empty b", "mergeRight", { a: 1 }, {}, { a: 1 }],
  ["mergeRight empty a", "mergeRight", {}, { a: 1 }, { a: 1 }],
  ["mergeDeepRight deep-merges nested", "mergeDeepRight", { a: { x: 1, y: 1 } }, { a: { y: 2 } }, { a: { x: 1, y: 2 } }],
  ["mergeDeepLeft keeps first at conflicts", "mergeDeepLeft", { a: { x: 1, y: 1 } }, { a: { y: 2 } }, { a: { x: 1, y: 1 } }],
  ["mergeDeepRight scalar override replaces branch", "mergeDeepRight", { a: { x: 1 } }, { a: 5 }, { a: 5 }],
];
for (const [label, fn, a, b, expected] of O4) check(Object[fn](a, b), expected, label);

// Parity rows: merge(a,b) must equal mergeRight(a,b) on the same inputs (doc: runtime-verified alias).
const O4b = [
  ["merge ≡ mergeRight parity", { a: 1, b: 2 }, { b: 3, c: 4 }],
  ["merge ≡ mergeRight parity empty", {}, {}],
  ["merge ≡ mergeRight parity nested scalars", { a: { x: 1 } }, { a: 9 }],
];
for (const [label, a, b] of O4b) assert(Object.merge(a, b) !== undefined && canon(Object.merge(a, b)) === canon(Object.mergeRight(a, b)), label);

// Table O5: predicate family [label, fn, args, expected].
const O5 = [
  ["propEq true", "propEq", [1, "a", { a: 1, b: 2 }], true],
  ["propEq false", "propEq", [2, "a", { a: 1 }], false],
  ["propEq missing key false", "propEq", [1, "z", { a: 1 }], false],
  ["eqProps true", "eqProps", ["a", { a: 1, b: 9 }, { a: 1, c: 7 }], true],
  ["eqProps false", "eqProps", ["a", { a: 1 }, { a: 2 }], false],
  ["pathEq true", "pathEq", [1, ["a", "b"], { a: { b: 1 } }], true],
  ["pathEq false", "pathEq", [2, ["a", "b"], { a: { b: 1 } }], false],
  ["pathEq missing path false", "pathEq", [1, ["z"], { a: 1 }], false],
  ["propSatisfies true", "propSatisfies", [(v) => v > 0, "a", { a: 1 }], true],
  ["propSatisfies false", "propSatisfies", [(v) => v > 0, "a", { a: -1 }], false],
  ["propSatisfies missing key sees undefined", "propSatisfies", [(v) => v === undefined, "z", { a: 1 }], true],
  ["pathSatisfies true", "pathSatisfies", [(v) => v > 0, ["a", "b"], { a: { b: 1 } }], true],
  ["pathSatisfies false", "pathSatisfies", [(v) => v > 0, ["a", "b"], { a: { b: 0 } }], false],
  ["where every predicate passes", "where", [{ a: (v) => v > 1, b: (v) => v < 3 }, { a: 2, b: 1, c: 9 }], true],
  ["where one predicate fails", "where", [{ a: (v) => v > 1, b: (v) => v < 0 }, { a: 2, b: 1 }], false],
  ["where empty spec passes", "where", [{}, { a: 1 }], true],
  ["whereEq subset match", "whereEq", [{ a: 1 }, { a: 1, b: 2 }], true],
  ["whereEq mismatch", "whereEq", [{ a: 1 }, { a: 2 }], false],
  ["whereEq empty spec", "whereEq", [{}, { z: 1 }], true],
  ["whereEq extra spec key fails", "whereEq", [{ a: 1, z: 2 }, { a: 1 }], false],
  ["whereAny one predicate passes", "whereAny", [{ a: (v) => v > 10, b: (v) => v > 1 }, { a: 1, b: 2 }], true],
  ["whereAny none passes", "whereAny", [{ a: (v) => v > 10, b: (v) => v > 10 }, { a: 1, b: 2 }], false],
  // propIs takes a CONSTRUCTOR (dynajs.d.ts: "is(Ctor, o[k])"); the d.ts's old
  // `type: string` annotation was a doc bug (a string throws TypeError)
  ["propIs Number", "propIs", [Number, "a", { a: 1 }], true],
  ["propIs String false", "propIs", [String, "a", { a: 1 }], false],
  ["propIs Object", "propIs", [Object, "a", { a: {} }], true],
];
for (const [label, fn, args, expected] of O5) check(Object[fn](...args), expected, label);
assertThrows(() => Object.propIs("Number", "a", { a: 1 }), "propIs refuses a string type name (needs a Ctor)", TypeError);

// Table O6a: has vs hasIn on a prototype chain [label, fn, key, obj, expected] — has/hasIn(key, obj).
function ProtoCtor() { this.y = 2; }
ProtoCtor.prototype.x = 1;
const chained = new ProtoCtor();
const O6a = [
  ["has own property", "has", "y", chained, true],
  ["has missing", "has", "zz", chained, false],
  ["hasIn own property", "hasIn", "y", chained, true],
  ["hasIn inherited enumerable", "hasIn", "x", chained, true],
  ["hasIn built-ins on the chain", "hasIn", "toString", chained, true],
  // DOC-TENSION: the doc comment over `has` says "present anywhere on the chain", but the has/hasIn
  // NAME PAIR and canonical semantics pin has=own / hasIn=chain; most specific reading implemented.
  ["has inherited property is own-only", "has", "x", chained, false],
];
for (const [label, fn, key, obj, expected] of O6a) check(Object[fn](key, obj), expected, label);

// Table O6b: keysIn/valuesIn [label, fn, obj, expected] (expected may be a predicate for chain rows).
const O6b = [
  ["keysIn plain object", "keysIn", { a: 1, b: 2 }, ["a", "b"]],
  ["valuesIn plain object", "valuesIn", { a: 1, b: 2 }, [1, 2]],
  ["keysIn includes chain members (structural)", "keysIn", chained, (got) => got.includes("y") && got.includes("x")],
  ["valuesIn includes chain members (structural)", "valuesIn", chained, (got) => got.includes(2) && got.includes(1)],
];
for (const [label, fn, obj, expected] of O6b) check(Object[fn](obj), expected, label);

// Table O7: copy-semantics ("a copy" per doc) rows [label, source, op(source)->result, expected result].
// Asserts BOTH: the source object is untouched (JSON snapshot) and the result matches.
const O7 = [
  ["assoc returns a copy", { a: 1 }, (o) => Object.assoc("b", 2, o), { a: 1, b: 2 }],
  ["assoc overwrite returns a copy", { a: 1 }, (o) => Object.assoc("a", 9, o), { a: 9 }],
  ["dissoc returns a copy", { a: 1, b: 2 }, (o) => Object.dissoc("b", o), { a: 1 }],
  ["dissoc absent key is a no-op copy", { a: 1 }, (o) => Object.dissoc("z", o), { a: 1 }],
  ["pick returns a copy", { a: 1, b: 2 }, (o) => Object.pick(["a"], o), { a: 1 }],
  ["omit returns a copy", { a: 1, b: 2 }, (o) => Object.omit(["b"], o), { a: 1 }],
  ["assocPath returns a copy", { a: 1 }, (o) => Object.assocPath(["x", "y"], 5, o), { a: 1, x: { y: 5 } }],
  ["dissocPath returns a copy", { a: { b: 1, c: 2 } }, (o) => Object.dissocPath(["a", "b"], o), { a: { c: 2 } }],
  ["mergeRight returns a copy", { a: 1 }, (o) => Object.mergeRight(o, { b: 2 }), { a: 1, b: 2 }],
  ["clone returns a copy", { a: 1, n: { b: 2 } }, (o) => Object.clone(o), { a: 1, n: { b: 2 } }],
  ["evolve returns a copy", { n: 1 }, (o) => Object.evolve({ n: (v) => v + 1 }, o), { n: 2 }],
  ["renameKeys returns a copy", { old: 1, keep: 2 }, (o) => Object.renameKeys({ old: "new" }, o), { new: 1, keep: 2 }],
  ["mapKeys returns a copy", { a: 1 }, (o) => Object.mapKeys((k) => k.toUpperCase(), o), { A: 1 }],
  ["defaults returns a copy", { b: 9 }, (o) => Object.defaults({ a: 1 }, o), { b: 9, a: 1 }],
  ["mergeDeepRight returns a copy", { a: { x: 1 } }, (o) => Object.mergeDeepRight(o, { a: { y: 2 } }), { a: { x: 1, y: 2 } }],
];
for (const [label, source, op, expected] of O7) {
  const snapshot = JSON.stringify(source);
  const result = op(source);
  assert(JSON.stringify(source) === snapshot, label + " — source untouched");
  assertDeepEqSorted(result, expected, label + " — result");
}
// set is the MUTATING deep set (dynajs.d.ts: "set(o, p, v) is the mutating deep
// set (returns o)") — deliberately unlike assocPath
{
  const o = { a: { b: 1 }, c: 1 };
  const r = Object.set(o, "a.b", 9);
  assert(r === o, "set returns o itself");
  assertEq(o.a.b, 9, "set mutates the target in place");
  Object.set(o, ["x", "y"], 1);
  assertDeepEqSorted(o.x, { y: 1 }, "set creates missing path");
}

// Table O8: forEachObjIndexed [label, obj, expectedPairs]; doc: returns void.
const O8 = [
  ["forEachObjIndexed visits single pair", { a: 1 }, [["a", 1]]],
  ["forEachObjIndexed visits pairs in order", { a: 1, b: 2 }, [["a", 1], ["b", 2]]],
];
for (const [label, obj, expectedPairs] of O8) {
  const collected = [];
  // d.ts return type is `void`: the return value is not part of the contract,
  // so only the visited pairs are pinned here (the engine happens to return
  // the object, ramda-style, but that is unpinned)
  Object.forEachObjIndexed((v, k) => collected.push([k, v]), obj);
  assertDeepEq(collected, expectedPairs, label + " — collected pairs");
}

// Table O9: Object legacy accessors (on Object.prototype per doc) [label, thunk, expected].
const O9 = [
  ["__defineGetter__ then read", () => { const o = {}; o.__defineGetter__("g", () => 42); return o.g; }, 42],
  ["__defineSetter__ then write", () => { let got; const o = {}; o.__defineSetter__("s", (v) => { got = v; }); o.s = 7; return got; }, 7],
  ["__lookupGetter__ found returns function", () => { const o = {}; o.__defineGetter__("g", () => 1); return typeof o.__lookupGetter__("g"); }, "function"],
  ["__lookupGetter__ miss on data property", () => { const o = { a: 1 }; return o.__lookupGetter__("a"); }, undefined],
  ["__lookupGetter__ miss on absent key", () => { const o = {}; return o.__lookupGetter__("nope"); }, undefined],
  ["__lookupSetter__ found returns function", () => { const o = {}; o.__defineSetter__("s", () => {}); return typeof o.__lookupSetter__("s"); }, "function"],
  ["__lookupSetter__ miss", () => { const o = {}; return o.__lookupSetter__("nope"); }, undefined],
];
for (const [label, thunk, expected] of O9) check(thunk(), expected, label);

/* ================================================================== *
 * 3. Date extensions (dynajs.d.ts 7871-7969)
 * ================================================================== */

// Table T1: fixed-date calendar facts [label, date, method, args, expected].
// All dates are UTC-constructed. Weekday facts verified by hand: 2024-01-01 Mon,
// 2024-01-31 Wed, 2024-02-04 Sun, 2024-03-02 Sat, 2024-12-25 Wed.
const T1 = [
  ["isValid valid date", D(2024, 0, 31), "isValid", [], true],
  ["isValid invalid date", new Date(NaN), "isValid", [], false],
  ["isLeapYear 2024", D(2024, 5, 15), "isLeapYear", [], true],
  ["isLeapYear 2023", D(2023, 5, 15), "isLeapYear", [], false],
  ["isLeapYear 2000 (divisible by 400)", D(2000, 5, 15), "isLeapYear", [], true],
  ["isLeapYear 1900 (divisible by 100 not 400)", D(1900, 5, 15), "isLeapYear", [], false],
  ["daysInMonth Jan", D(2024, 0, 15), "daysInMonth", [], 31],
  ["daysInMonth Feb leap", D(2024, 1, 15), "daysInMonth", [], 29],
  ["daysInMonth Feb common", D(2023, 1, 15), "daysInMonth", [], 28],
  ["daysInMonth Apr", D(2024, 3, 15), "daysInMonth", [], 30],
  ["daysInMonth Dec", D(2024, 11, 15), "daysInMonth", [], 31],
  ["isWednesday 2024-01-31", D(2024, 0, 31, 12), "isWednesday", [], true],
  ["isWeekday Wednesday", D(2024, 0, 31, 12), "isWeekday", [], true],
  ["isWeekend Wednesday false", D(2024, 0, 31, 12), "isWeekend", [], false],
  ["isMonday 2024-01-01", D(2024, 0, 1, 12), "isMonday", [], true],
  ["isSunday 2024-02-04", D(2024, 1, 4, 12), "isSunday", [], true],
  ["isWeekend Sunday", D(2024, 1, 4, 12), "isWeekend", [], true],
  ["isWeekday Sunday false", D(2024, 1, 4, 12), "isWeekday", [], false],
  ["isSaturday 2024-03-02", D(2024, 2, 2, 12), "isSaturday", [], true],
  ["isJanuary", D(2024, 0, 31), "isJanuary", [], true],
  ["isFebruary", D(2024, 1, 9), "isFebruary", [], true],
  ["isDecember", D(2024, 11, 25), "isDecember", [], true],
  ["getWeekday Monday (inferred: JS getDay convention)", D(2024, 0, 1, 12), "getWeekday", [], 1],
  ["getWeekday Wednesday", D(2024, 0, 31, 12), "getWeekday", [], 3],
  ["getWeekday Sunday", D(2024, 1, 4, 12), "getWeekday", [], 0],
  // 2024-01-01 is a Monday, so ISO week 1 = Jan 1-7; Jun 12 is day 164 => week 24 (hand-derived, safe).
  ["getISOWeek mid-year 2024-06-12 = 24", D(2024, 5, 12, 12), "getISOWeek", [], 24],
  ["isBefore later date", D(2024, 0, 1), "isBefore", [D(2024, 2, 1)], true],
  ["isBefore earlier date false", D(2024, 2, 1), "isBefore", [D(2024, 0, 1)], false],
  ["isBefore self false (strict reading)", D(2024, 0, 1), "isBefore", [D(2024, 0, 1)], false],
  ["isAfter earlier date", D(2024, 2, 1), "isAfter", [D(2024, 0, 1)], true],
  ["isAfter later date false", D(2024, 0, 1), "isAfter", [D(2024, 2, 1)], false],
  ["isBetween inside window", D(2024, 0, 15), "isBetween", [D(2024, 0, 1), D(2024, 1, 15)], true],
  ["isBetween before window", D(2023, 11, 15), "isBetween", [D(2024, 0, 1), D(2024, 1, 15)], false],
  ["isBetween after window", D(2024, 5, 15), "isBetween", [D(2024, 0, 1), D(2024, 1, 15)], false],
];
for (const [label, d, m, args, expected] of T1) check(d[m](...args), expected, label);

// Table T2: exact delta rows [label, base, method, args, expected number].
// 2024-01-01T00:00Z .. 2024-01-31T00:00Z is exactly 30 days apart.
const T2 = [
  ["secondsSince 30d (this - other)", D(2024, 0, 31), "secondsSince", [D(2024, 0, 1)], 2592000],
  ["secondsUntil mirrors (other - this)", D(2024, 0, 1), "secondsUntil", [D(2024, 0, 31)], 2592000],
  ["secondsSince negative direction", D(2024, 0, 1), "secondsSince", [D(2024, 0, 31)], -2592000],
  ["minutesSince", D(2024, 0, 31), "minutesSince", [D(2024, 0, 1)], 43200],
  ["hoursSince", D(2024, 0, 31), "hoursSince", [D(2024, 0, 1)], 720],
  ["millisecondsSince", D(2024, 0, 31), "millisecondsSince", [D(2024, 0, 1)], 2592000000],
  ["daysSince", D(2024, 0, 31), "daysSince", [D(2024, 0, 1)], 30],
  ["daysUntil", D(2024, 0, 1), "daysUntil", [D(2024, 0, 31)], 30],
  ["weeksSince exact multiple", D(2024, 0, 29), "weeksSince", [D(2024, 0, 1)], 4],
];
for (const [label, base, m, args, expected] of T2) check(base[m](...args), expected, label);

// Table T3: symmetry/identity rows [label, thunk, expected]. Sign conventions cancel out.
const T3 = [
  ["x.secondsSince(y) === -y.secondsSince(x)", () => { const x = D(2024, 0, 15), y = D(2024, 1, 15); return x.secondsSince(y) === -y.secondsSince(x); }, true],
  ["x.minutesUntil(y) === y.minutesSince(x)", () => { const x = D(2024, 0, 15), y = D(2024, 2, 15); return x.minutesUntil(y) === y.minutesSince(x); }, true],
  ["x.daysUntil(y) === y.daysSince(x)", () => { const x = D(2024, 0, 1), y = D(2024, 3, 1); return x.daysUntil(y) === y.daysSince(x); }, true],
  ["d.millisecondsAgo() === -d.millisecondsFromNow()", () => { const d = new Date(Date.now() - 60000); return d.millisecondsAgo() === -d.millisecondsFromNow(); }, true],
  ["hours family returns a number", () => typeof D(2024, 0, 1).hoursSince(D(2023, 0, 1)) === "number", true],
  ["months family returns a number (semantics unpinned)", () => typeof D(2024, 0, 1).monthsSince(D(2023, 0, 1)) === "number", true],
  ["years family returns a number (semantics unpinned)", () => typeof D(2024, 0, 1).yearsSince(D(2023, 0, 1)) === "number", true],
  ["weeksFromNow returns a number", () => typeof new Date(Date.now() + 604800000).weeksFromNow() === "number", true],
];
for (const [label, thunk, expected] of T3) check(thunk(), expected, label);

// Table T4: now-relative rows with slop [label, thunk, expected true].
const T4 = [
  ["secondsAgo ~60", () => { const v = new Date(Date.now() - 60000).secondsAgo(); return v >= 59 && v <= 61; }, true],
  ["millisecondsAgo ~60000", () => { const v = new Date(Date.now() - 60000).millisecondsAgo(); return v >= 59000 && v <= 61000; }, true],
  ["hoursFromNow ~2", () => { const v = new Date(Date.now() + 7200000).hoursFromNow(); return v >= 1.99 && v <= 2.01; }, true],
  ["isToday now", () => new Date().isToday(), true],
  ["isFuture now false", () => new Date().isFuture(), false],
  ["isPast now false", () => new Date().isPast(), false],
  ["isFuture +5s", () => new Date(Date.now() + 5000).isFuture(), true],
  ["isPast -5s", () => new Date(Date.now() - 5000).isPast(), true],
];
for (const [label, thunk, expected] of T4) check(thunk(), expected, label);

// Table T5: addX arithmetic [label, base, method, n, expected date]. Mid-month months rows only:
// the doc pins NO end-of-month clamp rule, so (Jan 31 +1M) style rows are deliberately absent.
const T5 = [
  ["addDays 30 across month", D(2024, 0, 1), "addDays", 30, D(2024, 0, 31)],
  ["addDays negative", D(2024, 0, 31), "addDays", -30, D(2024, 0, 1)],
  ["addWeeks 2", D(2024, 0, 1), "addWeeks", 2, D(2024, 0, 15)],
  ["addHours", D(2024, 0, 1), "addHours", 12, D(2024, 0, 1, 12)],
  ["addMinutes", D(2024, 0, 1, 0, 30), "addMinutes", 90, D(2024, 0, 1, 2, 0)],
  ["addSeconds", D(2024, 0, 1, 0, 0, 30), "addSeconds", 45, D(2024, 0, 1, 0, 1, 15)],
  ["addMilliseconds", D(2024, 0, 1, 0, 0, 0, 500), "addMilliseconds", 1500, D(2024, 0, 1, 0, 0, 2, 0)],
  ["addMonths mid-month", D(2024, 0, 15), "addMonths", 1, D(2024, 1, 15)],
  ["addMonths across year", D(2024, 11, 15), "addMonths", 1, D(2025, 0, 15)],
  ["addMonths negative across year", D(2024, 0, 15), "addMonths", -1, D(2023, 11, 15)],
  ["addMonths 12", D(2024, 0, 15), "addMonths", 12, D(2025, 0, 15)],
  ["addYears across a leap year", D(2023, 1, 15), "addYears", 1, D(2024, 1, 15)],
];
for (const [label, base, m, num, expected] of T5) {
  const t0 = base.getTime();
  assertEq(base[m](num).getTime(), expected.getTime(), label);
  assert(base.getTime() === t0, label + " — source date untouched (addX returns a new Date)");
}

// Zone-agnostic boundary helpers: the doc pins NO timezone convention, so each boundary row accepts
// either the local-midnight or the UTC-midnight reading, but requires consistency within one zone.
function zGetters(zone) { return zone === "local" ? (o, p) => o["get" + p]() : (o, p) => o["getUTC" + p](); }
function zDateParts(o, zone) { const G = zGetters(zone); return G(o, "FullYear") + "/" + G(o, "Month") + "/" + G(o, "Date"); }
function zTime(o, zone) { const G = zGetters(zone); return [G(o, "Hours"), G(o, "Minutes"), G(o, "Seconds"), G(o, "Milliseconds")]; }
function ztEq(t, e) { return t[0] === e[0] && t[1] === e[1] && t[2] === e[2] && t[3] === e[3]; }
// All boundary predicates have signature (result, base) to match the T6 loop below.
function dayStartMatches(r, base) { return ["local", "utc"].some((z) => zDateParts(r, z) === zDateParts(base, z) && ztEq(zTime(r, z), [0, 0, 0, 0])); }
function dayEndMatches(r, base) { return ["local", "utc"].some((z) => zDateParts(r, z) === zDateParts(base, z) && ztEq(zTime(r, z), [23, 59, 59, 999])); }
function monthStartMatches(r, base) {
  return ["local", "utc"].some((z) => { const G = zGetters(z); return G(r, "Date") === 1 && zDateParts(r, z).split("/").slice(0, 2).join("/") === zDateParts(base, z).split("/").slice(0, 2).join("/") && ztEq(zTime(r, z), [0, 0, 0, 0]); });
}
function monthEndMatches(r, base, lastDay) {
  return ["local", "utc"].some((z) => { const G = zGetters(z); return G(r, "Date") === lastDay && zDateParts(r, z).split("/").slice(0, 2).join("/") === zDateParts(base, z).split("/").slice(0, 2).join("/") && ztEq(zTime(r, z), [23, 59, 59, 999]); });
}
function yearStartMatches(r, base) {
  return ["local", "utc"].some((z) => { const G = zGetters(z); return G(r, "Month") === 0 && G(r, "Date") === 1 && G(r, "FullYear") === G(base, "FullYear") && ztEq(zTime(r, z), [0, 0, 0, 0]); });
}
function yearEndMatches(r, base) {
  return ["local", "utc"].some((z) => { const G = zGetters(z); return G(r, "Month") === 11 && G(r, "Date") === 31 && G(r, "FullYear") === G(base, "FullYear") && ztEq(zTime(r, z), [23, 59, 59, 999]); });
}
// week boundaries necessarily land on a DIFFERENT day than the base, so the
// day-scoped helpers above cannot apply: require a midnight/23:59:59.999 in
// some zone with the base inside the 7-day span that ends there
function weekStartMatches(r, base) {
  return ["local", "utc"].some((z) => ztEq(zTime(r, z), [0, 0, 0, 0])) &&
         r.getTime() <= base.getTime() && base.getTime() - r.getTime() < 604800000;
}
function weekEndMatches(r, base) {
  return ["local", "utc"].some((z) => ztEq(zTime(r, z), [23, 59, 59, 999])) &&
         r.getTime() >= base.getTime() && r.getTime() - base.getTime() < 604800000;
}

// Table T6: beginning/end boundaries [label, base, expr(base)->result, pred(result, base)].
const T6 = [
  ["beginningOfDay start of same day", D(2024, 0, 31, 15, 45), (b) => b.beginningOfDay(), dayStartMatches],
  ["endOfDay end of same day", D(2024, 0, 31, 15, 45), (b) => b.endOfDay(), dayEndMatches],
  ["beginningOfMonth day 1 same month", D(2024, 0, 31, 12), (b) => b.beginningOfMonth(), monthStartMatches],
  ["beginningOfMonth Feb", D(2024, 1, 9, 12), (b) => b.beginningOfMonth(), monthStartMatches],
  ["endOfMonth Jan 31", D(2024, 0, 31, 12), (b) => b.endOfMonth(), (r, b) => monthEndMatches(r, b, 31)],
  ["endOfMonth Feb leap 29", D(2024, 1, 9, 12), (b) => b.endOfMonth(), (r, b) => monthEndMatches(r, b, 29)],
  ["endOfMonth Feb common 28", D(2023, 1, 9, 12), (b) => b.endOfMonth(), (r, b) => monthEndMatches(r, b, 28)],
  ["beginningOfYear Jan 1", D(2024, 5, 15, 12), (b) => b.beginningOfYear(), yearStartMatches],
  ["endOfYear Dec 31", D(2024, 5, 15, 12), (b) => b.endOfYear(), yearEndMatches],
  ["beginningOfWeek is a midnight that starts a span containing the date", D(2024, 0, 31, 12), (b) => b.beginningOfWeek(), weekStartMatches],
  ["endOfWeek is 23:59:59.999 closing a span containing the date", D(2024, 0, 31, 12), (b) => b.endOfWeek(), weekEndMatches],
  ["beginning/endOfWeek span is 7d minus 1ms and contains the date", D(2024, 0, 31, 12),
    (b) => { const s = b.beginningOfWeek(); const e = b.endOfWeek(); return { s: s.getTime(), e: e.getTime(), b: b.getTime() }; },
    (r) => r.e - r.s === 604799999 && r.s <= r.b && r.b <= r.e],
];
for (const [label, base, expr, pred] of T6) { n++; if (!pred(expr(base), base)) throw new Error("assertion failed (boundary): " + label); }

// Table T7: advance/rewind parity, clone, iso, format/relative structural, legacy accessors.
const T7 = [
  ["advance {days} ≡ addDays", () => D(2024, 0, 15).advance({ days: 1 }).getTime() === D(2024, 0, 15).addDays(1).getTime(), true],
  ["advance {months} ≡ addMonths", () => D(2024, 0, 15).advance({ months: 1 }).getTime() === D(2024, 0, 15).addMonths(1).getTime(), true],
  ["advance multi-key ≡ chained adds", () => { const b = D(2024, 0, 15, 12); return b.advance({ days: 1, hours: 2 }).getTime() === b.addDays(1).addHours(2).getTime(); }, true],
  ["advance {years} ≡ addYears", () => D(2023, 1, 15).advance({ years: 1 }).getTime() === D(2023, 1, 15).addYears(1).getTime(), true],
  ["rewind {days} ≡ addDays(-1)", () => D(2024, 0, 15).rewind({ days: 1 }).getTime() === D(2024, 0, 15).addDays(-1).getTime(), true],
  ["rewind {seconds} ≡ addSeconds(-90)", () => D(2024, 0, 15).rewind({ seconds: 90 }).getTime() === D(2024, 0, 15).addSeconds(-90).getTime(), true],
  ["clone is a distinct Date with the same time", () => { const x = D(2024, 0, 31, 12); const c = x.clone(); return c !== x && c.getTime() === x.getTime(); }, true],
  ["clone is isolated from source", () => { const x = D(2024, 0, 31); const c = x.clone(); c.setYear(1999); return x.getFullYear() === 2024; }, true],
  ["iso fixed value", () => D(2024, 1, 29, 13, 14, 15).iso(), "2024-02-29T13:14:15.000Z"],
  ["iso ≡ toISOString for UTC-constructed date", () => { const x = D(2024, 5, 15, 8, 0, 0); return x.iso() === x.toISOString(); }, true],
  ["format returns a string (doc lists no layouts; structural only)", () => typeof D(2024, 0, 31).format("YYYY-MM-DD"), "string"],
  ["relative returns a string (doc gives no vocabulary; structural only)", () => typeof D(2024, 0, 31).relative(), "string"],
  ["getYear is the LEGACY year-1900 accessor (doc)", () => D(2024, 5, 15).getYear(), 124],
  ["setYear returns a number and mutates the year", () => { const x = D(2024, 5, 15); const r = x.setYear(2000); return typeof r === "number" && x.getFullYear() === 2000; }, true],
  ["toGMTString ≡ toUTCString (doc: GMT alias)", () => { const x = D(2024, 5, 15, 10, 30); return x.toGMTString() === x.toUTCString(); }, true],
];
for (const [label, thunk, expected] of T7) check(thunk(), expected, label);

/* ================================================================== *
 * 4. RegExp.escape + RegExp.unicodeSets (dynajs.d.ts 7971-7979)
 * ================================================================== */

// Table R1: escape functional + literal rows [label, thunk, expected].
const R1 = [
  ["escape round-trips a.b*c", () => new RegExp(RegExp.escape("a.b*c")).test("a.b*c"), true],
  ["escape is LITERAL: . must not match another char", () => new RegExp(RegExp.escape("a.b")).test("axb"), false],
  ["escape round-trips every metachar", () => new RegExp(RegExp.escape(".[]{}()^$?*+|")).test(".[]{}()^$?*+|"), true],
  // the contract pins only "escapes a literal string for use in a RegExp" —
  // the escaping STYLE (which chars get backslashed vs \x-hex) is unpinned,
  // so plain text is verified via the re-parse, not byte equality
  ["escape of plain text still matches the literal", () => new RegExp(RegExp.escape("abc")).test("abc"), true],
  ["escape output quotes metachars with backslash", () => RegExp.escape("a.b*c").includes("\\"), true],
  ["escape of v1.2 still matches the literal", () => new RegExp(RegExp.escape("v1.2")).test("v1.2") && !new RegExp(RegExp.escape("v1.2")).test("v1x2"), true],
  ["escape $& round-trip", () => new RegExp(RegExp.escape("$&")).test("$&"), true],
  ["escape arithmetic soup round-trip", () => new RegExp(RegExp.escape("1+1=2")).test("1+1=2"), true],
];
for (const [label, thunk, expected] of R1) check(thunk(), expected, label);

// Table R2: unicodeSets is true ONLY with the v flag [label, thunk, expected].
const R2 = [
  ["unicodeSets true with v flag", () => /a/v.unicodeSets, true],
  ["unicodeSets false without flags", () => /a/.unicodeSets, false],
  ["unicodeSets false with u flag (u is not v)", () => /a/u.unicodeSets, false],
  ["unicodeSets false with unrelated flags", () => /a/gi.unicodeSets, false],
];
for (const [label, thunk, expected] of R2) check(thunk(), expected, label);

print("bb_proto_number_object_date: all tests passed (" + n + " assertions)");
