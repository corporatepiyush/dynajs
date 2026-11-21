// flags: --std
// covers: contract dyna:sdl.* -- signature-driven input sweep of every declared name
// Signature-driven input sweep of dyna:sdl (tests/lib/sigfuzz.js): every name
// dynajs.d.ts declares gets its baseline, the edge values of each parameter in
// turn, wrong types, and hostile coercions that close the receiver mid-call.
// SDL is pinned to its dummy video and audio drivers first, so the sweep is
// headless wherever it runs; a build without CONFIG_SDL3 reports a SKIP.
import { inputSweep } from "../lib/sigfuzz.js";

await inputSweep("test_sdl_inputs", "sdl", async () => {
    const sdl = await import("dyna:sdl");
    sdl.setHint("SDL_VIDEO_DRIVER", "dummy");
    sdl.setHint("SDL_AUDIO_DRIVER", "dummy");
    sdl.init(["video", "audio", "gamepad"]);
    return null;
});
