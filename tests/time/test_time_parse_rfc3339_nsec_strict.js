// covers: dyna:time.parseRFC3339
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_rfc3339_nsec_strict");
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

__kitS.section("round-trip keeps the nsec", () => {
assert(parseRFC3339(formatRFC3339(0, 123456789)).nsec === 123456789,
    "round-trip keeps the nsec");
});
__kitS.end();

print("test_time_nsec_strict: all " + n + " assertions passed");
