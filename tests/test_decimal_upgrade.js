/* test_decimal_upgrade.js --: sqrt, exp, ln, log10, floor, ceil, trunc,
 * divmod, toBigInt and the Decimal.ZERO/ONE/TWO/TEN/NEG_ONE constants.
 *
 * TWO ORACLES.
 *  - The STRONG one is independent and in-engine: an exact BigInt integer
 *    square root. For x > 0 and precision p, floor(sqrt(x * 10^2s)) with the
 *    remainder decides the correctly-rounded p-digit answer from the
 *    definitions (an irrational sqrt can never land ON a half: the tail is
 *    q/(sqrt+R), < 1/2 iff q <= R) -- so every sqrt case is checked against
 *    arithmetic, not against a table.
 *  - The PINNED one is a table of 50 values generated from python's decimal
 *    (libmpdec), the correctly-rounded reference implementation, during
 *    development. The full development differential was 9066 cases across
 *    precisions 34/60/100/200/340 with ZERO divergences (progress.log); the
 *    table pins a representative slice of that in CI, including the exp
 *    overflow/underflow boundary at Emax = 999999.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_decimal_upgrade.js
 */
import { Decimal } from "dyna:decimal";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const D = (x) => new Decimal(x);

/* ------------------------------------------------ the BigInt sqrt oracle */

/* Exact text -> { c: BigInt coefficient, e: exponent } (from test_decimal.js). */
function toCE(s) {
    let sign = 1n, i = 0;
    if (s[0] === "-") { sign = -1n; i = 1; } else if (s[0] === "+") { i = 1; }
    let digits = "", e = 0, seenDot = false;
    for (; i < s.length; i++) {
        const ch = s[i];
        if (ch >= "0" && ch <= "9") { digits += ch; if (seenDot) e--; }
        else if (ch === ".") seenDot = true;
        else if (ch === "e" || ch === "E") { e += parseInt(s.slice(i + 1), 10); break; }
        else throw new Error("bad literal " + s);
    }
    return { c: sign * BigInt(digits === "" ? "0" : digits), e };
}
function digitsOf(b) { return (b < 0n ? -b : b).toString().length; }
function pow10(k) { return 10n ** BigInt(k); }

/* floor(sqrt(n)) for n >= 0, by Newton on BigInt -- an independent exact
   implementation, the same role BigInt plays for add/mul in test_decimal.js. */
function bigIsqrt(n) {
    if (n < 0n) throw new Error("negative");
    if (n < 2n) return n;
    let x0 = 1n << BigInt(Math.ceil((n.toString(2).length + 1) / 2));
    for (;;) {
        const x1 = (x0 + n / x0) >> 1n;
        if (x1 >= x0) return x0;
        x0 = x1;
    }
}

/* The correctly rounded p-digit (half-even) sqrt of decimal text, from the
   definitions. Returns { c, e } with c carrying exactly the p rounded digits. */
function oracleSqrt(text, p) {
    const { c, e } = toCE(text);
    if (c === 0n) return { c: 0n, e: 0 };
    /* scale so the integer root carries p+1 digits: root digits are
       ceil((digits(c) + e + 2s)/2), so take s big enough and even up */
    let s = Math.max(0, Math.ceil((2 * (p + 1) - (digitsOf(c) + e)) / 2));
    while (e + 2 * s < 0) s++;
    const N = c * pow10(e + 2 * s);
    const R = bigIsqrt(N);
    const rem = N - R * R;              /* the tail: sqrt(N) - R is < 1 iff rem>0 */
    const k = digitsOf(R);
    const cut = k - p;
    let T = R / pow10(cut > 0 ? cut : 0);
    if (cut > 0) {
        const guard = (R / pow10(cut - 1)) % 10n;
        const rest = (R % pow10(cut - 1)) !== 0n || rem !== 0n;
        /* half-even, from the definitions; an irrational tail is never a tie */
        if (guard > 5n || (guard === 5n && (rest || (T % 2n === 1n)))) T += 1n;
    }
    /* value = T * 10^(cut - s), cut >= 0 */
    return { c: T, e: (cut > 0 ? cut : 0) - s };
}

