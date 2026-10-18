// flags: --std
// dyna:raylib: the window controls, input queries, shapes, scoped modes, camera transforms and CPU
// Image drawing added for raylib 6. Expected values come from definitions, not from this binary:
// GL's additive blend (src*alpha + dst), the 2D camera transform (rotate(zoom*(p - target)) +
// offset), Bresenham lines that include both endpoints, and whole-image frames built from what was
// drawn (tests/lib/screen.js). Without a display or without CONFIG_RAYLIB the suite says SKIP.
import * as std from "std";
import { Frame, assertFrame, countColour } from "../lib/screen.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function refuses(fn, ctor, re, m) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    ok(e instanceof ctor && re.test(e.message), m + " (got " + e + ")");
}
const near = (a, b, eps) => Math.abs(a - b) <= eps;
const BG = [10, 20, 30, 255], RED = [230, 41, 55, 255], BLUE = [0, 121, 241, 255], WHITE = [255, 255, 255, 255];
const C = (c) => ((c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]) >>> 0;
const px = (img, x, y) => { const i = (y * img.width + x) * 4; return [img.pixels[i], img.pixels[i + 1], img.pixels[i + 2], img.pixels[i + 3]]; };
const same = (a, b) => a.every((v, i) => v === b[i]);

let rl = null;
try { rl = await import("dyna:raylib"); } catch (e) {}
if (!rl) print("test_raylib_more: SKIP (dyna:raylib is not in this build: CONFIG_RAYLIB=y)");
else {
    // --- CPU Image drawing: no window needed ----------------------------------------------------
    {
        const W = 24, H = 16;
        const im = new rl.Image(W, H, 0xff00ffff);
        im.clear(C(BG));
        assertFrame(ok, { width: W, height: H, pixels: im.pixels() }, new Frame(W, H, BG), "clear fills every pixel");
        im.drawRectangle(2, 3, 5, 4, C(RED));
        im.drawPixel(20, 1, C(BLUE));
        im.drawPixel(-1, -1, C(BLUE));
        im.drawPixel(W, H, C(BLUE));
        const f = new Frame(W, H, BG).rect(2, 3, 5, 4, RED).pixel(20, 1, BLUE);
        assertFrame(ok, { width: W, height: H, pixels: im.pixels() }, f, "drawRectangle and drawPixel land exactly; out-of-image pixels are clipped");
        im.drawLine(9, 12, 18, 12, C(WHITE));
        f.rect(9, 12, 10, 1, WHITE);
        assertFrame(ok, { width: W, height: H, pixels: im.pixels() }, f, "a horizontal drawLine covers both endpoints and nothing else");
        im.drawRectangle(-100, -100, 5, 5, C(RED));
        im.drawRectangle(W + 3, 0, 5, 5, C(RED));
        assertFrame(ok, { width: W, height: H, pixels: im.pixels() }, f, "a rectangle wholly outside the image draws nothing");

        const ring = new rl.Image(12, 10, C(BG));
        ring.drawRectangleLines(1, 1, 8, 6, C(WHITE));
        const rf = new Frame(12, 10, BG).rect(1, 1, 8, 1, WHITE).rect(1, 6, 8, 1, WHITE).rect(1, 1, 1, 6, WHITE).rect(8, 1, 1, 6, WHITE);
        assertFrame(ok, { width: 12, height: 10, pixels: ring.pixels() }, rf, "drawRectangleLines is a 1px border inside the rectangle");
        ring.clear(C(BG));
        ring.drawRectangleLines(1, 1, 8, 6, C(WHITE), 2);
        const rf2 = new Frame(12, 10, BG).rect(1, 1, 8, 6, WHITE).rect(3, 3, 4, 2, BG);
        assertFrame(ok, { width: 12, height: 10, pixels: ring.pixels() }, rf2, "thick 2 makes the border two pixels wide, still inside");

        const disc = new rl.Image(21, 21, C(BG));
        disc.drawCircle(10, 10, 6, C(RED));
        assertFrame(ok, { width: 21, height: 21, pixels: disc.pixels() }, new Frame(21, 21, BG).circle(10.5, 10.5, 6.5, RED, 1.5),
            "drawCircle fills a disc of the radius (rasteriser edge tolerated) and nothing else");
        ok(same(px({ width: 21, pixels: disc.pixels() }, 10, 10), RED) && same(px({ width: 21, pixels: disc.pixels() }, 0, 0), BG), "the centre is filled, a corner is not");

        const src = new rl.Image(2, 2, C(BLUE));
        const dst = new rl.Image(8, 8, C(BG));
        dst.drawImage(src, 3, 4);
        assertFrame(ok, { width: 8, height: 8, pixels: dst.pixels() }, new Frame(8, 8, BG).rect(3, 4, 2, 2, BLUE), "drawImage blits an opaque image at x, y");
        const clear = new rl.Image(2, 2, 0x00000000);
        dst.drawImage(clear, 0, 0);
        ok(same(px({ width: 8, pixels: dst.pixels() }, 0, 0), BG), "a transparent source leaves the destination unchanged");
        dst.drawImage(dst, 4, 0);
        ok(same(px({ width: 8, pixels: dst.pixels() }, 7, 4), BLUE) && same(px({ width: 8, pixels: dst.pixels() }, 3, 4), BLUE),
            "drawImage of an image onto itself reads the original pixels");

        refuses(() => im.drawImage({}, 0, 0), TypeError, /must be an Image/, "drawImage refuses a non-Image");
        refuses(() => im.drawLine(0, 0, 1, 1, C(RED), 0), RangeError, /1\.\.4096/, "a zero line thickness is refused");
        refuses(() => im.drawRectangle(0, 0, -1, 2, C(RED)), RangeError, /a size/, "a negative size is refused");
        refuses(() => im.drawPixel(1e9, 0, C(RED)), RangeError, /a coordinate/, "a coordinate far outside is refused");
        refuses(() => im.drawPixel(0.5, 0, C(RED)), RangeError, /integer/, "a fractional coordinate is refused");
        refuses(() => im.drawText("x", 0, 0, 10, C(RED)), TypeError, /needs an open Window/, "Image#drawText needs the default font, so a window");
        src.close();
        refuses(() => dst.drawImage(src, 0, 0), TypeError, /closed/, "a closed source is refused");
        refuses(() => src.clear(0), TypeError, /closed/, "a closed image refuses drawing");

        // The camera transform has no window dependence; its definition is rotate(zoom * (p - target)) + offset.
        const cam = { offset: { x: 100, y: 50 }, target: { x: 10, y: 20 }, zoom: 2, rotation: 0 };
        let s = rl.worldToScreen2D(15, 23, cam);
        ok(near(s.x, 110, 1e-4) && near(s.y, 56, 1e-4), "worldToScreen2D applies zoom and offset (" + JSON.stringify(s) + ")");
        let w = rl.screenToWorld2D(s.x, s.y, cam);
        ok(near(w.x, 15, 1e-3) && near(w.y, 23, 1e-3), "screenToWorld2D inverts it");
        const rot = { offset: { x: 0, y: 0 }, target: { x: 0, y: 0 }, zoom: 1, rotation: 90 };
        s = rl.worldToScreen2D(3, 0, rot);
        ok(near(s.x, 0, 1e-4) && near(s.y, 3, 1e-4), "rotation 90 turns +x onto +y (" + JSON.stringify(s) + ")");
        refuses(() => rl.worldToScreen2D(0, 0, { zoom: 0 }), RangeError, /zoom/, "a zero zoom is refused");
        refuses(() => rl.screenToWorld2D(NaN, 0, cam), RangeError, /finite/, "a NaN point is refused");
        refuses(() => rl.worldToScreen(0, 0, 0, { position: { x: 0, y: 0, z: 5 } }), TypeError, /needs an open Window/, "worldToScreen needs the window's size");
    }

    refuses(() => rl.drawRing(0, 0, 1, 2, 0, 360, 0, 0xff), TypeError, /needs an open Window/, "drawing before a window is refused");
    refuses(() => rl.touchCount(), TypeError, /needs an open Window/, "input before a window is refused");

    let win = null;
    try { win = new rl.Window(64, 48, "dyna:raylib more", { hidden: true }); } catch (e) {
        if (e instanceof InternalError && /InitWindow failed/.test(e.message)) print("test_raylib_more: SKIP (no display to open a window on): " + e.message);
        else throw e;
    }
    if (win) {
        const W = 32, H = 32;
        const rt = new rl.RenderTexture(W, H);

        // --- scoped modes -------------------------------------------------------------------
        rt.draw(() => {
            rl.clearBackground(C(BG));
            win.scissor(4, 6, 8, 10, () => rl.drawRectangle(0, 0, W, H, C(RED)));
        });
        assertFrame(ok, rt.readPixels(), new Frame(W, H, BG).rect(4, 6, 8, 10, RED), "scissor clips drawing to its rectangle of the target (top-left origin)");
        rt.draw(() => {
            rl.clearBackground(rl.rgba(50, 50, 50));
            win.blendMode("additive", () => rl.drawRectangle(8, 8, 8, 8, rl.rgba(100, 0, 0)));
            rl.drawRectangle(20, 20, 4, 4, rl.rgba(100, 0, 0));
        });
        assertFrame(ok, rt.readPixels(), new Frame(W, H, [50, 50, 50, 255]).rect(8, 8, 8, 8, [150, 50, 50, 255]).rect(20, 20, 4, 4, [100, 0, 0, 255]),
            "additive blending is src*alpha + dst, and it ends with its callback");
        refuses(() => win.blendMode("additive", () => {}), TypeError, /inside frame\(\) or RenderTexture#draw\(\)/, "blendMode outside a frame is refused");
        refuses(() => rt.draw(() => win.blendMode("glow", () => {})), RangeError, /'glow' is not one of/, "an unknown blend mode is refused by name");
        refuses(() => rt.draw(() => win.scissor(0, 0, 4, 4, () => win.scissor(0, 0, 2, 2, () => {}))), TypeError, /cannot nest/, "scissor does not nest in itself");
        refuses(() => rt.draw(() => win.blendMode("alpha", async () => {})), TypeError, /synchronous/, "an async callback is refused");
        refuses(() => rt.draw(() => win.scissor(0, 0, -1, 4, () => {})), RangeError, /scissor size/, "a negative scissor size is refused");
        let order = [];
        rt.draw(() => win.scissor(0, 0, 4, 4, () => win.blendMode("multiplied", () => order.push("inner"))));
        ok(order.join() === "inner", "blendMode nests inside scissor");
        let thrown = null;
        try { rt.draw(() => win.scissor(0, 0, 4, 4, () => { throw new Error("boom"); })); } catch (e) { thrown = e; }
        ok(thrown && thrown.message === "boom", "an exception inside scissor propagates");
        rt.draw(() => { rl.clearBackground(C(BG)); rl.drawRectangle(0, 0, W, H, C(RED)); });
        assertFrame(ok, rt.readPixels(), new Frame(W, H, RED), "after a throwing scissor the clip is gone");

        // --- shapes -------------------------------------------------------------------------
        rt.draw(() => {
            rl.clearBackground(C(BG));
            rl.drawRectangleGradient(0, 0, W, H, rl.rgba(255, 0, 0), rl.rgba(0, 255, 0), rl.rgba(0, 0, 255), rl.rgba(255, 255, 255));
        });
        {
            const img = rt.readPixels();
            const corner = (x, y, want, name) => ok(px(img, x, y).every((v, i) => near(v, want[i], 24)), name + " corner is its colour (" + px(img, x, y) + ")");
            corner(0, 0, [255, 0, 0, 255], "the top-left");
            corner(0, H - 1, [0, 255, 0, 255], "the bottom-left");
            corner(W - 1, 0, [0, 0, 255, 255], "the top-right");
            corner(W - 1, H - 1, [255, 255, 255, 255], "the bottom-right");
        }
        rt.draw(() => { rl.clearBackground(C(BG)); rl.drawRing(16, 16, 5, 10, 0, 360, 64, C(RED)); });
        {
            const img = rt.readPixels();
            ok(same(px(img, 16, 16), BG) && same(px(img, 23, 16), RED) && same(px(img, 16, 8), RED) && same(px(img, 0, 0), BG),
                "drawRing fills between the radii and leaves the hole");
        }
        rt.draw(() => { rl.clearBackground(C(BG)); rl.drawCircleSector(16, 16, 10, 0, 90, 32, C(RED)); });
        {
            const img = rt.readPixels();
            ok(same(px(img, 20, 20), RED) && same(px(img, 11, 11), BG) && same(px(img, 11, 20), BG) && same(px(img, 20, 11), BG),
                "a 0..90 sector covers only the quadrant between +x and +y");
        }
        const strip = new Float32Array([2.5, 2.5, 20.5, 2.5, 20.5, 20.5]);
        rt.draw(() => { rl.clearBackground(C(BG)); rl.drawLineStrip(strip, C(WHITE)); });
        {
            const img = rt.readPixels();
            ok(same(px(img, 10, 2), WHITE) && same(px(img, 20, 12), WHITE) && same(px(img, 10, 12), BG), "drawLineStrip joins its points and fills nothing between");
        }
        rt.draw(() => { rl.clearBackground(C(BG)); rl.drawLineStrip(strip, C(WHITE), 5); });
        {
            const img = rt.readPixels();
            ok(same(px(img, 10, 4), WHITE) && same(px(img, 10, 7), BG), "thick 5 widens the strip to about +-2.5px");
        }
        rt.draw(() => { rl.clearBackground(C(BG)); rl.drawTriangleFan(new Float32Array([4, 4, 4, 20, 20, 20, 20, 4]), C(BLUE)); });
        {
            const img = rt.readPixels();
            const n = countColour(img, 0, 0, W, H, BLUE);
            ok(same(px(img, 12, 12), BLUE) && same(px(img, 2, 2), BG) && n >= 16 * 16 - 32 && n <= 16 * 16 + 32,
                "a counter-clockwise fan of a square fills the square (" + n + " pixels)");
        }
        refuses(() => rl.drawLineStrip(new Float64Array(4), 0xff), TypeError, /Float32Array/, "a Float64Array of points is refused");
        refuses(() => rl.drawLineStrip(new Float32Array(3), 0xff), RangeError, /even number/, "an odd coordinate count is refused");
        refuses(() => rl.drawLineStrip(new Float32Array(2), 0xff), RangeError, /2 to 65536 points/, "one point is not a strip");
        refuses(() => rl.drawTriangleFan(new Float32Array(4), 0xff), RangeError, /3 to 65536 points/, "a fan needs three points");
        refuses(() => rl.drawLineStrip(new Float32Array([0, 0, NaN, 1]), 0xff), RangeError, /coordinate 2/, "a NaN coordinate is refused by position");
        refuses(() => rl.drawRing(0, 0, -1, 2, 0, 360, 0, 0xff), RangeError, /innerRadius/, "a negative radius is refused");
        {
            const pts = new Float32Array(new ArrayBuffer(16, { maxByteLength: 64 }));
            let e = null;
            try { rt.draw(() => rl.drawLineStrip(pts, { valueOf() { pts.buffer.transfer(); return 0xff; } })); } catch (x) { e = x; }
            ok(e instanceof TypeError && /detached/.test(e.message), "a colour whose valueOf detaches the points is refused by drawLineStrip, not read through (" + e + ")");
            ok(pts.buffer.detached, "the colour is coerced before the points are borrowed, so the detach itself went through");
        }

        // --- window state and input -------------------------------------------------------------
        ok(typeof win.fullscreen === "boolean" && typeof win.maximized === "boolean" && typeof win.resized === "boolean" && win.hidden === true,
            "the state getters answer booleans, and a hidden window says so");
        ok(typeof win.position.x === "number" && Number.isInteger(win.monitor), "position and monitor are numbers");
        const mons = rl.monitors();
        ok(Array.isArray(mons) && mons.every((m) => typeof m.name === "string" && m.width >= 0 && m.refreshRate >= 0), "monitors() describes each monitor");
        ok(mons.length === 0 || (win.monitor >= 0 && win.monitor < mons.length), "the window's monitor indexes monitors()");
        win.setResizable(true);
        win.setResizable(false);
        win.setAlwaysRun(true);
        win.setOpacity(1);
        win.setMinSize(16, 16);
        win.setMaxSize(4096, 4096);
        refuses(() => win.setResizable(1), TypeError, /boolean/, "a flag setter takes a boolean");
        refuses(() => win.setOpacity(1.5), RangeError, /0\.\.1/, "opacity over 1 is refused");
        refuses(() => win.setMinSize(0, 10), RangeError, /1\.\.16384/, "a zero minimum is refused");
        refuses(() => rt.draw(() => win.toggleFullscreen()), TypeError, /outside frame\(\)/, "a state change inside a draw is refused");
        refuses(() => rt.draw(() => win.setDecorated(false)), TypeError, /outside frame\(\)/, "a flag change inside a draw is refused");

        ok(rl.touchCount() === 0 && rl.touchPoint(0) === null, "no touch points on a desktop without touch");
        refuses(() => rl.touchPoint(8), RangeError, /0\.\.7/, "a touch index past 7 is refused");
        ok(rl.gesture() === "none", "no gesture has been made");
        const g = rl.gestureDetails();
        ok(typeof g.holdDuration === "number" && typeof g.drag.x === "number" && typeof g.pinchAngle === "number", "gestureDetails has every measurement");
        const wv = rl.mouseWheelV();
        ok(wv.x === 0 && wv.y === 0, "no wheel movement");
        ok(rl.getGamepadButtonPressed() === 0 || Number.isInteger(rl.getGamepadButtonPressed()), "getGamepadButtonPressed is a button code");
        ok(rl.gamepadAxisCount(0) === 0 || rl.isGamepadAvailable(0), "no axes without a pad");
        ok(rl.isGamepadButtonPressed(0, 1) === false && rl.isGamepadButtonReleased(0, 1) === false, "no pad presses without a pad");
        rl.setGamepadVibration(0, 0.5, 0.5, 0.1);
        refuses(() => rl.setGamepadVibration(0, 2, 0, 1), RangeError, /0\.\.1/, "a motor intensity over 1 is refused");
        refuses(() => rl.setGamepadVibration(4, 0, 0, 1), RangeError, /gamepad index/, "a pad index past 3 is refused");

        rl.hideCursor();
        ok(rl.isCursorHidden() === true, "hideCursor hides it");
        rl.showCursor();
        ok(rl.isCursorHidden() === false, "showCursor shows it");
        ok(typeof rl.isCursorOnScreen() === "boolean", "isCursorOnScreen answers");
        rl.setMouseCursor("pointer");
        rl.setMouseCursor("default");
        refuses(() => rl.setMouseCursor("hand"), RangeError, /'hand' is not one of/, "an unknown cursor is refused by name");
        refuses(() => rl.setMouseCursor(4), TypeError, /must be a string/, "a cursor is named, not numbered");

        const saved = rl.getClipboardText();
        rl.setClipboardText("dyna raylib é中");
        ok(rl.getClipboardText() === "dyna raylib é中", "the clipboard round-trips UTF-8 text");
        rl.setClipboardText(saved);
        refuses(() => rl.setClipboardText("a\ud800"), TypeError, /lone surrogate/, "clipboard text with a lone surrogate is refused");

        // Image#drawText needs the default font, which the window loaded.
        const ti = new rl.Image(40, 12, C(BG));
        ti.drawText("Hi", 1, 1, 10, C(WHITE));
        const tp = { width: 40, height: 12, pixels: ti.pixels() };
        const ink = countColour(tp, 0, 0, 40, 12, WHITE);
        ok(ink > 10 && countColour(tp, 20, 0, 20, 12, WHITE) === 0, "Image#drawText inks the start of the image only (" + ink + " pixels)");
        refuses(() => ti.drawText(5, 0, 0, 10, 0xff), TypeError, /text string/, "Image#drawText needs a string");

        const p3 = rl.worldToScreen(0, 0, 0, { position: { x: 0, y: 0, z: 10 }, target: { x: 0, y: 0, z: 0 } });
        ok(near(p3.x, win.size.width / 2, 0.5) && near(p3.y, win.size.height / 2, 0.5), "the camera's target projects to the window centre (" + JSON.stringify(p3) + ")");

        rt.close();
        win.close();
        refuses(() => win.maximize(), TypeError, /closed|needs an open Window/, "a closed window refuses state changes");
    }
}
print("test_raylib_more: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_raylib_more: " + fail + " failed");
