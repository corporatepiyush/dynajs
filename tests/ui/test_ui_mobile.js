// dyna:ui on a phone-shaped screen with safe-area insets: every frame is compared over the WHOLE
// screen against a frame built from the layout contract (ui_kit.js lists the constants).
//   Insets [0, 20, 0, 12]: a 20 px status bar on top, a 12 px gesture bar at the bottom.
//   App bar: content height = frame height (21) + 2 * padY = 29; the outermost top dock extends
//   through the top inset, so it covers 0..49 and the screen content starts at 49 + windowPad.
//   Nav bar: max(line + 2 * padY, minTarget) = 21 tall plus the bottom inset, items share the width,
//   the selected one is accent text with a 2 px accent line across its content width.
//   FAB: 56 x 56 (pill when extended, padX 56 / 3 rounded = 19), 16 px from the right and bottom
//   of the safe area, above any bottom dock. Drawer: min(300, 0.85 * safe width) wide, full height.
//   Sheet: full width, bottom edge on the screen edge, radius 12 when the theme has none.
//   Toast: centred, 16 px above the bottom inset and any bottom dock. Modal: centred in the safe area.
import * as ui from "dyna:ui";
import { LH, ADV, PADX, PADY, FH, GAP, WPAD, BW, T, frame, rect, ring, glyphs, check, ink, tw, over, roundRect, snap, any } from "./ui_kit.js";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; print("FAIL: " + m); } }

const W = 240, H = 200, TOP = 20, BOT = 12;
const BAR = FH + 2 * PADY, NAV = LH + 2 * PADY;
function ctx() { return new ui.Context({ width: W, height: H, insets: [0, TOP, 0, BOT], clock: "manual" }); }
function run(c, fn) { let r; c.frame((u) => { r = fn(u); }); return r; }

function navBar(F, items, sel) {
    const y = H - BOT - NAV, iw = W / items.length;
    rect(F, 0, y, W, NAV + BOT, T.title);
    items.forEach((s, k) => {
        const x = k * iw, inner = iw - 2 * PADX;
        if (k === sel) rect(F, x + PADX, y, inner, 2, T.accent);
        glyphs(F, snap(x + PADX + (inner - tw(s)) / 2), y + PADY, s, k === sel ? T.accent : T.textDim);
    });
}

{
    const c = ctx();
    ok(c.insets.join() === [0, TOP, 0, BOT].join() && c.width === W && c.height === H, `insets read back in logical points (${c.insets})`);
    const scene = (u) => { u.appBar("", () => {}); u.label("Hi"); u.navBar("n", ["A", "B"], 0); };
    run(c, scene);
    const F = frame(W, H);
    rect(F, 0, 0, W, TOP + BAR, T.title);
    glyphs(F, WPAD, TOP + BAR + WPAD, "Hi", T.text);
    navBar(F, ["A", "B"], 0);
    check(ok, c, F, "app bar and nav bar extend through the insets; content starts below the app bar",
        [ink(WPAD, TOP + BAR + WPAD, "Hi"), ink(snap(PADX + (W / 2 - 2 * PADX - ADV) / 2), H - BOT - NAV + PADY, "A", T.accent)]);
    const r = run(c, scene);
    ok(r === undefined, "a frame without a result returns nothing");
    c.close();
}

{
    const c = ctx();
    run(c, (u) => { u.navBar("n", ["A", "B", "C"], 2); u.fab("##add"); });
    const F = frame(W, H);
    navBar(F, ["A", "B", "C"], 2);
    const x = W - 16 - 56, y = H - BOT - NAV - 16 - 56;
    roundRect(F, x, y, 56, 56, 28, T.accent);
    any(F, x + 16, y + 16, 24, 24);
    check(ok, c, F, "the icon FAB sits 16 px inside the safe area above the nav bar, 56 px round; a selected item's line spans only that item",
        [ink(snap(2 * 80 + PADX + (80 - 2 * PADX - ADV) / 2), H - BOT - NAV + PADY, "C", T.accent)]);
    c.close();
}