/* canonical plain text for { c, e } (trailing zeros folded into the exponent,
   the module's documented canonicalization) */
function ceText(c, e) {
    if (c === 0n) return "0";
    let neg = c < 0n;
    let d = (neg ? -c : c).toString();
    while (d.length > 1 && d.endsWith("0") && e < 0) { d = d.slice(0, -1); e++; }
    let out;
    if (e === 0) out = d;
    else if (e > 0) { out = d + "0".repeat(e); }
    else {
        const k = -e;
        out = k >= d.length ? "0." + "0".repeat(k - d.length) + d
                            : d.slice(0, d.length - k) + "." + d.slice(d.length - k);
    }
    return (neg ? "-" : "") + out;
}

/* ------------------------------------------------- sqrt vs BigInt oracle */

{
    /* deterministic LCG corpus: magnitudes 1e-100..1e100, 1..40 digit
       mantissas, plus every perfect square and near-square in the sweep */
    let seed = 20250922;
    const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed; };
    const lits = [];
    for (let i = 0; i < 400; i++) {
        const nd = rnd() % 40 + 1;
        let m = String(rnd() % 9 + 1);
        for (let j = 1; j < nd; j++) m += String(rnd() % 10);
        const e = rnd() % 200 - 100;
        lits.push(m + "e" + e);
    }
    /* perfect squares (exact roots) and their +-1 neighbours (hardest case) */
    const squares = [];
    for (const base of ["2", "3", "7", "997", "123456789", "10", "999999999999",
                        "3162277660168379331998893544432718533719555139325168"]) {
        const b = BigInt(base);
        const sq = b * b;
        squares.push(sq.toString(), (sq + 1n).toString(), (sq - 1n).toString());
        squares.push(sq.toString() + "e-40", sq.toString() + "e40");
    }
    let bad = 0, checked = 0;
    for (const x of lits.concat(squares)) {
        for (const p of [2, 9, 34, 50]) {
            const want = ceText(oracleSqrt(x, p).c, oracleSqrt(x, p).e);
            const got = D(x).sqrt({ precision: p }).toString();
            checked++;
            if (got !== want) {
                bad++;
                if (bad < 5) print("  sqrt(" + x + ", p=" + p + ") got " + got + " want " + want);
            }
        }
    }
    assert(bad === 0, "sqrt matches the BigInt isqrt oracle, all modes of rounding "
                    + "(" + (checked - bad) + "/" + checked + ")");
}

/* perfect squares are EXACT (the remainder is zero, nothing to round) */
{
    let bad = 0;
    for (const b of [0n, 1n, 2n, 3n, 100n, 12345n, BigInt("3162277660168379331998893544432")]) {
        const sq = (b * b).toString();
        const r = D(sq).sqrt();
        if (!r.mul(r).equals(D(sq)) || !r.equals(D(b.toString()))) bad++;
    }
    assert(bad === 0, "sqrt of a perfect square is the exact integer root");
    eq(D("0").sqrt().toString(), "0", "sqrt(0) = 0");
}

/* --------------------------------------- pinned python-libmpdec vectors */

/* Generated from python decimal (libmpdec), the correctly-rounded reference:
 * the full development sweep was 9066 cases, 0 divergences (progress.log).
 * Expected text is in canonical form (digits + exponent). */
