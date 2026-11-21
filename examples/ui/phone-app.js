// Phone app — a touch-first screen on dyna:ui at a phone's size, density and safe area, driven and checked.
// apps:needs dyna:ui
// flags: --std
//
// WHAT IT IS
//   The shape of a typical phone app: an app bar under the status bar, a bottom navigation bar above the
//   gesture area, filter chips, wrapping cards, a contacts list with fling scrolling and a long-press menu,
//   a settings page (switches, a slider, a text field), a bottom sheet, a drawer and a floating action
//   button. It runs at 412 x 915 points, 2.625 device pixels per point and 64/48-point insets — a Pixel 7.
//   With no arguments it drives itself with TOUCH input and checks what each gesture must do: a tap
//   commits on release, a fling scrolls without selecting, a long press opens the menu for the row it
//   holds, the bars cover the insets while the content stays inside the safe area.
//
// MODULES  dyna:ui (Context, touch input, insets, lastRect) · dyna:sdl (only with --window)
//
// RUN      dynajs --std examples/ui/phone-app.js             drives itself with touch input and checks it
//          dynajs --std examples/ui/phone-app.js --window    opens it in a window (needs CONFIG_SDL3);
//          tools/android/build-apk.sh run examples/ui/phone-app.js    runs it on an Android device

import * as ui from "dyna:ui";

const contacts = Array.from({ length: 200 }, (_, i) => `Contact ${i}`);
const state = {
    nav: 0,
    filters: [true, false, false, true],
    wifi: true,
    volume: 0.5,
    name: "",
    sel: -1,
    calls: [],
    opened: 0,
    toasts: 0,
    menuShown: 0,
};
const rects = {};

function app(u) {
    const rec = (name) => {
        rects[name] = u.lastRect();
    };
    u.appBar(["Home", "Contacts", "Settings"][state.nav], () => {
        if (u.button("Menu")) u.openDrawer("nav");
        rec("menu");
    });
    if (state.nav === 0) {
        u.wrap(() =>
            ["New", "Unread", "Starred", "Shared"].forEach((f, i) => {
                state.filters[i] = u.chip(f, state.filters[i]);
                rec("chip" + i);
            }),
        );
        u.wrap(() => {
            for (let i = 0; i < 4; i++)
                u.card("Card " + i, { width: 170 }, () => {
                    u.text("Body text that wraps inside the card.");
                    if (u.button("Open##" + i)) {
                        state.opened++;
                        u.openSheet("detail");
                    }
                    rec("open" + i);
                });
        });
    } else if (state.nav === 1) {
        state.sel = u.list("##contacts", contacts, state.sel, { rows: 12 });
        rec("list");
        rects.rows = new Map();
        contacts.forEach((_, k) => {
            const r = u.lastRect(k);
            if (r) rects.rows.set(k, r);
        });
        u.contextMenu("cm", () => {
            state.menuShown++;
            if (u.menuItem("Call")) state.calls.push(state.sel);
            rec("call");
        });
    } else {
        u.group("Connectivity", () => {
            state.wifi = u.switch("Wi-Fi", state.wifi);
            rec("wifi");
        });
        state.volume = u.slider("Volume", state.volume, 0, 1, { step: 0.01, decimals: 2 });
        rec("volume");
        state.name = u.textField("Name", state.name);
        rec("name");
    }
    u.sheet("detail", () => {
        u.heading("Details");
        if (u.button("Close")) u.closePopup();
        rec("close");
    });
    u.drawer("nav", () =>
        ["Home", "Contacts", "Settings"].forEach((n, i) => {
            if (u.selectable(n + "##d", state.nav === i) && state.nav !== i) {
                state.nav = i;
                u.closePopup();
            }
            rec("drawer" + i);
        }),
    );
    state.nav = u.navBar("tabs", ["Home", "Contacts", "Settings"], state.nav);
    rec("nav");
    rects.navItems = [0, 1, 2].map((k) => u.lastRect(k));
    if (u.fab("##add")) {
        state.toasts++;
        u.toast("New item");
    }
    rec("fab");
}

// ---- headless driver: touch, one frame behind as the kit is ----
const W = 412,
    H = 915,
    S = 2.625,
    INSETS = [0, 64, 0, 48];
const c = new ui.Context({ width: W, height: H, scale: S, insets: INSETS, touch: true, clock: "manual" });
let now = 1;
const frame = () => {
    now += 1 / 60;
    c.setTime(now);
    return c.frame(app);
};
const touch = (x, y, buttons) => c.pointer({ kind: "touch", x, y, buttons, time: now });
const mid = (r) => [r.x + r.width / 2, r.y + r.height / 2];
const tap = ([x, y]) => {
    touch(x, y, 1);
    frame();
    touch(x, y, 0);
    frame();
    frame();
};
const idle = (n) => {
    for (let k = 0; k < n; k++) frame();
};

