/* test_number_ext.js — native Number.prototype extension methods
 * (unprefixed, non-enumerable). Core-engine builtins present in every build.
 * Run: dynajs tests/test_number_ext.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) {
    n++;
    if (!(a === b || (a !== a && b !== b)))   /* NaN === NaN handled */
        throw new Error("eq failed: " + m + " (got " + a + ", want " + b + ")");
}
function close(a, b, m) {
    n++;
    if (Math.abs(a - b) > 1e-9)
        throw new Error("close failed: " + m + " (got " + a + ", want " + b + ")");
}

/* ---- unary numeric ops ---- */
eq((5).negate(), -5, "negate");
eq((-3).negate(), 3, "negate neg");
eq((5).inc(), 6, "inc");
eq((5).dec(), 4, "dec");
eq((-1).inc(), 0, "inc to 0");
close((4).sqrt(), 2, "sqrt");
close((0).exp(), 1, "exp0");
close((3).abs(), 3, "abs pos");
close((-7).abs(), 7, "abs neg");
close((0).sin(), 0, "sin0");
close((0).cos(), 1, "cos0");
close((0).tan(), 0, "tan0");
close((0).asin(), 0, "asin0");
close((1).acos(), 0, "acos1");
close((0).atan(), 0, "atan0");
close(Math.PI.abs(), Math.PI, "abs pi");

/* differential vs Math for the delegating wrappers */
for (const x of [0, 0.5, 1, 2, -1, 3.7, 100, 0.001]) {
    close(x.abs(), Math.abs(x), "abs " + x);
    close(x.sqrt(), Math.sqrt(x), "sqrt " + x);
    close(x.exp(), Math.exp(x), "exp " + x);
    close(x.sin(), Math.sin(x), "sin " + x);
    close(x.cos(), Math.cos(x), "cos " + x);
    close(x.tan(), Math.tan(x), "tan " + x);
    close(x.atan(), Math.atan(x), "atan " + x);
}

/* ---- binary arithmetic ---- */
eq((2).add(3), 5, "add");
eq((10).subtract(4), 6, "subtract");
eq((6).multiply(7), 42, "multiply");
eq((20).divide(5), 4, "divide");
eq((17).modulo(5), 2, "modulo");
eq((-17).modulo(5), -2, "modulo neg (JS %)");
eq((2).pow(10), 1024, "pow");
close((9).pow(0.5), 3, "pow frac");
/* differential vs operators */
for (const a of [0, 1, -3, 7.5]) for (const b of [1, 2, -4, 0.5]) {
    close(a.add(b), a + b, "add " + a + "," + b);
    close(a.subtract(b), a - b, "sub " + a + "," + b);
    close(a.multiply(b), a * b, "mul " + a + "," + b);
    close(a.divide(b), a / b, "div " + a + "," + b);
    eq(a.modulo(b), a % b, "mod " + a + "," + b);
    close(a.pow(b), Math.pow(a, b), "pow " + a + "," + b);
}

/* ---- relational ---- */
assert((5).gt(3) === true, "gt true");
assert((3).gt(5) === false, "gt false");
assert((5).gte(5) === true, "gte eq");
assert((5).lt(9) === true, "lt");
assert((5).lte(5) === true, "lte eq");
assert((NaN).gt(1) === false, "NaN gt is false");
assert((NaN).lt(1) === false, "NaN lt is false");

/* ---- integer / parity / divisibility predicates ---- */
assert((4).isInteger() === true, "isInteger 4");
assert((4.5).isInteger() === false, "isInteger 4.5");
assert((Infinity).isInteger() === false, "isInteger Inf");
assert((NaN).isInteger() === false, "isInteger NaN");
assert((3).isOdd() === true, "isOdd 3");
assert((-3).isOdd() === true, "isOdd -3");
assert((4).isOdd() === false, "isOdd 4");
assert((3.5).isOdd() === false, "isOdd non-int");
assert((4).isEven() === true, "isEven 4");
assert((0).isEven() === true, "isEven 0");
assert((3).isEven() === false, "isEven 3");
assert((10).isMultipleOf(5) === true, "isMultipleOf 10/5");
assert((10).isMultipleOf(3) === false, "isMultipleOf 10/3");
assert((10).isMultipleOf(0) === false, "isMultipleOf /0");
/* differential vs Number.isInteger */
for (const x of [0, 1, -1, 2.5, 1e21, -0, 7, NaN, Infinity])
    assert(x.isInteger() === Number.isInteger(x), "isInteger diff " + x);

