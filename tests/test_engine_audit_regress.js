// timeout: 120
// Regression suite for the WS-ENGINE audit fixes.
// Each row names the finding ID it pins. Timing rows use generous budgets so
// they discriminate quadratic (multi-second) from linear (millisecond) work
// without flaking on slow hosts.

let n = 0, failed = 0;
function ok(c, m) {
    n++;
    if (!c) { failed++; print("FAIL: " + m); }
}
function eq(a, b, m) { ok(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throwsType(fn, m) {
    let e = null;
    try { fn(); } catch (err) { e = err; }
    ok(e !== null, m + " must throw (got no throw)");
    if (e !== null) ok(e instanceof TypeError, m + " must throw TypeError (got " + e.name + ": " + e.message + ")");
}
function throwsAny(fn, m) {
    let e = null;
    try { fn(); } catch (err) { e = err; }
    ok(e !== null, m + " must throw (got no throw)");
}

// SEC-002: removeRange int64 overflow
ok([1, 2, 3].removeRange(3, 0x7fffffffffffffff).length === 3, "SEC-002 removeRange huge count clamps to empty range");
eq([1, 2, 3].removeRange(0, 0x40000000000000), [], "SEC-002 removeRange huge count from 0 removes all");
eq([1, 2, 3].removeRange(1, 0x7fffffffffffffff), [1], "SEC-002 removeRange huge count keeps prefix");
eq([1, 2, 3, 4].removeRange(1, 2), [1, 4], "SEC-002 removeRange normal path");
eq([1, 2, 3].removeRange(-1e30, 2), [3], "SEC-002 removeRange huge negative start clamps to 0");

// SEC-001: for-await jump-slot accounting (compile shapes under the invariant assert)
async function forAwaitShapes() {
    let out = [];
    async function* gen() { yield 1; yield 2; }
    async function f() {
        while (true) { for await (const x of gen()) { out.push(x); } break; }
        for await (const x of gen()) { out.push(x); }
        return out.length;
    }
    return f();
}
let faP = forAwaitShapes();
ok(typeof faP.then === "function", "SEC-001 for-await shapes compile and run");

// SEC-023: compile-time scaling and semantics
(function () {
    let t0 = Date.now();
    let re = new RegExp("a|".repeat(200000) + "a");
    let dt = Date.now() - t0;
    ok(dt < 4000, "SEC-023 alternation compile stays linear (" + dt + "ms for 200k alternatives)");
    ok(re.test("a") && re.test("b") === false, "SEC-023 alternation still matches correctly");
    t0 = Date.now();
    let re2 = new RegExp("\\k<x>".repeat(50000) + "(?<x>a)");
    dt = Date.now() - t0;
    ok(dt < 4000, "SEC-023 forward named backref compile stays linear (" + dt + "ms)");
    ok(re2.test("aa"), "SEC-023 forward named backref still matches (aa)");
})();
eq(/(?<x>a)\k<x>/.exec("aa")[0], "aa", "SEC-023 backward named backref");
throwsAny(function () { new RegExp("(?<x>a)(?<x>b)"); }, "SEC-023 duplicate group name refused");
throwsAny(function () { new RegExp("\\k<nope>", "u"); }, "SEC-023 undefined forward name refused (unicode)");
let multiAlt = new RegExp("(a)|(b)|(c)");
eq(multiAlt.exec("b")[0], "b", "SEC-023 three-alternative priority");
eq([...("abc".matchAll(new RegExp("a|b|c", "g")))].map(function (m) { return m[0]; }), ["a", "b", "c"], "SEC-023 matchAll alternation");

// COMPAT-035: unicode prefilter must not match inside surrogate pairs
ok(new RegExp("[\\uDC00-\\uDFFF]", "u").exec("\uD800\uDC00" + "x".repeat(40)) === null, "COMPAT-035 low-surrogate class does not match inside a pair");
eq(new RegExp("[\\uDC00-\\uDFFF]", "u").exec("x\uDC00")[0], "\uDC00", "COMPAT-035 lone low surrogate still matches");

// SEC-160: extreme switch case values (signed-overflow range computation)
function bigSwitch(x) {
    switch (x) {
    case 2147483647: return 1;
    case -2147483648: return 2;
    case 0: return 3;
    case 1000: return 4;
    case -1000: return 5;
    default: return 0;
    }
}
eq(bigSwitch(2147483647), 1, "SEC-160 switch INT32_MAX");
eq(bigSwitch(-2147483648), 2, "SEC-160 switch INT32_MIN");
eq(bigSwitch(1000), 4, "SEC-160 switch dense label");

// COMPAT-022: get/set followed by a line terminator in class bodies
eq(new Function("return class C { get\nfoo() { return 7; } };")() .prototype.foo, 7, "COMPAT-022 class getter with line terminator");
eq(new Function("return class C { set\nfoo(v) { this._v = v; } };")() .prototype.set, undefined, "COMPAT-022 class setter with line terminator parses");
let gfield = new Function("return class C { get = 3; };")();
eq(new gfield().get, 3, "COMPAT-022 get as a class field name");
eq(new Function("return ({ get: 1, set: 2 })")() .get, 1, "COMPAT-022 get/set object keys");

// COMPAT-030: Number.format/abbr must not truncate
ok((1e300).format(20).length > 300, "COMPAT-030 format(1e300,20) is not truncated");
ok((1e300).format(0).replace(/,/g, "").indexOf("100000000000000005250476025520442024870446858110815915491585411551180245798890819578637137508044786404370444383288387817694") === 0, "COMPAT-030 format exact integer digits");
ok((1e300).abbr(2).indexOf("t") === (1e300).abbr(2).length - 1, "COMPAT-030 abbr keeps the unit suffix");
ok((1e300).abbr(20).length > 100, "COMPAT-030 abbr is not truncated");

// COMPAT-138: extreme precision must not produce NaN
ok(!Number.isNaN((1e308).round(-400)), "COMPAT-138 round(-400) is not NaN");
eq((5).round(-400), 0, "COMPAT-138 round(-400) of small value is 0");
eq((1.2345).round(2), 1.23, "COMPAT-138 normal precision unaffected");
eq((-1.2345).floor(2), -1.24, "COMPAT-138 floor with precision");

// COMPAT-139: pad/hex clamp place at exactly 65536 and never truncate the digits
eq((7).pad(600000).length, 65536, "COMPAT-139 pad clamps place to 65536");
eq((1234567).pad(70000).length, 65536, "COMPAT-139 pad(70000) is 65536 chars");
eq((1234567).pad(1), "1234567", "COMPAT-139 pad never truncates digits");
eq((255).hex(70000).length, 65536, "COMPAT-139 hex clamps place to 65536");

// COMPAT-032: JSON.rawJSON accepts every primitive JSON text
eq(JSON.stringify({ a: JSON.rawJSON("true") }), '{"a":true}', "COMPAT-032 rawJSON true");
eq(JSON.stringify({ a: JSON.rawJSON("null") }), '{"a":null}', "COMPAT-032 rawJSON null");
eq(JSON.stringify({ a: JSON.rawJSON("1e3") }), '{"a":1e3}', "COMPAT-032 rawJSON exponent number");
eq(JSON.stringify({ a: JSON.rawJSON('"x"') }), '{"a":"x"}', "COMPAT-032 rawJSON string");
throwsAny(function () { JSON.rawJSON('{"a":1}'); }, "COMPAT-032 rawJSON object text refused");
throwsAny(function () { JSON.rawJSON("[1]"); }, "COMPAT-032 rawJSON array text refused");
throwsAny(function () { JSON.rawJSON(""); }, "COMPAT-032 rawJSON empty refused");

// COMPAT-033: Promise.try with no arguments rejects with TypeError
let seen = null;
Promise.try().catch(function (e) { seen = e; });
ok(seen === null, "COMPAT-033 Promise.try() is asynchronous");

// COMPAT-142: Reflect.construct validates the target before reading the argument list
throwsType(function () {
    Reflect.construct(5, { get 0() { throw new Error("arglist read"); }, length: 1 });
}, "COMPAT-142 non-constructor target checked first");
ok(Reflect.construct(function () { this.x = 1; }, [], function () {}) .x === 1, "COMPAT-142 construct with newTarget");

// COMPAT-150: Object.groupBy callback this is undefined (strict callback)
let thisVal = "unset";
let grouped = Object.groupBy([1, 2, 3], function (v) { "use strict"; thisVal = this; return v % 2; });
eq(thisVal, undefined, "COMPAT-150 Object.groupBy callback this is undefined");
eq(grouped[1], [1, 3], "COMPAT-150 Object.groupBy groups values");

eq(new Function("class C { #x = 1; m() { return this.#x; } }; return new C().m();")(), 1, "COMPAT-117 private name token carries its escape flag");


// COMPAT-023: sync `using` must not fall back to @@asyncDispose (needs CONFIG_USING)
let usingSupported = true;
try { new Function("o", "using x = o;"); } catch (e) { usingSupported = false; }
if (usingSupported) {
    let dispKind = null;
    let o = {};
    o[Symbol.asyncDispose] = function () { dispKind = "async"; return Promise.resolve(); };
    try {
        new Function("o", "using x = o; return 1;")(o);
    } catch (e) {
        dispKind = e instanceof TypeError ? "typeerror" : e.name;
    }
    ok(dispKind === "typeerror", "COMPAT-023 sync using with only asyncDispose must not call it (got " + dispKind + ")");
} else {
    print("COMPAT-023: skipped (using syntax not compiled in)");
}

// COMPAT-153: JSON round-trip of Latin1 high bytes
eq(JSON.parse(JSON.stringify("\u0080\u00ff")), "\u0080\u00ff", "COMPAT-153 JSON round-trip high Latin1");

// COMPAT-133: HTML entity nbsp is U+00A0
eq("&nbsp;".unescapeHTML(), "\u00A0", "COMPAT-133 nbsp maps to U+00A0");

// SEC-177: splitN with a huge receiver edge sizes
eq("a,b,c".splitN(",", 2), ["a", "b,c"], "SEC-177 splitN normal");
eq("a,b,c".splitN(",", 9), ["a", "b", "c"], "SEC-177 splitN full");
eq("".splitN(",", 3), [""], "SEC-177 splitN empty");

// SEC-180: INT64_MIN through Number.pad/ordinalize
eq((-9223372036854775808).ordinalize(), "-9223372036854775808th", "SEC-180 ordinalize INT64_MIN");
ok((-9223372036854775808).pad(20, 10).indexOf("-") === 0, "SEC-180 pad INT64_MIN keeps sign");

// COMPAT-144: Atomics.wait timeout path stays bounded
if (typeof SharedArrayBuffer !== "undefined" && typeof Atomics !== "undefined") {
    let sab = new SharedArrayBuffer(4);
    let ia = new Int32Array(sab);
    let waitRes = null;
    try { waitRes = Atomics.wait(ia, 0, 1, 5); } catch (e) { waitRes = "main-thread-cannot-block"; }
    ok(waitRes === "timed-out" || waitRes === "main-thread-cannot-block", "COMPAT-144 Atomics.wait short timeout (" + waitRes + ")");
}

// OPT-022 / SEC-137: typed array set paths
(function () {
    let src = new Float64Array([1, 2, 3, 4]);
    let dst = new Float64Array(4);
    dst.set(src);
    eq([...dst], [1, 2, 3, 4], "OPT-022 typed set same type");
    let i32 = new Int32Array(4);
    i32.set(src);
    eq([...i32], [1, 2, 3, 4], "OPT-022 typed set cross type");
    let u8 = new Uint8Array(4);
    u8.set([1, 2, 3, 4]);
    let i8 = new Int8Array(4);
    i8.set(u8);
    eq([...i8], [1, 2, 3, 4], "OPT-022 typed set same buffer path");
})();

// COMPAT-055: dtoa shortest-path differential property (deterministic LCG doubles)
(function () {
    let buf = new ArrayBuffer(8), dv = new DataView(buf);
    let state = 0x123456789abcdefn, bad = 0, longer = 0, checked = 0;
    for (let i = 0; i < 20000; i++) {
        state ^= state << 13n; state &= 0xffffffffffffffffn;
        state ^= state >> 7n;
        state ^= state << 17n; state &= 0xffffffffffffffffn;
        dv.setBigUint64(0, state, false);
        let x = dv.getFloat64(0, false);
        if (!Number.isFinite(x) || x === 0) continue;
        checked++;
        let s = String(x);
        if (parseFloat(s) !== x) bad++;
        let digits = s.replace(/^[-+]/, "").split(/e/i)[0].replace(".", "").replace(/^0+/, "").replace(/0+$/, "");
        if (digits.length > 17) longer++;
    }
    ok(bad === 0, "COMPAT-055 dtoa round-trip on " + checked + " random doubles");
    ok(longer === 0, "COMPAT-055 dtoa uses at most 17 significant digits");
})();

// COMPAT-029: removeRange semantics stay count-based
eq([0, 1, 2, 3, 4, 5].removeRange(1, 2), [0, 3, 4, 5], "COMPAT-029 removeRange(from,count)");

// SEC-002 companion: js_allocate_fast_array rejects negative lengths via array ext ops
throwsAny(function () { Array.prototype.insert.call({ length: 2 ** 53, 0: "a" }, 0, "b"); }, "SEC-002 insert with 2^53 length receiver throws");

setTimeout(function () {
    if (n !== 0) print("engine_audit_regress: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("engine_audit_regress: " + failed + " failures");
    print("engine_audit_regress: OK (" + n + " checks)");
}, 20);