const results = {};
if (scriptArgs.includes("--window")) await runWindow();
else {
    idle(3);
    const px = c.pixelsInto(new Uint8Array(c.pixelWidth * c.pixelHeight * 4));
    const at = (x, y) => {
        const i = (Math.floor(y * S) * c.pixelWidth + Math.floor(x * S)) * 4;
        return (px[i] << 16) | (px[i + 1] << 8) | px[i + 2];
    };
    const bar = c.theme().title >>> 8;
    results["the app bar's colour fills the status-bar inset and the nav bar's the gesture inset"] =
        at(5, 10) === bar && at(5, H - 10) === bar && c.insets.join() === INSETS.join();
    results["content starts below the app bar, inside the safe area; the nav bar ends at the screen's edge"] =
        rects.chip0.y >= INSETS[1] + 44 && Math.abs(rects.nav.y + rects.nav.height - c.height) < 0.01;

    tap(mid(rects.chip1));
    results["a tap toggles a chip"] = state.filters[1] === true;

    tap(mid(rects.open2));
    idle(1);
    results["a card's button opens the bottom sheet"] = state.opened === 1 && !!rects.close;
    tap(mid(rects.close));
    idle(2);

    tap(mid(rects.navItems[1]));
    idle(1);
    results["the second nav item shows the contacts"] = state.nav === 1;

    const list = rects.list,
        x = list.x + list.width / 2;
    touch(x, list.y + list.height - 20, 1);
    frame();
    for (let k = 1; k <= 8; k++) {
        now += 0.012;
        touch(x, list.y + list.height - 20 - 50 * k, 1);
        frame();
    }
    touch(x, list.y + list.height - 420, 0);
    frame();
    idle(90);
    results["a fling scrolls the list and selects nothing"] = state.sel === -1;
    const [top] = [...rects.rows].find(([, r]) => r.y >= list.y);
    tap(mid(rects.rows.get(top)));
    const tapped = state.sel;
    results["a tap after the fling selects the row under the finger, beyond the first screenful"] =
        tapped === top && tapped >= 12;

    const [hx, hy] = mid(rects.rows.get(tapped + 2));
    touch(hx, hy, 1);
    idle(40);
    touch(hx, hy, 0);
    frame();
    frame();
    results["a long press opens the row's menu and selects the row it holds"] =
        state.menuShown > 0 && state.sel === tapped + 2 && !!rects.call;
    tap(mid(rects.call));
    results["the menu's action runs once, for that row"] = state.calls.length === 1 && state.calls[0] === tapped + 2;

    tap(mid(rects.fab));
    results["the floating action button raises a toast"] = state.toasts === 1;

    tap(mid(rects.menu));
    idle(1);
    tap(mid(rects.drawer2));
    idle(1);
    results["the drawer switches to Settings and closes"] = state.nav === 2 && !c.editing;

    tap(mid(rects.wifi));
    results["a tap on the switch turns Wi-Fi off"] = state.wifi === false;
    const v = rects.volume;
    touch(v.x + 2, v.y + v.height / 2, 1);
    frame();
    touch(v.x + v.width * 0.3, v.y + v.height / 2, 1);
    frame();
    touch(v.x + v.width * 0.3, v.y + v.height / 2, 0);
    frame();
    results["dragging the slider's thumb moves the value down"] = state.volume < 0.5;
    tap(mid(rects.name));
    results["a tap on the text field focuses it and asks for the keyboard"] = c.editing && c.textInput !== null;
    c.type("Ada");
    frame();
    results["typed text lands in the field"] = state.name === "Ada";

    c.close();
    check(results);
}

async function runWindow() {
    let sdl;
    try {
        sdl = await import("dyna:sdl");
    } catch (e) {
        throw new Error("--window needs dyna:sdl (build with CONFIG_SDL3=y)");
    }
    sdl.init(["video"]);
    const win = new sdl.Window("Phone", W, H, { resizable: true, highDensity: true });
    const r = new sdl.Renderer(win);
    const w = new ui.Context({ target: r, touch: "auto" });
    for (let running = true; running;) {
        for (const e of sdl.pollEvents()) if (e.type === "quit") running = false;
        w.frame(app);
        w.idle(1);
    }
    w.close();
    r.close();
    win.close();
    sdl.quit();
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