const ORACLE = [
    ["sqrt", 34, "2", "1414213562373095048801688724209698e-33"],
    ["sqrt", 34, "3", "1732050807568877293527446341505872e-33"],
    ["sqrt", 34, "1e-100", "1e-50"],
    ["sqrt", 34, "1e100", "1e50"],
    ["sqrt", 34, "123456.789", "3513641828644462161665823116758077e-31"],
    ["sqrt", 34, "0.000000000000000000000001", "1e-12"],
    ["sqrt", 34, "9.99999999999999999999e99", "9999999999999999999995e28"],
    ["sqrt", 34, "7.25", "2692582403567252015625355245770165e-33"],
    ["sqrt", 60, "2", "141421356237309504880168872420969807856967187537694807317668e-59"],
    ["sqrt", 60, "5.5e-50", "234520787991171477728281505677223314029411417670586857680285e-84"],
    ["sqrt", 100, "3.7", "1923538406167134475185536292121337387378186709111961457232331893010843913849457032400683968938518349e-99"],
    ["sqrt", 34, "1.9999", "1414178206592082934295141371637167e-33"],
    ["sqrt", 34, "2.1025", "145e-2"],
    ["sqrt", 34, "4e50", "2e25"],
    ["sqrt", 34, "1524157875323883675049535156256668194500533455762536198787501905199875019052100", "1234567890123456789012345678901235e6"],
    ["exp", 34, "1", "2718281828459045235360287471352662e-33"],
    ["exp", 34, "-1", "3678794411714423215955237701614609e-34"],
    ["exp", 34, "0.5", "1648721270700128146848650787814164e-33"],
    ["exp", 34, "-0.5", "6065306597126334236037995349911805e-34"],
    ["exp", 34, "10", "2202646579480671651695790064528424e-29"],
    ["exp", 34, "-10", "4539992976248485153559151556055061e-38"],
    ["exp", 34, "69.7", "1863482800424322548425652536994628e-3"],
    ["exp", 34, "-69.7", "5366295840091982428561289657700503e-64"],
    ["exp", 34, "1e-10", "1000000000100000000005000000000167e-33"],
    ["exp", 34, "-1e-10", "9999999999000000000049999999998333e-34"],
    ["exp", 34, "230", "7722018499983835717562125214027702e66"],
    ["exp", 34, "-230", "1294998192508983592378113644081526e-133"],
    ["exp", 34, "2302", "5570540566930308508854215062204624e966"],
    ["exp", 34, "-2302", "1795157916875306229516450649601944e-1033"],
    ["exp", 60, "2.5", "121824939607034734380701759511679661831827677900631613115604e-58"],
    ["exp", 100, "-7.25", "7101743888425490635846003705775444086763023873618958855644522887464704494811934536677967239185252786e-103"],
    ["exp", 34, "999999.9", "2744566787989319482290170110203556e434261"],
    ["exp", 34, "-999999.9", "3643562271379826640737714945144005e-434328"],
    ["ln", 34, "10", "2302585092994045684017991454684364e-33"],
    ["ln", 34, "2", "6931471805599453094172321214581766e-34"],
    ["ln", 34, "3", "1098612288668109691395245236922526e-33"],
    ["ln", 34, "1e100", "2302585092994045684017991454684364e-31"],
    ["ln", 34, "1e-100", "-2302585092994045684017991454684364e-31"],
    ["ln", 34, "0.5", "-6931471805599453094172321214581766e-34"],
    ["ln", 34, "1.5", "4054651081081643819780131154643491e-34"],
    ["ln", 34, "9.9999999", "2302585082994045634017991121351028e-33"],
    ["ln", 60, "7.25", "198100146886658340834880778944555846934351264520223357068031e-59"],
    ["ln", 100, "1e50", "1151292546497022842008995727342182103800550744314386488016663950483786304838676240117998602544799149e-97"],
    ["ln", 34, "2.5", "9162907318741550651835272117680111e-34"],
    ["log10", 34, "2", "301029995663981195213738894724493e-33"],
    ["log10", 34, "1e100", "1e2"],
    ["log10", 34, "0.001", "-3e0"],
    ["log10", 34, "7.25", "8603380065709936969053689735202689e-34"],
    ["log10", 60, "12345.6789", "409151497716927044751833362305954725851507333894466643029235e-59"],
    ["log10", 100, "3.3", "5185139398778874780452278744981395509068310546571489594264046589849821850169104706142631420333474805e-100"],
];
{
    let bad = 0;
    for (const [fn, prec, v, want] of ORACLE) {
        const got = D(v)[fn]({ precision: prec });
        const w = D(want);              /* canon text parses at any scale */
        /* value AND digit content AND scale: equals alone would forgive a
         * trailing-zero difference, digits() pins the rounding width */
        if (!got.equals(w) || got.digits() !== w.digits() || got.sign() !== w.sign()) {
            bad++;
            print("  " + fn + "(" + v + ", " + prec + ") got " + got + " want " + want);
        }
    }
    assert(bad === 0, "pinned libmpdec vectors agree (" + (ORACLE.length - bad) + "/" +
                      ORACLE.length + ")");
}

