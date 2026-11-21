// flags: --std
// covers: contract dyna:raylib.* -- signature-driven input sweep of every declared name
// Signature-driven input sweep of dyna:raylib (tests/lib/sigfuzz.js): every name dynajs.d.ts
// declares gets its baseline, each parameter's edge values, wrong types and hostile coercions. raylib
// drives one window per process, so receivers come from tests/lib/sigfuzz_baselines.js, which reopens
// the window whenever a hostile argument closed it. No display, or a build without CONFIG_RAYLIB: SKIP.
import { inputSweep } from "../lib/sigfuzz.js";
import { SKIP } from "../lib/sigfuzz_baselines.js";

await inputSweep("test_raylib_inputs", "raylib", async () => {
    const rl = await import("dyna:raylib");
    rl.setLogLevel("none");
    try { new rl.Window(32, 32, "sigfuzz", { hidden: true }).close(); } catch (e) { return "no display to open a window on"; }
    try { rl.initAudio(); } catch (e) {
        for (const c of ["Sound", "Music", "AudioStream", "loadSound", "loadMusic", "setMasterVolume"]) SKIP["raylib." + c] = "no audio device on this host";
    }
    return null;
});
