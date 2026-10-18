// flags: --std
// dyna:sdl on the dummy drivers: every Event field round-trips through pushEvent() into pollEvent(),
// window geometry and state setters are read back through their getters AND the events SDL raises,
// drawRect draws an outline (and only an outline), and every resource disposes through dispose() and
// Symbol.dispose. Expected values come from the d.ts contract: an event pops with the fields it was
// pushed with, and a setter's value is what the getter then reports. A build without CONFIG_SDL3 SKIPs.
import { countColour } from "../lib/screen.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function refuses(fn, ctor, re, m) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    ok(e instanceof ctor && re.test(e.message), m + (e ? " (" + e.constructor.name + ": " + e.message + ")" : " (no throw)"));
}

let sdl = null;
try { sdl = await import("dyna:sdl"); } catch (e) {}
if (!sdl) print("test_sdl_events: SKIP (dyna:sdl is not in this build: CONFIG_SDL3=y)");
else {
    sdl.setHint("SDL_VIDEO_DRIVER", "dummy");
    sdl.setHint("SDL_AUDIO_DRIVER", "dummy");
    sdl.init(["video", "audio"]);
    const drain = () => sdl.pollEvents();
    const one = (ev) => { drain(); sdl.pushEvent(ev); const got = sdl.pollEvents().filter((e) => e.type === ev.type); return got.length === 1 ? got[0] : null; };
    const same = (got, want) => Object.keys(want).every((k) => k === "type" || Math.abs(got[k] - want[k]) < 1e-6 || got[k] === want[k]);

    // --- pushEvent round trips -------------------------------------------------------------------
    const A = sdl.keyFromName("A");
    for (const type of ["keydown", "keyup"]) {
        const want = { type, windowId: 7, key: A, scancode: 4, mod: 0x0040, repeat: type === "keydown" };
        const got = one(want);
        ok(got && same(got, want) && got.keyName === "A", `${type} pops with key, keyName, scancode, mod, repeat and windowId (${JSON.stringify(got)})`);
    }
    {
        const want = { type: "mousemotion", windowId: 3, x: 12.5, y: 40, xrel: -3, yrel: 7.25, buttons: 5 };
        const got = one(want);
        ok(got && same(got, want), `mousemotion pops with x, y, xrel, yrel and buttons (${JSON.stringify(got)})`);
    }
    for (const type of ["mousebuttondown", "mousebuttonup"]) {
        const want = { type, windowId: 3, button: 3, clicks: 2, x: 5, y: 6 };
        const got = one(want);
        ok(got && same(got, want), `${type} pops with button, clicks and position (${JSON.stringify(got)})`);
    }
    {
        const want = { type: "mousewheel", windowId: 1, x: 0, y: -2.5 };
        const got = one(want);
        ok(got && same(got, want), `mousewheel pops with its amounts (${JSON.stringify(got)})`);
    }
    for (const type of ["fingerdown", "fingermotion", "fingerup", "fingercanceled"]) {
        const want = { type, windowId: 2, touchId: 9, fingerId: 4, x: 0.25, y: 0.75, dx: -0.125, dy: 0.5, pressure: 0.5 };
        const got = one(want);
        ok(got && same(got, want), `${type} pops with touchId, fingerId, x, y, dx, dy and pressure (${JSON.stringify(got)})`);
    }
    {
        const got = one({ type: "user", code: 99, windowId: 4 });
        ok(got && got.code === 99, "a user event keeps its code");
        ok(typeof got.timestamp === "number" && got.timestamp >= 0, "every event carries a timestamp in milliseconds");
    }
    refuses(() => sdl.pushEvent({ type: "keydown", keyname: "A" }), TypeError, /keyname/, "an unknown event key is refused by name");
    refuses(() => sdl.pushEvent({ type: "mousemotion", button: 1 }), TypeError, /'button' is not a field of a mousemotion event/, "a field of another event type is refused by name");
    refuses(() => sdl.pushEvent({ type: "keydown", x: 1 }), TypeError, /'x' is not a field of a keydown event/, "keydown has no position");
    refuses(() => sdl.pushEvent({ type: "quit", windowId: 1 }), TypeError, /'windowId' is not a field of a quit event/, "quit carries no fields");
    refuses(() => sdl.pushEvent({ type: "mousewheel", pressure: 0.5 }), TypeError, /'pressure'/, "a wheel event has no pressure");
    {
        const proto = { windowId: 77, code: 5 };
        const got = one(Object.assign(Object.create(proto), { type: "user" }));
        ok(got && got.code === 0, `inherited fields are not read, the same as the unknown-key check (${JSON.stringify(got)})`);
        const hidden = { type: "user" };
        Object.defineProperty(hidden, "code", { value: 3, enumerable: false });
        const g2 = one(hidden);
        ok(g2 && g2.code === 0, `a non-enumerable own field is not read either (${JSON.stringify(g2)})`);
        const acc = { type: "user", get code() { return 12; } };
        const g3 = one(acc);
        ok(g3 && g3.code === 12, `an own enumerable getter is read (${JSON.stringify(g3)})`);
    }
    ok(sdl.keyName(0x10ffff + 1) === "", `keyName of a code that is no key is "" (${JSON.stringify(sdl.keyName(0x10ffff + 1))})`);
    ok(sdl.keyName(0xd800) === "", `keyName of a surrogate code is "" (${JSON.stringify(sdl.keyName(0xd800))})`);
    ok(sdl.keyName(A) === "A", "keyName of a real key is its name");
    refuses(() => sdl.pushEvent({ type: "mousebuttondown", button: 9 }), RangeError, /button .*1\.\.5/, "a button outside 1..5 is refused");
    refuses(() => sdl.pushEvent({ type: "fingerdown", x: 1.5 }), RangeError, /0\.\.1 of the window/, "a finger outside the window is refused");
    refuses(() => sdl.pushEvent({ type: "fingerdown", pressure: 2 }), RangeError, /pressure/, "a pressure above 1 is refused");
    refuses(() => sdl.pushEvent({ type: "keydown", repeat: 1 }), TypeError, /repeat must be a boolean/, "a non-boolean repeat is refused");
    refuses(() => sdl.pushEvent({ type: "keydown", scancode: 1.5 }), RangeError, /scancode must be an integer/, "a fractional scancode is refused");
    refuses(() => sdl.pushEvent({ type: "mousemotion", x: NaN }), RangeError, /x must be a number/, "a NaN coordinate is refused");
    refuses(() => sdl.pushEvent({ type: "mousemotion", x: { valueOf() { throw new Error("boom"); } } }), Error, /boom/, "an exception from coercion propagates");
    ok(drain().length === 0, "a refused push queues nothing");

    // --- window geometry and state ---------------------------------------------------------------
    const win = new sdl.Window("events", 40, 30, { hidden: true });
    drain();
    win.setSize(64, 48);
    win.sync();
    let evs = drain();
    ok(win.size.width === 64 && win.size.height === 48, "setSize is what size reports");
    ok(win.pixelSize.width >= 64 && win.pixelSize.height >= 48 && win.pixelSize.width % 64 === 0, `pixelSize is size times the density (${JSON.stringify(win.pixelSize)})`);
    ok(evs.some((e) => e.type === "windowresized" && e.windowId === win.id && e.data1 === 64 && e.data2 === 48), "and SDL reports windowresized with data1/data2 = the new size");
    win.setPosition(10, 20);
    evs = drain();
    ok(win.position.x === 10 && win.position.y === 20, "setPosition is what position reports");
    ok(evs.some((e) => e.type === "windowmoved" && e.data1 === 10 && e.data2 === 20), "and SDL reports windowmoved with data1/data2 = the position");
    win.show();
    evs = drain();
    ok(!win.state.hidden && evs.some((e) => e.type === "windowshown"), "show() unhides it and raises windowshown");
    win.raise();
    win.hide();
    evs = drain();
    ok(win.state.hidden && evs.some((e) => e.type === "windowhidden"), "hide() hides it and raises windowhidden");
    win.setFullscreen(true);
    ok(win.state.fullscreen === true, "setFullscreen(true) is what state.fullscreen reports");
    win.setFullscreen(false);
    ok(win.state.fullscreen === false, "and setFullscreen(false) leaves it");
    win.setMouseGrab(true);
    ok(win.state.mouseGrabbed === true, "setMouseGrab(true) grabs the mouse");
    win.setMouseGrab(false);
    ok(win.state.mouseGrabbed === false, "and setMouseGrab(false) releases it");
    win.setRelativeMouseMode(true);
    ok(win.state.relativeMouse === true, "setRelativeMouseMode(true) turns relative mode on");
    win.setRelativeMouseMode(false);
    ok(win.state.relativeMouse === false, "and off again");
    win.setKeyboardGrab(true);
    win.setKeyboardGrab(false);
    ok(true, "setKeyboardGrab accepts both states on the dummy driver");
    for (const m of ["setMouseGrab", "setKeyboardGrab", "setRelativeMouseMode", "setFullscreen"])
        refuses(() => win[m](1), TypeError, /boolean/, `${m} takes a boolean`);
    refuses(() => win.setSize(0, 10), RangeError, /1\.\.16384/, "a zero width is refused");

    // --- system queries --------------------------------------------------------------------------
    const t0 = sdl.ticks();
    ok(Number.isInteger(t0) && t0 >= 0 && sdl.ticks() >= t0, "ticks() is a non-decreasing millisecond count");
    ok(typeof sdl.getError() === "string", "getError() returns SDL's last message as a string");
    const ad = sdl.audioDrivers();
    ok(Array.isArray(ad) && ad.includes("dummy") && ad.includes(sdl.currentAudioDriver()), `audioDrivers() lists the driver in use (${ad})`);
    const ds = sdl.displays();
    ok(ds.length >= 1 && ds.every((d) => Number.isInteger(d.id) && d.width > 0 && d.height > 0 && d.pixelDensity > 0), "displays() lists at least one display with a size and density");

    // --- drawRect: an outline, not a fill ---------------------------------------------------------
    {
        const w2 = new sdl.Window("rect", 16, 16, { hidden: true });
        const r = new sdl.Renderer(w2, { driver: "software" });
        r.setDrawColor(0, 0, 0);
        r.clear();
        r.setDrawColor(255, 0, 0);
        r.drawRect(2, 3, 8, 6);
        const img = r.readPixels(), RED = [255, 0, 0, 255];
        ok(countColour(img, 2, 3, 8, 6, RED) === 2 * 8 + 2 * (6 - 2), `drawRect inks exactly the 24-pixel perimeter of an 8x6 box (${countColour(img, 2, 3, 8, 6, RED)})`);
        ok(countColour(img, 3, 4, 6, 4, RED) === 0, "and leaves its inside alone");
        ok(countColour(img, 0, 0, 16, 16, RED) === 24, "and nothing outside it");
        // dispose() and Symbol.dispose close like close()
        const tex = new sdl.Texture(r, 2, 2);
        const tex2 = new sdl.Texture(r, 2, 2);
        tex2[Symbol.dispose]();
        ok(tex2.closed, "Texture[Symbol.dispose]() closes it");
        tex.dispose();
        ok(tex.closed, "Texture.dispose() closes it");
        r.dispose();
        ok(r.closed, "Renderer.dispose() closes it");
        const w3 = new sdl.Window("disposable", 8, 8, { hidden: true });
        w3[Symbol.dispose]();
        ok(w3.closed, "Window[Symbol.dispose]() closes it");
        w2.dispose();
        ok(w2.closed, "Window.dispose() closes it");
    }

    // --- AudioStream: flush and dispose ------------------------------------------------------------
    {
        const s = new sdl.AudioStream({ freq: 48000, channels: 1, format: "f32" });
        s.put(new Float32Array(480));
        const q = s.queued;
        ok(q === 480 * 4, `put queues 4 bytes per f32 sample (${q})`);
        s.flush();
        ok(s.queued === q, "flush() keeps what is queued (it marks the end; nothing plays while paused)");
        s.clear();
        ok(s.queued === 0, "clear() drops it");
        const s2 = new sdl.AudioStream({ freq: 48000, channels: 1 });
        s2[Symbol.dispose]();
        ok(s2.closed, "AudioStream[Symbol.dispose]() closes it");
        s.dispose();
        ok(s.closed, "AudioStream.dispose() closes it");
        refuses(() => s.flush(), TypeError, /closed/, "flush() on a closed stream is refused");
    }
    win.close();
    sdl.quit();
    print(`test_sdl_events: ${pass} passed, ${fail} failed`);
    if (fail) throw new Error(`test_sdl_events: ${fail} failed`);
}