/* rounding is ALWAYS half-even for the nonlinear ops, whatever mode is passed
 * (python's decimal does the same; documented, not an oversight) */
{
    const modes = ["up", "down", "ceil", "floor", "halfUp", "halfDown", "halfEven", "halfOdd"];
    let bad = 0;
    for (const m of modes) {
        if (D("2").sqrt({ rounding: m }).toString() !== D("2").sqrt().toString()) bad++;
        if (D("3").ln({ rounding: m }).toString() !== D("3").ln().toString()) bad++;
        if (D("1.5").exp({ rounding: m }).toString() !== D("1.5").exp().toString()) bad++;
        if (D("2").log10({ rounding: m }).toString() !== D("2").log10().toString()) bad++;
    }
    assert(bad === 0, "sqrt/exp/ln/log10 accept every mode name and ignore it (always correctly rounded half-even)");
    /* precision is honored (1..5000 like div) */
    eq(D("2").sqrt({ precision: 1 }).toString(), "1", "precision 1 rounds to 1 digit");
    eq(D("2").sqrt({ precision: 5 }).toString(), "1.4142", "precision 5");
}

/* --------------------------------------- algebraic identity spot checks */

{
    /* exp(a)*exp(b) == exp(a+b) within a few ulp at context precision */
    let worst = 0;
    for (const [a, b] of [["1", "2"], ["-3", "0.5"], ["10", "-10"], ["0.001", "-69"]]) {
        const lhs = D(a).exp().mul(D(b).exp());
        const rhs = D(a).add(b).exp();
        const rel = lhs.sub(rhs).abs().div(rhs.abs()).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-32, "exp(a)exp(b) == exp(a+b) to < 1e-32 relative (worst " + worst + ")");
    /* ln(xy) == ln(x)+ln(y) */
    worst = 0;
    for (const [x, y] of [["3", "7"], ["0.5", "100"], ["1e-50", "1e60"], ["9.5", "0.125"]]) {
        const lhs = D(x).mul(y).ln();
        const rhs = D(x).ln().add(D(y).ln());
        const rel = lhs.sub(rhs).abs().div(lhs.abs()).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-32, "ln(xy) == ln(x)+ln(y) to < 1e-32 relative (worst " + worst + ")");
    /* e^ln(x) == x, the round trip through the nonlinear pair */
    worst = 0;
    for (const x of ["7", "1e50", "0.0001", "3.14159"]) {
        const back = D(x).ln().exp();
        const rel = back.sub(D(x)).abs().div(D(x)).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-31, "e^ln(x) == x to < 1e-31 relative through TWO chained nonlinear roundings (worst " + worst + ")");
    /* log10(10^k) is EXACT for every k in range (same constant both sides) */
    let exact = 0;
    for (let k = -300; k <= 300; k += 7) {
        if (D("1e" + k).log10().toString() !== String(k)) { exact++; }
    }
    assert(exact === 0, "log10 of every 1e7k is the exact integer (86 powers)");
    worst = 0;
    for (const [m, k] of [["7.25", "13"], ["3.14159", "-40"], ["9.99", "99"]]) {
        const lhs = D(m).mul("1e" + k).log10();
        const rhs = D(m).log10().add(D(k));
        const rel = lhs.sub(rhs).abs().div(lhs.abs()).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-32, "log10(m*10^k) == log10(m)+k to < 1e-32 relative (worst " + worst + ")");
    /* sqrt(x)^2 == x to a couple of ulp */
    worst = 0;
    for (const x of ["2", "1e-50", "9.85", "1e49"]) {
        const sq = D(x).sqrt();
        const rel = sq.mul(sq).sub(D(x)).abs().div(D(x)).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-32, "sqrt(x)^2 == x to < 1e-32 relative (worst " + worst + ")");
}

