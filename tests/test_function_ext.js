/* test_function_ext.js — native Function.prototype combinators.
 * Run: dynajs tests/test_function_ext.js */

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(a, b, m) {
    n++;
    const A = JSON.stringify(a), B = JSON.stringify(b);
    if (A !== B) throw new Error("eq failed: " + m + " (got " + A + ", want " + B + ")");
}

const inc = (x) => x + 1;
const dbl = (x) => x * 2;
const neg = (x) => -x;
const add = (a, b) => a + b;

/* ---- pipe / compose ---- */
eq(inc.pipe(dbl)(3), 8, "pipe: dbl(inc(3))=8");
eq(inc.compose(dbl)(3), 7, "compose: inc(dbl(3))=7");
eq(add.pipe(dbl, neg)(2, 3), -10, "pipe multi: neg(dbl(add(2,3)))");
eq(inc.pipe(dbl, inc, neg)(0), -3, "pipe chain");
eq(inc.compose(dbl, neg)(5), -9, "compose multi: inc(dbl(neg(5)))");
eq(inc.pipe()(41), 42, "pipe of one still applies this");

/* ---- both / either / complement / allPass / anyPass ---- */
const gt0 = (x) => x > 0;
const even = (x) => x % 2 === 0;
assert(gt0.both(even)(4) === true, "both true");
assert(gt0.both(even)(3) === false, "both false");
assert(gt0.either(even)(-2) === true, "either true (even)");
assert(gt0.either(even)(-3) === false, "either false");
assert(gt0.complement()(5) === false, "complement of true");
assert(gt0.complement()(-5) === true, "complement of false");
assert(gt0.allPass(even, (x) => x < 100)(4) === true, "allPass true");
assert(gt0.allPass(even)(3) === false, "allPass false");
assert(gt0.anyPass(even)(3) === true, "anyPass gt0");
assert(((x) => x > 10).anyPass((x) => x < 0)(5) === false, "anyPass false");

/* ---- juxt ---- */
eq(inc.juxt(dbl, neg)(5), [6, 10, -5], "juxt");

/* ---- converge ---- */
const sum = (a, b) => a + b;
const divide = (a, b) => a / b;
const len = (arr) => arr.length;
const total = (arr) => arr.reduce((a, b) => a + b, 0);
eq(sum.converge(total, len)([1, 2, 3, 4]), 14, "converge: sum(total,len)=10+4");
const average = divide.converge(total, len);
eq(average([1, 2, 3, 4]), 2.5, "converge average = divide(total,len)");

/* ---- useWith ---- */
const mul = (a, b) => a * b;
eq(mul.useWith(inc, dbl)(3, 4), (3 + 1) * (4 * 2), "useWith transforms each arg");

/* ---- flip ---- */
const sub = (a, b) => a - b;
eq(sub.flip()(3, 10), 7, "flip: 10-3");
eq(sub(3, 10), -7, "flip left original unchanged");

/* ---- unary / binary / nAry ---- */
const variadic = (...args) => args.length;
eq(variadic.unary()(1, 2, 3), 1, "unary passes 1 arg");
eq(variadic.binary()(1, 2, 3), 2, "binary passes 2 args");
eq(variadic.nAry(3)(1, 2, 3, 4, 5), 3, "nAry(3)");
eq(variadic.nAry(2)(1), 2, "nAry pads with undefined");
eq(parseInt.unary()("10", 2), 10, "unary parseInt ignores radix -> base 10");

/* ---- once ---- */
{
    let calls = 0;
    const f = (() => { calls++; return calls; });
    const o = f.once();
    eq(o(), 1, "once first call");
    eq(o(), 1, "once cached");
    eq(o(), 1, "once cached again");
    eq(calls, 1, "underlying called once");
}

/* ---- partial / partialRight ---- */
const three = (a, b, c) => a + "-" + b + "-" + c;
eq(three.partial("a", "b")("c"), "a-b-c", "partial left");
eq(three.partialRight("y", "z")("x"), "x-y-z", "partialRight");
eq(add.partial(10)(5), 15, "partial add");

/* ---- ifElse / when / unless ---- */
const classify = gt0.ifElse((x) => "pos", (x) => "nonpos");
eq(classify(5), "pos", "ifElse true");
eq(classify(-5), "nonpos", "ifElse false");
eq(gt0.when(dbl)(5), 10, "when: pred true -> transform");
eq(gt0.when(dbl)(-5), -5, "when: pred false -> identity");
eq(gt0.unless(neg)(5), 5, "unless: pred true -> identity");
eq(gt0.unless(neg)(-5), 5, "unless: pred false -> transform");

/* ---- tryCatch ---- */
const risky = (x) => { if (x < 0) throw new Error("neg:" + x); return x * 10; };
const safe = risky.tryCatch((err, x) => "caught " + err.message);
eq(safe(3), 30, "tryCatch success");
eq(safe(-2), "caught neg:-2", "tryCatch handles + passes args");

/* ---- curry / curryN ---- */
const add3 = (a, b, c) => a + b + c;
const c3 = add3.curry();
eq(c3(1)(2)(3), 6, "curry one at a time");
eq(c3(1, 2)(3), 6, "curry two then one");
eq(c3(1)(2, 3), 6, "curry one then two");
eq(c3(1, 2, 3), 6, "curry all at once");
assert(typeof c3(1) === "function", "curry partial is a function");
assert(typeof c3(1)(2) === "function", "curry partial 2 is a function");
const cn = ((a, b, c, d) => a + b + c + d).curryN(4);
eq(cn(1)(2)(3)(4), 10, "curryN(4)");
eq(cn(1, 2)(3, 4), 10, "curryN mixed");
/* curried functions are reusable (independent accumulation) */
{
    const base = add3.curry();
    const withA = base(100);
    eq(withA(2, 3), 105, "curry reuse 1");
    eq(withA(20, 30), 150, "curry reuse 2 (no arg bleed)");
    eq(base(1, 2, 3), 6, "curry base reusable");
}

/* ---- deep composition works; beyond the func_data cap throws RangeError ---- */
{
    const fs = [];
    for (let i = 0; i < 200; i++) fs.push((x) => x + 1);
    eq(fs[0].pipe(...fs.slice(1))(0), 200, "deep pipe 200");
    let threw = false;
    try { fs[0].pipe(...Array(400).fill((x) => x)); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "over-cap composition throws RangeError (no silent truncation)");
}

/* ---- non-enumerable on Function.prototype ---- */
{
    const d = Object.getOwnPropertyDescriptor(Function.prototype, "compose");
    assert(d && d.enumerable === false, "compose non-enumerable");
    let leaked = false;
    for (const k in inc) if (k === "compose") leaked = true;
    assert(!leaked, "combinators not enumerable via for..in");
}

console.log("test_function_ext.js OK — " + n + " assertions");
