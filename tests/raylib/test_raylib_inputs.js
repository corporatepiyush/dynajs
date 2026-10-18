// flags: --std
// Signature-driven input sweep of dyna:raylib (tests/lib/sigfuzz.js): every name dynajs.d.ts
// declares gets its baseline, each parameter's edge values, wrong types and hostile coercions. raylib
// drives one window per process, so receivers come from tests/lib/sigfuzz_baselines.js, which reopens
// the window whenever a hostile argument closed it. No display, or a build without CONFIG_RAYLIB: SKIP.
import { fuzzModule, scratchCleanup } from "../lib/sigfuzz.js";
import { SKIP } from "../lib/sigfuzz_baselines.js";

let rl = null, why = "dyna:raylib is not in this build: CONFIG_RAYLIB=y";
try { rl = await import("dyna:raylib"); } catch (e) {}
if (rl) {
    rl.setLogLevel("none");
    try { new rl.Window(32, 32, "sigfuzz", { hidden: true }).close(); } catch (e) { why = "no display to open a window on"; rl = null; }
}
if (rl) {
    try { rl.initAudio(); } catch (e) {
        for (const c of ["Sound", "Music", "AudioStream", "loadSound", "loadMusic", "setMasterVolume"]) SKIP["raylib." + c] = "no audio device on this host";
        print("  note: no audio device, the audio names are skipped");
    }
}
const r = rl ? await fuzzModule("raylib") : { skipped: true };
scratchCleanup();
if (!r.skipped) {
    const bad = [...r.fails];
    if (r.reachedNone.length) bad.push("never reached: " + r.reachedNone.join(", "));
    if (r.noInstance.length) bad.push("no instance: " + r.noInstance.join(", "));
    if (bad.length) throw new Error("dyna:raylib input sweep: " + bad.join("; "));
}
print("test_raylib_inputs: " + (r.skipped ? "SKIP (" + why + ")" : "ok"));
