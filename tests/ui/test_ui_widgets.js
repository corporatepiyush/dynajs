// dyna:ui display elements and buttons, each frame compared over the WHOLE screen against a frame
// built here from the layout contract (ui_kit.js lists the constants). Interaction runs through the
// synthetic input calls, one frame behind as documented: a pointer event lands in the next frame.
import * as ui from "dyna:ui";
import { LH, ADV, PADX, PADY, FH, GAP, WPAD, BW, T, frame, rect, ring, glyphs, check, buttonBox, ink, tw, over, roundRect, snap, any } from "./ui_kit.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }
function refuses(fn, ctor, re, m) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    ok(e instanceof ctor && re.test(e.message), m + " (got " + e + ")");
}

const W = 240, H = 120;
function ctx() { return new ui.Context({ width: W, height: H, clock: "manual" }); }
function run(c, fn) { let r; c.frame((u) => { r = fn(u); }); return r; }

{
    const c = ctx();
    const th = c.theme();
    for (const k of Object.keys(T)) ok(th[k] === T[k], `theme default ${k} is ${T[k].toString(16)} (got ${th[k] >>> 0})`);
    ok(th.fontSize === 13 && th.padX === PADX && th.padY === PADY && th.spacing === GAP && th.windowPad === WPAD && th.borderWidth === BW,
        "theme metrics are the documented ones");
    c.close();
}

{
    const c = ctx();
    run(c, (u) => { u.label("Hi"); u.heading("Hi"); u.bullet("Hi"); u.separator(); u.separator("T"); });
    const F = frame(W, H);
    let y = WPAD;
    glyphs(F, WPAD, y, "Hi", T.text); y += LH + GAP;
    glyphs(F, WPAD, y, "Hi", T.text, 26); y += 26 + GAP;
    rect(F, WPAD, y + 5, 4, 4, T.textDim);
    glyphs(F, WPAD + 4 + PADX, y, "Hi", T.text); y += LH + GAP;
    rect(F, WPAD, y, W - 2 * WPAD, 1, T.border); y += 1 + GAP;
    rect(F, WPAD, y + 6, 12, 1, T.border);
    glyphs(F, WPAD + 12 + PADX, y, "T", T.textDim);
    rect(F, WPAD + 12 + PADX + ADV + PADX, y + 6, W - 2 * WPAD - 12 - ADV - 2 * PADX, 1, T.border);
    check(ok, c, F, "label, 26 px heading, bullet, plain and titled separators",
        [ink(WPAD, WPAD, "Hi"), [WPAD, WPAD + LH + GAP, 28, 26, T.text, 20], ink(WPAD + 10, WPAD + LH + GAP + 26 + GAP, "Hi")]);
    c.close();
}

{
    const c = ctx();
    let clicked;
    const scene = (u) => u.button("Save");
    const shot = (fill, label) => {
        const F = frame(W, H);
        buttonBox(F, WPAD, WPAD, "Save", fill);
        check(ok, c, F, `button ${label}: 40x21 at the padding, centred text, border last`, [ink(WPAD + PADX, WPAD + PADY, "Save")]);
    };
    clicked = run(c, scene);
    ok(clicked === false, "an idle button is not clicked");
    shot(T.button, "idle");
    c.pointer({ x: 20, y: 15 });
    run(c, scene);
    shot(T.buttonHot, "hot");
    c.pointer({ x: 20, y: 15, buttons: 1 });
    ok(run(c, scene) === false, "pressing is not yet a click");
    shot(T.buttonActive, "pressed");
    c.pointer({ x: 20, y: 15, buttons: 0 });
    ok(run(c, scene) === true, "releasing over the button is a click");
    shot(T.buttonHot, "after release");
    c.pointer({ x: 20, y: 15, buttons: 1 });
    run(c, scene);
    c.pointer({ x: 100, y: 100, buttons: 1 });
    run(c, scene);
    shot(T.buttonActive, "dragged off while held: still active");
    c.pointer({ x: 100, y: 100, buttons: 0 });
    ok(run(c, scene) === false, "releasing elsewhere is not a click");
    shot(T.button, "released elsewhere");
    c.key("tab");
    run(c, scene);
    {
        const F = frame(W, H);
        buttonBox(F, WPAD, WPAD, "Save", T.button, T.text, T.accent);
        check(ok, c, F, "Tab focuses the button and draws the focus ring in the accent colour");
    }
    c.key("return");
    ok(run(c, scene) === true, "Return on a keyboard-focused button clicks it");
    c.close();
}

