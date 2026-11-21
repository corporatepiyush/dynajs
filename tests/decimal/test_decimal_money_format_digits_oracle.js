// covers: contract dyna:decimal.Money.toString dyna:decimal.Money.format dyna:decimal.Money.toDecimal dyna:decimal.Money.toJSON -- one shared amount formatter
// Money's text (toString, format, toJSON, and the text toDecimal parses) was built with snprintf("%lld.%0*lld") and a
// second snprintf for the symbol / code; it is now written digit by digit (int64 magnitude through uint64, so
// -2^63 prints) and joined with memcpy. Oracle: the same text built with BigInt in JS: whole = trunc(amount /
// 10^d), fraction zero-padded to d digits, "-" before a zero whole part of a negative amount, the symbol table
// ($, EUR, GBP, JPY, CNY, INR, KRW, CA$, A$) after the sign, otherwise "<amount> <CODE>"; every minorDigits 0..6, amounts
// from 0 and +-1 to +-(2^63-1) and -2^63 (fromMinor with a bigint).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_money_format_digits_oracle");
import { Money } from "dyna:decimal";

let checks = 0, fails = 0;
let seed = 55;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const SYM = { USD: "$", EUR: "€", GBP: "£", JPY: "¥", CNY: "¥", INR: "₹", KRW: "₩", CAD: "CA$", AUD: "A$" };
function text(a, d) {
    if (d === 0) return a.toString();
    const div = 10n ** BigInt(d), neg = a < 0n, m = neg ? -a : a;
    const whole = m / div, frac = (m % div).toString().padStart(d, "0");
    return (neg ? "-" : "") + whole.toString() + "." + frac;
}
function fmt(a, d, code) {
    const t = text(a, d);
    if (SYM[code] !== undefined) return (a < 0n ? "-" : "") + SYM[code] + (t[0] === "-" ? t.slice(1) : t);
    return t + " " + code;
}
function ok(got, want, what) {
    checks++;
    if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + JSON.stringify(got) + ", want " + JSON.stringify(want)); }
}
const codes = ["USD", "EUR", "GBP", "JPY", "CNY", "INR", "KRW", "CAD", "AUD", "CHF", "XYZ"];
const amounts = [0n, 1n, -1n, 9n, -9n, 10n, -10n, 99n, -100n, 123456n, -123456n, 2n ** 53n - 1n, -(2n ** 53n - 1n), 2n ** 63n - 1n, -(2n ** 63n), -(2n ** 63n) + 1n];
for (let i = 0; i < 400; i++) {
    let s = String(1 + ((rnd() * 9) | 0));
    const n = (rnd() * 18) | 0;
    for (let k = 0; k < n; k++) s += (rnd() * 10) | 0;
    amounts.push(BigInt(s) * (rnd() < 0.5 ? -1n : 1n));
}
__kitS.section("toString, toJSON, format and toDecimal equal the BigInt text", () => {
    for (const code of codes) for (let d = 0; d <= 6; d++) for (const a of amounts) {
        if (a > 2n ** 63n - 1n || a < -(2n ** 63n)) continue;
        const m = Money.fromMinor(a, code, { minorDigits: d });
        const t = text(a, d);
        ok(m.toString(), t, "toString " + a + " d=" + d);
        ok(m.toJSON(), t, "toJSON " + a + " d=" + d);
        ok(m.format(), fmt(a, d, code), "format " + code + " " + a + " d=" + d);
        ok(m.toDecimal().toString(), t.replace(/^(-?)0*(\d)/, "$1$2").replace(/(\.\d*?)0+$/, "$1").replace(/\.$/, "").replace(/^-0$/, "0"), "toDecimal " + a + " d=" + d);
    }
});
if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_decimal_money_format_digits_oracle: " + checks + " checks passed");
__kitS.end();