/* ---- mathMod (Euclidean modulo: result always non-negative, unlike %) ---- */
eq((-17).mathMod(5), 3, "mathMod -17,5");
eq((17).mathMod(5), 2, "mathMod 17,5");
eq((17.2).mathMod(5), NaN, "mathMod non-int m");
eq((17).mathMod(0), NaN, "mathMod p<1");
eq((17).mathMod(-5), NaN, "mathMod neg p");
function mathModRef(m, p) {
    if (!Number.isInteger(m) || !Number.isInteger(p) || p < 1) return NaN;
    return ((m % p) + p) % p;
}
for (const m of [-17, 0, 5, 100, 3.3]) for (const p of [1, 5, 0, -2, 7]) {
    const got = m.mathMod(p), ref = mathModRef(m, p);
    eq(got, ref, "mathMod diff " + m + "," + p);
}

/* ---- clamp ---- */
eq((5).clamp(1, 10), 5, "clamp in");
eq((0).clamp(1, 10), 1, "clamp lo");
eq((99).clamp(1, 10), 10, "clamp hi");
eq((-5).clamp(-3, 3), -3, "clamp neg lo");

/* ---- log (change-of-base; no argument = natural log) ---- */
close((Math.E).log(), 1, "log e");
close((8).log(2), 3, "log2 8");
close((1000).log(10), 3, "log10 1000");
close((100).log(), Math.log(100), "log default");

/* ---- precision round/ceil/floor ---- */
eq((3.14159).round(2), 3.14, "round 2dp");
eq((3.14159).round(), 3, "round 0dp");
eq((3.7).round(), 4, "round up");
eq((1234).round(-2), 1200, "round -2");
eq((3.14159).ceil(2), 3.15, "ceil 2dp");
eq((3.001).ceil(2), 3.01, "ceil 2dp up");
eq((3.999).floor(2), 3.99, "floor 2dp");
eq((3.7).floor(), 3, "floor 0dp");
eq((3.2).ceil(), 4, "ceil 0dp");
/* differential vs the reference formula */
function roundRef(x, p, fn) { const f = Math.pow(10, p); return fn(x * f) / f; }
for (const x of [3.14159, 0.5, -2.675, 12345.678, 0]) for (const p of [0, 1, 2, 3, -1, -2]) {
    close(x.round(p), roundRef(x, p, Math.round), "round diff " + x + "," + p);
    close(x.ceil(p),  roundRef(x, p, Math.ceil),  "ceil diff " + x + "," + p);
    close(x.floor(p), roundRef(x, p, Math.floor), "floor diff " + x + "," + p);
}

/* ---- chr ---- */
eq((65).chr(), "A", "chr 65");
eq((97).chr(), "a", "chr 97");
eq((0x263A).chr(), "☺", "chr smiley");
for (const c of [32, 48, 65, 97, 122, 0x263A, 0x4E2D])
    eq(c.chr(), String.fromCharCode(c), "chr diff " + c);

/* ---- works on Number wrapper objects ---- */
eq(new Number(7).inc(), 8, "wrapper inc");
eq(new Number(3).add(4), 7, "wrapper add");
assert(new Number(4).isEven() === true, "wrapper isEven");

/* ---- reentrancy: a {valueOf}-armed argument must not corrupt the result ---- */
let side = 0;
const evil = { valueOf() { side++; return 3; } };
eq((10).add(evil), 13, "reentrant add arg");
eq((10).clamp(evil, 100), 10, "reentrant clamp arg");
assert(side === 2, "valueOf ran exactly twice");

/* ---- non-enumerable: must not show in for..in / Object.keys ---- */
{
    let found = false;
    for (const k in (5)) if (k === "inc" || k === "add") found = true;
    assert(!found, "ext methods are non-enumerable");
    assert(Object.getOwnPropertyNames(Number.prototype).indexOf("add") >= 0,
           "add is an own prop of Number.prototype");
    const d = Object.getOwnPropertyDescriptor(Number.prototype, "add");
    assert(d && d.enumerable === false, "add descriptor non-enumerable");
    assert(d.writable === true && d.configurable === true, "add descriptor writable+configurable");
}

console.log("test_number_ext.js OK — " + n + " assertions");
