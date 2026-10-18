// flags: --std
// Signature-driven input sweep of dyna:sdl (tests/lib/sigfuzz.js): every name
// dynajs.d.ts declares gets its baseline, the edge values of each parameter in
// turn, wrong types, and hostile coercions that close the receiver mid-call.
// SDL is pinned to its dummy video and audio drivers first, so the sweep is
// headless wherever it runs; a build without CONFIG_SDL3 reports a SKIP.
import { fuzzModule, scratchCleanup } from "../lib/sigfuzz.js";

let sdl = null;
try { sdl = await import("dyna:sdl"); } catch (e) {}
if (sdl) {
    sdl.setHint("SDL_VIDEO_DRIVER", "dummy");
    sdl.setHint("SDL_AUDIO_DRIVER", "dummy");
    sdl.init(["video", "audio", "gamepad"]);
}
const r = await fuzzModule("sdl");
scratchCleanup();
if (!r.skipped) {
    const bad = [...r.fails];
    if (r.reachedNone.length) bad.push("never reached: " + r.reachedNone.join(", "));
    if (r.noInstance.length) bad.push("no instance: " + r.noInstance.join(", "));
    if (bad.length) throw new Error("dyna:sdl input sweep: " + bad.join("; "));
}
print("test_sdl_inputs: " + (r.skipped ? "SKIP (dyna:sdl is not in this build: CONFIG_SDL3=y)" : "ok"));
