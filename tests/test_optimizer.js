/* Implicit bytecode-optimizer correctness suite.
   Exercises the fused superinstructions the engine emits automatically:
     - ARITH shard: `<local> <*|+|-> <local>` -> OP2_{mul,add,sub}_loc_loc
     - fused compare-branch: `<rel> if_false`, `<strict_eq|strict_neq> <if_false|if_true>`
   Every case asserts the observable result, which must be identical whether or
   not the fusion fires (CONFIG_FUSED_ARITH / CONFIG_FUSED_CMP). Style follows
   tests/test_modern.js. */
"use strict";

function assert(actual, expected, message) {
    if (arguments.length === 1)
        expected = true;
    if (typeof actual === typeof expected) {
        if (actual === expected) {
            if (actual !== 0 || (1 / actual) === (1 / expected))
                return;
        }
        if (typeof actual === 'number' && isNaN(actual) && isNaN(expected))
            return;
    }
    throw Error("assertion failed: got |" + actual + "|, expected |" + expected +
                "|" + (message ? " (" + message + ")" : ""));
}

function assert_throws(expected_error, func, message) {
    var err = false;
    try { func(); } catch (e) {
        err = true;
        if (!(e instanceof expected_error))
            throw Error("unexpected exception type: " + e +
                        (message ? " (" + message + ")" : ""));
    }
    if (!err)
        throw Error("expected exception" + (message ? " (" + message + ")" : ""));
}

/* ---- ARITH shard: two-local mul/add/sub ---- */
function test_arith_fused_int() {
    // let/const/var locals all reach the fused op (checked + unchecked reads)
    function f() { let a = 3, b = 4; return a * b; }
    function g() { const a = 10, b = 7; return a + b; }
    function h() { var a = 9, b = 2; return a - b; }
    assert(f(), 12);
    assert(g(), 17);
    assert(h(), 7);
    // int overflow promotes to float, exactly like the unfused ops
    function ov() { let a = 2147483647, b = 1; return a + b; }
    function ovm() { let a = 1073741824, b = 4; return a * b; }
    assert(ov(), 2147483648);
    assert(ovm(), 4294967296);
    // -0 result from mul
    function nz() { let a = -1, b = 0; return a * b; }
    assert(Object.is(nz(), -0), true, "neg-zero mul");
}

function test_arith_fused_float() {
    function mul() { let a = 2.5, b = 4.0; return a * b; }
    function add() { let a = 0.1, b = 0.2; return a + b; }
    function sub() { let a = 5.5, b = 2.25; return a - b; }
    function mixed() { let a = 3, b = 1.5; return a * b; } // int * float
    assert(mul(), 10);
    assert(add(), 0.30000000000000004);
    assert(sub(), 3.25);
    assert(mixed(), 4.5);
    // NaN / Infinity propagate
    function nan() { let a = NaN, b = 1.0; return a + b; }
    function inf() { let a = Infinity, b = 1.0; return a - b; }
    assert(nan(), NaN);
    assert(inf(), Infinity);
}

function test_arith_fused_slow() {
    // string + string, object valueOf, bigint -> the fused slow path
    function cat() { let a = "foo", b = "bar"; return a + b; }
    assert(cat(), "foobar");
    function vo() { let a = { valueOf() { return 5; } }, b = 3; return a * b; }
    assert(vo(), 15);
    function big() { let a = 10n, b = 3n; return a * b; }
    assert(big(), 30n);
    // left-to-right evaluation / side-effect order preserved through the fusion
    var log = [];
    (function () {
        let a = { valueOf() { log.push("a"); return 2; } };
        let b = { valueOf() { log.push("b"); return 3; } };
        assert(a - b, -1);
    })();
    assert(log.join(""), "ab");
    // an operand whose valueOf throws must still throw (nothing pushed astray)
    assert_throws(Error, function () {
        let a = { valueOf() { throw new Error("boom"); } }, b = 2;
        return a * b;
    });
}

