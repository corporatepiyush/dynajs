/* test_object_ext.js — native static Object utility extensions
 * (non-enumerable, on the Object constructor). Run: dynajs tests/test_object_ext.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) {
    n++;
    const A = JSON.stringify(a), B = JSON.stringify(b);
    if (A !== B) throw new Error("eq failed: " + m + " (got " + A + ", want " + B + ")");
}

/* ---- type guards ---- */
assert(Object.isObject({}) === true, "isObject {}");
assert(Object.isObject(Object.create(null)) === true, "isObject null-proto");
assert(Object.isObject([]) === false, "isObject []");
assert(Object.isObject(null) === false, "isObject null");
assert(Object.isObject(new Date()) === false, "isObject Date");
assert(Object.isObject(() => 1) === false, "isObject fn");
assert(Object.isArray([]) === true, "isArray []");
assert(Object.isArray({}) === false, "isArray {}");
assert(Object.isBoolean(true) === true, "isBoolean true");
assert(Object.isBoolean(1) === false, "isBoolean 1");
assert(Object.isNumber(3.14) === true, "isNumber");
assert(Object.isNumber(NaN) === true, "isNumber NaN (still a number)");
assert(Object.isNumber("3") === false, "isNumber string");
assert(Object.isString("x") === true, "isString");
assert(Object.isString(1) === false, "isString num");
assert(Object.isFunction(() => 1) === true, "isFunction arrow");
assert(Object.isFunction(function*(){}) === true, "isFunction gen");
assert(Object.isFunction({}) === false, "isFunction obj");
assert(Object.isDate(new Date()) === true, "isDate");
assert(Object.isDate(Date.now()) === false, "isDate ms");
assert(Object.isRegExp(/x/) === true, "isRegExp");
assert(Object.isRegExp("x") === false, "isRegExp str");
assert(Object.isError(new Error("e")) === true, "isError");
assert(Object.isError(new TypeError("e")) === true, "isError sub");
assert(Object.isError({}) === false, "isError obj");
assert(Object.isSet(new Set()) === true, "isSet");
assert(Object.isSet(new Map()) === false, "isSet map");
assert(Object.isMap(new Map()) === true, "isMap");
assert(Object.isMap(new WeakMap()) === false, "isMap weakmap");
(function () { assert(Object.isArguments(arguments) === true, "isArguments"); })(1, 2);
assert(Object.isArguments([]) === false, "isArguments []");

/* wrapper objects report the primitive tag (the check is toString-tag based,
 * not typeof, so new Number(5) is a number) */
assert(Object.isNumber(new Number(5)) === true, "isNumber wrapper");
assert(Object.isString(new String("x")) === true, "isString wrapper");
assert(Object.isBoolean(new Boolean(true)) === true, "isBoolean wrapper");

/* ---- isNil / isNotNil ---- */
assert(Object.isNil(null) === true, "isNil null");
assert(Object.isNil(undefined) === true, "isNil undefined");
assert(Object.isNil(0) === false, "isNil 0");
assert(Object.isNil("") === false, "isNil empty");
assert(Object.isNotNil(0) === true, "isNotNil 0");
assert(Object.isNotNil(null) === false, "isNotNil null");

/* ---- type ---- */
eq(Object.type(null), "Null", "type null");
eq(Object.type(undefined), "Undefined", "type undefined");
eq(Object.type(1), "Number", "type num");
eq(Object.type("s"), "String", "type str");
eq(Object.type(true), "Boolean", "type bool");
eq(Object.type([]), "Array", "type arr");
eq(Object.type({}), "Object", "type obj");
eq(Object.type(/x/), "RegExp", "type re");
eq(Object.type(new Date()), "Date", "type date");
eq(Object.type(() => 1), "Function", "type fn");
eq(Object.type(new Error()), "Error", "type err");
eq(Object.type(new Set()), "Set", "type set");
eq(Object.type(new Map()), "Map", "type map");
eq(Object.type(Symbol("s")), "Symbol", "type sym");
eq(Object.type(10n), "BigInt", "type bigint");

/* ---- defaultTo ---- */
eq(Object.defaultTo(42, null), 42, "defaultTo null");
eq(Object.defaultTo(42, undefined), 42, "defaultTo undef");
eq(Object.defaultTo(42, NaN), 42, "defaultTo NaN");
eq(Object.defaultTo(42, 7), 7, "defaultTo present");
eq(Object.defaultTo(42, 0), 0, "defaultTo 0 kept");
eq(Object.defaultTo(42, ""), "", "defaultTo empty kept");
eq(Object.defaultTo(42, false), false, "defaultTo false kept");

