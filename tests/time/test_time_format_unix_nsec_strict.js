// covers: dyna:time.formatUnix
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_format_unix_nsec_strict");
import { formatRFC3339, formatUnix, Format, parseRFC3339 } from "dyna:time";


let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function throws(fn, kind, re, msg) {
    n++;
    let e = null;
    try { fn(); } catch (err) { e = err; }
    if (e === null) throw new Error(msg + ": expected a throw");
    if (kind && !(e instanceof kind))
        throw new Error(msg + ": wrong error type: " + e);
    if (re && !re.test(String(e.message)))
        throw new Error(msg + ": wrong message: " + e.message);
}

throws(() => formatUnix(0, "%z", { offsetMinutes: "x" }), TypeError,
    /offsetMinutes must be a number/, "formatUnix shares the reader");

throws(() => formatUnix(0, "%z", { offsetMinutes: null }), TypeError,
    /offsetMinutes must be a number/, "formatUnix: null is not a number");

assert(formatUnix(0, "%z", undefined) === "Z", "an absent bag is still no options");

__kitS.section(", oracle", () => {
    function civil(secs) {
        const z = Math.floor(secs / 86400) + 719468, era = Math.floor(z / 146097), doe = z - era * 146097;
        const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365);
        const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100)), mp = Math.floor((5 * doy + 2) / 153);
        const d = doy - Math.floor((153 * mp + 2) / 5) + 1, m = mp < 10 ? mp + 3 : mp - 9;
        return (yoe + era * 400 + (m <= 2 ? 1 : 0)) + "-" + String(m).padStart(2, "0") + "-" + String(d).padStart(2, "0");
    }
    for (const s of [253402300799, 253402300800, 1e12, 1e15, 9.2e15]) {
        const got = formatUnix(s, "2006-01-02"), want = civil(s);
        assert(got === want, "formatUnix(" + s + ") = " + got + ", oracle " + want);
    }
    assert(formatUnix(9.2e15, "2006") === "291538364", "a 9-digit year is written whole");
});
__kitS.end();

print("test_time_nsec_strict: all " + n + " assertions passed");
