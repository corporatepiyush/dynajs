// covers: dyna:decimal.Decimal.exp
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_exp_upgrade");
import { Decimal } from "dyna:decimal";
import { cpuNow } from "../lib/cpuclock.js";

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

function oracleSqrt(text, p) {
    const { c, e } = toCE(text);
    if (c === 0n) return { c: 0n, e: 0 };
    let s = Math.max(0, Math.ceil((2 * (p + 1) - (digitsOf(c) + e)) / 2));
    while (e + 2 * s < 0) s++;
    const N = c * pow10(e + 2 * s);
    const R = bigIsqrt(N);
    const rem = N - R * R;
    const k = digitsOf(R);
    const cut = k - p;
    let T = R / pow10(cut > 0 ? cut : 0);
    if (cut > 0) {
        const guard = (R / pow10(cut - 1)) % 10n;
        const rest = (R % pow10(cut - 1)) !== 0n || rem !== 0n;
        if (guard > 5n || (guard === 5n && (rest || (T % 2n === 1n)))) T += 1n;
    }
    return { c: T, e: (cut > 0 ? cut : 0) - s };
}

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

__kitS.section("sqrt of a negative is refused", () => {
    throws(() => D("-4").sqrt(), "sqrt of a negative is refused");
    throws(() => D("0").ln(), "ln(0) is refused (no -Infinity in this module)");
    throws(() => D("-2").ln(), "ln of a negative is refused");
    throws(() => D("-1").log10(), "log10 of a negative is refused");
    eq(D("1").ln().toString(), "0", "ln(1) = 0 exactly");
    eq(D("1").log10().toString(), "0", "log10(1) = 0 exactly");
    eq(D("0").exp().toString(), "1", "exp(0) = 1 exactly");
    for (const k of [1, 5, 20, 100, 1000]) {
        eq(D("1e" + k).log10().toString(), String(k), "log10(1e" + k + ") is exactly " + k);
        eq(D("1e-" + k).log10().toString(), String(-k), "log10(1e-" + k + ") is exactly " + String(-k));
    }
    throws(() => D("1e100").exp(), "exp(1e100) overflows the context");
    throws(() => D("2302586").exp(), "exp(2302586) overflows");
    throws(() => D("2302585.1").exp(), "exp(2302585.1) overflows (past 10^6*ln10)");
    {
        const r = D("2302585.09").exp({ precision: 5 });
        assert(r.digits() === 5 && r.cmp(D("1e999999")) > 0 && r.cmp(D("1e1000000")) < 0,
               "exp(2302585.09) computes: 5 digits, between 1e999999 and 1e1000000");
    }
    assert(D("-1e100").exp().isZero(), "exp(-1e100) underflows to 0");
    assert(D("-1e1000000").exp().isZero(), "exp(-1e1000000) underflows to 0");
    {
        const r = D("-2302585").exp({ precision: 34 });
        assert(!r.isZero() && r.digits() === 33 && r.sign() === 1,
               "exp(-2302585) is a nonzero 33-digit Etiny-quantized POSITIVE value (e^-x never changes sign)");
        throws(() => r.toString(), "...whose positional text crosses the 1M-char print bound");
    }
    assert(D("-2302660").exp({ precision: 34 }).isZero(),
           "exp(-2302660) underflows to 0 (past Etiny - log10(2))");
    assert(!D("-2302659").exp({ precision: 34 }).isZero(),
           "exp(-2302659) is still a nonzero subnormal");
    eq(D("1e-40").exp({ precision: 34 }).toString(), "1",
       "exp(1e-40) rounds to 1 at 34 digits");
    assert(D("1e-40").exp({ precision: 34 }).cmp(D("1")) >= 0,
       "...and by value it is e^1e-40 >= 1");
    {
        const t0 = cpuNow();
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
        assert(cpuNow() - t0 < 300, "huge-magnitude exp calls are fast");
    }
    {
        const r = D("1E-200").exp({ precision: 340 });
        assert(r.digits() <= 340 && D("1").sub(r).abs().cmp(D("1E-199")) < 0,
               "exp(1E-200) at precision 340 computes (budget never fires for small |x|)");
    }
    {
        const r = D("-2302659").exp({ precision: 34 });
        assert(!r.isZero() && r.digits() === 1,
               "the smallest subnormal: exp(-2302659) quantizes to 1E-(Etiny+1)... nonzero, 1 digit");
    }
    {
        const r = D("77856789.41890705650e-90").exp({ precision: 100 });
        assert(r.toString().endsWith("77856789418907057"),
               "the tie at digit 100 rounds UP (tail sticky recovered by the retry)");
    }

    {
        const t0 = cpuNow();
        throws(() => D("2").ln({ precision: 1000 }), "ln at precision 1000 refuses (budget)");
        throws(() => D("2").exp({ precision: 1000 }), "exp at precision 1000 refuses (budget)");
        assert(cpuNow() - t0 < 500, "budget refusals are immediate");
        assert(D("2").sqrt({ precision: 5000 }).digits() === 5001 - 1,
               "sqrt at precision 5000 has no such cap");
    }
});
__kitS.end();

if (fails) {
    print("test_decimal_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal_upgrade failed");
}
print("test_decimal_upgrade: " + n + " assertions, 0 failures");