for (const [label, textW] of [["Compose", 7 * ADV], ["Go", 2 * ADV]]) {
    const c = ctx();
    run(c, (u) => u.fab(label));
    const F = frame(W, H);
    const pad = Math.round(56 / 3), w = Math.max(56, textW + 2 * pad), x = W - 16 - w, y = H - BOT - 16 - 56;
    roundRect(F, x, y, w, 56, 28, T.accent);
    glyphs(F, snap(x + (w - textW) / 2), snap(y + (56 - LH) / 2), label, T.onAccent);
    check(ok, c, F, `extended FAB "${label}": ${w} px wide, its right edge 16 px inside the safe area`,
        [ink(snap(x + (w - textW) / 2), snap(y + (56 - LH) / 2), label, T.onAccent)]);
    c.close();
}

{
    const c = ctx();
    const scene = (u) => { u.label("Hi"); u.drawer("d", () => u.label("X")); };
    run(c, (u) => { u.openDrawer("d"); scene(u); });
    run(c, scene);
    const F = frame(W, H);
    rect(F, 0, 0, W, H, T.shade);
    const dw = Math.min(300, Math.round(0.85 * W));
    rect(F, 0, 0, dw, H, T.panel);
    ring(F, 0, 0, dw, H, T.border);
    glyphs(F, WPAD, WPAD + TOP, "X", T.text);
    check(ok, c, F, "the drawer and its shade cover the whole screen, insets included; its content starts inside the safe area",
        [ink(WPAD, WPAD + TOP, "X")]);
    c.close();
}

{
    const c = ctx();
    const scene = (u) => u.sheet("##s", () => u.label("S"));
    run(c, (u) => { u.openSheet("##s"); scene(u); });
    run(c, scene);
    const F = frame(W, H);
    rect(F, 0, 0, W, H, T.shade);
    const r = 12, h = WPAD + 4 + GAP + LH + WPAD + BOT + r, y = H - h;
    roundRect(F, 0, y, W, h, r, T.popup);
    roundRect(F, 0, y, W, h, r, T.border, BW);
    roundRect(F, (W - 32) / 2, y + WPAD, 32, 4, 2, T.border);
    glyphs(F, WPAD, y + WPAD + 4 + GAP, "S", T.text);
    check(ok, c, F, `the sheet's bottom edge is the screen's (y ${y}, ${h} tall) and its content clears the bottom inset`,
        [ink(WPAD, y + WPAD + 4 + GAP, "S")]);
    c.close();
}

{
    const c = ctx();
    c.toast("T");
    run(c, (u) => u.navBar("n", ["A", "B"], 1));
    const F = frame(W, H);
    navBar(F, ["A", "B"], 1);
    const tw_ = ADV + 4 * PADX, th = LH + 4 * PADY, x = snap(WPAD + (W - 2 * WPAD - tw_) / 2), y = H - BOT - NAV - 2 * WPAD - th;
    rect(F, x, y, tw_, th, T.tooltip);
    ring(F, x, y, tw_, th, T.border);
    glyphs(F, x + 2 * PADX, y + 2 * PADY, "T", T.text);
    check(ok, c, F, `a toast sits ${2 * WPAD} px above the nav bar, which covers the bottom inset`, [ink(x + 2 * PADX, y + 2 * PADY, "T")]);
    ok(c.wakeIn > 2.9 && c.wakeIn <= 3, `the toast asks for the frame that removes it (wakeIn ${c.wakeIn})`);
    c.setTime(3.5);
    run(c, (u) => u.navBar("n", ["A", "B"], 1));
    const G = frame(W, H);
    navBar(G, ["A", "B"], 1);
    check(ok, c, G, "after its 3 s the toast is gone");
    c.close();
}

{
    const c = ctx();
    const scene = (u) => u.modal("##m", () => u.label("M"));
    run(c, (u) => { u.openModal("##m"); scene(u); });
    run(c, scene);
    const F = frame(W, H);
    rect(F, 0, 0, W, H, T.shade);
    const w = ADV + 2 * WPAD, h = LH + 2 * WPAD, x = snap((W - w) / 2), y = snap(TOP + (H - TOP - BOT - h) / 2);
    rect(F, x, y, w, h, T.popup);
    ring(F, x, y, w, h, T.border);
    glyphs(F, x + WPAD, y + WPAD, "M", T.text);
    check(ok, c, F, `a modal is centred in the safe area (y ${y}), its shade covers the whole screen`, [ink(x + WPAD, y + WPAD, "M")]);
    c.close();
}