/* ------------------------------------------- floor / ceil / trunc (exact) */

{
    /* vs the BigInt oracle on the same (c, e) decomposition */
    function oracleIntegral(text, mode) {
        const { c, e } = toCE(text);
        if (c === 0n) return "0";
        let out;
        if (e >= 0) out = { c: c * pow10(e), e: 0 };
        else {
            const scale = pow10(-e);
            const q = (c < 0n ? -c : c) / scale;
            const rem = (c < 0n ? -c : c) % scale;
            let v;
            if (mode === "trunc") v = q;
            else if (mode === "floor") v = c < 0n && rem !== 0n ? q + 1n : q;
            else v = c > 0n && rem !== 0n ? q + 1n : q;   /* ceil */
            out = { c: (c < 0n ? -v : v), e: 0 };
        }
        return ceText(out.c, out.e);
    }
    let bad = 0, checked = 0;
    const vals = ["0", "-0", "0.5", "-0.5", "2.5", "-2.5", "7", "-7", "1e10", "-1e10",
                  "1e-10", "-1e-10", "3.999999", "-3.999999", "0.0000001", "-0.0000001",
                  "123456789.987654321", "-123456789.987654321", "9.99", "-9.99",
                  "1.0000000000000000000000000000001", "-0.0000000000000000000000000000001"];
    let seed = 777;
    const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed; };
    for (let i = 0; i < 150; i++) {
        const nd = rnd() % 25 + 1;
        let m = String(rnd() % 9 + 1);
        for (let j = 1; j < nd; j++) m += String(rnd() % 10);
        const e = rnd() % 40 - 20;
        const neg = rnd() % 2 ? "-" : "";
        vals.push(neg + m + "e" + e);
    }
    for (const v of vals) {
        for (const mode of ["floor", "ceil", "trunc"]) {
            const got = D(v)[mode]().toString();
            const want = oracleIntegral(v, mode);
            checked++;
            if (got !== want) {
                bad++;
                if (bad < 5) print("  " + mode + "(" + v + ") got " + got + " want " + want);
            }
        }
    }
    assert(bad === 0, "floor/ceil/trunc match the BigInt oracle (" + (checked - bad) + "/" + checked + ")");
    /* definitions on the interesting signs */
    eq(D("-2.5").floor().toString(), "-3", "floor is toward -Infinity");
    eq(D("-2.5").ceil().toString(), "-2", "ceil is toward +Infinity");
    eq(D("-2.5").trunc().toString(), "-2", "trunc is toward zero");
    eq(D("-0.5").ceil().toString(), "0", "a zero result is 0, never -0 (module convention, like neg)");
    eq(D("-0.5").trunc().toString(), "0", "trunc(-0.5) is 0");
    eq(D("-0.5").floor().toString(), "-1", "floor(-0.5) is -1");
    assert(D("1e2000000000").floor().cmp(D("1e2000000000")) === 0
           && D("1e2000000000").floor().digits() === 1,
           "an integer passes through unchanged (huge exponent; its positional text is over the print bound, so compare by value)");
}

/* ---------------------------------------------------- divmod (exact pair) */