{
    const c = ctx();
    let on = false, checked = false, sw = false;
    const scene = (u) => {
        on = u.toggle("Bold", on);
        checked = u.checkbox("On", checked);
        sw = u.switch("Wi", sw);
    };
    run(c, scene);
    const expect = (label) => {
        const F = frame(W, H);
        let y = WPAD;
        buttonBox(F, WPAD, y, "Bold", on ? T.accent : T.button, on ? T.onAccent : T.text);
        y += FH + GAP;
        rect(F, WPAD, y, LH, LH, T.frame);
        if (checked) rect(F, WPAD + 3, y + 3, LH - 6, LH - 6, T.accent);
        ring(F, WPAD, y, LH, LH, T.border);
        glyphs(F, WPAD + LH + PADX, y, "On", T.text);
        y += LH + GAP;
        roundRect(F, WPAD, y, 2 * LH, LH, LH / 2, sw ? T.accent : T.frame);
        roundRect(F, WPAD, y, 2 * LH, LH, LH / 2, T.border, BW);
        roundRect(F, WPAD + (sw ? LH + 2 : 2), y + 2, LH - 4, LH - 4, (LH - 4) / 2, sw ? T.onAccent : T.textDim);
        glyphs(F, WPAD + 2 * LH + PADX, y, "Wi", T.text);
        check(ok, c, F, `toggle/checkbox/switch ${label}`);
    };
    expect("all off");
    const click = (x, y) => { c.pointer({ x, y, buttons: 1 }); run(c, scene); c.pointer({ x, y, buttons: 0 }); run(c, scene); c.pointer({ x: 230, y: 110 }); run(c, scene); };
    click(12, 12);
    ok(on === true, "clicking the toggle turns it on");
    click(WPAD + LH + PADX + 3, WPAD + FH + GAP + 6);
    ok(checked === true, "clicking a checkbox's label toggles it");
    click(WPAD + 3, WPAD + FH + GAP + LH + GAP + 6);
    ok(sw === true, "clicking the switch track turns it on");
    expect("all on");
    c.close();
}

{
    const c = ctx();
    let r = 0, seg = 1, tab = 0, picked = false;
    const scene = (u) => {
        r = u.radio("R", ["A", "B"], r);
        seg = u.segmented("S", ["A", "B"], seg);
        tab = u.tabs("Tb", ["One", "Two"], tab);
        picked = u.selectable("Item", picked);
    };
    run(c, scene);
    const expect = (label) => {
        const F = frame(W, H);
        let y = WPAD;
        for (let k = 0; k < 2; k++) {
            roundRect(F, WPAD, y, LH, LH, LH / 2, T.frame);
            if (k === r) roundRect(F, WPAD + 3, y + 3, LH - 6, LH - 6, LH / 2 - 3, T.accent);
            roundRect(F, WPAD, y, LH, LH, LH / 2, T.border, BW);
            glyphs(F, WPAD + LH + PADX, y, k ? "B" : "A", T.text);
            y += LH + GAP;
        }
        let x = WPAD;
        for (let k = 0; k < 2; k++) x += buttonBox(F, x, y, k ? "B" : "A", k === seg ? T.accent : T.button, k === seg ? T.onAccent : T.text);
        y += FH + GAP;
        x = WPAD;
        for (let k = 0; k < 2; k++) {
            const lab = k ? "Two" : "One", w = tw(lab) + 4 * PADX;
            rect(F, x, y, w, FH, k === tab ? T.panel : T.title);
            glyphs(F, x + 2 * PADX, y + PADY, lab, k === tab ? T.text : T.textDim);
            if (k === tab) rect(F, x, y, w, 2, T.accent);
            x += w + 1;
        }
        y += FH + GAP;
        if (picked) rect(F, WPAD, y, W - 2 * WPAD, LH + 4, T.selection);
        glyphs(F, WPAD + PADX, y + 2, "Item", T.text);
        check(ok, c, F, `radio, segmented, tabs, selectable: ${label}`);
    };
    expect("initial");
    const click = (x, y) => { c.pointer({ x, y, buttons: 1 }); run(c, scene); c.pointer({ x, y, buttons: 0 }); run(c, scene); c.pointer({ x: 230, y: 115 }); run(c, scene); };
    click(WPAD + 3, WPAD + LH + GAP + 5);
    ok(r === 1, "clicking the second radio selects it");
    click(WPAD + 3, WPAD + 2 * (LH + GAP) + 5);
    ok(seg === 0, "clicking the first segment selects it");
    click(WPAD + 45 + 1 + 5, WPAD + 2 * (LH + GAP) + FH + GAP + 5);
    ok(tab === 1, "clicking the second tab selects it");
    click(WPAD + 100, WPAD + 2 * (LH + GAP) + 2 * (FH + GAP) + 5);
    ok(picked === true, "clicking a selectable anywhere along its row selects it");
    expect("after clicks");
    c.close();
}