{
    const c = ctx();
    run(c, (u) => {
        u.box({ float: "safe", x: 230, y: 0, width: 30, height: 10, fill: 0xff0000ff }, () => {});
        u.box({ float: "screen", x: 230, y: 190, width: 30, height: 10, fill: 0x00ff00ff }, () => {});
    });
    const F = frame(W, H);
    rect(F, W - 30, TOP, 30, 10, 0xff0000ff);
    rect(F, 230, 190, 10, 10, 0x00ff00ff);
    check(ok, c, F, "float \"safe\" is moved inside the safe area, float \"screen\" stays where it was put");
    let e = null;
    try { run(c, (u) => u.box({ float: "edge" }, () => {})); } catch (x) { e = x; }
    ok(e instanceof TypeError && /"screen" or "safe"/.test(e.message), `an unknown float is refused naming the four (${e})`);
    c.close();
}

{
    const L = 30, R = 24;
    const c = new ui.Context({ width: W, height: H, insets: [L, TOP, R, BOT], clock: "manual" });
    run(c, (u) => { u.appBar("T", () => {}); u.label("Hi"); u.navBar("n", ["A", "B"], 1); });
    const F = frame(W, H);
    rect(F, 0, 0, W, TOP + BAR, T.title);
    any(F, L + WPAD, TOP, 16, BAR);
    glyphs(F, L + WPAD, TOP + BAR + WPAD, "Hi", T.text);
    const y = H - BOT - NAV, iw = (W - L - R) / 2;
    rect(F, 0, y, W, NAV + BOT, T.title);
    ["A", "B"].forEach((s, k) => {
        const x = L + k * iw, inner = iw - 2 * PADX;
        if (k === 1) rect(F, x + PADX, y, inner, 2, T.accent);
        glyphs(F, snap(x + PADX + (inner - tw(s)) / 2), y + PADY, s, k === 1 ? T.accent : T.textDim);
    });
    check(ok, c, F, "with side insets (a landscape cutout) the bars still span the full width; the title, the items and the content stay inside the safe area",
        [ink(L + WPAD, TOP + BAR + WPAD, "Hi")]);
    {
        const px = c.pixels();
        let marked = 0;
        for (let y = TOP; y < TOP + BAR; y++)
            for (let x = L + WPAD; x < L + WPAD + 16; x++) {
                const i = (y * W + x) * 4;
                if (((px[i] << 24) | (px[i + 1] << 16) | (px[i + 2] << 8) | px[i + 3]) >>> 0 !== T.title) marked++;
            }
        ok(marked >= 10, `the app bar title is drawn inside the safe area (${marked} pixels differ from the bar)`);
    }
    c.close();
}

{
    const c = new ui.Context({ width: 400, height: 300, scale: 2, insets: [0, TOP, 0, BOT], clock: "manual" });
    ok(c.pixelWidth === 800 && c.insets.join() === [0, TOP, 0, BOT].join(), `at scale 2 the insets stay in logical points (${c.insets}) while pixels double`);
    run(c, (u) => { u.appBar("", () => {}); u.navBar("n", ["A"], 0); });
    const px = c.pixels(), at = (x, y) => ((px[(y * 800 + x) * 4] << 24) | (px[(y * 800 + x) * 4 + 1] << 16) | (px[(y * 800 + x) * 4 + 2] << 8) | px[(y * 800 + x) * 4 + 3]) >>> 0;
    ok(at(3, 0) === T.title && at(3, 2 * TOP - 1) === T.title && at(3, 599) === T.title && at(3, 600 - 2 * BOT) === T.title,
        "at scale 2 both bars still cover their inset rows in device pixels");
    c.close();
}