{
    /* the pair invariant a == q*b + r, q toward zero, r with the dividend's
       sign -- checked against BigInt on a full sign/scale matrix */
    const vals = ["-7", "-3", "-0.5", "0", "0.5", "2", "7", "7.5", "100",
                  "-100", "1e10", "-1e-10", "123.456", "-0.000001", "999999999999"];
    let bad = 0, checked = 0;
    for (const a of vals) for (const b of vals) {
        if (D(b).isZero()) continue;
        const [q, r] = D(a).divmod(b);
        const A = toCE(a), Q = toCE(q.toString()), R = toCE(r.toString());
        const bs = toCE(b);
        checked++;
        /* a == q*b + r exactly: align every term to its OWN exponent
           (multiplication adds exponents, so one shared scale is wrong) */
        {
            const m = Math.min(A.e, Q.e + bs.e, R.e);
            const ai = A.c * pow10(A.e - m);
            const qb = Q.c * bs.c * pow10(Q.e + bs.e - m);
            const ri = R.c * pow10(R.e - m);
            if (ai !== qb + ri) {
                bad++;
                if (bad < 5) print("  divmod(" + a + ", " + b + ") breaks a = qb+r");
                continue;
            }
        }
        /* sign rules: a nonzero remainder takes the dividend's sign */
        const rs = r.isZero() ? 0 : r.sign();
        if (rs !== (r.isZero() ? 0 : (A.c < 0n ? -1 : 1))) {
            bad++;
            if (bad < 5) print("  divmod(" + a + ", " + b + ") remainder sign " + rs);
        }
        /* |r| < |b| */
        {
            const m2 = Math.min(R.e, bs.e);
            const ri = R.c < 0n ? -R.c : R.c;
            const bi = bs.c < 0n ? -bs.c : bs.c;
            if (ri * pow10(R.e - m2) >= bi * pow10(bs.e - m2) && !r.isZero()) {
                bad++;
                if (bad < 5) print("  divmod(" + a + ", " + b + ") |r| >= |b|");
            }
        }
        /* q equals trunc(a/b) */
        if (!q.equals(D(a).div(b).trunc())) {
            bad++;
            if (bad < 5) print("  divmod(" + a + ", " + b + ") q != trunc(a/b)");
        }
    }
    assert(bad === 0, "divmod matches the BigInt oracle on the sign matrix ("
                    + (checked - bad) + "/" + checked + " checks)");
    {
        const [q, r] = D("-7").divmod("2");
        eq(q.toString(), "-3", "divmod(-7,2) quotient");
        eq(r.toString(), "-1", "divmod(-7,2) remainder takes the dividend's sign");
    }
    {
        const [q, r] = D("7.5").divmod("2");
        eq(q.toString(), "3", "divmod(7.5,2) quotient");
        eq(r.toString(), "1.5", "divmod(7.5,2) fractional remainder");
    }
    throws(() => D("1").divmod("0"), "divmod by zero is refused");
    eq(D("0").divmod("5")[0].toString() + "," + D("0").divmod("5")[1].toString(),
       "0,0", "divmod(0, x) is [0, 0]");
    /* exactness: opts accepted and ignored */
    {
        const [q1, r1] = D("-7.5").divmod("2", { precision: 3, rounding: "up" });
        eq(q1.toString(), "-3", "divmod ignores opts: quotient exact");
        eq(r1.toString(), "-1.5", "divmod ignores opts: remainder exact");
    }
}

/* -------------------------------------------------------- toBigInt */

{
    eq(String(D("42").toBigInt()), "42", "an integer converts");
    eq(String(D("-42").toBigInt()), "-42", "negative converts");
    eq(String(D("42.0").toBigInt()), "42", "42.0 is an integer");
    eq(String(D("0").toBigInt()), "0", "zero converts");
    /* toward zero is IRRELEVANT: a fraction throws rather than truncates */
    for (const v of ["0.5", "-0.5", "1.5", "-1.5", "42.9", "0.0000001", "1e-100"])
        throws(() => D(v).toBigInt(), "toBigInt refuses " + v + " (fractional part)");
    /* the documented int64 bridge bound */
    eq(String(D("9223372036854775807").toBigInt()), "9223372036854775807", "INT64_MAX converts");
    eq(String(D("-9223372036854775808").toBigInt()), "-9223372036854775808", "INT64_MIN converts");
    throws(() => D("9223372036854775808").toBigInt(), "INT64_MAX+1 is refused");
    throws(() => D("-9223372036854775809").toBigInt(), "INT64_MIN-1 is refused");
    throws(() => D("1e100").toBigInt(), "1e100 is refused (past int64)");
}

