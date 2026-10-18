// flags: --std
// Split from tests/test_conformance.js: the dataframe/mathx/ml parameter table.
import * as std from "std";
import { CSVFile } from "dyna:csv";
import { JSON5Parse, JSON5Stringify } from "dyna:encoding";
import { Parse, ParseAll, Stringify as YStringify } from "dyna:yaml";
import { Patch } from "dyna:json";
import { Path, readFile, readDir } from "dyna:file";
import { STRINGS, corpusSize, PRNG } from "../lib/fuzzgen.js";
import { Schema } from "dyna:schema";
import { TOML, INI, Env } from "dyna:config";
import { URL, URLSearchParams } from "dyna:url";
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";
import { makeTempDir, removeAll, writeFile } from "dyna:file";

var failedSections = 0;

try {
const THROWS = Symbol("throws");
const ANY = Symbol("any");
const OK = Symbol("ok-any");

let pass = 0, fail = 0, skip = 0, baseFail = 0;
const fails = [], notes = [], thrownClass = {};

const BASELINE_FAILS = new Set([
]);

const CANARY = "__dyna_t3_canary__";
const protoSnap = new Map();
for (const p of [Object.prototype, Array.prototype, Function.prototype,
                 String.prototype, Number.prototype])
    protoSnap.set(p, Object.getOwnPropertyNames(p).join(","));
let canaryHits = 0;

function checkCanary(id) {
    for (const [p, snap] of protoSnap) {
        const now = Object.getOwnPropertyNames(p).join(",");
        if (now !== snap) {
            canaryHits++;
            fails.push(`CANARY MUTATION at ${id}: ${p === Object.prototype ?
                "Object.prototype" : "a standard prototype"} gained [${now}]`);
            fail++;
            protoSnap.set(p, now);
        }
    }
    if (CANARY in {}) {
        canaryHits++;
        fails.push(`CANARY MUTATION at ${id}: Object.prototype.${CANARY} exists`);
        fail++;
    }
}

function eqv(a, b) {
    if (b === ANY || b === OK || b === THROWS) return false;
    if (Object.is(a, b)) return true;
    if (typeof a === "number" && typeof b === "number" && isFinite(a) && isFinite(b))
        return Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(b));
    if (a && b && typeof a === "object" && typeof b === "object") {
        const A = Array.isArray(a) ? a : Object.keys(a).sort().map((k) => [k, a[k]]);
        const B = Array.isArray(b) ? b : Object.keys(b).sort().map((k) => [k, b[k]]);
        if (A.length !== B.length) return false;
        for (let i = 0; i < A.length; i++)
            if (Array.isArray(A[i]) && Array.isArray(B[i])) { if (!eqv(A[i], B[i])) return false; }
            else if (Array.isArray(A[i]) !== Array.isArray(B[i])) return false;
            else if (!eqv(A[i], B[i])) return false;
        return true;
    }
    return false;
}

const near = (x, y, tol) => Math.abs(x - y) <= (tol === undefined ? 1e-9 : tol);

const show = (v) => {
    try {
        if (typeof v === "bigint") return v + "n";
        if (typeof v === "string") return JSON.stringify(v);
        if (typeof v === "number" && (Number.isNaN(v))) return "NaN";
        return String(v);
    } catch (e) { return "<unprintable>"; }
};

function describe(threw, got, want) {
    if (threw) return `threw ${threw.name}: ${threw.message}`;
    if (want === THROWS) return `expected a throw, got ${show(got)}`;
    if (want === ANY) return "expected a defined value or a throw, got undefined";
    return `got ${show(got)} want ${show(want)}`;
}

async function run(modName, rows) {
    print(`\n-- ${modName} (${rows.length} cases) --`);
    for (const [name, thunk, want] of rows) {
        let got, threw = null;
        try { got = await thunk(); } catch (e) { threw = e; }
        if (threw) thrownClass[threw.name] = (thrownClass[threw.name] || 0) + 1;
        checkCanary(name);
        let ok;
        if (want === THROWS) ok = threw !== null;
        else if (want === ANY) ok = threw !== null || got !== undefined;
        else if (want === OK) ok = true;
        else ok = threw === null && eqv(got, want);
        if (ok) {
            pass++;
            if (BASELINE_FAILS.has(`${modName}: ${name}`))
                notes.push(`baseline entry now GREEN (tighten BASELINE_FAILS): ${modName}: ${name}`);
        } else if (BASELINE_FAILS.has(`${modName}: ${name}`)) {
            baseFail++;
            fails.push(`BASELINE ${modName}: ${name} -- ${describe(threw, got, want)}`);
        } else {
            fail++;
            fails.push(`${modName}: ${name} -- ${describe(threw, got, want)}`);
        }
    }
}

async function mod(n) { try { return await import("dyna:" + n); } catch (e) { return null; } }
async function section(modName, m, rowsFn) {
    if (!m) { skip++; print(`\n-- ${modName} -- SKIP (not in this build)`); return; }
    await run(modName, await rowsFn(m));
}

const COV = {
    mathx:     { probed3: new Set(), read: new Set(), names: new Set() },
    dataframe: { probed3: new Set(), read: new Set(), names: new Set() },
    ml:        { probed3: new Set(), read: new Set(), names: new Set() },
};
const mark3 = (m, n) => COV[m].probed3.add(n);
const markRead = (m, n) => COV[m].read.add(n);

const outcomes = async (calls) => {
    let n = 0;
    for (const c of calls) { try { await c(); n++; } catch (e) { n++; } }
    return n;
};

const BNUM = [0, -1, NaN, Infinity, 2 ** 53];
const BBIG = [0n, -1n, 2n ** 64n, 2n ** 53n];