// Direct pointers (touch, pen) commit on CLICK: a press may still become a scroll or a long press.
// List rows here: 17 px (line + 4), the list box starts at the screen padding with a 1 px border.
const RH = LH + 4, LIST_Y = WPAD + BW;
function rowY(k) { return LIST_Y + k * RH + RH / 2; }
{
    const c = new ui.Context({ width: W, height: H, clock: "manual" });
    const items = Array.from({ length: 30 }, (_, i) => "Item " + i);
    let sel = -1, menu = 0, t = 1;
    const scene = (u) => { sel = u.list("L", items, sel, { rows: 5 }); u.contextMenu("cm", () => { menu++; u.menuItem("Call"); }); };
    const step = (p) => { if (p) c.pointer({ kind: "touch", time: t, ...p }); c.setTime(t); run(c, scene); t += 0.016; };
    step(null);
    step({ x: 60, y: rowY(2), buttons: 1 });
    ok(sel === -1, `a touch press does not select a row yet (sel ${sel})`);
    for (let k = 1; k <= 4; k++) step({ x: 60, y: rowY(2) - 8 * k, buttons: 1 });
    step({ x: 60, y: rowY(2) - 32, buttons: 0 });
    for (let k = 0; k < 40; k++) step(null);
    ok(sel === -1, `a press that became a scroll selects nothing (sel ${sel})`);
    t += 1;
    step({ x: 60, y: rowY(1), buttons: 1 });
    ok(sel === -1, "a tap's press frame selects nothing");
    step({ x: 60, y: rowY(1), buttons: 0 });
    ok(sel >= 2, `the tap selects on release, the row under the finger after the scroll (sel ${sel})`);
    const tapped = sel;
    t += 1;
    step({ x: 60, y: rowY(3), buttons: 1 });
    for (let k = 0; k < 40; k++) step(null);
    ok(menu > 0 && sel === tapped + 2, `holding still 0.5 s opens the list's context menu and selects the row held (menu ${menu}, sel ${sel})`);
    const held = sel;
    step({ x: 60, y: rowY(3), buttons: 0 });
    step(null);
    ok(sel === held && menu > 1, `lifting after the long press neither clicks nor closes the menu (sel ${sel})`);
    c.close();
}

{
    const c = new ui.Context({ width: W, height: H, clock: "manual" });
    const items = ["Item 0", "Item 1"];
    let sel = -1;
    const scene = (u) => { sel = u.list("L", items, sel, { rows: 2 }); };
    run(c, scene);
    c.pointer({ kind: "mouse", x: 60, y: rowY(1), buttons: 1 });
    run(c, scene);
    ok(sel === 1, `a mouse press selects at once, as desktop list boxes do (sel ${sel})`);
    c.close();
}

{
    const c = new ui.Context({ width: W, height: H, clock: "manual" });
    const items = ["Red", "Green"];
    let sel = 0, popupFrames = 0;
    const scene = (u) => { sel = u.combo("C", items, sel); };
    const isOpen = () => { const px = c.pixels(); let n = 0; for (let y = WPAD + FH + 2; y < WPAD + FH + 30; y++) { const i = (y * W + 20) * 4; if (((px[i] << 24) | (px[i + 1] << 16) | (px[i + 2] << 8) | px[i + 3]) >>> 0 !== T.bg) n++; } return n > 0; };
    const tap = (x, y) => { c.pointer({ kind: "touch", x, y, buttons: 1 }); run(c, scene); const pressOpen = isOpen(); c.pointer({ kind: "touch", x, y, buttons: 0 }); run(c, scene); return [pressOpen, isOpen()]; };
    run(c, scene);
    let [p, r] = tap(20, WPAD + FH / 2);
    ok(!p && r, `a touch on a combo opens its list on release, not on press (press ${p}, release ${r})`);
    [p, r] = tap(20, WPAD + FH / 2);
    ok(!r, "a second tap on the combo closes the open list");
    c.close();
}

{
    const c = new ui.Context({ width: W, height: H, clock: "manual" });
    let s = "";
    const scene = (u) => u.column({ id: "sc", height: 60, scroll: true }, (u) => { s = u.textField("F", s); for (let k = 0; k < 10; k++) u.label("line " + k); });
    run(c, scene);
    const y = WPAD + FH / 2;
    c.pointer({ kind: "touch", x: 40, y, buttons: 1 }); run(c, scene);
    for (let k = 1; k <= 4; k++) { c.pointer({ kind: "touch", x: 40, y: y + 8 * k, buttons: 1 }); run(c, scene); }
    c.pointer({ kind: "touch", x: 40, y: y + 32, buttons: 0 }); run(c, scene);
    run(c, scene);
    ok(!c.editing && c.textInput === null, "a scroll that starts on a text field does not focus it (no keyboard)");
    c.pointer({ kind: "touch", x: 40, y: y + 1, buttons: 1 }); run(c, scene);
    c.pointer({ kind: "touch", x: 40, y: y + 1, buttons: 0 }); run(c, scene);
    run(c, scene);
    ok(c.editing && c.textInput !== null, "a tap focuses the text field and asks for the keyboard");
    c.close();
}

