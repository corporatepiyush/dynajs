// flags: --std
// Signature-driven input sweep of dyna:ui (tests/lib/sigfuzz.js): every name dynajs.d.ts declares
// gets its baseline, each parameter's edge values, wrong types and hostile coercions. Widgets exist
// only inside frame(), so sigfuzz_baselines.js's CALL hook runs each call inside one (except the
// names refused there). A build without CONFIG_UI: SKIP.
import { fuzzModule, scratchCleanup } from "../lib/sigfuzz.js";

let mod = null;
try { mod = await import("dyna:ui"); } catch (e) {}
const r = mod ? await fuzzModule("ui") : { skipped: true };
scratchCleanup();
if (!r.skipped) {
    const bad = [...r.fails];
    if (r.reachedNone.length) bad.push("never reached: " + r.reachedNone.join(", "));
    if (r.noInstance.length) bad.push("no instance: " + r.noInstance.join(", "));
    if (bad.length) throw new Error("dyna:ui input sweep: " + bad.join("; "));
}
print("test_ui_inputs: " + (r.skipped ? "SKIP (dyna:ui is not in this build: CONFIG_UI=y)" : "ok"));
