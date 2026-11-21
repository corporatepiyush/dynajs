// covers: dyna:time.date
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_date_offset");
import { formatRFC3339, formatUnix, parseRFC3339, Format, date } from "dyna:time";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) {
    ok(a === b, what + " (got " + a + ", want " + b + ")");
}
function throws(fn, ErrType, re, what) {
    let t = null;
    try { fn(); } catch (e) { t = e; }
    ok(t !== null, "expected throw: " + what);
    if (t === null) return;
    ok(t instanceof ErrType,
       what + " (wrong error type: " + (t && t.constructor && t.constructor.name) + ")");
    ok(!re || re.test(String(t.message)),
       what + " (message [" + String(t && t.message) + "] !~ " + re + ")");
}

function rfc3339ref(t, m) {
    const d = new Date((t + m * 60) * 1000);
    const base = d.toISOString().replace(/\.\d+Z$/, "");
    if (m === 0) return base + "Z";
    const sign = m < 0 ? "-" : "+";
    const a = Math.abs(m);
    return base + sign + String(Math.floor(a / 60)).padStart(2, "0") + ":" +
           String(a % 60).padStart(2, "0");
}

__kitS.section("legacy date", () => {
eq(formatRFC3339(date(2026, 8, 17, 10, 30, 0)), "2026-08-17T10:30:00Z", "legacy date");
});
__kitS.end();

print("test_time_offset: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
