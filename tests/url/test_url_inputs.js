// flags: --std
// Signature-driven input sweep of dyna:url (tests/lib/sigfuzz.js): every name
// dynajs.d.ts declares gets its baseline, the edge values of each parameter in
// turn, wrong types, and hostile coercions that close the receiver mid-call.
// Fails on a call over its time budget, a non-Error throw, a changed built-in
// prototype, a name no call ever got past the argument boundary, or a class
// no instance could be built for (its methods would silently go untested).
import { fuzzModule, scratchCleanup } from "../lib/sigfuzz.js";

const r = await fuzzModule("url");
scratchCleanup();
if (!r.skipped) {
    const bad = [...r.fails];
    if (r.reachedNone.length) bad.push("never reached: " + r.reachedNone.join(", "));
    if (r.noInstance.length) bad.push("no instance: " + r.noInstance.join(", "));
    if (bad.length) throw new Error("dyna:url input sweep: " + bad.join("; "));
}