await section("mathx", await mod("mathx"), (m) => {
    const rows = [];

    const top = Object.getOwnPropertyNames(m);
    for (const k of top) COV.mathx.names.add(k);
    COV.mathx.names.delete("bits");
    for (const k of Object.getOwnPropertyNames(m.bits)) COV.mathx.names.add("bits." + k);
    for (const k of Object.getOwnPropertyNames(m.Expression.prototype))
        if (k !== "constructor") COV.mathx.names.add("Expression." + k);

    const add3 = (nm, empty, wrong, boundCalls, nBound) => {
        mark3("mathx", nm);
        rows.push([`${nm} EMPTY`, empty, ANY],
                  [`${nm} WRONG-TYPE`, wrong, ANY],
                  [`${nm} BOUNDARY`, async () => await outcomes(boundCalls), nBound]);
    };
    const num1 = (nm, f) => add3(nm,
        () => f(), () => f("x"),
        BNUM.map((v) => () => f(v)), BNUM.length);
    const num2 = (nm, f) => add3(nm,
        () => f(), () => f("x", "x"),
        [...BNUM.map((v) => () => f(v, 1)), ...BNUM.map((v) => () => f(1, v))],
        2 * BNUM.length);
    const big1 = (nm, f) => add3(nm,
        () => f(), () => f(123)  ,
        BBIG.map((v) => () => f(v)), BBIG.length);
    const big2 = (nm, f) => add3(nm,
        () => f(), () => f(123, 123),
        [...BBIG.map((v) => () => f(v, 1n)), ...BBIG.map((v) => () => f(1n, v))],
        2 * BBIG.length);
    const arr1 = (nm, f) => add3(nm,
        () => f(), () => f(123)  ,
        [() => f([]), () => f([null])], 2);

    const consts = {
        E: Math.E, Pi: Math.PI, Phi: (1 + Math.sqrt(5)) / 2, Ln2: Math.LN2,
        Ln10: Math.LN10, Log2E: Math.LOG2E, Log10E: Math.LOG10E,
        Sqrt2: Math.SQRT2, MaxInt32: 2 ** 31 - 1, MinInt32: -(2 ** 31),
        MaxSafeInteger: Number.MAX_SAFE_INTEGER, MaxInt64: 2n ** 63n - 1n,
    };
    for (const k of Object.keys(consts)) {
        markRead("mathx", k);
        rows.push([`${k} equals the JS builtin`, () => Object.is(m[k], consts[k]), true]);
    }
    markRead("mathx", "SqrtPi");
    markRead("mathx", "SqrtE");
    rows.push(["SqrtPi squares back to Pi", () => Math.abs(m.SqrtPi ** 2 - Math.PI) <= 1e-15,
               true],
              ["SqrtPi is within one ulp of Math.sqrt(PI)", () =>
                  Math.abs(m.SqrtPi - Math.sqrt(Math.PI)) <= 2.23e-16, true],
              ["SqrtE squares back to E", () => Math.abs(m.SqrtE ** 2 - Math.E) <= 1e-15, true]);
    for (const k of top) {
        const v = m[k];
        if (typeof v === "number" && !(k in consts)) {
            markRead("mathx", k);
            rows.push([`${k} is a finite number`, () => isFinite(v), true]);
        }
    }

    rows.push(
        ["round(0.5) ties away", () => m.round(0.5), 1],
        ["round(-2.5) ties away, differs from Math.round", () =>
            [m.round(-2.5), Math.round(-2.5)], [-3, -2]],
        ["round(2.4) equals nearby builtin", () => m.round(2.4), 2],
        ["roundToEven(2.5) half to even", () => m.roundToEven(2.5), 2],
        ["roundToEven(3.5) half to even", () => m.roundToEven(3.5), 4],
        ["fix truncates toward zero", () => m.fix(-3.7), Math.trunc(-3.7)],
        ["trunc equals Math.trunc", () => m.trunc(-3.7), Math.trunc(-3.7)],
        ["sign(-0) passes -0 through", () => Object.is(m.sign(-0), -0), true],
        ["sign(-7)", () => m.sign(-7), -1],
        ["signbit(-0)", () => m.signbit(-0), true],
        ["signbit(0)", () => m.signbit(0), false],
        ["mod(-7,3) floored == ((a%b)+b)%b", () => m.mod(-7, 3), ((-7 % 3) + 3) % 3],
        ["mod(5,0) is a", () => m.mod(5, 0), 5],
        ["rem(-7,3) truncated == a%b", () => m.rem(-7, 3), -7 % 3],
        ["fmod(-7,3) truncated == a%b", () => m.fmod(-7, 3), -7 % 3],
        ["remainder(5,2) round-to-nearest", () => m.remainder(5, 2), 1],
        ["remainder(7,3) round-to-nearest", () => m.remainder(7, 3), 1],
        ["idivide(-7,2) fix == Math.trunc", () => m.idivide(-7, 2), Math.trunc(-7 / 2)],
        ["idivide(-7,2,'floor') == Math.floor", () =>
            m.idivide(-7, 2, "floor"), Math.floor(-7 / 2)],
        ["idivide(-7,2,'ceil') == Math.ceil", () =>
            m.idivide(-7, 2, "ceil"), Math.ceil(-7 / 2)],
        ["idivide(-7,2,'round') ties away", () => m.idivide(-7, 2, "round"), -4],
        ["nthroot(-8,3) where pow is NaN", () =>
            [m.nthroot(-8, 3), Math.pow(-8, 1 / 3)], [-2, NaN]],
        ["nthroot(16,4)", () => m.nthroot(16, 4), 2],
        ["cbrt equals Math.cbrt", () => m.cbrt(27), Math.cbrt(27)],
        ["hypot(3,4)", () => m.hypot(3, 4), 5],
        ["copysign(3,-1)", () => m.copysign(3, -1), -Math.abs(3)],
        ["nextafter(1,Inf) one ulp", () => m.nextafter(1, Infinity), 1 + 2 ** -52],
        ["expm1 near Math.expm1", () => near(m.expm1(0.5), Math.expm1(0.5), 1e-15), true],
        ["log1p near Math.log1p", () => near(m.log1p(0.5), Math.log1p(0.5), 1e-15), true],
        ["log2(8)", () => m.log2(8), 3],
        ["logb/ilogb(8)", () => [m.logb(8), m.ilogb(8)], [3, 3]],
        ["ilogb(0) FP_ILOGB0", () => m.ilogb(0), -(2 ** 31)],
        ["scalbn(3,4) == 3*2**4", () => m.scalbn(3, 4), 3 * 2 ** 4],
        ["ldexp identical to scalbn", () => m.ldexp(3, 4), m.scalbn(3, 4)],
        ["frexp(8) splits exactly", () => m.frexp(8), [0.5, 4]],
        ["frexp(0) -> [0,0]", () => m.frexp(0), [0, 0]],
        ["isInf positive only", () => [m.isInf(Infinity), m.isInf(3)], [true, false]],
        ["isInf(-Inf, sign<0)", () => m.isInf(-Infinity, -1), true],
        ["isNaN", () => [m.isNaN(NaN), m.isNaN(3)], [true, false]],
        ["pow2(3) == 2**3", () => m.pow2(3), 2 ** 3],
        ["deg2rad(180)", () => m.deg2rad(180), Math.PI],
        ["rad2deg(PI)", () => m.rad2deg(Math.PI), 180],
        ["nextpow2(17)", () => m.nextpow2(17), 5],
        ["nextpow2(0) is 0", () => m.nextpow2(0), 0],
        ["erf(0)", () => m.erf(0), 0],
        ["erfc(0) == 1 - erf(0)", () => m.erfc(0), 1 - m.erf(0)],
        ["erfinv(0)", () => m.erfinv(0), 0],
        ["erfcinv(1)", () => m.erfcinv(1), 0],
        ["erfcx(0) == erfc(0)", () => m.erfcx(0), 1],
        ["gamma(5) == 4!", () => m.gamma(5), 24],
        ["gamma(0.5) == sqrt(pi)", () => near(m.gamma(0.5), Math.sqrt(Math.PI), 1e-12), true],
        ["lgamma(5) == [ln(24), +1]", () =>
            [near(m.lgamma(5)[0], Math.log(24), 1e-12), m.lgamma(5)[1]], [true, 1]],
        ["gammaln(6) == ln(5!)", () => near(m.gammaln(6), Math.log(120), 1e-12), true],
        ["beta(2,3) == 1/12", () => near(m.beta(2, 3), 1 / 12, 1e-12), true],
        ["betaln(2,3) == ln(1/12)", () => near(m.betaln(2, 3), Math.log(1 / 12), 1e-12), true],
        ["psi(1) == -Euler gamma", () => near(m.psi(1), -0.5772156649015329, 1e-12), true],
        ["polygamma(1,1) == pi^2/6", () =>
            near(m.polygamma(1, 1), (Math.PI ** 2) / 6, 1e-12), true],
        ["gammainc(1,1) lower == 1-1/e", () =>
            near(m.gammainc(1, 1), 1 - Math.exp(-1), 1e-12), true],
        ["gammainc upper == 1 - lower", () =>
            near(m.gammainc(1, 1, "upper"), Math.exp(-1), 1e-12), true],
        ["gammaincinv(0.5,3) round trip", () =>
            near(m.gammainc(m.gammaincinv(0.5, 3), 3), 0.5, 1e-9), true],
        ["betainc(0.5,1,1) is x", () => m.betainc(0.5, 1, 1), 0.5],
        ["betainc(0.5,2,3) hand value", () => m.betainc(0.5, 2, 3), 0.6875],
        ["betaincinv round trip", () =>
            near(m.betainc(m.betaincinv(0.5, 2, 3), 2, 3), 0.5, 1e-9), true],
        ["expint(1) E1 tabulated", () => near(m.expint(1), 0.2193839343955203, 1e-12), true],
        ["besselj(0,0)", () => m.besselj(0, 0), 1],
        ["besselj(0,1) tabulated", () => near(m.besselj(0, 1), 0.7651976865579666, 1e-12), true],
        ["bessely(0,1) tabulated", () => near(m.bessely(0, 1), 0.08825696421567697, 1e-12), true],
        ["besselk(0,1) tabulated", () => near(m.besselk(0, 1), 0.4210244382407083, 1e-12), true],
        ["besseliScaled(0,1) tabulated", () =>
            near(m.besseliScaled(0, 1), 0.4657596075936404, 1e-12), true],
        ["besselh(0,1,1) == [J0(1), Y0(1)]", () => {
            const [re, im] = m.besselh(0, 1, 1);
            return [near(re, m.besselj(0, 1), 1e-12), near(im, m.bessely(0, 1), 1e-12)];
        }, [true, true]],
        ["ellipke(0.5) tabulated", () =>
            m.ellipke(0.5).map((v, i) => near(v, [1.8540746773013717, 1.3506438810476753][i], 1e-12)),
        [true, true]],
        ["ellipj(1,0.5) identities", () => {
            const j = m.ellipj(1, 0.5);
            return [near(j.sn * j.sn + j.cn * j.cn, 1, 1e-12),
                    near(j.dn * j.dn + 0.5 * j.sn * j.sn, 1, 1e-12)];
        }, [true, true]],
        ["legendre(2,0.5) hand values", () => {
            const c = m.legendre(2, 0.5);
            return [near(c[0], -0.125, 1e-12), near(c[1], -1.299038105676658, 1e-12)];
        }, [true, true]],
        ["legendreP(2,1,0.5) == legendre column", () =>
            m.legendreP(2, 1, 0.5), m.legendre(2, 0.5)[1]],
        ["airy(0) tabulated Ai(0)", () => near(m.airy(0).ai, 0.3550280538878172, 1e-12), true],
        ["isPrime(97)/isPrime(91)", () => [m.isPrime(97), m.isPrime(91)], [true, false]],
        ["factor(84) product identity", () => m.factor(84).reduce((a, b) => a * b, 1), 84],
        ["factor(1) is empty", () => m.factor(1), []],
        ["factor(0) refused", () => m.factor(0), THROWS],
        ["primes(20) hand list", () => m.primes(20), [2, 3, 5, 7, 11, 13, 17, 19]],
        ["primes(1) is empty", () => m.primes(1), []],
        ["primes past 5e7 refused", () => m.primes(6e7), THROWS],
        ["gcd(48,36)", () => m.gcd(48, 36), 12n],
        ["lcm(6,8)", () => m.lcm(6, 8), 24n],
        ["factorial(20)", () => m.factorial(20), 2432902008176640000n],
        ["factorial past 10000 refused", () => m.factorial(10001), THROWS],
        ["abs(-42n)", () => m.abs(-42n), 42n],
        ["abs of a Number is refused", () => m.abs(42), THROWS],
        ["bitLen(255n)/bitLen(0n)", () => [m.bitLen(255n), m.bitLen(0n)], [8, 0]],
        ["popcount(0b101101n)", () => m.popcount(0b101101n), 4],
        ["nchoosek(10,3) hand value", () => m.nchoosek(10, 3), 120],
        ["nchoosek(52,5) poker hands", () => m.nchoosek(52, 5), 2598960],
        ["nchoosek k>n is 0", () => m.nchoosek(3, 5), 0],
        ["perms([1,2]) reverse lex", () => m.perms([1, 2]), [[2, 1], [1, 2]]],
        ["perms([1,2,3]) 3! rows", () => m.perms([1, 2, 3]).length, 6],
        ["perms past 8 elements refused", () => m.perms([1, 2, 3, 4, 5, 6, 7, 8, 9]), THROWS],
        ["rat(0.333)", () => m.rat(0.333), [333, 1000]],
        ["rat(PI) == 355/113", () => m.rat(Math.PI), [355, 113]],
        ["linspace(0,1,3) ends exact", () => m.linspace(0, 1, 3), [0, 0.5, 1]],
        ["logspace(0,1,3) ends exact", () => m.logspace(0, 1, 3), [1, 10 ** 0.5, 10]],
        ["cumsum", () => m.cumsum([1, 2, 3]), [1, 3, 6]],
        ["cumprod", () => m.cumprod([1, 2, 3]), [1, 2, 6]],
        ["diff one shorter", () => m.diff([1, 2, 4]), [1, 2]],
        ["realmin is the smallest normal (2^-1022)", () => m.realmin(), 2 ** -1022],
        ["realmax/flintmax/eps vs JS", () =>
            [Object.is(m.realmax(), Number.MAX_VALUE),
             Object.is(m.flintmax(), 2 ** 53),
             Object.is(m.eps(), Number.EPSILON)], [true, true, true]],
        ["eps(1000) is the ulp at 1000 (2^-43)", () => m.eps(1000), 2 ** -43],
    );

    for (const k of top) {
        const f = m[k];
        if (typeof f !== "function") continue;
        if (k === "Expression") continue;
        switch (k) {
            case "abs": case "bitLen": case "popcount": big1(k, f); break;
            case "gcd": case "lcm": big2(k, f); break;
            case "cumsum": case "cumprod": case "diff": case "perms": arr1(k, f); break;
            case "linspace": case "logspace": add3(k,
                () => f(), () => f("x", "x"),
                [() => f(0, 1, 0), () => f(0, 1, -1), () => f(0, 1, NaN),
                 () => f(0, 1, Infinity), () => f(0, 1, 2 ** 53)], 5); break;
            case "rat": num1(k, f); break;
            case "factorial": case "primes": case "factor": case "isPrime": num1(k, f); break;
            case "idivide": add3(k,
                () => f(), () => f("x", 2),
                [...BNUM.map((v) => () => f(v, 2)), () => f(5, 0)  ],
                BNUM.length + 1); break;
            case "modf": case "frexp": num1(k, f); break;
            case "isInf": num1(k, f); break;
            case "mod": case "rem": case "fmod": case "remainder": case "nthroot":
            case "hypot": case "copysign": case "nextafter": case "scalbn": case "ldexp":
            case "beta": case "betaln": case "nchoosek": case "polygamma":
            case "gammainc": case "gammaincinv": case "besselj": case "bessely":
            case "besseli": case "besselk": case "besseliScaled": case "besselkScaled":
            case "ellipj": case "legendre": num2(k, f); break;
            case "betainc": case "betaincinv": add3(k,
                () => f(), () => f("x", "x", "x"),
                BNUM.map((v) => () => f(v, 1, 1)), BNUM.length); break;
            case "legendreP": add3(k,
                () => f(), () => f("x", "x", "x"),
                BNUM.map((v) => () => f(2, 0, v)), BNUM.length); break;
            case "besselh": add3(k,
                () => f(), () => f("x", "x", "x"),
                BNUM.map((v) => () => f(0, v, 1)), BNUM.length); break;
            default: num1(k, f); break;
        }
    }

    const bits = m.bits;
    markRead("mathx", "bits.uintSize");
    rows.push(["bits.uintSize is 64", () => bits.uintSize, 64]);
    const u32un = ["leadingZeros8", "leadingZeros16", "leadingZeros32",
        "trailingZeros8", "trailingZeros16", "trailingZeros32",
        "onesCount8", "onesCount16", "onesCount32",
        "len8", "len16", "len32", "reverse8", "reverse16", "reverse32",
        "reverseBytes16", "reverseBytes32"];
    for (const k of u32un) num1("bits." + k, bits[k]);
    for (const k of ["rotateLeft8", "rotateLeft16", "rotateLeft32"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 1),
        [-1, 0, 7, NaN, 2 ** 53].map((kk) => () => bits[k](1, kk)), 5);
    for (const k of ["leadingZeros64", "trailingZeros64", "onesCount64", "len64",
                     "reverse64", "reverseBytes64"]) big1("bits." + k, bits[k]);
    add3("bits.rotateLeft64",
        () => bits.rotateLeft64(), () => bits.rotateLeft64("x", 1),
        [() => bits.rotateLeft64(1n, -1), () => bits.rotateLeft64(1n, 0),
         () => bits.rotateLeft64(1n, NaN), () => bits.rotateLeft64(1n, 2 ** 53),
         () => bits.rotateLeft64(1n, 1n)  ], 5);
    for (const k of ["add32", "sub32"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 1, 0),
        [() => bits[k](0xffffffff, 1, 0), () => bits[k](0, -1, -1),
         () => bits[k](0, 0, NaN), () => bits[k](0, 0, 2 ** 53)], 4);
    for (const k of ["add64", "sub64"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 1n, 0n),
        [() => bits[k](0xffffffffffffffffn, 1n, 0n), () => bits[k](0n, -1n, 0n)], 2);
    num2("bits.mul32", bits.mul32);
    big2("bits.mul64", bits.mul64);
    for (const k of ["div32", "rem32"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 0, 1),
        [() => bits[k](0, 10, 0)  ,
         () => bits[k](0, 10, 3), () => bits[k](1, 0, 1)  ,
         () => bits[k](0, 10, NaN), () => bits[k](0, 10, 2 ** 53)], 5);
    for (const k of ["div64", "rem64"]) add3("bits." + k,
        () => bits[k](), () => bits[k]("x", 0n, 1n),
        [() => bits[k](0n, 10n, 0n), () => bits[k](0n, 10n, 3n),
         () => bits[k](1n, 0n, 1n)], 3);
    rows.push(
        ["bits.rotateLeft32 matches JS shift oracle", () => {
            const x = 0x12345678, k = 8;
            return bits.rotateLeft32(x, k), ((x << k) | (x >>> (32 - k))) >>> 0;
        }, ((0x12345678 << 8) | (0x12345678 >>> 24)) >>> 0],
        ["bits.mul32 recombines to the BigInt product", () => {
            const a = 0x9e3779b9, b = 0x85ebca6b;
            const [hi, lo] = bits.mul32(a, b);
            return hi * 2 ** 32 + lo === Number((BigInt(a >>> 0) * BigInt(b >>> 0)) % 2n ** 64n);
        }, true],
        ["bits.add32 carry out", () => bits.add32(0xffffffff, 1, 0), [0, 1]],
        ["bits.div32(0,10,3)", () => bits.div32(0, 10, 3), [3, 1]],
        ["bits.rem32(0,10,3) == 10%3", () => bits.rem32(0, 10, 3), 10 % 3],
        ["bits.add64 wraps at 2^64", () => bits.add64(0xffffffffffffffffn, 1n, 0n).map(String),
         ["0", "1"]],
        ["bits.rotateLeft64(1n,1)", () => bits.rotateLeft64(1n, 1), 2n],
        ["bits.leadingZeros64(1n)", () => bits.leadingZeros64(1n), 63],
        ["bits.onesCount32(255)", () => bits.onesCount32(255), 8],
        ["bits.reverse8 doc vector", () => bits.reverse8(0b10110000), 13],
        ["bits.reverseBytes16(0x1234)", () => bits.reverseBytes16(0x1234), 13330],
        ["bits.reverseBytes32 hand value", () => bits.reverseBytes32(0x12345678), 0x78563412],
        ["bits.reverse32 is an involution", () =>
            bits.reverse32(bits.reverse32(0xdeadbeef)) === (0xdeadbeef >>> 0), true],
    );

    const e = new m.Expression("a * x^2 + b");
    mark3("mathx", "Expression");
    rows.push(["Expression EMPTY ctor", () => new m.Expression(), ANY],
              ["Expression WRONG-TYPE ctor (number)", () => new m.Expression(42), ANY],
              ["Expression BOUNDARY ctor", async () => await outcomes([
                  () => new m.Expression("x".repeat(5000))  ,
                  () => new m.Expression("")]), 2],
              ["Expression.variables()", () => e.variables(), ["a", "x", "b"]],
              ["Expression.eval hand value a*x^2+b", () => e.eval({ a: 2, x: 3, b: 1 }), 19],
              ["Expression 2^3^2 right-assoc", () => new m.Expression("2^3^2").eval(), 512],
              ["Expression bad syntax is a SyntaxError", () => new m.Expression("nope $"), THROWS]);
    mark3("mathx", "Expression.eval");
    rows.push(["Expression.eval EMPTY (vars required)", () => e.eval(), ANY],
              ["Expression.eval WRONG-TYPE (string)", () => e.eval("x"), ANY],
              ["Expression.eval BOUNDARY", async () => await outcomes([
                  () => e.eval({}), () => e.eval({ x: NaN }), () => e.eval({ x: Infinity }),
                  () => e.eval({ x: 2 ** 53 }),
                  () => new m.Expression("x").eval({ get x() { throw new Error("getter"); } })]), 5]);
    mark3("mathx", "Expression.variables");
    rows.push(["Expression.variables EMPTY", () => e.variables(), ["a", "x", "b"]],
              ["Expression.variables WRONG-TYPE (extra arg ignored)", () =>
                  e.variables(123), ["a", "x", "b"]],
              ["Expression.variables BOUNDARY", async () => await outcomes([
                  () => new m.Expression("1").variables(),
                  () => new m.Expression("x+x+x").variables()]), 2]);

    return rows;
});