function test_arith_slow_at_max_depth() {
    // deep expression tree so a fused op sits at the function's peak stack depth
    // with a non-numeric operand forcing the slow path (regression: the slow
    // path must not overflow the operand stack by one slot).
    function deep(o) {
        let a = o, b = o, c = o, d = o, e = o, f = o, g = o, h = o;
        return ((a + b) * (c - d)) + ((e * f) - (g + h)) + (a * b) + (c + d) - (e - f);
    }
    assert(deep({ valueOf() { return 3; } }), 18);  // slow path (valueOf)
    assert(deep(2), 8);                              // fast path (plain ints)
}

function test_arith_tdz() {
    // reading a lexical local before init inside a fused op keeps the TDZ error
    assert_throws(ReferenceError, function () {
        let r = a * b;   // a, b are in TDZ
        let a = 1, b = 2;
        return r;
    });
    assert_throws(ReferenceError, function () {
        let a = 5;
        let r = a + b;   // b in TDZ; a is initialized (checked first, passes)
        let b = 3;
        return r;
    });
    // mixed: a is `var` (never uninitialized), b is lexical in TDZ -> ReferenceError on b
    assert_throws(ReferenceError, function () {
        var a = 7;
        let r = a - b;
        let b = 1;
        return r;
    });
}

function test_arith_large_index() {
    // >256 local index exercises the u16 loc2 operand path
    var src = "let x=v10*v290, y=v290+v10, z=v290-v10; return x+','+y+','+z;";
    var decls = "";
    for (var i = 0; i < 300; i++) decls += "let v" + i + "=" + (i) + ";";
    var fn = new Function(decls + src);
    assert(fn(), "2900,300,280");
}

/* ---- fused compare-branch ---- */
function test_cmp_relational() {
    function classify(a, b) {
        if (a < b) return "lt";
        if (a > b) return "gt";
        return "eq";
    }
    assert(classify(1, 2), "lt");
    assert(classify(2, 1), "gt");
    assert(classify(2, 2), "eq");
    assert(classify(1.5, 2.5), "lt");
    // <=, >= via while/for exit conditions
    function count_le(n) { let i = 0, c = 0; while (i <= n) { c++; i++; } return c; }
    assert(count_le(5), 6);
    // mixed int/float and NaN (NaN comparisons are always false)
    function cmp(a, b) { return a < b; }
    assert(cmp(NaN, 1), false);
    assert(cmp(1, NaN), false);
    // valueOf side effect on the slow relational path
    var log = [];
    var o = { valueOf() { log.push("o"); return 3; } };
    assert(o < 5, true);
    assert(log.join(""), "o");
}

function test_cmp_strict() {
    // collatz: hot loop is `while (x !== 1)` -> strict_neq + if_false
    function collatz(n) {
        let x = n, s = 0;
        while (x !== 1) { x = (x & 1) ? (3 * x + 1) : (x >>> 1); s++; }
        return s;
    }
    assert(collatz(1), 0);
    assert(collatz(27), 111);
    assert(collatz(97), 118);
    // strict_eq + if_false (ternary / if)
    function eq(a, b) { return (a === b) ? "y" : "n"; }
    assert(eq(1, 1), "y");
    assert(eq(1, 2), "n");
    assert(eq("s", "s"), "y");
    assert(eq("s", "t"), "n");
    assert(eq(null, null), "y");
    assert(eq(null, undefined), "n");
    assert(eq(NaN, NaN), "n");          // NaN !== NaN
    assert(eq(0, -0), "y");             // strict eq: +0 === -0
    var o = {};
    assert(eq(o, o), "y");              // object identity
    assert(eq(o, {}), "n");
    // strict_neq / strict_eq with if_true polarity (do-while continues while true)
    function dw() { let i = 0, c = 0; do { c++; i++; } while (i !== 5); return c; }
    assert(dw(), 5);
    // mixed int/float strict eq
    assert(eq(1, 1.0), "y");
    assert(eq(1, 1.5), "n");
    // 1n === 1 is false (different types); reaches js_strict_eq2 slow path
    assert(eq(1n, 1), "n");
    assert(eq(1n, 1n), "y");
}