/* ---- size / isEmpty ---- */
eq(Object.size({ a: 1, b: 2, c: 3 }), 3, "size 3");
eq(Object.size({}), 0, "size 0");
assert(Object.isEmpty({}) === true, "isEmpty {}");
assert(Object.isEmpty({ a: 1 }) === false, "isEmpty non");
/* non-enumerable keys excluded */
{
    const o = {};
    Object.defineProperty(o, "hidden", { value: 1, enumerable: false });
    o.shown = 2;
    eq(Object.size(o), 1, "size skips non-enumerable");
}

/* ---- invert / invertObj ---- */
eq(Object.invert({ a: "1", b: "2" }), { "1": "a", "2": "b" }, "invert basic");
eq(Object.invert({ a: 1, b: 2 }), { "1": "a", "2": "b" }, "invert numeric vals -> string keys");
eq(Object.invertObj({ x: "y" }), { y: "x" }, "invertObj alias");
eq(Object.invert({ a: "x", b: "x" }), { x: "b" }, "invert last wins");

/* ---- objOf ---- */
eq(Object.objOf("k", 42), { k: 42 }, "objOf");
eq(Object.objOf(3, "v"), { "3": "v" }, "objOf numeric key");

/* ---- pick / omit ---- */
eq(Object.pick(["a", "c"], { a: 1, b: 2, c: 3 }), { a: 1, c: 3 }, "pick");
eq(Object.pick(["a", "z"], { a: 1, b: 2 }), { a: 1 }, "pick missing skipped");
eq(Object.omit(["b"], { a: 1, b: 2, c: 3 }), { a: 1, c: 3 }, "omit");
eq(Object.omit([], { a: 1 }), { a: 1 }, "omit none");
eq(Object.omit(["a", "b"], { a: 1, b: 2 }), {}, "omit all");

/* ---- pickBy ---- */
eq(Object.pickBy((v) => v > 1, { a: 1, b: 2, c: 3 }), { b: 2, c: 3 }, "pickBy value");
eq(Object.pickBy((v, k) => k === "keep", { keep: 1, drop: 2 }), { keep: 1 }, "pickBy key");

/* ---- toPairs / fromPairs (round trip) ---- */
eq(Object.toPairs({ a: 1, b: 2 }), [["a", 1], ["b", 2]], "toPairs");
eq(Object.fromPairs([["a", 1], ["b", 2]]), { a: 1, b: 2 }, "fromPairs");
eq(Object.fromPairs(Object.toPairs({ x: 10, y: 20 })), { x: 10, y: 20 }, "round trip");

/* ---- assoc / dissoc (non-mutating) ---- */
{
    const o = { a: 1, b: 2 };
    eq(Object.assoc("c", 3, o), { a: 1, b: 2, c: 3 }, "assoc add");
    eq(Object.assoc("a", 9, o), { a: 9, b: 2 }, "assoc overwrite");
    eq(o, { a: 1, b: 2 }, "assoc did not mutate source");
    eq(Object.dissoc("b", o), { a: 1 }, "dissoc");
    eq(Object.dissoc("z", o), { a: 1, b: 2 }, "dissoc missing");
    eq(o, { a: 1, b: 2 }, "dissoc did not mutate source");
}

/* ---- tap ---- */
{
    let seen = null;
    const x = { v: 1 };
    const r = Object.tap((o) => { seen = o; }, x);
    assert(r === x, "tap returns identity");
    assert(seen === x, "tap ran the fn on the value");
}

/* ---- non-enumerable on the constructor, not on the prototype ---- */
{
    assert(Object.getOwnPropertyNames(Object.prototype).indexOf("pick") < 0,
           "ext methods NOT on Object.prototype");
    const d = Object.getOwnPropertyDescriptor(Object, "pick");
    assert(d && d.enumerable === false, "pick non-enumerable on ctor");
    let leaked = false;
    for (const k in Object) if (k === "pick") leaked = true;
    assert(!leaked, "pick not enumerable via for..in");
}

/* ---- getter side effects observed (enumeration reads values) ---- */
{
    let reads = 0;
    const o = { get a() { reads++; return 5; } };
    eq(Object.size(o), 1, "size of getter object");
    eq(Object.pickBy(() => true, o).a, 5, "pickBy reads getter");
    assert(reads >= 1, "getter was invoked");
}

console.log("test_object_ext.js OK — " + n + " assertions");