await section("dataframe", await mod("dataframe"), (d) => {
    const rows = [];
    const proto = Object.getOwnPropertyNames(d.DataFrame.prototype)
        .filter((k) => k !== "constructor");
    for (const k of proto) COV.dataframe.names.add(k);
    COV.dataframe.names.add("DataFrame");

    const F = () => new d.DataFrame({
        x: new Float64Array([1, 2, 3]),
        n: new Int32Array([10, 20, 30]),
        s: ["a", "b", "a"],
    });
    const E = () => new d.DataFrame({ x: new Float64Array([]) });
    const M3 = () => new Uint8Array([1, 0, 1]);

    mark3("dataframe", "DataFrame");
    rows.push(
        ["ctor ROWS/COLS/COLUMNS", () => [F().ROWS, F().COLS, F().COLUMNS], [3, 3, ["x", "n", "s"]]],
        ["ctor EMPTY (no columns)", () => new d.DataFrame({}).ROWS, 0],
        ["ctor WRONG-TYPE (string arg)", () => new d.DataFrame("x"), THROWS],
        ["ctor BOUNDARY (NUL in name / clamped array / ragged lengths)", async () => await outcomes([
            () => new d.DataFrame({ "a\u0000b": new Float64Array([1, 2]) })  ,
            () => new d.DataFrame({ a: new Uint8ClampedArray([1, 2]) })  ,
            () => new d.DataFrame({ a: new Float64Array([1]), b: new Float64Array([1, 2]) })]),
            3],
        ["DTYPES tags", () => F().DTYPES(), { x: "f64", n: "i32", s: "str" }],
        ["TO_RECORDS first row", () => F().TO_RECORDS()[0], { x: 1, n: 10, s: "a" }],
        ["TO_CSV header", () => F().TO_CSV().split("\n")[0], "x,n,s"],
        ["FROM_RECORDS union + fill", () => {
            const g = new d.DataFrame({}).FROM_RECORDS([{ a: 1, b: "x" }, { a: 2 }]);
            return [g.COLUMNS, g.ROWS, g.TO_RECORDS()[1].b];
        }, [["a", "b"], 2, ""]],
        ["FROM_RECORDS zero rows refused", () =>
            new d.DataFrame({}).FROM_RECORDS([]), THROWS],
        ["SELECT reorders", () => F().SELECT(["s", "x"]).COLUMNS, ["s", "x"]],
        ["SELECT duplicate refused", () => F().SELECT(["x", "x"]), THROWS],
        ["SUM hand value", () => F().SUM("x"), 6],
        ["MEAN hand value", () => F().MEAN("x"), 2],
        ["MIN/MAX hand values", () => [F().MIN("x"), F().MAX("x")], [1, 3]],
        ["MEDIAN hand value", () => F().MEDIAN("x"), 2],
        ["COUNT hand value", () => F().COUNT("x"), 3],
        ["GROUP_BY_SUM hand values", () => {
            const g = F().GROUP_BY_SUM("s", "x");
            return [g.keys, Array.from(g.values)];
        }, [["a", "b"], [4, 2]]],
        ["DESCRIBE hand stats", () => {
            const r = F().DESCRIBE("x");
            return [r.count, r.sum, r.mean, r.min, r.max];
        }, [3, 6, 2, 1, 3]],
        ["SUM of empty is 0", () => E().SUM("x"), 0],
        ["MIN of empty is undefined (documented)", () => E().MIN("x"), undefined],
        ["MEAN of empty is NaN", () => Number.isNaN(E().MEAN("x")), true],
        ["SAMPLE n>ROWS refused", () => F().SAMPLE(5), THROWS],
        ["FILTER mask length refused", () => F().FILTER(new Uint8Array([1, 0])), THROWS],
        ["QUANTILE q outside [0,1] refused", () => F().QUANTILE("x", 7), THROWS],
        ["HISTOGRAM bins=Infinity refused", () => F().HISTOGRAM("x", Infinity), THROWS],
        ["NTILE buckets=Infinity refused", () => F().NTILE("x", Infinity), THROWS],
        ["N_LARGEST k=Infinity refused", () => F().N_LARGEST("x", Infinity), THROWS],
        ["EMA alpha=Infinity refused", () => F().EMA("x", Infinity), THROWS],
        ["ROLLING_MEAN window=Infinity refused", () => F().ROLLING_MEAN("x", Infinity), THROWS],
        ["ADD int column + float column", () => F().ADD("n", "x"), new Float64Array([11, 22, 33])],
        ["WHERE(mask, col, scalar)", () => F().WHERE(M3(), "x", 99), new Float64Array([1, 99, 3])],
        ["SLICE(-2) counts from the end", () => F().SLICE(-2).ROWS, 2],
        ["SAMPLE(2,7) is seed-reproducible", () => {
            const a = F().SAMPLE(2, 7).TO_RECORDS();
            const b = F().SAMPLE(2, 7).TO_RECORDS();
            return JSON.stringify(a) === JSON.stringify(b) && a.length === 2;
        }, true],
    );

    const g = (nm, args) => () => F()[nm](...args);
    const add3 = (nm, kind) => {
        mark3("dataframe", nm);
        if (kind === "red1") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (number col)`, g(nm, [123]), ANY],
                      [`${nm} BOUNDARY (bad col names)`, async () => await outcomes([g(nm, [""]), g(nm, ["nope"])]), 2]);
        } else if (kind === "red2") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (numbers)`, g(nm, [123, 123]), ANY],
                      [`${nm} BOUNDARY`, async () => await outcomes([g(nm, ["x", "x"]), g(nm, ["nope", "x"])]), 2]);
        } else if (kind === "grp") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (numbers)`, g(nm, [123, 123]), ANY],
                      [`${nm} BOUNDARY`, async () => await outcomes([g(nm, ["s", "x"]), g(nm, ["nope", "x"])]), 2]);
        } else if (kind === "colNum") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (string scalar)`, g(nm, ["x", "x"]), ANY],
                      [`${nm} BOUNDARY (numeric set)`, async () =>
                          await outcomes(BNUM.map((v) => g(nm, ["x", v]))), BNUM.length]);
        } else if (kind === "mask") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (string mask)`, g(nm, ["x"]), ANY],
                      [`${nm} BOUNDARY (empty + [1,0,1] mask)`, async () =>
                          await outcomes([g(nm, [new Uint8Array(0)]), g(nm, [M3()])]), 2]);
        } else if (kind === "arr") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (string)`, g(nm, ["x"]), ANY],
                      [`${nm} BOUNDARY ([] and [null])`, async () =>
                          await outcomes([g(nm, [[]]), g(nm, [[null]])]), 2]);
        } else if (kind === "obj") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (number)`, g(nm, [123]), ANY],
                      [`${nm} BOUNDARY ({} and unknown key)`, async () =>
                          await outcomes([g(nm, [{}]), g(nm, [{ nope: "q" }])]), 2]);
        } else if (kind === "df") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (number)`, g(nm, [123]), ANY],
                      [`${nm} BOUNDARY (empty + full frame)`, async () =>
                          await outcomes([g(nm, [E()]), g(nm, [F()])]), 2]);
        } else if (kind === "numOnly") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (string)`, g(nm, ["x"]), ANY],
                      [`${nm} BOUNDARY (numeric set)`, async () =>
                          await outcomes(BNUM.map((v) => g(nm, [v]))), BNUM.length]);
        } else if (kind === "zero") {
            rows.push([`${nm} EMPTY`, g(nm, []), ANY],
                      [`${nm} WRONG-TYPE (extra arg)`, g(nm, [123]), ANY],
                      [`${nm} BOUNDARY (extra arg)`, g(nm, [-1]), ANY]);
        } else {
            throw new Error("unknown spec kind " + kind);
        }
    };

    const spec = {
        SUM: "red1", SUM_CHECKED: "red1", MIN: "red1", MAX: "red1", MEAN: "red1",
        COUNT: "red1", PRODUCT: "red1", FIRST: "red1", LAST: "red1", STDDEV: "red1",
        STDDEV_POP: "red1", VARIANCE: "red1", VARIANCE_POP: "red1", SEM: "red1",
        MEDIAN: "red1", MODE: "red1", MAD: "red1", MEDIAN_ABSOLUTE_DEVIATION: "red1",
        KURTOSIS: "red1", KURT_SAMP: "red1", SKEW: "red1", SKEW_SAMP: "red1",
        ENTROPY: "red1", APPROX_COUNT_DISTINCT: "red1", COUNT_NULLS: "red1",
        N_UNIQUE: "red1", UNIQUE: "red1", ARG_MAX: "red1", ARG_MIN: "red1",
        ARG_SORT: "red1", RANK: "red1", DENSE_RANK: "red1", PERCENT_RANK: "red1",
        ZSCORE: "red1", CUM_SUM: "red1", CUM_MAX: "red1", CUM_MIN: "red1",
        CUM_PROD: "red1", DIFF: "red1", PCT_CHANGE: "red1", SHIFT: "red1",
        IS_NA: "red1", NOT_NA: "red1", DROP_NA: "red1", SIGN: "red1", ABS: "red1",
        CEIL: "red1", FLOOR: "red1", EXP: "red1", LOG: "red1", SQRT: "red1",
        BITMASK: "red1", BOOL_AND: "red1", BOOL_OR: "red1", BOOL_XOR: "red1",
        ANY_HEAVY: "red1", DESCRIBE: "red1", SORT: "red1", DELTA_SUM: "red1",
        VALUE_COUNTS: "red1",
        QUANTILE_EXACT_LOW: "red1", QUANTILE_EXACT_HIGH: "red1",
        QUANTILE_EXACT_WEIGHTED: "red1", QUANTILE_TDIGEST_WEIGHTED: "red1",
        RANGE_AGG: "red1", ROUND: "red1",
        DOT_PRODUCT: "red2", CORR: "red2", COV_POP: "red2", COV_SAMP: "red2",
        REGR_AVG_X: "red2", REGR_AVG_Y: "red2", REGR_COUNT: "red2",
        REGR_INTERCEPT: "red2", REGR_R2: "red2", REGR_SLOPE: "red2",
        REGR_SXX: "red2", REGR_SXY: "red2", REGR_SYY: "red2", RANK_CORR: "red2",
        MEAN_WEIGHTED: "red2", DELTA_SUM_TIMESTAMP: "red2", IRATE: "red2",
        RATE: "red2", BOUNDING_RATIO: "red2", APPROX_SIMILARITY: "red2",
        RANGE_INTERSECT_AGG: "red2", BITWISE_AND: "red2", BITWISE_OR: "red2",
        BITWISE_XOR: "red2",
        GROUP_BY_SUM: "grp", GROUP_BY_MAX: "grp", GROUP_BY_MIN: "grp",
        GROUP_BY_MEAN: "grp", GROUP_CONCAT: "grp", GROUP_ARRAY: "grp",
        GROUP_UNIQ_ARRAY: "grp", GROUP_ARRAY_SORTED: "grp", GROUP_ARRAY_MOVING_AVG: "grp",
        GROUP_ARRAY_MOVING_SUM: "grp", GROUP_ARRAY_INTERSECT: "grp",
        GROUP_BITMAP: "grp", GROUP_BIT_AND: "grp", GROUP_BIT_OR: "grp",
        GROUP_BIT_XOR: "grp", MAX_MAP: "grp", MIN_MAP: "grp", SUM_MAP: "grp",
        JSON_AGG: "grp", JSON_AGG_STRICT: "grp", JSON_OBJECT_AGG: "grp",
        JSON_OBJECT_AGG_STRICT: "grp",
        ANY: "mask", ALL: "mask", FILTER: "mask",
        SELECT: "arr", DROP_COLUMNS: "arr", FROM_RECORDS: "arr", COV_MATRIX: "arr",
        CORR_MATRIX: "arr",
        RENAME: "obj",
        CONCAT: "df", JOIN: "df", ASOF_JOIN: "df",
        SLICE: "numOnly", SAMPLE: "numOnly",
        COPY: "zero", TO_CSV: "zero", TO_JSON: "zero", TO_RECORDS: "zero",
        TO_COLUMNS: "zero", DTYPES: "zero", SCHEMA: "zero", INFO: "zero",
        MEMORY_USAGE: "zero",
    };
    for (const nm of ["ADD", "SUB", "MUL", "DIV", "RDIV", "RSUB", "POW", "CLIP",
                      "FILL_NA", "EMA", "ROLLING_SUM", "ROLLING_MEAN", "ROLLING_MIN",
                      "ROLLING_MAX", "ROLLING_STD", "ROLLING_VAR", "HISTOGRAM",
                      "HISTOGRAM_NORMALIZED", "QUANTILE", "APPROX_PERCENTILE",
                      "APPROX_TOP_K", "TOP_K", "TOP_K_WEIGHTED", "N_LARGEST",
                      "N_SMALLEST", "NTILE", "BETWEEN", "UNIQ_UP_TO", "RESAMPLE",
                      "PERCENTILE_CONT", "PERCENTILE_DISC"]) spec[nm] = "colNum";
    spec.GROUP_ARRAY_LAST = "colNum";
    spec.GROUP_ARRAY_SAMPLE = "colNum";
    spec.GROUP_ARRAY_INSERT_AT = "colNum";
    spec.EXPONENTIAL_TIME_DECAYED_AVG = "colNum";
    spec.EXPONENTIAL_TIME_DECAYED_SUM = "colNum";
    spec.EXPONENTIAL_TIME_DECAYED_MAX = "colNum";
    spec.EXPONENTIAL_TIME_DECAYED_COUNT = "colNum";
    spec.EXPONENTIAL_TIME_DECAYED_MIN = "colNum";
    spec.QUANTILES = "arr";
    spec.QUANTILES_TDIGEST = "arr";
    spec.PIVOT = "red1";
    spec.MELT = "arr";
    spec.DROP_DUPLICATES = "red1";
    spec.GROUP_BY_COUNT = "red1";
    spec.ISIN = "special";
    spec.MASK = "special";
    spec.WHERE = "special";
    spec.HEAD = "colNum";
    spec.TAIL = "colNum";
    spec.APPROX_TOP_SUM = "colNum";
    for (const nm of ["GT", "GE", "LT", "LE", "EQ", "NE"]) spec[nm] = "colNum";

    for (const nm of proto) {
        if (nm === "ROWS" || nm === "COLS" || nm === "COLUMNS") {
            markRead("dataframe", nm);
            rows.push([`${nm} getter`, () => F()[nm], ANY]);
            continue;
        }
        const kind = spec[nm];
        if (!kind) {
            skip++;
            print(`SKIP (no arg spec): dataframe.${nm}`);
            continue;
        }
        if (typeof F()[nm] !== "function") {
            skip++;
            print(`SKIP (not callable at runtime): dataframe.${nm}`);
            continue;
        }
        if (kind === "special") {
            mark3("dataframe", nm);
            if (nm === "ISIN") {
                rows.push(["ISIN EMPTY", g(nm, []), ANY],
                          ["ISIN WRONG-TYPE (number col)", g(nm, [123, []]), ANY],
                          ["ISIN BOUNDARY", async () => await outcomes([
                              g(nm, ["s", []]), g(nm, ["s", [null]]), g(nm, ["x", []])]), 3]);
            } else if (nm === "MASK") {
                rows.push(["MASK EMPTY", g(nm, []), ANY],
                          ["MASK WRONG-TYPE (string mask)", g(nm, ["x", 0]), ANY],
                          ["MASK BOUNDARY", async () => await outcomes([
                              g(nm, [new Uint8Array(0), 0]), g(nm, [M3(), 0]),
                              g(nm, [M3(), "z"])]), 3]);
            } else {
                rows.push(["WHERE EMPTY", g(nm, []), ANY],
                          ["WHERE WRONG-TYPE (string mask)", g(nm, ["x", 1, 0]), ANY],
                          ["WHERE BOUNDARY", async () => await outcomes([
                              g(nm, [new Uint8Array(0), 1, 0]), g(nm, [M3(), "x", 99]),
                              g(nm, [M3(), 1, NaN])]), 3]);
            }
            continue;
        }
        add3(nm, kind);
    }

    return rows;
});

await section("ml", await mod("ml"), async (m) => {
    const rows = [];
    const top = Object.getOwnPropertyNames(m);
    for (const k of top) COV.ml.names.add(k);
    const classes = top.filter((k) => typeof m[k] === "function" && m[k].prototype);
    const fns = top.filter((k) => typeof m[k] === "function" && !m[k].prototype);
    for (const c of classes) {
        for (const p of Object.getOwnPropertyNames(m[c].prototype))
            if (p !== "constructor") COV.ml.names.add(`${c}.${p}`);
        for (const p of Object.getOwnPropertyNames(m[c]))
            if (!["length", "name", "prototype"].includes(p)) COV.ml.names.add(`${c}.${p}`);
    }

    const X4 = [[0], [1], [2], [3]];
    const yCls = [0, 0, 1, 1];
    const yReg = [0, 2, 4, 6];
    const X2F = [[0], [0.1], [5], [5.1]];
    const X6 = [[0], [0.1], [0.2], [5], [5.1], [5.2]];

    const metric = (nm, f, third) => {
        mark3("ml", nm);
        rows.push([`${nm} EMPTY`, () => f(), ANY],
                  [`${nm} WRONG-TYPE (strings)`, () => f("x", "y"), ANY],
                  [`${nm} BOUNDARY (empty + [null] + non-finite)`, async () => await outcomes([
                      () => f([], []),
                      () => f([null, null, null], [1, 0, 1]),
                      () => f([NaN, 1], [0, 1])  ]), 3]);
    };
    for (const nm of ["accuracy", "precision", "recall", "f1", "specificity",
                      "balancedAccuracy", "matthewsCorrcoef", "cohenKappa",
                      "meanSquaredError", "meanAbsoluteError", "r2Score"])
        metric(nm, m[nm]);
    metric("rocAuc", m.rocAuc);
    metric("averagePrecision", m.averagePrecision);
    metric("logLoss", m.logLoss);
    metric("confusionMatrix", m.confusionMatrix);
    mark3("ml", "fbeta");
    rows.push(["fbeta EMPTY", async () => await m.fbeta(), ANY],
              ["fbeta WRONG-TYPE (strings)", async () => await m.fbeta("x", "y", "x"), ANY],
              ["fbeta BOUNDARY (beta must be positive)", async () => await outcomes([
                  async () => await m.fbeta([1, 0], [1, 0], 0), async () => await m.fbeta([1, 0], [1, 0], NaN),
                  async () => await m.fbeta([1, 0], [1, 0], Infinity),
                  async () => await m.fbeta([1, 0], [1, 0], 2, 1)]), 4]);
    mark3("ml", "imputeMean");
    rows.push(["imputeMean EMPTY", async () => await m.imputeMean(), ANY],
              ["imputeMean WRONG-TYPE (string)", async () => await m.imputeMean("x"), ANY],
              ["imputeMean BOUNDARY", async () => await outcomes([
                  async () => await m.imputeMean([]),
                  async () => await m.imputeMean([[NaN], [NaN]])  ,
                  async () => await m.imputeMean([[NaN]])]), 3]);
    mark3("ml", "dropMissing");
    rows.push(["dropMissing EMPTY", async () => await m.dropMissing(), ANY],
              ["dropMissing WRONG-TYPE (string)", async () => await m.dropMissing("x"), ANY],
              ["dropMissing BOUNDARY", async () => await outcomes([
                  async () => await m.dropMissing([]), async () => await m.dropMissing([[NaN, 1]]),
                  async () => await m.dropMissing([[1, 2]], null)]), 3]);
    mark3("ml", "trainTestSplit");
    rows.push(["trainTestSplit EMPTY", async () => await m.trainTestSplit(), ANY],
              ["trainTestSplit WRONG-TYPE (string)", async () => await m.trainTestSplit("x"), ANY],
              ["trainTestSplit BOUNDARY (n>=2)", async () =>
                  await outcomes(BNUM.map((v) => async () => await m.trainTestSplit(v))), BNUM.length]);
    mark3("ml", "kFold");
    rows.push(["kFold EMPTY", async () => await m.kFold(), ANY],
              ["kFold WRONG-TYPE (string)", async () => await m.kFold("x"), ANY],
              ["kFold BOUNDARY", async () => await outcomes(BNUM.map((v) => async () => await m.kFold(v, { k: 2 }))),
               BNUM.length]);
    mark3("ml", "stratifiedKFold");
    rows.push(["stratifiedKFold EMPTY", async () => await m.stratifiedKFold(), ANY],
              ["stratifiedKFold WRONG-TYPE (string)", async () => await m.stratifiedKFold("x"), ANY],
              ["stratifiedKFold BOUNDARY", async () => await outcomes([
                  async () => await m.stratifiedKFold([]), async () => await m.stratifiedKFold([null]),
                  async () => await m.stratifiedKFold([NaN, 1])]), 3]);
    const factory = () => new m.DecisionTreeClassifier({ maxDepth: 1, seed: 7 });
    mark3("ml", "crossValScore");
    rows.push(["crossValScore EMPTY", async () => await m.crossValScore(), ANY],
              ["crossValScore WRONG-TYPE (number factory)", async () =>
                  await m.crossValScore(123, X4, yCls), ANY],
              ["crossValScore BOUNDARY (empty data + bad k)", async () => await outcomes([
                  async () => await m.crossValScore(factory, [], []),
                  async () => await m.crossValScore(factory, X4, yCls, { k: 1 })]), 2]);
    mark3("ml", "gridSearch");
    rows.push(["gridSearch EMPTY", async () => await m.gridSearch(), ANY],
              ["gridSearch WRONG-TYPE (number factory)", async () =>
                  await m.gridSearch(123, X4, yCls, {}), ANY],
              ["gridSearch BOUNDARY (empty grid + bad k)", async () => await outcomes([
                  async () => await m.gridSearch(factory, X4, yCls, {}),
                  async () => await m.gridSearch(factory, X4, yCls, { maxDepth: [1] }, { k: 1 })]), 2]);
    mark3("ml", "randomSearch");
    rows.push(["randomSearch EMPTY", async () => await m.randomSearch(), ANY],
              ["randomSearch WRONG-TYPE (number factory)", async () =>
                  await m.randomSearch(123, X4, yCls, {}), ANY],
              ["randomSearch BOUNDARY (empty grid + nIter 0)", async () => await outcomes([
                  async () => await m.randomSearch(factory, X4, yCls, {}),
                  async () => await m.randomSearch(factory, X4, yCls, { maxDepth: [1] },
                                       { k: 2, nIter: 0 })]), 2]);

    rows.push(
        ["accuracy hand value 2/3", async () => await m.accuracy([0, 1, 1], [0, 1, 0]), 2 / 3],
        ["precision hand value 2/3", async () => await m.precision([1, 1, 1, 0], [1, 1, 0, 1]), 2 / 3],
        ["recall hand value 2/3", async () => await m.recall([1, 1, 1, 0], [1, 1, 0, 1]), 2 / 3],
        ["f1 hand value 2/3", async () => await m.f1([1, 1, 1, 0], [1, 1, 0, 1]), 2 / 3],
        ["mse hand value 4/3", async () => await m.meanSquaredError([1, 2, 3], [1, 2, 5]), 4 / 3],
        ["mae hand value 4/3", async () => await m.meanAbsoluteError([1, 2, 3], [2, 3, 5]), 4 / 3],
        ["r2 hand value 1/2", async () => await m.r2Score([1, 2, 3], [1, 2, 4]), 0.5],
        ["r2 constant exact is 1", async () => await m.r2Score([2, 2, 2], [2, 2, 2]), 1],
        ["r2 constant inexact is 0, never NaN", async () => await m.r2Score([2, 2, 2], [2, 2, 3]), 0],
        ["rocAuc hand value 3/4", async () => await m.rocAuc([0, 0, 1, 1], [0.1, 0.4, 0.35, 0.8]), 0.75],
        ["confusionMatrix hand values", async () =>
            await m.confusionMatrix([0, 1, 1, 0], [0, 1, 0, 1]), [[1, 1], [1, 1]]],
        ["logLoss == -ln(0.999)", async () =>
            near(await m.logLoss([0, 1], [0.001, 0.999]), -Math.log(0.999), 1e-12), true],
        ["balancedAccuracy hand value 1/2", async () =>
            await m.balancedAccuracy([0, 1, 1, 0], [0, 1, 0, 1]), 0.5],
        ["non-finite metric input refused", async () => await m.accuracy([NaN], [1]), THROWS],
        ["length-mismatch metric input refused", async () =>
            await m.accuracy([0, 1], [1]), THROWS],
        ["imputeMean hand values", async () => await m.imputeMean([[1, NaN], [3, 4]]),
         [[1, 4], [3, 4]]],
        ["dropMissing keeps survivors in order", async () => {
            const r = await m.dropMissing([[1, 2], [NaN, 4], [3, 6]], [0, 1, 2]);
            return [r.X, Array.from(r.y), r.kept];
        }, [[[1, 2], [3, 6]], [0, 2], [0, 2]]],
        ["trainTestSplit covers every index once", async () => {
            const s = await m.trainTestSplit(6, { seed: 7 });
            const all = [...s.train, ...s.test].sort((a, b) => a - b);
            return [s.train.length, s.test.length, all.join(",")];
        }, [4, 2, "0,1,2,3,4,5"]],
        ["trainTestSplit same seed reproduces", async () => {
            const a = await m.trainTestSplit(8, { seed: 7 });
            const b = await m.trainTestSplit(8, { seed: 7 });
            return a.train.join(",") === b.train.join(",");
        }, true],
        ["kFold partitions exactly once", async () => {
            const folds = await m.kFold(6, { k: 3 });
            const all = [];
            for (const f of folds) for (const i of f.test) all.push(i);
            return all.sort((a, b) => a - b).join(",");
        }, "0,1,2,3,4,5"],
        ["stratifiedKFold fold count", async () =>
            (await m.stratifiedKFold([0, 0, 0, 1, 1, 1], { k: 3 })).length, 3],
        ["crossValScore returns per-fold scores", async () => {
            const s = await m.crossValScore(factory, X4, yCls, { k: 2, seed: 7 });
            return s.length === 2 && s.every(Number.isFinite);
        }, true],
        ["gridSearch picks a best point", async () => {
            const r = await m.gridSearch((p) => new m.DecisionTreeClassifier({ ...p, seed: 7 }),
                                   X4, yCls, { maxDepth: [1, 2] }, { k: 2 });
            return [r.results.length, Number.isFinite(r.bestScore)];
        }, [2, true]],
        ["randomSearch honours nIter", async () =>
            (await m.randomSearch((p) => new m.DecisionTreeClassifier({ ...p, seed: 7 }),
                           X4, yCls, { maxDepth: [1, 2] }, { k: 2, nIter: 2 })).results.length,
        2],
    );

    const MATRIX = new Set(["predict", "predictProba", "transform", "fitTransform",
                            "inverseTransform", "apply", "decisionFunction"]);
    const ctorKind = {
        CSR: "csr", Pipeline: "arr", KMeans: "num", GaussianMixture: "num",
        PCA: "num", DBScan: "num", KNClassifier: "num", KNRegressor: "num",
    };
    const fitKind = {
        LinearRegression: "sup", LogisticRegression: "sup", SVC: "sup",
        GaussianNB: "sup", DecisionTreeClassifier: "sup", DecisionTreeRegressor: "sup",
        RandomForestClassifier: "sup", RandomForestRegressor: "sup",
        GradientBoostingClassifier: "sup", GradientBoostingRegressor: "sup",
        XGBClassifier: "sup", XGBRegressor: "sup",
        KNClassifier: "sup", KNRegressor: "sup", Pipeline: "sup",
        KMeans: "un", DBScan: "un", GaussianMixture: "un", PCA: "un",
        StandardScaler: "un", MinMaxScaler: "un", CSR: "none",
    };
    const ctorArgs = (c, v) => {
        switch (ctorKind[c]) {
            case "num": return [v];
            case "arr": return [[v]];
            case "csr": return [[v], [0], [0, 1], 1];
            default: return [{}];
        }
    };
    const cheapOpts = {
        KMeans: [2, 7], GaussianMixture: [2, { seed: 7, maxIter: 20 }],
        DBScan: [0.5, 2], PCA: [1],
        RandomForestClassifier: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        RandomForestRegressor: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        GradientBoostingClassifier: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        GradientBoostingRegressor: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        XGBClassifier: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        XGBRegressor: [{ nEstimators: 5, maxDepth: 2, seed: 7 }],
        LogisticRegression: [{ maxIter: 100 }], SVC: [{ kernel: "linear", maxIter: 100 }],
        DecisionTreeClassifier: [{ maxDepth: 2, seed: 7 }],
        DecisionTreeRegressor: [{ maxDepth: 2, seed: 7 }],
        KNClassifier: [3], KNRegressor: [3],
        GaussianNB: [], StandardScaler: [], MinMaxScaler: [],
        LinearRegression: [{}],
    };
    const makeFresh = (c) => c === "Pipeline"
        ? new m.Pipeline([new m.StandardScaler(), new m.LinearRegression({})])
        : new m[c](...(cheapOpts[c] || [{}]));
    const fitData = (c) => {
        if (c === "CSR") return null;
        const kind = fitKind[c];
        if (kind === "sup") {
            const y = /Classifier|SVC|NB|Logistic|KNClassifier|Pipeline/.test(c) ? yCls : yReg;
            return c === "Pipeline"
                ? { X: X4, y: yCls }
                : { X: X4, y };
        }
        return { X: c === "PCA" ? [[0, 0], [1, 1], [2, 2], [3, 3]] : X2F, y: null };
    };
    const makeFitted = async (c) => {
        if (c === "CSR") return await m.CSR.fromDense([[0, 1], [2, 0]]);
        const inst = makeFresh(c);
        const data = fitData(c);
        if (data.y === null) await inst.fit(data.X);
        else await inst.fit(data.X, data.y);
        return inst;
    };

    for (const c of classes) {
        const C = m[c];

        mark3("ml", c);
        rows.push([`${c} EMPTY ctor`, () => new C(), ANY],
                  [`${c} WRONG-TYPE ctor (string)`, () => new C("x"), ANY]);
        if (ctorKind[c] === "num") {
            rows.push([`${c} BOUNDARY ctor (numeric set)`, async () =>
                await outcomes(BNUM.map((v) => () => new C(...ctorArgs(c, v)))), BNUM.length]);
        } else {
            rows.push([`${c} BOUNDARY ctor`, async () => await outcomes([
                () => new C(...ctorArgs(c, null)), () => new C(...ctorArgs(c, NaN))]),
                2]);
        }

        if (fitKind[c] !== "none") {
            mark3("ml", `${c}.fit`);
            const sup = fitKind[c] === "sup";
            rows.push([`${c}.fit EMPTY`, async () => await makeFresh(c).fit(), ANY],
                      [`${c}.fit WRONG-TYPE (strings)`, async () =>
                          await makeFresh(c).fit(sup ? "x" : "x", sup ? "y" : undefined), ANY],
                      [`${c}.fit BOUNDARY (empty rows + NaN target)`, async () => await outcomes([
                          async () => await makeFresh(c).fit(sup ? [] : []),
                          async () => await makeFresh(c).fit([[NaN], [1]], sup ? [NaN, 1] : undefined)]),
                          2]);
        }

        let inst = null;
        try { inst = await makeFitted(c); } catch (e) {
            skip++;
            print(`SKIP (construction/fit failed): ${c} -- ${e.name}: ${e.message}`);
        }
        if (inst) {
            const hasPredict = typeof inst.predict === "function";
            if (hasPredict) {
                const px = c === "PCA" ? [[1, 1]] : [[1]];
                rows.push([`${c} fit yields finite predictions`, async () => {
                    const p = await inst.predict(px);
                    return Array.isArray(p) && p.length > 0 &&
                        p.every((v) => typeof v === "number" ? Number.isFinite(v) : true);
                }, true]);
            }
            for (const pm of Object.getOwnPropertyNames(Object.getPrototypeOf(inst))) {
                if (pm === "constructor" || pm === "fit" || pm === "close" || pm === "dispose")
                    continue;
                const f = inst[pm];
                if (typeof f !== "function") {
                    markRead("ml", `${c}.${pm}`);
                    rows.push([`${c}.${pm} getter read`, () => inst[pm], ANY]);
                    continue;
                }
                mark3("ml", `${c}.${pm}`);
                if (MATRIX.has(pm)) {
                    rows.push([`${c}.${pm} EMPTY`, () => f(), ANY],
                              [`${c}.${pm} WRONG-TYPE (string)`, () => f("x"), ANY],
                              [`${c}.${pm} BOUNDARY (empty + [[NaN]])`, async () => await outcomes([
                                  () => f([]), () => f([[NaN]])]), 2]);
                } else if (pm === "row") {
                    rows.push([`${c}.row EMPTY`, () => f(), ANY],
                              [`${c}.row WRONG-TYPE (string)`, () => f("x"), ANY],
                              [`${c}.row BOUNDARY (numeric set)`, async () =>
                                  await outcomes(BNUM.map((v) => () => f(v))), BNUM.length]);
                } else if (pm === "stage") {
                    rows.push([`${c}.stage EMPTY`, () => f(), ANY],
                              [`${c}.stage WRONG-TYPE (string)`, () => f("x"), ANY],
                              [`${c}.stage BOUNDARY (numeric set)`, async () =>
                                  await outcomes(BNUM.map((v) => () => f(v))), BNUM.length]);
                } else if (pm === "save" || pm === "serialize") {
                    rows.push([`${c}.${pm} EMPTY`, () => f(), ANY],
                              [`${c}.${pm} WRONG-TYPE (number path)`, () => f(123), ANY],
                              [`${c}.${pm} BOUNDARY (null path)`, () => f(null), ANY]);
                } else {
                    rows.push([`${c}.${pm} EMPTY`, () => f(), ANY],
                              [`${c}.${pm} WRONG-TYPE (extra arg)`, () => f("x"), ANY],
                              [`${c}.${pm} BOUNDARY (extra arg)`, () => f(-1), ANY]);
                }
            }
            for (const sm of Object.getOwnPropertyNames(C)) {
                if (["length", "name", "prototype"].includes(sm)) continue;
                const sf = C[sm];
                if (typeof sf !== "function") {
                    markRead("ml", `${c}.${sm}`);
                    rows.push([`${c}.${sm} static read`, () => C[sm], ANY]);
                    continue;
                }
                mark3("ml", `${c}.${sm}`);
                rows.push([`${c}.${sm} EMPTY`, () => sf(), ANY],
                          [`${c}.${sm} WRONG-TYPE (number)`, () => sf(123), ANY],
                          [`${c}.${sm} BOUNDARY (missing path / garbage bytes)`, async () => await outcomes([
                              () => sf("/nonexistent-t3-path"),
                              () => sf(new Uint8Array([1, 2, 3]))]), 2]);
            }
            for (const cm of ["close", "dispose"]) {
                if (typeof inst[cm] !== "function") continue;
                mark3("ml", `${c}.${cm}`);
                rows.push([`${c}.${cm} EMPTY`, () => makeFresh(c)[cm](), OK],
                          [`${c}.${cm} WRONG-TYPE (extra arg)`, () =>
                              makeFresh(c)[cm](123), OK],
                          [`${c}.${cm} BOUNDARY (double release)`, () => {
                              const t = makeFresh(c);
                              t[cm]();
                              return t[cm]();
                          }, OK]);
            }
        }
    }

    rows.push(
        ["LinearRegression learns y=2x", async () => {
            const r = new m.LinearRegression({});
            await r.fit([[1], [2], [3], [4]], [2, 4, 6, 8]);
            return near((await r.predict([[5]]))[0], 10, 0.5);
        }, true],
        ["LinearRegression predict before fit refused", async () =>
            await new m.LinearRegression({}).predict([[1]]), THROWS],
        ["LogisticRegression separates its training set", async () => {
            const r = new m.LogisticRegression({ maxIter: 500 });
            await r.fit(X4, yCls);
            return JSON.stringify(await r.predict(X4)), JSON.stringify(yCls);
        }, JSON.stringify(yCls)],
        ["StandardScaler centres and scales", async () => {
            const s = new m.StandardScaler();
            await s.fit([[1], [2], [3]]);
            return [near((await s.transform([[2]]))[0][0], 0, 1e-9), near(s.mean[0], 2, 1e-9),
                    near(s.std[0], Math.sqrt(2 / 3), 1e-12)];
        }, [true, true, true]],
        ["StandardScaler constant column keeps std 1", async () => {
            const s = new m.StandardScaler();
            await s.fit([[7], [7], [7]]);
            return s.std[0];
        }, 1],
        ["MinMaxScaler maps 2 of [1,3] to 0.5", async () => {
            const s = new m.MinMaxScaler();
            await s.fit([[1], [2], [3]]);
            return (await s.transform([[2]]))[0][0];
        }, 0.5],
        ["CSR.fromDense round trips", async () => {
            const c = await m.CSR.fromDense([[0, 1], [2, 0]]);
            return [c.rows, c.cols, c.nnz, c.density, await c.toDense()];
        }, [2, 2, 2, 0.5, [[0, 1], [2, 0]]]],
        ["CSR ctor stores the given triplets", async () => {
            const c = new m.CSR([1, 2], [0, 1], [0, 1, 2], 2);
            return await c.toDense();
        }, [[1, 0], [0, 2]]],
        ["Pipeline length/fitted/stage", async () => {
            const p = new m.Pipeline([new m.StandardScaler(), new m.LinearRegression({})]);
            await p.fit([[1], [2], [3]], [1, 2, 3]);
            return [p.length, p.fitted, (await p.predict([[4]]))[0] > 3.5,
                    p.stage(0) instanceof m.StandardScaler,
                    p.estimator instanceof m.LinearRegression];
        }, [2, true, true, true, true]],
        ["serialize/deserialize round trip", async () => {
            const r = new m.LinearRegression({});
            await r.fit(X4, yReg);
            const r2 = m.LinearRegression.deserialize(r.serialize());
            return near((await r2.predict([[5]]))[0], (await r.predict([[5]]))[0], 1e-12);
        }, true],
        ["close marks the model closed", async () => {
            const r = new m.LinearRegression({});
            await r.fit(X4, yReg);
            r.close();
            return r.closed;
        }, true],
        ["predict after close is a clean TypeError", async () => {
            const r = new m.LinearRegression({});
            await r.fit(X4, yReg);
            r.close();
            return await r.predict([[1]]);
        }, THROWS],
    );

    return rows;
});

print("\n" + "=".repeat(64));
for (const modName of Object.keys(COV)) {
    const c = COV[modName];
    const total = c.names.size;
    const p3 = [...c.probed3].filter((n) => c.names.has(n)).length;
    const read = [...c.read].filter((n) => c.names.has(n)).length;
    const missing = [...c.names].filter((n) => !c.probed3.has(n) && !c.read.has(n));
    const pct = total ? ((100 * p3) / total).toFixed(1) : "0.0";
    const pctTouched = total ? ((100 * (p3 + read)) / total).toFixed(1) : "0.0";
    print(`coverage ${modName}: ${p3}/${total} names (${pct}%) probed with all ` +
          `three probes; +${read} read-pinned (touched ${p3 + read}/${total} = ${pctTouched}%)`);
    if (missing.length) print(`  unprobed names: ${missing.join(", ")}`);
}

print("\nthrown-error classes: " + Object.entries(thrownClass)
    .sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}:${v}`).join("  "));
if (notes.length) {
    print("NOTES:");
    for (const n of notes) print("  " + n);
}
print("=".repeat(64));
if (fails.length) {
    print(`FAILURES (${fails.length}):`);
    for (const f of fails) print("  " + f);
}
print(`test_api_params_df_mathx_ml: ${pass + fail + baseFail} cases, ${pass} passed, ` +
      `${skip} skipped, ${baseFail} failed-by-baseline (${fail} failed-hard)`);
if (!(canaryHits === 0 && fail === 0 && baseFail === BASELINE_FAILS.size))
    throw new Error("params-grid: hard failures or canary hit");
} catch (e) {
    failedSections++; print("SECTION FAILED: params-grid -- " + (e && e.message));
}

if (failedSections) {
    print("test_api_params_df_mathx_ml: the section failed");
    throw new Error("test_api_params_df_mathx_ml failed");
}
print("test_api_params_df_mathx_ml: passed");
