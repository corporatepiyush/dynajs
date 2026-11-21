// covers: dyna:url.domainToASCII
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_domain_to_ascii_strict");
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

__kitS.section("domainToASCII non-transitional", () => {
eq(domainToASCII("Bücher.de", { transitional: false }), "xn--bcher-kva.de",
   "domainToASCII non-transitional");
});

__kitS.section("domainToASCII transitional (same answer for this name)", () => {
eq(domainToASCII("Bücher.de", { transitional: true }), "xn--bcher-kva.de",
   "domainToASCII transitional (same answer for this name)");
});

__kitS.section("sharp s: non-transitional keeps it (IDs compatible false)", () => {
eq(domainToASCII("faß.de", { transitional: false }), "xn--fa-hia.de",
   "sharp s: non-transitional keeps it (IDs compatible false)");
});

__kitS.section("sharp s: transitional maps it (the case the option exists for)", () => {
eq(domainToASCII("faß.de", { transitional: true }), "fass.de",
   "sharp s: transitional maps it (the case the option exists for)");
});

__kitS.section("typo'd `transitionl` refuses with the valid set", () => {
throws(() => domainToASCII("x.test", { transitionl: false }),
       /unknown option "transitionl" \(valid: transitional, strict\)/,
       "typo'd `transitionl` refuses with the valid set");
});

__kitS.section("a second plausible-but-absent key refuses", () => {
throws(() => domainToASCII("x.test", { transitional: false, std3: true }),
       /unknown option "std3"/,
       "a second plausible-but-absent key refuses");
});

__kitS.section("the one-argument form is untouched", () => {
eq(domainToASCII("münchen.de"), "xn--mnchen-3ya.de",
   "the one-argument form is untouched");
});

__kitS.section("uppercase folds through", () => {
eq(domainToASCII("MÜNCHEN.DE"), "xn--mnchen-3ya.de",
   "uppercase folds through");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_url_strict: " + fail + " failures");
