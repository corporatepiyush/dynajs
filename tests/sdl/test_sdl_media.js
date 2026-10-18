// flags: --std
// dyna:sdl text input, render targets, clip/viewport/scale, draw blend modes, geometry, image and
// WAV loading, and audio capture, on SDL's dummy drivers. Expected pixels are the colours drawn;
// expected samples come from the file this test wrote (s16 -> f32 is x / 32768 and u8 -> f32 is
// (x - 128) / 128, both exact in binary floating point). A build without CONFIG_SDL3 reports a SKIP.
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
function writeBytes(path, bytes) {
    const f = std.open(path, "wb");
    f.write(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    f.close();
}
function crc32(bytes) {
    let crc = 0xffffffff;
    for (const b of bytes) {
        let c = (crc ^ b) & 0xff;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        crc = (crc >>> 8) ^ c;
    }
    return (crc ^ 0xffffffff) >>> 0;
}
function adler32(bytes) {
    let a = 1, b = 0;
    for (const x of bytes) { a = (a + x) % 65521; b = (b + a) % 65521; }
    return ((b << 16) | a) >>> 0;
}
// 24-bit bottom-up BMP, rows padded to 4 bytes.
function bmp24(w, h, rgbAt) {
    const row = (w * 3 + 3) & ~3, size = 54 + row * h, b = new Uint8Array(size), dv = new DataView(b.buffer);
    b[0] = 0x42; b[1] = 0x4d;
    dv.setUint32(2, size, true); dv.setUint32(10, 54, true); dv.setUint32(14, 40, true);
    dv.setInt32(18, w, true); dv.setInt32(22, h, true); dv.setUint16(26, 1, true); dv.setUint16(28, 24, true);
    dv.setUint32(34, row * h, true);
    for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
            const [R, G, B] = rgbAt(x, h - 1 - y), o = 54 + y * row + x * 3;
            b[o] = B; b[o + 1] = G; b[o + 2] = R;
        }
    return b;
}
// RGBA PNG with one stored (uncompressed) deflate block.
function png(w, h, rgba) {
    const stride = 1 + w * 4, raw = new Uint8Array(h * stride);
    for (let y = 0; y < h; y++) raw.set(rgba.subarray(y * w * 4, (y + 1) * w * 4), y * stride + 1);
    const z = new Uint8Array(2 + 5 + raw.length + 4);
    z[0] = 0x78; z[1] = 0x01; z[2] = 1;
    z[3] = raw.length & 255; z[4] = raw.length >> 8; z[5] = ~raw.length & 255; z[6] = (~raw.length >> 8) & 255;
    z.set(raw, 7);
    new DataView(z.buffer).setUint32(7 + raw.length, adler32(raw));
    const chunk = (type, data) => {
        const c = new Uint8Array(12 + data.length), dv = new DataView(c.buffer);
        dv.setUint32(0, data.length);
        for (let i = 0; i < 4; i++) c[4 + i] = type.charCodeAt(i);
        c.set(data, 8);
        dv.setUint32(8 + data.length, crc32(c.subarray(4, 8 + data.length)));
        return c;
    };
    const ihdr = new Uint8Array(13), hv = new DataView(ihdr.buffer);
    hv.setUint32(0, w); hv.setUint32(4, h); ihdr[8] = 8; ihdr[9] = 6;
    const parts = [new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", z), chunk("IEND", new Uint8Array(0))];
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
}
// PCM WAV: bits 8 (unsigned) or 16 (signed little-endian).
function wav(freq, channels, bits, samples) {
    const bps = bits / 8, data = samples.length * bps, b = new Uint8Array(44 + data), dv = new DataView(b.buffer);
    const tag = (o, s) => { for (let i = 0; i < 4; i++) b[o + i] = s.charCodeAt(i); };
    tag(0, "RIFF"); dv.setUint32(4, 36 + data, true); tag(8, "WAVE"); tag(12, "fmt ");
    dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, channels, true);
    dv.setUint32(24, freq, true); dv.setUint32(28, freq * channels * bps, true);
    dv.setUint16(32, channels * bps, true); dv.setUint16(34, bits, true); tag(36, "data"); dv.setUint32(40, data, true);
    samples.forEach((s, i) => (bits === 16 ? dv.setInt16(44 + i * 2, s, true) : dv.setUint8(44 + i, s)));
    return b;
}