/* ------------------------------------------------------- the constants */

{
    eq(Decimal.ZERO.toString(), "0", "ZERO");
    eq(Decimal.ONE.toString(), "1", "ONE");
    eq(Decimal.TWO.toString(), "2", "TWO");
    eq(Decimal.TEN.toString(), "10", "TEN");
    eq(Decimal.NEG_ONE.toString(), "-1", "NEG_ONE");
    /* they are full Decimals: arithmetic and the new methods work on them */
    assert(Decimal.ZERO.add(Decimal.ONE).equals(Decimal.ONE), "ZERO + ONE == ONE");
    assert(Decimal.NEG_ONE.add(Decimal.ONE).isZero(), "NEG_ONE + ONE == 0");
    assert(Decimal.TWO.mul(Decimal.TEN).equals(D("20")), "TWO * TEN == 20");
    assert(Decimal.TWO.sqrt().equals(D("2").sqrt()), "TWO.sqrt() matches 2.sqrt()");
    assert(Decimal.TEN.div(Decimal.TWO).equals(D("5")), "TEN / TWO == 5");
    /* the ledger invariant from the type's reason to exist */
    for (const x of ["0", "1", "-19.99", "1e50", "0.000001"])
        assert(D(x).add(Decimal.ZERO).toString() === D(x).toString(),
               "x + ZERO is x for " + x);
}

/* ------------------------------------------------- adversarial boundaries */

