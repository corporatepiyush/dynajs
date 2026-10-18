// flags: --std
// dyna:raylib: the names no other suite asserts -- window geometry and state setters read back
// through their getters, setTargetFPS against the wall clock (a LOWER bound only), the input
// queries' resting values and range refusals, shape outlines and fills by their geometry, the 3D
// primitives inside mode3D, Image resize/flip/circle outlines, texture filters, and Music/Sound
// controls on a WAV this test writes. Expected values come from definitions (a regular polygon's
// extent, an ellipse's bounding box, drawText == drawTextEx at spacing fontSize/10) and from the
// d.ts contract. Without a display or without CONFIG_RAYLIB the suite says SKIP.
import * as os from "os";
import * as std from "std";
import { countColour } from "../lib/screen.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function refuses(fn, ctor, re, m) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    ok(e instanceof ctor && re.test(e.message), m + " (got " + e + ")");
}
const BG = [0, 0, 0, 255], WHITE = [255, 255, 255, 255];
const C = (c) => ((c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]) >>> 0;

function inked(img) {
    const p = img.pixels, out = [];
    for (let y = 0; y < img.height; y++)
        for (let x = 0; x < img.width; x++) {
            const i = (y * img.width + x) * 4;
            if (p[i] | p[i + 1] | p[i + 2]) out.push([x, y]);
        }
    return out;
}
function bbox(pts) {
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const [x, y] of pts) { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
    return { x0, y0, x1, y1, w: x1 - x0 + 1, h: y1 - y0 + 1 };
}
const isInk = (img, x, y) => { const i = (y * img.width + x) * 4; return (img.pixels[i] | img.pixels[i + 1] | img.pixels[i + 2]) !== 0; };