// Overlays are interactive surfaces: a tap inside the panel reaches its widgets and keeps it open;
// a tap on the shade outside closes it. Modal, drawer and sheet names are screen-wide.
for (const kind of ["mouse", "touch"]) {
    const c = new ui.Context({ width: W, height: H, touch: kind === "touch", clock: "manual" });
    let page = 0, shown = 0, rB = null, rP = null;
    const app = (u) => {
        u.label("page " + page);
        u.drawer("nav", () => {
            shown++;
            ["A", "B"].forEach((n, i) => { if (u.selectable(n + "##d", page === i) && page !== i) { page = i; u.closePopup(); } if (i === 1) rB = u.lastRect(); });
            u.spacer(10, 40);
            u.label("panel");
            rP = u.lastRect();
        });
    };
    const tap = (x, y) => { c.pointer({ kind, x, y, buttons: 1 }); c.frame(app); c.pointer({ kind, x, y, buttons: 0 }); c.frame(app); c.frame(app); };
    c.frame((u) => { u.openDrawer("nav"); app(u); });
    c.frame(app); c.frame(app);
    const dw = Math.min(300, Math.round(0.85 * W));
    let n = shown;
    tap(dw / 2, H - 20);
    ok(shown > n + 2 && page === 0, `${kind}: a tap on the drawer's empty area keeps it open (${shown - n} frames shown)`);
    tap(rB.x + rB.width / 2, rB.y + rB.height / 2);
    ok(page === 1, `${kind}: a tap on an item inside the drawer reaches it (page ${page})`);
    n = shown; c.frame(app);
    ok(shown === n, `${kind}: and its closePopup() closes the drawer`);
    c.frame((u) => { u.openDrawer("nav"); app(u); });
    c.frame(app);
    n = shown;
    tap(W - 5, H / 2);
    ok(shown <= n + 1, `${kind}: a tap on the shade beside the drawer closes it`);
    c.close();
}

{
    const c = new ui.Context({ width: W, height: H, touch: true, clock: "manual" });
    let sheet = 0, modal = 0, rOpen = null, rModal = null;
    const app = (u) => {
        u.card("Card", { width: 140 }, () => {
            if (u.button("Sheet##s")) u.openSheet("detail");
            rOpen = u.lastRect();
            if (u.button("Ask##m")) u.openModal("ask");
            rModal = u.lastRect();
        });
        u.sheet("detail", () => { sheet++; u.label("sheet"); });
        u.modal("ask", () => { modal++; if (u.button("OK")) u.closePopup(); });
    };
    const tap = (r) => { const x = r.x + r.width / 2, y = r.y + r.height / 2; c.pointer({ kind: "touch", x, y, buttons: 1 }); c.frame(app); c.pointer({ kind: "touch", x, y, buttons: 0 }); c.frame(app); c.frame(app); };
    c.frame(app); c.frame(app);
    tap(rOpen);
    ok(sheet > 0, `openSheet() called inside a card opens the screen's sheet of that name (${sheet} frames)`);
    c.pointer({ kind: "touch", x: 5, y: 5, buttons: 1 }); c.frame(app); c.pointer({ kind: "touch", x: 5, y: 5, buttons: 0 }); c.frame(app);
    const s = sheet; c.frame(app);
    ok(sheet === s, "a tap on the sheet's shade closes it");
    tap(rModal);
    ok(modal > 0, `openModal() called inside a card opens the screen's modal of that name (${modal} frames)`);
    c.close();
}

print(`test_ui_mobile: ${pass} pass, ${fail} fail`);
if (fail) throw new Error(`${fail} failures`);
