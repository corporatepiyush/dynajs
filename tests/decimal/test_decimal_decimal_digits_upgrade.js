// covers: dyna:decimal.Decimal.digits
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_digits_upgrade");
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

__kitS.section(") got", () => {
    let bad = 0;
    for (const [fn, prec, v, want] of ORACLE) {
        const got = D(v)[fn]({ precision: prec });
        const w = D(want);
        if (!got.equals(w) || got.digits() !== w.digits() || got.sign() !== w.sign()) {
            bad++;
            print("  " + fn + "(" + v + ", " + prec + ") got " + got + " want " + want);
        }
    }
    assert(bad === 0, "pinned libmpdec vectors agree (" + (ORACLE.length - bad) + "/" +
                      ORACLE.length + ")");
});
__kitS.end();

if (fails) {
    print("test_decimal_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal_upgrade failed");
}
print("test_decimal_upgrade: " + n + " assertions, 0 failures");