{
    const c = ctx();
    let hit = false;
    const img = c.addImage(new Uint8Array([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]), 2, 2);
    const scene = (u) => {
        hit = u.link("Go");
        u.badge("N");
        u.image(img, 8, 8);
        u.progress(0.25);
    };
    run(c, scene);
    const expect = (hot) => {
        const F = frame(W, H);
        let y = WPAD;
        glyphs(F, WPAD, y, "Go", T.link);
        if (hot) rect(F, WPAD, y + LH - 1, 14, 1, T.link);
        y += LH + GAP;
        rect(F, WPAD, y, ADV + 6, LH + 2, T.accent);
        glyphs(F, WPAD + 3, y + 1, "N", T.onAccent);
        y += LH + 2 + GAP;
        const px = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255]];
        for (let py = 0; py < 8; py++)
            for (let qx = 0; qx < 8; qx++) {
                const p = px[(py >> 2) * 2 + (qx >> 2)];
                rect(F, WPAD + qx, y + py, 1, 1, ((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | 255) >>> 0);
            }
        y += 8 + GAP;
        rect(F, WPAD, y, W - 2 * WPAD, FH, T.frame);
        const x0 = WPAD + 1, x1 = snap(WPAD + 1 + 0.25 * (W - 2 * WPAD - 2));
        rect(F, x0, y + 1, x1 - x0, FH - 2, T.accent);
        ring(F, WPAD, y, W - 2 * WPAD, FH, T.border);
        check(ok, c, F, `link (${hot ? "hot, underlined" : "idle"}), badge, 2x2 image scaled to 8x8 nearest, 25% progress`);
    };
    expect(false);
    c.pointer({ x: 10, y: 10 });
    run(c, scene);
    expect(true);
    c.pointer({ x: 10, y: 10, buttons: 1 });
    run(c, scene);
    c.pointer({ x: 10, y: 10, buttons: 0 });
    run(c, scene);
    ok(hit === true, "a clicked link reports it");
    refuses(() => run(c, (u) => u.image(99)), RangeError, /not an image handle/, "an unknown image handle is refused");
    refuses(() => c.addImage(new Uint8Array(15), 2, 2), RangeError, /width \* height \* 4/, "addImage needs exactly w*h*4 bytes");
    c.removeImage(img);
    refuses(() => run(c, (u) => u.image(img)), RangeError, /not an image handle/, "a removed image handle is refused");
    c.close();
}

{
    const c = ctx();
    c.setTime(10);
    run(c, (u) => u.spinner());
    ok(c.wakeIn > 0 && c.wakeIn <= 0.125, `a spinner asks for the next frame within 1/8 s (wakeIn ${c.wakeIn})`);
    const F = frame(W, H);
    any(F, WPAD, WPAD, LH, LH);
    check(ok, c, F, "a spinner draws only inside its 13x13 box");
    const before = c.pixels();
    c.setTime(10.125);
    ok(c.frame((u) => u.spinner()) === true, "the next eighth of a second is a changed frame");
    const after = c.pixels();
    let differ = 0;
    for (let i = 0; i < before.length; i++) if (before[i] !== after[i]) differ++;
    ok(differ > 0, "the lit dot moves after 1/8 s");
    run(c, (u) => u.label("x"));
    ok(c.wakeIn === Infinity, "without an animation nothing asks for a frame");
    c.close();
}

{
    const c = ctx();
    refuses(() => run(c, (u) => u.checkbox("x", 1)), TypeError, /boolean/, "checkbox takes a boolean");
    refuses(() => run(c, (u) => u.button(5)), TypeError, /string/, "button takes a string");
    refuses(() => run(c, (u) => u.radio("r", ["a", 3], 0)), TypeError, /strings/, "radio items are strings");
    refuses(() => run(c, (u) => u.row({ bogus: 1 }, () => {})), TypeError, /unknown option 'bogus'/, "unknown row option is refused");
    refuses(() => run(c, (u) => u.row({ width: "12px" }, () => {})), TypeError, /fit.*grow/, "a malformed size is refused");
    refuses(() => run(c, (u) => u.column({ scroll: true }, () => {})), TypeError, /needs an id/, "a scrolling container needs an id");
    refuses(() => run(c, (u) => u.row(async () => {})), TypeError, /synchronous/, "an async layout callback is refused");
    refuses(() => new ui.Context({ width: 0, height: 10 }), RangeError, /1\.\.16384/, "a zero-width context is refused");
    { const f = new ui.Context({ width: 10.5, height: 10 }); ok(f.pixelWidth === 11 && f.width === 11, `a fractional logical width rounds to whole device pixels (${f.pixelWidth}, ${f.width})`); f.close(); }
    refuses(() => new ui.Context({ target: {} }), TypeError, /target/, "a target that is not a renderer or window is refused");
    { const m = new ui.Context({ width: 10, height: 10 }); refuses(() => m.setTime(5), TypeError, /manual/, "setTime needs a manual-clock context"); m.close(); }
    c.close();
}

print(`test_ui_widgets: ${pass} pass, ${fail} fail`);
if (fail) throw new Error(`${fail} failures`);
