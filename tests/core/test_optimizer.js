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

function test_arith_fused_int() {
    function f() { let a = 3, b = 4; return a * b; }
    function g() { const a = 10, b = 7; return a + b; }
    function h() { var a = 9, b = 2; return a - b; }
    assert(f(), 12);
    assert(g(), 17);
    assert(h(), 7);
    function ov() { let a = 2147483647, b = 1; return a + b; }
    function ovm() { let a = 1073741824, b = 4; return a * b; }
    assert(ov(), 2147483648);
    assert(ovm(), 4294967296);
    function nz() { let a = -1, b = 0; return a * b; }
    assert(Object.is(nz(), -0), true, "neg-zero mul");
}

function test_arith_fused_float() {
    function mul() { let a = 2.5, b = 4.0; return a * b; }
    function add() { let a = 0.1, b = 0.2; return a + b; }
    function sub() { let a = 5.5, b = 2.25; return a - b; }
    function mixed() { let a = 3, b = 1.5; return a * b; }
    assert(mul(), 10);
    assert(add(), 0.30000000000000004);
    assert(sub(), 3.25);
    assert(mixed(), 4.5);
    function nan() { let a = NaN, b = 1.0; return a + b; }
    function inf() { let a = Infinity, b = 1.0; return a - b; }
    assert(nan(), NaN);
    assert(inf(), Infinity);
}

function test_arith_fused_slow() {
    function cat() { let a = "foo", b = "bar"; return a + b; }
    assert(cat(), "foobar");
    function vo() { let a = { valueOf() { return 5; } }, b = 3; return a * b; }
    assert(vo(), 15);
    function big() { let a = 10n, b = 3n; return a * b; }
    assert(big(), 30n);
    var log = [];
    (function () {
        let a = { valueOf() { log.push("a"); return 2; } };
        let b = { valueOf() { log.push("b"); return 3; } };
        assert(a - b, -1);
    })();
    assert(log.join(""), "ab");
    assert_throws(Error, function () {
        let a = { valueOf() { throw new Error("boom"); } }, b = 2;
        return a * b;
    });
}

function test_arith_slow_at_max_depth() {
    function deep(o) {
        let a = o, b = o, c = o, d = o, e = o, f = o, g = o, h = o;
        return ((a + b) * (c - d)) + ((e * f) - (g + h)) + (a * b) + (c + d) - (e - f);
    }
    assert(deep({ valueOf() { return 3; } }), 18);
    assert(deep(2), 8);
}

function test_arith_tdz() {
    assert_throws(ReferenceError, function () {
        let r = a * b;
        let a = 1, b = 2;
        return r;
    });
    assert_throws(ReferenceError, function () {
        let a = 5;
        let r = a + b;
        let b = 3;
        return r;
    });
    assert_throws(ReferenceError, function () {
        var a = 7;
        let r = a - b;
        let b = 1;
        return r;
    });
}

function test_arith_large_index() {
    var src = "let x=v10*v290, y=v290+v10, z=v290-v10; return x+','+y+','+z;";
    var decls = "";
    for (var i = 0; i < 300; i++) decls += "let v" + i + "=" + (i) + ";";
    var fn = new Function(decls + src);
    assert(fn(), "2900,300,280");
}

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
    function count_le(n) { let i = 0, c = 0; while (i <= n) { c++; i++; } return c; }
    assert(count_le(5), 6);
    function cmp(a, b) { return a < b; }
    assert(cmp(NaN, 1), false);
    assert(cmp(1, NaN), false);
    var log = [];
    var o = { valueOf() { log.push("o"); return 3; } };
    assert(o < 5, true);
    assert(log.join(""), "o");
}

function test_cmp_strict() {
    function collatz(n) {
        let x = n, s = 0;
        while (x !== 1) { x = (x & 1) ? (3 * x + 1) : (x >>> 1); s++; }
        return s;
    }
    assert(collatz(1), 0);
    assert(collatz(27), 111);
    assert(collatz(97), 118);
    function eq(a, b) { return (a === b) ? "y" : "n"; }
    assert(eq(1, 1), "y");
    assert(eq(1, 2), "n");
    assert(eq("s", "s"), "y");
    assert(eq("s", "t"), "n");
    assert(eq(null, null), "y");
    assert(eq(null, undefined), "n");
    assert(eq(NaN, NaN), "n");
    assert(eq(0, -0), "y");
    var o = {};
    assert(eq(o, o), "y");
    assert(eq(o, {}), "n");
    function dw() { let i = 0, c = 0; do { c++; i++; } while (i !== 5); return c; }
    assert(dw(), 5);
    assert(eq(1, 1.0), "y");
    assert(eq(1, 1.5), "n");
    assert(eq(1n, 1), "n");
    assert(eq(1n, 1n), "y");
}

function test_cmp_no_reorder_across_label() {
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

function keys(o) { return JSON.stringify(Object.getOwnPropertyNames(o)); }

function test_presize_basic() {
    class Vec3 { constructor(x, y, z) { this.x = x; this.y = y; this.z = z; } }
    var v = new Vec3(1, 2, 3);
    assert(keys(v), '["x","y","z"]');
    assert(v.x === 1 && v.y === 2 && v.z === 3, true);
    function F(a, b) { this.a = a; this.b = b; }
    assert(keys(new F(4, 5)), '["a","b"]');
    class W { constructor() { this.z = 1; this.a = 2; this.m = 3; } }
    assert(keys(new W()), '["z","a","m"]');
}

function test_presize_conditional_field() {
    function mk(c) { function F() { if (c) this.a = 1; this.b = 2; } return F; }
    assert(keys(new (mk(false))()), '["b"]');
    assert(keys(new (mk(true))()), '["a","b"]');
    function mk2(c) { function F() { this.x = 0; if (c) this.a = 1; this.z = 2; } return F; }
    assert(keys(new (mk2(false))()), '["x","z"]');
    assert(keys(new (mk2(true))()), '["x","a","z"]');
}

function test_presize_loc0_not_this() {
    function make() {
        var cap = {};
        function F() { var s = cap; cap; s.foo = 1; s.bar = 2; }
        return { F: F, cap: cap };
    }
    var m = make();
    var o = new m.F();
    assert(keys(o), '[]');
    assert(o.hasOwnProperty('foo'), false);
    assert(keys(m.cap), '["foo","bar"]');
}

function test_presize_proto_interference() {
    function F() { this.a = 1; this.b = 2; }
    Object.defineProperty(F.prototype, "a", { set(v) { this._a = v; }, configurable: true });
    var o = new F();
    assert(o._a, 1);
    assert(o.hasOwnProperty('a'), false);
    function G() { this.a = 1; this.b = 2; }
    G.prototype.a = 99;
    assert(new G().a, 1);
    assert(keys(new G()), '["a","b"]');
}

function test_presize_throw_and_return() {
    function F() { this.a = 1; throw new Error("x"); }
    assert_throws(Error, function () { new F(); });
    function G() { this.a = 1; return { z: 9 }; }
    assert(keys(new G()), '["z"]');
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