let rl = null;
try { rl = await import("dyna:raylib"); } catch (e) {}
let win = null;
if (rl) {
    rl.setLogLevel("error");
    try { win = new rl.Window(64, 48, "surface", { hidden: true }); } catch (e) {
        if (e instanceof InternalError && /InitWindow failed/.test(e.message)) print("test_raylib_surface: SKIP (no display to open a window on): " + e.message);
        else throw e;
    }
}
if (!rl) print("test_raylib_surface: SKIP (dyna:raylib is not in this build: CONFIG_RAYLIB=y)");
else if (win) {
    const shot = (w, h, draw) => { const rt = new rl.RenderTexture(w, h); rt.draw(() => { rl.clearBackground(C(BG)); draw(); }); const img = rt.readPixels(); rt.close(); return img; };

    // --- window getters and setters ------------------------------------------------------------
    ok(win.focused === true || win.focused === false, "focused is a boolean");
    ok(win.minimized === false, "a freshly opened window is not minimized");
    ok(win.shouldClose === false, "nothing has asked a fresh window to close");
    ok(win.scaleDPI.x > 0 && win.scaleDPI.y > 0, `scaleDPI is positive (${JSON.stringify(win.scaleDPI)})`);
    ok(win.renderSize.width >= win.size.width && win.renderSize.height >= win.size.height, "renderSize is at least size");
    win.setSize(80, 50);
    ok(win.size.width === 80 && win.size.height === 50, `setSize is what size reports (${JSON.stringify(win.size)})`);
    ok(win.renderSize.width % 80 === 0 && win.renderSize.width / 80 === win.renderSize.height / 50, `and renderSize follows it at one density (${JSON.stringify(win.renderSize)})`);
    win.setSize(120, 70);
    win.setSize(80, 50);
    ok(win.size.width === 80 && win.size.height === 50, `a second resize does not compound the display scale (${JSON.stringify(win.size)})`);
    const sized = win.size.width;
    refuses(() => win.setSize(0, 10), RangeError, /1\.\.16384/, "a zero width is refused");
    win.setPosition(30, 140);
    const p1 = win.position;
    win.setPosition(70, 140);
    const p2 = win.position;
    ok(p1.x === 30 && p2.x === 70 && p2.y === p1.y, `setPosition moves the window to x and keeps y (the OS may clamp y below its menu bar) (${JSON.stringify([p1, p2])})`);
    win.minimize();
    win.restore();
    ok(win.minimized === false, "restore() leaves it not minimized");
    win.focus();
    win.toggleBorderless();
    win.toggleBorderless();
    win.setTopmost(true);
    win.setTopmost(false);
    win.setMousePassthrough(true);
    win.setMousePassthrough(false);
    ok(!win.closed && win.size.width === sized, "minimize/restore/focus/toggleBorderless twice/setTopmost/setMousePassthrough leave a live window of the same size");
    for (const m of ["setTopmost", "setMousePassthrough"]) refuses(() => win[m](1), TypeError, /needs a boolean/, `${m} takes a boolean`);
    for (const m of ["minimize", "restore", "focus", "toggleBorderless"]) {
        let e = null;
        win.frame(() => { try { win[m](); } catch (x) { e = x; } });
        ok(e instanceof TypeError, `${m}() inside frame() is refused (${e})`);
    }

    win.setTargetFPS(30);
    const t0 = Date.now();
    for (let i = 0; i < 7; i++) win.frame(() => rl.clearBackground(C(BG)));
    const took = Date.now() - t0;
    ok(took >= 6 * (1000 / 30) * 0.8, `setTargetFPS(30) makes seven frames wait out six slots (${took} ms >= ${(6 * 1000 / 30 * 0.8).toFixed(0)})`);
    ok(win.frameTime >= (1 / 30) * 0.8, `frameTime is about one slot at 30 fps (${win.frameTime})`);
    ok(Number.isInteger(win.fps) && win.fps >= 0 && win.fps <= 31, `fps is an integer no higher than the cap (${win.fps})`);
    win.setTargetFPS(0);
    refuses(() => win.setTargetFPS(1001), RangeError, /0\.\.1000/, "fps over 1000 is refused");
    win.setExitKey(0);
    win.setExitKey(rl.keys.Q);
    ok(win.shouldClose === false, "setExitKey does not itself close the window");
    refuses(() => win.setExitKey(512), RangeError, /0\.\.511/, "an exit key outside 0..511 is refused");

    // --- input queries at rest -------------------------------------------------------------------
    win.frame(() => {});
    ok(rl.isKeyDown(rl.keys.A) === false && rl.isKeyPressed(rl.keys.A) === false && rl.isKeyReleased(rl.keys.A) === false, "no key is down, pressed or released");
    ok(rl.getKeyPressed() === 0 && rl.getCharPressed() === "", "the key and char queues are empty (0 and \"\")");
    const mp = rl.mousePosition(), md = rl.mouseDelta();
    ok(Number.isFinite(mp.x) && Number.isFinite(mp.y) && md.x === 0 && md.y === 0 && rl.mouseWheel() === 0, "the mouse has a position, no delta and no wheel");
    for (const b of Object.values(rl.mouseButtons))
        ok(rl.isMouseButtonDown(b) === false && rl.isMouseButtonPressed(b) === false && rl.isMouseButtonReleased(b) === false, `mouse button ${b} is idle`);
    refuses(() => rl.isKeyDown(512), RangeError, /key code .*0\.\.511/, "a key code over 511 is refused");
    refuses(() => rl.isMouseButtonDown(7), RangeError, /mouse button .*0\.\.6/, "mouse button 7 is refused");
    const pad = rl.isGamepadAvailable(0);
    ok(pad ? typeof rl.gamepadName(0) === "string" : rl.gamepadName(0) === null, "gamepadName is a string for a connected pad and null otherwise");
    if (!pad) ok(rl.gamepadAxis(0, rl.gamepadAxes.LEFT_X) === 0 && rl.isGamepadButtonDown(0, rl.gamepadButtons.RIGHT_FACE_DOWN) === false, "an absent pad reads axis 0 and no button");
    refuses(() => rl.gamepadAxis(4, 0), RangeError, /gamepad index .*0\.\.3/, "gamepad 4 is refused");
    refuses(() => rl.gamepadAxis(0, 6), RangeError, /gamepad axis .*0\.\.5/, "axis 6 is refused");
    refuses(() => rl.isGamepadButtonDown(0, 18), RangeError, /0\.\.17/, "button 18 is refused");
    rl.disableCursor();
    ok(rl.isCursorHidden() === true, "disableCursor() hides the cursor");
    rl.enableCursor();
    ok(rl.isCursorHidden() === false, "enableCursor() shows it again");

    // --- 2D shapes by geometry -------------------------------------------------------------------
    {
        const ring = shot(41, 41, () => rl.drawCircleLines(20, 20, 12, C(WHITE)));
        const pts = inked(ring);
        ok(pts.length > 0 && pts.every(([x, y]) => Math.abs(Math.hypot(x + 0.5 - 20, y + 0.5 - 20) - 12) <= 1.5), `drawCircleLines inks only pixels within a pixel of radius 12 (${pts.length})`);
        const rb = bbox(pts);
        ok(!isInk(ring, 20, 20) && Math.abs(rb.w - 24) <= 1 && Math.abs(rb.h - 24) <= 1, `it leaves the centre empty and spans the 24-pixel diameter (${rb.w}x${rb.h})`);
        const disc = shot(41, 41, () => rl.drawCircle(20, 20, 12, C(WHITE)));
        ok(pts.length < inked(disc).length / 3, "an outline is far fewer pixels than the disc");

        const ell = shot(61, 41, () => rl.drawEllipse(30, 20, 20, 8, C(WHITE)));
        const eb = bbox(inked(ell));
        ok(Math.abs(eb.w - 40) <= 2 && Math.abs(eb.h - 16) <= 2 && isInk(ell, 30, 20), `drawEllipse fills a 2rx x 2ry box around its centre (${eb.w}x${eb.h})`);
        ok(inked(ell).every(([x, y]) => ((x + 0.5 - 30) / 20) ** 2 + ((y + 0.5 - 20) / 8) ** 2 <= 1.25), "and nothing outside the ellipse");

        const sq = shot(41, 41, () => rl.drawPoly(20, 20, 4, 10, 45, C(WHITE)));
        const sb = bbox(inked(sq));
        ok(Math.abs(sb.w - 10 * Math.SQRT2) <= 2 && Math.abs(sb.h - 10 * Math.SQRT2) <= 2, `drawPoly(4 sides, radius 10, rotated 45) is an axis-aligned square of side r*sqrt2 (${sb.w}x${sb.h})`);
        const dia = shot(41, 41, () => rl.drawPoly(20, 20, 4, 10, 0, C(WHITE)));
        const db = bbox(inked(dia));
        ok(Math.abs(db.w - 20) <= 2 && Math.abs(db.h - 20) <= 2 && !isInk(dia, 13, 13), `unrotated it is a diamond: 2r across and an empty corner (${db.w}x${db.h})`);
        const tri = shot(41, 41, () => rl.drawPoly(20, 20, 3, 10, 0, C(WHITE)));
        ok(inked(tri).length < inked(sq).length, "three sides cover less than four at the same radius");

        const plain = shot(40, 30, () => rl.drawRectangle(5, 4, 30, 20, C(WHITE)));
        const flat = shot(40, 30, () => rl.drawRectangleRounded(5, 4, 30, 20, 0, 0, C(WHITE)));
        ok(inked(plain).length === 600 && inked(flat).length === 600, `roundness 0 is the plain rectangle (${inked(flat).length} of 600)`);
        const round = shot(40, 30, () => rl.drawRectangleRounded(5, 4, 30, 20, 1, 16, C(WHITE)));
        ok(!isInk(round, 5, 4) && !isInk(round, 34, 23) && isInk(round, 20, 4) && isInk(round, 5, 14) && isInk(round, 20, 14), "roundness 1 cuts the corners and keeps the edge midpoints and centre");
        refuses(() => rl.drawRectangleRounded(0, 0, 4, 4, 1.5, 0, 0xffffffff), RangeError, /0\.\.1/, "roundness over 1 is refused");

        const a = shot(80, 30, () => rl.drawText("Hi!", 2, 2, 20, C(WHITE)));
        const b = shot(80, 30, () => rl.drawTextEx(null, "Hi!", 2, 2, 20, 2, C(WHITE)));
        ok(inked(a).length > 0 && inked(a).length === inked(b).length && a.pixels.every((v, i) => v === b.pixels[i]), "drawTextEx(null, ..., spacing fontSize/10) is drawText (raylib's DrawText definition)");
        const wide = shot(80, 30, () => rl.drawTextEx(null, "Hi!", 2, 2, 20, 8, C(WHITE)));
        ok(bbox(inked(wide)).w > bbox(inked(b)).w, "a larger spacing widens the text");
        const fps = shot(100, 30, () => rl.drawFPS(2, 2));
        ok(inked(fps).length > 0 && bbox(inked(fps)).y0 >= 2 && bbox(inked(fps)).x0 >= 2, "drawFPS inks text below and right of its origin");
    }

    // --- 3D primitives -------------------------------------------------------------------------
    {
        const cam = { position: { x: 0, y: 0, z: 10 }, target: { x: 0, y: 0, z: 0 }, fovy: 45 };
        const top = { position: { x: 0, y: 10, z: 0.001 }, target: { x: 0, y: 0, z: 0 }, fovy: 45 };
        const d3 = (camera, draw) => shot(64, 64, () => win.mode3D(camera, draw));
        const sph = d3(cam, () => rl.drawSphere(0, 0, 0, 2, C(WHITE)));
        ok(isInk(sph, 32, 32) && !isInk(sph, 2, 2), "drawSphere at the origin covers the centre and not a corner");
        const wires = d3(cam, () => rl.drawCubeWires(0, 0, 0, 4, 4, 4, C(WHITE)));
        const solid = d3(cam, () => rl.drawCube(0, 0, 0, 4, 4, 4, C(WHITE)));
        ok(inked(wires).length > 0 && !isInk(wires, 32, 32) && isInk(solid, 32, 32), "drawCubeWires draws edges and leaves the face centre empty, where drawCube fills it");
        ok(inked(wires).length < inked(solid).length / 2, "and covers far fewer pixels than the solid cube");
        const plane = d3(top, () => rl.drawPlane(0, 0, 0, 4, 4, C(WHITE)));
        const edgeOn = d3(cam, () => rl.drawPlane(0, 0, 0, 4, 4, C(WHITE)));
        ok(isInk(plane, 32, 32) && inked(plane).length > 10 * inked(edgeOn).length, "drawPlane is an XZ plane: filled seen from above, a sliver edge-on");
        const line = d3(cam, () => rl.drawLine3D(-3, 0, 0, 3, 0, 0, C(WHITE)));
        const lb = bbox(inked(line));
        ok(lb.h <= 2 && lb.w > 20 && lb.y0 >= 30 && lb.y1 <= 33, `drawLine3D along x is a horizontal line through the centre (${JSON.stringify(lb)})`);
        const grid = d3(top, () => rl.drawGrid(4, 1));
        const grid2 = d3(top, () => rl.drawGrid(8, 1));
        ok(inked(grid).length > 0 && bbox(inked(grid2)).w > bbox(inked(grid)).w, "drawGrid draws lines and more slices span wider");
        refuses(() => shot(4, 4, () => win.mode3D(cam, () => rl.drawGrid(0, 1))), RangeError, /1\.\.1024/, "zero slices are refused");
    }

    // --- Images and textures -------------------------------------------------------------------
    {
        const im = new rl.Image(4, 2, C([200, 10, 20, 255]));
        im.resize(9, 5);
        const p = im.pixels();
        ok(im.width === 9 && im.height === 5 && p.length === 9 * 5 * 4, "resize changes the size and the byte count");
        let uniform = true;
        for (let i = 0; i < p.length; i += 4) if (p[i] !== 200 || p[i + 1] !== 10 || p[i + 2] !== 20 || p[i + 3] !== 255) uniform = false;
        ok(uniform, "a uniform image stays that colour at every pixel after resizing");
        refuses(() => im.resize(0, 2), RangeError, /1\.\.16384/, "resize to zero is refused");
        const col = new rl.Image(2, 3, C(BG));
        col.drawPixel(0, 0, C(WHITE));
        col.drawPixel(1, 1, C([1, 2, 3, 255]));
        const before = col.pixels();
        col.flipVertical();
        const after = col.pixels();
        let mirrored = true;
        for (let y = 0; y < 3; y++) for (let i = 0; i < 8; i++) if (after[y * 8 + i] !== before[(2 - y) * 8 + i]) mirrored = false;
        ok(mirrored, "flipVertical mirrors the rows: row y becomes row height-1-y");
        col.flipVertical();
        ok(col.pixels().every((v, i) => v === before[i]), "and twice is the identity");
        const ring = new rl.Image(21, 21, C(BG));
        ring.drawCircleLines(10, 10, 6, C(WHITE));
        const rp = inked({ width: 21, height: 21, pixels: ring.pixels() });
        ok(rp.length > 0 && rp.every(([x, y]) => Math.abs(Math.hypot(x - 10, y - 10) - 6) <= 1), `Image#drawCircleLines inks only pixels within a pixel of radius 6 (${rp.length})`);
        ok(rp.some(([x, y]) => x === 10 && y === 4) && rp.some(([x, y]) => x === 16 && y === 10) && !rp.some(([x, y]) => x === 10 && y === 10), "it reaches the top and right and leaves the centre");
        const sym = rp.every(([x, y]) => rp.some(([u, v]) => u === 20 - x && v === y) && rp.some(([u, v]) => u === x && v === 20 - y));
        ok(sym, "the outline is symmetric about both axes");

        const t = new rl.Image(2, 2, C(WHITE)).toTexture();
        for (const m of ["point", "bilinear", "trilinear"]) t.setFilter(m);
        refuses(() => t.setFilter("nearest"), RangeError, /point, bilinear, trilinear/, "an unknown filter is refused, naming the three");
        const rt = new rl.RenderTexture(4, 4);
        rt.setFilter("bilinear");
        refuses(() => rt.setFilter(""), RangeError, /unknown filter/, "RenderTexture#setFilter refuses the same way");
        t.dispose();
        rt.dispose();
        ok(t.closed && rt.closed, "Texture.dispose() and RenderTexture.dispose() close them");
        refuses(() => t.setFilter("point"), TypeError, /closed/, "setFilter on a disposed texture is refused");
    }

    // --- Music and Sound controls ----------------------------------------------------------------
    {
        const dir = `${std.getenv("TMPDIR") || "/tmp"}/dyna_raylib_surface_${os.getpid()}_${(Math.random() * 1e9) | 0}`;
        os.mkdir(dir);
        const rate = 8000, n = rate * 2;
        const b = new Uint8Array(44 + n * 2), dv = new DataView(b.buffer);
        const tag = (o, s) => { for (let i = 0; i < 4; i++) b[o + i] = s.charCodeAt(i); };
        tag(0, "RIFF"); dv.setUint32(4, 36 + n * 2, true); tag(8, "WAVE"); tag(12, "fmt "); dv.setUint32(16, 16, true);
        dv.setUint16(20, 1, true); dv.setUint16(22, 1, true); dv.setUint32(24, rate, true); dv.setUint32(28, rate * 2, true);
        dv.setUint16(32, 2, true); dv.setUint16(34, 16, true); tag(36, "data"); dv.setUint32(40, n * 2, true);
        for (let i = 0; i < n; i++) dv.setInt16(44 + i * 2, Math.round(1000 * Math.sin(i / 10)), true);
        const path = dir + "/two-seconds.wav", f = std.open(path, "wb");
        f.write(b.buffer, 0, b.length);
        f.close();
        let audio = true;
        try { rl.initAudio(); } catch (e) { audio = false; print("  note: no audio device here, playback cases skipped: " + e.message); }
        if (audio) {
            rl.setMasterVolume(0);
            const mus = rl.loadMusic(path);
            ok(Math.abs(mus.length - 2) < 0.01, `a 2 s WAV streams as 2 s of Music (${mus.length})`);
            ok(mus.played === 0 && mus.playing === false, "nothing played before play()");
            mus.setLooping(false);
            mus.setPan(0.25);
            mus.play();
            mus.update();
            ok(mus.playing === true, "play() starts it");
            mus.pause();
            ok(mus.playing === false, "pause() stops it reporting playing");
            mus.resume();
            ok(mus.playing === true, "resume() picks it up again");
            mus.seek(1.5);
            ok(Math.abs(mus.played - 1.5) < 0.01, `seek(1.5) moves the play head there (${mus.played})`);
            mus.update();
            refuses(() => mus.seek(3), RangeError, /past the end/, "seeking past the length is refused");
            refuses(() => mus.setPan(1.5), RangeError, /0\.\.1/, "a pan over 1 is refused");
            refuses(() => mus.setLooping(1), TypeError, /boolean/, "setLooping takes a boolean");
            mus.stop();
            ok(mus.playing === false, "stop() stops it");
            mus.dispose();
            ok(mus.closed, "Music.dispose() closes it");
            refuses(() => mus.play(), TypeError, /closed/, "a disposed Music is refused");
            refuses(() => rl.loadMusic(dir + "/missing.wav"), InternalError, /could not load/, "a missing file is refused");

            const snd = rl.loadSound(path);
            snd.setPan(0.75);
            snd.play();
            snd.pause();
            ok(snd.playing === false, "Sound#pause stops it reporting playing");
            snd.resume();
            ok(snd.playing === true, "Sound#resume picks it up again");
            refuses(() => snd.setPan(-0.1), RangeError, /0\.\.1/, "a negative pan is refused");
            snd.stop();
            snd.dispose();
            ok(snd.closed, "Sound.dispose() closes it");
            rl.closeAudio();
        }
        os.remove(path);
        os.remove(dir);
    }

    win.dispose();
    ok(win.closed, "Window.dispose() closes it");
    refuses(() => win.setSize(10, 10), TypeError, /closed/, "a disposed window refuses setSize");
}
print("test_raylib_surface: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_raylib_surface: " + fail + " failed");
