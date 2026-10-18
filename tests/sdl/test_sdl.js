// flags: --std
// dyna:sdl behaviour on SDL's dummy video and audio drivers (headless, software
// renderer). Pixel expectations are properties, not recorded output: a pixel
// reads back as the colour drawn there, and a texture's texels land in its
// destination rect. A build without CONFIG_SDL3 reports a SKIP.
import * as os from "os";
import * as std from "std";
import { Frame, assertFrame } from "../lib/screen.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function refuses(fn, ctor, re, m) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    ok(e instanceof ctor && re.test(e.message), m + " (got " + e + ")");
}

let sdl = null;
try { sdl = await import("dyna:sdl"); } catch (e) {}
if (!sdl) {
    print("test_sdl: SKIP (dyna:sdl is not in this build: CONFIG_SDL3=y)");
} else {
    const v = sdl.version();
    ok(v.major === 3 && typeof v.revision === "string", "version() reports SDL 3 (" + JSON.stringify(v) + ")");
    ok(typeof sdl.platform() === "string" && sdl.platform().length > 0, "platform() names the OS");
    ok(sdl.videoDrivers().includes("dummy"), "the dummy video driver is compiled in");

    refuses(() => new sdl.Window("x", 10, 10), TypeError, /init\(\["video"\]\) first/, "a Window before init() is refused");
    refuses(() => sdl.pollEvent(), TypeError, /init\(\) first/, "pollEvent before init() is refused");
    refuses(() => sdl.init(["video", "telepathy"]), RangeError, /unknown subsystem 'telepathy'/, "an unknown subsystem is refused by name");
    refuses(() => sdl.init("video"), TypeError, /array of names/, "init takes an array");

    // An SDL_VIDEODRIVER/SDL_AUDIODRIVER environment variable outranks a normal
    // hint (setHint then answers false); either way the dummy drivers must run.
    ok(typeof sdl.setHint("SDL_VIDEO_DRIVER", "dummy") === "boolean" && typeof sdl.setHint("SDL_AUDIO_DRIVER", "dummy") === "boolean", "setHint answers a boolean");
    sdl.init(["video", "audio"]);
    ok(sdl.currentVideoDriver() === "dummy" && sdl.currentAudioDriver() === "dummy", "the dummy drivers are running");
    ok(["video", "audio", "events"].every((s) => sdl.wasInit().includes(s)), "wasInit lists video, audio and events (" + sdl.wasInit() + ")");
    sdl.init(["video"]);
    ok(sdl.wasInit().includes("video"), "init() again is harmless");

    // --- drawing and readback -------------------------------------------------
    const W = 40, H = 30;
    const win = new sdl.Window("test", W, H, { hidden: true });
    ok(win.id > 0 && win.title === "test", "window id and title");
    ok(win.size.width === W && win.size.height === H, "window size " + JSON.stringify(win.size));
    win.setTitle("renamed");
    // Cocoa turns invalid UTF-8 into a nil NSString and -[NSWindow setTitle:] then aborts the process;
    // a JS string reaches C as WTF-8, so a lone surrogate is the one way to get there.
    refuses(() => new sdl.Window("a\ud800b", 8, 8, { hidden: true }), TypeError, /lone surrogate/, "a title with a lone surrogate is refused");
    refuses(() => win.setTitle("\udfff"), TypeError, /lone surrogate/, "setTitle refuses a lone surrogate");
    ok(win.title === "renamed", "and the refused title changed nothing");
    ok(win.title === "renamed", "setTitle");
    const r = new sdl.Renderer(win);
    ok(typeof r.name === "string" && r.outputSize.width === W && r.outputSize.height === H, "renderer " + r.name + " " + JSON.stringify(r.outputSize));
    refuses(() => new sdl.Renderer(win), TypeError, /already has a Renderer/, "a second renderer on one window is refused");

    const px = () => r.readPixels();
    const at = (img, x, y) => Array.from(img.pixels.subarray((y * img.width + x) * 4, (y * img.width + x) * 4 + 4)).join(",");
    r.setDrawColor(10, 20, 30);
    r.clear();
    r.setDrawColor(200, 0, 0, 255);
    r.fillRect(5, 5, 10, 10);
    r.setDrawColor(0, 0, 250);
    r.drawLine(0, 25, 39, 25);
    r.drawPoint(38, 2);
    let img = px();
    ok(img.width === W && img.height === H && img.pixels.length === W * H * 4, "readPixels is width*height RGBA");
    const BG = [10, 20, 30, 255], RED = [200, 0, 0, 255], BLUE = [0, 0, 250, 255];
    const scene = new Frame(W, H, BG).rect(5, 5, 10, 10, RED).rect(0, 25, 40, 1, BLUE).pixel(38, 2, BLUE);
    assertFrame(ok, img, scene, "clear + fillRect + drawLine (both endpoints) + drawPoint: the whole frame");
    const part = r.readPixels({ x: 5, y: 5, width: 3, height: 2 });
    assertFrame(ok, part, new Frame(3, 2, RED), "readPixels(rect) reads exactly the sub-rectangle");

    // --- textures --------------------------------------------------------------
    const tex = new sdl.Texture(r, 2, 2);
    ok(tex.width === 2 && tex.height === 2, "texture size");
    const green = new Uint8Array(16);
    for (let i = 0; i < 4; i++) green.set([0, 255, 0, 255], i * 4);
    tex.update(green);
    r.drawTexture(tex, null, { x: 20, y: 10, width: 4, height: 4 });
    scene.rect(20, 10, 4, 4, [0, 255, 0, 255]);
    assertFrame(ok, px(), scene, "a 2x2 texture stretched over a 4x4 dst rect, and nothing else changed");
    tex.update(new Uint8Array([255, 255, 0, 255]), { x: 1, y: 1, width: 1, height: 1 });
    r.drawTexture(tex, { x: 1, y: 1, width: 1, height: 1 }, { x: 30, y: 10, width: 1, height: 1 });
    assertFrame(ok, px(), scene.pixel(30, 10, [255, 255, 0, 255]), "update(rect) then a src rect draws exactly that texel");
    tex.setColorMod(255, 0, 0);
    r.drawTexture(tex, { x: 1, y: 1, width: 1, height: 1 }, { x: 31, y: 10, width: 1, height: 1 });
    assertFrame(ok, px(), scene.pixel(31, 10, [255, 0, 0, 255]), "setColorMod multiplies the texel");
    tex.setColorMod(255, 255, 255);
    tex.setAlphaMod(0);
    r.drawTexture(tex, null, { x: 0, y: 0, width: 2, height: 2 });
    assertFrame(ok, px(), scene, "alpha 0 with blending leaves the whole target unchanged");
    tex.setAlphaMod(255);
    tex.setBlendMode("none");
    refuses(() => tex.setBlendMode("screen"), RangeError, /unknown blend mode/, "an unknown blend mode is refused");
    refuses(() => tex.update(new Uint8Array(15)), RangeError, /needs 16 bytes/, "too few bytes for the area is refused");
    refuses(() => tex.update(new Uint16Array(8)), TypeError, /byte view/, "a wider typed array is refused");
    refuses(() => tex.update(green, { x: 1, y: 1, width: 2, height: 2 }), RangeError, /inside the 2x2 texture/, "a rect outside the texture is refused");
    r.present();

    // --- argument refusals -----------------------------------------------------
    refuses(() => r.setDrawColor(256, 0, 0), RangeError, /0\.\.255/, "a colour channel over 255 is refused");
    refuses(() => r.setDrawColor(1.5, 0, 0), RangeError, /0\.\.255/, "a fractional colour channel is refused");
    refuses(() => r.fillRect(NaN, 0, 1, 1), RangeError, /finite/, "a NaN coordinate is refused");
    refuses(() => new sdl.Window("x", 0, 10, { hidden: true }), RangeError, /1\.\.16384/, "a zero width is refused");
    refuses(() => new sdl.Window("x", 16385, 10, { hidden: true }), RangeError, /1\.\.16384/, "a width over 16384 is refused");
    refuses(() => new sdl.Window("x", 10, 10, { hidden: true, transparent: true }), TypeError, /unknown option 'transparent'/, "an unknown window option is refused by name");
    const w2 = new sdl.Window("other", 8, 8, { hidden: true });
    const r2 = new sdl.Renderer(w2);
    refuses(() => r2.drawTexture(tex), TypeError, /another renderer/, "a texture of another renderer is refused");
    let closedMid = null;
    refuses(() => r2.fillRect({ valueOf() { w2.close(); closedMid = true; return 1; } }, 0, 1, 1), TypeError, /window was closed/,
        "a window closed by an argument's valueOf is caught before the draw");
    ok(closedMid === true && w2.closed, "and the coercion really ran first");

    // --- events ------------------------------------------------------------------
    sdl.pollEvents();
    sdl.pushEvent({ type: "user", code: 41 });
    sdl.pushEvent({ type: "user", code: -7 });
    sdl.pushEvent({ type: "quit" });
    const evs = sdl.pollEvents().filter((e) => e.type === "user" || e.type === "quit");
    ok(evs.map((e) => e.type + (e.code !== undefined ? ":" + e.code : "")).join(" ") === "user:41 user:-7 quit",
        "pushed events come back in order with their codes (" + JSON.stringify(evs) + ")");
    ok(evs.every((e) => typeof e.timestamp === "number" && e.timestamp >= 0), "events carry a timestamp");
    ok(sdl.pollEvent() === null || typeof sdl.pollEvent() === "object", "an empty queue polls as null");
    refuses(() => sdl.pushEvent({ type: "windowclose" }), RangeError, /unknown type .windowclose./, "a window event cannot be pushed");
    refuses(() => sdl.pollEvents(0), RangeError, /1\.\.65536/, "pollEvents(0) is refused");
    ok(sdl.keyName(sdl.keyFromName("A")) === "A", "keyName(keyFromName(\"A\")) round-trips");
    ok(sdl.keyFromName("no such key") === 0, "an unknown key name is 0");
    const ms = sdl.mouseState();
    ok(typeof ms.x === "number" && typeof ms.buttons === "number", "mouseState shape");

    refuses(() => sdl.setClipboardText("x\ud800"), TypeError, /lone surrogate/, "clipboard text with a lone surrogate is refused");
    sdl.setClipboardText("dyna clip");
    ok(sdl.hasClipboardText() && sdl.getClipboardText() === "dyna clip", "clipboard round-trip on the dummy driver");

    // --- audio ---------------------------------------------------------------------
    const a = new sdl.AudioStream({ freq: 48000, channels: 2, format: "f32" });
    a.put(new Float32Array(960));
    ok(a.queued === 960 * 4, "f32: queued counts bytes (" + a.queued + ")");
    a.clear();
    ok(a.queued === 0, "clear() drops the queue");
    refuses(() => a.put(new Int16Array(4)), TypeError, /takes a Float32Array/, "an f32 stream refuses Int16Array samples");
    a.setGain(0.5);
    refuses(() => a.setGain(17), RangeError, /0\.\.16/, "a gain over 16 is refused");
    a.resume();
    a.pause();
    const s16 = new sdl.AudioStream({ freq: 22050, channels: 1, format: "s16" });
    s16.put(new Int16Array(100));
    ok(s16.queued === 200, "s16: two bytes per sample (" + s16.queued + ")");
    refuses(() => new sdl.AudioStream({ format: "u8" }), RangeError, /"f32" or "s16"/, "an unsupported sample format is refused");
    refuses(() => new sdl.AudioStream({ freq: 100 }), RangeError, /8000\.\.384000/, "a frequency below 8000 is refused");

    // --- lifetimes ---------------------------------------------------------------
    refuses(() => sdl.quit(), TypeError, /window\(s\), \d+ audio stream\(s\) and \d+ gamepad\(s\) are open/, "quit() while objects are open is refused");
    a.close();
    s16.close();
    refuses(() => a.put(new Float32Array(1)), TypeError, /closed/, "a closed stream refuses put()");
    win.close();
    refuses(() => r.clear(), TypeError, /renderer's window was closed/, "closing the window kills its renderer");
    refuses(() => tex.update(green), TypeError, /texture's renderer was closed/, "and the renderer's textures");
    r.close();
    tex.close();
    r2.close();

    // A renderer keeps its window alive: dropping every JS reference to the
    // window must not let the collector destroy it under the renderer.
    let held = new sdl.Renderer(new sdl.Window("held", 8, 8, { hidden: true }));
    std.gc();
    held.setDrawColor(1, 2, 3);
    held.clear();
    ok(at(held.readPixels(), 0, 0) === "1,2,3,255", "a renderer whose window has no other reference still draws after a GC");
    held.close();
    held = null;
    std.gc();

    // A window that holds its own renderer is a cycle (window -> renderer -> window)
    // only the collector can free; it needs the renderer's mark to see it. After a
    // GC nothing may stay open, or quit() below would be refused.
    {
        let cw = new sdl.Window("cycle", 8, 8, { hidden: true });
        cw.renderer = new sdl.Renderer(cw);
        cw.renderer.texture = new sdl.Texture(cw.renderer, 2, 2);
        cw = null;
    }
    std.gc();

    ok(sdl.wasInit().length > 0, "still initialized before quit");
    sdl.quit();
    ok(sdl.wasInit().length === 0, "quit() shuts down what init() started (" + sdl.wasInit() + ")");

    // Video belongs to the main thread: a Worker must be refused, not crash.
    const wf = `${std.getenv("TMPDIR") || "/tmp"}/dyna_sdl_worker_${os.getpid()}.js`;
    const f = std.open(wf, "w");
    f.puts(`import * as os from "os";
import * as sdl from "dyna:sdl";
let msg = "no error";
try { sdl.init(["video"]); } catch (e) { msg = e.constructor.name + ": " + e.message; }
os.Worker.parent.postMessage(msg);
`);
    f.close();
    const worker = new os.Worker(wf);
    const reply = await new Promise((res) => { worker.onmessage = (e) => { worker.onmessage = null; res(e.data); }; });
    os.remove(wf);
    ok(/^TypeError: .*main thread, not in a Worker/.test(reply), "init([\"video\"]) inside a Worker is refused (" + reply + ")");
}

print("test_sdl: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_sdl: " + fail + " failures");
