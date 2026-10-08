// flags: --std
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

eq(domainToASCII("Bücher.de", { transitional: false }), "xn--bcher-kva.de",
   "domainToASCII non-transitional");
eq(domainToASCII("Bücher.de", { transitional: true }), "xn--bcher-kva.de",
   "domainToASCII transitional (same answer for this name)");
eq(domainToUnicode("xn--bcher-kva.de"), "bücher.de",
   "domainToUnicode without opts unchanged (mapping lowercases)");
eq(domainToASCII("faß.de", { transitional: false }), "xn--fa-hia.de",
   "sharp s: non-transitional keeps it (IDs compatible false)");
eq(domainToASCII("faß.de", { transitional: true }), "fass.de",
   "sharp s: transitional maps it (the case the option exists for)");

throws(() => domainToASCII("x.test", { transitionl: false }),
       /unknown option "transitionl" \(valid: transitional\)/,
       "typo'd `transitionl` refuses with the valid set");
throws(() => domainToUnicode("x.test", { ts: 1 }),
       /unknown option "ts"/,
       "domainToUnicode rejects unknown keys too");
throws(() => domainToASCII("x.test", { transitional: false, std3: true }),
       /unknown option "std3"/,
       "a second plausible-but-absent key refuses");

eq(domainToASCII("münchen.de"), "xn--mnchen-3ya.de",
   "the one-argument form is untouched");
eq(domainToASCII("MÜNCHEN.DE"), "xn--mnchen-3ya.de",
   "uppercase folds through");

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_url_strict: " + fail + " failures");