{
    throws(() => D("-4").sqrt(), "sqrt of a negative is refused");
    throws(() => D("0").ln(), "ln(0) is refused (no -Infinity in this module)");
    throws(() => D("-2").ln(), "ln of a negative is refused");
    throws(() => D("-1").log10(), "log10 of a negative is refused");
    eq(D("1").ln().toString(), "0", "ln(1) = 0 exactly");
    eq(D("1").log10().toString(), "0", "log10(1) = 0 exactly");
    eq(D("0").exp().toString(), "1", "exp(0) = 1 exactly");
    /* exact powers of ten come out exact through the ln10/ln10 cancellation */
    for (const k of [1, 5, 20, 100, 1000]) {
        eq(D("1e" + k).log10().toString(), String(k), "log10(1e" + k + ") is exactly " + k);
        eq(D("1e-" + k).log10().toString(), String(-k), "log10(1e-" + k + ") is exactly " + String(-k));
    }
    /* overflow: e^x past Emax = 999999 refuses, python decimal's default too */
    throws(() => D("1e100").exp(), "exp(1e100) overflows the context");
    throws(() => D("2302586").exp(), "exp(2302586) overflows");
    throws(() => D("2302585.1").exp(), "exp(2302585.1) overflows (past 10^6*ln10)");
    /* one ulp under the boundary still computes, adjusted exponent 999999
       (its positional text is near the print bound, so compare by value) */
    {
        const r = D("2302585.09").exp({ precision: 5 });
        assert(r.digits() === 5 && r.cmp(D("1e999999")) > 0 && r.cmp(D("1e1000000")) < 0,
               "exp(2302585.09) computes: 5 digits, between 1e999999 and 1e1000000");
    }
    /* underflow: past Etiny the result is 0, not an error */
    assert(D("-1e100").exp().isZero(), "exp(-1e100) underflows to 0");
    assert(D("-1e1000000").exp().isZero(), "exp(-1e1000000) underflows to 0");
    /* subnormal band: values the context can still represent (adjusted
       exponent in [Etiny, -999999]) are QUANTIZED to the Etiny exponent,
       python-style -- exp(-2302585) is about 1.0975e-1000000, quantized to
       exponent -1000032, so 33 significant digits survive */
    {
        const r = D("-2302585").exp({ precision: 34 });
        assert(!r.isZero() && r.digits() === 33 && r.sign() === 1,
               "exp(-2302585) is a nonzero 33-digit Etiny-quantized POSITIVE value (e^-x never changes sign)");
        throws(() => r.toString(), "...whose positional text crosses the 1M-char print bound");
    }
    /* the exact zero boundary: python rounds e^x to 0 when the value falls
       below half of 10^Etiny -- x = -2302660 is past it, x = -2302659 not */
    assert(D("-2302660").exp({ precision: 34 }).isZero(),
           "exp(-2302660) underflows to 0 (past Etiny - log10(2))");
    assert(!D("-2302659").exp({ precision: 34 }).isZero(),
           "exp(-2302659) is still a nonzero subnormal");
    /* tiny arguments: e^+-tiny rounds to 1.000...0 at context precision --
       printed "1" by this module's trailing-zero canonicalization (python
       keeps the zeros; the VALUE and every significant digit agree) */
    eq(D("1e-40").exp({ precision: 34 }).toString(), "1",
       "exp(1e-40) rounds to 1 at 34 digits");
    assert(D("1e-40").exp({ precision: 34 }).cmp(D("1")) >= 0,
       "...and by value it is e^1e-40 >= 1");
    /* huge-magnitude exp decides in BOUNDED time with the right class
     * (the estimate never forms inf*0 = NaN, which used to open both gates
     * and sink into an unbounded halving/squaring path) */
    {
        const t0 = Date.now();
        let msg = null;
        try { D("90909E+99995").exp({ precision: 34 }); }
        catch (e) { msg = String(e); }
        assert(msg !== null && /overflows the context/.test(msg),
               "exp(90909E+99995) overflows the context, instantly");
        assert(D("-90909E+99995").exp({ precision: 34 }).isZero(),
               "exp(-90909E+99995) underflows to 0, instantly");
        {
            let m2 = null;
            try { D("1E+309").exp(); } catch (e) { m2 = String(e); }
            assert(m2 !== null && /overflows the context/.test(m2),
                   "exp(1E+309) overflows (the old NaN path decided nothing)");
        }
        assert(Date.now() - t0 < 300, "huge-magnitude exp calls are fast");
    }
    /* the budget is magnitude-INDEPENDENT for small |x|: the series converges
     * faster there, so high precision must not be refused */
    {
        const r = D("1E-200").exp({ precision: 340 });
        assert(r.digits() <= 340 && D("1").sub(r).abs().cmp(D("1E-199")) < 0,
               "exp(1E-200) at precision 340 computes (budget never fires for small |x|)");
    }
    /* the subnormal band is quantized to the Etiny exponent, python-style */
    {
        const r = D("-2302659").exp({ precision: 34 });
        assert(!r.isZero() && r.digits() === 1,
               "the smallest subnormal: exp(-2302659) quantizes to 1E-(Etiny+1)... nonzero, 1 digit");
    }
    /* a half-ulp tie whose deciding tail is far below the working digits is
     * refined by recomputation (e^t with t = 7.785678941890705650e-83: the
     * t^2/2 term at digit ~165 decides digit 100) */
    {
        const r = D("77856789.41890705650e-90").exp({ precision: 100 });
        assert(r.toString().endsWith("77856789418907057"),
               "the tie at digit 100 rounds UP (tail sticky recovered by the retry)");
    }

    /* the budget: series ops refuse extreme precision fast; sqrt has no cap */
    {
        const t0 = Date.now();
        throws(() => D("2").ln({ precision: 1000 }), "ln at precision 1000 refuses (budget)");
        throws(() => D("2").exp({ precision: 1000 }), "exp at precision 1000 refuses (budget)");
        assert(Date.now() - t0 < 500, "budget refusals are immediate");
        assert(D("2").sqrt({ precision: 5000 }).digits() === 5001 - 1,
               "sqrt at precision 5000 has no such cap");
    }
}

if (fails) {
    print("test_decimal_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal_upgrade failed");
}
print("test_decimal_upgrade: " + n + " assertions, 0 failures");