let sdl = null;
try { sdl = await import("dyna:sdl"); } catch (e) {}
if (!sdl) {
    print("test_sdl_media: SKIP (dyna:sdl is not in this build: CONFIG_SDL3=y)");
} else {
    sdl.init(["video", "audio"]);
    const dir = `${std.getenv("TMPDIR") || "/tmp"}/dyna_sdl_media_${os.getpid()}_${(Math.random() * 1e9) | 0}`;
    ok(os.mkdir(dir) === 0, "scratch directory " + dir);
    const scratch = [];
    const file = (name, bytes) => { const p = dir + "/" + name; writeBytes(p, bytes); scratch.push(p); return p; };

    const W = 16, H = 16;
    const win = new sdl.Window("media", W, H, { hidden: true });
    const r = new sdl.Renderer(win);
    const at = (img, x, y) => Array.from(img.pixels.subarray((y * img.width + x) * 4, (y * img.width + x) * 4 + 4)).join(",");
    const frame = () => r.readPixels();
    const clear = (cr, cg, cb) => { r.setDrawColor(cr, cg, cb); r.clear(); };

    // --- window: text input and limits ---------------------------------------
    ok(win.textInputActive === false, "text input is off until a window asks for it");
    win.startTextInput();
    ok(win.textInputActive === true, "startTextInput turns it on");
    win.setTextInputArea({ x: 2, y: 3, width: 10, height: 6 }, 4);
    win.setTextInputArea(null);
    refuses(() => win.setTextInputArea({ x: 0, y: 0, width: -1, height: 1 }), RangeError, /must not be negative/, "a negative input area is refused");
    refuses(() => win.setTextInputArea({ x: 0, y: 0, width: 4, height: 4 }, -1), RangeError, /cursor/, "a negative cursor is refused");
    win.stopTextInput();
    ok(win.textInputActive === false, "stopTextInput turns it off");
    ok(typeof win.displayScale === "number" && win.displayScale > 0, "displayScale is positive (" + win.displayScale + ")");
    win.setMinimumSize(8, 8);
    win.setMaximumSize(0, 0);
    win.setMinimumSize(0, 0);
    refuses(() => win.setMinimumSize(-1, 2), RangeError, /0\.\.16384/, "a negative minimum is refused");
    refuses(() => win.setMaximumSize(1, 16385), RangeError, /0\.\.16384/, "a maximum over 16384 is refused");
    let opacityErr = null;
    try { win.setOpacity(0.5); } catch (e) { opacityErr = e; }
    ok(opacityErr === null || /SDL_SetWindowOpacity/.test(opacityErr.message), "setOpacity works or names the SDL call that refused (" + opacityErr + ")");
    refuses(() => win.setOpacity(1.5), RangeError, /0\.\.1/, "an opacity over 1 is refused");
    refuses(() => win.setOpacity(NaN), RangeError, /0\.\.1/, "a NaN opacity is refused");

    // --- render targets ---------------------------------------------------------
    const BLACK = [0, 0, 0, 255], RED = [255, 0, 0, 255], GREEN = [0, 255, 0, 255], BLUE = [0, 0, 255, 255];
    const tgt = new sdl.Texture(r, 4, 4, { target: true });
    ok(tgt.target === true && new sdl.Texture(r, 1, 1).target === false, "target reports how the texture was made");
    r.setTarget(tgt);
    clear(255, 0, 0);
    assertFrame(ok, r.readPixels(), new Frame(4, 4, RED), "readPixels reads the whole current target");
    r.setTarget(null);
    clear(0, 0, 255);
    r.drawTexture(tgt, null, { x: 0, y: 0, width: 4, height: 4 });
    assertFrame(ok, frame(), new Frame(W, H, BLUE).rect(0, 0, 4, 4, RED), "the target lands where it is drawn and the rest keeps the window's clear colour");
    refuses(() => r.setTarget(new sdl.Texture(r, 2, 2)), TypeError, /\{ target: true \}/, "a streaming texture cannot be a target");
    refuses(() => new sdl.Texture(r, 2, 2, { targett: true }), TypeError, /unknown option 'targett'/, "an unknown texture option is refused by name");
    const w2 = new sdl.Window("other", 8, 8, { hidden: true }), r2 = new sdl.Renderer(w2);
    const foreign = new sdl.Texture(r2, 2, 2, { target: true });
    refuses(() => r.setTarget(foreign), TypeError, /another renderer/, "a target of another renderer is refused");
    const doomed = new sdl.Texture(r, 2, 2, { target: true });
    r.setTarget(doomed);
    doomed.close();
    clear(0, 255, 0);
    assertFrame(ok, frame(), new Frame(W, H, GREEN), "closing the current target returns drawing to the whole window");
    const tex2 = new sdl.Texture(r, 2, 2, { target: true });
    tex2.update(new Uint8Array(16).map((_, i) => [200, 100, 50, 255][i % 4]));
    r.drawTexture(tex2, null, { x: 0, y: 0, width: 2, height: 2 });
    assertFrame(ok, frame(), new Frame(W, H, GREEN).rect(0, 0, 2, 2, [200, 100, 50, 255]), "update() also fills a target texture");

    // --- clip rect, viewport, scale ---------------------------------------------
    clear(0, 0, 0);
    r.setClipRect({ x: 2, y: 2, width: 3, height: 3 });
    r.setDrawColor(255, 255, 255);
    r.fillRect(0, 0, W, H);
    r.setClipRect(null);
    const clipped = new Frame(W, H, BLACK).rect(2, 2, 3, 3, [255, 255, 255, 255]);
    assertFrame(ok, frame(), clipped, "the clip rect admits drawing inside it and nothing outside, anywhere on the screen");
    r.fillRect(10, 10, 1, 1);
    assertFrame(ok, frame(), clipped.pixel(10, 10, [255, 255, 255, 255]), "setClipRect(null) removes the clip");
    clear(0, 0, 0);
    r.setViewport({ x: 8, y: 8, width: 8, height: 8 });
    r.setDrawColor(255, 255, 0);
    r.fillRect(0, 0, 2, 2);
    r.setViewport(null);
    assertFrame(ok, frame(), new Frame(W, H, BLACK).rect(8, 8, 2, 2, [255, 255, 0, 255]), "the viewport moves the origin and keeps the rect's size");
    clear(0, 0, 0);
    r.setScale(4, 4);
    r.setDrawColor(0, 255, 255);
    r.fillRect(1, 1, 1, 1);
    r.setScale(1, 1);
    assertFrame(ok, frame(), new Frame(W, H, BLACK).rect(4, 4, 4, 4, [0, 255, 255, 255]), "setScale(4, 4) makes a 1x1 rect exactly 4x4 pixels at (4, 4)");
    refuses(() => r.setScale(0, 1), RangeError, /\(0, 1024\]/, "a zero scale is refused");
    refuses(() => r.setScale(1, NaN), RangeError, /\(0, 1024\]/, "a NaN scale is refused");
    refuses(() => r.setScale(1), TypeError, /setScale\(scaleX, scaleY\)/, "setScale needs both factors");

    // --- draw blend modes -------------------------------------------------------
    clear(0, 0, 200);
    r.setBlendMode("add");
    r.setDrawColor(100, 0, 0, 255);
    r.fillRect(0, 0, 2, 2);
    const added = new Frame(W, H, [0, 0, 200, 255]).rect(0, 0, 2, 2, [100, 0, 200, 255]);
    assertFrame(ok, frame(), added, "additive blending adds the source to the target, only inside the rect");
    r.setBlendMode("blend");
    r.setDrawColor(255, 255, 255, 0);
    r.fillRect(0, 0, W, H);
    assertFrame(ok, frame(), added, "alpha blending with alpha 0 leaves the whole target unchanged");
    r.setBlendMode("none");
    refuses(() => r.setBlendMode("screen"), RangeError, /unknown blend mode/, "an unknown blend mode is refused");
    refuses(() => r.setBlendMode(3), TypeError, /setBlendMode\(/, "a non-string blend mode is refused");
    let vsyncErr = null;
    try { r.setVSync(true); r.setVSync(false); } catch (e) { vsyncErr = e; }
    ok(vsyncErr === null || /SDL_SetRenderVSync/.test(vsyncErr.message), "setVSync works or names the SDL call that refused (" + vsyncErr + ")");
    refuses(() => r.setVSync(1), TypeError, /boolean/, "setVSync takes a boolean");

    // --- geometry ---------------------------------------------------------------
    const vtx = (x, y, R, G, B, A, u = 0, v = 0) => [x, y, R, G, B, A, u, v];
    const MAGENTA = [255, 0, 255, 255];
    const quad = new Float32Array([...vtx(2, 2, 1, 0, 1, 1), ...vtx(10, 2, 1, 0, 1, 1), ...vtx(10, 10, 1, 0, 1, 1), ...vtx(2, 10, 1, 0, 1, 1)]);
    const quadIdx = new Int32Array([0, 1, 2, 0, 2, 3]);
    clear(0, 0, 0);
    r.geometry(quad, quadIdx);
    assertFrame(ok, frame(), new Frame(W, H, BLACK).rectEdge(2, 2, 8, 8, MAGENTA), "an indexed quad covers its square and nothing else (edge pixels either way)");
    clear(0, 0, 0);
    r.geometry(new Float32Array([...vtx(0, 0, 0, 1, 0, 1), ...vtx(8, 0, 0, 1, 0, 1), ...vtx(0, 8, 0, 1, 0, 1)]));
    assertFrame(ok, frame(), new Frame(W, H, BLACK).triangle(0, 0, 8, 0, 0, 8, GREEN), "three vertices without indices are one triangle, and only that triangle");
    const yellow = new sdl.Texture(r, 1, 1);
    yellow.update(new Uint8Array([255, 255, 0, 255]));
    clear(0, 0, 0);
    r.geometry(new Float32Array([...vtx(0, 0, 1, 1, 1, 1, 0, 0), ...vtx(6, 0, 1, 1, 1, 1, 1, 0), ...vtx(6, 6, 1, 1, 1, 1, 1, 1), ...vtx(0, 6, 1, 1, 1, 1, 0, 1)]), quadIdx, yellow);
    assertFrame(ok, frame(), new Frame(W, H, BLACK).rectEdge(0, 0, 6, 6, [255, 255, 0, 255]), "a textured quad samples its texture over exactly its square");
    clear(0, 0, 0);
    r.geometry(new Float32Array(0));
    r.geometry(quad, new Int32Array(0));
    assertFrame(ok, frame(), new Frame(W, H, BLACK), "an empty vertex or index list draws nothing anywhere");
    refuses(() => r.geometry(new Float32Array(7)), RangeError, /8 floats per vertex, got 7 floats/, "a partial vertex is refused");
    refuses(() => r.geometry(new Float32Array(16)), RangeError, /vertex count must be a multiple of 3/, "two vertices are not a triangle");
    refuses(() => r.geometry(new Float64Array(24)), TypeError, /Float32Array/, "Float64Array vertices are refused");
    const nanVtx = new Float32Array(24);
    nanVtx[9] = NaN;
    refuses(() => r.geometry(nanVtx), RangeError, /vertex 1 holds a value that is not a finite number/, "a NaN in a vertex is refused, naming the vertex");
    const farVtx = new Float32Array(24);
    farVtx[16] = 2e7;
    refuses(() => r.geometry(farVtx), RangeError, /vertex 2 .*within \+-1e7/, "a coordinate beyond 1e7 is refused");
    refuses(() => r.geometry(quad, new Int32Array([0, 1, 4])), RangeError, /index 2 is 4, outside the 4 vertices/, "an index past the vertices is refused");
    refuses(() => r.geometry(quad, new Int32Array([0, 1, -1])), RangeError, /index 2 is -1/, "a negative index is refused");
    refuses(() => r.geometry(quad, new Int32Array([0, 1])), RangeError, /index count must be a multiple of 3/, "two indices are not a triangle");
    refuses(() => r.geometry(quad, new Uint32Array([0, 1, 2])), TypeError, /Int32Array/, "Uint32Array indices are refused");
    refuses(() => r.geometry(quad, quadIdx, foreign), TypeError, /another renderer/, "a texture of another renderer is refused");
    assertFrame(ok, frame(), new Frame(W, H, BLACK), "and no refused call drew anything");

    // --- images -------------------------------------------------------------------
    const RGB = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255]];
    const bmpPath = file("quad.bmp", bmp24(2, 2, (x, y) => RGB[y * 2 + x]));
    const bt = sdl.loadTexture(r, bmpPath);
    ok(bt instanceof sdl.Texture && bt.width === 2 && bt.height === 2 && bt.target === false, "loadTexture reads a BMP into a 2x2 Texture");
    clear(0, 0, 0);
    r.drawTexture(bt, null, { x: 0, y: 0, width: 2, height: 2 });
    assertFrame(ok, frame(), new Frame(W, H, BLACK).pixel(0, 0, [...RGB[0], 255]).pixel(1, 0, [...RGB[1], 255]).pixel(0, 1, [...RGB[2], 255]).pixel(1, 1, [...RGB[3], 255]),
        "every BMP pixel arrives where it was written, and nothing else is drawn");
    const pngPx = new Uint8Array([10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255]);
    const pt = sdl.loadTexture(r, file("row.png", png(3, 1, pngPx)));
    ok(pt.width === 3 && pt.height === 1, "loadTexture reads a PNG");
    clear(0, 0, 0);
    r.drawTexture(pt, null, { x: 0, y: 0, width: 3, height: 1 });
    assertFrame(ok, frame(), new Frame(W, H, BLACK).pixel(0, 0, [10, 20, 30, 255]).pixel(1, 0, [40, 50, 60, 255]).pixel(2, 0, [70, 80, 90, 255]),
        "every PNG pixel arrives as written");
    refuses(() => sdl.loadTexture(r, dir + "/missing.png"), Error, /SDL_LoadSurface/, "a missing image names SDL_LoadSurface");
    refuses(() => sdl.loadTexture(r, file("junk.png", new Uint8Array([1, 2, 3, 4, 5]))), Error, /SDL_LoadSurface/, "a file that is no image names SDL_LoadSurface");
    refuses(() => sdl.loadTexture(r, "a\u0000b.png"), TypeError, /NUL character/, "a path holding NUL is refused");
    refuses(() => sdl.loadTexture(r, 5), TypeError, /path string/, "a non-string path is refused");
    refuses(() => sdl.loadTexture(tgt, bmpPath), TypeError, /Renderer object expected/, "a Texture in the renderer slot is refused by class");
    w2.close();
    refuses(() => sdl.loadTexture(r2, bmpPath), TypeError, /window was closed/, "a renderer whose window closed is refused");

    // --- WAV --------------------------------------------------------------------------
    const s16 = [0, 16384, -16384, 32767, -32768, 1];
    const w16 = sdl.loadWAV(file("s16.wav", wav(22050, 2, 16, s16)));
    ok(w16.freq === 22050 && w16.channels === 2 && w16.samples instanceof Float32Array && w16.samples.length === 6, "loadWAV reads a stereo s16 file");
    ok(s16.every((v, i) => w16.samples[i] === v / 32768), "s16 samples arrive as x / 32768 (" + Array.from(w16.samples).join(",") + ")");
    const u8 = [128, 0, 255, 64];
    const w8 = sdl.loadWAV(file("u8.wav", wav(8000, 1, 8, u8)));
    ok(w8.freq === 8000 && w8.channels === 1 && u8.every((v, i) => w8.samples[i] === (v - 128) / 128), "u8 samples arrive as (x - 128) / 128 (" + Array.from(w8.samples).join(",") + ")");
    refuses(() => sdl.loadWAV(dir + "/missing.wav"), Error, /SDL_LoadWAV/, "a missing WAV names SDL_LoadWAV");
    refuses(() => sdl.loadWAV(file("short.wav", wav(8000, 1, 16, [1, 2]).subarray(0, 20))), Error, /SDL_LoadWAV/, "a truncated WAV names SDL_LoadWAV");
    refuses(() => sdl.loadWAV("x\u0000.wav"), TypeError, /NUL character/, "a WAV path holding NUL is refused");

    // --- audio devices and capture ------------------------------------------------
    const plays = sdl.audioDevices(), recs = sdl.audioDevices({ recording: true });
    const shaped = (l) => Array.isArray(l) && l.every((d) => Number.isInteger(d.id) && d.id > 0 && typeof d.name === "string");
    ok(shaped(plays) && shaped(recs), "audioDevices lists ids and names (" + plays.length + " playback, " + recs.length + " recording)");
    refuses(() => sdl.audioDevices({ recordng: true }), TypeError, /unknown option 'recordng'/, "an unknown audioDevices option is refused by name");
    const out = new sdl.AudioStream();
    ok(out.recording === false && out.available === 0, "a playback stream reports recording false");
    refuses(() => out.get(), TypeError, /get\(\) reads a recording stream/, "get() on a playback stream is refused");
    out.close();
    const mic = new sdl.AudioStream({ recording: true, freq: 16000, channels: 2, format: "s16" });
    ok(mic.recording === true, "a recording stream reports recording true");
    refuses(() => mic.put(new Int16Array(4)), TypeError, /recording stream is read with get\(\)/, "put() on a recording stream is refused");
    refuses(() => mic.get(0), RangeError, /1\.\.16777216/, "get(0) is refused");
    mic.resume();
    let got = mic.get();
    for (let i = 0; i < 100 && got.length === 0; i++) { os.sleep(20); got = mic.get(); }
    ok(got instanceof Int16Array && got.length > 0 && got.length % 2 === 0, "captured samples arrive as whole s16 frames (" + got.length + ")");
    if (sdl.currentAudioDriver() === "dummy") ok(got.every((v) => v === 0), "the dummy driver records silence");
    else print("  note: not the dummy audio driver, the silence check does not apply");
    const few = mic.get(3);
    ok(few.length <= 2 && few.length % 2 === 0, "get(maxSamples) rounds down to whole frames (" + few.length + ")");
    mic.close();
    const fmic = new sdl.AudioStream({ recording: true, channels: 1 });
    fmic.resume();
    let fgot = fmic.get();
    for (let i = 0; i < 100 && fgot.length === 0; i++) { os.sleep(20); fgot = fmic.get(); }
    ok(fgot instanceof Float32Array && fgot.length > 0, "an f32 recording stream yields a Float32Array (" + fgot.length + ")");
    fmic.close();
    refuses(() => new sdl.AudioStream({ device: 0 }), RangeError, /an id from audioDevices/, "device 0 is refused");
    refuses(() => new sdl.AudioStream({ device: 1.5 }), RangeError, /an id from audioDevices/, "a fractional device id is refused");
    refuses(() => new sdl.AudioStream({ device: 987654 }), RangeError, /no audio device has id 987654/, "an id no device has is refused, naming it");
    if (plays.length) refuses(() => new sdl.AudioStream({ recording: true, device: plays[0].id }), TypeError, /is a playback device but recording is true/, "a playback device cannot record");
    else print("  note: no playback device listed, the mismatch check did not run");
    if (recs.length) refuses(() => new sdl.AudioStream({ device: recs[0].id }), TypeError, /is a recording device but recording is false/, "a recording device cannot play");
    else print("  note: no recording device listed, the mismatch check did not run");

    r.close();
    win.close();
    for (const p of scratch) os.remove(p);
    os.remove(dir);
    sdl.quit();
}
print("test_sdl_media: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_sdl_media: " + fail + " failed");
