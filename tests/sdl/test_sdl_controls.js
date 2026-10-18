// flags: --std
// dyna:sdl on the dummy drivers: window state, cursors, gamepads (driven through SDL's own virtual
// gamepad, so no device is needed), renderer debug text, logical presentation, rotated and flipped
// textures, and system information. Expected pixels come from definitions: a 180-degree turn about
// the centre maps (x, y) to (w-1-x, h-1-y), and SDL's debug font draws each character in an 8x8 cell.
// A build without CONFIG_SDL3 reports a SKIP.
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

let sdl = null;
try { sdl = await import("dyna:sdl"); } catch (e) {}
if (!sdl) {
    print("test_sdl_controls: SKIP (dyna:sdl is not in this build: CONFIG_SDL3=y)");
} else {
    refuses(() => sdl.gamepads(), TypeError, /init\(\["gamepad"\]\) first/, "gamepads() before init is refused");
    refuses(() => sdl.attachVirtualGamepad(), TypeError, /init\(\["gamepad"\]\) first/, "a virtual gamepad before init is refused");
    refuses(() => sdl.setCursor("pointer"), TypeError, /init\(\["video"\]\) first/, "a cursor before video is refused");
    sdl.setHint("SDL_VIDEO_DRIVER", "dummy");
    sdl.setHint("SDL_AUDIO_DRIVER", "dummy");
    sdl.setHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    sdl.init(["video", "gamepad"]);
    ok(sdl.wasInit().includes("gamepad"), "the gamepad subsystem runs");
    sdl.pollEvents();

    // --- system information ----------------------------------------------------------------
    const pi = sdl.powerInfo();
    ok(["error", "unknown", "battery", "no-battery", "charging", "charged"].includes(pi.state)
        && (pi.seconds === null || pi.seconds >= 0) && (pi.percent === null || (pi.percent >= 0 && pi.percent <= 100)), "powerInfo reports a state (" + JSON.stringify(pi) + ")");
    ok(["unknown", "light", "dark"].includes(sdl.systemTheme()), "systemTheme is light, dark or unknown");
    const loc = sdl.preferredLocales();
    ok(Array.isArray(loc) && loc.every((l) => /^[a-z]{2,3}(-[A-Za-z0-9]+)?$/.test(l)), "preferredLocales are language[-country] tags (" + loc + ")");
    ok(Number.isInteger(sdl.cpuCount()) && sdl.cpuCount() >= 1 && Number.isInteger(sdl.systemRAM()) && sdl.systemRAM() > 0, "cpuCount and systemRAM are positive");
    const bp = sdl.basePath();
    ok(bp === null || (typeof bp === "string" && /[\/\\]$/.test(bp)), "basePath ends in a separator (" + bp + ")");

    // --- window state ------------------------------------------------------------------------
    const W = 16, H = 8;
    const win = new sdl.Window("controls", W, H, { hidden: true });
    const st = win.state;
    ok(st.hidden === true && st.resizable === false && st.borderless === false && typeof st.occluded === "boolean", "state reports the creation flags");
    {
        const rw = new sdl.Window("resizable", 8, 8, { hidden: true, resizable: true, borderless: true });
        ok(rw.state.resizable === true && rw.state.borderless === true, "state reflects the resizable and borderless creation options");
        rw.close();
    }
    // The dummy driver has no window manager: SDL refuses these as unsupported there, and a real
    // driver must turn the flag the way it was asked.
    for (const [set, key, on, want] of [["setResizable", "resizable", true, true], ["setResizable", "resizable", false, false], ["setBordered", "borderless", false, true], ["setBordered", "borderless", true, false]]) {
        let e = null;
        try { win[set](on); } catch (x) { e = x; }
        if (e && e instanceof InternalError && /not supported/.test(e.message)) { print("test_sdl_controls: note: " + set + " is unsupported on the " + sdl.currentVideoDriver() + " driver"); continue; }
        ok(e === null && win.state[key] === want, set + "(" + on + ") leaves " + key + " " + want + " (" + e + ")");
    }
    refuses(() => win.setAlwaysOnTop("yes"), TypeError, /boolean/, "a flag setter takes a boolean");
    for (const m of ["maximize", "restore", "minimize", "restore", "flash"]) {
        let e = null;
        try { win[m](); } catch (x) { e = x; }
        ok(e === null || (e instanceof InternalError && /failed/.test(e.message)), m + "() succeeds or reports SDL's error");
    }

    // --- cursors -------------------------------------------------------------------------------
    sdl.hideCursor();
    ok(sdl.cursorVisible() === false, "hideCursor hides the cursor");
    sdl.showCursor();
    ok(sdl.cursorVisible() === true, "showCursor shows it");
    refuses(() => sdl.setCursor("hand"), RangeError, /'hand' is not one of/, "an unknown cursor is refused by name");
    refuses(() => sdl.setCursor(3), TypeError, /must be a string/, "a cursor is named");
    {
        let e = null;
        try { sdl.setCursor("pointer"); } catch (x) { e = x; }
        ok(e === null || (e instanceof InternalError && /SDL_CreateSystemCursor/.test(e.message)), "setCursor works or names SDL's refusal");
    }

    // --- renderer ------------------------------------------------------------------------------
    const r = new sdl.Renderer(win, { driver: "software" });
    r.setDrawColor(0, 0, 0);
    r.clear();
    r.setDrawColor(255, 255, 255);
    r.debugText(0, 0, "M");
    {
        const img = r.readPixels();
        const inCell = countColour(img, 0, 0, 8, 8, [255, 255, 255, 255]), all = countColour(img, 0, 0, W, H, [255, 255, 255, 255]);
        ok(inCell > 8 && all === inCell, "debugText inks one 8x8 cell and nothing past it (" + inCell + " of " + all + ")");
    }
    refuses(() => r.debugText(0, 0, "a\ud800"), TypeError, /lone surrogate/, "debug text with a lone surrogate is refused");
    refuses(() => r.debugText(NaN, 0, "a"), RangeError, /finite/, "a NaN position is refused");

    const tex = new sdl.Texture(r, 2, 2);
    tex.update(new Uint8Array([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255]));
    const R = [255, 0, 0, 255], G = [0, 255, 0, 255], B = [0, 0, 255, 255], Y = [255, 255, 0, 255], K = [0, 0, 0, 255];
    const quad = (tl, tr, bl, br) => new Frame(W, H, K).pixel(0, 0, tl).pixel(1, 0, tr).pixel(0, 1, bl).pixel(1, 1, br);
    const drawn = (opts) => { r.setDrawColor(0, 0, 0); r.clear(); r.drawTexture(tex, null, { x: 0, y: 0, width: 2, height: 2 }, opts); return r.readPixels(); };
    assertFrame(ok, drawn({}), quad(R, G, B, Y), "drawTexture with empty options draws it upright");
    assertFrame(ok, drawn({ flip: "horizontal" }), quad(G, R, Y, B), "flip horizontal mirrors left and right");
    assertFrame(ok, drawn({ flip: "vertical" }), quad(B, Y, R, G), "flip vertical mirrors top and bottom");
    assertFrame(ok, drawn({ flip: "both" }), quad(Y, B, G, R), "flip both mirrors both");
    assertFrame(ok, drawn({ angle: 180 }), quad(Y, B, G, R), "a 180-degree turn about the centre equals flipping both ways");
    assertFrame(ok, drawn({ angle: 90 }), quad(B, R, Y, G), "a 90-degree clockwise turn sends the left column to the top row");
    refuses(() => drawn({ flip: "diagonal" }), RangeError, /'diagonal' is not one of/, "an unknown flip is refused");
    refuses(() => drawn({ spin: 1 }), TypeError, /unknown option 'spin'/, "an unknown option is refused by name");
    refuses(() => drawn({ angle: Infinity }), RangeError, /finite/, "an infinite angle is refused");
    refuses(() => drawn({ center: 5 }), TypeError, /\{ x, y \}/, "a centre must be a point");
    refuses(() => r.drawTexture(tex, null, null, 7), TypeError, /options must be an object/, "options must be an object");

    r.setLogicalPresentation(8, 4, "stretch");
    let p = r.windowToRender(16, 8);
    ok(near(p.x, 8, 1e-3) && near(p.y, 4, 1e-3), "stretch maps the window's far corner to the logical one (" + JSON.stringify(p) + ")");
    p = r.renderToWindow(4, 2);
    ok(near(p.x, 8, 1e-3) && near(p.y, 4, 1e-3), "renderToWindow is the inverse (" + JSON.stringify(p) + ")");
    r.setDrawColor(0, 0, 0);
    r.clear();
    r.setDrawColor(255, 0, 0);
    r.fillRect(0, 0, 4, 2);
    r.present();
    r.setLogicalPresentation(0, 0, "disabled");
    p = r.windowToRender(16, 8);
    ok(near(p.x, 16, 1e-3) && near(p.y, 8, 1e-3), "disabled restores window coordinates");
    refuses(() => r.setLogicalPresentation(8, 4, "zoom"), RangeError, /'zoom' is not one of/, "an unknown mode is refused by name");
    refuses(() => r.setLogicalPresentation(0, 4), RangeError, /at least 1/, "a zero size needs mode disabled");
    refuses(() => r.setLogicalPresentation(-1, 4), RangeError, /0\.\.16384/, "a negative size is refused");

    // --- gamepads (SDL's virtual device) -----------------------------------------------------------
    const before = sdl.gamepads().length;
    refuses(() => new sdl.Gamepad(0x7fffff00), RangeError, /not a connected gamepad/, "an unknown id is refused");
    refuses(() => new sdl.Gamepad(0), RangeError, /1\.\.0xFFFFFFFF/, "id 0 is refused");
    refuses(() => sdl.detachVirtualGamepad(0x7fffff00), RangeError, /not a virtual gamepad/, "detaching an unknown id is refused");
    const id = sdl.attachVirtualGamepad("dyna test pad");
    ok(Number.isInteger(id) && id > 0, "attachVirtualGamepad returns an id");
    ok(sdl.gamepads().length === before + 1 && sdl.gamepads().includes(id), "gamepads() lists it");
    const added = sdl.pollEvents().filter((e) => e.type === "gamepadadded");
    ok(added.some((e) => e.which === id), "a gamepadadded event names it");
    const pad = new sdl.Gamepad(id);
    ok(pad.id === id && pad.name === "dyna test pad" && pad.virtual === true && pad.connected === true && typeof pad.type === "string",
        "the Gamepad reports id, name, virtual and connected (" + pad.name + ", " + pad.type + ")");
    ok(pad.button("a") === false && pad.axis("leftx") === 0, "a fresh pad is at rest");
    pad.setVirtualButton("a", true);
    ok(pad.button("a") === true && pad.button("b") === false, "setVirtualButton presses exactly that button");
    pad.setVirtualAxis("leftx", 0.5);
    ok(near(pad.axis("leftx"), 0.5, 1e-3), "setVirtualAxis moves the axis (" + pad.axis("leftx") + ")");
    pad.setVirtualAxis("lefty", -1);
    ok(pad.axis("lefty") === -1, "-1 reads back as exactly -1");
    pad.setVirtualAxis("righttrigger", 1);
    ok(pad.axis("righttrigger") === 1, "a full trigger reads 1");
    pad.setVirtualButton("a", false);
    ok(pad.button("a") === false, "and the button releases");
    const evs = sdl.pollEvents();
    ok(evs.some((e) => e.type === "gamepadbuttondown" && e.which === id && e.button === "a")
        && evs.some((e) => e.type === "gamepadbuttonup" && e.button === "a"), "button events name the button");
    ok(evs.some((e) => e.type === "gamepadaxis" && e.axis === "leftx" && near(e.value, 0.5, 1e-3)), "axis events name the axis and carry -1..1");
    ok(typeof pad.rumble(0.5, 0.5, 10) === "boolean", "rumble answers whether the device took it");
    refuses(() => pad.button("z"), RangeError, /unknown button 'z'/, "an unknown button is refused by name");
    refuses(() => pad.axis("leftz"), RangeError, /unknown axis 'leftz'/, "an unknown axis is refused by name");
    refuses(() => pad.button(0), TypeError, /button name/, "buttons are named");
    refuses(() => pad.setVirtualButton("a", 1), TypeError, /boolean/, "a press is a boolean");
    refuses(() => pad.setVirtualAxis("leftx", 1.5), RangeError, /-1\.\.1/, "an axis value past 1 is refused");
    refuses(() => pad.rumble(2, 0, 10), RangeError, /0\.\.1/, "a rumble intensity over 1 is refused");
    refuses(() => pad.rumble(0, 0, 60001), RangeError, /0\.\.60000/, "a rumble over a minute is refused");
    refuses(() => sdl.quit(), TypeError, /1 gamepad\(s\)/, "quit() is refused while a gamepad is open");
    pad.close();
    refuses(() => pad.button("a"), TypeError, /closed/, "a closed gamepad refuses reads");
    sdl.detachVirtualGamepad(id);
    ok(!sdl.gamepads().includes(id), "detaching removes it");
    ok(sdl.pollEvents().some((e) => e.type === "gamepadremoved" && e.which === id), "a gamepadremoved event names it");

    tex.close();
    r.close();
    win.close();
    sdl.quit();
    ok(sdl.wasInit().length === 0, "quit() shuts everything down, cursors included");
}
print("test_sdl_controls: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_sdl_controls: " + fail + " failed");