function test_cmp_no_reorder_across_label() {
    // a compare whose branch target is a jump destination must NOT fuse; verify
    // the loop still behaves correctly (labelled break/continue around it).
    function f(n) {
        let sum = 0;
        outer: for (let i = 0; i < n; i++) {
            for (let j = 0; j < n; j++) {
                if (i === j) continue outer;
                if (i * j > 6) break outer;
                sum += i - j;
            }
        }
        return sum;
    }
    assert(f(5), 17);
}

/* ---- construction pre-sizing (create `this` at the ctor's final shape) ---- */
function keys(o) { return JSON.stringify(Object.getOwnPropertyNames(o)); }

function test_presize_basic() {
    class Vec3 { constructor(x, y, z) { this.x = x; this.y = y; this.z = z; } }
    var v = new Vec3(1, 2, 3);
    assert(keys(v), '["x","y","z"]');
    assert(v.x === 1 && v.y === 2 && v.z === 3, true);
    function F(a, b) { this.a = a; this.b = b; }
    assert(keys(new F(4, 5)), '["a","b"]');
    // enumeration order preserved
    class W { constructor() { this.z = 1; this.a = 2; this.m = 3; } }
    assert(keys(new W()), '["z","a","m"]');
}

function test_presize_conditional_field() {
    // a field guarded by a branch must NOT be pre-created (regression: a
    // conditional store was mis-recorded as unconditional). The condition is a
    // closure var so it reaches the scanner's branch handling.
    function mk(c) { function F() { if (c) this.a = 1; this.b = 2; } return F; }
    assert(keys(new (mk(false))()), '["b"]');
    assert(keys(new (mk(true))()), '["a","b"]');
    // unconditional prefix before a conditional field is still pre-sized safely
    function mk2(c) { function F() { this.x = 0; if (c) this.a = 1; this.z = 2; } return F; }
    assert(keys(new (mk2(false))()), '["x","z"]');
    assert(keys(new (mk2(true))()), '["x","a","z"]');
}

function test_presize_loc0_not_this() {
    // local slot 0 aliased to a non-`this` object must NOT get phantom own props
    // on the instance (regression: get_loc0 was assumed to be `this`).
    function make() {
        var cap = {};
        function F() { var s = cap; cap; s.foo = 1; s.bar = 2; }
        return { F: F, cap: cap };
    }
    var m = make();
    var o = new m.F();
    assert(keys(o), '[]');                 // `this` untouched
    assert(o.hasOwnProperty('foo'), false);
    assert(keys(m.cap), '["foo","bar"]');  // writes landed on the alias
}

function test_presize_proto_interference() {
    // a prototype accessor / non-writable for a field must disable pre-sizing
    function F() { this.a = 1; this.b = 2; }
    Object.defineProperty(F.prototype, "a", { set(v) { this._a = v; }, configurable: true });
    var o = new F();
    assert(o._a, 1);
    assert(o.hasOwnProperty('a'), false);  // setter ran, no own 'a'
    // a writable inherited data property is shadowed normally (own prop created)
    function G() { this.a = 1; this.b = 2; }
    G.prototype.a = 99;
    assert(new G().a, 1);
    assert(keys(new G()), '["a","b"]');
}

function test_presize_throw_and_return() {
    // throw mid-constructor: instance is discarded, no observable leak of fields
    function F() { this.a = 1; throw new Error("x"); }
    assert_throws(Error, function () { new F(); });
    // explicit object return overrides the pre-sized `this`
    function G() { this.a = 1; return { z: 9 }; }
    assert(keys(new G()), '["z"]');
    // Reflect.construct with a different new.target: proto comes from newTarget
    function H() { this.a = 1; }
    function Other() {}
    var o = Reflect.construct(H, [], Other);
    assert(Object.getPrototypeOf(o) === Other.prototype, true);
    assert(keys(o), '["a"]');
}

var tests = [
    test_arith_fused_int, test_arith_fused_float, test_arith_fused_slow,
    test_arith_slow_at_max_depth, test_arith_tdz, test_arith_large_index,
    test_cmp_relational, test_cmp_strict, test_cmp_no_reorder_across_label,
    test_presize_basic, test_presize_conditional_field, test_presize_loc0_not_this,
    test_presize_proto_interference, test_presize_throw_and_return,
];
for (var i = 0; i < tests.length; i++)
    tests[i]();
print("test_optimizer: all tests passed");
