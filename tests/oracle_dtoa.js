const N = parseInt(scriptArgs[1] || "4000", 10);
const SEED = parseInt(scriptArgs[2] || "20260728", 10);

let _s = SEED >>> 0 || 1;
function rnd() { _s ^= _s << 13; _s >>>= 0; _s ^= _s >>> 17; _s ^= _s << 5; _s >>>= 0; return _s; }

let H = 2166136261 >>> 0;
function mix(str) {
    for (let i = 0; i < str.length; i++) { H ^= str.charCodeAt(i); H = Math.imul(H, 16777619) >>> 0; }
    H ^= 10; H = Math.imul(H, 16777619) >>> 0;
}

const vals = [
    0, -0, 1, -1, 0.5, 1/3, 2/3, 10, 100, 1e15, 1e16, 1e21, 1e-7,
    2 ** 53, 2 ** 53 - 1, 2 ** 53 + 2, -(2 ** 53),
    Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER,
    Number.MAX_VALUE, Number.MIN_VALUE, Number.EPSILON,
    5e-324, 1e-323, 2.2250738585072014e-308, 2.225073858507201e-308,
    1.7976931348623157e308, 4.9406564584124654e-324,
    1.2345678901234567e-308, 1.2345678901234567e308,
    0.1, 0.2, 0.3, 1.005, 1234.5678, 9007199254740993,
    Infinity, -Infinity, NaN,
];
for (let e = -320; e <= 308; e += 7) { vals.push(Number("1e" + e)); vals.push(-Number("1e" + e)); }
for (let e = -1070; e <= 1023; e += 23) vals.push(Math.pow(2, e));
const dv = new DataView(new ArrayBuffer(8));
for (let i = 0; i < N; i++) {
    dv.setUint32(0, rnd()); dv.setUint32(4, rnd());
    const d = dv.getFloat64(0);
    if (d === d && d !== Infinity && d !== -Infinity) vals.push(d);
}

let rtFail = 0, checks = 0;
for (const v of vals) {
    mix(String(v)); checks++;
    if (v === v && Number(String(v)) !== v) rtFail++;

    if (isFinite(v)) {
        const a = Math.abs(v);
        for (const p of [0, 1, 2, 6, 17, 20, 50, 100]) {
            if (a < 1e21) { try { mix(v.toFixed(p)); checks++; } catch (e) { mix("Ef" + p); } }
        }
        for (const p of [0, 1, 6, 17, 20]) { try { mix(v.toExponential(p)); checks++; } catch (e) { mix("Ee" + p); } }
        for (const p of [1, 2, 6, 17, 21]) { try { mix(v.toPrecision(p)); checks++; } catch (e) { mix("Ep" + p); } }
        for (let r = 2; r <= 36; r++) { mix(v.toString(r)); checks++; }
        const ex = v.toExponential(17);
        if (Number(ex) !== v) rtFail++;
    }
}

const parseCases = [
    "0", "-0", "1e-323", "5e-324", "1e309", "-1e309", "0x1fffffffffffff",
    "0b1010", "0o777", "1_0", " 12 ", "", "  ", ".5", "5.", "1e", "1e+", "0x",
    "1.7976931348623157e309", "2.2250738585072011e-308",
    "9007199254740993", "900719925474099300000",
    "0.000000000000000000000001", "1".repeat(400), "1e-400", "1e400",
];
for (const s of parseCases) { mix(s + "=" + String(Number(s))); checks++; }
for (let i = 0; i < 400; i++) {
    const digits = 1 + (rnd() % 25), ex = (rnd() % 640) - 320;
    let m = ""; for (let d = 0; d < digits; d++) m += String(rnd() % 10);
    const s = m + "e" + ex;
    const x = Number(s);
    mix(s + "=" + String(x)); checks++;
    if (isFinite(x) && Number(String(x)) !== x) rtFail++;
}

print("#D " + H);
print("oracle_dtoa: values=" + vals.length + " checks=" + checks +
      " roundtrip_failures=" + rtFail + " hash=" + H);
if (rtFail) throw new Error("oracle_dtoa: " + rtFail + " round-trip failures");
