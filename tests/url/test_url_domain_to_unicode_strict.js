// covers: dyna:url.domainToUnicode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_domain_to_unicode_strict");
import { domainToASCII, domainToUnicode } from "dyna:url";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("  FAIL: " + m + " (got " + a + ", want " + b + ")"); } };
const throws = (fn, rx, m) => {
    try { fn(); fail++; print("  FAIL: " + m + " (did not throw)"); }
    catch (e) {
        if (!(e instanceof TypeError)) { fail++; fail++; print("  FAIL: " + m + " wrong kind " + (e && e.constructor.name)); return; }
        if (rx && !rx.test(e.message)) { fail++; print("  FAIL: " + m + " message [" + e.message + "]"); return; }
        pass++;
    }
};

__kitS.section("domainToUnicode without opts unchanged (mapping lowercases)", () => {
eq(domainToUnicode("xn--bcher-kva.de"), "bücher.de",
   "domainToUnicode without opts unchanged (mapping lowercases)");
});

__kitS.section("domainToUnicode rejects unknown keys too", () => {
throws(() => domainToUnicode("x.test", { ts: 1 }),
       /unknown option "ts"/,
       "domainToUnicode rejects unknown keys too");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_url_strict: " + fail + " failures");
