/* Differential oracle for the js_atod fast path.
 *
 * The shape CLAUDE.md asks for: compile the SAME source twice, once with
 * -DJS_ATOD_NO_FASTPATH, run millions of inputs through both, diff the hashes.
 * A rewrite that can change VALUES needs an oracle about values, so this
 * hashes the exact 64 bits of every result, not its decimal rendering -- a
 * one-ULP disagreement moves the hash, a `String()` comparison could hide it.
 *
 *   ./dynajs tests/test_atod_diff.js [rounds]
 *
 * Prints one hash per input family plus a total. Build both ways and diff.
 * The families are chosen to straddle the fast path's bail conditions, since
 * a corpus that never leaves the fast domain proves nothing about the domain
 * boundary -- inject a fault in js_atod_fast and every total must move.
 */
"use strict";

var buf = new ArrayBuffer(8);
var f64 = new Float64Array(buf);
var u32 = new Uint32Array(buf);

/* order-dependent, and sensitive to every bit including the sign of zero */
function mix(h, s) {
    f64[0] = s;
    h = (Math.imul(h, 31) + u32[0]) | 0;
    h = (Math.imul(h, 31) + u32[1]) | 0;
    return h;
}

/* deterministic PRNG: results must not depend on the host or the run */
var seed = 123456789;
function rnd(n) {
    seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff;
    return seed % n;
}

var DIGITS = "0123456789";

function digits(k) {
    var s = "";
    for (var i = 0; i < k; i++) s += DIGITS[rnd(10)];
    return s;
}

var families = {};

/* plain integers of every length across the u64 and 2^53 boundaries */
families.ints = function (n) {
    var h = 17;
    for (var i = 0; i < n; i++) {
        var s = digits(1 + rnd(24));
        if (rnd(2)) s = "-" + s;
        h = mix(h, +s);
        h = mix(h, parseFloat(s));
        h = mix(h, parseInt(s));
        h = mix(h, Number(s));
    }
    return h;
};

/* fractions: straddles frac-digit counts and the |expn| <= 22 gate */
families.fracs = function (n) {
    var h = 19;
    for (var i = 0; i < n; i++) {
        var s = digits(1 + rnd(20)) + "." + digits(1 + rnd(24));
        if (rnd(2)) s = "-" + s;
        h = mix(h, +s);
        h = mix(h, parseFloat(s));
    }
    return h;
};

/* exponents on both sides of the 22 cutoff and out past the double range */
families.exps = function (n) {
    var h = 23;
    for (var i = 0; i < n; i++) {
        var s = digits(1 + rnd(18));
        if (rnd(2)) s += "." + digits(1 + rnd(18));
        s += (rnd(2) ? "e" : "E") + (rnd(3) === 0 ? "-" : (rnd(2) ? "+" : "")) +
             digits(1 + rnd(3));
        if (rnd(2)) s = "-" + s;
        h = mix(h, +s);
        h = mix(h, parseFloat(s));
    }
    return h;
};

/* leading zeros, which inflate the fast path's digit count without adding
   significance, and the tiny values that fall out of the exact domain */
families.zeros = function (n) {
    var h = 29;
    for (var i = 0; i < n; i++) {
        var z = "";
        for (var k = 0, kn = rnd(25); k < kn; k++) z += "0";
        var s = "0." + z + digits(1 + rnd(6));
        if (rnd(2)) s = "-" + s;
        h = mix(h, +s);
        h = mix(h, parseFloat(s));
    }
    return h;
};

/* the bail conditions themselves: prefixes, separators, junk, signed zero,
   'e' with nothing after it, lone dots, Infinity */
families.edges = function () {
    var cases = [
        "0", "-0", "+0", "0.0", "-0.0", "0e0", "-0e-0", "0.", "-0.", ".0", "-.0",
        ".5", "-.5", "5.", "-5.", ".", "-.", "", "-", "+", "e", "e5", "1e", "1e+",
        "1e-", "1E", "1e5", "1E5", "1e+5", "1e-5", "1e308", "1e309", "1e-308",
        "1e-323", "1e-324", "5e-324", "2.5e-324", "1e1000", "-1e1000",
        "Infinity", "-Infinity", "+Infinity", "InfinityX", "NaN", "-NaN",
        "0x10", "-0x10", "0X1f", "0o17", "0b101", "017", "08", "09", "00",
        "000.5", "1_000", "1__0", "_1", "1_", "1.5_", " 1", "1 ", " 1 ", "\t1\n",
        "1.7976931348623157e308", "1.7976931348623159e308",
        "9007199254740991", "9007199254740993", "18446744073709551615",
        "18446744073709551616", "99999999999999999999999999",
        "0.1", "0.2", "0.3", "1.005", "2.675", "8.98846567431158e307",
        "4.9406564584124654e-324", "2.2250738585072014e-308",
        "1234567890123456789", "12345678901234567890",
        "1e22", "1e23", "1e-22", "1e-23", "9007199254740992e22",
        "1.2345678901234567890123456789", "0.000000000000000000001",
        "123456789012345678901234567890e-30"
    ];
    var h = 31, i;
    for (i = 0; i < cases.length; i++) {
        var s = cases[i];
        h = mix(h, +s);
        h = mix(h, parseFloat(s));
        h = mix(h, parseInt(s));
        h = mix(h, Number(s));
        /* the parser's own numeric literal path, where it is a valid literal */
        try { h = mix(h, eval("(" + s + ")")); } catch (e) { h = (h * 31 + 7) | 0; }
    }
    /* JSON.parse reaches js_atod through a different caller */
    var jcases = ["0", "-0", "1", "-1.5", "1e5", "1e-5", "1e308", "1e-323",
                  "9007199254740993", "0.1", "123456789012345678901234567890"];
    for (i = 0; i < jcases.length; i++) {
        try { h = mix(h, JSON.parse(jcases[i])); } catch (e) { h = (h * 31 + 11) | 0; }
    }
    return h;
};

var rounds = scriptArgs[1] ? +scriptArgs[1] : 200000;
var total = 7;
var names = ["ints", "fracs", "exps", "zeros"];
for (var i = 0; i < names.length; i++) {
    var h = families[names[i]](rounds);
    print(names[i] + "\t" + h);
    total = (Math.imul(total, 31) + h) | 0;
}
var he = families.edges();
print("edges\t" + he);
total = (Math.imul(total, 31) + he) | 0;
print("TOTAL\t" + total);
