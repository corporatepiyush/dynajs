// flags: --std
// dyna:raylib on a hidden window. Pixels are read back from RenderTextures (offscreen, so the
// result does not depend on the window being shown) and every check compares the WHOLE target
// against a frame built from what was drawn (tests/lib/screen.js). Constants are checked against
// raylib.h's own definitions; decoded samples against the bytes this test wrote. Without a display
// (raylib cannot open a window) the suite says SKIP; without CONFIG_RAYLIB it says SKIP.
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
const rgbaOf = (c) => [c >>> 24, (c >>> 16) & 255, (c >>> 8) & 255, c & 255];

let rl = null;
try { rl = await import("dyna:raylib"); } catch (e) {}
let win = null;
if (rl) {
    // --- before any window -----------------------------------------------------------
    ok(/^6\.\d+/.test(rl.version()), "version() is raylib 6 (" + rl.version() + ")");
    // raylib.h: #define RED CLITERAL(Color){ 230, 41, 55, 255 }, RAYWHITE { 245, 245, 245, 255 }, BLANK { 0, 0, 0, 0 }
    ok(rl.colors.RED === 0xe62937ff && rl.colors.RAYWHITE === 0xf5f5f5ff && rl.colors.BLANK === 0, "colors carry raylib.h's values");
    // raylib.h KeyboardKey: KEY_A = 65, KEY_SPACE = 32, KEY_ESCAPE = 256, KEY_KP_0 = 320
    ok(rl.keys.A === 65 && rl.keys.SPACE === 32 && rl.keys.ESCAPE === 256 && rl.keys.KP_0 === 320 && !("NULL" in rl.keys), "keys carry raylib.h's codes, without KEY_NULL");
    ok(rl.mouseButtons.LEFT === 0 && rl.mouseButtons.BACK === 6 && rl.gamepadAxes.RIGHT_TRIGGER === 5 && rl.gamepadButtons.RIGHT_THUMB === 17, "button and axis tables match raylib.h");
    ok(Object.isFrozen(rl.colors) || (!Object.isExtensible(rl.colors) && !Object.getOwnPropertyDescriptor(rl.colors, "RED").writable), "the constant tables cannot be changed");
    ok(rl.rgba(1, 2, 3) === 0x010203ff && rl.rgba(255, 255, 255, 0) === 0xffffff00, "rgba packs 0xRRGGBBAA, alpha defaulting to 255");
    refuses(() => rl.rgba(256, 0, 0), RangeError, /0\.\.255/, "a channel over 255 is refused");
    refuses(() => rl.drawRectangle(0, 0, 1, 1, 0xff), TypeError, /needs an open Window/, "drawing before a window is refused");
    refuses(() => new rl.RenderTexture(4, 4), TypeError, /needs an open Window/, "a RenderTexture before a window is refused");
    refuses(() => rl.measureText("x", 10), TypeError, /needs an open Window/, "measuring text before a window is refused (the default font loads with it)");
    refuses(() => new rl.Texture(), TypeError, /loadTexture\(path\) or Image#toTexture\(\)/, "Texture has no public constructor");
    rl.setLogLevel("error");
    // Cocoa turns invalid UTF-8 into a nil NSString and -[NSWindow setTitle:] aborts the process.
    let surrogate = null;
    try { new rl.Window(16, 16, "a\ud800b", { hidden: true }).close(); } catch (e) { surrogate = e; }
    ok(surrogate === null ? false : surrogate instanceof TypeError ? /lone surrogate/.test(surrogate.message) : /InitWindow failed/.test(surrogate.message),
        "a title with a lone surrogate is refused before raylib sees it (" + surrogate + ")");
    try { win = new rl.Window(64, 48, "dyna:raylib test", { hidden: true }); } catch (e) {
        if (e instanceof InternalError && /InitWindow failed/.test(e.message)) print("test_raylib: SKIP (no display to open a window on): " + e.message);
        else throw e;
    }
}
if (!rl) print("test_raylib: SKIP (dyna:raylib is not in this build: CONFIG_RAYLIB=y)");
else if (win) {
    const dir = `${std.getenv("TMPDIR") || "/tmp"}/dyna_raylib_${os.getpid()}_${(Math.random() * 1e9) | 0}`;
    ok(os.mkdir(dir) === 0, "scratch directory");
    const scratch = [];
    const file = (name, bytes) => {
        const p = dir + "/" + name, f = std.open(p, "wb");
        f.write(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        f.close();
        scratch.push(p);
        return p;
    };
    const BG = [10, 20, 30, 255], RED = rgbaOf(rl.colors.RED), BLUE = [0, 0, 255, 255], WHITE = [255, 255, 255, 255];
    const C = (c) => rl.rgba(...c);

    refuses(() => new rl.Window(10, 10, "two"), TypeError, /one window per process/, "a second window is refused");
    ok(win.size.width === 64 && win.size.height === 48 && typeof win.time === "number", "window size and clock");
    refuses(() => win.setTitle("\udfff"), TypeError, /lone surrogate/, "setTitle refuses a lone surrogate");

    // --- shapes into a RenderTexture, whole target ---------------------------------
    const W = 32, H = 24;
    const rt = new rl.RenderTexture(W, H);
    ok(rt.width === W && rt.height === H, "RenderTexture size");
    rt.draw(() => {
        rl.clearBackground(C(BG));
        rl.drawRectangle(2, 3, 5, 4, rl.colors.RED);
        rl.drawPixel(20, 1, C(BLUE));
        rl.drawLine(0, 20.5, 32, 20.5, C(BLUE));
    });
    const shapes = new Frame(W, H, BG).rect(2, 3, 5, 4, RED).pixel(20, 1, BLUE).rect(0, 20, 32, 1, BLUE);
    assertFrame(ok, rt.readPixels(), shapes, "rectangle, pixel and a 1px line: the whole target");
    rt.draw(() => {
        rl.clearBackground(C(BG));
        rl.drawCircle(16, 12, 6, C(WHITE));
    });
    assertFrame(ok, rt.readPixels(), new Frame(W, H, BG).circle(16, 12, 6, WHITE), "a circle covers its disc and nothing else");
    rt.draw(() => {
        rl.clearBackground(C(BG));
        rl.drawTriangle(4, 2, 2, 20, 26, 20, C(WHITE));
        rl.drawTriangle(4, 2, 26, 20, 2, 20, C(RED));
    });
    assertFrame(ok, rt.readPixels(), new Frame(W, H, BG).triangle(4, 2, 2, 20, 26, 20, WHITE),
        "a counter-clockwise triangle fills; the same triangle clockwise draws nothing (raylib culls it)");
    rt.draw(() => {
        rl.clearBackground(C(BG));
        rl.drawRectangleLines(4, 4, 10, 8, C(WHITE));
    });
    const outline = new Frame(W, H, BG).rect(4, 4, 10, 1, WHITE).rect(4, 11, 10, 1, WHITE).rect(4, 4, 1, 8, WHITE).rect(13, 4, 1, 8, WHITE);
    assertFrame(ok, rt.readPixels(), outline, "drawRectangleLines draws a 1px border inside the rect");

    // --- textures, images, flipping ----------------------------------------------------
    const px = new Uint8Array([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]);
    const img = new rl.Image(2, 2, px);
    ok(img.width === 2 && img.height === 2 && img.pixels().every((v, i) => v === px[i]), "an Image built from bytes hands the same bytes back");
    const tex = img.toTexture();
    rt.draw(() => {
        rl.clearBackground(C(BG));
        rl.drawTexture(tex, 10, 10);
        rl.drawTexturePro(tex, { x: 0, y: 0, width: 2, height: 2 }, { x: 20, y: 4, width: 4, height: 4 });
    });
    const quad = (f, x, y, s) => f.rect(x, y, s, s, [255, 0, 0, 255]).rect(x + s, y, s, s, [0, 255, 0, 255]).rect(x, y + s, s, s, [0, 0, 255, 255]).rect(x + s, y + s, s, s, WHITE);
    assertFrame(ok, rt.readPixels(), quad(quad(new Frame(W, H, BG), 10, 10, 1), 20, 4, 2), "a texture at its size and scaled 2x (point filter) lands texel for texel");
    const rt2 = new rl.RenderTexture(W, H);
    rt2.draw(() => { rl.clearBackground(C(BG)); rl.drawTexture(rt, 0, 0); });
    assertFrame(ok, rt2.readPixels(), quad(quad(new Frame(W, H, BG), 10, 10, 1), 20, 4, 2), "a RenderTexture drawn into another lands upright, whole target equal");
    rt2.draw(() => { rl.clearBackground(C(BG)); rl.drawTexturePro(rt, { x: 10, y: 10, width: 2, height: 2 }, { x: 0, y: 0, width: 2, height: 2 }); });
    assertFrame(ok, rt2.readPixels(), quad(new Frame(W, H, BG), 0, 0, 1), "drawTexturePro src counts from a RenderTexture's top-left");
    tex.update(new Uint8Array([9, 9, 9, 255]), { x: 1, y: 1, width: 1, height: 1 });
    rt2.draw(() => { rl.clearBackground(C(BG)); rl.drawTexture(tex, 0, 0); });
    assertFrame(ok, rt2.readPixels(), new Frame(W, H, BG).pixel(0, 0, [255, 0, 0, 255]).pixel(1, 0, [0, 255, 0, 255]).pixel(0, 1, [0, 0, 255, 255]).pixel(1, 1, [9, 9, 9, 255]),
        "Texture#update(rect) changes exactly that texel");
    refuses(() => tex.update(new Uint8Array(3)), RangeError, /needs 16 bytes/, "too few bytes for an update are refused");
    refuses(() => tex.update(new Uint8Array(16), { x: 1, y: 0, width: 2, height: 2 }), RangeError, /inside the 2x2 texture/, "an update rect past the right edge is refused");
    refuses(() => tex.update(new Uint8Array(16), { x: 0, y: 1, width: 2, height: 2 }), RangeError, /inside the 2x2 texture/, "an update rect past the bottom edge is refused");
    refuses(() => tex.update(new Uint8Array(16), { x: 0.5, y: 0, width: 1, height: 1 }), RangeError, /whole pixels/, "a fractional update rect is refused");
    refuses(() => tex.update(new Uint16Array(8)), TypeError, /byte view/, "a wider typed array is refused");

    const big = new rl.Image(4, 2, rl.rgba(1, 2, 3));
    ok(big.pixels().length === 32 && big.pixels().every((v, i) => v === [1, 2, 3, 255][i % 4]), "new Image(w, h, colour) fills every pixel");
    const flip = new rl.Image(2, 1, new Uint8Array([1, 1, 1, 255, 2, 2, 2, 255]));
    flip.flipHorizontal();
    ok(flip.pixels()[0] === 2 && flip.pixels()[4] === 1, "flipHorizontal swaps the columns");
    const crop = img.copy();
    crop.crop(1, 0, 1, 2);
    ok(crop.width === 1 && crop.height === 2 && crop.pixels().join() === "0,255,0,255,255,255,255,255", "crop keeps the right column of a copy");
    ok(img.width === 2, "and the original is untouched");
    refuses(() => img.copy().crop(1, 1, 2, 2), RangeError, /inside the 2x2 image/, "a crop outside the image is refused");
    const pngPath = dir + "/rt.png";
    img.export(pngPath);
    scratch.push(pngPath);
    const back = rl.loadImage(pngPath);
    ok(back.width === 2 && back.pixels().every((v, i) => v === px[i]), "export to PNG and loadImage round-trip every byte");
    const reloaded = rl.loadTexture(pngPath);
    ok(reloaded.width === 2 && reloaded.height === 2, "loadTexture reads the same file");
    refuses(() => rl.loadImage(dir + "/missing.png"), InternalError, /could not load an image/, "a missing image is refused");
    refuses(() => rl.loadTexture("x\u0000.png"), TypeError, /NUL/, "a path holding NUL is refused");
    refuses(() => new rl.Image(4, 4, new Uint8Array(15)), RangeError, /exactly 64 bytes/, "pixels of the wrong length are refused");
    refuses(() => new rl.Image(16385, 1), RangeError, /1\.\.16384/, "a side over 16384 is refused");
    refuses(() => new rl.Image(16384, 16384), RangeError, /pixel limit/, "an image over 2^26 pixels is refused");
    refuses(() => img.export(dir + "/x.unknown"), InternalError, /extension picks the format/, "an export with an unknown extension is refused");

    // --- text ------------------------------------------------------------------------------
    const tw = rl.measureText("Hi", 10);
    ok(tw > 0 && rl.measureText("HiHi", 10) > tw && rl.measureText("Hi", 20) > tw, "measureText grows with the text and the size (" + tw + ")");
    rt.draw(() => { rl.clearBackground(C(BG)); rl.drawText("Hi", 4, 6, 10, C(WHITE)); });
    const textImg = rt.readPixels();
    assertFrame(ok, textImg, new Frame(W, H, BG).oneOf(4, 6, tw, 10, [WHITE]), "text draws only inside its measured box, only in its colour");
    let lit = 0;
    for (let i = 0; i < textImg.pixels.length; i += 4) lit += textImg.pixels[i] === 255;
    ok(lit > 4, "and it draws something (" + lit + " pixels)");
    const m = rl.measureTextEx(null, "Hi", 10, 1);
    ok(m.width > 0 && m.height === 10, "measureTextEx with the default font (" + JSON.stringify(m) + ")");
    refuses(() => rl.loadFont(dir + "/missing.ttf"), InternalError, /could not load a font/, "a missing font is refused");
    refuses(() => rl.loadFont(file("junk.ttf", new Uint8Array(64).fill(7))), InternalError, /could not load a font/,
        "a file that is not a font is refused (raylib hands back its default font for it)");
    const sysFont = ["/System/Library/Fonts/Supplemental/Arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"].find((p) => { try { return os.stat(p)[1] === 0; } catch (e) { return false; } });
    if (sysFont) {
        const f = rl.loadFont(sysFont, 24);
        ok(f.baseSize === 24 && f.glyphCount >= 95, "loadFont rasterises a system TTF (" + f.glyphCount + " glyphs)");
        ok(rl.measureTextEx(f, "Hi", 24, 0).width > 0, "and measures with it");
    } else print("  note: no system TTF found, loadFont success path not run");

    // --- cameras -----------------------------------------------------------------------------
    rt.draw(() => {
        rl.clearBackground(C(BG));
        win.mode2D({ zoom: 2 }, () => rl.drawRectangle(1, 1, 2, 2, C(WHITE)));
    });
    assertFrame(ok, rt.readPixels(), new Frame(W, H, BG).rect(2, 2, 4, 4, WHITE), "mode2D zoom 2 doubles a rect's size and position");
    rt.draw(() => {
        rl.clearBackground(C(BG));
        win.mode2D({ offset: { x: 10, y: 5 }, target: { x: 1, y: 1 } }, () => rl.drawRectangle(1, 1, 3, 2, C(WHITE)));
    });
    assertFrame(ok, rt.readPixels(), new Frame(W, H, BG).rect(10, 5, 3, 2, WHITE), "mode2D maps target to offset");
    refuses(() => win.mode2D({ zoom: 0 }, () => {}), RangeError, /zoom must be greater than 0/, "zoom 0 is refused");
    refuses(() => win.mode2D({}, () => {}), TypeError, /inside frame\(\) or RenderTexture#draw\(\)/, "mode2D outside a frame is refused");
    rt.draw(() => {
        rl.clearBackground(C(BG));
        win.mode3D({ position: { x: 0, y: 0, z: 10 }, target: { x: 0, y: 0, z: 0 }, fovy: 45 }, () => rl.drawCube(0, 0, 0, 2, 2, 2, C(WHITE)));
    });
    const cube = rt.readPixels();
    let litC = 0, cx = 0, cy = 0;
    for (let i = 0; i < cube.pixels.length; i += 4) if (cube.pixels[i] === 255) { litC++; cx += (i / 4) % W; cy += ((i / 4 / W) | 0); }
    ok(litC > 0 && Math.abs(cx / litC - W / 2) < 2 && Math.abs(cy / litC - H / 2) < 2, "a cube at the origin seen down -Z lands centred (" + litC + " pixels)");
    refuses(() => win.mode3D({ projection: "fisheye" }, () => {}), TypeError, /perspective" or "orthographic/, "an unknown projection is refused");

    // --- frames, nesting and lifetimes ----------------------------------------------------
    win.frame(() => { rl.clearBackground(C(BG)); rl.drawTexture(rt, 0, 0); });
    refuses(() => win.frame(() => win.frame(() => {})), TypeError, /cannot run inside frame\(\)/, "frame() cannot nest");
    refuses(() => rt.draw(() => rt2.draw(() => {})), TypeError, /cannot run inside another draw\(\)/, "RenderTexture#draw cannot nest");
    refuses(() => rt.draw(() => rl.drawTexture(rt, 0, 0)), TypeError, /inside its own draw\(\)/, "a RenderTexture cannot be drawn into itself");
    refuses(() => rt.draw(() => rt.readPixels()), TypeError, /inside the same RenderTexture's draw\(\)/, "nor read inside its own draw()");
    refuses(() => win.frame(async () => {}), TypeError, /must be synchronous/, "an async draw callback is refused");
    let thrown = null;
    try { win.frame(() => { throw new RangeError("boom"); }); } catch (e) { thrown = e; }
    ok(thrown instanceof RangeError && thrown.message === "boom", "an exception in the callback propagates");
    win.frame(() => rl.clearBackground(C(BG)));
    ok(true, "and the next frame still works (the frame was ended)");
    const doomed = new rl.RenderTexture(4, 4);
    doomed.draw(() => { doomed.close(); rl.clearBackground(C(RED)); });
    ok(doomed.closed, "a RenderTexture closed inside its own draw() is released after it");
    const keepRt = new rl.RenderTexture(4, 4);
    win.frame(() => { rl.clearBackground(C(BG)); });
    refuses(() => rl.drawRectangle({ valueOf() { win.close(); return 0; } }, 0, 1, 1, 0xff), TypeError, /needs an open Window/,
        "an argument's valueOf that closes the window is caught before the draw");
    ok(win.closed, "and the coercion really closed it");
    refuses(() => keepRt.width, TypeError, /Window that owned this resource was closed/, "a RenderTexture dies with its window");
    refuses(() => tex.update(px), TypeError, /needs an open Window|was closed/, "so does a Texture");
    ok(img.width === 2 && img.pixels().length === 16, "an Image is CPU-side and outlives the window");

    win = new rl.Window(32, 32, "again", { hidden: true });
    ok(!win.closed, "a new window opens after the first closed");
    let insideClose = null;
    win.frame(() => { win.close(); try { rl.clearBackground(0); } catch (e) { insideClose = e; } });
    ok(insideClose instanceof TypeError && win.closed, "close() inside frame() takes effect at once for drawing and ends the window after the frame");
    win = new rl.Window(32, 32, "third", { hidden: true });

    // --- audio ---------------------------------------------------------------------------------
    const wav = (rate, ch, s16) => {
        const b = new Uint8Array(44 + s16.length * 2), dv = new DataView(b.buffer);
        const tag = (o, t) => { for (let i = 0; i < 4; i++) b[o + i] = t.charCodeAt(i); };
        tag(0, "RIFF"); dv.setUint32(4, 36 + s16.length * 2, true); tag(8, "WAVE"); tag(12, "fmt "); dv.setUint32(16, 16, true);
        dv.setUint16(20, 1, true); dv.setUint16(22, ch, true); dv.setUint32(24, rate, true); dv.setUint32(28, rate * ch * 2, true);
        dv.setUint16(32, ch * 2, true); dv.setUint16(34, 16, true); tag(36, "data"); dv.setUint32(40, s16.length * 2, true);
        s16.forEach((v, i) => dv.setInt16(44 + i * 2, v, true));
        return b;
    };
    const s16 = [0, 16384, -16384, 32767, -32768, 100];
    const wavPath = file("t.wav", wav(22050, 2, s16));
    const w = rl.loadWave(wavPath);
    ok(w.sampleRate === 22050 && w.channels === 2 && w.samples instanceof Float32Array && w.samples.length === 6, "loadWave decodes a stereo s16 WAV");
    ok(s16.every((v, i) => Math.abs(w.samples[i] - v / 32768) <= 1 / 32767 && Math.sign(w.samples[i]) === Math.sign(v)), "samples are the written values scaled to -1..1 (" + Array.from(w.samples).map((x) => x.toFixed(5)).join(",") + ")");
    refuses(() => rl.loadWave(dir + "/missing.wav"), InternalError, /could not load audio/, "a missing WAV is refused");
    refuses(() => rl.loadSound(wavPath), TypeError, /needs initAudio\(\) first/, "loadSound before initAudio is refused");
    let audio = true;
    try { rl.initAudio(); } catch (e) { audio = false; print("  note: no audio device here, playback cases skipped: " + e.message); }
    if (audio) {
        ok(rl.audioReady(), "initAudio opens the device");
        rl.setMasterVolume(0);
        const snd = rl.loadSound(wavPath);
        snd.setVolume(0);
        snd.play();
        ok(snd.playing === true, "a sound plays");
        snd.stop();
        ok(snd.playing === false, "and stops");
        refuses(() => snd.setPitch(0), RangeError, /0\.01\.\.16/, "pitch 0 is refused");
        const stream = new rl.AudioStream(44100, 1);
        ok(stream.processed === true || stream.processed === false, "a stream reports processed");
        refuses(() => new rl.AudioStream(44100, 3), RangeError, /1\.\.2/, "three channels are refused");
        refuses(() => stream.update(new Float64Array(4)), TypeError, /Float32Array/, "a stream takes Float32Array samples");
        const st2 = new rl.AudioStream(44100, 2);
        refuses(() => st2.update(new Float32Array(3)), RangeError, /whole frames of 2/, "a partial stereo frame is refused");
        rl.closeAudio();
        ok(!rl.audioReady(), "closeAudio closes the device");
        refuses(() => snd.playing, TypeError, /needs initAudio\(\) first|audio device was closed/, "a sound dies with the device");
    }

    win.close();
    for (const p of scratch) try { os.remove(p); } catch (e) {}
    os.remove(dir);
}
print("test_raylib: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_raylib: " + fail + " failed");
